// Three palettes over one layout (UI.md 1.2). Changing theme rebuilds the screens with another table.
#pragma once
#include <lvgl.h>

struct Col {
    lv_color_t c;
    lv_opa_t o;
};

struct Palette {
    bool glass;
    lv_color_t screen;
    Col text, text2, text3, chev, status;
    lv_color_t plat, sep;
    Col sel, sel_line;
    lv_color_t cap, cap_text, cap_hi, cap_hi_text, cap_plat;
    lv_color_t go, sw_off, prog, prog_fill, spin, spin_top;
    lv_color_t ok_win, ok_sheet, ok_text, warn_text;
    lv_color_t sheet1, sheet2, sheet_plat, sheet_sep, sheet_cap, sheet_cap_hi;
    Col sheet_line;
    lv_color_t kb1, kb2, key, key_f, key_ret;
    lv_opa_t dim;
    lv_color_t hl_temp, hl_water, hl_nutri;
    Col side_line;
};

extern const Palette *P;
void theme_use(uint8_t theme);
