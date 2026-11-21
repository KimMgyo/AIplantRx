#include "fwupdate.h"
#include "identify.h"
#include "nodes.h"
#include "app.h"
#include "lvgl_v8_port.h"
#include "net.h"
#include "settings.h"
#include "ui.h"
#include <ArduinoJson.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <time.h>

static const uint32_t FIRST_CHECK_MS = 60 * 1000;
static const uint32_t CHECK_EVERY_MS = 6UL * 3600 * 1000;
static const uint32_t RETRY_MS = 30UL * 60 * 1000;          // after a failed check
static const uint32_t IDLE_FOR_AUTO_MS = 10UL * 60 * 1000;  // untouched panel before an automatic install
static const uint32_t CONFIRM_AFTER_MS = 20 * 1000;         // UI and Wi-Fi up at least this long after boot
static const uint32_t CONFIRM_DEADLINE_MS = 120 * 1000;     // else back to the previous image

static FwuInfo s_info;
static volatile uint32_t s_gen;
static volatile bool s_req_check, s_req_install;
static SemaphoreHandle_t s_mu;

// What the manifest offers for this board.
static char s_url[512], s_sha[65];
static uint32_t s_size;

static void set_state(FwuState st, const char *err = nullptr) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_info.state = st;
    if (err) strlcpy(s_info.err, err, sizeof s_info.err);
    xSemaphoreGive(s_mu);
    s_gen++;
}

FwuInfo fwu_info() {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    FwuInfo i = s_info;
    xSemaphoreGive(s_mu);
    return i;
}
uint32_t fwu_gen() { return s_gen; }
void fwu_check_now() { s_req_check = true; }
void fwu_install_now() { s_req_install = true; }

// ---------------------------------------------------------------- versions

static void ver_parse(const char *s, long n[3], const char **suffix) {
    n[0] = n[1] = n[2] = 0;
    if (*s == 'v' || *s == 'V') s++;
    for (int k = 0; k < 3; k++) {
        char *e;
        n[k] = strtol(s, &e, 10);
        s = e;
        if (*s != '.') break;
        s++;
    }
    while (*s && *s != '-' && *s != '+') s++;
    *suffix = *s == '-' ? s + 1 : "";
}

int fwu_vercmp(const char *a, const char *b) {
    long x[3], y[3];
    const char *sa, *sb;
    ver_parse(a, x, &sa);
    ver_parse(b, y, &sb);
    for (int k = 0; k < 3; k++)
        if (x[k] != y[k]) return x[k] < y[k] ? -1 : 1;
    if (!*sa != !*sb) return *sa ? -1 : 1;  // 2.0.1-dev < 2.0.1
    return strcmp(sa, sb);
}

// ---------------------------------------------------------------- self-check and rollback

// Arduino confirms a new image at boot unless this says "later" (esp32-hal-misc.c). We confirm it
// ourselves once the UI and Wi-Fi are up, so an image that boots but cannot get back on the network
// (and so could never be updated again) also goes back to the previous one.
extern "C" bool verifyRollbackLater() { return true; }

static const esp_partition_t *s_bad;   // the image the bootloader refused, if any

static void boot_state() {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    s_info.pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    s_bad = esp_ota_get_last_invalid_partition();
}

// Which version the refused image was. The app descriptor's version is the framework's, so look for our
// User-Agent string ("AIplantRx-S3/<version>", see http_cfg), which every image carries in its rodata.
static void find_rolled_back() {
    if (!s_bad) return;
    static const char TAG[] = "AIplantRx-S3/";
    const size_t TL = sizeof TAG - 1;
    char *buf = (char *)ps_malloc_or_null(4096 + 64);
    if (!buf) return;
    char v[24] = "?";
    bool found = false;
    for (uint32_t off = 0; !found && off + 4096 + 64 <= s_bad->size && off < 0x600000; off += 4096) {
        if (esp_partition_read(s_bad, off, buf, 4096 + 64) != ESP_OK) break;
        // The bare TAG literal (this function's own) is in the image too; it is followed by NUL.
        for (char *p = buf; !found && (p = (char *)memmem(p, buf + 4096 + TL - p, TAG, TL)); p += TL) {
            const char *s = p + TL;
            int n = 0;
            while (n < (int)sizeof v - 1 && s + n < buf + 4096 + 64 && s[n] > ' ' && s[n] < 127) n++;
            if (n && (s + n >= buf + 4096 + 64 || s[n] == 0)) {
                memcpy(v, s, n);
                v[n] = 0;
                found = true;
            }
        }
    }
    free(buf);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strlcpy(s_info.rolled_back, v, sizeof s_info.rolled_back);
    xSemaphoreGive(s_mu);
    s_gen++;
    Serial.printf("[fwu] last boot refused %s\n", v);
}

static void confirm_if_pending() {
    if (!s_info.pending) return;
    Serial.printf("[fwu] %s is pending, self-check\n", FW_VERSION);
#ifdef FWU_TEST_NO_CONFIRM
    // Test build: never confirms, to exercise the way back.
    while (millis() < CONFIRM_DEADLINE_MS) vTaskDelay(pdMS_TO_TICKS(1000));
#else
    while (millis() < CONFIRM_DEADLINE_MS) {
        if (millis() > CONFIRM_AFTER_MS && net_state() == NET_UP) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_info.pending = false;
            s_gen++;
            Serial.printf("[fwu] %s confirmed\n", FW_VERSION);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    Serial.printf("[fwu] %s failed its self-check, going back\n", FW_VERSION);
    delay(200);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

// ---------------------------------------------------------------- manifest

struct Body {
    char *buf;
    size_t len, cap;
};

static esp_err_t on_http(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        Body *b = (Body *)e->user_data;
        // Bodies of redirect responses arrive here too; only the final 200 counts.
        if (esp_http_client_get_status_code(e->client) != 200) return ESP_OK;
        size_t n = e->data_len;
        if (b->len + n >= b->cap) n = b->cap - 1 - b->len;
        memcpy(b->buf + b->len, e->data, n);
        b->len += n;
        b->buf[b->len] = 0;
    }
    return ESP_OK;
}

static void http_cfg(esp_http_client_config_t &c, const char *url) {
    memset(&c, 0, sizeof c);
    c.url = url;
    c.timeout_ms = 20000;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    // GitHub answers with two redirects; the last one (release-assets.githubusercontent.com) is about
    // 900 characters of signed query string, and it has to fit in a header line and the request line.
    c.buffer_size = 4096;
    c.buffer_size_tx = 4096;
    c.max_redirection_count = 5;
    c.keep_alive_enable = true;
    c.user_agent = "AIplantRx-S3/" FW_VERSION;
}

static bool check() {
    set_state(FWU_CHECKING);
    if (net_state() != NET_UP) { set_state(FWU_FAIL, "Wi-Fi 없음"); return false; }
    Body b = {(char *)ps_malloc_or_null(16384), 0, 16384};
    if (!b.buf) { set_state(FWU_FAIL, "메모리 부족"); return false; }
    b.buf[0] = 0;
    esp_http_client_config_t c;
    http_cfg(c, FWU_MANIFEST_URL);
    c.event_handler = on_http;
    c.user_data = &b;
    if (!tls_lock(60000)) { free(b.buf); set_state(FWU_FAIL, "다른 연결이 끝나지 않음"); return false; }
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t err = esp_http_client_perform(h);
    int code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    tls_unlock();
    char e[64] = "";
    bool ok = false;
    if (err != ESP_OK) snprintf(e, sizeof e, "연결 실패 · %s", esp_err_to_name(err));
    else if (code == 404) strlcpy(e, "릴리스 없음", sizeof e);
    else if (code != 200) snprintf(e, sizeof e, "HTTP %d", code);
    else {
        JsonDocument doc;
        if (deserializeJson(doc, b.buf)) strlcpy(e, "manifest 형식 오류", sizeof e);
        else {
            JsonObject d = doc["devices"]["s3"];
            const char *v = d["version"] | "", *u = d["url"] | "", *sh = d["sha256"] | "";
            uint32_t sz = d["size"] | 0;
            if (!*v || !*u || strlen(sh) != 64 || !sz) strlcpy(e, "manifest에 S3 없음", sizeof e);
            else {
                strlcpy(s_url, u, sizeof s_url);
                strlcpy(s_sha, sh, sizeof s_sha);
                s_size = sz;
                xSemaphoreTake(s_mu, portMAX_DELAY);
                strlcpy(s_info.latest, v, sizeof s_info.latest);
                time_t now = time(nullptr);
                s_info.checked = net_time_ok() ? (uint32_t)now : 0;
                s_info.err[0] = 0;
                xSemaphoreGive(s_mu);
                ok = true;
            }
        }
    }
    free(b.buf);
    if (!ok) {
        Serial.printf("[fwu] check failed: %s\n", e);
        set_state(FWU_FAIL, e);
        return false;
    }
    bool newer = fwu_vercmp(s_info.latest, FW_VERSION) > 0;
    Serial.printf("[fwu] manifest s3 %s (running %s)%s\n", s_info.latest, FW_VERSION, newer ? ", newer" : "");
    set_state(newer ? FWU_AVAILABLE : FWU_LATEST);
    return true;
}

// ---------------------------------------------------------------- install

static void progress_ui(int pct, const char *msg) {
    if (!lvgl_port_lock(200)) return;
    ui_update_overlay(s_info.latest, pct, msg);
    lvgl_port_unlock();
}

static bool partition_sha_ok(const esp_partition_t *p, uint32_t size) {
    uint8_t *buf = (uint8_t *)ps_malloc_or_null(4096);
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
    if (!ok) return false;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    Serial.printf("[fwu] sha256 %s, manifest %s\n", hex, s_sha);
    return !strcasecmp(hex, s_sha);
}

static void install() {
    if (s_info.state != FWU_AVAILABLE) return;
    Serial.printf("[fwu] installing %s (%u bytes)\n", s_info.latest, (unsigned)s_size);
    cam_set_wanted(false);   // free core 0 and the radio for the download
    nodes_pause(true);
    s_info.pct = 0;
    set_state(FWU_INSTALLING);
    progress_ui(0, "받는 중");
    const esp_partition_t *dst = esp_ota_get_next_update_partition(nullptr);
    char e[64] = "";
    if (!dst || s_size > dst->size) strlcpy(e, "OTA 칸 없음", sizeof e);
    esp_http_client_config_t hc;
    http_cfg(hc, s_url);
    esp_https_ota_config_t oc = {};
    oc.http_config = &hc;
    esp_https_ota_handle_t h = nullptr;
    uint32_t t0 = millis();
    bool locked = !e[0] && tls_lock(60000);
    if (!e[0] && !locked) strlcpy(e, "다른 연결이 끝나지 않음", sizeof e);
    if (!e[0]) {
        esp_err_t err = esp_https_ota_begin(&oc, &h);
        if (err != ESP_OK) snprintf(e, sizeof e, "받기 시작 실패 · %s", esp_err_to_name(err));
    }
    if (!e[0]) {
        int last = -1;
        esp_err_t err;
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int pct = (int)((uint64_t)esp_https_ota_get_image_len_read(h) * 100 / s_size);
            if (pct > 99) pct = 99;
            if (pct != last) {
                last = pct;
                s_info.pct = pct;
                s_gen++;
                progress_ui(pct, "받는 중");
            }
        }
        int got = esp_https_ota_get_image_len_read(h);
        if (err != ESP_OK) snprintf(e, sizeof e, "받는 중 끊김 · %s", esp_err_to_name(err));
        else if (!esp_https_ota_is_complete_data_received(h) || (uint32_t)got != s_size)
            snprintf(e, sizeof e, "크기 다름 · %d/%u", got, (unsigned)s_size);
    }
    if (!e[0]) {
        progress_ui(100, "확인 중");
        if (!partition_sha_ok(dst, s_size)) strlcpy(e, "SHA-256 다름", sizeof e);
    }
    if (!e[0]) {
        esp_err_t err = esp_https_ota_finish(h);   // checks the image and switches the boot partition
        h = nullptr;
        if (err != ESP_OK) snprintf(e, sizeof e, "이미지 확인 실패 · %s", esp_err_to_name(err));
    }
    if (h) esp_https_ota_abort(h);
    if (locked) tls_unlock();
    if (e[0]) {
        Serial.printf("[fwu] install failed: %s\n", e);
        set_state(FWU_FAIL, e);
        nodes_pause(false);
        cam_set_wanted(true);
        if (lvgl_port_lock(200)) { ui_update_overlay_close(); lvgl_port_unlock(); }
        return;
    }
    Serial.printf("[fwu] %s written in %.1f s, restarting\n", s_info.latest, (millis() - t0) / 1000.0f);
    set_state(FWU_RESTARTING);
    progress_ui(100, "다시 시작하는 중");
    delay(1500);
    esp_restart();
}

// ---------------------------------------------------------------- task

static bool idle_on_main() {
    if (!lvgl_port_lock(200)) return false;
    bool ok = ui_main_idle(IDLE_FOR_AUTO_MS);
    lvgl_port_unlock();
    return ok;
}

static void task(void *) {
    confirm_if_pending();
    find_rolled_back();
    uint32_t next = FIRST_CHECK_MS;
    for (;;) {
        if (s_req_install) { s_req_install = false; install(); }
        // Not while a plant is being found: two TLS connections do not fit in internal RAM.
        if (!s_req_check && (int32_t)(millis() - next) >= 0 && find_busy()) next = millis() + 60000;
        if (s_req_check || (int32_t)(millis() - next) >= 0) {
            s_req_check = false;
            bool ok = check();
            next = millis() + (ok ? CHECK_EVERY_MS : RETRY_MS);
        }
        if (s_info.state == FWU_AVAILABLE && g_set.auto_update && idle_on_main()) {
            Serial.println("[fwu] panel idle on the main screen, installing");
            install();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void fwu_init() {
    s_mu = xSemaphoreCreateMutex();
    boot_state();
    // TLS (mbedTLS handshake) runs on this stack.
    xTaskCreatePinnedToCore(task, "fwu", 10240, nullptr, 1, nullptr, 0);
}

void fwu_json(char *out, size_t n) {
    static const char *NAMES[] = {"idle", "checking", "latest", "available", "installing", "restarting", "fail"};
    FwuInfo i = fwu_info();
    snprintf(out, n, "{\"state\":\"%s\",\"latest\":\"%s\",\"pct\":%d,\"pending\":%s,\"rolled_back\":\"%s\",\"auto\":%s,\"checked\":%lu,\"err\":\"%s\"}",
             NAMES[i.state], i.latest, i.pct, i.pending ? "true" : "false", i.rolled_back, g_set.auto_update ? "true" : "false",
             (unsigned long)i.checked, i.err);
}
