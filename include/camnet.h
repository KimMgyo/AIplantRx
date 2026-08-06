// Network camera: pulls the ESP32-CAM's HTTP MJPEG stream and decodes it into a
// shared frame sink. This is the only camera transport — the CAM reaches the
// display over WiFi, so the video path lives and dies with the radio.
//
// Note: the CAM also serves true RTSP (:8554), but a full RTSP/RTP client is
// heavy and fragile on-device; the HTTP MJPEG endpoint carries identical JPEG
// frames over a trivial protocol, so that is what this consumes. (RTSP is for
// external viewers — phones / VLC.)
#pragma once
#include <stddef.h>
#include <stdint.h>

// QVGA, the size the CAM sends; the UI builds its scale maps from these.
#define CAMNET_W 320
#define CAMNET_H 240

void camnet_init(void);   // starts the puller task; call after camprov_init()

// RGB stream (/rgb/stream).
bool camnet_live(void);   // an RGB network frame decoded within the last ~3 s
bool camnet_take_scaled(uint16_t *dst, int dst_w, int dst_h,
                        const int16_t *sx_map, const int16_t *sy_map);

// Non-consuming variant: same scale, but it leaves the frame available to the
// monitor page's live canvas instead of taking it away. See camframe_peek_scaled.
bool camnet_peek_scaled(uint16_t *dst, int dst_w, int dst_h,
                        const int16_t *sx_map, const int16_t *sy_map);

// On-screen consumption switch, monitor page -> puller. The decoded RGB frame is
// only drawn by the monitor canvas; aijudge and the uplink do not need it live.
// While viewing is false the puller keeps draining the socket and keeps the raw
// JPEG (camnet_jpeg_*) fresh, but drops the ~31ms RGB decode to a low keep-alive
// rate so it stops burning core 0 beside the WiFi stack. Defaults true: until the
// monitor page says otherwise, assume someone is watching. See camnet.cpp.
void camnet_set_viewing(bool viewing);

// ---- the compressed frame ---------------------------------------------------
//
// A copy of one complete JPEG, for code that has to *send* the frame somewhere
// rather than draw it. The uplink cannot forward what the decoder produced: the
// server wants an image, and re-encoding RGB565 on this SoC would cost more than
// the pull itself when the puller already holds the original bytes.
//
// Request/publish rather than a lock. The puller runs in its own task and only
// touches the keep buffer while a request is outstanding; the consumer only
// reads it while `ready` holds. So a request is one frame's worth of work in the
// puller, and between a ready and the next request nothing writes the buffer -
// no mutex, and the live view never stalls waiting for the uplink.
//
// Only requested frames are copied. Keeping every frame would put a 60KB PSRAM
// memcpy in a 15fps loop for the ~1-in-3000 frames that are actually uploaded.
void camnet_jpeg_request(void);          // keep the next complete frame
bool camnet_jpeg_ready(void);            // a kept frame is waiting
const uint8_t *camnet_jpeg(size_t *len); // NULL unless ready; valid until release
void camnet_jpeg_release(void);          // done with it; clears ready

// Prints pull rate, byte rate and where the puller's per-frame time goes (JPEG
// scan vs decode) every ~4s. Call freely from loop(); it self-throttles.
void camnet_debug_tick(void);
