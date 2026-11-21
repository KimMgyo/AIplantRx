#include "devsrv.h"
#include "fwupdate.h"
#include "nodes.h"
#include "app.h"
#include "lvgl_v8_port.h"
#include "net.h"
#include "perf.h"
#include "settings.h"
#include "ui.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_rom_sys.h>
#include <soc/rtc_cntl_reg.h>

static WebServer s_srv(80);
static volatile int s_ota = -1;

int devsrv_ota_progress() { return s_ota; }

// ---------------------------------------------------------------- injected touch

static volatile bool s_inj = false;
static volatile int s_x0, s_y0, s_x1, s_y1;
static volatile uint32_t s_t0, s_dur;
static int s_lx, s_ly;

static void inj_read(lv_indev_drv_t *, lv_indev_data_t *d) {
    if (s_inj) {
        uint32_t el = millis() - s_t0;
        if (el >= s_dur) {
            s_inj = false;
            d->state = LV_INDEV_STATE_REL;
        } else {
            float f = s_dur ? (float)el / s_dur : 1;
            s_lx = s_x0 + (s_x1 - s_x0) * f;
            s_ly = s_y0 + (s_y1 - s_y0) * f;
            d->state = LV_INDEV_STATE_PR;
        }
    } else d->state = LV_INDEV_STATE_REL;
    d->point.x = s_lx;
    d->point.y = s_ly;
}

void devsrv_attach_indev() {
    static lv_indev_drv_t drv;
    lv_indev_drv_init(&drv);
    drv.type = LV_INDEV_TYPE_POINTER;
    drv.read_cb = inj_read;
    lv_indev_drv_register(&drv);
}

static void inject(int x0, int y0, int x1, int y1, uint32_t ms) {
    while (s_inj) vTaskDelay(pdMS_TO_TICKS(10));
    s_lx = x0; s_ly = y0;
    s_x0 = x0; s_y0 = y0; s_x1 = x1; s_y1 = y1;
    s_dur = ms;
    s_t0 = millis();
    s_inj = true;
}

static int arg(const char *k, int d) { return s_srv.hasArg(k) ? s_srv.arg(k).toInt() : d; }

// ---------------------------------------------------------------- handlers

static void h_info() {
    // Why the last boot happened and how close each task came to its stack end, so a crash can be told
    // apart from a power dip without a USB cable (1 power-on, 3 software, 4 panic, 5/6/7 watchdogs,
    // 9 brownout).
    static const char *TASKS[] = {"lvgl", "cam", "jpeg", "node", "disc", "net", "dev", "llm", "fwu", "find", "ai"};
    char st[200] = "";
    int k = 0;
    for (const char *t : TASKS) {
        TaskHandle_t h = xTaskGetHandle(t);
        if (h) k += snprintf(st + k, sizeof st - k, "%s\"%s\":%u", k ? "," : "", t, (unsigned)uxTaskGetStackHighWaterMark(h));
    }
    char fw[200];
    fwu_json(fw, sizeof fw);
    char b[860];
    snprintf(b, sizeof b,
             "{\"name\":\"%s\",\"ver\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"heap_int\":%u,\"heap_int_min\":%u,\"psram\":%u,\"psram_big\":%u,\"uptime_s\":%lu,"
             "\"reset\":%d,\"stack_free\":{%s},\"fw\":%s}",
             APP_HOST, FW_VERSION, net_ip().c_str(), net_rssi(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned long)(millis() / 1000), (int)esp_reset_reason(), st, fw);
    s_srv.send(200, "application/json", b);
}

// GET /update            update state (same as "fw" in /info)
// GET /update?check=1    ask GitHub now
// GET /update?install=1  install what the last check found
static void h_update() {
    if (s_srv.hasArg("check")) fwu_check_now();
    if (s_srv.hasArg("install")) fwu_install_now();
    char fw[200];
    fwu_json(fw, sizeof fw);
    s_srv.send(200, "application/json", fw);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static const size_t FRAME_BYTES = SCR_W * SCR_H * 2;

// A copy of what the panel shows, freed by the caller (768 KB, held only while it is sent). In direct
// mode LVGL swaps buf_act after each finished refresh, so the buffer on the panel is the other one.
static uint16_t *snapshot() {
    uint16_t *copy = (uint16_t *)ps_malloc_or_null(FRAME_BYTES);
    if (!copy) return nullptr;
    lvgl_port_lock(-1);
    lv_disp_draw_buf_t *db = lv_disp_get_default()->driver->draw_buf;
    const void *front = db->buf2 ? (db->buf_act == db->buf1 ? db->buf2 : db->buf1) : db->buf1;
    memcpy(copy, front, FRAME_BYTES);
    lvgl_port_unlock();
    return copy;
}

static void h_screen() {
    const size_t N = FRAME_BYTES;
    uint16_t *copy = snapshot();
    if (!copy) { s_srv.send(500, "text/plain", "no memory"); return; }
    uint8_t h[66] = {'B', 'M'};
    put32(h + 2, sizeof(h) + N);
    put32(h + 10, sizeof(h));
    put32(h + 14, 40);
    put32(h + 18, SCR_W);
    put32(h + 22, (uint32_t)(-SCR_H));     // top-down
    put16(h + 26, 1);
    put16(h + 28, 16);
    put32(h + 30, 3);                      // BI_BITFIELDS
    put32(h + 34, N);
    put32(h + 54, 0xF800);
    put32(h + 58, 0x07E0);
    put32(h + 62, 0x001F);
    WiFiClient c = s_srv.client();
    s_srv.setContentLength(sizeof(h) + N);
    s_srv.send(200, "image/bmp", "");
    c.write(h, sizeof(h));
    const uint8_t *p = (const uint8_t *)copy;
    for (size_t off = 0; off < N;) {
        size_t n = N - off < 8192 ? N - off : 8192;
        size_t w = c.write(p + off, n);
        if (!w) break;
        off += w;
    }
    free(copy);
}

static void h_tap() {
    inject(arg("x", 0), arg("y", 0), arg("x", 0), arg("y", 0), arg("ms", 120));
    s_srv.send(200, "text/plain", "ok");
}

static void h_swipe() {
    inject(arg("x", 0), arg("y", 0), arg("x2", 0), arg("y2", 0), arg("ms", 400));
    s_srv.send(200, "text/plain", "ok");
}

static void h_reboot() {
    s_srv.send(200, "text/plain", "ok");
    delay(200);
    ESP.restart();
}

static void h_perf() { s_srv.send(200, "application/json", perf_json(s_srv.hasArg("reset"))); }

// Settings that are long to type on the panel: AI server address and key, PlantNet key.
static void h_set() {
    bool any = false;
    if (s_srv.hasArg("llm_url")) { g_set.llm_url = s_srv.arg("llm_url"); any = true; }
    if (s_srv.hasArg("llm_key")) { g_set.llm_key = s_srv.arg("llm_key"); any = true; }
    if (s_srv.hasArg("pn_key")) { g_set.plantnet_key = s_srv.arg("pn_key"); any = true; }
    if (any) settings_save();
    // Screen and theme, for scripted tests (tools/perf_run.py).
    if (s_srv.hasArg("theme")) {
        lvgl_port_lock(-1);
        ui_set_theme((uint8_t)s_srv.arg("theme").toInt());
        lvgl_port_unlock();
        any = true;
    }
    if (s_srv.hasArg("screen")) {
        lvgl_port_lock(-1);
        if (s_srv.arg("screen") == "main") ui_go_main();
        else ui_go_settings(s_srv.hasArg("page") ? s_srv.arg("page").toInt() : 0);
        lvgl_port_unlock();
        any = true;
    }
    s_srv.send(200, "text/plain", any ? "saved" : "nothing to set");
}

static void srv_task(void *) {
    bool started = false;
    for (;;) {
        if (net_state() != NET_UP) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!started) {
            s_srv.on("/info", h_info);
            s_srv.on("/screen.bmp", h_screen);
            s_srv.on("/tap", h_tap);
            s_srv.on("/swipe", h_swipe);
            s_srv.on("/reboot", h_reboot);
            s_srv.on("/set", h_set);
            s_srv.on("/perf", h_perf);
            s_srv.on("/update", h_update);
            // Board images through the S3 (fwupdate.cpp update_board): no TLS on the board.
            s_srv.on("/fw/c3.bin", []() { fwu_serve_image("c3", s_srv); });
            s_srv.on("/fw/cam.bin", []() { fwu_serve_image("cam", s_srv); });
            s_srv.on("/fw/node.bin", []() { fwu_serve_image("node", s_srv); });
            s_srv.begin();
            ArduinoOTA.setHostname(APP_HOST);
            ArduinoOTA.setMdnsEnabled(false);       // net.cpp owns mDNS
            // The video decode shares core 0 with this task: with it running, a 4.6 MB espota took over 6 min
            // instead of 44 s (2026-11-21). Stop the camera and the node polling for the upload.
            ArduinoOTA.onStart([]() { s_ota = 0; cam_set_wanted(false); nodes_pause(true); });
            ArduinoOTA.onProgress([](unsigned d, unsigned t) { s_ota = t ? d * 100 / t : 0; });
            ArduinoOTA.onError([](ota_error_t) { s_ota = -1; cam_set_wanted(true); nodes_pause(false); });
            ArduinoOTA.onEnd([]() { s_ota = 100; });
            ArduinoOTA.begin();
            MDNS.enableArduino(3232);
            started = true;
            Serial.printf("[dev] http://%s/  espota %s.local\n", net_ip().c_str(), APP_HOST);
        }
        s_srv.handleClient();
        ArduinoOTA.handle();
        // Fast while an upload runs (each handle() moves one chunk), lazy otherwise.
        vTaskDelay(pdMS_TO_TICKS(s_ota >= 0 ? 1 : 10));
    }
}

void devsrv_init() { xTaskCreatePinnedToCore(srv_task, "dev", 8192, nullptr, 4, nullptr, 0); }

// ---------------------------------------------------------------- serial commands

static void ser_line(char *l) {
    int a[5] = {0, 0, 0, 0, 0};
    if (!strcmp(l, "info")) {
        Serial.printf("info ver=%s theme=%u state=%d ssid=%s saved=%s ip=%s rssi=%d int=%u psram=%u/%u big=%u\n", FW_VERSION, g_set.theme, net_state(),
                      net_ssid().c_str(), g_set.ssid.c_str(), net_ip().c_str(), net_rssi(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    } else if (sscanf(l, "tap %d %d %d", &a[0], &a[1], &a[2]) >= 2) {
        inject(a[0], a[1], a[0], a[1], a[2] ? a[2] : 120);
        Serial.println("ok");
    } else if (sscanf(l, "swipe %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4]) >= 4) {
        inject(a[0], a[1], a[2], a[3], a[4] ? a[4] : 400);
        Serial.println("ok");
    } else if (!strncmp(l, "wifi ", 5)) {
        char *ss = l + 5, *pw = strchr(ss, '\t');
        if (pw) *pw++ = 0;
        net_join(ss, pw ? pw : "");
        Serial.println("ok");
    } else if (sscanf(l, "baud %d", &a[0]) == 1) {
        Serial.println("ok");
        Serial.flush();
        Serial.updateBaudRate(a[0]);
    } else if (!strcmp(l, "shot")) {
        uint16_t *f = snapshot();
        if (!f) { Serial.println("SHOT 0"); return; }
        Serial.printf("SHOT %u\n", (unsigned)FRAME_BYTES);
        Serial.write((const uint8_t *)f, FRAME_BYTES);
        Serial.flush();
        free(f);
    } else if (!strcmp(l, "reboot")) {
        ESP.restart();
    } else if (!strcmp(l, "perf")) {
        Serial.println(perf_json(true));
    } else if (!strcmp(l, "aipx-download")) {
        // C3 USB relay (ARCHITECTURE.md 4.4): the C3 sends this when it sees esptool's SYNC.  Restart into
        // the ROM serial loader; it listens on UART0, which the UART switch (UART2) routes to the C3.
        // esptool then runs with --before no_reset --after watchdog_reset.
        Serial.println("ok");
        Serial.flush();
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        // Software SYSTEM reset (RTC_SW_SYS_RST): clears every digital peripheral (Wi-Fi MAC, LCD, GDMA)
        // but keeps the RTC flag above.  With esp_restart() (CPU reset only) the loader went deaf 2 times
        // in 15, after the app had run a few minutes, and needed a power cycle; with this reset 5 of 5
        // entries worked, the last a full upload after 4 min of uptime (2026-11-21).  An RTC watchdog
        // system reset was tried first and never fired (this task hung, serial went dead).
        esp_rom_software_reset_system();
    }
}

static void ser_task(void *) {
    static char line[200];
    int n = 0;
    for (;;) {
        while (Serial.available()) {
            int c = Serial.read();
            if (c == '\n' || c == '\r') {
                line[n] = 0;
                if (n) ser_line(line);
                n = 0;
            } else if (n < (int)sizeof(line) - 1) line[n++] = c;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void devsrv_serial_init() { xTaskCreatePinnedToCore(ser_task, "ser", 4096, nullptr, 2, nullptr, 0); }
