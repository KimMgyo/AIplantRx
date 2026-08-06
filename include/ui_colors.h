// Runtime theme palette. The C_* names are kept as macros over the active
// palette so widget code never changes; switching themes swaps `g_pal` and
// rebuilds the UI (ui_set_dark()).
#pragma once
#include "lvgl.h"

struct UiPalette {
    lv_color_t screen_bg;
    lv_color_t surface;         // card / bar / menu background
    lv_color_t border;
    lv_color_t text_dark;       // primary text
    lv_color_t text_secondary;
    lv_color_t green;
    lv_color_t green_tint;
    lv_color_t pill_bg;
    lv_color_t blue;
    lv_color_t blue_tint;
    lv_color_t amber;
    lv_color_t amber_tint;
    lv_color_t gray;
    lv_color_t gray_tint;
    lv_color_t inactive_menu;
    lv_color_t progress_track;
    lv_color_t skeleton;        // loading placeholder bars
    lv_color_t led_tint;
    lv_color_t rgb_grad1;
    lv_color_t rgb_grad2;
    lv_color_t thermal_grad1;
    lv_color_t thermal_grad2;
    lv_color_t switch_off;
    lv_color_t allstop_bg;
    lv_color_t allstop_border;
    lv_color_t allstop_text;
    lv_color_t allstop_subtext;
    lv_color_t range_track;
    lv_color_t white;           // true white: knobs, chips, overlay text
    lv_color_t live_red;

    // The AI-RX menu accent. Its own hue because the nav had two greens: the item
    // for the page whose whole subject is the model lit up in 모니터링's exact
    // C_GREEN, so the highlight said nothing about which of the two was selected.
    // Violet is the only family here not already spoken for - green acts, blue
    // measures, amber configures, red stops.
    //
    // A spectrum and not one colour: the highlight is a two-stop gradient (see
    // topbar_refresh) run wide - indigo through violet to magenta - so it reads as
    // "generated" and is the only multi-hue item in the nav. The endpoints stop
    // short of C_BLUE and C_LIVE_RED, so the family still says violet, not "measure"
    // or "stop". `ai` is the solid the label and icon take (LVGL 8 has no gradient
    // for text): near-white on the dark theme, where the vivid band would drown
    // violet ink, and the violet itself on the light theme's paler band.
    lv_color_t ai;
    lv_color_t ai_grad1;
    lv_color_t ai_grad2;
};

extern const UiPalette *g_pal;  // active palette (ui_theme.cpp)

#define C_SCREEN_BG        (g_pal->screen_bg)
#define C_SURFACE          (g_pal->surface)
#define C_BORDER           (g_pal->border)
#define C_TEXT_DARK        (g_pal->text_dark)
#define C_TEXT_SECONDARY   (g_pal->text_secondary)
#define C_GREEN            (g_pal->green)
#define C_GREEN_TINT       (g_pal->green_tint)
#define C_PILL_BG          (g_pal->pill_bg)
#define C_BLUE             (g_pal->blue)
#define C_BLUE_TINT        (g_pal->blue_tint)
#define C_AMBER            (g_pal->amber)
#define C_AMBER_TINT       (g_pal->amber_tint)
#define C_GRAY             (g_pal->gray)
#define C_GRAY_TINT        (g_pal->gray_tint)
#define C_INACTIVE_MENU    (g_pal->inactive_menu)
#define C_PROGRESS_TRACK   (g_pal->progress_track)
#define C_SKELETON         (g_pal->skeleton)
#define C_LED_TINT         (g_pal->led_tint)
#define C_RGB_GRAD1        (g_pal->rgb_grad1)
#define C_RGB_GRAD2        (g_pal->rgb_grad2)
#define C_THERMAL_GRAD1    (g_pal->thermal_grad1)
#define C_THERMAL_GRAD2    (g_pal->thermal_grad2)
#define C_SWITCH_OFF       (g_pal->switch_off)
#define C_ALLSTOP_BG       (g_pal->allstop_bg)
#define C_ALLSTOP_BORDER   (g_pal->allstop_border)
#define C_ALLSTOP_TEXT     (g_pal->allstop_text)
#define C_ALLSTOP_SUBTEXT  (g_pal->allstop_subtext)
#define C_RANGE_TRACK      (g_pal->range_track)
#define C_WHITE            (g_pal->white)
#define C_LIVE_RED         (g_pal->live_red)
#define C_AI               (g_pal->ai)
#define C_AI_GRAD1         (g_pal->ai_grad1)
#define C_AI_GRAD2         (g_pal->ai_grad2)
