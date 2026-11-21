#include "aipnode.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <lwip/sockets.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>

static const uint32_t AP_AFTER_MS = 120 * 1000;      // no station for this long: open the setup AP
static const uint32_t REJOIN_MS = 20 * 1000;         // retry the station this often while it is down
static const uint32_t TRY_NET_MS = 30 * 1000;        // a network from POST /wifi has this long
static const uint32_t ANNOUNCE_MS = 5000;
static const uint32_t CONFIRM_AFTER_MS = 20 * 1000;
static const uint32_t CONFIRM_DEADLINE_MS = 120 * 1000;
static const int MAX_PEERS = 8;

static AipNodeConfig C;
static char s_host[40], s_ap_name[40];
static httpd_handle_t s_http;
static Preferences s_prefs;
static String s_ssid, s_pass;           // saved network
static String s_try_ssid, s_try_pass;   // network from POST /wifi, not saved yet
static volatile bool s_try_pending;
static uint32_t s_try_since;
static volatile bool s_up;
static volatile uint8_t s_last_reason;  // why the station last dropped (wifi_err_reason_t)
static bool s_ap_on;
static uint32_t s_down_since, s_last_join;
static bool s_pending;                  // this image waits for its self-check
static WiFiUDP s_udp;
static bool s_udp_on, s_ota_dev_on;
static AipPeer s_peers[MAX_PEERS];
static SemaphoreHandle_t s_mu;
static volatile bool s_reboot;

static volatile int s_ota_pct = -1;
static char s_ota_state[12] = "idle", s_ota_err[64] = "", s_ota_ver[24] = "";
static char s_ota_url[512], s_ota_sha[65];
static uint32_t s_ota_size;
static volatile bool s_ota_busy;

void aipnode_logf(const char *fmt, ...) {
    if (!C.log) return;
    char b[192];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    C.log->print(b);
}

// ---------------------------------------------------------------- Wi-Fi

static void load_net() {
    s_prefs.begin("net", true);
    s_ssid = s_prefs.getString("ssid", "");
    s_pass = s_prefs.getString("pass", "");
    s_prefs.end();
    if (!s_ssid.length()) {
        // smartfarm's camera kept its network in its own namespace.
        Preferences p;
        if (p.begin("cam", true)) {
            s_ssid = p.getString("ssid", "");
            s_pass = p.getString("pass", "");
            p.end();
        }
        if (s_ssid.length()) {
            s_prefs.begin("net", false);
            s_prefs.putString("ssid", s_ssid);
            s_prefs.putString("pass", s_pass);
            s_prefs.end();
            aipnode_logf("[node] moved the saved network from the old camera settings\n");
        }
    }
}

static void save_net(const String &ssid, const String &pass) {
    s_prefs.begin("net", false);
    s_prefs.putString("ssid", ssid);
    s_prefs.putString("pass", pass);
    s_prefs.end();
    s_ssid = ssid;
    s_pass = pass;
}

static void join(const String &ssid, const String &pass) {
    s_last_join = millis();
    if (!ssid.length()) return;
    aipnode_logf("[node] joining %s\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    if (C.tx_power) WiFi.setTxPower((wifi_power_t)C.tx_power);
}

static void ap_set(bool on) {
    if (on == s_ap_on) return;
    s_ap_on = on;
    if (on) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(s_ap_name, AIPNODE_AP_PASS);
        aipnode_logf("[node] setup AP %s at %s\n", s_ap_name, WiFi.softAPIP().toString().c_str());
    } else {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        aipnode_logf("[node] setup AP closed\n");
    }
}

static void on_wifi(arduino_event_id_t ev, arduino_event_info_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        s_up = true;
    } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED || ev == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
        if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) s_last_reason = info.wifi_sta_disconnected.reason;
        if (s_up) s_down_since = millis();
        s_up = false;
    }
}

bool aipnode_wifi_up() { return s_up; }
String aipnode_ip() { return s_up ? WiFi.localIP().toString() : String(""); }

// ---------------------------------------------------------------- discovery

static void announce() {
    char b[160];
    int n = snprintf(b, sizeof b, "{\"role\":\"%s\",\"name\":\"%s\",\"ver\":\"%s\",\"port\":80}", C.role, C.name, C.version);
    s_udp.beginPacket(WiFi.broadcastIP(), AIPNODE_UDP_PORT);
    s_udp.write((const uint8_t *)b, n);
    s_udp.endPacket();
}

static void listen() {
    char b[256];
    for (int k = 0; k < 8; k++) {
        int n = s_udp.parsePacket();
        if (n <= 0) return;
        n = s_udp.read(b, sizeof b - 1);
        if (n <= 0) continue;
        b[n] = 0;
        IPAddress from = s_udp.remoteIP();
        if (from == WiFi.localIP()) continue;
        JsonDocument d;
        if (deserializeJson(d, b)) continue;
        const char *role = d["role"] | "";
        if (!*role) continue;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        int slot = -1, oldest = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (s_peers[i].seen_ms && s_peers[i].ip == from) { slot = i; break; }
            if (s_peers[i].seen_ms < s_peers[oldest].seen_ms) oldest = i;
        }
        if (slot < 0) slot = oldest;
        AipPeer &p = s_peers[slot];
        strlcpy(p.role, role, sizeof p.role);
        strlcpy(p.name, d["name"] | role, sizeof p.name);
        strlcpy(p.ver, d["ver"] | "", sizeof p.ver);
        p.ip = from;
        p.port = d["port"] | 80;
        p.seen_ms = millis() | 1;
        xSemaphoreGive(s_mu);
    }
}

bool aipnode_peer(const char *role, AipPeer *out, uint32_t max_age_ms) {
    bool ok = false;
    uint32_t best = 0, now = millis();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_PEERS; i++) {
        const AipPeer &p = s_peers[i];
        if (!p.seen_ms || strcmp(p.role, role) || now - p.seen_ms > max_age_ms) continue;
        if (!ok || p.seen_ms - best < 0x80000000u) {
            *out = p;
            best = p.seen_ms;
            ok = true;
        }
    }
    xSemaphoreGive(s_mu);
    return ok;
}

// ---------------------------------------------------------------- OTA

extern "C" bool verifyRollbackLater() { return true; }

bool aipnode_ota_busy() { return s_ota_busy; }
int aipnode_ota_pct() { return s_ota_pct; }

static void ota_set(const char *state, const char *err = nullptr) {
    strlcpy(s_ota_state, state, sizeof s_ota_state);
    if (err) strlcpy(s_ota_err, err, sizeof s_ota_err);
}

static bool partition_sha_ok(const esp_partition_t *p, uint32_t size, const char *want) {
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool ok = true;
    for (uint32_t off = 0; off < size; off += 4096) {
        uint32_t n = size - off < 4096 ? size - off : 4096;
        if (esp_partition_read(p, off, buf, n) != ESP_OK) { ok = false; break; }
        mbedtls_sha256_update(&ctx, buf, n);
    }
    uint8_t d[32];
    mbedtls_sha256_finish(&ctx, d);
    mbedtls_sha256_free(&ctx);
    free(buf);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return ok && !strcasecmp(hex, want);
}

static void ota_task(void *) {
    char e[64] = "";
    aipnode_logf("[node] OTA %s (%u bytes) from %s\n", s_ota_ver, (unsigned)s_ota_size, s_ota_url);
    if (C.before_ota && !C.before_ota()) strlcpy(e, "refused", sizeof e);
    const esp_partition_t *dst = esp_ota_get_next_update_partition(nullptr);
    if (!e[0] && (!dst || s_ota_size > dst->size)) strlcpy(e, "image does not fit", sizeof e);
    esp_http_client_config_t hc = {};
    hc.url = s_ota_url;
    hc.timeout_ms = 20000;
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    hc.buffer_size = 4096;        // GitHub's last redirect is a ~900-character signed URL
    hc.buffer_size_tx = 4096;
    hc.max_redirection_count = 5;
    hc.keep_alive_enable = true;
    esp_https_ota_config_t oc = {};
    oc.http_config = &hc;
    esp_https_ota_handle_t h = nullptr;
    if (!e[0]) {
        ota_set("downloading");
        esp_err_t err = esp_https_ota_begin(&oc, &h);
        if (err != ESP_OK) snprintf(e, sizeof e, "begin: %s", esp_err_to_name(err));
    }
    if (!e[0]) {
        esp_err_t err;
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int pct = (int)((uint64_t)esp_https_ota_get_image_len_read(h) * 100 / s_ota_size);
            s_ota_pct = pct > 99 ? 99 : pct;
        }
        int got = esp_https_ota_get_image_len_read(h);
        if (err != ESP_OK) snprintf(e, sizeof e, "download: %s", esp_err_to_name(err));
        else if (!esp_https_ota_is_complete_data_received(h) || (uint32_t)got != s_ota_size)
            snprintf(e, sizeof e, "size %d/%u", got, (unsigned)s_ota_size);
    }
    if (!e[0]) {
        ota_set("verifying");
        if (!partition_sha_ok(dst, s_ota_size, s_ota_sha)) strlcpy(e, "sha256 mismatch", sizeof e);
    }
    if (!e[0]) {
        esp_err_t err = esp_https_ota_finish(h);
        h = nullptr;
        if (err != ESP_OK) snprintf(e, sizeof e, "image: %s", esp_err_to_name(err));
    }
    if (h) esp_https_ota_abort(h);
    if (e[0]) {
        aipnode_logf("[node] OTA failed: %s\n", e);
        ota_set("fail", e);
        s_ota_pct = -1;
        s_ota_busy = false;
        if (C.ota_failed) C.ota_failed();
        vTaskDelete(nullptr);
    }
    s_ota_pct = 100;
    ota_set("restarting", "");
    aipnode_logf("[node] OTA done, restarting\n");
    delay(800);
    esp_restart();
}

static void confirm_step() {
    if (!s_pending) return;
    uint32_t now = millis();
    if (now > CONFIRM_AFTER_MS && s_up && (!C.self_check || C.self_check())) {
        esp_ota_mark_app_valid_cancel_rollback();
        s_pending = false;
        aipnode_logf("[node] %s confirmed\n", C.version);
    } else if (now > CONFIRM_DEADLINE_MS) {
        aipnode_logf("[node] %s failed its self-check, going back\n", C.version);
        delay(200);
        esp_ota_mark_app_invalid_rollback_and_reboot();
        s_pending = false;   // no other image to go back to: keep running this one
    }
}

// ---------------------------------------------------------------- HTTP

esp_err_t aipnode_send_text(httpd_req_t *req, const char *status, const char *text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t aipnode_send_json(httpd_req_t *req, JsonDocument &doc, const char *status) {
    String s;
    serializeJson(doc, s);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, s.c_str(), s.length());
}

int aipnode_read_body(httpd_req_t *req, char *buf, size_t cap) {
    if (req->content_len >= cap) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = 0;
    return (int)got;
}

static void fill_info(JsonObject o) {
    o["role"] = C.role;
    o["name"] = C.name;
    o["ver"] = C.version;
    o["host"] = s_host;
    o["ip"] = aipnode_ip();
    o["ssid"] = s_ssid;
    o["rssi"] = s_up ? WiFi.RSSI() : 0;
    o["ap"] = s_ap_on;
    o["wifi_reason"] = s_last_reason;
    o["uptime_s"] = millis() / 1000;
    o["reset"] = (int)esp_reset_reason();
    o["heap"] = ESP.getFreeHeap();
    o["heap_min"] = ESP.getMinFreeHeap();
    o["pending"] = s_pending;
    JsonObject ota = o["ota"].to<JsonObject>();
    ota["state"] = s_ota_state;
    ota["pct"] = s_ota_pct;
    ota["err"] = s_ota_err;
    if (C.info_extra) C.info_extra(o);
}

static esp_err_t h_info(httpd_req_t *req) {
    JsonDocument d;
    fill_info(d.to<JsonObject>());
    return aipnode_send_json(req, d);
}

static esp_err_t h_wifi(httpd_req_t *req) {
    char b[256];
    JsonDocument d;
    if (aipnode_read_body(req, b, sizeof b) < 0 || deserializeJson(d, b)) return aipnode_send_text(req, "400 Bad Request", "need {\"ssid\",\"pass\"}");
    const char *ssid = d["ssid"] | "";
    if (!*ssid || strlen(ssid) > 32) return aipnode_send_text(req, "400 Bad Request", "bad ssid");
    s_try_ssid = ssid;
    s_try_pass = d["pass"] | "";
    s_try_pending = true;
    JsonDocument r;
    r["ok"] = true;
    r["trying"] = ssid;
    return aipnode_send_json(req, r);
}

static esp_err_t h_ota(httpd_req_t *req) {
    char b[768];
    JsonDocument d;
    if (s_ota_busy) return aipnode_send_text(req, "409 Conflict", "busy");
    if (aipnode_read_body(req, b, sizeof b) < 0 || deserializeJson(d, b)) return aipnode_send_text(req, "400 Bad Request", "need {\"url\",\"sha256\",\"size\"}");
    const char *url = d["url"] | "", *sha = d["sha256"] | "";
    uint32_t size = d["size"] | 0;
    if (strncmp(url, "https://", 8) || strlen(url) >= sizeof s_ota_url || strlen(sha) != 64 || !size)
        return aipnode_send_text(req, "400 Bad Request", "bad url, sha256 or size");
    strlcpy(s_ota_url, url, sizeof s_ota_url);
    strlcpy(s_ota_sha, sha, sizeof s_ota_sha);
    strlcpy(s_ota_ver, d["version"] | "", sizeof s_ota_ver);
    s_ota_size = size;
    s_ota_busy = true;
    s_ota_pct = 0;
    ota_set("starting", "");
    xTaskCreate(ota_task, "ota", 8192, nullptr, 2, nullptr);
    JsonDocument r;
    r["ok"] = true;
    return aipnode_send_json(req, r, "202 Accepted");
}

static esp_err_t h_reboot(httpd_req_t *req) {
    aipnode_send_text(req, "200 OK", "rebooting");
    s_reboot = true;
    return ESP_OK;
}

httpd_handle_t aipnode_http() { return s_http; }

// No Nagle: esp_http_server writes the headers and the body as separate sends, so with Nagle the body
// waited for the ACK of the headers, and the S3's lwIP delays ACKs on its 250 ms timer. A 4 KB camera
// frame took 248 ms to arrive at the S3 against 68 ms at the PC (2026-11-21).
static esp_err_t on_open(httpd_handle_t, int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return ESP_OK;
}

static void http_start() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.open_fn = on_open;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        aipnode_logf("[node] HTTP server did not start\n");
        return;
    }
    httpd_uri_t u = {};
    u.uri = "/info"; u.method = HTTP_GET; u.handler = h_info; httpd_register_uri_handler(s_http, &u);
    u.uri = "/wifi"; u.method = HTTP_POST; u.handler = h_wifi; httpd_register_uri_handler(s_http, &u);
    u.uri = "/ota"; u.method = HTTP_POST; u.handler = h_ota; httpd_register_uri_handler(s_http, &u);
    u.uri = "/reboot"; u.method = HTTP_GET; u.handler = h_reboot; httpd_register_uri_handler(s_http, &u);
}

// ---------------------------------------------------------------- console

bool aipnode_command(const char *line, Print &out) {
    if (!strncmp(line, "wifi ", 5)) {
        String rest = String(line + 5);
        rest.trim();
        int sp = rest.indexOf(' ');
        s_try_ssid = sp < 0 ? rest : rest.substring(0, sp);
        s_try_pass = sp < 0 ? String("") : rest.substring(sp + 1);
        s_try_pending = true;
        out.printf("trying %s\n", s_try_ssid.c_str());
        return true;
    }
    if (!strcmp(line, "info")) {
        JsonDocument d;
        fill_info(d.to<JsonObject>());
        serializeJson(d, out);
        out.println();
        return true;
    }
    if (!strcmp(line, "reboot")) {
        out.println("rebooting");
        s_reboot = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- task

static void task(void *) {
    uint32_t last_announce = 0;
    bool trying = false;
    for (;;) {
        uint32_t now = millis();
        if (s_reboot) { delay(300); ESP.restart(); }

        if (s_try_pending) {
            s_try_pending = false;
            trying = true;
            s_try_since = now;
            WiFi.disconnect();
            join(s_try_ssid, s_try_pass);
        }
        if (trying) {
            if (s_up && WiFi.SSID() == s_try_ssid) {
                save_net(s_try_ssid, s_try_pass);
                trying = false;
                aipnode_logf("[node] moved to %s\n", s_ssid.c_str());
            } else if (now - s_try_since > TRY_NET_MS) {
                trying = false;
                aipnode_logf("[node] %s did not answer, back to %s\n", s_try_ssid.c_str(), s_ssid.c_str());
                WiFi.disconnect();
                join(s_ssid, s_pass);
            }
        } else if (!s_up && now - s_last_join > REJOIN_MS) {
            join(s_ssid, s_pass);
        }

        // Setup AP: nothing saved, or the station has been down for 2 minutes. Closed once the station
        // is back and nobody is connected to the AP.
        if (!s_up && !s_ap_on && (!s_ssid.length() || now - s_down_since > AP_AFTER_MS)) ap_set(true);
        if (s_up && s_ap_on && WiFi.softAPgetStationNum() == 0) ap_set(false);

        if (s_up) {
            if (!s_udp_on) s_udp_on = s_udp.begin(AIPNODE_UDP_PORT);
            if (!s_ota_dev_on) {
                ArduinoOTA.setHostname(s_host);
                ArduinoOTA.onStart([]() { s_ota_busy = true; ota_set("espota"); });
                ArduinoOTA.onError([](ota_error_t) { s_ota_busy = false; ota_set("fail", "espota"); });
                ArduinoOTA.begin();
                s_ota_dev_on = true;
                aipnode_logf("[node] %s.local %s\n", s_host, aipnode_ip().c_str());
            }
            ArduinoOTA.handle();
            if (s_udp_on) {
                listen();
                if (now - last_announce > ANNOUNCE_MS && !s_ota_busy) {
                    last_announce = now;
                    announce();
                }
            }
        }
        confirm_step();
        vTaskDelay(pdMS_TO_TICKS(s_ota_busy ? 2 : 50));
    }
}

void aipnode_begin(const AipNodeConfig &cfg) {
    C = cfg;
    s_mu = xSemaphoreCreateMutex();
    snprintf(s_host, sizeof s_host, "aiplantrx-%s", C.name);
    snprintf(s_ap_name, sizeof s_ap_name, "AIPLANT-%s", C.name);

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    s_pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_pending) aipnode_logf("[node] %s is pending, self-check\n", C.version);

    load_net();
    WiFi.persistent(false);
    WiFi.onEvent(on_wifi);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_host);
    WiFi.setSleep(false);
    if (C.tx_power) WiFi.setTxPower((wifi_power_t)C.tx_power);
    s_down_since = millis();
    join(s_ssid, s_pass);
    http_start();
    xTaskCreate(task, "aipnode", 6144, nullptr, 2, nullptr);
}
