#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "camnet.h"
#include "camframe.h"
#include "camprov.h"
#include "updatemode.h"
#include "net.h"
#include "hlog.h"

static const size_t MAX_JPEG = 60 * 1024;
// Longer than a full WiFi scan. The settings page sweeps all 13 channels, which
// parks the STA off-channel for close to four seconds, and at the old 3s window
// that alone flipped the UI to its "disconnected" placeholder - so returning to
// the monitor page mid-scan showed the placeholder instead of the last frame.
// The cost is that a camera which really has died takes this long to be called
// offline, which for a live view is a fine trade.
static const uint32_t LIVE_TIMEOUT_MS = 6000;
static const uint32_t CONNECT_TIMEOUT_MS = 4000;
// Also longer than a full scan, and for a sharper reason: at the old 4s this sat
// right on the boundary of a ~3.9s channel sweep, so a scan could tear the HTTP
// session down and force a reconnect - a far bigger hitch than the stall itself.
static const uint32_t READ_STALL_MS = 8000;

// One puller = one HTTP MJPEG endpoint on the CAM, decoded into its own sink.
// Only RGB is pulled: thermal reaches the display from the sensor node over
// ESP-NOW now, not from the CAM.
struct NetPuller {
    const char *path;             // "/rgb/stream"
    uint8_t *jpeg_buf;            // PSRAM: one JPEG assembled from the MJPEG stream
    CamFrame *cf;                 // shared decode/double-buffer sink
};

static NetPuller s_rgb;

// Link health, printed from the UI thread. Splitting scan from decode is what
// showed that the byte-at-a-time assembly loop, not the JPEG decoder, was the
// original bottleneck.
static volatile uint32_t s_dbg_bytes = 0, s_dbg_frames = 0;
static volatile uint32_t s_us_scan = 0, s_us_decode = 0, s_us_idle = 0;

// ---- the compressed frame tap ----------------------------------------------
// A slot of its own, never np->jpeg_buf: the puller keeps assembling the next
// frame into that buffer the moment this one is submitted, so handing its
// pointer out would let the following frame overwrite the bytes mid-upload.
// NULL when PSRAM was exhausted at init, which is a working state - the puller
// skips the copy and ready never turns true, so the live view is untouched.
static uint8_t *s_keep = NULL;
static volatile size_t s_keep_len = 0;
static volatile bool s_keep_ready = false;  // puller -> consumer: bytes are valid
static volatile bool s_want = false;        // consumer -> puller: keep the next one
static volatile uint32_t s_dbg_kept = 0;    // tapped bytes this debug window

// ---- decode throttle / core-0 fairness -------------------------------------
// The decode is the puller's one expensive act: ~31ms of JPEGDEC Huffman/IDCT
// plus ~4800 scattered PSRAM stores, all on core 0 beside the WiFi/lwIP driver.
// Left ungated on a sender that keeps its window full, the pull loop never hits
// its one vTaskDelay (only the no-data path at :126 sleeps), so core 0's idle
// task - which the lwIP RX path and the task watchdog run behind - is starved.
// Two gates fix that: a floor between decodes so a fast sender can't drive more
// than ~25fps of them, and dropping the decode to a keep-alive rate whenever the
// monitor canvas is off screen and nothing draws the RGB frame.
//
// s_viewing defaults true: until the monitor page reports otherwise the safe
// assumption is that someone is watching. page_monitor.cpp drives it off the same
// canvas-visibility test that gates its scaling; see camnet_set_viewing.
static volatile bool s_viewing = true;
// ~25fps ceiling on decodes while viewing. The native inline rate is ~16-20fps
// (see the decode-cost note below), so this only caps a pathologically fast
// sender and never touches the normal feed.
static const uint32_t DECODE_MIN_INTERVAL_MS = 40;
// While the canvas is hidden, decode this rarely - just often enough to keep
// camframe's last_ms inside LIVE_TIMEOUT_MS so camnet_live() stays true, and to
// hand aijudge's 3-minute thumbnail peek a recent frame. ~2fps drops decode load
// ~90% vs the viewing path. Deliberately NOT zero: camnet_live() gates plantrx's
// uplink request (plantrx.cpp:753) and aijudge's evidence peek (aijudge.cpp:294),
// both of which would stop the moment last_ms went stale.
static const uint32_t DECODE_HIDDEN_INTERVAL_MS = 500;

// Puller task only. Kept out of the scan loop so the common path is one test.
//
// The consumer's frame is inviolable: while it holds one (ready set, not yet
// released) a new frame is simply not tapped and the request stays pending, so
// the next release is followed by a later frame. Dropping a frame costs the
// uplink a poll interval; tearing one would cost the server a decode.
static void jpeg_keep(const uint8_t *src, size_t len) {
    if (s_keep == NULL || s_keep_ready) return;
    memcpy(s_keep, src, len);
    s_keep_len = len;
    s_want = false;
    // The fence, not the volatiles, is what makes the length safe to trust:
    // volatile only stops the compiler reordering these stores, and the puller
    // is on core 0 while the consumer runs on core 1. Publishing ready last
    // means a consumer that sees it also sees the matching length and bytes.
    // Cheaper than a mutex, and the puller must never block on the uplink.
    __sync_synchronize();
    s_keep_ready = true;
    s_dbg_kept += (uint32_t)len;
}

// Decode stays INLINE here, deliberately. It was tried in its own task, first on
// core 0 below the puller and then on core 1 below LVGL, and both were worse:
// 9.5fps and 6.9fps against 15.7fps inline. Neither core has idle time to
// overlap into — core 0 runs the WiFi stack and this puller, core 1 is LVGL
// blitting at 30fps — so a split only adds a copy and pays for being preempted
// mid-decode (measured decode time doubled from 31ms to 60ms on core 1). If a
// future revision frees up a core, revisit; until then inline is both simpler
// and faster.
//
// Pull one MJPEG session: connect, request the endpoint, then carve complete
// JPEGs out of the byte stream by scanning for SOI (FF D8) .. EOI (FF D9). This
// is framing-agnostic (ignores multipart boundaries/Content-Length), so it
// can't desync on header formatting. Returns when the link drops.
//
// The scan reads from an internal-RAM chunk and copies out in RUNS. Copying
// byte-at-a-time instead would put every one of tens of KB/s of stores through
// PSRAM individually, where a single-byte write costs far more than the same
// span as one memcpy — that inner loop was the puller's dominant cost.
static void pull_session(NetPuller *np, const IPAddress &ip) {
    WiFiClient client;
    // No Stream::setTimeout: it takes milliseconds (this line used to pass
    // seconds, so 4), and nothing here reads through the Stream helpers it gates
    // - the scan loop below owns READ_STALL_MS.
    if (!client.connect(ip, 80, CONNECT_TIMEOUT_MS)) {
        // Deliberately NOT a net_note_uplink_fail() vote. A camera can be off, rebooting, or on
        // its own bad AP while this panel's uplink to the rest of the LAN is perfectly fine, and
        // cycling the radio for that would drop a working server link to chase a problem the
        // camera owns. Only the server poll (plantrx) votes the uplink dead; see net.h.
        return;
    }
    net_note_uplink();  // reached the CAM over TCP - proof the uplink is alive, so it resets the
                        // watchdog even when the server itself is down
    client.printf("GET %s HTTP/1.1\r\nHost: cam\r\nConnection: close\r\n\r\n", np->path);

    static uint8_t rx[4096];
    size_t got = 0;          // bytes buffered into np->jpeg_buf
    bool in_frame = false;
    uint8_t prev = 0;        // last byte, for cross-chunk FF D8 / FF D9 detection
    uint32_t last_rx = millis();
    uint32_t last_decode_ms = 0;  // decode-gate clock; 0 forces the first frame through

    while (client.connected()) {
        // An update started while this session was open. Leave now and let the socket close;
        // see net_task for why the camera gets out of the way rather than sharing the core.
        if (updatemode_active()) break;
        uint32_t t_idle = micros();
        int n = client.read(rx, sizeof(rx));
        if (n <= 0) {
            // Nothing buffered: this is the only place worth sleeping. Yielding
            // after every partial read instead caps the pull at one buffer per
            // tick and leaves the sender's window closed for no reason.
            if (millis() - last_rx > READ_STALL_MS) break;
            vTaskDelay(pdMS_TO_TICKS(2));
            s_us_idle += micros() - t_idle;
            continue;
        }
        s_us_idle += micros() - t_idle;
        last_rx = millis();
        s_dbg_bytes += (uint32_t)n;
        net_note_uplink();  // bytes still flowing from the LAN; keep the link marked alive

        uint32_t t_mark = micros();
        int i = 0;
        while (i < n) {
            if (!in_frame) {
                while (i < n) {
                    uint8_t b = rx[i++];
                    bool soi = (prev == 0xFF && b == 0xD8);
                    prev = b;
                    if (soi) {
                        np->jpeg_buf[0] = 0xFF;
                        np->jpeg_buf[1] = 0xD8;
                        got = 2;
                        in_frame = true;
                        break;
                    }
                }
                continue;
            }
            // Inside a frame: find EOI, then copy the whole span at once. The
            // span deliberately includes the FF D9 pair.
            int start = i;
            bool eoi = false;
            while (i < n) {
                uint8_t b = rx[i++];
                eoi = (prev == 0xFF && b == 0xD9);
                prev = b;
                if (eoi) break;
            }
            size_t span = (size_t)(i - start);
            if (got + span > MAX_JPEG) {
                in_frame = false;  // oversized/garbage: resync on the next SOI
                continue;
            }
            memcpy(np->jpeg_buf + got, rx + start, span);
            got += span;
            if (eoi) {
                s_us_scan += micros() - t_mark;
                // Raw JPEG first, always, and here and nowhere else: this is the
                // only point in the loop where jpeg_buf holds SOI..EOI, and it is
                // before the decoder, which takes the buffer non-const and may write
                // through it. Kept ahead of and independent of the decode gate below
                // so throttling or skipping the decode never starves the uplink.
                if (s_want) jpeg_keep(np->jpeg_buf, got);
                // Gate the decode. Viewing: at most ~25fps. Hidden: ~2fps keep-alive
                // so camnet_live() and aijudge's peek stay fed while the ~31ms decode
                // stops hammering core 0. A skipped frame leaves jpeg_buf untouched,
                // so the next SOI just reuses it.
                uint32_t now_ms = millis();
                uint32_t floor_ms = s_viewing ? DECODE_MIN_INTERVAL_MS
                                              : DECODE_HIDDEN_INTERVAL_MS;
                if (last_decode_ms == 0 || now_ms - last_decode_ms >= floor_ms) {
                    last_decode_ms = now_ms;
                    uint32_t t_dec = micros();
                    camframe_submit(np->cf, np->jpeg_buf, got);
                    s_us_decode += micros() - t_dec;
                }
                s_dbg_frames++;
                in_frame = false;
                // Hand core 0 back to its idle task for a tick on every complete
                // frame, decoded or not: the loop otherwise only sleeps on an empty
                // socket (:152), so a full send window kept it from ever yielding and
                // starved lwIP RX and the task watchdog on this core. Same rationale
                // as the yields in ota.cpp:94-99 / fwpull.cpp:441-450.
                vTaskDelay(pdMS_TO_TICKS(2));
                t_mark = micros();  // scan time for the rest of this chunk
            }
        }
        s_us_scan += micros() - t_mark;
    }
    client.stop();
}

static void net_task(void *arg) {
    NetPuller *np = (NetPuller *)arg;
    for (;;) {
        // WHY THE CAMERA STOPS DURING A FIRMWARE UPDATE.
        //
        // This task and the OTA task share core 0, and this one loses on purpose. Measured:
        // during an update the decode cost per frame went 22.8ms -> 66.1ms, the pull fell
        // 20fps -> 11.4fps, and then the board took a task-watchdog reset 2.6s later. The
        // update task runs at priority 5 and hands back about half a millisecond per chunk;
        // this task at priority 2 took every one of those windows, so the core-0 idle task -
        // which is what the watchdog actually watches - got none of them and the board died
        // mid-flash. Four OTA attempts, two resets.
        //
        // Dropping the stream rather than throttling it, because a 20fps MJPEG feed nobody
        // can consume is not worth a socket: the sender's window stays open, the frames pile
        // up, and the panel would show stale video anyway. The UI keeps painting from core 1
        // and shows update progress, which is the only thing worth looking at for 40 seconds.
        if (updatemode_active()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Pull only once the CAM reports itself online with a usable IP.
        uint8_t ip4[4];
        if (WiFi.status() == WL_CONNECTED && camprov_cam_ip4(ip4)) {
            pull_session(np, IPAddress(ip4[0], ip4[1], ip4[2], ip4[3]));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));  // retry / re-check cadence
    }
}

static void start_puller(NetPuller *np, const char *path, const char *task_name) {
    np->path = path;
    np->jpeg_buf = (uint8_t *)heap_caps_malloc(MAX_JPEG, MALLOC_CAP_SPIRAM);
    np->cf = camframe_create();
    if (np->jpeg_buf == NULL || np->cf == NULL) {
        return;  // PSRAM exhausted; the camera feed stays disabled
    }
    // One global slot rather than one per puller: only the RGB stream exists and
    // only the uplink taps it. Allocated after the puller's own buffers so a
    // camera that never comes up does not strand 60KB of PSRAM, and before the
    // task starts so the puller never sees a half-published pointer.
    if (s_keep == NULL) {
        s_keep = (uint8_t *)heap_caps_malloc(MAX_JPEG, MALLOC_CAP_SPIRAM);
    }
    // Core 0, with the network stack it feeds off. Priority 2 keeps it below
    // nothing in particular; it is simply a background pull.
    // 6KB rather than 8KB: measured 2,224 bytes used while streaming at a sustained 20fps
    // (FreeRTOS high-water mark), and internal DRAM is the memory this board is short of.
    // That still leaves 3.9KB of margin on the hottest task in the system.
    xTaskCreatePinnedToCore(net_task, task_name, 6144, np, 2, NULL, 0);
}

void camnet_init(void) {
    start_puller(&s_rgb, "/rgb/stream", "camnet_rgb");
}

bool camnet_live(void) {
    return camframe_live(s_rgb.cf, LIVE_TIMEOUT_MS);
}

// Monitor page -> puller. A plain volatile store: the puller reads it once per
// frame to pick the decode floor, and a stale read costs at most one frame at the
// wrong rate, so no fence or mutex is warranted.
void camnet_set_viewing(bool viewing) {
    s_viewing = viewing;
}

bool camnet_take_scaled(uint16_t *dst, int dst_w, int dst_h,
                        const int16_t *sx_map, const int16_t *sy_map) {
    return camframe_take_scaled(s_rgb.cf, dst, dst_w, dst_h, sx_map, sy_map);
}

bool camnet_peek_scaled(uint16_t *dst, int dst_w, int dst_h,
                        const int16_t *sx_map, const int16_t *sy_map) {
    return camframe_peek_scaled(s_rgb.cf, dst, dst_w, dst_h, sx_map, sy_map);
}

void camnet_jpeg_request(void) {
    if (s_keep == NULL) return;  // no slot: leave the puller's test false forever
    s_want = true;
}

bool camnet_jpeg_ready(void) {
    return s_keep_ready;
}

const uint8_t *camnet_jpeg(size_t *len) {
    if (!s_keep_ready) return NULL;
    __sync_synchronize();  // pairs with jpeg_keep: bytes and length precede ready
    if (len) *len = s_keep_len;
    return s_keep;
}

void camnet_jpeg_release(void) {
    s_keep_len = 0;
    // Clearing ready last, after the fence: the puller may refill the slot the
    // instant it goes false, so nothing may still be read after this line. The
    // request flag is deliberately untouched - the consumer arms the next frame.
    __sync_synchronize();
    s_keep_ready = false;
}

void camnet_debug_tick(void) {
    static uint32_t s_print_ms = 0;
    uint32_t now = millis();
    if (now - s_print_ms < 4000) return;
    uint32_t span = now - s_print_ms;
    s_print_ms = now;

    // The puller only runs once the CAM's ESP-NOW beacon reports a usable IP, so
    // print that gate too: "offline" with no IP is a CAM problem, "offline" with
    // an IP is ours.
    char ip[20];
    camprov_cam_ip(ip, sizeof(ip));
    uint32_t f = s_dbg_frames ? s_dbg_frames : 1;
    hlogf("[camnet] %.1ffps %.1fKB/s | scan=%lu decode=%lu idle=%lu us/frame "
                  "| tap=%luB | %s | cam=%s ip=%s\n",
                  span ? (s_dbg_frames * 1000.0f / span) : 0.0f,
                  span ? (s_dbg_bytes / (float)span) : 0.0f,
                  (unsigned long)(s_us_scan / f),
                  (unsigned long)(s_us_decode / f),
                  (unsigned long)(s_us_idle / f),
                  (unsigned long)s_dbg_kept,
                  camnet_live() ? "LIVE" : "offline",
                  camprov_cam_online() ? "online" : "offline", ip);

    // The timing trio resets with the counters it is divided by. It did not, so
    // `s_us_decode / frames_this_window` was microseconds-since-boot over frames in
    // the last four seconds: a figure that climbs forever and describes no frame
    // that was ever decoded. It read 6,030,642 us/frame on a link running 20fps,
    // which is 52ms of budget - the number was six thousand percent of the whole
    // frame time and nobody could have acted on it.
    s_dbg_bytes = s_dbg_frames = s_dbg_kept = 0;
    s_us_scan = s_us_decode = s_us_idle = 0;
}
