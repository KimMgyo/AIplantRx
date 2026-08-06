#include <Arduino.h>
#include <JPEGDEC.h>
#include <new>
#include <esp_heap_caps.h>
#include "camframe.h"

struct CamFrame {
    uint16_t *decode_buf;   // PSRAM: decoder target (back buffer)
    uint16_t *frame_buf;    // PSRAM: latest complete frame (front buffer)
    SemaphoreHandle_t mutex;
    volatile bool fresh;
    volatile uint32_t last_ms;
};

// One decoder shared by every CamFrame: transports are gated to a single active
// source, so they never decode concurrently. s_jpeg_mutex guards the rare
// overlap; the frame being filled rides on the decoder's user pointer (for the
// MCU callback), avoiding a JPEGDEC-per-source (~18KB each).
//
// JPEGDEC, not esp32-camera's jpg2rgb565: both were run on the same frames on
// this board and tjpgd took 60.9ms against JPEGDEC's 48.5ms, 25% slower. Decode
// is the largest per-frame cost in the camera path, so the faster one stays.
//
// IN PSRAM, NOT .bss. sizeof(JPEGDEC) is 17,884 bytes, and as a static it sat in
// internal DRAM - the one memory this board runs short of. Measured on this
// board: 12.5KB internal free at steady state with a largest free block of
// 7.1KB, which means an 8KB allocation by the WiFi driver already fails. Nothing
// in the decoder needs internal RAM: it is touched only by the camera task with
// the cache on, never from an ISR and never while flash is being written, so it
// moves out and the internal heap keeps the 17.9KB. Placement new because
// JPEGDEC has a constructor and no allocator hook.
static JPEGDEC *s_jpeg = NULL;
static SemaphoreHandle_t s_jpeg_mutex = NULL;

// Draw callback: copy each decoded MCU block into the CamFrame's back buffer.
//
// This writes MCU rows straight into PSRAM - roughly 4800 scattered 32-byte
// stores per 320x240 frame. Staging a whole MCU-high band in internal RAM and
// flushing it with one bulk copy per band was tried and measured no faster
// (27-34ms per decode either way): JPEGDEC's Huffman and IDCT work dominates,
// not the stores. Not worth the shared mutable band state.
static int camframe_on_mcu(JPEGDRAW *d) {
    CamFrame *cf = (CamFrame *)d->pUser;
    for (int row = 0; row < d->iHeight; row++) {
        int y = d->y + row;
        if (y >= CAMFRAME_H) break;
        int w = d->iWidth;
        if (d->x + w > CAMFRAME_W) w = CAMFRAME_W - d->x;
        if (w <= 0) continue;
        memcpy(&cf->decode_buf[y * CAMFRAME_W + d->x], &d->pPixels[row * d->iWidth], (size_t)w * 2);
    }
    return 1;
}

CamFrame *camframe_create(void) {
    if (s_jpeg_mutex == NULL) {
        s_jpeg_mutex = xSemaphoreCreateMutex();  // idempotent: creates run at init
    }
    if (s_jpeg == NULL) {
        void *mem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM);
        if (mem != NULL) s_jpeg = new (mem) JPEGDEC();
    }
    CamFrame *cf = new CamFrame();
    cf->decode_buf = (uint16_t *)heap_caps_malloc(CAMFRAME_W * CAMFRAME_H * 2, MALLOC_CAP_SPIRAM);
    cf->frame_buf = (uint16_t *)heap_caps_malloc(CAMFRAME_W * CAMFRAME_H * 2, MALLOC_CAP_SPIRAM);
    cf->mutex = xSemaphoreCreateMutex();
    cf->fresh = false;
    cf->last_ms = 0;
    if (cf->decode_buf == NULL || cf->frame_buf == NULL || cf->mutex == NULL ||
            s_jpeg_mutex == NULL || s_jpeg == NULL) {
        // Partial allocation failed (PSRAM exhausted): free what we got.
        heap_caps_free(cf->decode_buf);
        heap_caps_free(cf->frame_buf);
        if (cf->mutex) vSemaphoreDelete(cf->mutex);
        delete cf;
        return NULL;
    }
    return cf;
}

void camframe_submit(CamFrame *cf, uint8_t *jpeg, size_t len) {
    if (cf == NULL) return;

    xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY);
    bool ok = false;
    if (s_jpeg->openRAM(jpeg, (int)len, camframe_on_mcu)) {
        s_jpeg->setUserPointer(cf);
        s_jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
        ok = s_jpeg->decode(0, 0, 0);
        s_jpeg->close();
    }
    xSemaphoreGive(s_jpeg_mutex);
    if (!ok) return;

    xSemaphoreTake(cf->mutex, portMAX_DELAY);
    uint16_t *tmp = cf->frame_buf;  // swap front/back: no large copy here
    cf->frame_buf = cf->decode_buf;
    cf->decode_buf = tmp;
    cf->fresh = true;
    cf->last_ms = millis();
    xSemaphoreGive(cf->mutex);
}

bool camframe_live(CamFrame *cf, uint32_t timeout_ms) {
    return cf != NULL && cf->last_ms != 0 && (millis() - cf->last_ms) < timeout_ms;
}

bool camframe_take(CamFrame *cf, uint16_t *dst, int dst_stride) {
    if (cf == NULL || !cf->fresh) {
        return false;
    }
    xSemaphoreTake(cf->mutex, portMAX_DELAY);
    for (int y = 0; y < CAMFRAME_H; y++) {
        memcpy(&dst[y * dst_stride], &cf->frame_buf[y * CAMFRAME_W], CAMFRAME_W * 2);
    }
    cf->fresh = false;
    xSemaphoreGive(cf->mutex);
    return true;
}

// Nearest-neighbour scale into an RGB565 target, staged through SRAM.
//
// The destination is a PSRAM canvas, so writing it pixel by pixel means dst_w
// scattered 2-byte stores per row. Building the row in SRAM and handing it over
// as one memcpy is materially cheaper, and it makes repeated source rows free:
// scaling 240 rows up to 280 repeats about one row in seven, and a repeat is
// then just a second memcpy of the row still sitting in the staging buffer.
#define CAMFRAME_MAX_DST_W 512

// Both entry points below funnel through this. The caller MUST already hold
// cf->mutex: it reads frame_buf, which camframe_submit swaps out from the puller
// task, and the staging row is a single shared buffer.
static void camframe_scale_locked(CamFrame *cf, uint16_t *dst, int dst_w, int dst_h,
                                  const int16_t *sx_map, const int16_t *sy_map) {
    static uint16_t orow[CAMFRAME_MAX_DST_W];
    if (dst_w <= CAMFRAME_MAX_DST_W) {
        int prev_sy = -1;
        for (int dy = 0; dy < dst_h; dy++) {
            int sy = sy_map[dy];
            if (sy != prev_sy) {
                const uint16_t *srow = &cf->frame_buf[sy * CAMFRAME_W];
                for (int dx = 0; dx < dst_w; dx++) {
                    orow[dx] = srow[sx_map[dx]];
                }
                prev_sy = sy;
            }
            memcpy(&dst[dy * dst_w], orow, (size_t)dst_w * 2);
        }
    } else {
        // Wider than the staging row: no SRAM to stage through, so eat the
        // scattered PSRAM stores. Nothing in this firmware hits this path.
        for (int dy = 0; dy < dst_h; dy++) {
            const uint16_t *srow = &cf->frame_buf[sy_map[dy] * CAMFRAME_W];
            uint16_t *drow = &dst[dy * dst_w];
            for (int dx = 0; dx < dst_w; dx++) {
                drow[dx] = srow[sx_map[dx]];
            }
        }
    }
}

bool camframe_take_scaled(CamFrame *cf, uint16_t *dst, int dst_w, int dst_h,
                          const int16_t *sx_map, const int16_t *sy_map) {
    if (cf == NULL || !cf->fresh) {
        return false;
    }
    xSemaphoreTake(cf->mutex, portMAX_DELAY);
    camframe_scale_locked(cf, dst, dst_w, dst_h, sx_map, sy_map);
    cf->fresh = false;
    xSemaphoreGive(cf->mutex);
    return true;
}

bool camframe_peek_scaled(CamFrame *cf, uint16_t *dst, int dst_w, int dst_h,
                          const int16_t *sx_map, const int16_t *sy_map) {
    // last_ms, not fresh: the live canvas is the fresh flag's owner. Gating here
    // would both consume its frames and make a snapshot succeed only in the
    // window between its ticks, which is not a window the caller controls.
    // Whatever frame is currently published is exactly what the caller wants.
    if (cf == NULL || cf->last_ms == 0) {
        return false;
    }
    xSemaphoreTake(cf->mutex, portMAX_DELAY);
    camframe_scale_locked(cf, dst, dst_w, dst_h, sx_map, sy_map);
    xSemaphoreGive(cf->mutex);
    return true;
}
