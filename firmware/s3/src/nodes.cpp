#include "nodes.h"
#include <freertos/idf_additions.h>
#include "app.h"
#include "net.h"
#include "perf.h"
#include "ui_assets.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <WiFi.h>
#include <WiFiUdp.h>

static const int FW = 320, FH = 240;
static const uint32_t CAM_PERIOD_MS = 83;     // camera at most 12 fps
static const uint32_t TH_PERIOD_MS = 125;     // thermal polled at 8 Hz (the MLX90640 runs at 4 or 8)

// ---------------------------------------------------------------- triple buffer
struct Tri {
    uint16_t *b[3];
    volatile int ready = -1, shown = -1;
    volatile uint32_t seq = 0;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bool init() {
        for (auto &p : b) if (!(p = (uint16_t *)ps_malloc_or_null(FW * FH * 2))) return false;
        return true;
    }
    // The free buffer, picked under the lock: read outside it, `shown` could move to the buffer we pick.
    int claim() {
        portENTER_CRITICAL(&mux);
        int k = 0;
        for (int i = 0; i < 3; i++) if (i != ready && i != shown) { k = i; break; }
        portEXIT_CRITICAL(&mux);
        return k;
    }
    void publish(int i) { portENTER_CRITICAL(&mux); ready = i; seq = seq + 1; portEXIT_CRITICAL(&mux); }
    const uint16_t *take(uint32_t *s) {
        portENTER_CRITICAL(&mux);
        if (ready >= 0) { shown = ready; ready = -1; }
        int k = shown;
        if (s) *s = seq;
        portEXIT_CRITICAL(&mux);
        return k >= 0 ? b[k] : nullptr;
    }
};
static Tri s_cam;

// ---------------------------------------------------------------- discovery
struct Peer { uint32_t last_ms; uint16_t port; char ip[16], name[24], ver[24]; };
static Peer s_peer[ROLE_COUNT];
static portMUX_TYPE s_pmux = portMUX_INITIALIZER_UNLOCKED;
static const uint32_t ONLINE_MS = 15000;

NodeInfo node_info(Role r) {
    NodeInfo n{};
    portENTER_CRITICAL(&s_pmux);
    Peer p = s_peer[r];
    portEXIT_CRITICAL(&s_pmux);
    n.online = p.last_ms && millis() - p.last_ms < ONLINE_MS;
    n.age_s = p.last_ms ? (millis() - p.last_ms) / 1000 : 0;
    n.port = p.port ? p.port : 80;
    memcpy(n.ip, p.ip, sizeof(n.ip));
    memcpy(n.name, p.name, sizeof(n.name));
    memcpy(n.ver, p.ver, sizeof(n.ver));
    return n;
}

// A board that answers is online even when its UDP announcements get lost: Wi-Fi broadcasts go out at the
// lowest rate without retries, and losing three in a row made the camera drop off the screen.
static void peer_seen(Role r) {
    portENTER_CRITICAL(&s_pmux);
    if (s_peer[r].last_ms) s_peer[r].last_ms = millis();
    portEXIT_CRITICAL(&s_pmux);
}

int nodes_online() {
    int k = 0;
    for (int r = 0; r < ROLE_COUNT; r++) k += node_info((Role)r).online;
    return k;
}

static void disc_task(void *) {
    WiFiUDP udp;
    bool bound = false;
    uint32_t last_tx = 0;
    char buf[256];
    for (;;) {
        if (net_state() != NET_UP) {
            if (bound) { udp.stop(); bound = false; }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!bound) bound = udp.begin(DISCOVERY_PORT);
        int n;
        while ((n = udp.parsePacket()) > 0) {
            int len = udp.read(buf, sizeof(buf) - 1);
            if (len <= 0) continue;
            buf[len] = 0;
            JsonDocument doc;
            if (deserializeJson(doc, buf)) continue;
            const char *role = doc["role"] | "";
            int r = !strcmp(role, "cam") ? ROLE_CAM : !strcmp(role, "node") ? ROLE_NODE : !strcmp(role, "oled") ? ROLE_OLED : -1;
            if (r < 0) continue;
            Peer p{};
            p.last_ms = millis();
            strlcpy(p.ip, udp.remoteIP().toString().c_str(), sizeof(p.ip));
            strlcpy(p.name, doc["name"] | role, sizeof(p.name));
            strlcpy(p.ver, doc["ver"] | "", sizeof(p.ver));
            p.port = doc["port"] | 80;
            portENTER_CRITICAL(&s_pmux);
            s_peer[r] = p;
            portEXIT_CRITICAL(&s_pmux);
        }
        if (millis() - last_tx > 5000) {
            last_tx = millis();
            char hello[96];
            int k = snprintf(hello, sizeof(hello), "{\"role\":\"panel\",\"name\":\"%s\",\"ver\":\"%s\"}", APP_HOST, FW_VERSION);
            udp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
            udp.write((const uint8_t *)hello, k);
            udp.endPacket();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------- camera
static volatile bool s_cam_wanted = true;
static volatile float s_cam_fps = 0;
static volatile uint32_t s_cam_ok_ms = 0;
static uint16_t *s_dst;                 // decode target, one frame
static int s_dst_w, s_dst_h;

void cam_set_wanted(bool on) { s_cam_wanted = on; }

static volatile bool s_node_paused;
void nodes_pause(bool on) { s_node_paused = on; }
bool cam_live() { return s_cam_ok_ms && millis() - s_cam_ok_ms < 3000; }
const uint16_t *cam_take(uint32_t *seq) { return s_cam.take(seq); }
uint32_t cam_seq() { return s_cam.seq; }
float cam_fps() { return cam_live() ? s_cam_fps : 0; }

static int jpeg_draw(JPEGDRAW *d) {
    for (int y = 0; y < d->iHeight; y++) {
        int yy = d->y + y;
        if (yy >= s_dst_h) break;
        int w = d->iWidth;
        if (d->x + w > s_dst_w) w = s_dst_w - d->x;
        if (w > 0) memcpy(s_dst + yy * s_dst_w + d->x, d->pPixels + y * d->iWidth, w * 2);
    }
    return 1;
}

// One kept-alive connection per task. A fresh TCP connection per request cost a handshake every frame
// (and a socket on the ESP32-CAM, which has few). Any error closes it, so a half-read body never leaks
// into the next response.
struct Http {
    HTTPClient http;
    WiFiClient c;
    char host[24] = "";
    uint32_t hdr_us = 0;          // request sent and response headers parsed (last call)
    Http() { http.setReuse(true); }
    void drop() { c.stop(); }
    int get(const char *ip, uint16_t port, const char *path, uint8_t *buf, int cap, uint32_t timeout_ms) {
        char h[24], url[64];
        snprintf(h, sizeof h, "%s:%u", ip, port);
        if (strcmp(h, host)) { drop(); strlcpy(host, h, sizeof host); }
        snprintf(url, sizeof url, "http://%s%s", h, path);
        http.setConnectTimeout(timeout_ms);
        http.setTimeout(timeout_ms);
        if (!http.begin(c, url)) { drop(); return -1; }
        uint32_t t0 = micros();
        int code = http.GET();
        hdr_us = micros() - t0;
        int got = -1;
        if (code == 200) {
            int len = http.getSize();
            WiFiClient *s = http.getStreamPtr();
            got = 0;
            uint32_t t0 = millis();
            while ((len < 0 || got < len) && got < cap && millis() - t0 < timeout_ms) {
                int a = s->available();
                if (a <= 0) {
                    if (!s->connected()) break;
                    vTaskDelay(1);
                    continue;
                }
                int r = s->read(buf + got, min(a, cap - got));
                if (r > 0) got += r;
            }
            if (len > 0 && got != len) got = -1;
            if (len < 0) { http.end(); drop(); return got; }   // no length: read to close, cannot reuse
        }
        http.end();
        if (got < 0) drop();
        return got;
    }
};

// Fetch and decode run as two tasks over two JPEG buffers, so the next frame downloads while this one
// decodes. Measured with video on screen: fetch ~120 ms, decode ~40 ms; in one loop that was 160 ms a
// frame (6 fps), overlapped it is the fetch alone.
static const int JCAP = 96 * 1024;
struct Jpg { uint8_t *buf; int len; uint32_t t0_ms, fetch_us, hdr_us; };
static Jpg s_jpg[2];
static QueueHandle_t s_jfree, s_jfull;     // indices into s_jpg

// The CAM's push stream (INTERFACES.md 3, GET :81/stream). Polling /frame paid a request round trip per
// frame on the CAM's busy radio: 80 ms a frame, 11 fps at most (2026-11-21). Pushed, the frames only wait
// for the sensor. Frames that arrive while both JPEG buffers are taken are read and dropped.
static const uint16_t CAM_STREAM_PORT = 81;
static const int CAM_STREAM_FPS = 15;
static uint8_t s_scratch[1024];

static bool rd_line(WiFiClient &c, char *b, int cap, uint32_t timeout_ms) {
    int n = 0;
    uint32_t t0 = millis();
    for (;;) {
        int ch = c.read();
        if (ch < 0) {
            if (!c.connected() || millis() - t0 > timeout_ms) return false;
            vTaskDelay(1);
            continue;
        }
        if (ch == '\n') {
            if (n && b[n - 1] == '\r') n--;
            b[n] = 0;
            return true;
        }
        if (n < cap - 1) b[n++] = (char)ch;
    }
}

// Reads len bytes into b, or throws them away when b is null.
static bool rd_exact(WiFiClient &c, uint8_t *b, int len, uint32_t timeout_ms) {
    int got = 0;
    uint32_t t0 = millis();
    while (got < len) {
        int a = c.available();
        if (a <= 0) {
            if (!c.connected() || millis() - t0 > timeout_ms) return false;
            vTaskDelay(1);
            continue;
        }
        int want = min(a, len - got);
        int r = b ? c.read(b + got, want) : c.read(s_scratch, min(want, (int)sizeof s_scratch));
        if (r > 0) got += r;
    }
    return true;
}

// Runs the stream until it breaks or the camera is not wanted. false: no stream from this CAM (older
// firmware, or no frame came), so the caller polls /frame for a while.
static bool cam_stream(const NodeInfo &n) {
    WiFiClient c;
    if (!c.connect(n.ip, CAM_STREAM_PORT, 1500)) return false;
    c.setNoDelay(true);
    c.printf("GET /stream?fps=%d HTTP/1.1\r\nHost: %s\r\n\r\n", CAM_STREAM_FPS, n.ip);
    char line[96];
    if (!rd_line(c, line, sizeof line, 2000) || !strstr(line, " 200")) { c.stop(); return false; }
    while (rd_line(c, line, sizeof line, 2000) && line[0]) {}
    uint32_t frames = 0;
    for (;;) {
        NodeInfo now = node_info(ROLE_CAM);
        if (!s_cam_wanted || !now.online || strcmp(now.ip, n.ip) || net_state() != NET_UP) break;
        uint32_t t0 = millis();
        if (!rd_line(c, line, sizeof line, 2000)) break;
        if (strncmp(line, "--aipframe", 10)) continue;   // the CRLF after the previous part
        int len = -1;
        while (rd_line(c, line, sizeof line, 2000) && line[0])
            if (!strncasecmp(line, "Content-Length:", 15)) len = atoi(line + 15);
        if (len <= 0 || len > JCAP) break;
        int k;
        if (xQueueReceive(s_jfree, &k, 0) == pdTRUE) {
            Jpg &j = s_jpg[k];
            if (!rd_exact(c, j.buf, len, 2000)) { xQueueSend(s_jfree, &k, 0); break; }
            j.len = len;
            j.t0_ms = t0;
            j.fetch_us = (millis() - t0) * 1000;   // mostly the wait for the next frame
            j.hdr_us = 0;
            xQueueSend(s_jfull, &k, 0);
        } else if (!rd_exact(c, nullptr, len, 2000)) {
            break;
        }
        peer_seen(ROLE_CAM);
        frames++;
    }
    c.stop();
    return frames > 0;
}

static void cam_fetch_task(void *) {
    Http h;
    uint32_t poll_until = 0;   // the CAM had no stream: poll /frame until then, then try the stream again
    for (;;) {
        NodeInfo n = node_info(ROLE_CAM);
        if (!s_cam_wanted || !n.online || net_state() != NET_UP) { h.drop(); vTaskDelay(pdMS_TO_TICKS(300)); continue; }
        if ((int32_t)(millis() - poll_until) >= 0) {
            h.drop();
            if (cam_stream(n)) continue;
            poll_until = millis() + 30000;
        }
        int k;
        if (xQueueReceive(s_jfree, &k, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        Jpg &j = s_jpg[k];
        j.t0_ms = millis();
        j.len = h.get(n.ip, n.port, "/frame", j.buf, JCAP, 2000);
        j.fetch_us = (millis() - j.t0_ms) * 1000;
        j.hdr_us = h.hdr_us;
        if (j.len > 0) { xQueueSend(s_jfull, &k, 0); peer_seen(ROLE_CAM); }
        else { xQueueSend(s_jfree, &k, 0); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        uint32_t el = millis() - j.t0_ms;
        vTaskDelay(pdMS_TO_TICKS(el < CAM_PERIOD_MS ? CAM_PERIOD_MS - el : 2));
    }
}

static void cam_decode_task(void *) {
    JPEGDEC *jpeg = new (ps_malloc_or_null(sizeof(JPEGDEC))) JPEGDEC();
    for (;;) {
        int k;
        if (xQueueReceive(s_jfull, &k, portMAX_DELAY) != pdTRUE) continue;
        Jpg &j = s_jpg[k];
        if (jpeg->openRAM(j.buf, j.len, jpeg_draw)) {
            int slot = s_cam.claim();
            s_dst = s_cam.b[slot];
            s_dst_w = FW;
            s_dst_h = FH;
            int scale = jpeg->getWidth() >= 4 * FW ? JPEG_SCALE_QUARTER : jpeg->getWidth() >= 2 * FW ? JPEG_SCALE_HALF : 0;
            jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
            uint32_t td = micros();
            if (jpeg->decode(0, 0, scale)) {
                perf_cam(j.fetch_us, micros() - td, j.hdr_us);
                s_cam.publish(slot);
                uint32_t now = millis();
                float dt = (now - (s_cam_ok_ms ? s_cam_ok_ms : j.t0_ms)) / 1000.0f;
                if (dt > 0) s_cam_fps = s_cam_fps * 0.8f + (1.0f / dt) * 0.2f;
                s_cam_ok_ms = now;
            }
            jpeg->close();
        }
        xQueueSend(s_jfree, &k, 0);
    }
}

// ---------------------------------------------------------------- sensor node
static SensorState s_sensor{};
static ThermalInfo s_thinfo{};
static volatile uint32_t s_sensor_ms = 0, s_th_ms = 0;
static portMUX_TYPE s_smux = portMUX_INITIALIZER_UNLOCKED;

SensorState sensor_get() {
    portENTER_CRITICAL(&s_smux);
    SensorState s = s_sensor;
    portEXIT_CRITICAL(&s_smux);
    s.live = s_sensor_ms && millis() - s_sensor_ms < 5000;
    return s;
}

ThermalInfo thermal_info() {
    portENTER_CRITICAL(&s_smux);
    ThermalInfo t = s_thinfo;
    portEXIT_CRITICAL(&s_smux);
    t.live = s_th_ms && millis() - s_th_ms < 3000;
    return t;
}

static ThermalGrid s_grid;
static volatile uint32_t s_grid_seq = 0;

bool thermal_grid(ThermalGrid *out, uint32_t *seq) {
    if (!s_grid_seq) return false;
    portENTER_CRITICAL(&s_smux);           // 1.5 KB copy, a couple of microseconds
    *out = s_grid;
    *seq = s_grid_seq;
    portEXIT_CRITICAL(&s_smux);
    return true;
}
uint32_t thermal_seq() { return s_grid_seq; }

static void parse_state(const uint8_t *buf, int len) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return;
    SensorState s{};
    // `| NAN` keeps NAN for a missing field and for null (a sensor that is not answering).
    s.air_c = doc["air_c"] | NAN;
    s.rh = doc["rh"] | NAN;
    s.co2 = doc["co2"] | NAN;
    s.soil_c = doc["soil_c"] | NAN;
    s.soil_pct = doc["soil_pct"] | NAN;
    s.ec = doc["ec"] | NAN;
    s.ph = doc["ph"] | NAN;
    s.leaf_c = doc["leaf_c"] | NAN;
    s.day = doc["day"] | true;
    s.light = doc["light"] | false;
    s.water = doc["water"] | false;
    s.nutr = doc["nutr"] | false;
    s.ph_pump = doc["ph_pump"] | false;
    s.capture = doc["capture"] | false;
    s.ok_scd41 = doc["sensors"]["scd41"] | true;
    s.ok_mlx = doc["sensors"]["mlx"] | true;
    s.ok_soil = doc["sensors"]["soil"] | true;
    s.water_ml_today = doc["water_ml_today"] | 0;
    s.nutr_ml_today = doc["nutr_ml_today"] | 0;
    s.ph_ml_today = doc["ph_ml_today"] | 0;
    s.t = doc["t"] | 0;
    s.last_water = doc["last_water"] | 0;
    s.next_water = doc["next_water"] | 0;
    s.next_nutr = doc["next_nutr"] | 0;
    strlcpy(s.grade, doc["profile"]["grade"] | "", sizeof s.grade);
    for (JsonObject e : doc["events"].as<JsonArray>()) {
        if (s.nev >= 16) break;
        const char *k = e["kind"] | "";
        uint8_t kind = !strcmp(k, "water") ? EV_WATER : !strcmp(k, "nutr") ? EV_NUTR : !strcmp(k, "ph") ? EV_PH : 0;
        if (!kind) continue;
        s.ev[s.nev].kind = kind;
        s.ev[s.nev].at_min = e["at_min"] | 0;
        s.ev[s.nev].len_s = e["len_s"] | 0;
        s.ev[s.nev].ml = e["ml"] | 0;
        s.nev++;
    }
    portENTER_CRITICAL(&s_smux);
    s_sensor = s;
    portEXIT_CRITICAL(&s_smux);
    s_sensor_ms = millis();
}

// 32x24 centi-degrees -> palette grid (drawn by the main screen), range, peak and the leaf box median.
static void ingest_thermal(const int16_t *raw) {
    int lo = INT32_MAX, hi = INT32_MIN, pk = 0;
    for (int i = 0; i < 768; i++) {
        if (raw[i] < lo) lo = raw[i];
        if (raw[i] > hi) { hi = raw[i]; pk = i; }
    }
    int span = hi - lo < 400 ? 400 : hi - lo;  // at least 4 degrees across the palette
    static ThermalGrid g;
    for (int i = 0; i < 768; i++) g.u[i] = (uint16_t)((int32_t)(raw[i] - lo) * 65280 / span);
    // Leaf: median of the cells inside the plant box (UI.md 2, fixed in the middle for now).
    int16_t cells[16 * 12];
    int n = 0;
    for (int y = LEAF_GY0; y < LEAF_GY1; y++)
        for (int x = LEAF_GX0; x < LEAF_GX1; x++) cells[n++] = raw[y * 32 + x];
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cells[j] < cells[j - 1]; j--) { int16_t q = cells[j]; cells[j] = cells[j - 1]; cells[j - 1] = q; }
    ThermalInfo ti{};
    ti.lo = lo / 100.0f; ti.hi = hi / 100.0f; ti.peak_c = ti.hi;
    ti.peak_x = (pk % 32) * 10 + 5;
    ti.peak_y = (pk / 32) * 10 + 5;
    ti.leaf_c = n ? cells[n / 2] / 100.0f : NAN;
    portENTER_CRITICAL(&s_smux);
    s_thinfo = ti;
    s_grid = g;
    s_grid_seq = s_grid_seq + 1;
    portEXIT_CRITICAL(&s_smux);
    s_th_ms = millis();
}

// ---------------------------------------------------------------- commands to the boards
//
// POSTs the UI asks for (pump test, Wi-Fi move). They go out from node_task, which already talks to the
// network, so the LVGL task never waits on a socket.

struct Cmd {
    bool used, done;
    Role role;
    char path[16];
    char body[200];
    int result;
};
static Cmd s_cmds[4];
static portMUX_TYPE s_cmux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_cmd_gen;

int node_post(Role r, const char *path, const char *json) {
    int slot = -1;
    portENTER_CRITICAL(&s_cmux);
    for (int i = 0; i < 4; i++)
        if (!s_cmds[i].used) {
            slot = i;
            s_cmds[i].used = true;
            s_cmds[i].done = false;
            s_cmds[i].role = r;
            strlcpy(s_cmds[i].path, path, sizeof s_cmds[i].path);
            strlcpy(s_cmds[i].body, json, sizeof s_cmds[i].body);
            s_cmds[i].result = 0;
            break;
        }
    portEXIT_CRITICAL(&s_cmux);
    return slot;
}

int node_post_result(int slot, bool *done) {
    if (slot < 0 || slot >= 4) { *done = true; return -1; }
    *done = s_cmds[slot].done;
    int r = s_cmds[slot].result;
    if (*done) s_cmds[slot].used = false;   // read once
    return r;
}

uint32_t node_cmd_gen() { return s_cmd_gen; }

static void run_cmds() {
    for (int i = 0; i < 4; i++) {
        Cmd &c = s_cmds[i];
        if (!c.used || c.done) continue;
        NodeInfo t = node_info(c.role);
        int code = -1;
        if (t.online && net_state() == NET_UP) {
            char url[64];
            snprintf(url, sizeof url, "http://%s:%u%s", t.ip, t.port, c.path);
            WiFiClient wc;
            HTTPClient hc;
            hc.setConnectTimeout(2000);
            hc.setTimeout(4000);
            if (hc.begin(wc, url)) {
                hc.addHeader("Content-Type", "application/json");
                code = hc.POST((uint8_t *)c.body, strlen(c.body));
                hc.end();
            }
        }
        Serial.printf("[nodes] POST %s to role %d -> %d\n", c.path, (int)c.role, code);
        c.result = code;
        c.done = true;
        s_cmd_gen++;
    }
}

static void node_task(void *) {
    uint8_t *buf = (uint8_t *)ps_malloc_or_null(8192);
    static int16_t prev[768];
    bool have_prev = false;
    uint32_t last_state = 0;
    Http h;
    for (;;) {
        run_cmds();
        NodeInfo n = node_info(ROLE_NODE);
        if (!n.online || net_state() != NET_UP || s_node_paused) { h.drop(); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        uint32_t t0 = millis();
        if (millis() - last_state > 1000) {
            last_state = millis();
            int len = h.get(n.ip, n.port, "/state", buf, 8192, 1500);
            if (len > 0) { parse_state(buf, len); peer_seen(ROLE_NODE); }
        }
        if (s_cam_wanted) {
            if (h.get(n.ip, n.port, "/thermal", buf, 8192, 1500) == 768 * 2) {
                // The node may hand out the same frame twice when polled faster than the sensor runs:
                // then only the "live" clock moves, nothing is redrawn.
                if (have_prev && !memcmp(prev, buf, sizeof prev)) s_th_ms = millis();
                else {
                    memcpy(prev, buf, sizeof prev);
                    have_prev = true;
                    uint32_t tt = micros();
                    ingest_thermal(prev);
                    perf_thermal(micros() - tt);
                }
            }
        }
        uint32_t el = millis() - t0;
        vTaskDelay(pdMS_TO_TICKS(el < TH_PERIOD_MS ? TH_PERIOD_MS - el : 5));
    }
}

void nodes_init() {
    s_cam.init();
    xTaskCreatePinnedToCore(disc_task, "disc", 4096, nullptr, 2, nullptr, 0);
    s_jfree = xQueueCreate(2, sizeof(int));
    s_jfull = xQueueCreate(2, sizeof(int));
    for (int k = 0; k < 2; k++) {
        s_jpg[k].buf = (uint8_t *)ps_malloc_or_null(JCAP);
        if (s_jpg[k].buf) xQueueSend(s_jfree, &k, 0);
    }
    // Not in PSRAM: lwIP asserts that a task opening a TCP connection has an internal stack
    // (lwip_hook_tcp_isn, seen 2026-11-21). Only the decoder, which never touches the network, can.
    xTaskCreatePinnedToCore(cam_fetch_task, "cam", 8192, nullptr, 2, nullptr, 0);
    // Below the fetch, Wi-Fi and the sensor node: it only uses the time they leave.
    xTaskCreatePinnedToCoreWithCaps(cam_decode_task, "jpeg", 6144, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(node_task, "node", 8192, nullptr, 2, nullptr, 0);
}
