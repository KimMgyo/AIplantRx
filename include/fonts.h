#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Subset fonts: ASCII, the UI's punctuation, and every Hangul syllable the
// firmware or the server can name in advance. Regenerate with tools/gen_fonts.py,
// which derives that charset from both source trees - editing a --symbols list by
// hand is what left each of these leaning on a 12px fallback for 136-147
// syllables, drawing them a size smaller than the line around them.
LV_FONT_DECLARE(font_bold_10);
LV_FONT_DECLARE(font_reg_12);
LV_FONT_DECLARE(font_bold_12);
LV_FONT_DECLARE(font_bold_13);
LV_FONT_DECLARE(font_bold_14);
LV_FONT_DECLARE(font_bold_19);
// Whole-block fallbacks (0xAC00-0xD7A3), for the text neither program can name:
// a scanned SSID, a species name, a sentence the model wrote. Two sizes because a
// fallback draws at its own size, not the caller's: font_bold_14 is the one font
// that both draws unnameable text and is not 12px, so it gets the 14px one and a
// prescription headline stays one size all the way across.
LV_FONT_DECLARE(font_kr_full_12);
LV_FONT_DECLARE(font_kr_full_14);
// Lucide icon subset for device badges (see ICON_* in ui_internal.h).
LV_FONT_DECLARE(font_icons);
// Smaller variant (dropdown menu items): monitor / control / settings glyphs.
LV_FONT_DECLARE(font_icons_sm);
// Single Lucide "bot" glyph (0xe1bb) for the plant-identify button.
LV_FONT_DECLARE(font_bot);

#ifdef __cplusplus
}
#endif
