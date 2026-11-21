#include "net.h"
#include "app.h"
#include "settings.h"
#include <ESPmDNS.h>
#include <WiFi.h>
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

void net_init() {
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
    // mbedTLS needs two ~16 KB contiguous buffers in internal RAM: log what is there when it starts.
    Serial.printf("[tls] %s, internal free %u, largest %u\n", ok ? "lock" : "busy",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ok;
}

void tls_unlock() { xSemaphoreGive(s_tls); }
