// Light/dark palettes and the active palette pointer.
#include "ui_internal.h"

// Converted from the design's oklch() values to sRGB.
static const UiPalette PAL_LIGHT = {
    .screen_bg       = lv_color_hex(0xEDF3F3),
    .surface         = lv_color_hex(0xFFFFFF),
    .border          = lv_color_hex(0xD7DBDE),
    .text_dark       = lv_color_hex(0x12171A),
    .text_secondary  = lv_color_hex(0x626A70),
    .green           = lv_color_hex(0x2C965D),
    .green_tint      = lv_color_hex(0xD5F5DE),
    .pill_bg         = lv_color_hex(0xF3F5F7),
    .blue            = lv_color_hex(0x0085C0),
    .blue_tint       = lv_color_hex(0xD6F0FF),
    .amber           = lv_color_hex(0xC78200),
    .amber_tint      = lv_color_hex(0xFFE6BF),
    .gray            = lv_color_hex(0xABAEB1),
    .gray_tint       = lv_color_hex(0xECEFF1),
    .inactive_menu   = lv_color_hex(0x292F32),
    .progress_track  = lv_color_hex(0xDFE2E4),
    .skeleton        = lv_color_hex(0xDFE2E4),
    .led_tint        = lv_color_hex(0xD2F6DD),
    .rgb_grad1       = lv_color_hex(0x4FAADB),
    .rgb_grad2       = lv_color_hex(0x7CC4EA),
    .thermal_grad1   = lv_color_hex(0xEB7A52),
    .thermal_grad2   = lv_color_hex(0xDE4D52),
    .switch_off      = lv_color_hex(0xB4B8BB),
    .allstop_bg      = lv_color_hex(0xFFDFDA),
    .allstop_border  = lv_color_hex(0xD33A3C),
    .allstop_text    = lv_color_hex(0xA20519),
    .allstop_subtext = lv_color_hex(0xA83634),
    .range_track     = lv_color_hex(0xDBDFE2),
    .white           = lv_color_hex(0xFFFFFF),
    .live_red        = lv_color_hex(0xFF4444),
    .ai              = lv_color_hex(0x6B3FD1),
    .ai_grad1        = lv_color_hex(0xB2BCFF),  // punchier periwinkle -> lavender -> candy pink;
    .ai_grad2        = lv_color_hex(0xFFA5D5),  // saturation capped by the dark-violet ink's contrast
};

// Dark counterparts: dark neutral surfaces, brightened accents, tints as
// deep saturated fills so ON states stay readable on dark cards.
static const UiPalette PAL_DARK = {
    .screen_bg       = lv_color_hex(0x101518),
    .surface         = lv_color_hex(0x1B2126),
    .border          = lv_color_hex(0x30383E),
    .text_dark       = lv_color_hex(0xE9EDF0),
    .text_secondary  = lv_color_hex(0x99A3AA),
    .green           = lv_color_hex(0x3FBA76),
    .green_tint      = lv_color_hex(0x143723),
    .pill_bg         = lv_color_hex(0x242B31),
    .blue            = lv_color_hex(0x2BA6DE),
    .blue_tint       = lv_color_hex(0x0E2E40),
    .amber           = lv_color_hex(0xE09A1F),
    .amber_tint      = lv_color_hex(0x38290E),
    .gray            = lv_color_hex(0x6A7278),
    .gray_tint       = lv_color_hex(0x262C31),
    .inactive_menu   = lv_color_hex(0xC9D1D6),
    .progress_track  = lv_color_hex(0x2C343A),
    .skeleton        = lv_color_hex(0x2C353D),  // subtle lift over the dark card, not harsh squares
    .led_tint        = lv_color_hex(0x12331F),
    .rgb_grad1       = lv_color_hex(0x2C6E96),
    .rgb_grad2       = lv_color_hex(0x3E85AD),
    .thermal_grad1   = lv_color_hex(0xB65535),
    .thermal_grad2   = lv_color_hex(0xA33338),
    .switch_off      = lv_color_hex(0x4A5258),
    .allstop_bg      = lv_color_hex(0x3A1614),
    .allstop_border  = lv_color_hex(0xD33A3C),
    .allstop_text    = lv_color_hex(0xFF9C9C),
    .allstop_subtext = lv_color_hex(0xD97C74),
    .range_track     = lv_color_hex(0x30373D),
    .white           = lv_color_hex(0xFFFFFF),
    .live_red        = lv_color_hex(0xFF5A5A),
    .ai              = lv_color_hex(0xFFFFFF),  // pure white: crispest on the near-neon gradient below
    .ai_grad1        = lv_color_hex(0x550CFF),  // near-max-chroma electric indigo -> violet -> hot
    .ai_grad2        = lv_color_hex(0xFF0CA6),  // magenta; endpoints still clear of C_BLUE / C_LIVE_RED
};

const UiPalette *g_pal = &PAL_DARK;  // dark is the default theme

void ui_theme_set_dark(bool dark) { g_pal = dark ? &PAL_DARK : &PAL_LIGHT; }
