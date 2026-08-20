// The console, kept in memory and served over the network.
//
// WHY THIS EXISTS. The panel's display sits on GPIO 43/44, which are UART0's pins, and a switch
// on the board hands those either to the USB bridge or to the header. Running the display means
// running with no console. Everything else added for this - the reset reason, the crash counter,
// the coredump summary - answers questions about a run that has already ended. This answers the
// other half: what is the board saying RIGHT NOW, while it is misbehaving but not crashing.
// "The sensor node is offline" and "the sensor node is fine and the panel is not listening" look
// identical from the server, and the line that distinguishes them is already being printed to a
// UART nobody can read.
//
// So every diagnostic line goes to two places: the UART, exactly as before, and a ring in PSRAM.
// A TCP listener on port 23 hands a connecting client the ring first and then every new line as
// it happens, so `nc <panel> 23` is the console the hardware took away.
//
// WHY A RING AND NOT A FILE. The interesting output is the most recent output, and a board that
// is misbehaving may be about to reboot: a file needs flushing, an unmount, and a filesystem
// that survives the panic, while a ring needs none of those and loses nothing that matters.
//
// WHY PORT 23 IS NOT ENOUGH, AND WHAT THE SECOND READER IS. `nc <panel> 23` only exists on the
// LAN, and the greenhouse is an hour away from the laptop that has the toolchain. So the same
// ring is also pushed to the server, which serves it on the operator page beside the cards - the
// one place reachable from a phone. The push is a second reader of the same ring with its own
// cursor, deliberately: the two must never be able to disagree about what the board said, and
// sharing the ring rather than the socket is what guarantees that.
//
// WHY THE UART WRITE STAYS. A serial console is still the only thing that works when WiFi does
// not, and this file exists precisely because one diagnostic path is not enough.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdarg.h>
#include "hlog.h"
#include "net.h"
#include "plantrx.h"
#include "sitecfg.h"

// 256KB in PSRAM. Internal RAM is the scarce pool on this board - the RGB framebuffer and LVGL
// already live there - and a log ring has no latency requirement that would justify spending it.
//
// The size is a retention target, not a guess. Measured: this panel prints about 15KB a minute
// with the ordinary tags on, so 16KB held barely one minute and a fault that happened while
// nobody was attached had already scrolled out of the ring before anyone could ask about it.
// 256KB holds about 17 minutes, which covers the gap between a board misbehaving and somebody
// opening the page. PSRAM has 1.8MB free with the camera streaming, so the 240KB is affordable.
static const size_t RING_CAP = 256 * 1024;

// One line's worth. Longer lines are truncated rather than heap-allocated: the longest thing
// this project prints is plantrx's status line at about 320 characters, and a diagnostic that
// silently allocates during a crash-adjacent moment is worse than one that clips.
static const size_t LOG_LINE_MAX = 512;

static char *s_ring = nullptr;
static size_t s_head = 0;            // total bytes ever written, not an index
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped_clients = 0;

// Appends to the ring. Called from every task that logs, so the cursor update has to be atomic
// with the copy - a reader computing "how far behind am I" against a half-updated head would
// either repeat or skip a line. The critical section covers a memcpy of at most LOG_LINE_MAX bytes.
static void ring_put(const char *p, size_t n) {
    if (s_ring == nullptr || n == 0) return;
    if (n > RING_CAP) { p += n - RING_CAP; n = RING_CAP; }

    taskENTER_CRITICAL(&s_mux);
    size_t at = s_head % RING_CAP;
    size_t first = RING_CAP - at;
    if (first > n) first = n;
    memcpy(s_ring + at, p, first);
    if (n > first) memcpy(s_ring, p + first, n - first);
    s_head += n;
    taskEXIT_CRITICAL(&s_mux);
}

// Copies out the window starting at absolute position `from`. Returns bytes copied and advances
// `from`. A client that fell more than a ring behind is snapped forward to the oldest byte still
// held, because handing it stale bytes that have since been overwritten would be a lie about
// what the board said.
static size_t ring_take(size_t *from, char *out, size_t max) {
    if (s_ring == nullptr) return 0;

    taskENTER_CRITICAL(&s_mux);
    size_t head = s_head;
    size_t oldest = (head > RING_CAP) ? head - RING_CAP : 0;
    bool lost = (*from < oldest);
    if (lost) *from = oldest;

    size_t n = head - *from;
    if (n > max) n = max;
    size_t at = *from % RING_CAP;
    size_t first = RING_CAP - at;
    if (first > n) first = n;
    memcpy(out, s_ring + at, first);
    if (n > first) memcpy(out + first, s_ring, n - first);
    *from += n;
    taskEXIT_CRITICAL(&s_mux);

    if (lost) s_dropped_clients++;
    return n;
}

// The tee. Formats once, then writes the same bytes to the UART and the ring, so the two can
// never disagree about what the board said - which is the only reason to trust the remote copy.
void hlogf(const char *fmt, ...) {
    char line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;

    Serial.write((const uint8_t *)line, len);
    ring_put(line, len);
}

// The framework's own logging, which is where the panel driver, the WiFi stack and the touch
// controller report. CORE_DEBUG_LEVEL=1 keeps this to errors and the board-bringup lines, and
// those are exactly the ones worth having when a board comes up wrong at the far end of a
// greenhouse - the GT911 "Unable to initialize the I2C address" that cost an afternoon arrived
// on this path, not through Serial.
static int log_vprintf(const char *fmt, va_list ap) {
    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) return n;
    size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;

    // Forwarded by hand: installing a vprintf REPLACES the default one, so without this the
    // console loses every framework message the moment the ring gains it.
    Serial.write((const uint8_t *)line, len);
    ring_put(line, len);
    return n;
}

// ---- the push to the server ------------------------------------------------------------
//
// WHY THE BODY IS RAW BYTES AND NOT JSON. What this ships is a slice of a byte ring, not a list
// of lines: a slice can begin and end mid-line, and that is a property worth keeping rather than
// hiding. Escaping 4KB of arbitrary console text into JSON would cost a second buffer and an
// encode pass on a board whose scarce pool is internal RAM, to describe bytes that need no
// description. So the chunk goes up as text/plain and the server stores it as it arrived; the
// page concatenates chunks and never tries to reassemble lines it was not given.
//
// THIS IS THE FOURTH PLACE IN THIS FIRMWARE THAT HAND-WRITES AN HTTP HEAD - plantrx.cpp,
// plantid.cpp and nodelog.cpp are the others, and folding them is a real change that this is not.
// They differ in path, method and content type, and the last attempt to share a socket layer
// across them (shared/srvconn.h, for TLS) was reverted whole. shared/srvurl.h is the precedent
// for how it should be done when someone does it: share the part that is genuinely identical -
// there, parsing one URL four times - and leave the part that is not.

static const char *CONLOG_PATH = "/v1/conlog";
static const uint32_t PUSH_CONNECT_MS = 4000;
static const uint32_t PUSH_REPLY_MS = 4000;

// A gap between pushes, which is batching and not throttling, for the reason nodelog.cpp gives
// about its own: at 15KB a minute one request per line would spend more time in TCP handshakes
// than the log is worth. Three seconds is chosen against the page's 2s tail - the operator's view
// is then at most one push behind, which reads as live without pretending to be a stream.
static const uint32_t PUSH_GAP_MS = 3000;
// And after a failure, for nodelog.cpp's reason: a connect to a dead host costs PUSH_CONNECT_MS,
// and retrying at the success cadence on a panel whose server is down makes the log the reason
// the log task is busy.
static const uint32_t PUSH_FAIL_GAP_MS = 20000;

// Staged in PSRAM and not on the stack. This task has 8KB and a WiFiClient connect goes through
// lwIP inside it; a 4KB frame on top of that is how a log task becomes the thing that panics.
static const size_t PUSH_CHUNK = 4 * 1024;
static char *s_push_buf = nullptr;
static size_t s_push_cursor = 0;
static uint32_t s_push_next_ms = 0;
static uint32_t s_push_fails = 0;

// The status line, then the rest of the reply read and thrown away - the same shape and the same
// reason as nodelog.cpp's: nothing the server says about an accepted chunk is worth parsing, but
// the socket is still drained before stop(), because a close with bytes outstanding arrives as an
// RST and uvicorn logs that as a broken pipe for a request it handled perfectly.
static int push_status(WiFiClient &c, uint32_t start) {
    char line[128];
    size_t i = 0;
    int status = -1;
    bool have_line = false;

    for (;;) {
        int a = c.available();
        if (a > 0) {
            int ch = c.read();
            if (ch < 0) continue;
            if (!have_line) {
                if (ch == '\n') {
                    line[i] = '\0';
                    have_line = true;
                    // "HTTP/1.1 200 OK" - the code is the token after the first space.
                    const char *sp = strchr(line, ' ');
                    status = sp ? atoi(sp + 1) : 0;
                } else if (ch != '\r' && i + 1 < sizeof(line)) {
                    line[i++] = (char)ch;
                }
            }
            continue;
        }
        if (!c.connected()) break;
        if (millis() - start > PUSH_REPLY_MS) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return status;
}

// Takes whatever the ring has beyond the push cursor and POSTs it. The cursor advances only on a
// 2xx, so a server that is down loses nothing that the ring still holds - and when the ring does
// overwrite unsent bytes, ring_take() snaps the cursor forward and counts it, which is why the
// dropped total travels in a header rather than being silently absent.
static void push_once(void) {
    if (s_push_buf == nullptr) return;
    if (!plantrx_srv_host()[0]) return;               // no server configured, nothing to push to

    size_t at = s_push_cursor;
    size_t n = 0;
    while (n < PUSH_CHUNK) {
        size_t got = ring_take(&at, s_push_buf + n, PUSH_CHUNK - n);
        if (got == 0) break;
        n += got;
    }
    if (n == 0) return;

    WiFiClient c;
    uint32_t start = millis();
    if (!c.connect(plantrx_srv_host(), plantrx_srv_port(), PUSH_CONNECT_MS)) {
        s_push_fails++;
        s_push_next_ms = millis() + PUSH_FAIL_GAP_MS;
        return;
    }

    char device[20];
    net_mac(device, sizeof(device));
    const char *tok = sitecfg_token();
    c.print("POST "); c.print(plantrx_srv_prefix()); c.print(CONLOG_PATH);
    c.print(" HTTP/1.0\r\n");
    c.print("Host: "); c.print(plantrx_srv_host()); c.print(":"); c.print(plantrx_srv_port());
    c.print("\r\n");
    c.print("User-Agent: SmartFarm-ESP32/1.0\r\n");
    c.print("Content-Type: text/plain\r\n");
    c.print("Content-Length: "); c.print(n); c.print("\r\n");
    c.print("X-Device: "); c.print(device); c.print("\r\n");
    c.print("X-Dropped: "); c.print(s_dropped_clients); c.print("\r\n");
    if (tok[0]) { c.print("Authorization: Bearer "); c.print(tok); c.print("\r\n"); }
    c.print("Connection: close\r\n\r\n");
    // No flush(), for the reason nodelog.cpp and fwpull.cpp both spell out: NetworkClient
    // implements it as clear(), which empties the RX buffer this request is about to be judged by.

    size_t sent = 0;
    while (sent < n) {
        size_t take = n - sent;
        if (take > 1460) take = 1460;                 // one MSS, as the sibling POSTs send
        size_t w = c.write((const uint8_t *)s_push_buf + sent, take);
        if (w == 0) break;
        sent += w;
    }

    int status = (sent == n) ? push_status(c, start) : -1;
    c.stop();

    if (status >= 200 && status < 300) {
        s_push_cursor = at;                           // only now: an unacknowledged chunk is resent
        s_push_fails = 0;
        s_push_next_ms = millis() + PUSH_GAP_MS;
        // Proof this panel's LAN uplink is alive, which net.h asks any successful exchange with
        // any peer to report. Pointedly not the failure half - net_note_uplink_fail() is the
        // server poll's vote alone, and a log push losing a race is not evidence the radio needs
        // cycling. Same division nodelog.cpp draws, for the same reason.
        net_note_uplink();
        return;
    }

    // Said once per streak and not once per failure: this line goes into the ring it is about, so
    // a server that is down for an hour would otherwise fill the log with the news that the log
    // cannot be sent, and push the actual fault out the far end.
    if (s_push_fails == 0) {
        hlogf("[hlog] push failed, status=%d after %lums; retrying every %lus\n", status,
              (unsigned long)(millis() - start), (unsigned long)(PUSH_FAIL_GAP_MS / 1000));
    }
    s_push_fails++;
    s_push_next_ms = millis() + PUSH_FAIL_GAP_MS;
}

// ---- the listener ----------------------------------------------------------------------
//
// One client at a time, and the newest connection wins. Two people reading the same log is not
// a case worth the second socket, and a stale half-open connection from a laptop that went to
// sleep must not be able to lock out the person standing in the greenhouse now.

static WiFiServer s_server(23);
static bool s_listening = false;

static void hlog_task(void *arg) {
    WiFiClient client;
    size_t cursor = 0;
    char out[1024];

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            if (s_listening) {
                s_server.end();
                s_listening = false;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!s_listening) {
            s_server.begin();
            s_server.setNoDelay(true);
            s_listening = true;
            hlogf("[hlog] console on tcp/23 at %s\n", WiFi.localIP().toString().c_str());
        }

        WiFiClient fresh = s_server.accept();
        if (fresh) {
            if (client) client.stop();
            client = fresh;
            client.setNoDelay(true);
            // Hands over the whole ring first, so the first thing a client sees is how the
            // board got into its current state rather than only what it does next.
            cursor = (s_head > RING_CAP) ? s_head - RING_CAP : 0;
            client.printf("--- smartfarm panel console, %u bytes of history ---\r\n",
                          (unsigned)(s_head - cursor));
        }

        if (client && !client.connected()) client.stop();

        if (client) {
            size_t n = ring_take(&cursor, out, sizeof(out));
            if (n > 0 && client.write((const uint8_t *)out, n) != n) client.stop();
        }

        // The second reader, on its own cursor and its own clock. Placed here rather than in
        // loop() because loop() runs the UI: a POST that stalls for PUSH_CONNECT_MS in there is
        // four seconds of frozen touch screen, and this task already exists and already reads
        // this ring for the socket above.
        if ((int32_t)(millis() - s_push_next_ms) >= 0) push_once();

        // 20ms with a client attached. Fast enough that a line appears as it is printed and slow
        // enough to stay out of the way: this board has already been reset once by a task that
        // polled at 1ms and starved the idle task the watchdog watches (see camnet.cpp). 200ms
        // idle is still well inside PUSH_GAP_MS, so the push cadence is set by its own timer and
        // not by which branch this lands in.
        vTaskDelay(pdMS_TO_TICKS(client ? 20 : 200));
    }
}

void hlog_init(void) {
    s_ring = (char *)heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM);
    if (s_ring == nullptr) {
        // No ring, no remote console - but hlogf() still reaches the UART, so the board is no
        // worse off than before this file existed. Said out loud because a silent downgrade to
        // "the console is empty" would look like the board having nothing to say.
        Serial.println("[hlog] PSRAM alloc failed; remote console off, UART only");
        return;
    }

    // The push stages here rather than on the task stack, and a failure to get it costs the
    // server copy only: the ring, the UART and port 23 all still work, so the board is degraded
    // to what it was before the push existed rather than broken.
    s_push_buf = (char *)heap_caps_malloc(PUSH_CHUNK, MALLOC_CAP_SPIRAM);
    if (s_push_buf == nullptr) {
        Serial.println("[hlog] PSRAM alloc failed for the push buffer; tcp/23 and UART only");
    }

    esp_log_set_vprintf(log_vprintf);

    // Priority 1 and core 0: below everything that matters, beside the other network tasks.
    // A log reader must never be the reason a frame is dropped or an update stalls.
    //
    // 8KB and not the 4KB this shipped with. The push opens a WiFiClient from inside this task
    // and a connect goes through lwIP on the caller's stack; 4KB was sized for a task that only
    // ever did a memcpy and a socket write on an already-open connection.
    xTaskCreatePinnedToCore(hlog_task, "hlog", 8192, nullptr, 1, nullptr, 0);
}

uint32_t hlog_overruns(void) { return s_dropped_clients; }
