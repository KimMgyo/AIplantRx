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
// that survives the panic, while a ring needs none of those and loses nothing that matters. Its
// size is the only tuning: 16KB holds several minutes of the periodic status lines and the whole
// boot sequence, so a client connecting after the fact still sees how the board came up.
//
// WHY THE UART WRITE STAYS. A serial console is still the only thing that works when WiFi does
// not, and this file exists precisely because one diagnostic path is not enough.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdarg.h>
#include "hlog.h"

// 16KB in PSRAM. Internal RAM is the scarce pool on this board - the RGB framebuffer and LVGL
// already live there - and a log ring has no latency requirement that would justify spending it.
static const size_t RING_CAP = 16 * 1024;

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

        // 50ms. Fast enough that a line appears as it is printed and slow enough to stay out of
        // the way: this board has already been reset once by a task that polled at 1ms and
        // starved the idle task the watchdog watches (see camnet.cpp).
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

    esp_log_set_vprintf(log_vprintf);

    // Priority 1 and core 0: below everything that matters, beside the other network tasks.
    // A log reader must never be the reason a frame is dropped or an update stalls.
    xTaskCreatePinnedToCore(hlog_task, "hlog", 4096, nullptr, 1, nullptr, 0);
}

uint32_t hlog_overruns(void) { return s_dropped_clients; }
