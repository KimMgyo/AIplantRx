// Sheets over the current screen: an information card and text entry with a visionOS-style keyboard
// (UI.md 4). They live on lv_layer_top; the dim behind them is drawn once.
#include "ui_int.h"
#include "app.h"
#include "ui.h"

static struct {
    lv_obj_t *dim, *ta, *kb;
    text_done_cb done;
    void *user;
    int mode;                // 0 lower, 1 upper, 2 symbols, 3 more symbols
} K;

// The firmware-update card (ui_update_overlay). Its objects live under K.dim like any sheet.
static struct {
    lv_obj_t *ver, *msg, *fill;
} U;

bool sheet_open() { return K.dim != nullptr; }

void sheet_close() {
    if (K.dim) lv_obj_del(K.dim);
    memset(&K, 0, sizeof(K));
    memset(&U, 0, sizeof(U));
}

// Buttons close the sheet they sit on, so the delete waits until their event has finished.
static void close_async() { lv_async_call([](void *) { sheet_close(); }, nullptr); }

static lv_obj_t *dim_layer() {
    sheet_close();
    K.dim = box(lv_layer_top(), 0, 0, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(K.dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(K.dim, P->dim, 0);
    lv_obj_add_flag(K.dim, LV_OBJ_FLAG_CLICKABLE);      // swallow taps on the screen behind
    return K.dim;
}

static lv_obj_t *sheet_box(int x, int y, int w, int h) {
    lv_obj_t *s = box(K.dim, x, y, w, h);
    lv_obj_set_style_bg_color(s, P->sheet1, 0);
    lv_obj_set_style_bg_grad_color(s, P->sheet2, 0);
    lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s, 28, 0);
    lv_obj_set_style_border_width(s, P->sheet_line.o ? 1 : 0, 0);
    lv_obj_set_style_border_color(s, P->sheet_line.c, 0);
    lv_obj_set_style_border_opa(s, P->sheet_line.o, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    return s;
}

static lv_obj_t *sheet_cap(lv_obj_t *par, const char *t, bool hi, lv_event_cb_t cb) {
    lv_obj_t *c = capsule(par, t, &f_s14, hi ? P->sheet_cap_hi : P->sheet_cap, hi ? P->cap_hi_text : (P->glass ? lv_color_white() : P->cap_text), 32, 16);
    make_button(c, cb, nullptr);
    return c;
}

lv_obj_t *sheet_begin(int x, int y, int w, int h) {
    dim_layer();
    return sheet_box(x, y, w, h);
}

lv_obj_t *sheet_circle(lv_obj_t *par, const char *icon, bool hi, lv_event_cb_t cb) {
    lv_obj_t *c = fill(par, 0, 0, 34, 34, hi ? P->sheet_cap_hi : P->sheet_cap, 17);
    lv_obj_center(labelc(c, &i16, hi ? P->cap_hi_text : (P->glass ? lv_color_white() : P->cap_text), icon));
    lv_obj_set_ext_click_area(c, 8);
    make_button(c, cb, nullptr);
    return c;
}

// ---------------------------------------------------------------- information

void sheet_info(const char *title, const char *body) {
    dim_layer();
    lv_obj_t *s = sheet_box(170, 120, 460, 210);
    lv_obj_t *t = label(s, &f_s16, P->text, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *b = label(s, &f_r15, P->text, body);
    lv_obj_set_width(b, 400);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(b, 4, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t *ok = sheet_cap(s, "확인", true, [](lv_event_t *) { close_async(); });
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -18);
}

// ---------------------------------------------------------------- firmware update

static const int UPD_BAR_W = 380;

void ui_update_overlay(const char *ver, int pct, const char *msg) {
    if (!U.fill) {
        dim_layer();
        lv_obj_t *s = sheet_box(190, 140, 420, 190);
        lv_obj_t *t = label(s, &f_s16, P->text, "소프트웨어 업데이트");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
        U.ver = label(s, &f_r15, P->text2, "");
        lv_obj_align(U.ver, LV_ALIGN_TOP_MID, 0, 56);
        fill(s, 20, 102, UPD_BAR_W, 8, P->prog, 4);
        U.fill = fill(s, 20, 102, 8, 8, P->prog_fill, 4);
        U.msg = label(s, &f_m15, P->text, "");
        lv_obj_align(U.msg, LV_ALIGN_TOP_MID, 0, 128);
    }
    char b[48];
    snprintf(b, sizeof b, "AIplantRx %s", ver);
    set_text(U.ver, b);
    lv_obj_align(U.ver, LV_ALIGN_TOP_MID, 0, 56);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = UPD_BAR_W * pct / 100;
    lv_obj_set_width(U.fill, w < 8 ? 8 : w);
    if (pct < 100) snprintf(b, sizeof b, "%s %d%%", msg, pct);
    else strlcpy(b, msg, sizeof b);
    set_text(U.msg, b);
    lv_obj_align(U.msg, LV_ALIGN_TOP_MID, 0, 128);
}

void ui_update_overlay_close() {
    if (U.fill) sheet_close();
}

// ---------------------------------------------------------------- keyboard

#define KDEL F7_DELETE_LEFT
#define KSHIFT F7_SHIFT
static const char *MAP_LO[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", KDEL, "\n",
                               "a", "s", "d", "f", "g", "h", "j", "k", "l", "확인", "\n",
                               KSHIFT, "z", "x", "c", "v", "b", "n", "m", "!", "?", KSHIFT, "\n",
                               ".?123", " ", "@", ".", ""};
static const char *MAP_UP[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", KDEL, "\n",
                               "A", "S", "D", "F", "G", "H", "J", "K", "L", "확인", "\n",
                               KSHIFT, "Z", "X", "C", "V", "B", "N", "M", "!", "?", KSHIFT, "\n",
                               ".?123", " ", "@", ".", ""};
static const char *MAP_S1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", KDEL, "\n",
                               "-", "/", ":", ";", "(", ")", "$", "&", "\"", "확인", "\n",
                               "#+=", ".", ",", "?", "!", "'", "%", "*", "+", "=", "\n",
                               "abc", " ", "@", ".", ""};
static const char *MAP_S2[] = {"[", "]", "{", "}", "#", "^", "<", ">", "|", "~", KDEL, "\n",
                               "_", "\\", "\x60", "'", "\"", ";", ":", "/", "&", "확인", "\n",
                               "123", ".", ",", "?", "!", "%", "*", "+", "=", "-", "\n",
                               "abc", " ", "@", ".", ""};
#define KW(n) (lv_btnmatrix_ctrl_t)(n)
#define KF(n) (lv_btnmatrix_ctrl_t)((n) | LV_BTNMATRIX_CTRL_CHECKED)
static const lv_btnmatrix_ctrl_t CTRL[] = {KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KF(5),
                                           KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KF(7),
                                           KF(5), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KW(4), KF(5),
                                           KF(6), KW(15), KF(3), KF(3)};
static const char **MAPS[4] = {MAP_LO, MAP_UP, MAP_S1, MAP_S2};

static void kb_set(int mode) {
    K.mode = mode;
    lv_btnmatrix_set_map(K.kb, MAPS[mode]);
    lv_btnmatrix_set_ctrl_map(K.kb, CTRL);
}

static void finish() {
    static char txt[160];
    static text_done_cb cb;
    static void *u;
    strlcpy(txt, lv_textarea_get_text(K.ta), sizeof txt);
    cb = K.done;
    u = K.user;
    // Close first, then hand over the text: the callback may open the next sheet.
    lv_async_call([](void *) {
        sheet_close();
        if (cb) cb(txt, u);
    }, nullptr);
}

static void kb_ev(lv_event_t *) {
    uint16_t id = lv_btnmatrix_get_selected_btn(K.kb);
    if (id == LV_BTNMATRIX_BTN_NONE) return;
    const char *t = lv_btnmatrix_get_btn_text(K.kb, id);
    if (!t) return;
    if (!strcmp(t, KDEL)) lv_textarea_del_char(K.ta);
    else if (!strcmp(t, KSHIFT)) kb_set(K.mode == 0 ? 1 : 0);
    else if (!strcmp(t, ".?123") || !strcmp(t, "123")) kb_set(2);
    else if (!strcmp(t, "#+=")) kb_set(3);
    else if (!strcmp(t, "abc")) kb_set(0);
    else if (!strcmp(t, "확인")) finish();
    else {
        lv_textarea_add_text(K.ta, t);
        if (K.mode == 1) kb_set(0);      // shift is for one letter
    }
}

// Word keys a size smaller, the return key in the accent color.
static void kb_draw(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->class_p != &lv_btnmatrix_class || d->type != LV_BTNMATRIX_DRAW_PART_BTN) return;
    const char *t = lv_btnmatrix_get_btn_text(K.kb, d->id);
    if (!t) return;
    bool ret = !strcmp(t, "확인");
    if (ret) {
        d->rect_dsc->bg_color = P->key_ret;
        d->label_dsc->color = P->cap_hi_text;
    }
    if (ret || (strlen(t) > 1 && (unsigned char)t[0] < 0x80)) d->label_dsc->font = &f_s14;
}

static void eye_ev(lv_event_t *e) {
    bool on = !lv_textarea_get_password_mode(K.ta);
    lv_textarea_set_password_mode(K.ta, on);
    lv_obj_set_style_text_opa(lv_event_get_target(e), on ? LV_OPA_70 : LV_OPA_COVER, 0);
}

void sheet_text(const char *title, const char *initial, bool password, text_done_cb done, void *user) {
    dim_layer();
    K.done = done;
    K.user = user;
    lv_obj_t *s = sheet_box(170, 12, 460, password ? 170 : 150);
    lv_obj_t *t = label(s, &f_s16, P->text, title);
    lv_obj_set_width(t, 260);
    one_line(t);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *cancel = sheet_cap(s, "취소", false, [](lv_event_t *) { close_async(); });
    lv_obj_set_pos(cancel, 14, 12);
    lv_obj_t *ok = sheet_cap(s, "확인", true, [](lv_event_t *) { finish(); });
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -14, 12);

    lv_obj_t *field = fill(s, 18, 68, 424, 49, P->sheet_plat, 18);
    K.ta = lv_textarea_create(field);
    lv_obj_remove_style_all(K.ta);
    lv_obj_set_pos(K.ta, 16, 0);
    lv_obj_set_size(K.ta, password ? 360 : 392, 49);
    lv_textarea_set_one_line(K.ta, true);
    lv_textarea_set_max_length(K.ta, 128);
    lv_obj_set_style_text_font(K.ta, &f_m15, 0);
    lv_obj_set_style_text_color(K.ta, P->text.c, 0);
    lv_obj_set_style_pad_top(K.ta, 15, 0);
    lv_obj_set_style_border_side(K.ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(K.ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_color(K.ta, P->text.c, LV_PART_CURSOR);
    lv_obj_set_style_border_opa(K.ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_anim_time(K.ta, 500, LV_PART_CURSOR);
    lv_obj_add_state(K.ta, LV_STATE_FOCUSED);   // shows the cursor
    if (password) {
        lv_textarea_set_password_bullet(K.ta, "•");
        lv_textarea_set_password_mode(K.ta, true);
        lv_textarea_set_password_show_time(K.ta, 600);
        lv_obj_t *eye = label(field, &i19, P->text, F7_EYE);
        lv_obj_set_style_text_opa(eye, LV_OPA_70, 0);
        lv_obj_align(eye, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_add_flag(eye, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(eye, 10);
        lv_obj_add_event_cb(eye, eye_ev, LV_EVENT_CLICKED, nullptr);
    }
    lv_textarea_set_text(K.ta, initial ? initial : "");
    if (password) {
        lv_obj_t *n = label(s, &f_r12, P->text2, "영문 키보드만 씁니다. 연결되면 이 네트워크를 저장합니다.");
        lv_obj_set_pos(n, 34, 132);
    }

    lv_obj_t *kbw = box(K.dim, 56, SCR_H - 12 - 214, SCR_W - 112, 214);
    lv_obj_set_style_bg_color(kbw, P->kb1, 0);
    lv_obj_set_style_bg_grad_color(kbw, P->kb2, 0);
    lv_obj_set_style_bg_grad_dir(kbw, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(kbw, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(kbw, 30, 0);
    lv_obj_set_style_border_width(kbw, P->sheet_line.o ? 1 : 0, 0);
    lv_obj_set_style_border_color(kbw, P->sheet_line.c, 0);
    lv_obj_set_style_border_opa(kbw, P->sheet_line.o, 0);
    lv_obj_add_flag(kbw, LV_OBJ_FLAG_CLICKABLE);
    K.kb = lv_btnmatrix_create(kbw);
    lv_obj_remove_style_all(K.kb);
    lv_obj_set_pos(K.kb, 12, 12);
    lv_obj_set_size(K.kb, SCR_W - 112 - 24, 214 - 22);
    lv_obj_set_style_pad_row(K.kb, 7, 0);
    lv_obj_set_style_pad_column(K.kb, 7, 0);
    lv_obj_set_style_bg_color(K.kb, P->key, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(K.kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(K.kb, 12, LV_PART_ITEMS);
    lv_obj_set_style_text_font(K.kb, &f_m19, LV_PART_ITEMS);
    lv_obj_set_style_text_color(K.kb, P->text.c, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(K.kb, P->key_f, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(K.kb, P->cap_hi, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(K.kb, kb_ev, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(K.kb, kb_draw, LV_EVENT_DRAW_PART_BEGIN, nullptr);
    kb_set(0);
}

