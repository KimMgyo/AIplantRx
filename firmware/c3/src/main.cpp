// AIplantRx display unit: ESP32-C3 SuperMini (UI.md 5, ARCHITECTURE.md 4.4, INTERFACES.md 4).
//
//   - Five OLEDs behind a TCA9548A (IO5 SDA, IO6 SCL; channels 2..6 = OLED 1..5) show the sensor node's
//     values. The C3 reads the node itself (GET /state every 2 s, GET /thermal every 4 s), so the OLEDs
//     keep working while the S3 restarts or updates.
//   - The C3's own USB-C is the box's only port, and it belongs to the S3: an esptool SYNC switches the
//     C3 into a byte relay to the S3's ROM loader (relay_* below, measured 2026-11-21 with c3_hwtest).
//     Nothing is printed on USB unless asked ("dump <ch>", "info", "relay", "wifi <ssid> <pass>").
//   - The S3's serial output (its UART0 comes here, IO20) is kept in a ring buffer: GET /s3log.
//   - Common module (firmware/common): Wi-Fi with setup AP, UDP 4210, /info, /wifi, /ota with rollback,
//     espota (aiplantrx-oled.local).
//
// HTTP (port 80, besides the common ones):
//   GET /s3log            the S3's last ~8 KB of serial output (text)
//   GET /s3?line=info     sends one line to the S3 over the UART
//   GET /oled?ch=2..6     that OLED's frame buffer, 1024 bytes (SSD1306 page order) for tools/oled_dump.py
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <time.h>
#include "aipnode.h"
#include "oled_ui.h"

#ifndef FW_VERSION
#define FW_VERSION "1.0.0-dev"
#endif

#define PIN_SDA 5
#define PIN_SCL 6
#define PIN_S3_RX 20  // C3 RX <- S3 TXD (GPIO43)
#define PIN_S3_TX 21  // C3 TX -> S3 RXD (GPIO44)
#define PIN_LED 8     // SuperMini blue LED, LOW = on
#define TCA 0x70
#define FIRST_CH 2    // OLED 1 is on channel 2
#define USB_TX_TIMEOUT_MS 10  // see setup()

struct Oled {
    bool found = false, ok = false;
    uint8_t addr = 0;
    uint32_t err = 0;
    Adafruit_SSD1306 *d = nullptr;
};
static Oled oled[8];
static bool tca_ok = false;

// ---------------------------------------------------------------- I2C, TCA, OLEDs

static bool ping(uint8_t a) {
    Wire.beginTransmission(a);
    return Wire.endTransmission() == 0;
}

static bool tca_sel(int ch) {
    uint8_t m = ch < 0 ? 0 : (1 << ch);
    Wire.beginTransmission(TCA);
    Wire.write(m);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)TCA, (uint8_t)1) != 1) return false;
    return Wire.read() == m;
}

static void scan() {
    tca_ok = ping(TCA) && tca_sel(-1);
    if (!tca_ok) {
        for (auto &o : oled) o.found = o.ok = false;
        return;
    }
    for (int ch = 0; ch < 8; ch++) {
        uint8_t oa = 0;
        if (tca_sel(ch)) {
            if (ping(0x3C)) oa = 0x3C;
            else if (ping(0x3D)) oa = 0x3D;
        }
        if (!oa) oled[ch].ok = false;
        oled[ch].found = oa != 0;
        oled[ch].addr = oa;
    }
    tca_sel(-1);
}

static bool oled_begin(int ch) {
    Oled &o = oled[ch];
    if (!o.d) o.d = new Adafruit_SSD1306(128, 64, &Wire, -1, 400000, 400000);
    if (!tca_sel(ch)) return o.ok = false;
    // periphBegin=false: Wire is already on IO5/IO6; letting the library call Wire.begin() could move it.
    o.ok = o.d->begin(SSD1306_SWITCHCAPVCC, o.addr, true, false);
    return o.ok;
}

static int n_found() {
    int k = 0;
    for (auto &o : oled) k += o.found;
    return k;
}

static int n_ok() {
    int k = 0;
    for (auto &o : oled) k += o.ok;
    return k;
}

static volatile bool relay = false;   // the S3 upload relay is running (see relay_start)

// ---------------------------------------------------------------- sensor node

static NodeView s_view;                 // written by node_task, copied by the drawing loop
static SemaphoreHandle_t s_view_mu;
static uint32_t s_state_ms;             // millis() of the last good /state
static uint32_t s_state_ok, s_state_fail, s_th_ok, s_th_fail;
static char s_node_ip[16];

static float pick(JsonVariantConst v) { return v.isNull() ? NAN : v.as<float>(); }

static void pair(JsonVariantConst a, float &lo, float &hi) {
    if (a.is<JsonArrayConst>() && a.size() == 2) {
        lo = a[0].as<float>();
        hi = a[1].as<float>();
    } else lo = hi = NAN;
}

static bool parse_state(const String &body, NodeView &v) {
    JsonDocument d;
    if (deserializeJson(d, body)) return false;
    v.air_c = pick(d["air_c"]);
    v.rh = pick(d["rh"]);
    v.co2 = pick(d["co2"]);
    v.soil_c = pick(d["soil_c"]);
    v.soil_pct = pick(d["soil_pct"]);
    v.ec = pick(d["ec"]);
    v.ph = pick(d["ph"]);
    v.leaf_c = pick(d["leaf_c"]);
    v.th_max = pick(d["th_max"]);
    v.day = d["day"] | true;
    v.water = d["water"] | false;
    v.nutr = d["nutr"] | false;
    v.ph_pump = d["ph_pump"] | false;
    v.capture = d["capture"] | false;
    v.ok_scd41 = d["sensors"]["scd41"] | true;
    v.ok_mlx = d["sensors"]["mlx"] | true;
    v.ok_soil = d["sensors"]["soil"] | true;
    uint32_t t = d["t"] | 0;
    v.now = t ? t : (uint32_t)time(nullptr);
    v.last_water = d["last_water"] | 0;
    v.next_nutr = d["next_nutr"] | 0;
    JsonObjectConst p = d["profile"];
    float dlo, dhi, nlo, nhi;
    pair(p["temp_day"], dlo, dhi);
    pair(p["temp_night"], nlo, nhi);
    v.t_lo = v.day ? dlo : nlo;
    v.t_hi = v.day ? dhi : nhi;
    pair(p["rh"], v.rh_lo, v.rh_hi);
    pair(p["soil"], v.soil_lo, v.soil_hi);
    pair(p["ec"], v.ec_lo, v.ec_hi);
    pair(p["ph"], v.ph_lo, v.ph_hi);
    return true;
}

// One kept-alive connection to the node (the S3 found that a fresh HTTPClient per request, destroyed
// while the server kept the connection, corrupts the heap; this one lives for the whole run).
static WiFiClient s_wc;
static HTTPClient s_hc;
static char s_hc_ip[16];

static int node_get(const AipPeer &p, const char *path, String *text, uint8_t *bin, size_t bin_len) {
    char url[64];
    snprintf(url, sizeof url, "http://%s:%u%s", p.ip.toString().c_str(), p.port, path);
    if (strcmp(s_hc_ip, p.ip.toString().c_str())) {
        s_hc.end();
        strlcpy(s_hc_ip, p.ip.toString().c_str(), sizeof s_hc_ip);
    }
    s_hc.setReuse(true);
    s_hc.setConnectTimeout(1500);
    s_hc.setTimeout(2000);
    if (!s_hc.begin(s_wc, url)) return -1;
    int code = s_hc.GET();
    if (code == 200) {
        if (text) *text = s_hc.getString();
        else {
            int len = s_hc.getSize();
            if (len != (int)bin_len) code = -2;
            else {
                WiFiClient *st = s_hc.getStreamPtr();
                size_t got = 0;
                uint32_t t0 = millis();
                while (got < bin_len && millis() - t0 < 2000) {
                    int n = st->read(bin + got, bin_len - got);
                    if (n > 0) got += n;
                    else delay(2);
                }
                if (got != bin_len) code = -3;
            }
        }
    }
    if (code != 200) s_hc.end();   // drop the connection after an error
    return code;
}

static void node_task(void *) {
    static int16_t th[768];
    uint32_t t_state = 0, t_th = 0;
    for (;;) {
        uint32_t now = millis();
        if (relay) {   // the S3 upload relay gets the (single) core to itself
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        AipPeer p;
        bool found = !aipnode_ota_busy() && aipnode_wifi_up() && aipnode_peer("node", &p, 20000);
        if (found) strlcpy(s_node_ip, p.ip.toString().c_str(), sizeof s_node_ip);
        else s_node_ip[0] = 0;
        if (found && now - t_state >= 2000) {
            t_state = now;
            String body;
            static NodeView v;   // 1.6 KB: off this task's stack
            xSemaphoreTake(s_view_mu, portMAX_DELAY);
            v = s_view;
            xSemaphoreGive(s_view_mu);
            if (node_get(p, "/state", &body, nullptr, 0) == 200 && parse_state(body, v)) {
                v.have = true;
                s_state_ms = millis();
                s_state_ok++;
                xSemaphoreTake(s_view_mu, portMAX_DELAY);
                memcpy(v.th, s_view.th, sizeof v.th);   // the thermal grid is the other request's
                v.have_th = s_view.have_th;
                s_view = v;
                xSemaphoreGive(s_view_mu);
            } else s_state_fail++;
        }
        if (found && now - t_th >= 4000) {
            t_th = now;
            if (node_get(p, "/thermal", nullptr, (uint8_t *)th, sizeof th) == 200) {
                s_th_ok++;
                xSemaphoreTake(s_view_mu, portMAX_DELAY);
                memcpy(s_view.th, th, sizeof th);
                s_view.have_th = true;
                xSemaphoreGive(s_view_mu);
            } else s_th_fail++;
        }
        xSemaphoreTake(s_view_mu, portMAX_DELAY);
        s_view.found = found;
        s_view.age_ms = s_view.have ? millis() - s_state_ms : 0;
        xSemaphoreGive(s_view_mu);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------- S3 serial

static char s3_ring[8192];
static size_t s3_head = 0, s3_len = 0;
static portMUX_TYPE s3_mux = portMUX_INITIALIZER_UNLOCKED;
static char s3line[160];
static int s3n = 0;
static uint32_t s3_last_ok = 0, s3_bytes = 0;
static char s3_ver[24] = "";

static void on_s3_line(const char *l) {
    const char *p = strstr(l, "info ver=");
    if (!p) return;
    p += 9;
    size_t n = strcspn(p, " \r\n");
    if (n >= sizeof s3_ver) n = sizeof s3_ver - 1;
    memcpy(s3_ver, p, n);
    s3_ver[n] = 0;
    s3_last_ok = millis();
}

static void ring_put(const char *s, size_t n) {
    portENTER_CRITICAL(&s3_mux);
    for (size_t i = 0; i < n; i++) {
        s3_ring[s3_head] = s[i];
        s3_head = (s3_head + 1) % sizeof s3_ring;
        if (s3_len < sizeof s3_ring) s3_len++;
    }
    portEXIT_CRITICAL(&s3_mux);
}

// Whole lines go to the ring, except the S3's answers to our own "info" every 3 s.  A line longer than
// the line buffer is stored in pieces.
static void poll_s3() {
    while (Serial0.available()) {
        int c = Serial0.read();
        s3_bytes++;
        if (c == '\n' || c == '\r') {
            if (s3n) {
                s3line[s3n] = 0;
                on_s3_line(s3line);
                if (strncmp(s3line, "info ver=", 9)) {
                    s3line[s3n++] = '\n';
                    ring_put(s3line, s3n);
                }
                s3n = 0;
            }
        } else {
            if (s3n >= (int)sizeof(s3line) - 2) {
                ring_put(s3line, s3n);
                s3n = 0;
            }
            s3line[s3n++] = c;
        }
    }
}

static bool s3_alive() { return s3_last_ok && millis() - s3_last_ok < 10000; }

// ---------------------------------------------------------------- S3 upload relay (ARCHITECTURE.md 4.4)
// esptool on the PC talks to the C3's own USB port:
//   esptool --chip esp32s3 --port COMxx --before no_reset --after watchdog_reset -b 460800 write_flash ...
// The first esptool SYNC packet (SLIP C0 00 08) switches the C3 into relay mode: it sends "aipx-download"
// to the S3 app (which restarts into the ROM serial loader, devsrv.cpp) and from then on copies bytes both
// ways.  esptool's CHANGE_BAUDRATE (command 0x0F) is followed: the new rate is read from the request and
// applied to the S3 UART right after the S3's reply has gone to the PC (the C3's USB side has no baud
// rate).  8 s without bytes from the PC ends relay mode.  Unchanged from c3_hwtest (tested 2026-11-21).
struct Slip {
    bool in = false, esc = false;
    uint8_t b[12];
    int n = 0;
};

static bool slip_feed(Slip &s, uint8_t c) {
    if (c == 0xC0) {
        if (s.in && s.n) {
            s.in = false;
            return true;
        }
        s.in = true;
        s.n = 0;
        s.esc = false;
        return false;
    }
    if (!s.in) return false;
    if (s.esc) {
        c = c == 0xDC ? 0xC0 : (c == 0xDD ? 0xDB : c);
        s.esc = false;
    } else if (c == 0xDB) {
        s.esc = true;
        return false;
    }
    if (s.n < 12) s.b[s.n] = c;
    s.n++;
    return false;
}

#define RELAY_SETTLE_MS 1500
static char relay_log[400];
static size_t relay_log_n = 0;
static uint32_t relay_banner_ms = 0;
static int relay_selftest = 0;
static uint32_t relay_found_baud = 0;
static uint32_t relay_t = 0, relay_up = 0, relay_down = 0, relay_baud = 115200, pending_baud = 0, relay_count = 0;
static Slip slip_pc, slip_s3;

static void all_oleds(void (*f)(Adafruit_SSD1306 *, int), int arg) {
    for (int ch = 0; ch < 8; ch++) {
        if (!oled[ch].ok || !tca_sel(ch)) continue;
        f(oled[ch].d, arg);
        oled[ch].d->display();
    }
    tca_sel(-1);
}

static int sync_test(int tries) {
    static const uint8_t SYNC[] = {0xC0, 0x00, 0x08, 0x24, 0, 0, 0, 0, 0, 0x07, 0x07, 0x12, 0x20,
                                   0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                   0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                   0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xC0};
    for (int attempt = 1; attempt <= tries; attempt++) {
        while (Serial0.available()) Serial0.read();
        Serial0.write(SYNC, sizeof SYNC);
        Serial0.flush();
        uint32_t t1 = millis();
        Slip sl;
        while (millis() - t1 < 250) {
            if (!Serial0.available()) {
                delay(1);
                continue;
            }
            if (slip_feed(sl, Serial0.read()) && sl.n >= 2 && sl.b[0] == 1 && sl.b[1] == 0x08) return attempt;
        }
    }
    return 0;
}

static void relay_start() {
    relay = true;
    // One core: with Wi-Fi, the node polling and the HTTP server also running, esptool's 4 KB blocks
    // stalled long enough for "The chip stopped responding" (2026-11-21, twice at 460800 and 115200).
    vTaskPrioritySet(nullptr, 10);
    relay_count++;
    relay_t = millis();
    relay_up = relay_down = pending_baud = 0;
    relay_baud = 115200;
    relay_found_baud = 0;
    slip_pc = Slip();
    slip_s3 = Slip();
    all_oleds([](Adafruit_SSD1306 *d, int) { ui_upload(d); }, 0);
    Serial.setTxTimeoutMs(200);  // replies to esptool must not be dropped
    Serial0.updateBaudRate(115200);
    while (Serial0.available()) Serial0.read();
    Serial0.print("\naipx-download\n");
    Serial0.flush();
    const char *want = "waiting for download";
    size_t got = 0;
    uint32_t t0 = millis();
    relay_log_n = 0;
    while (millis() - t0 < 2500 && want[got]) {
        if (!Serial0.available()) {
            delay(1);
            continue;
        }
        char c = Serial0.read();
        if (relay_log_n < sizeof relay_log - 1) relay_log[relay_log_n++] = c;
        got = c == want[got] ? got + 1 : (c == want[0] ? 1 : 0);
    }
    relay_banner_ms = want[got] ? 0 : millis() - t0;
    // Forwarding esptool's SYNC straight after the banner left the loader deaf (2026-11-21): settle, send
    // a plain line, check with our own SYNC, then let esptool's traffic through.
    delay(RELAY_SETTLE_MS);
    while (Serial0.available()) Serial0.read();
    Serial0.print("\r\n");
    Serial0.flush();
    delay(100);
    relay_selftest = sync_test(5);
    // A broken-off upload leaves esptool's stub at its faster rate: find it and bring it back to 115200.
    static const uint32_t FALLBACK[] = {460800, 921600, 230400, 1843200, 3686400};
    for (uint32_t b : FALLBACK) {
        if (relay_selftest) break;
        Serial0.updateBaudRate(b);
        int a = sync_test(3);
        if (a) {
            relay_selftest = a;
            relay_found_baud = b;
        }
    }
    if (relay_found_baud) {
        uint32_t nb = 115200, ob = relay_found_baud;
        uint8_t f[] = {0xC0, 0x00, 0x0F, 0x08, 0x00, 0, 0, 0, 0,
                       (uint8_t)nb, (uint8_t)(nb >> 8), (uint8_t)(nb >> 16), (uint8_t)(nb >> 24),
                       (uint8_t)ob, (uint8_t)(ob >> 8), (uint8_t)(ob >> 16), (uint8_t)(ob >> 24), 0xC0};
        while (Serial0.available()) Serial0.read();
        Serial0.write(f, sizeof f);
        Serial0.flush();
        uint32_t t1 = millis();
        Slip sl;
        bool ok = false;
        while (millis() - t1 < 300 && !ok) {
            if (!Serial0.available()) {
                delay(1);
                continue;
            }
            ok = slip_feed(sl, Serial0.read()) && sl.n >= 2 && sl.b[0] == 1 && sl.b[1] == 0x0F;
        }
        delay(20);
    }
    Serial0.updateBaudRate(115200);
    if (relay_found_baud) relay_selftest = sync_test(3);
    delay(50);
    while (Serial0.available()) Serial0.read();
    while (Serial.available()) Serial.read();
}

static void relay_report(Print &out) {
    out.printf("[relay] count %lu, last: PC->S3 %lu B, S3->PC %lu B, last baud %lu, banner after %lu ms, own SYNC answered %s, stub found at %lu\n",
               (unsigned long)relay_count, (unsigned long)relay_up, (unsigned long)relay_down, (unsigned long)relay_baud,
               (unsigned long)relay_banner_ms, relay_selftest ? String("at try " + String(relay_selftest)).c_str() : "NO",
               (unsigned long)relay_found_baud);
    relay_log[relay_log_n] = 0;
    for (size_t i = 0; i < relay_log_n; i++)
        if (relay_log[i] != '\n' && (relay_log[i] < 32 || relay_log[i] > 126)) relay_log[i] = '.';
    out.printf("[relay] S3 said while restarting:\n%s\n", relay_log);
}

static void relay_loop() {
    static uint8_t buf[512];
    uint32_t now = millis();
    int k;
    while ((k = Serial.available()) > 0) {
        k = Serial.read(buf, k < (int)sizeof buf ? k : (int)sizeof buf);
        if (k <= 0) break;
        for (int i = 0; i < k; i++)
            if (slip_feed(slip_pc, buf[i]) && slip_pc.n >= 12 && slip_pc.b[0] == 0 && slip_pc.b[1] == 0x0F)
                pending_baud = slip_pc.b[8] | slip_pc.b[9] << 8 | (uint32_t)slip_pc.b[10] << 16 | (uint32_t)slip_pc.b[11] << 24;
        Serial0.write(buf, k);
        relay_up += k;
        relay_t = now;
    }
    while ((k = Serial0.available()) > 0) {
        k = Serial0.read(buf, k < (int)sizeof buf ? k : (int)sizeof buf);
        if (k <= 0) break;
        int from = 0;
        for (int i = 0; i < k; i++)
            if (slip_feed(slip_s3, buf[i]) && pending_baud && slip_s3.n >= 2 && slip_s3.b[0] == 1 && slip_s3.b[1] == 0x0F) {
                Serial.write(buf + from, i + 1 - from);  // the reply still goes out at the old rate
                from = i + 1;
                Serial.flush();
                Serial0.updateBaudRate(pending_baud);
                relay_baud = pending_baud;
                pending_baud = 0;
            }
        if (k > from) Serial.write(buf + from, k - from);
        relay_down += k;
    }
    if (now - relay_t > 8000) {  // esptool finished or gave up
        relay = false;
        vTaskPrioritySet(nullptr, 1);
        Serial0.updateBaudRate(115200);
        Serial.setTxTimeoutMs(USB_TX_TIMEOUT_MS);
        delay(2000);  // let the S3 app boot before the C3 talks to it again
        while (Serial0.available()) Serial0.read();
        for (auto &o : oled) o.ok = false;  // re-init and redraw
    }
}

// ---------------------------------------------------------------- USB console (only answers)

static void dump_oled(Print &out, int ch) {
    if (ch < 0 || ch > 7 || !oled[ch].d) {
        out.println("[dump] no OLED there");
        return;
    }
    const uint8_t *b = oled[ch].d->getBuffer();
    out.printf("[dump] ch%d ", ch);
    for (int i = 0; i < 1024; i++) out.printf("%02x", b[i]);
    out.println();
}

static void poll_usb() {
    static char line[200];
    static int n = 0;
    static int h0 = -1, h1 = -1;
    while (Serial.available()) {
        int c = Serial.read();
        if (h0 == 0xC0 && h1 == 0x00 && c == 0x08) {  // esptool SYNC: become the S3 upload relay
            h0 = h1 = -1;
            n = 0;
            relay_start();
            return;
        }
        h0 = h1;
        h1 = c;
        if (c != '\n' && c != '\r') {
            if (n < (int)sizeof(line) - 1) line[n++] = c;
            continue;
        }
        if (!n) continue;
        line[n] = 0;
        n = 0;
        if (!strncmp(line, "dump ", 5)) dump_oled(Serial, atoi(line + 5));
        else if (!strcmp(line, "relay")) relay_report(Serial);
        else if (!strncmp(line, "s3 ", 3)) Serial0.printf("%s\n", line + 3);
        else if (!aipnode_command(line, Serial)) Serial.println("[c3] commands: info | wifi <ssid> <pass> | reboot | dump <ch> | relay | s3 <line>");
    }
}

// ---------------------------------------------------------------- HTTP

static esp_err_t h_s3log(httpd_req_t *req) {
    static char out[sizeof s3_ring];
    size_t n;
    portENTER_CRITICAL(&s3_mux);
    n = s3_len;
    size_t start = (s3_head + sizeof s3_ring - n) % sizeof s3_ring;
    for (size_t i = 0; i < n; i++) out[i] = s3_ring[(start + i) % sizeof s3_ring];
    portEXIT_CRITICAL(&s3_mux);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, out, n);
}

static esp_err_t h_s3(httpd_req_t *req) {
    char q[160], line[128];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "line", line, sizeof line) != ESP_OK)
        return aipnode_send_text(req, "400 Bad Request", "need ?line=");
    // %20 and + in the query become spaces.
    char *w = line;
    for (char *r = line; *r; r++) {
        if (*r == '+') *w++ = ' ';
        else if (*r == '%' && r[1] && r[2]) {
            char h[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(h, nullptr, 16);
            r += 2;
        } else *w++ = *r;
    }
    *w = 0;
    if (!strcmp(line, "aipx-download")) return aipnode_send_text(req, "403 Forbidden", "use the USB relay");
    if (relay) return aipnode_send_text(req, "409 Conflict", "relay running");
    Serial0.printf("%s\n", line);
    return aipnode_send_text(req, "200 OK", "sent");
}

// The OLED buffers are written by the loop; a copy taken mid-draw is at worst one frame torn.
static esp_err_t h_oled(httpd_req_t *req) {
    char q[32], v[8];
    int ch = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "ch", v, sizeof v) == ESP_OK) ch = atoi(v);
    if (ch < 0 || ch > 7 || !oled[ch].d) return aipnode_send_text(req, "404 Not Found", "no OLED there");
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)oled[ch].d->getBuffer(), 1024);
}

static void info_extra(JsonObject o) {
    o["tca"] = tca_ok;
    o["oled"] = n_ok();
    JsonObject n = o["node"].to<JsonObject>();
    n["ip"] = s_node_ip;
    n["have"] = s_view.have;
    n["age_ms"] = s_view.age_ms;
    n["state_ok"] = s_state_ok;
    n["state_fail"] = s_state_fail;
    n["thermal_ok"] = s_th_ok;
    n["thermal_fail"] = s_th_fail;
    JsonObject s = o["s3"].to<JsonObject>();
    s["alive"] = s3_alive();
    s["ver"] = s3_ver;
    s["rx_bytes"] = s3_bytes;
    s["relays"] = relay_count;
}

static bool self_check() { return tca_ok && n_ok() > 0; }

// ---------------------------------------------------------------- main

void setup() {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    Serial.setRxBufferSize(24576);  // relay: esptool sends a whole 16 KB flash block before it waits
    Serial.begin(115200);
    // Not 0: HWCDC::write (core 3.1) counts its retries down from this value, so 0 underflows and write()
    // spins until a PC opens the port. With 10 it gives up after 10 ms and drops output.
    Serial.setTxTimeoutMs(USB_TX_TIMEOUT_MS);
    Serial0.setRxBufferSize(2048);
    Serial0.setTxBufferSize(2048);
    Serial0.begin(115200, SERIAL_8N1, PIN_S3_RX, PIN_S3_TX);
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    s_view_mu = xSemaphoreCreateMutex();
    s_view.have = false;

    scan();
    for (int ch = 0; ch < 8; ch++)
        if (oled[ch].found) oled_begin(ch);
    tca_sel(-1);

    static AipNodeConfig cfg;
    cfg.role = "oled";
    cfg.name = "oled";
    cfg.version = FW_VERSION;
    cfg.log = nullptr;      // the USB port is the S3's relay
    cfg.tx_power = WIFI_POWER_8_5dBm;   // SuperMini antenna (see aipnode.h)
    cfg.self_check = self_check;
    cfg.info_extra = info_extra;
    aipnode_begin(cfg);
    httpd_handle_t h = aipnode_http();
    if (h) {
        httpd_uri_t u = {};
        u.method = HTTP_GET;
        u.uri = "/s3log"; u.handler = h_s3log; httpd_register_uri_handler(h, &u);
        u.uri = "/s3"; u.handler = h_s3; httpd_register_uri_handler(h, &u);
        u.uri = "/oled"; u.handler = h_oled; httpd_register_uri_handler(h, &u);
    }
    configTzTime("KST-9", "pool.ntp.org", "time.google.com");
    xTaskCreate(node_task, "node", 6144, nullptr, 1, nullptr);
}

void loop() {
    static uint32_t t_draw = 0, t_info = 0, t_led = 0, t_scan = 0;
    static bool led = false;
    uint32_t now = millis();
    if (relay) {  // S3 upload in progress: nothing else touches the UART or the USB port
        relay_loop();
        if (now - t_led >= 60) {
            t_led = now;
            led = !led;
            digitalWrite(PIN_LED, led ? LOW : HIGH);
        }
        return;
    }
    poll_s3();
    poll_usb();
    if (relay) return;
    if (now - t_scan >= 5000 && (!tca_ok || n_found() < 5)) {  // something missing: look again
        t_scan = now;
        scan();
    }
    if (now - t_draw >= 150) {
        t_draw = now;
        NodeView v;
        xSemaphoreTake(s_view_mu, portMAX_DELAY);
        v = s_view;
        xSemaphoreGive(s_view_mu);
        int pct = aipnode_ota_pct();
        bool self_upd = aipnode_ota_busy() && pct >= 0;
        for (int ch = 0; ch < 8; ch++) {
            Oled &o = oled[ch];
            if (!o.found || !tca_sel(ch)) continue;
            if (!ping(o.addr)) {
                o.err++;
                o.ok = false;
                continue;
            }
            if (!o.ok && !oled_begin(ch)) continue;
            if (self_upd) ui_self_update(o.d, pct);
            else ui_panel(o.d, ch - FIRST_CH, now, v);
            o.d->display();
        }
        tca_sel(-1);
    }
    if (now - t_info >= 3000) {
        t_info = now;
        Serial0.print("info\n");
    }
    bool good = tca_ok && n_ok() == 5 && aipnode_wifi_up() && s_view.have && s_view.age_ms < 10000;
    if (now - t_led >= (good ? 500u : 120u)) {
        t_led = now;
        led = !led;
        digitalWrite(PIN_LED, led ? LOW : HIGH);
    }
    delay(2);
}
