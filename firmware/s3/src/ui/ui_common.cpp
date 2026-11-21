#include "ui_int.h"
#include "ui.h"
#include "app.h"
#include "settings.h"
#include "nodes.h"
#include <time.h>

lv_obj_t *g_main, *g_settings;

lv_obj_t *box(lv_obj_t *par, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

lv_obj_t *fill(lv_obj_t *par, int x, int y, int w, int h, lv_color_t c, int r) {
    lv_obj_t *o = box(par, x, y, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, r, 0);
    return o;
}

void set_col(lv_obj_t *o, Col c) {
    // Setting a style always invalidates the object, even to the value it already has: the 200 ms main
    // tick repainted the status icons five times a second that way. Only change what differs.
    if (lv_obj_get_style_text_color(o, 0).full != c.c.full) lv_obj_set_style_text_color(o, c.c, 0);
    if (lv_obj_get_style_text_opa(o, 0) != c.o) lv_obj_set_style_text_opa(o, c.o, 0);
}

void set_bg(lv_obj_t *o, lv_color_t c, lv_opa_t opa) {
    if (lv_obj_get_style_bg_color(o, 0).full != c.full) lv_obj_set_style_bg_color(o, c, 0);
    if (lv_obj_get_style_bg_opa(o, 0) != opa) lv_obj_set_style_bg_opa(o, opa, 0);
}

void show(lv_obj_t *o, bool on) {
    // lv_obj_clear_flag(HIDDEN) invalidates a visible object twice; skip when nothing changes.
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != on) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

bool set_text(lv_obj_t *l, const char *t) {
    const char *cur = lv_label_get_text(l);
    if (cur && !strcmp(cur, t)) return false;
    lv_label_set_text(l, t);
    return true;
}

lv_obj_t *label(lv_obj_t *par, const lv_font_t *f, Col c, const char *txt) {
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, f, 0);
    set_col(l, c);
    lv_label_set_text(l, txt);
    return l;
}

lv_obj_t *labelc(lv_obj_t *par, const lv_font_t *f, lv_color_t c, const char *txt) {
    return label(par, f, Col{c, 255}, txt);
}

void one_line(lv_obj_t *l) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

lv_obj_t *capsule(lv_obj_t *par, const char *txt, const lv_font_t *f, lv_color_t bg, lv_color_t fg, int h, int padx) {
    lv_obj_t *o = box(par, 0, 0, LV_SIZE_CONTENT, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_pad_hor(o, padx, 0);
    lv_obj_t *l = labelc(o, f, fg, txt);
    lv_obj_center(l);
    return o;
}

static lv_color_t lighten(lv_color_t c, float k) {
    uint32_t v = lv_color_to32(c);
    uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return lv_color_make(r + (255 - r) * k, g + (255 - g) * k, b + (255 - b) * k);
}

lv_obj_t *vic(lv_obj_t *par, const char *icon, uint32_t rgb, int d, const lv_font_t *f) {
    lv_obj_t *o = box(par, 0, 0, d, d);
    bool gray = rgb == 0;
    lv_color_t c2 = gray ? lv_color_hex(0xADADB3) : lv_color_hex(rgb);
    lv_color_t c1 = gray ? lv_color_hex(0xEDEDF0) : lighten(c2, 0.38f);
    lv_obj_set_style_bg_color(o, c1, 0);
    lv_obj_set_style_bg_grad_color(o, c2, 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *l = labelc(o, f, gray ? lv_color_hex(0x56565C) : lv_color_white(), icon);
    lv_obj_center(l);
    return o;
}

static void toggle_ev(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_current_target(e);
    bool on = !toggle_on(sw);
    lv_obj_set_user_data(sw, (void *)(intptr_t)on);
    lv_obj_set_style_bg_color(sw, on ? P->go : P->sw_off, 0);
    lv_obj_t *knob = lv_obj_get_child(sw, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, knob);
    lv_anim_set_values(&a, lv_obj_get_x(knob), on ? 20 : 2);
    lv_anim_set_time(&a, 150);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, v); });
    lv_anim_start(&a);
    lv_event_send(sw, LV_EVENT_VALUE_CHANGED, nullptr);
}

lv_obj_t *toggle(lv_obj_t *par, bool on, lv_event_cb_t cb, void *user) {
    lv_obj_t *sw = fill(par, 0, 0, 46, 28, on ? P->go : P->sw_off, 14);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sw, 8);
    lv_obj_set_user_data(sw, (void *)(intptr_t)on);
    fill(sw, on ? 20 : 2, 2, 24, 24, lv_color_white(), 12);
    lv_obj_add_event_cb(sw, toggle_ev, LV_EVENT_CLICKED, nullptr);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
    return sw;
}

bool toggle_on(lv_obj_t *sw) { return (intptr_t)lv_obj_get_user_data(sw) != 0; }

lv_obj_t *spinner(lv_obj_t *par, int d) {
    lv_obj_t *s = lv_spinner_create(par, 900, 90);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, d, d);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s, P->spin, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s, P->spin_top, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    return s;
}

void make_button(lv_obj_t *o, lv_event_cb_t cb, void *user) {
    // Pressed look: the object's own colors mixed 30% toward the screen, through a color filter. An
    // object opacity (the old LV_OPA_70) makes LVGL draw the button and its children into a separate
    // layer and blend it on every frame while the finger is down.
    static lv_color_filter_dsc_t filt;
    static lv_style_t st;
    static bool ready = false;
    if (!ready) {
        lv_color_filter_dsc_init(&filt, [](const lv_color_filter_dsc_t *, lv_color_t c, lv_opa_t opa) {
            return lv_color_mix(P->screen, c, opa);
        });
        lv_style_init(&st);
        lv_style_set_color_filter_dsc(&st, &filt);
        lv_style_set_color_filter_opa(&st, 77);
        ready = true;
    }
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(o, &st, LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
}

void fmt_time(char *out, size_t n) {
    time_t t = time(nullptr);
    if (t < 1700000000) { strlcpy(out, "--:--", n); return; }
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void fmt_date(char *out, size_t n) {
    static const char *WD[] = {"일요일", "월요일", "화요일", "수요일", "목요일", "금요일", "토요일"};
    time_t t = time(nullptr);
    if (t < 1700000000) { strlcpy(out, "시간 맞추는 중", n); return; }
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%d월 %d일 %s", tm.tm_mon + 1, tm.tm_mday, WD[tm.tm_wday]);
}

// ---------------------------------------------------------------- screens

void ui_show_main() {
    if (!g_main) g_main = main_build();
    lv_scr_load(g_main);
    cam_set_wanted(true);
    if (g_settings) settings_destroy();
}

void ui_show_settings(int page) {
    cam_set_wanted(false);
    // Settings already on screen (opening another page through /set): the active screen cannot be
    // deleted, so a blank one holds the display while the old settings go.
    lv_obj_t *tmp = nullptr;
    if (g_settings && lv_scr_act() == g_settings) {
        tmp = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(tmp, P->screen, 0);
        lv_scr_load(tmp);
    }
    if (g_settings) settings_destroy();
    g_settings = settings_build(page);
    lv_scr_load(g_settings);
    if (tmp) lv_obj_del(tmp);
}

void ui_init() {
    theme_use(g_set.theme);
    ui_show_main();
}

void ui_go_main() { if (lv_scr_act() != g_main) ui_show_main(); }
void ui_go_settings(int page) { ui_show_settings(page); }

void ui_set_theme(uint8_t t) {
    g_set.theme = t;
    settings_save();
    theme_use(t);
    sheet_close();
    // Rebuild both screens with the new palette; settings stays open on the display page.
    // A blank screen holds the display while the old ones are deleted (the active screen cannot be).
    lv_obj_t *tmp = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(tmp, P->screen, 0);
    lv_scr_load(tmp);
    if (g_settings) settings_destroy();
    if (g_main) main_destroy();
    if (t != 0) glass_release();     // 1 MB of PSRAM back while the theme has no glass
    g_settings = settings_build(UI_PAGE_DISPLAY);
    lv_scr_load(g_settings);
    lv_obj_del(tmp);
}
