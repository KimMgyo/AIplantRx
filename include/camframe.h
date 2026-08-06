// Shared JPEG -> RGB565 double-buffered frame sink.
//
// camnet's MJPEG puller — the only camera transport — hands complete JPEG frames
// to a CamFrame; it decodes into a back buffer, swaps it to the front under a
// mutex, and lets the UI copy the latest frame out. The JPEGDEC instance is
// file-scope rather than per-CamFrame, keeping ~18KB of internal RAM off the
// heap.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define CAMFRAME_W 320
#define CAMFRAME_H 240

struct CamFrame;  // opaque

CamFrame *camframe_create(void);  // allocates PSRAM buffers + mutex; NULL on OOM

// Decode a complete JPEG and publish it as the newest frame. Silently drops a
// frame that fails to decode. Safe to call from a producer task.
void camframe_submit(CamFrame *cf, uint8_t *jpeg, size_t len);

// True if a frame was published within the last `timeout_ms`.
bool camframe_live(CamFrame *cf, uint32_t timeout_ms);

// Copy the latest frame (CAMFRAME_W x CAMFRAME_H, RGB565) into dst, advancing
// dst_stride pixels per row, and clear the fresh flag. False if none is new.
bool camframe_take(CamFrame *cf, uint16_t *dst, int dst_stride);

// Like camframe_take but nearest-neighbour scales straight from the front
// buffer into a dst_w x dst_h target in ONE pass (no intermediate copy). The
// maps hold source x/y indices per destination x/y. False if none is new.
bool camframe_take_scaled(CamFrame *cf, uint16_t *dst, int dst_w, int dst_h,
                          const int16_t *sx_map, const int16_t *sy_map);

// Same scaling as camframe_take_scaled, but it neither requires nor clears the
// fresh flag. The monitor page's live canvas already consumes fresh frames; a
// second consumer using the take API would take every other frame from it and
// halve the canvas's effective rate. False only if no frame has ever arrived.
bool camframe_peek_scaled(CamFrame *cf, uint16_t *dst, int dst_w, int dst_h,
                          const int16_t *sx_map, const int16_t *sy_map);
