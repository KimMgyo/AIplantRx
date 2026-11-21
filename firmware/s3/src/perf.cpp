#include "perf.h"
#include <esp_timer.h>
#include <lvgl.h>

struct Win {
    uint32_t t0_ms;
    uint32_t frames, px;
    uint64_t render_us, wait_us, handler_us;
    uint32_t render_max_us, slow;          // slow: render longer than one panel period
    uint64_t t_rect, t_img, t_letter;      // time inside each kind of draw call
    uint32_t cam_n, th_n;
    uint64_t cam_fetch_us, cam_dec_us, cam_hdr_us, th_us;
};

static Win w;
static int64_t s_start, s_done;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static const uint32_t PERIOD_US = 26800;

static void render_start(lv_disp_drv_t *) { s_start = esp_timer_get_time(); }

static void monitor(lv_disp_drv_t *, uint32_t, uint32_t px) {
    w.frames++;
    w.px += px;
}

// Where a refresh spends its time, by wrapping the draw context's rectangle, image and letter calls.
// This is what showed every video frame repainting the screen and both tiles (scripts/patch_lvgl.py).
static void (*o_rect)(lv_draw_ctx_t *, const lv_draw_rect_dsc_t *, const lv_area_t *);
static void (*o_img)(lv_draw_ctx_t *, const lv_draw_img_dsc_t *, const lv_area_t *, const uint8_t *, lv_img_cf_t);
static void (*o_letter)(lv_draw_ctx_t *, const lv_draw_label_dsc_t *, const lv_point_t *, uint32_t);
static void w_rect(lv_draw_ctx_t *dc, const lv_draw_rect_dsc_t *d, const lv_area_t *a) {
    int64_t t = esp_timer_get_time();
    o_rect(dc, d, a);
    w.t_rect += esp_timer_get_time() - t;
}
static void w_img(lv_draw_ctx_t *dc, const lv_draw_img_dsc_t *d, const lv_area_t *a, const uint8_t *m, lv_img_cf_t cf) {
    int64_t t = esp_timer_get_time();
    o_img(dc, d, a, m, cf);
    w.t_img += esp_timer_get_time() - t;
}
static void w_letter(lv_draw_ctx_t *dc, const lv_draw_label_dsc_t *d, const lv_point_t *p, uint32_t l) {
    int64_t t = esp_timer_get_time();
    o_letter(dc, d, p, l);
    w.t_letter += esp_timer_get_time() - t;
}

extern "C" void perf_render_done(void) {
    s_done = esp_timer_get_time();
    uint32_t r = (uint32_t)(s_done - s_start);
    w.render_us += r;
    if (r > w.render_max_us) w.render_max_us = r;
    if (r > PERIOD_US) w.slow++;
}

extern "C" void perf_wait_done(void) { w.wait_us += esp_timer_get_time() - s_done; }
extern "C" void perf_handler(uint32_t us) { w.handler_us += us; }

void perf_attach() {
    lv_disp_drv_t *d = lv_disp_get_default()->driver;
    d->render_start_cb = render_start;
    d->monitor_cb = monitor;
    lv_draw_ctx_t *dc = d->draw_ctx;
    o_rect = dc->draw_rect;
    dc->draw_rect = w_rect;
    o_img = dc->draw_img_decoded;
    dc->draw_img_decoded = w_img;
    o_letter = dc->draw_letter;
    dc->draw_letter = w_letter;
    w = Win{};
    w.t0_ms = millis();
}

void perf_cam(uint32_t f, uint32_t d, uint32_t hdr) {
    portENTER_CRITICAL(&s_mux);
    w.cam_n++;
    w.cam_fetch_us += f;
    w.cam_dec_us += d;
    w.cam_hdr_us += hdr;
    portEXIT_CRITICAL(&s_mux);
}

void perf_thermal(uint32_t us) {
    portENTER_CRITICAL(&s_mux);
    w.th_n++;
    w.th_us += us;
    portEXIT_CRITICAL(&s_mux);
}

String perf_json(bool reset) {
    Win c = w;
    float s = (millis() - c.t0_ms) / 1000.0f;
    if (s <= 0) s = 1;
    uint32_t f = c.frames ? c.frames : 1;
    float cn = c.cam_n ? c.cam_n : 1;
    char b[520];
    snprintf(b, sizeof b,
             "{\"win_s\":%.1f,\"fps\":%.1f,\"render_ms\":%.2f,\"render_max_ms\":%.2f,\"slow\":%lu,\"wait_ms\":%.2f,\"px\":%lu,"
             "\"lv_busy_pct\":%.1f,\"rect_ms\":%.2f,\"img_ms\":%.2f,\"letter_ms\":%.2f,"
             "\"cam_fps\":%.1f,\"cam_fetch_ms\":%.1f,\"cam_hdr_ms\":%.1f,\"cam_decode_ms\":%.1f,\"th_fps\":%.1f,\"th_ms\":%.1f}",
             s, c.frames / s, c.render_us / 1000.0f / f, c.render_max_us / 1000.0f, (unsigned long)c.slow, c.wait_us / 1000.0f / f,
             (unsigned long)(c.px / f), (c.handler_us - c.wait_us) / 10.0f / (s * 1000.0f), c.t_rect / 1000.0f / f,
             c.t_img / 1000.0f / f, c.t_letter / 1000.0f / f, c.cam_n / s, c.cam_fetch_us / 1000.0f / cn,
             c.cam_hdr_us / 1000.0f / cn, c.cam_dec_us / 1000.0f / cn, c.th_n / s, c.th_n ? c.th_us / 1000.0f / c.th_n : 0);
    if (reset) {
        portENTER_CRITICAL(&s_mux);
        w = Win{};
        w.t0_ms = millis();
        portEXIT_CRITICAL(&s_mux);
    }
    return String(b);
}

