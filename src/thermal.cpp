// MLX90640 thermal receive sink: reassembles the sensor node's ESP-NOW frame
// fragments into a palette-ready RGB565 double buffer (no decode — the node
// already computed the palette).
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>
#include "thermal.h"
#include "net.h"
#include "hlog.h"

// The node emits one frame per ~2s (MLX at 2Hz, one subpage per node loop), and
// a single dropped fragment discards the whole frame, so a few seconds would
// flap on normal jitter. 10s is ~5 missed frames — long enough to ride through
// the node's hot-plug re-acquire stall, short enough to still be the "no sensor
// / node gone" indicator the UI needs.
static const uint32_t LIVE_TIMEOUT_MS = 10000;

static uint16_t s_therm_a[THERMAL_W * THERMAL_H];
static uint16_t s_therm_b[THERMAL_W * THERMAL_H];
static uint16_t *s_therm_front = s_therm_a;
static uint16_t *s_therm_back = s_therm_b;
static SemaphoreHandle_t s_therm_mutex;
static volatile bool s_therm_fresh = false;
static volatile uint32_t s_therm_last_ms = 0;
// The scene peak the last accepted frame carried, in °C. Written on every frame
// and cleared by nothing, so it outlives the stream by hours - which is why this
// static is private and thermal_max() gates it on thermal_live().
static const float THERM_NONE = -1000.0f;  // no current reading; the < -999 sentinel used across this codebase
static volatile float s_therm_max = THERM_NONE;
// Where that peak was, as a row-major index into the frame, or THERMAL_PEAK_NONE.
// Separate from s_therm_max because the two can disagree: a node that predates the
// field sends a temperature and no position, and a screen that guessed a position
// for it would point at a pixel for no reason.
static volatile int32_t s_therm_peak = THERMAL_PEAK_NONE;

// ---------------------------------------------------------------------------
// Fragment reassembly
// ---------------------------------------------------------------------------

// Exactly one frame is assembled at a time: the node emits a frame's fragments
// back-to-back, so a fragment carrying a new frame_id means the previous frame
// will never complete. Abandoning it is the right behaviour here — a dropped
// thermal frame just leaves the last complete one on screen for another tick,
// which is far better than blending two frames together.
static uint8_t s_asm[THERMAL_PAYLOAD_BYTES];
static uint8_t s_asm_frame_id = 0;
static bool s_asm_active = false;
static uint32_t s_asm_mask = 0;  // bit per received frag_index
// Bytes actually written into s_asm for the frame being assembled - not the size of
// s_asm, which is a compile-time constant that says nothing about the sender.
static size_t s_asm_bytes = 0;
// 2.4GHz channels overlap by roughly two either side, so when the node has to
// sweep we hear its neighbouring-channel copies of a frame we already published.
// Without this, each late duplicate looks like the start of a new frame, leaves a
// one-fragment partial behind, and the next real frame counts it as a drop —
// which is how a healthy link came to report 97% frame loss.
static uint8_t s_asm_done_id = 0;
static bool s_asm_have_done = false;
static const uint32_t ASM_MASK_FULL = (1u << THERMAL_FRAG_COUNT) - 1u;
static_assert(THERMAL_FRAG_COUNT <= 32, "fragment bitmask is a uint32_t");

// Link health, printed from the UI thread. A fragmented link over a shared
// radio can silently lose whole frames, so the drop count is the number that
// matters: it distinguishes "the node stopped sending" from "we hear it but
// can never assemble it" — the latter would mean the node is on the wrong
// channel or the fragments are being spaced too tightly.
static volatile uint32_t s_dbg_frags = 0;
static volatile uint32_t s_dbg_frames = 0;
static volatile uint32_t s_dbg_dropped = 0;
static volatile uint32_t s_dbg_rejected = 0;

// Rolling frame rate for the settings page, updated on a window of its own so
// it does not fight the serial debug tick's counter resets.
static volatile uint32_t s_fps_frames = 0;
static uint32_t s_fps_ms = 0;
static float s_fps = 0.0f;

void thermal_init(void) {
    s_therm_mutex = xSemaphoreCreateMutex();
}

// Runs inside camprov's ESP-NOW receive callback, so it stays allocation-free
// and short: validate, memcpy one fragment, and on the last fragment swap the
// buffers. No logging, no heap, no long waits.
void thermal_on_frag(const ThermalFragMsg &m) {
    // Init failed or has not run yet: without the mutex there is nowhere safe to
    // publish, so drop rather than take a NULL semaphore.
    if (s_therm_mutex == NULL) return;

    // This is raw radio input — every bound below is load-bearing, not
    // decorative. A bad header must never write past the staging buffer.
    if (m.magic != THERMAL_MAGIC || m.type != THERMAL_FRAG) { s_dbg_rejected++; return; }
    if (m.frag_count != THERMAL_FRAG_COUNT || m.frag_index >= m.frag_count) { s_dbg_rejected++; return; }
    size_t off = (size_t)m.frag_index * THERMAL_FRAG_BYTES;
    if (off + m.frag_len > THERMAL_PAYLOAD_BYTES) { s_dbg_rejected++; return; }
    s_dbg_frags++;

    // Already published this frame: the rest of its copies carry no new
    // information and must not be mistaken for the start of the next one.
    if (s_asm_have_done && m.frame_id == s_asm_done_id) return;

    if (!s_asm_active || m.frame_id != s_asm_frame_id) {
        // A new frame_id while the previous one was still incomplete means that
        // frame lost at least one fragment and will never assemble.
        if (s_asm_active) s_dbg_dropped++;
        s_asm_frame_id = m.frame_id;
        s_asm_active = true;
        s_asm_mask = 0;
        s_asm_bytes = 0;
    }
    memcpy(&s_asm[off], m.data, m.frag_len);
    s_asm_mask |= 1u << m.frag_index;
    // The staging buffer is sized for the CURRENT payload, so its size says nothing
    // about what arrived. A node that predates peak_idx sends 1540 bytes in the same
    // seven fragments and its last two bytes here are whatever the previous frame
    // left; counting what was actually written is the only way to tell the two apart.
    // See the backward-compatibility note in include/camprov.h.
    if (off + m.frag_len > s_asm_bytes) s_asm_bytes = off + m.frag_len;
    if (s_asm_mask != ASM_MASK_FULL) return;
    s_asm_active = false;
    s_asm_done_id = m.frame_id;
    s_asm_have_done = true;
    s_dbg_frames++;
    s_fps_frames++;

    const size_t img_bytes = THERMAL_W * THERMAL_H * sizeof(uint16_t);
    float mx;
    memcpy(&mx, s_asm + img_bytes, sizeof(float));  // peak temp trails the image
    int32_t peak = THERMAL_PEAK_NONE;
    if (s_asm_bytes >= THERMAL_PAYLOAD_BYTES) {
        uint16_t idx;
        memcpy(&idx, s_asm + img_bytes + sizeof(float), sizeof(idx));
        // Bounds-checked because this is still raw radio input: an index past the
        // image would be read as a pixel that does not exist.
        if (idx < THERMAL_W * THERMAL_H) peak = (int32_t)idx;
    }
    xSemaphoreTake(s_therm_mutex, portMAX_DELAY);
    memcpy(s_therm_back, s_asm, img_bytes);
    uint16_t *tmp = s_therm_front;
    s_therm_front = s_therm_back;
    s_therm_back = tmp;
    s_therm_fresh = true;
    s_therm_last_ms = millis();
    s_therm_max = mx;
    s_therm_peak = peak;
    xSemaphoreGive(s_therm_mutex);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// The s_therm_last_ms != 0 guard matters at boot: without it `millis() - 0` is
// small enough to claim LIVE for the first seconds, and the UI would blit the
// still-blank front buffer.
bool thermal_live(void) {
    return s_therm_fresh || (s_therm_last_ms != 0 && millis() - s_therm_last_ms < LIVE_TIMEOUT_MS);
}

// Gated on thermal_live() rather than returned raw, and that gate is the whole
// point of the accessor. s_therm_max is written per frame and reset by nothing,
// so an MLX90640 that dies at noon still reads back its last peak at midnight.
// Every caller was gating instead on "has a frame ever arrived", which is only
// false in the first seconds after boot - so the monitor page drew the thermal
// panel OFFLINE and a live-looking 표면온도 underneath it at the same time, and
// the 잎-공기 tile computed a delta between two different moments whenever the
// thermal channel died while the node's SCD41 kept broadcasting. One gate here
// answers for the tiles, the settings row and the value that goes on the wire.
//
// There is no accessor for the stale peak: no caller wanted a number it knows is
// not current. thermal_debug_tick() reads the static directly, which is honest
// there only because it prints LIVE/offline on the same line.
float thermal_max(void) { return thermal_live() ? s_therm_max : THERM_NONE; }

// Where the peak thermal_max() reports actually is: a row-major index into the
// THERMAL_W x THERMAL_H frame, or THERMAL_PEAK_NONE.
//
// Gated on thermal_live() for the same reason thermal_max() is - a position from a
// frame that stopped arriving marks a spot in a picture nobody is updating - and
// separately absent when the node did not send one, which is not the same thing and
// must not be collapsed into it. A caller that cannot tell them apart would draw a
// marker at index 0, the top-left corner, on every frame from an older node.
int32_t thermal_peak_index(void) {
    return thermal_live() ? s_therm_peak : THERMAL_PEAK_NONE;
}

// Frames per second over a rolling ~2s window. Call from the UI as often as it
// likes; the window only closes when enough time has passed.
float thermal_fps(void) {
    uint32_t now = millis();
    uint32_t span = now - s_fps_ms;
    if (span >= 2000) {
        s_fps = s_fps_frames * 1000.0f / span;
        s_fps_frames = 0;
        s_fps_ms = now;
    }
    return s_fps;
}

// Bilinear upscale of the 32x24 palette frame into an RGB565 target.
//
// Two things dominated this and both are structural, not micro-optimisation:
//
// 1. It used to run on every UI tick regardless of whether a new frame had
//    arrived. `s_therm_fresh` was set on arrival and cleared here but never
//    tested, so a source that produces a couple of frames per second was
//    re-upscaled ~15 times a second. Measured at 374x280 that was 0.71s of
//    every second spent on the LVGL thread redrawing an identical image, which
//    is what held the whole display at 4Hz. It now returns false when the frame
//    has not changed, and the caller skips the invalidate.
//
// 2. The blend was 2D: every destination row redid the horizontal
//    interpolation, 4 taps x 3 channels per pixel. There are only 24 source
//    rows, so each is interpolated horizontally once into an SRAM cache and a
//    destination row is then a 2-tap blend of two cached rows. Rows are staged
//    in SRAM and handed to the PSRAM canvas as one memcpy each, instead of
//    dst_w scattered 2-byte stores.
//
// For anyone measuring this again: 374x280, on the LVGL thread, per frame -
// float 2D 87ms, integer 2D 48ms, integer separable (this) ~24ms. And it only
// runs on a genuinely new frame now, not once per UI tick. Note invalidate
// counts overstate what reaches the panel; only completed refreshes count.
#define THERMAL_MAX_DST_W 512
static int16_t s_wx0[THERMAL_MAX_DST_W];   // left source column
static int16_t s_wx1[THERMAL_MAX_DST_W];   // right source column
static uint8_t s_wxf[THERMAL_MAX_DST_W];   // right-column weight, 0..32
static int s_wx_for_w = -1;                // dst_w the cache was built for

// RGB565 lerp with a 5-bit weight. r and b blend in one multiply pair: masked
// together, b's field tops out at 31*32 = 992 and so cannot carry into r's bits
// (which start at 11), leaving only g to need its own pair. Endpoints are exact
// (f=0 yields a, f=32 yields b), so source pixels survive the upscale unchanged.
// A 5-bit weight is 33 blend levels across a source interval that spans ~12
// destination pixels here - finer than the geometry can use.
#define RB565_MASK 0xF81Fu
#define G565_MASK  0x07E0u
static inline uint16_t lerp565(uint16_t a, uint16_t b, uint32_t f)
{
    uint32_t nf = 32u - f;
    uint32_t rb = ((((a & RB565_MASK) * nf) + ((b & RB565_MASK) * f)) >> 5) & RB565_MASK;
    uint32_t g  = ((((a & G565_MASK)  * nf) + ((b & G565_MASK)  * f)) >> 5) & G565_MASK;
    return (uint16_t)(rb | g);
}

static void build_x_weights(int dst_w) {
    for (int dx = 0; dx < dst_w; dx++) {
        // 27.5 position in source columns, then split into index + fraction.
        int32_t pos = (dst_w > 1) ? (int32_t)dx * ((THERMAL_W - 1) << 5) / (dst_w - 1) : 0;
        int x0 = pos >> 5;
        s_wx0[dx] = (int16_t)x0;
        s_wx1[dx] = (int16_t)((x0 < THERMAL_W - 1) ? x0 + 1 : x0);
        s_wxf[dx] = (uint8_t)(pos & 31);
    }
    s_wx_for_w = dst_w;
}

// One source row, interpolated horizontally to dst_w, still packed RGB565.
static void therm_row_h(uint16_t *out, const uint16_t *src, int dst_w) {
    for (int dx = 0; dx < dst_w; dx++) {
        out[dx] = lerp565(src[s_wx0[dx]], src[s_wx1[dx]], s_wxf[dx]);
    }
}

// The bilinear body, shared by take and peek. The caller MUST already hold
// s_therm_mutex: it reads s_therm_front, which thermal_on_frag swaps out from
// inside the ESP-NOW receive callback.
//
// hbuf/orow stay static rather than becoming locals: at THERMAL_MAX_DST_W that is
// 3KB, which is more stack than either caller's task has to spare. The two
// callers are the LVGL thread drawing the live panel and the judge tick from
// loop() freezing a thumbnail, and both hold the mutex across the whole scale, so
// the shared staging is serialised and static is safe.
//
// The x-weight cache is rebuilt in here, under the lock, and not in the callers.
// It used to sit outside because there was only ever one caller and one dst_w;
// with two callers at different sizes (374x280 panel, 64x48 thumbnail) an
// unlocked rebuild could re-index a scale already in progress on the other
// thread. The rebuild is dst_w iterations of shifts and one divide - noise next
// to the scale itself, even when the two sizes alternate and it runs every call.
static void thermal_scale_locked(uint16_t *dst, int dst_w, int dst_h) {
    static uint16_t hbuf[2][THERMAL_MAX_DST_W];   // two interpolated source rows
    static uint16_t orow[THERMAL_MAX_DST_W];      // staged destination row

    if (s_wx_for_w != dst_w) build_x_weights(dst_w);

    uint16_t *h0 = hbuf[0], *h1 = hbuf[1];
    int c0 = -1, c1 = -1;                            // source rows the cache holds
    for (int dy = 0; dy < dst_h; dy++) {
        int32_t ypos = (dst_h > 1) ? (int32_t)dy * ((THERMAL_H - 1) << 5) / (dst_h - 1) : 0;
        int y0 = ypos >> 5;
        int y1 = (y0 < THERMAL_H - 1) ? y0 + 1 : y0;
        uint32_t fy = (uint32_t)(ypos & 31);         // fy weighs the lower row

        // y0 advances monotonically, so the row below usually becomes the row
        // above on the next destination row: rotate rather than recompute.
        if (c1 == y0) { uint16_t *t = h0; h0 = h1; h1 = t; c0 = c1; c1 = -1; }
        if (c0 != y0) { therm_row_h(h0, &s_therm_front[y0 * THERMAL_W], dst_w); c0 = y0; }

        if (fy == 0 || y1 == y0) {
            // Lands exactly on a source row: the horizontal pass already made it,
            // so this row costs one memcpy and no blending at all.
            memcpy(&dst[dy * dst_w], h0, (size_t)dst_w * 2);
            continue;
        }
        if (c1 != y1) { therm_row_h(h1, &s_therm_front[y1 * THERMAL_W], dst_w); c1 = y1; }

        for (int dx = 0; dx < dst_w; dx++) {
            orow[dx] = lerp565(h0[dx], h1[dx], fy);
        }
        memcpy(&dst[dy * dst_w], orow, (size_t)dst_w * 2);
    }
}

bool thermal_take_scaled(uint16_t *dst, int dst_w, int dst_h) {
    if (!thermal_live()) return false;
    if (dst_w <= 0 || dst_w > THERMAL_MAX_DST_W || dst_h <= 0) return false;

    xSemaphoreTake(s_therm_mutex, portMAX_DELAY);
    if (!s_therm_fresh) {                            // nothing new to draw
        xSemaphoreGive(s_therm_mutex);
        return false;
    }
    thermal_scale_locked(dst, dst_w, dst_h);
    s_therm_fresh = false;
    xSemaphoreGive(s_therm_mutex);
    return true;
}

bool thermal_peek_scaled(uint16_t *dst, int dst_w, int dst_h) {
    if (dst_w <= 0 || dst_w > THERMAL_MAX_DST_W || dst_h <= 0) return false;
    // Gated on "a frame was ever published", not on thermal_live() or freshness.
    // The live panel owns s_therm_fresh, and clearing it here would cost that
    // panel a redraw of a frame it never got to show. Staleness is also fine and
    // wanted: a frozen thumbnail should be whatever the verdict was made from,
    // even if the node has since gone quiet. s_therm_last_ms != 0 doubles as the
    // mutex-exists check, since nothing can publish before thermal_init().
    if (s_therm_last_ms == 0) return false;

    xSemaphoreTake(s_therm_mutex, portMAX_DELAY);
    thermal_scale_locked(dst, dst_w, dst_h);
    xSemaphoreGive(s_therm_mutex);
    return true;
}

void thermal_debug_tick(void) {
    static uint32_t s_print_ms = 0;
    if (millis() - s_print_ms < 4000) return;
    s_print_ms = millis();

    // WiFi state is printed alongside deliberately: association changes our
    // channel and reinstates modem sleep, and both silently cost whole frames.
    uint8_t ch = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&ch, &second);
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    char ip[20];
    net_ip(ip, sizeof(ip));

    // The peak's position as source coordinates, because "38.4C somewhere" and
    // "38.4C at (17,9)" are different facts and only the second one can be checked
    // against a hand held in front of the sensor. Prints "-" when the node sent no
    // position, which is also how an older node is identified from here without
    // reading its serial.
    int32_t pk = s_therm_peak;
    char peak_at[16];
    if (pk < 0) {
        snprintf(peak_at, sizeof(peak_at), "-");
    } else {
        snprintf(peak_at, sizeof(peak_at), "%ld,%ld",
                 (long)(pk % THERMAL_W), (long)(pk / THERMAL_W));
    }

    hlogf("[thermal] frags=%lu frames=%lu dropped=%lu rejected=%lu | %s peak=%.1fC "
                  "at=%s | wifi ch%u ps=%s ip=%s\n",
                  (unsigned long)s_dbg_frags, (unsigned long)s_dbg_frames,
                  (unsigned long)s_dbg_dropped, (unsigned long)s_dbg_rejected,
                  thermal_live() ? "LIVE" : "offline", s_therm_max, peak_at,
                  ch, ps == WIFI_PS_NONE ? "off" : "ON(bad)", ip);
}
