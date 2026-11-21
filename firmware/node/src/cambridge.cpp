#include "cambridge.h"
#include <Preferences.h>
#include <aipnode.h>
#include <lwip/sockets.h>
#include <errno.h>

#define PIN_CAM_TX 18    // -> 1k -> CAM U0R
#define PIN_CAM_RX 19    // <- 1k <- CAM U0T
#define PIN_CAM_IO0 25   // HIGH: PC817C pulls CAM IO0 low
#define PIN_CAM_EN 33    // HIGH: PC817C pulls CAM EN low (held in reset)

static const uint16_t BRIDGE_PORT = 4000;
static const uint32_t BRIDGE_IDLE_MS = 30000;   // esptool went away without closing
static HardwareSerial &U = Serial1;

static SemaphoreHandle_t s_mu;
static char s_log[4096];
static size_t s_head;          // next write position
static bool s_full;            // the ring has wrapped
static uint32_t s_rx_bytes, s_rx_ms;   // the CAM's console output, all time / last byte (millis)

static bool s_swap;            // RX/TX the other way round (a wiring check; kept in NVS "camlink")
static volatile bool s_bridge;
static uint32_t s_bridges, s_last_up, s_last_down, s_last_ms;
static char s_last_peer[16];

static void log_put(const char *p, size_t n) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        s_log[s_head++] = p[i];
        if (s_head == sizeof s_log) { s_head = 0; s_full = true; }
    }
    xSemaphoreGive(s_mu);
}

static void log_note(const char *fmt, ...) {
    char b[160];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (n > 0) log_put(b, min((size_t)n, sizeof b - 1));
    aipnode_logf("%s", b);
}

// ---------------------------------------------------------------- reset lines

static void restart_into(bool download) {
    digitalWrite(PIN_CAM_IO0, download ? HIGH : LOW);
    digitalWrite(PIN_CAM_EN, HIGH);   // EN low: the CAM stops
    delay(100);
    digitalWrite(PIN_CAM_EN, LOW);    // EN back up: the ROM reads IO0 now
    if (download) {
        delay(150);
        digitalWrite(PIN_CAM_IO0, LOW);   // IO0 is the camera's XCLK later: never hold it longer than needed
    }
}

void cam_restart() {
    if (s_bridge) return;
    log_note("\n[devkit] CAM restart\n");
    restart_into(false);
}

// ---------------------------------------------------------------- esptool bridge

static bool send_all(int c, const uint8_t *p, int n) {
    while (n > 0) {
        int k = send(c, p, n, 0);
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { delay(1); continue; }
            return false;
        }
        p += k;
        n -= k;
    }
    return true;
}

static void serve(int c) {
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    timeval tv = {0, 10000};   // recv waits at most 10 ms, so the UART side keeps moving
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    s_bridge = true;
    s_bridges++;
    s_last_up = s_last_down = 0;
    log_note("\n[devkit] bridge from %s: CAM to download mode\n", s_last_peer);
    U.flush();
    restart_into(true);
    // The ROM prints its banner first; keep it for the log, and throw away what esptool sent while the
    // CAM was still in reset (a burst of stale SYNCs can leave the loader deaf). esptool retries SYNC.
    uint8_t buf[1024];
    uint32_t t0 = millis();
    while (millis() - t0 < 300) {
        int k = U.available();
        if (k > 0) {
            int m = U.read(buf, min(k, (int)sizeof buf));
            log_put((const char *)buf, m);
        }
        delay(5);
    }
    while (recv(c, buf, sizeof buf, MSG_DONTWAIT) > 0) {}

    uint32_t last = millis();
    for (;;) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n == 0) break;   // esptool closed the port
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (n > 0) {
            U.write(buf, n);
            s_last_up += n;
            last = millis();
        }
        int k;
        while ((k = U.available()) > 0) {
            int m = U.read(buf, min(k, (int)sizeof buf));
            if (!send_all(c, buf, m)) goto done;
            s_last_down += m;
            last = millis();
        }
        if (millis() - last > BRIDGE_IDLE_MS) break;
    }
done:
    s_last_ms = millis();
    restart_into(false);
    s_bridge = false;
    log_note("[devkit] bridge closed: PC->CAM %lu B, CAM->PC %lu B; CAM restarted\n",
             (unsigned long)s_last_up, (unsigned long)s_last_down);
}

static void bridge_task(void *) {
    while (!aipnode_wifi_up()) vTaskDelay(pdMS_TO_TICKS(500));
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(BRIDGE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
        aipnode_logf("[devkit] bridge port %u did not open\n", BRIDGE_PORT);
        close(ls);
        vTaskDelete(nullptr);
    }
    for (;;) {
        sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept(ls, (sockaddr *)&peer, &pl);
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        inet_ntoa_r(peer.sin_addr, s_last_peer, sizeof s_last_peer);
        if (aipnode_ota_busy()) {   // the devkit itself is updating
            close(c);
            continue;
        }
        serve(c);
        close(c);
    }
}

// ---------------------------------------------------------------- console and log

void cam_poll() {
    if (s_bridge) return;
    char b[256];
    int k;
    while ((k = U.available()) > 0) {
        int m = U.read((uint8_t *)b, min(k, (int)sizeof b));
        log_put(b, m);
        s_rx_bytes += m;
        s_rx_ms = millis();
    }
}

bool cam_bridge_active() { return s_bridge; }

void cam_set_swap(bool on) {
    if (s_bridge) return;
    s_swap = on;
    Preferences p;
    p.begin("camlink", false);
    p.putBool("swap", on);
    p.end();
    U.end();
    U.begin(115200, SERIAL_8N1, on ? PIN_CAM_TX : PIN_CAM_RX, on ? PIN_CAM_RX : PIN_CAM_TX);
    log_note("\n[devkit] CAM UART %s\n", on ? "swapped (RX 18, TX 19)" : "normal (RX 19, TX 18)");
}

bool cam_send_line(const char *line) {
    if (s_bridge) return false;
    U.print(line);
    U.print('\n');
    return true;
}

bool cam_send_wifi() {
    if (s_bridge) return false;
    Preferences p;
    String ssid, pass;
    if (p.begin("net", true)) {
        ssid = p.getString("ssid", "");
        pass = p.getString("pass", "");
        p.end();
    }
    if (!ssid.length()) return false;
    U.printf("wifi %s %s\n", ssid.c_str(), pass.c_str());   // the CAM answers "trying <ssid>", never the password
    log_note("\n[devkit] gave the CAM the network %s\n", ssid.c_str());
    return true;
}

// Copies the ring oldest-first into out (cap includes the terminating 0); returns the length.
static size_t log_copy(char *out, size_t cap) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    size_t n = s_full ? sizeof s_log : s_head;
    size_t start = s_full ? s_head : 0;
    if (n > cap - 1) { start = (start + n - (cap - 1)) % sizeof s_log; n = cap - 1; }
    for (size_t i = 0; i < n; i++) out[i] = s_log[(start + i) % sizeof s_log];
    xSemaphoreGive(s_mu);
    out[n] = 0;
    return n;
}

void cam_print_log(Print &out) {
    static char b[sizeof s_log + 1];
    size_t n = log_copy(b, sizeof b);
    out.write((const uint8_t *)b, n);
    out.println();
}

void cam_info(JsonObject o) {
    JsonObject c = o["cam_link"].to<JsonObject>();
    c["bridge"] = (bool)s_bridge;
    c["bridges"] = s_bridges;
    c["last_up"] = s_last_up;
    c["last_down"] = s_last_down;
    c["rx_bytes"] = s_rx_bytes;
    c["rx_age_s"] = s_rx_ms ? (int32_t)((millis() - s_rx_ms) / 1000) : -1;
    c["swap"] = s_swap;
}

// ---------------------------------------------------------------- HTTP

static esp_err_t h_log(httpd_req_t *req) {
    static char b[sizeof s_log + 1];
    size_t n = log_copy(b, sizeof b);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, b, n);
}

static esp_err_t h_reset(httpd_req_t *req) {
    if (s_bridge) return aipnode_send_text(req, "409 Conflict", "bridge running");
    cam_restart();
    return aipnode_send_text(req, "200 OK", "ok");
}

static esp_err_t h_send(httpd_req_t *req) {
    char line[200];
    int n = aipnode_read_body(req, line, sizeof line);
    if (n <= 0) return aipnode_send_text(req, "400 Bad Request", "body = one line");
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!cam_send_line(line)) return aipnode_send_text(req, "409 Conflict", "bridge running");
    return aipnode_send_text(req, "200 OK", "ok");
}

static esp_err_t h_wifi(httpd_req_t *req) {
    if (s_bridge) return aipnode_send_text(req, "409 Conflict", "bridge running");
    if (!cam_send_wifi()) return aipnode_send_text(req, "409 Conflict", "no saved network");
    return aipnode_send_text(req, "200 OK", "ok");
}

void cam_register(httpd_handle_t h) {
    httpd_uri_t u = {};
    u.uri = "/cam/log"; u.method = HTTP_GET; u.handler = h_log; httpd_register_uri_handler(h, &u);
    u.uri = "/cam/reset"; u.method = HTTP_POST; u.handler = h_reset; httpd_register_uri_handler(h, &u);
    u.uri = "/cam/send"; u.method = HTTP_POST; u.handler = h_send; httpd_register_uri_handler(h, &u);
    u.uri = "/cam/wifi"; u.method = HTTP_POST; u.handler = h_wifi; httpd_register_uri_handler(h, &u);
}

void cam_begin() {
    s_mu = xSemaphoreCreateMutex();
    // Both opto LEDs off before anything else: the CAM runs its firmware.
    pinMode(PIN_CAM_IO0, OUTPUT);
    digitalWrite(PIN_CAM_IO0, LOW);
    pinMode(PIN_CAM_EN, OUTPUT);
    digitalWrite(PIN_CAM_EN, LOW);
    Preferences p;
    if (p.begin("camlink", true)) {
        s_swap = p.getBool("swap", false);
        p.end();
    }
    U.setRxBufferSize(4096);
    U.begin(115200, SERIAL_8N1, s_swap ? PIN_CAM_TX : PIN_CAM_RX, s_swap ? PIN_CAM_RX : PIN_CAM_TX);
    xTaskCreate(bridge_task, "cambridge", 4096, nullptr, 3, nullptr);
}
