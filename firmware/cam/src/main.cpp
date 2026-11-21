// AIplantRx camera: AI-Thinker ESP32-CAM (OV2640) looking at the plant (ARCHITECTURE.md 2.3, INTERFACES.md 3).
//
//   GET /frame                        QVGA (320x240) JPEG for the S3's screen: the newest frame, up to 12 fps
//   GET /capture?size=uxga|sxga&q=N   one still for identification (q 4..63, default 12; 1-3 s)
//   + the common module (firmware/common): /info, /wifi, /ota from GitHub, espota, rollback.
//
// The driver is initialised at UXGA so its buffers hold a full still; the live picture is the same driver
// switched down to QVGA, and a still switches it up for one frame and back. (smartfarm's streamer
// initialised at QVGA, and switching up from there never produced a frame.) XCLK is 8 MHz: at 20 MHz the
// camera's DMA starved the radio (smartfarm, measured: ping 387 ms against 5 ms at 8 MHz).
//
// UART0 is the console. In the box it is wired to the sensor node's UART1 (firmware/node cambridge.cpp),
// which keeps the log for GET /cam/log and can send "wifi <ssid> <pass>", "info", "reboot", "cam".
#include <Arduino.h>
#include <aipnode.h>
#include <esp_camera.h>

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

// AI-Thinker pin map
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#define FLASH_LED_PIN 4   // kept off: the sensor node's ring lights the plant

static const int LIVE_Q = 12;
static const uint32_t RETRY_MS = 10000;

static SemaphoreHandle_t s_cam_mu;
static volatile bool s_cam_ok;
static bool s_psram;
static uint32_t s_frames, s_captures, s_fails, s_last_try;
static uint32_t s_capture_ms, s_capture_bytes;
static uint16_t s_capture_w, s_capture_h;
static char s_cam_err[48] = "";

// ---------------------------------------------------------------- camera

static bool cam_init() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM;
    c.pin_d1 = Y3_GPIO_NUM;
    c.pin_d2 = Y4_GPIO_NUM;
    c.pin_d3 = Y5_GPIO_NUM;
    c.pin_d4 = Y6_GPIO_NUM;
    c.pin_d5 = Y7_GPIO_NUM;
    c.pin_d6 = Y8_GPIO_NUM;
    c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM;
    c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM;
    c.pin_href = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn = PWDN_GPIO_NUM;
    c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 8000000;
    c.pixel_format = PIXFORMAT_JPEG;
    if (s_psram) {
        c.frame_size = FRAMESIZE_UXGA;   // buffers sized for the largest still
        c.jpeg_quality = 10;
        c.fb_count = 2;
        c.fb_location = CAMERA_FB_IN_PSRAM;
        c.grab_mode = CAMERA_GRAB_LATEST;
    } else {   // should not happen on this board: live picture only, no full still
        c.frame_size = FRAMESIZE_SVGA;
        c.jpeg_quality = 12;
        c.fb_count = 1;
        c.fb_location = CAMERA_FB_IN_DRAM;
        c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }
    esp_err_t e = esp_camera_init(&c);
    if (e != ESP_OK) {
        snprintf(s_cam_err, sizeof s_cam_err, "init 0x%x", (unsigned)e);
        aipnode_logf("[cam] camera init failed: 0x%x\n", (unsigned)e);
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, LIVE_Q);
    s_cam_err[0] = 0;
    aipnode_logf("[cam] camera up (sensor 0x%02x), PSRAM %s\n", s->id.PID, s_psram ? "yes" : "NO");
    return true;
}

static void cam_stop() {
    if (!s_cam_ok) return;
    s_cam_ok = false;
    esp_camera_deinit();
    aipnode_logf("[cam] camera off\n");
}

// A frame of the given width, dropping the ones still at another size (after set_framesize) and, for a
// still, `settle` more so the exposure has caught up. nullptr on failure.
static camera_fb_t *grab(int width, int settle, int tries) {
    for (int i = 0; i < tries; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) return nullptr;
        if (fb->width == width) {
            if (settle-- <= 0) return fb;
        }
        esp_camera_fb_return(fb);
    }
    return nullptr;
}

// ---------------------------------------------------------------- HTTP

static esp_err_t send_jpeg(httpd_req_t *req, camera_fb_t *fb) {
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)fb->buf, fb->len);
}

static esp_err_t h_frame(httpd_req_t *req) {
    if (!s_cam_ok || xSemaphoreTake(s_cam_mu, pdMS_TO_TICKS(3000)) != pdTRUE)
        return aipnode_send_text(req, "503 Service Unavailable", "camera not running");
    camera_fb_t *fb = s_cam_ok ? grab(320, 0, 4) : nullptr;
    esp_err_t r;
    if (fb) {
        r = send_jpeg(req, fb);
        esp_camera_fb_return(fb);
        s_frames++;
    } else {
        s_fails++;
        r = aipnode_send_text(req, "503 Service Unavailable", "no frame");
    }
    xSemaphoreGive(s_cam_mu);
    return r;
}

static esp_err_t h_capture(httpd_req_t *req) {
    char q[48] = "", v[8];
    framesize_t fs = FRAMESIZE_UXGA;
    int width = 1600, quality = 12;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "size", v, sizeof v) == ESP_OK) {
            if (!strcmp(v, "sxga")) { fs = FRAMESIZE_SXGA; width = 1280; }
            else if (strcmp(v, "uxga")) return aipnode_send_text(req, "400 Bad Request", "size: uxga | sxga");
        }
        if (httpd_query_key_value(q, "q", v, sizeof v) == ESP_OK) {
            quality = atoi(v);
            if (quality < 4 || quality > 63) return aipnode_send_text(req, "400 Bad Request", "q: 4..63");
        }
    }
    if (!s_psram) return aipnode_send_text(req, "503 Service Unavailable", "no PSRAM for a still");
    if (!s_cam_ok || xSemaphoreTake(s_cam_mu, pdMS_TO_TICKS(5000)) != pdTRUE)
        return aipnode_send_text(req, "503 Service Unavailable", "camera not running");
    uint32_t t0 = millis();
    camera_fb_t *fb = nullptr;
    if (s_cam_ok) {
        sensor_t *s = esp_camera_sensor_get();
        s->set_quality(s, quality);
        s->set_framesize(s, fs);
        fb = grab(width, 1, 10);
    }
    esp_err_t r;
    if (fb) {
        s_capture_ms = millis() - t0;
        s_capture_bytes = fb->len;
        s_capture_w = fb->width;
        s_capture_h = fb->height;
        r = send_jpeg(req, fb);
        esp_camera_fb_return(fb);
        s_captures++;
        aipnode_logf("[cam] still %ux%u q%d %u B in %lu ms\n", s_capture_w, s_capture_h, quality,
                     (unsigned)s_capture_bytes, (unsigned long)s_capture_ms);
    } else {
        s_fails++;
        r = aipnode_send_text(req, "503 Service Unavailable", "no still");
    }
    if (s_cam_ok) {
        sensor_t *s = esp_camera_sensor_get();
        s->set_framesize(s, FRAMESIZE_QVGA);
        s->set_quality(s, LIVE_Q);
    }
    xSemaphoreGive(s_cam_mu);
    return r;
}

// ---------------------------------------------------------------- common module hooks

static bool before_ota() {
    xSemaphoreTake(s_cam_mu, portMAX_DELAY);
    cam_stop();   // the camera's DMA slows the radio; the download gets it all
    xSemaphoreGive(s_cam_mu);
    return true;
}

static void ota_failed() { s_last_try = 0; }   // loop() brings the camera back

static void info_extra(JsonObject o) {
    JsonObject c = o["camera"].to<JsonObject>();
    c["ok"] = (bool)s_cam_ok;
    if (s_cam_err[0]) c["err"] = s_cam_err;
    c["psram"] = s_psram;
    c["psram_free"] = ESP.getFreePsram();
    c["frames"] = s_frames;
    c["captures"] = s_captures;
    c["fails"] = s_fails;
    if (s_captures) {
        JsonObject l = c["last_capture"].to<JsonObject>();
        l["w"] = s_capture_w;
        l["h"] = s_capture_h;
        l["bytes"] = s_capture_bytes;
        l["ms"] = s_capture_ms;
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);
    Serial.printf("\n[cam] AIplantRx camera %s\n", FW_VERSION);
    s_cam_mu = xSemaphoreCreateMutex();
    s_psram = psramFound();
    s_cam_ok = cam_init();
    s_last_try = millis();

    AipNodeConfig cfg;
    cfg.role = "cam";
    cfg.name = "cam";
    cfg.version = FW_VERSION;
    cfg.before_ota = before_ota;
    cfg.ota_failed = ota_failed;
    cfg.info_extra = info_extra;
    aipnode_begin(cfg);

    httpd_handle_t h = aipnode_http();
    httpd_uri_t u = {};
    u.uri = "/frame"; u.method = HTTP_GET; u.handler = h_frame; httpd_register_uri_handler(h, &u);
    u.uri = "/capture"; u.method = HTTP_GET; u.handler = h_capture; httpd_register_uri_handler(h, &u);
}

void loop() {
    static char line[200];
    static size_t n = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[n] = 0;
            n = 0;
            if (!line[0]) continue;
            if (!strcmp(line, "cam")) {
                JsonDocument d;
                info_extra(d.to<JsonObject>());
                serializeJson(d, Serial);
                Serial.println();
            } else if (!aipnode_command(line, Serial)) {
                Serial.println("[cam] commands: wifi <ssid> <pass> | info | reboot | cam");
            }
        } else if (n < sizeof line - 1) {
            line[n++] = c;
        }
    }
    // espota does not call before_ota(): stop the camera here so the upload has the radio.
    if (aipnode_ota_busy() && s_cam_ok) before_ota();
    if (!s_cam_ok && !aipnode_ota_busy() && millis() - s_last_try > RETRY_MS) {
        s_last_try = millis();
        xSemaphoreTake(s_cam_mu, portMAX_DELAY);
        s_cam_ok = cam_init();
        xSemaphoreGive(s_cam_mu);
    }
    delay(10);
}
