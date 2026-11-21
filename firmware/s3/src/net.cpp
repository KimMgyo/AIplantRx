#include "net.h"
#include "app.h"
#include "settings.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_tls.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/platform.h>
#include <time.h>

static volatile NetState s_state = NET_IDLE;
static volatile uint32_t s_gen = 1;
static SemaphoreHandle_t s_mx;
static ApInfo s_aps[24];
static int s_nap = 0;
static volatile bool s_scan_req = false, s_scanning = false;
static String s_join_ssid, s_join_pass;
static volatile bool s_join_req = false;
static uint32_t s_join_t0 = 0;
static bool s_trial = false;          // joining a network that is not saved yet
static bool s_mdns = false;

static void bump() { s_gen = s_gen + 1; }

NetState net_state() { return s_state; }
uint32_t net_gen() { return s_gen; }
bool net_scanning() { return s_scanning || s_scan_req; }
int net_rssi() { return s_state == NET_UP ? WiFi.RSSI() : 0; }
String net_ip() { return s_state == NET_UP ? WiFi.localIP().toString() : String(""); }
bool net_time_ok() { return time(nullptr) > 1700000000; }

String net_ssid() {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    String s = s_trial ? s_join_ssid : g_set.ssid;
    xSemaphoreGive(s_mx);
    return s;
}

int net_aps(ApInfo *out, int max) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int n = s_nap < max ? s_nap : max;
    memcpy(out, s_aps, n * sizeof(ApInfo));
    xSemaphoreGive(s_mx);
    return n;
}

void net_scan() { s_scan_req = true; }

void net_set_enabled(bool on) {
    g_set.wifi_on = on;
    settings_save_wifi();
    if (!on) {
        WiFi.disconnect(true);
        s_state = NET_OFF;
    } else {
        s_state = NET_IDLE;
    }
    bump();
}

void net_join(const char *ssid, const char *pass) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_join_ssid = ssid;
    s_join_pass = pass ? pass : "";
    xSemaphoreGive(s_mx);
    s_join_req = true;
}

static void begin(const String &ssid, const String &pass) {
    WiFi.disconnect(false, false);
    WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
    s_join_t0 = millis();
    s_state = NET_JOINING;
    bump();
}

static void on_up() {
    if (s_trial) {
        // The new network works: it becomes the saved one.
        xSemaphoreTake(s_mx, portMAX_DELAY);
        g_set.ssid = s_join_ssid;
        g_set.pass = s_join_pass;
        s_trial = false;
        xSemaphoreGive(s_mx);
        settings_save_wifi();
    }
    configTzTime(APP_TZ, "pool.ntp.org", "time.google.com", "kr.pool.ntp.org");
    if (!s_mdns) s_mdns = MDNS.begin(APP_HOST);
    if (s_mdns) MDNS.addService("http", "tcp", 80);
    s_state = NET_UP;
    bump();
    Serial.printf("[net] up %s %s rssi=%d\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static void collect_scan(int n) {
    ApInfo tmp[24];
    int k = 0;
    String cur = net_ssid();
    for (int i = 0; i < n && k < 24; i++) {
        String s = WiFi.SSID(i);
        if (!s.length() || (s == cur && s_state == NET_UP)) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) if (s == tmp[j].ssid) { dup = true; break; }
        if (dup) continue;
        strlcpy(tmp[k].ssid, s.c_str(), sizeof(tmp[k].ssid));
        tmp[k].rssi = WiFi.RSSI(i);
        tmp[k].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        k++;
    }
    // strongest first
    for (int a = 1; a < k; a++)
        for (int b = a; b > 0 && tmp[b].rssi > tmp[b - 1].rssi; b--) { ApInfo t = tmp[b]; tmp[b] = tmp[b - 1]; tmp[b - 1] = t; }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memcpy(s_aps, tmp, k * sizeof(ApInfo));
    s_nap = k;
    xSemaphoreGive(s_mx);
    WiFi.scanDelete();
}

static void net_task(void *) {
    uint32_t last_try = 0;
    for (;;) {
        wl_status_t st = WiFi.status();
        if (!g_set.wifi_on) {
            if (s_state != NET_OFF) { s_state = NET_OFF; bump(); }
        } else if (s_join_req) {
            s_join_req = false;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            String ss = s_join_ssid, pp = s_join_pass;
            s_trial = ss != g_set.ssid || pp != g_set.pass;
            xSemaphoreGive(s_mx);
            begin(ss, pp);
            last_try = millis();
        } else if (st == WL_CONNECTED) {
            if (s_state != NET_UP) on_up();
        } else {
            if (s_state == NET_UP) { s_state = NET_IDLE; bump(); Serial.println("[net] down"); }
            if (s_state == NET_JOINING && millis() - s_join_t0 > 15000) {
                // Gave up on this attempt. A trial network falls back to the saved one.
                if (s_trial) { xSemaphoreTake(s_mx, portMAX_DELAY); s_trial = false; xSemaphoreGive(s_mx); }
                s_state = NET_IDLE;
                bump();
            }
            if (s_state == NET_IDLE && g_set.ssid.length() && millis() - last_try > 20000 && !s_scanning) {
                begin(g_set.ssid, g_set.pass);
                last_try = millis();
            }
        }
        if (s_scan_req && !s_scanning && g_set.wifi_on) {
            s_scan_req = false;
            if (WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING) { s_scanning = true; bump(); }
        }
        if (s_scanning) {
            int n = WiFi.scanComplete();
            if (n >= 0 || n == WIFI_SCAN_FAILED) {
                if (n > 0) collect_scan(n);
                s_scanning = false;
                bump();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// mbedTLS memory. The prebuilt library allocates everything in internal RAM (CONFIG_MBEDTLS_INTERNAL_MEM_
// ALLOC), and a TLS connection needs two ~16.7 KB contiguous record buffers there. With the camera stream
// running, internal RAM was 75 KB free but no two such blocks: mbedtls_ssl_setup -0x7F00, the GitHub
// check failed (2026-11-21). The library keeps MBEDTLS_PLATFORM_MEMORY, so the allocator can be swapped
// at run time: big blocks (record buffers, certificate chains) go to PSRAM, small ones (bignums, contexts)
// stay internal where the handshake math is fast. IDF supports TLS buffers in PSRAM on the S3
// (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC); the AES/SHA DMA drivers bounce external buffers themselves.
static const size_t TLS_PSRAM_FROM = 4096;
static volatile bool s_tls_psram = true;   // tls_probe() can turn it off to compare

static void *tls_calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = nullptr;
    if (s_tls_psram && total >= TLS_PSRAM_FROM) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p && total < TLS_PSRAM_FROM) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}

static void tls_free(void *p) { heap_caps_free(p); }

void net_init() {
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    s_mx = xSemaphoreCreateMutex();
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(APP_HOST);
    WiFi.setSleep(false);
    s_state = g_set.wifi_on ? NET_IDLE : NET_OFF;
    if (g_set.wifi_on && g_set.ssid.length()) begin(g_set.ssid, g_set.pass);
    xTaskCreatePinnedToCore(net_task, "net", 4096, nullptr, 3, nullptr, 0);
}

static SemaphoreHandle_t s_tls;

bool tls_lock(uint32_t wait_ms) {
    if (!s_tls) {
        static portMUX_TYPE m = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&m);
        if (!s_tls) s_tls = xSemaphoreCreateMutex();
        portEXIT_CRITICAL(&m);
    }
    bool ok = xSemaphoreTake(s_tls, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
    // Internal RAM when a TLS connection starts (its big buffers are in PSRAM since 2.1.2, see tls_calloc).
    Serial.printf("[tls] %s, internal free %u, largest %u\n", ok ? "lock" : "busy",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ok;
}

void tls_unlock() { xSemaphoreGive(s_tls); }

// ---------------------------------------------------------------- TLS probe (GET /tlstest, devsrv.cpp)
// Times DNS, a bare TCP connect and a full esp-tls connection (DNS + TCP + handshake, certificate checked
// against the bundle) to host:port, to find where an S3 HTTPS request spends its time.

struct Probe {
    const char *host;
    uint16_t port;
    bool psram;
    char out[320];
    SemaphoreHandle_t done;
};

static void probe_task(void *arg) {
    Probe *p = (Probe *)arg;
    bool old = s_tls_psram;
    s_tls_psram = p->psram;
    uint32_t t0 = millis();
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(p->host, nullptr, &hints, &res);
    uint32_t dns = millis() - t0;
    char first[48] = "", v4[16] = "";
    int naddr = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next, naddr++) {
        if (!first[0]) strlcpy(first, a->ai_family == AF_INET6 ? "ipv6" : "ipv4", sizeof first);
        if (a->ai_family == AF_INET && !v4[0]) inet_ntoa_r(((sockaddr_in *)a->ai_addr)->sin_addr, v4, sizeof v4);
    }
    if (res) freeaddrinfo(res);
    int tcp = -1;
    if (!gai && v4[0]) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(p->port);
        inet_aton(v4, &a.sin_addr);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        uint32_t t1 = millis();
        if (s >= 0 && connect(s, (sockaddr *)&a, sizeof a) == 0) tcp = millis() - t1;
        if (s >= 0) close(s);
    }
    size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 40000;
    uint32_t t2 = millis();
    esp_tls_t *tls = esp_tls_init();
    int r = tls ? esp_tls_conn_new_sync(p->host, strlen(p->host), p->port, &cfg, tls) : -1;
    uint32_t full = millis() - t2;
    size_t int_during = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    char peer[48] = "";
    if (r == 1) {
        int fd = -1;
        esp_tls_get_conn_sockfd(tls, &fd);
        sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        if (fd >= 0 && getpeername(fd, (sockaddr *)&ss, &sl) == 0) {
            if (ss.ss_family == AF_INET) inet_ntoa_r(((sockaddr_in *)&ss)->sin_addr, peer, sizeof peer);
            else strlcpy(peer, "ipv6", sizeof peer);
        }
    }
    if (tls) esp_tls_conn_destroy(tls);
    s_tls_psram = old;
    snprintf(p->out, sizeof p->out,
             "{\"host\":\"%s\",\"psram\":%s,\"dns_ms\":%lu,\"addrs\":%d,\"first\":\"%s\",\"tcp_ms\":%d,"
             "\"tls_ok\":%s,\"tls_total_ms\":%lu,\"peer\":\"%s\",\"internal_used_by_tls\":%d}",
             p->host, p->psram ? "true" : "false", (unsigned long)dns, naddr, first, tcp, r == 1 ? "true" : "false",
             (unsigned long)full, peer, (int)int_before - (int)int_during);
    xSemaphoreGive(p->done);
    vTaskDelete(nullptr);
}

String tls_probe(const char *host, uint16_t port, bool psram) {
    if (!tls_lock(30000)) return "{\"err\":\"TLS busy\"}";
    Probe p = {host, port, psram, "", xSemaphoreCreateBinary()};
    // Same stack and core as the LLM's tasks (core 0, internal stack: lwIP refuses PSRAM stacks).
    xTaskCreatePinnedToCore(probe_task, "tlsprobe", 12288, &p, 2, nullptr, 0);
    xSemaphoreTake(p.done, portMAX_DELAY);
    vSemaphoreDelete(p.done);
    tls_unlock();
    return String(p.out);
}
