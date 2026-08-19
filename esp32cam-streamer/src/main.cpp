// ESP32-CAM (AI-Thinker) -> WiFi camera streamer.
//
// One capture, fanned out to two WiFi consumers:
//   1. RTSP/MJPEG -> phones / VLC on the LAN:
//        rtsp://<ip>:8554/mjpeg/1
//   2. HTTP -> browser:
//        /rgb/image   one frame as a standalone JPEG
//        /rgb/stream  live MJPEG
//
// The board has nothing to configure by hand: WiFi creds arrive over ESP-NOW
// from the S3 display on first boot (see provision.h) and are cached in NVS.
// UART0 is the flashing and console port; the status line in loop() reports
// the IP the RTSP and HTTP servers came up on.
//
// The thing that made this board look like it had a hopeless WiFi link was the
// camera itself. At the stock 20MHz sensor clock its DMA into PSRAM ran flat out
// and left the radio starved: pinged from the PC the board answered in 387ms on
// average against the S3's 2ms on the same AP, and a 3KB socket write took
// anywhere from 45ms to 1.3s. With the camera never initialised the same board
// pinged at 4ms, which is what identified the culprit. Dropping the sensor clock
// to 8MHz (see make_cam_config) took ping to 5ms, and the delivered stream from
// a ragged 1.4-2.6fps to a steady 14fps at ~137KB/s.
//
// The rest is kept tight to match: the loop grabs every sensor frame but only
// pays for the ones a consumer can take, and decides that AFTER the grab rather
// than before (that ordering alone was worth 10 -> 14fps); RTSP is paced and
// runs on its own task; the HTTP mailbox is double-buffered so transmit overlaps
// capture; each consumer gets exactly one copy; and nothing reconfigures the
// driver while the stream is live. What remains is the link itself at ~142KB/s.
// The levers left are outside this firmware - the AP's 2.4GHz channel (it sits
// on 2, overlapping a neighbour on 1 and one at 28% utilisation on 8), a direct
// ESP-NOW path that skips the AP hop, or this board's antenna and 5V supply.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "CStreamer.h"
#include "CRtspSession.h"
#include "provision.h"
#include "nodeagent.h"

// AI-Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

static const uint32_t MAX_FRAME = 60 * 1024;  // sanity cap, QVGA q15 is ~5-10KB
static const int CAM_W = 320, CAM_H = 240;

// Everything the camera produces is QVGA q15 - both the live stream and the
// /rgb/image still. A higher-resolution still was tried and this board cannot
// produce one without wedging the live feed; see serve_photo().
// (lower quality number = better/larger)
static const framesize_t STREAM_FS = FRAMESIZE_QVGA;   // 320x240
static const int         STREAM_Q  = 15;

// Quality is deliberately FIXED, and adapting it to hold a byte budget was tried
// and removed. Now that the sensor clock is no longer starving the radio the
// link does carry a steady ~99KB/s, so frame size genuinely does divide into the
// frame rate - but this sensor's quality knob has almost no authority over it.
// Walking q from 15 to 32 on a static scene moved a frame from 7990 to 6127
// bytes, 23%, and bought no measurable frame rate. What it would cost is the
// picture: the camera here is pointed at a monitor full of dense coloured text,
// which is why frames run 9-10KB rather than the usual 2-4KB, and that is
// exactly the content a coarser quantiser destroys first. The status line
// reports B/f so the trade stays visible if the scene ever changes.

static const int FLASH_LED_PIN = 4;  // AI-Thinker onboard white flash LED

// A CStreamer that packetizes a JPEG we hand it, instead of grabbing its own
// frame (Micro-RTSP's OV2640Streamer would capture a second time). We feed it
// the exact buffer the loop just captured, so every consumer shares one frame.
class SharedStreamer : public CStreamer {
public:
    SharedStreamer(u_short w, u_short h) : CStreamer(w, h) {}
    const uint8_t *buf = nullptr;
    uint32_t len = 0;
    void streamImage(uint32_t curMsec) override {
        if (buf != nullptr && len > 0) {
            streamFrame(buf, len, curMsec);
        }
    }
};

static WiFiServer rtspServer(8554);
static SharedStreamer *streamer = nullptr;
static WiFiClient *rtsp_client = nullptr;  // owned here; the lib never frees it

static WiFiServer httpServer(80);          // browser MJPEG at http://<ip>/
static WiFiClient http_client;             // one browser viewer

static bool cam_ok = false;
static bool rtsp_up = false;

// Frames handed to the consumers since boot. Cheap, and it is what separates a
// quiet board from one whose sensor has stopped delivering.
static uint32_t frame_count = 0;

// Console heartbeat. Throttled because a per-frame print would swamp the port
// and pace the capture loop against 115200 baud.
static const uint32_t STATUS_MS = 5000;

// RTSP send interval. Micro-RTSP does not pace itself: streamImage() packetizes
// and sends whatever it is handed, whenever it is handed one. Driven straight
// off the capture loop that meant RTP at the full sensor rate - about 50fps back
// when the sensor ran at 20MHz - which overran the link by several times over
// and is what made RTSP unstable rather than merely slow. 80ms holds it to
// ~12fps. At 9-10KB a frame that is still more than the ~99KB/s the link
// carries, so it stays a ceiling rather than a target, and it leaves room for
// the HTTP viewer sharing the same radio.
static const uint32_t RTSP_INTERVAL_MS = 80;

// Where the loop's wall clock actually goes, accumulated since the last status
// line. Both consumers can block: RTSP packetizes into the socket, and the HTTP
// write is bounded only by the viewer's send timeout. A slow viewer therefore
// paces the capture loop, and without a breakdown that is indistinguishable
// from a slow sensor.
static uint32_t s_us_capture = 0, s_us_rtsp = 0, s_us_http = 0, s_win_frames = 0;
static uint32_t s_us_write = 0, s_win_written = 0;  // the socket write itself
static uint32_t s_rtsp_last_ms = 0;   // paces streamImage(); see RTSP_INTERVAL_MS
static uint32_t s_win_idle = 0;       // loop passes that needed no frame at all
static uint32_t s_win_grabs = 0;      // fb_get calls; cap= is per grab, not per sent frame
static uint64_t s_win_bytes = 0;      // JPEG bytes of the frames we actually used

// The capture loop must never block on a consumer. Measured: one /rgb/stream
// viewer that could not keep up made write() take 1.05s per frame, dragging the
// board from 10.8fps to 0.9 — and because the same loop services RTSP, it
// starved that too. (setConnectionTimeout() does NOT bound write(); it is the
// connect timeout, so the socket blocks until the viewer drains it.)
//
// So the loop hands the writer task a copy and moves on. One buffer is enough:
// a non-empty mailbox means the task is still busy, and the loop then simply
// skips this frame for HTTP. MJPEG is a latest-frame stream, so dropping frames
// for a slow viewer is the correct trade — and RTSP keeps full rate regardless.
//
// The mailbox holds the COMPLETE multipart chunk, boundary header and trailing
// CRLF included, so the writer issues exactly one write() per frame. Three
// separate writes put a ~60 byte segment, then the image, then a 2 byte segment
// on the wire for every frame, which is a poor shape for TCP and measurably
// slower.
//
// Two slots, not one, so capture and transmit overlap: with a single slot the
// loop cannot stage the next frame while the writer is still sending, and any
// write that overran the 48ms sensor period cost a whole frame.
//
// Be honest about the size of that: it bought 14.0 -> 14.3fps, about 1.5%. The
// handshake was not the limit - the link is. What the second slot changed is
// visible in the write timing, which went from 44ms to 66ms per frame: the
// writer is now continuously busy instead of waiting on the loop, and 9.4KB in
// 66ms is the ~142KB/s this link actually carries. It is kept because delivery
// now tracks the link rate directly rather than quantising to the sensor period,
// which is what will pick up any future headroom.
//
// No mutex: one producer (the loop) and one consumer (the writer task), each
// owning its own index and only ever writing its own end of the handshake - the
// producer takes a slot from 0 to len, the consumer returns it from len to 0.
// Two slots keep frames in capture order without any further bookkeeping.
static const size_t HTTP_HDR_MAX = 96;
static const int    HTTP_SLOTS = 2;
static uint8_t *s_http_buf[HTTP_SLOTS] = {nullptr, nullptr};
static volatile size_t s_http_len[HTTP_SLOTS] = {0, 0};
static int s_http_prod = 0;   // loop only
static int s_http_cons = 0;   // writer task only
static uint32_t s_http_written = 0, s_http_short = 0;

// RTSP gets the same treatment as the HTTP viewer, and for the same measured
// reason: streamImage() writes straight into the client socket, and over TCP
// interleaved that blocks until the viewer drains it. Left in loop() it stalled
// the capture for 98ms, 598ms and 59ms in three consecutive status windows,
// dragging the board to 1.6fps and starving the HTTP viewer with it. Hand the
// frame to a task instead and let the loop move on.
//
// CStreamer is not thread-safe, so rtsp_task owns it outright - see there. The
// loop only ever writes this mailbox and reads the session flag.
static uint8_t *s_rtsp_buf = nullptr;   // MAX_FRAME, PSRAM
static volatile size_t s_rtsp_len = 0;  // bytes of a frame waiting to go out
static volatile bool s_rtsp_sessions = false;  // published by rtsp_task

// Set while the writer is actually touching the socket, so the accept path can
// wait before replacing http_client under it. Swapping a WiFiClient out from
// under an in-flight write is a use-after-free waiting to happen.
static volatile bool s_http_busy = false;

// ---- standing the streamer down for a firmware download ---------------------
//
// An OTA write on this board competes with the two things it does for a living. RTSP and HTTP
// MJPEG hold PSRAM frame buffers and between them saturate a link that carries ~142KB/s on a good
// day, and the camera's DMA into PSRAM is what was measured adding ~380ms of latency to every
// packet this board sent at the stock sensor clock (see the header). A download fighting all of
// that does not merely go slowly, it stalls - and a stalled OTA on a camera bolted to a
// greenhouse wall is the expensive failure the whole feature exists to avoid.
//
// WHY THIS IS A HANDSHAKE AND NOT vTaskSuspend(). Suspending rtsp_task or http_writer_task by
// force stops them wherever they happen to be, and where they usually are is inside a socket
// write - which on ESP32 means holding lwIP's core lock. Freezing a task in there deadlocks the
// TCP stack for everything else on the board, the download that suspended it included. So each
// path is ASKED to stop and publishes an acknowledgement once it has reached a point where it
// holds nothing, and the camera driver is only torn down after all three have answered.
//
// The two LISTENING sockets stay bound throughout, and that is deliberate. Nothing is accepted
// while the paths are parked - rtsp_task never reaches rtspServer.accept() and loop() returns
// before the HTTP one - so a viewer arriving mid-download waits in the backlog and is served
// when the board comes back, which is a better answer than a refused connection plus the mDNS
// re-advertisement that rebinding them would need.
//
// Each *_parked flag is written by exactly one task and read by camstream_stand_down(), which is
// why none of them needs a lock: a stale read costs one 10ms poll and nothing else.
static volatile bool s_stand_down  = false;
static volatile bool s_loop_parked = false;   // written by loop() on core 1
static volatile bool s_rtsp_parked = false;   // written by rtsp_task
static volatile bool s_http_parked = false;   // written by http_writer_task

// Drains the mailbox at whatever rate the viewer can take. Runs on core 0 with
// the network stack; blocking here costs nothing because the capture loop is on
// core 1 and never waits for it.
static void http_writer_task(void *arg) {
    (void)arg;
    for (;;) {
        // Ahead of the connected() check on purpose: that branch never publishes an ack, so a
        // task sitting in it while the flag went up would make camstream_stand_down() wait out
        // its whole budget and then refuse a download that was perfectly safe to start.
        //
        // The ack can lag the request by as long as an in-flight write takes - 1.05s measured for
        // a viewer that could not keep up. loop()'s park closes the socket, which is what makes
        // that write return instead of sitting there.
        if (s_stand_down) {
            for (int i = 0; i < HTTP_SLOTS; i++) s_http_len[i] = 0;
            s_http_cons = 0;
            s_http_parked = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_http_parked = false;

        // Release both slots ONLY when the viewer is gone. Clearing after the
        // idle delay destroys whatever the capture loop queued during that
        // delay, which starves the stream instead of pacing it.
        if (!http_client.connected()) {
            for (int i = 0; i < HTTP_SLOTS; i++) s_http_len[i] = 0;
            s_http_cons = 0;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        size_t len = s_http_len[s_http_cons];
        if (len == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        s_http_busy = true;
        uint32_t t_wr = micros();
        size_t w = http_client.write(s_http_buf[s_http_cons], len);
        s_us_write += micros() - t_wr;
        s_win_written++;
        if (w == len) {
            s_http_written++;
        } else {
            s_http_short++;
            http_client.stop();  // partial write desyncs the multipart framing
        }
        s_http_len[s_http_cons] = 0;   // hand the slot back before advancing
        s_http_cons = (s_http_cons + 1) % HTTP_SLOTS;
        s_http_busy = false;
    }
}

// Everything RTSP runs here and nowhere else: accepting the viewer, answering
// the protocol, and pushing frames. Single ownership is the point - there is no
// lock, because nothing else touches the streamer.
//
// The first attempt did share it with the loop under a mutex, and that was worse
// than useless: streamImage() blocks on the client socket (up to 600ms measured
// over TCP interleaved), and the loop's handleRequests() then blocked on the
// same mutex for the whole send. The RTSP keepalives went unanswered, so the
// viewer froze, sat frozen, then dropped the session and reconnected. Moving the
// protocol next to the send means a slow send only delays RTSP's own traffic,
// which is unavoidable, and never the capture loop or the HTTP viewer.
static void rtsp_task(void *arg) {
    (void)arg;
    for (;;) {
        // Before the rtsp_up check, not after. On a board whose WiFi never came up rtsp_up is
        // false forever and the branch below spins on a 100ms delay without ever acknowledging
        // anything - which would make a stand-down time out on the one board most likely to be
        // asked for a firmware update.
        if (s_stand_down) {
            if (!s_rtsp_parked) {
                // Drop the viewer rather than leave it staring at a frozen frame for the length
                // of a download. stop() closes the socket and the next handleRequests() is what
                // makes CStreamer notice and reap the session - the same path a viewer that walks
                // away already takes, so the delete below is the same single ownership rule as
                // the accept branch further down.
                if (rtsp_client != nullptr) {
                    rtsp_client->stop();
                    streamer->handleRequests(0);
                    delete rtsp_client;
                    rtsp_client = nullptr;
                }
                s_rtsp_len = 0;
                s_rtsp_sessions = false;
                s_rtsp_parked = true;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_rtsp_parked = false;

        if (!rtsp_up) {           // set once the loop has bound the listener
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool sessions = streamer->anySessions();
        if (!sessions) {
            // The library stops the client but never deletes it, so the previous
            // one is freed here - after its session is gone, and on the only task
            // that can still be holding it.
            if (rtsp_client != nullptr) {
                delete rtsp_client;
                rtsp_client = nullptr;
            }
            s_rtsp_len = 0;  // drop whatever was queued for the session that ended
            WiFiClient c = rtspServer.accept();
            if (c) {
                rtsp_client = new WiFiClient(c);
                streamer->addSession(rtsp_client);
                sessions = true;
            }
        }
        s_rtsp_sessions = sessions;

        streamer->handleRequests(0);

        size_t len = s_rtsp_len;
        if (len != 0) {
            if (sessions) {
                uint32_t t_mark = micros();
                streamer->buf = s_rtsp_buf;
                streamer->len = len;
                streamer->streamImage(millis());
                streamer->buf = nullptr;
                s_us_rtsp += micros() - t_mark;
            }
            s_rtsp_len = 0;  // last: the loop refills only once this clears
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// What the association actually settled on. A board that falls back to 11g or
// 11b gets no A-MPDU aggregation, which shows up as long socket writes for
// small payloads - exactly the symptom this link has.
static const char *phy_name(void) {
    wifi_phy_mode_t m;
    if (esp_wifi_sta_get_negotiated_phymode(&m) != ESP_OK) return "?";
    switch (m) {
        case WIFI_PHY_MODE_LR:   return "LR";
        case WIFI_PHY_MODE_11B:  return "11b";
        case WIFI_PHY_MODE_11G:  return "11g";
        case WIFI_PHY_MODE_HT20: return "HT20";
        case WIFI_PHY_MODE_HT40: return "HT40";
        default:                 return "other";
    }
}

static void print_status(void) {
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now - last_ms < STATUS_MS) return;
    uint32_t span = now - last_ms;
    last_ms = now;

    uint32_t n = s_win_frames ? s_win_frames : 1;
    uint32_t wn = s_win_written ? s_win_written : 1;
    uint32_t gn = s_win_grabs ? s_win_grabs : 1;
    bool up = (WiFi.status() == WL_CONNECTED);
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);  // association silently reinstates it; see provision.cpp
    uint32_t mean_bytes = (uint32_t)(s_win_bytes / n);
    nodeagent_logf("[cam] cam=%s frames=%lu %.1ffps %luB/f rtsp=%s http=%s buf=%s "
                   "| idle=%lu written=%lu short=%lu "
                   "| cap=%lu rtsp=%lu http=%lu wr=%lu us/frame "
                   "| wifi=%s ps=%s phy=%s ip=%s rssi=%ddBm | heap=%u\n",
                   cam_ok ? "OK" : "FAIL",
                   (unsigned long)frame_count,
                   span ? (s_win_frames * 1000.0f / span) : 0.0f,
                   (unsigned long)mean_bytes,
                   s_rtsp_sessions ? "client" : "idle",
                   http_client.connected() ? "client" : "idle",
                   s_http_buf[0] ? "ok" : "NOALLOC",
                   (unsigned long)s_win_idle,
                   (unsigned long)s_http_written,
                   (unsigned long)s_http_short,
                   (unsigned long)(s_us_capture / gn),
                   (unsigned long)(s_us_rtsp / n),
                   (unsigned long)(s_us_http / n),
                   (unsigned long)(s_us_write / wn),
                   up ? "up" : "down",
                   ps == WIFI_PS_NONE ? "off" : "ON(bad)",
                   phy_name(),
                   WiFi.localIP().toString().c_str(),
                   (int)WiFi.RSSI(),
                   (unsigned)ESP.getFreeHeap());

    s_us_capture = s_us_rtsp = s_us_http = s_win_frames = s_win_grabs = 0;
    s_us_write = s_win_written = s_win_idle = 0;
    s_win_bytes = 0;
}

// 180 degree rotation; re-applied after every framesize change (set_framesize
// can reset the flip/mirror registers on some sensors).
static void apply_rotation(sensor_t *s) {
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
}

// Read the HTTP request line ("GET /path HTTP/1.1"), bounded so a silent
// client can't stall the loop.
static String read_request_line(WiFiClient &client) {
    String line;
    uint32_t t = millis();
    while (millis() - t < 500) {
        while (client.available()) {
            char ch = client.read();
            if (ch == '\n') return line;
            if (ch != '\r') line += ch;
            if (line.length() > 200) return line;
        }
        delay(1);
    }
    return line;
}

// Pin map and defaults for one camera_config_t, so the still path can re-init
// the driver at a different resolution instead of trying to change it live.
static camera_config_t make_cam_config(framesize_t fs, int quality, int fb_count,
                                       camera_grab_mode_t grab) {
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_d0 = Y2_GPIO_NUM;
    cfg.pin_d1 = Y3_GPIO_NUM;
    cfg.pin_d2 = Y4_GPIO_NUM;
    cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM;
    cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM;
    cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM;
    cfg.pin_pclk = PCLK_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM;
    cfg.pin_href = HREF_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn = PWDN_GPIO_NUM;
    cfg.pin_reset = RESET_GPIO_NUM;
    // 8MHz, not the usual 20MHz. The camera's DMA into PSRAM is what was
    // strangling this board's WiFi, and the sensor clock is the knob that sets
    // how much of it there is. Pinged from the PC while the S3 streamed, and
    // measured at the S3's receiver, sweeping only this value:
    //
    //   XCLK   ping avg/max   S3 delivered    per-frame capture
    //   20MHz   387 / 771ms   1.4-2.6 fps     18ms   (sensor ~55fps)
    //   10MHz   122 / 983ms   2.7-6.2 fps     38ms   (sensor ~26fps)
    //    8MHz     5 /  17ms   9.7-10.0 fps    48ms   (sensor ~20fps)
    //    5MHz     6 /  20ms   4.4-6.4 fps     78ms   (sensor ~13fps)
    //
    // For reference the S3 on the same AP pings at 2ms, and this board with the
    // camera never initialised pings at 4ms - so at 20MHz the sensor alone was
    // adding ~380ms of latency to every packet the board sent.
    //
    // 8MHz sits below the cliff between 8 and 10MHz and still leaves the sensor
    // producing about twice what the link carries. 5MHz is past the useful point:
    // latency is no better and the sensor itself becomes the cap. Image quality
    // is indistinguishable from 20MHz at this frame size.
    cfg.xclk_freq_hz = 8000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size = fs;
    cfg.jpeg_quality = quality;
    cfg.fb_count = fb_count;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = grab;
    return cfg;
}

// Bring the driver up in streaming configuration. Also the recovery path when a
// still fails: whatever happens, the live feed must come back.
static bool cam_start_stream_mode(void) {
    camera_config_t cfg = make_cam_config(STREAM_FS, STREAM_Q, 2, CAMERA_GRAB_LATEST);
    if (esp_camera_init(&cfg) != ESP_OK) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) apply_rotation(s);
    return true;
}

// How long all three paths get to reach a safe stopping point. The capture loop parks within one
// sensor period (48ms at 8MHz), rtsp_task within its 2ms tick, and http_writer_task only after
// whatever socket write it is inside returns - measured at up to 1.05s for a viewer that could
// not keep up. Five seconds is that worst case with room to spare; past it a task is wedged, and
// a wedged task is a reboot's problem rather than a download's.
static const uint32_t PARK_MS = 5000;

bool camstream_stand_down(void) {
    s_stand_down = true;

    uint32_t t = millis();
    while (!(s_loop_parked && s_rtsp_parked && s_http_parked)) {
        if (millis() - t > PARK_MS) {
            nodeagent_logf("[cam] stand-down timed out (loop=%d rtsp=%d http=%d); nothing stopped\n",
                           (int)s_loop_parked, (int)s_rtsp_parked, (int)s_http_parked);
            s_stand_down = false;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // ONLY now, and this ordering is the whole reason the handshake exists: loop() is the thread
    // that calls esp_camera_fb_get(), and deinit()ing the driver while it is blocked inside one
    // frees the buffer it is about to be handed. The ack above is the proof it is not.
    //
    // Deinit rather than simply stopping the grab. loop()'s own comment records that leaving the
    // driver's buffers unfetched makes it log "cam_hal: EV-VSYNC-OVF" and stall its pipeline,
    // which would be the worst of both: the sensor still running and still DMAing into PSRAM,
    // starving the radio this download needs, AND a wedged pipeline to recover from afterwards.
    // Stopping the driver stops the DMA at its source.
    if (cam_ok) {
        esp_camera_deinit();
        cam_ok = false;
    }
    nodeagent_logf("[cam] streaming stood down: camera off, RTSP and HTTP parked\n");
    return true;
}

void camstream_resume(void) {
    // Camera first, flag second. Clearing the flag first would let loop() start grabbing against
    // a driver that is not up yet.
    if (!cam_ok) {
        cam_ok = cam_start_stream_mode();
        if (!cam_ok) {
            // Honest degradation rather than a reboot: RTSP and HTTP still answer, they just have
            // no frames, and the status line says cam=FAIL. A board that reboots itself here
            // would throw away the failure reason sitting on the panel's screen, which is the
            // only thing anybody gets out of an update that did not work.
            nodeagent_logf("[cam] camera did not come back after the download\n");
        }
    }
    s_stand_down = false;
    nodeagent_logf("[cam] streaming restored\n");
}

void camstream_summary(char *out, size_t cap) {
    snprintf(out, cap, "stream cam=%s standby=%s frames=%lu rtsp=%s http=%s psram=%u",
             cam_ok ? "OK" : "FAIL",
             s_stand_down ? "yes" : "no",
             (unsigned long)frame_count,
             s_rtsp_sessions ? "client" : "idle",
             http_client.connected() ? "client" : "idle",
             (unsigned)ESP.getFreePsram());
}

// Send one frame as a standalone JPEG, at the streaming resolution.
//
// It used to promise a full-res UXGA still, and this board cannot deliver one.
// Two approaches were measured and both failed. Calling set_framesize(UXGA) on
// the live driver returned the two frames still queued (34ms, 80ms) and then
// NULL from every subsequent grab after a 4000ms timeout each - the sensor was
// reprogrammed but the DMA was still set up for QVGA, so no frame ever
// completed. Tearing the driver down and re-initialising it at UXGA with
// fb_count=1 was no better: esp_camera_init() succeeded and the very first grab
// still came back "NULL 0x0 len=0".
//
// Both cost the thing that actually matters. A single /rgb/image request froze
// the live feed for 5 to 20 seconds while those timeouts ran, which is a far
// worse trade than a smaller photo - the live path is the product here. So take
// the frame the sensor is already producing: one ordinary grab, no
// reconfiguration, nothing for the stream to recover from.
static void serve_photo(WiFiClient &client) {
    digitalWrite(FLASH_LED_PIN, HIGH);  // worth it in a dark greenhouse
    delay(120);                         // let the scene light up and AEC react
    camera_fb_t *d = esp_camera_fb_get();  // pre-flash frame; drop it
    if (d) esp_camera_fb_return(d);
    camera_fb_t *fb = esp_camera_fb_get();
    digitalWrite(FLASH_LED_PIN, LOW);

    if (fb != nullptr) {
        char hdr[160];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "Content-Length: %u\r\n"
                         "Content-Disposition: inline; filename=photo.jpg\r\n"
                         "Connection: close\r\n\r\n",
                         (unsigned)fb->len);
        client.write((const uint8_t *)hdr, n);
        client.write(fb->buf, fb->len);
        esp_camera_fb_return(fb);
    } else {
        client.print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);  // let the host monitor reattach after reset, or the banner is lost
    nodeagent_logf("\n=== SmartFarm ESP32-CAM: RTSP :8554/mjpeg/1 + HTTP /rgb/image /rgb/stream ===\n");

    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);  // flash off until /photo fires it

    // One configuration for everything, so nothing ever reconfigures the driver
    // while the stream is live. Buffers are QVGA-sized.
    cam_ok = cam_start_stream_mode();
    if (!cam_ok) {
        nodeagent_logf("[cam] esp_camera_init failed\n");
    }

    // Bring up WiFi in the background: on a first boot the ESP-NOW handshake
    // can hop all 13 channels before the S3 answers, and loop() has to keep
    // running meanwhile. Creds come from NVS or, on first boot, from the S3
    // (provision_task runs on core 0 and calls WiFi.begin once it has them).
    // RTSP/HTTP come online on their own once the link is up (see loop()).
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // steadier timing; avoids bursty modem-sleep wakeups
    // Keep the 802.11b rate set ENABLED, and say so rather than inheriting it:
    // the flag survives a reboot, and a board that comes up with 11b disabled
    // both streams worse and can fail to associate at all (auth/assoc frames go
    // out at the AP's basic rate set). Disabling it was tried on the theory that
    // 1-2Mbps fallbacks were eating airtime; measured over 65s windows it more
    // than halved the stream, 3.1fps against 6.9fps written. The earlier reading
    // that seemed to favour disabling it shared a flash with reverting a TX-power
    // reduction, and that revert was the actual win.
    esp_wifi_config_11b_rate(WIFI_IF_STA, false);
    provision_start();

    streamer = new SharedStreamer(CAM_W, CAM_H);

    // PSRAM: the JPEG copies the writer task sends while the loop moves on. If
    // either fails the HTTP viewer is simply never served; capture and RTSP are
    // untouched, which is the right way to lose this feature.
    bool http_ok = true;
    for (int i = 0; i < HTTP_SLOTS; i++) {
        s_http_buf[i] = (uint8_t *)heap_caps_malloc(HTTP_HDR_MAX + MAX_FRAME + 2,
                                                    MALLOC_CAP_SPIRAM);
        if (s_http_buf[i] == nullptr) http_ok = false;
    }
    if (http_ok) {
        xTaskCreatePinnedToCore(http_writer_task, "http_tx", 4096, NULL, 2, NULL, 0);
    } else {
        nodeagent_logf("[cam] PSRAM alloc failed: /rgb/stream disabled\n");
    }

    // The frame mailbox is optional; the task is not. rtsp_task answers the
    // protocol and accepts viewers, so it runs even without the buffer - a board
    // out of PSRAM should still say hello and then serve nothing, rather than
    // leave port 8554 open and silent.
    s_rtsp_buf = (uint8_t *)heap_caps_malloc(MAX_FRAME, MALLOC_CAP_SPIRAM);
    if (s_rtsp_buf == nullptr) {
        nodeagent_logf("[cam] PSRAM alloc failed: RTSP video disabled\n");
    }
    // 8192: this used to run on the Arduino loop task, which has that much.
    xTaskCreatePinnedToCore(rtsp_task, "rtsp", 8192, NULL, 2, NULL, 0);

    // Last, because it is the only thing here that can tear the rest of it down: nodeagent's
    // worker calls camstream_stand_down(), which waits on acknowledgements from the two tasks
    // above and from loop(). Starting it before they exist would mean a NODE_UPDATE arriving in
    // the first few milliseconds after boot waits out PARK_MS and then refuses itself.
    nodeagent_start();
}

void loop() {
    // Ahead of the cam_ok bail-out below: a board whose sensor never came up is
    // exactly the one whose status line you need.
    print_status();

    // Parked for a firmware download - ahead of the cam_ok bail-out and everything else, because
    // the whole point of the stand-down is that this loop stops touching the sensor and the
    // sockets, and the ack below is what nodeagent waits on before it deinits the driver.
    if (s_stand_down) {
        if (!s_loop_parked) {
            // Same order the /rgb/stream accept path uses, and for the same reason: let an
            // in-flight write finish before the socket goes away under it. Closing it is also
            // what unblocks http_writer_task so it can park.
            uint32_t t_wait = millis();
            while (s_http_busy && millis() - t_wait < 500) delay(1);
            for (int i = 0; i < HTTP_SLOTS; i++) s_http_len[i] = 0;
            if (http_client.connected()) {
                http_client.stop();
            }
            s_http_prod = 0;
            s_loop_parked = true;
        }
        delay(50);
        return;
    }
    s_loop_parked = false;

    if (!cam_ok) {
        delay(500);
        return;
    }

    // Start RTSP + HTTP + mDNS the first time WiFi comes up.
    if (!rtsp_up && WiFi.status() == WL_CONNECTED) {
        rtspServer.begin();       // rtsp://<ip>:8554/mjpeg/1
        httpServer.begin();
        MDNS.begin("esp32cam");
        MDNS.addService("rtsp", "tcp", 8554);
        MDNS.addService("http", "tcp", 80);
        rtsp_up = true;
    }

    // No RTSP work here on purpose - rtsp_task owns the streamer end to end.

    // Grab first, decide after. The grab blocks for a whole sensor period (48ms
    // at 8MHz), and asking "is a consumer ready?" before it means answering with
    // state that is a frame out of date: the mailbox we filled last pass is still
    // full at that instant, but the writer - which only needs 24ms - has emptied
    // it long before the grab returns. That threw away every other frame and
    // pinned delivery at exactly half the sensor rate, 10fps against 20.7.
    //
    // The grab itself is never gated. Leaving the driver's buffers unfetched
    // makes it log "cam_hal: EV-VSYNC-OVF" and stall its pipeline, and the board
    // then limped along at 0.4-2.4fps. It costs nothing to keep: a blocking wait
    // on the sensor, not CPU.
    uint32_t t_mark = micros();
    camera_fb_t *fb = esp_camera_fb_get();
    s_us_capture += micros() - t_mark;
    s_win_grabs++;
    if (fb != NULL) {
        uint32_t now_ms = millis();
        bool rtsp_due = s_rtsp_sessions && s_rtsp_buf != nullptr && s_rtsp_len == 0 &&
                        (now_ms - s_rtsp_last_ms >= RTSP_INTERVAL_MS);
        bool http_due = http_client.connected() && s_http_buf[s_http_prod] != nullptr &&
                        s_http_len[s_http_prod] == 0;
        if (!rtsp_due && !http_due) {
            // Nobody is waiting: hand the buffer straight back. This is the whole
            // saving - no memcpy, no packetize - while the sensor keeps its
            // cadence. Never return early here; the accept paths are below.
            s_win_idle++;
            esp_camera_fb_return(fb);
            delay(1);
        } else {
            // CAMERA_GRAB_LATEST hands back the newest COMPLETED frame, so polling
            // faster than the sensor produces returns the same image again. Sending
            // it twice costs real airtime for zero new information — and this board
            // shares the channel with the sensor node's ESP-NOW link, which is
            // exactly what that airtime starves. Key on the driver's capture
            // timestamp rather than the buffer address, which the driver reuses.
            static int64_t last_us = -1;
            int64_t frame_us = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
            bool fresh = (frame_us != last_us);
            last_us = frame_us;

            if (fresh && fb->len > 0 && fb->len <= MAX_FRAME) {
                frame_count++;
                s_win_frames++;
                s_win_bytes += fb->len;
                if (rtsp_due) {
                    // Copy and move on. The mailbox is known empty - rtsp_due
                    // tested it - so this never overwrites a frame in flight.
                    s_rtsp_last_ms = now_ms;
                    memcpy(s_rtsp_buf, fb->buf, fb->len);
                    s_rtsp_len = fb->len;
                }
                // Browser MJPEG (/rgb/stream): compose the whole multipart chunk
                // here and hand it over, so the writer task issues a single write.
                // The slot is known free - that is what http_due tested.
                if (http_due) {
                    t_mark = micros();
                    uint8_t *slot = s_http_buf[s_http_prod];
                    int hn = snprintf((char *)slot, HTTP_HDR_MAX,
                                      "--frame\r\nContent-Type: image/jpeg\r\n"
                                      "Content-Length: %u\r\n\r\n",
                                      (unsigned)fb->len);
                    memcpy(slot + hn, fb->buf, fb->len);
                    memcpy(slot + hn + fb->len, "\r\n", 2);
                    s_http_len[s_http_prod] = (size_t)hn + fb->len + 2;
                    s_http_prod = (s_http_prod + 1) % HTTP_SLOTS;
                    s_us_http += micros() - t_mark;
                }
            }
            esp_camera_fb_return(fb);
        }
    }

    // Accept a new HTTP connection and route by path:
    //   GET /rgb/image  -> one frame as a standalone JPEG, then close
    //   GET /rgb/stream -> live MJPEG (one viewer)
    //   anything else   -> a tiny index page linking to both
    if (rtsp_up) {
        WiFiClient c = httpServer.accept();
        if (c) {
            String req = read_request_line(c);
            if (req.indexOf("/rgb/image") >= 0) {
                serve_photo(c);
                c.stop();
            } else if (req.indexOf("/rgb/stream") >= 0) {
                // Hand over to the newest viewer rather than refusing. A viewer
                // that vanishes without a clean FIN — the S3 rebooting on a
                // flash, most obviously — leaves this socket reporting
                // connected() indefinitely, and answering 503 then locks the
                // stream out until the CAM is power-cycled. There is only ever
                // one intended viewer, so the live request always wins.
                // Drop everything queued for the old socket and start the ring
                // over, so the new viewer's stream begins on a boundary header.
                for (int i = 0; i < HTTP_SLOTS; i++) s_http_len[i] = 0;
                uint32_t t_wait = millis();
                while (s_http_busy && millis() - t_wait < 500) delay(1);
                s_http_prod = 0;
                s_http_cons = 0;
                if (http_client.connected()) {
                    http_client.stop();
                }
                http_client = c;
                http_client.setNoDelay(true);  // MJPEG parts are latency-sensitive
                http_client.print("HTTP/1.1 200 OK\r\n"
                                  "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                  "Cache-Control: no-cache\r\n"
                                  "Connection: close\r\n\r\n");
            } else {
                String ip = WiFi.localIP().toString();
                String body = "<h3>SmartFarm Camera</h3>"
                              "<p><a href=\"/rgb/stream\">/rgb/stream</a> - live | "
                              "<a href=\"/rgb/image\">/rgb/image</a> - single photo</p>"
                              "<p>RTSP: rtsp://" + ip + ":8554/mjpeg/1</p>";
                c.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" + body);
                c.stop();
            }
        }
    }

    delay(1);
}
