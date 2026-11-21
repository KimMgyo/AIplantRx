// UI internals shared by the screen files.
#pragma once
#include <lvgl.h>
#include "f7.h"
#include "fonts.h"
#include "theme.h"
#include "ui_assets.h"

// Plain container: no style, no scrolling, not clickable.
lv_obj_t *box(lv_obj_t *par, int x, int y, int w, int h);
// Box with a fill and radius.
lv_obj_t *fill(lv_obj_t *par, int x, int y, int w, int h, lv_color_t c, int r);
lv_obj_t *label(lv_obj_t *par, const lv_font_t *f, Col c, const char *txt);
lv_obj_t *labelc(lv_obj_t *par, const lv_font_t *f, lv_color_t c, const char *txt);
// These three change nothing (and invalidate nothing) when the object already looks that way.
void set_col(lv_obj_t *o, Col c);
void set_bg(lv_obj_t *o, lv_color_t c, lv_opa_t opa);
void show(lv_obj_t *o, bool on);
bool set_text(lv_obj_t *l, const char *t);      // true when the text changed
// Cut a label to one line with "…". LVGL only cuts a label whose height is fixed; otherwise it wraps.
void one_line(lv_obj_t *l);
// Content-sized capsule with centered text.
lv_obj_t *capsule(lv_obj_t *par, const char *txt, const lv_font_t *f, lv_color_t bg, lv_color_t fg, int h, int padx);
// Round icon with a two-stop vertical gradient (visionOS settings icon).
lv_obj_t *vic(lv_obj_t *par, const char *icon, uint32_t rgb, int d, const lv_font_t *f);
// Switch 46x28 (no shadow). The callback gets LV_EVENT_VALUE_CHANGED; read it with toggle_on().
lv_obj_t *toggle(lv_obj_t *par, bool on, lv_event_cb_t cb, void *user);
bool toggle_on(lv_obj_t *sw);
lv_obj_t *spinner(lv_obj_t *par, int d);
void make_button(lv_obj_t *o, lv_event_cb_t cb, void *user);   // clickable, with a pressed look

// Screens.
#define UI_PAGE_DISPLAY 11
lv_obj_t *main_build();
void main_destroy();
lv_obj_t *settings_build(int page);
void settings_destroy();
void ui_show_main();
void ui_show_settings(int page);
extern lv_obj_t *g_main, *g_settings;

// Sheets over the current screen (lv_layer_top). One at a time.
void sheet_info(const char *title, const char *body);
typedef void (*text_done_cb)(const char *text, void *user);
void sheet_text(const char *title, const char *initial, bool password, text_done_cb done, void *user);
bool sheet_open();
void sheet_close();
// A sheet of any size over the dimmed screen (the box is returned; sheet_close() removes it).
lv_obj_t *sheet_begin(int x, int y, int w, int h);
// 34 px round button on a sheet (visionOS .vcirc); hi = the accent one.
lv_obj_t *sheet_circle(lv_obj_t *par, const char *icon, bool hi, lv_event_cb_t cb);
// The find-plant sheet (find_sheet.cpp): progress, then the result (UI.md 3).
void find_sheet_open();
// "Change kind": Pl@ntNet's other candidates and earlier plants.
void kind_sheet_open();
// Main screen again with the current profile (after a plant was found).
void ui_rebuild_main();

int text_w(const lv_font_t *f, const char *t);

// Korean date and time strings.
void fmt_time(char *out, size_t n);
void fmt_date(char *out, size_t n);
