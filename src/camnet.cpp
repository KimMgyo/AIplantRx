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
// The decode is the puller's one expensive act: JPEGDEC Huffman/IDCT plus ~4800
// scattered PSRAM stores, all on core 0 beside the WiFi/lwIP driver. Left ungated
// on a sender that keeps its window full, the pull loop never hits its one
// vTaskDelay (only the no-data path at :273 sleeps), so core 0's idle task -
// which the lwIP RX path and the task watchdog run behind - is starved.
// Three gates fix that: a floor between decodes so a fast sender can't drive more
// than ~25fps of them, a budget that widens that floor in proportion to what a
// decode actually costs once one costs anything much, and dropping the decode to a
// keep-alive rate whenever the monitor canvas is off screen and nothing draws the
// RGB frame.
//
// WHAT A DECODE COSTS, MEASURED.
//
// 2.8ms, and not the ~31ms this note claimed for the life of the flat floor. Two
// healthy windows, 2026-08-23, monitor page up and the feed being watched:
//
//   16.9fps  69.2KB/s  scan=343  decode=2839  idle=53475  us/frame
//
// and a second at 17:09 that reads decode=2795 idle=44059 on the same feed, so
// 2.8ms is the steady state and not one lucky window.
//
// So the steady state is a 2.8ms decode inside the sender's ~59ms frame: ~5% duty,
// ~50ms of the frame handed back to core 0, and the 40ms floor below never binds
// because the sender's own frame is longer than it. The old ~31ms figure does not
// reproduce on this build; it survives here as the number the flat floor was sized
// against, and it is also the number the core-1 experiment at :216 was measured
// against, so treat that experiment's absolute values as a different baseline than
// today's while its conclusion stands.
//
// WHY A PROPORTIONAL BUDGET ON TOP OF THE FLOOR.
//
// Because the cost is not stable, and the board died in the window where it was
// not. The five windows before an int_wdt reset - v0.4.5, 2026-08-23, idle
// greenhouse, nothing being flashed, health counter 205 -> 206:
//
//   18.5fps  76.0KB/s  scan=469  decode=65784  idle=2045  us/frame
//   20.1fps  82.8KB/s  scan=438  decode=40674  idle=6729
//   19.6fps  80.8KB/s  scan=434  decode=47201  idle=2033
//   19.1fps  77.5KB/s  scan=410  decode=48261  idle=2014
//   12.4fps  51.7KB/s  scan=431  decode=76416  idle=2202   <- last line before it died
//
// 40-76ms against a 2.8ms steady state is a 27x EXCURSION, not a baseline - and
// idle falling 53ms -> 2ms per frame is the same event seen from the other side.
// Whatever stalls the decode that hard is not this file's to fix, and it is not
// JPEGDEC: a cost dominated by scattered PSRAM stores going 27x slower in bursts is
// the PSRAM bus being taken away, and on this board it is. PSRAM here is octal and
// shares the SPI controller with flash, and Espressif document that PSRAM is
// disabled for the duration of a flash write - so anything that writes flash or NVS
// while the panel is merely displaying buys a stall of exactly this shape, and a
// long enough one is itself an int_wdt. That writer is being hunted elsewhere; what
// this file must survive is the consequence. At 76ms of decode inside a 59-80ms
// frame there is nothing left for core 0's idle task, and the flat floor cannot
// help because 40ms is SHORTER than the work it was meant to space out - the decode
// alone overruns it, so the test is already satisfied by the time it is reached.
// updatemode.cpp records the same shape from the other direction: 22.8ms -> 66.1ms
// per decoded frame and a task-watchdog reset 2.6s later.
//
// So the floor keeps the normal feed and a proportional budget bounds the
// excursion. After a decode costing C the next one waits until C + C/GAP_DIVISOR
// has passed since this one STARTED; the decode's own runtime counts against that
// interval, so what is reserved is a gap of C/GAP_DIVISOR after it. Below ~30ms of
// decode the floor is the larger of the two and this term does nothing whatsoever -
// which, at 2.8ms a decode, is all of the time.
//
// s_viewing defaults true: until the monitor page reports otherwise the safe
// assumption is that someone is watching. page_monitor.cpp drives it off the same
// canvas-visibility test that gates its scaling; see camnet_set_viewing.
static volatile bool s_viewing = true;
// ~25fps ceiling on decodes while viewing. The native inline rate is ~16-20fps
// (see the decode-cost note below), so this only caps a pathologically fast
// sender and never touches the normal feed. Still true at 2.8ms per decode, and
// still the whole gate there; what it cannot do is bound an excursion, because a
// decode dearer than the floor satisfies the floor by itself. Hence the divisor.
static const uint32_t DECODE_MIN_INTERVAL_MS = 40;
// The gap owed to core 0 after a decode, as a divisor of that decode's own cost:
// 3 reserves a third of the cost, so a decode holds at most 3/4 of the wall clock
// and a quarter of it is left for the idle task, lwIP RX and the watchdog however
// dear the decode gets. Three, for two reasons that both come from measurement.
//
// It must not touch the feed. This term only exceeds the 40ms floor above 30ms of
// decode (40 * 3/4), which is more than ten times the 2.8ms steady state, so in
// normal operation it is not merely slack but inert. Even at the old ~31ms figure
// it asks for a 41ms interval, still inside the sender's 50-59ms frame, so every
// frame would still decode and the preview rate would be untouched. A divisor of 1
// - half the clock - would demand 62ms at that cost, overshoot the sender's frame
// and halve a preview the board could plainly afford.
//
// It must leave enough. The reset above happened with ~4% of the frame outside the
// decode. A quarter is six times that, and it is the floor rather than the target:
// dropping a frame can only lengthen the interval, never shorten it, so the
// sustained duty cycle lands at or under 3/4 and the idle share at or over a
// quarter. The single frame that discovers a new cost is the one exception, and it
// is documented at the gate itself.
static const uint32_t DECODE_GAP_DIVISOR = 3;
// While the canvas is hidden, decode this rarely - just often enough to keep
// camframe's last_ms inside LIVE_TIMEOUT_MS so camnet_live() stays true, and to
// hand aijudge's 3-minute thumbnail peek a recent frame. ~2fps drops decode load
// ~90% vs the viewing path. Deliberately NOT zero: camnet_live() gates plantrx's
// uplink request (plantrx.cpp:753) and aijudge's evidence peek (aijudge.cpp:294),
// both of which would stop the moment last_ms went stale.
static const uint32_t DECODE_HIDDEN_INTERVAL_MS = 500;

// The cost the budget reacts to, in microseconds. Not an average of any kind: an
// excursion is a handful of frames long, and a mean is the wrong instrument for
// something that rare. A 3/4-weighted running mean fed one 76ms frame on top of a
// 2.8ms history reads 21ms - under the 30ms engage threshold - so the whole
// excursion would pass with the budget still asleep, which is precisely the
// failure being fixed. So: attack instantly, release gradually. Each decode raises
// this to its own cost if it is dearer, and otherwise decays it by a quarter.
//
// Decaying per DECODE rather than per millisecond is what keeps this honest about
// "recent": from 76ms it is back under the 30ms threshold in four decoded frames
// and back at the steady state in about eleven, so a decode that went long an hour
// ago has been divided by 0.75 some tens of thousands of times and throttles
// nothing. It also cannot feed on itself - a gated frame is not a decode, so it
// neither raises this nor decays it, and the budget can only be sustained by
// decodes that really are still expensive.
static volatile uint32_t s_decode_cost_us = 0;
// Frames this debug window whose decode the budget or the hidden rate dropped.
// Printed rather than kept internal: idle falling through a floor is now something
// this file ACTS on, and the [camnet] line is what made the starvation diagnosable
// in the first place, so the reaction belongs beside the three numbers that caused
// it instead of being inferred from them.
static volatile uint32_t s_dbg_gated = 0;

static void decode_cost_note(uint32_t us) {
    uint32_t decayed = s_decode_cost_us - s_decode_cost_us / 4;
    s_decode_cost_us = (us > decayed) ? us : decayed;
}

// The interval the next decode must wait for, measured from the start of the last
// one. Whole milliseconds: the floor is in milliseconds, the gate is compared
// against millis(), and a decode cheap enough for the microseconds to matter is a
// decode this function is not gating anyway.
static uint32_t decode_gate_ms(void) {
    uint32_t cost_ms = s_decode_cost_us / 1000;
    uint32_t budget_ms = cost_ms + cost_ms / DECODE_GAP_DIVISOR;
    return (budget_ms > DECODE_MIN_INTERVAL_MS) ? budget_ms : DECODE_MIN_INTERVAL_MS;
}

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
// mid-decode (measured decode time doubled from 31ms to 60ms on core 1). That
// 31ms is not what a decode costs on this build: it measures 2.8ms today, and the
// 40-76ms in the throttle note above is an excursion, not a return to it. So read
// those three rates as one comparison taken under one baseline, whatever that
// baseline was, rather than as figures to hold today's build against — the finding
// is what stands, and it stands on the ratio: a split adds a copy and pays for
// preemption, and both alternatives lost by a third or more. If a future revision
// frees up a core, revisit; until then inline is both simpler and faster.
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
                // Gate the decode. Viewing: the ~25fps floor, widened to
                // decode_gate_ms() whenever a decode has just cost more than ~30ms,
                // so an excursion cannot spend the whole frame in the decoder.
                // Hidden: ~2fps keep-alive so camnet_live() and aijudge's peek stay
                // fed while the decode stops touching core 0 at all. A skipped frame
                // leaves jpeg_buf untouched, so the next SOI just reuses it.
                //
                // The gate is a skip and NOT a sleep, deliberately: the socket keeps
                // draining at the sender's rate, so the frame that finally decodes is
                // the freshest one rather than the head of a backlog, the tap above
                // still sees every frame, and the sender's window never closes. The
                // price is quantisation - the interval can only land on a frame
                // boundary, so the realised gap rounds UP to the next one and the duty
                // cycle settles at or under the 3/4 the divisor asks for, never over.
                // Simulated against the sender's measured 59ms frame: a sustained 50ms
                // decode gates every second frame for 8.5fps at 43% duty, a sustained
                // 76ms one the same 8.5fps at 64%. The exception is the single frame
                // that DISCOVERS a new cost - it runs against the old gate, so the
                // first frame of an excursion can overrun its share, and the gate
                // closes behind it. One 76ms frame is not a watchdog reset; the five
                // four-second windows of them above were.
                uint32_t now_ms = millis();
                uint32_t gate_ms = s_viewing ? decode_gate_ms()
                                             : DECODE_HIDDEN_INTERVAL_MS;
                if (last_decode_ms == 0 || now_ms - last_decode_ms >= gate_ms) {
                    last_decode_ms = now_ms;
                    uint32_t t_dec = micros();
                    camframe_submit(np->cf, np->jpeg_buf, got);
                    // This decode's own cost sizes the next gate. Measured, not
                    // predicted: whatever made this frame dear is still true one frame
                    // later, and one frame is as far ahead as this can usefully see.
                    uint32_t cost_us = micros() - t_dec;
                    s_us_decode += cost_us;
                    decode_cost_note(cost_us);
                } else {
                    s_dbg_gated++;
                }
                s_dbg_frames++;
                in_frame = false;
                // Hand core 0 back to its idle task for a tick on every complete
                // frame, decoded or not: the loop otherwise only sleeps on an empty
                // socket (:273), so a full send window kept it from ever yielding and
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
// frame to pick the decode gate, and a stale read costs at most one frame at the
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
    // Read the pair once and together: the puller bumps gated immediately before
    // frames, so a read that straddles those two lines can see gated == frames + 1.
    uint32_t frames = s_dbg_frames, gated = s_dbg_gated;
    uint32_t f = frames ? frames : 1;
    // decode is per DECODED frame, not per pulled frame. s_us_decode only accrues on
    // frames that reached the decoder, so dividing it by everything that arrived would
    // report a fraction of the true cost exactly when the gate is dropping frames -
    // that is, during the excursion this number exists to expose. It would also have
    // divided the hidden-canvas windows by ten. Same class of error as the stale
    // counter noted at the bottom of this function: a decode figure that describes no
    // frame that was ever decoded.
    uint32_t decoded = (frames > gated) ? frames - gated : 0;
    uint32_t d = decoded ? decoded : 1;
    hlogf("[camnet] %.1ffps %.1fKB/s | scan=%lu decode=%lu idle=%lu us/frame "
                  "| tap=%luB | %s | cam=%s ip=%s\n",
                  span ? (frames * 1000.0f / span) : 0.0f,
                  span ? (s_dbg_bytes / (float)span) : 0.0f,
                  (unsigned long)(s_us_scan / f),
                  (unsigned long)(s_us_decode / d),
                  (unsigned long)(s_us_idle / f),
                  (unsigned long)s_dbg_kept,
                  camnet_live() ? "LIVE" : "offline",
                  camprov_cam_online() ? "online" : "offline", ip);

    // When the gate is holding the feed down, say so and say why, on the line under
    // the three numbers that caused it. Silent in normal operation by construction:
    // at the 2.8ms steady state the budget sits on its floor and the sender's frame
    // is longer than the floor, so nothing gets gated and nothing prints. A line here
    // at all means either a decode went ten times long - the state that preceded the
    // int_wdt reset, now readable while it is happening rather than reconstructible
    // from a crash counter afterwards - or the sender has started outrunning 25fps.
    // The hidden path is excluded: it gates ~90% of frames on purpose and would say
    // nothing but that.
    uint32_t gate_ms = decode_gate_ms();
    if (s_viewing && (gated || gate_ms > DECODE_MIN_INTERVAL_MS)) {
        hlogf("[camnet] decode gate holding the feed: cost=%lums -> gate=%lums "
              "(floor %lums), %lu of %lu frames gated, %.1ffps decoded. %s\n",
              (unsigned long)(s_decode_cost_us / 1000), (unsigned long)gate_ms,
              (unsigned long)DECODE_MIN_INTERVAL_MS,
              (unsigned long)gated, (unsigned long)frames,
              span ? (decoded * 1000.0f / span) : 0.0f,
              gate_ms > DECODE_MIN_INTERVAL_MS
                  ? "That is the proportional budget: core 0 is kept at >=1/4 of the "
                    "wall clock, which at this decode cost ungated measured ~4% and "
                    "took an int_wdt reset."
                  : "That is just the ~25fps floor - the sender is offering frames "
                    "faster than the decode is allowed to run.");
    }

    // The timing trio resets with the counters it is divided by. It did not, so
    // `s_us_decode / frames_this_window` was microseconds-since-boot over frames in
    // the last four seconds: a figure that climbs forever and describes no frame
    // that was ever decoded. It read 6,030,642 us/frame on a link running 20fps,
    // which is 52ms of budget - the number was six thousand percent of the whole
    // frame time and nobody could have acted on it.
    s_dbg_bytes = s_dbg_frames = s_dbg_kept = s_dbg_gated = 0;
    s_us_scan = s_us_decode = s_us_idle = 0;
}
