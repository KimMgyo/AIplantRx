// Pictures the S3 computes instead of storing (UI.md 1.6): the glass background, background pieces that
// cover round corners, the AI glyph and the thermal color table.
#pragma once
#include <lvgl.h>
#include <stdint.h>

// Thermal plant box, in 32x24 sensor cells (the tile shows the frame 1:1, 10 px per cell).
#define LEAF_GX0 9
#define LEAF_GX1 23
#define LEAF_GY0 4
#define LEAF_GY1 15

// 800x480 RGB565 glass background, built once from a 100x60 copy and dithered (768 KB of PSRAM).
const uint16_t *glass_bg();
// The settings sidebar: the left 242 px of the glass background with the sidebar tint (232 KB).
// Settings draws it over glass_bg instead of keeping a second full screen.
#define SIDE_W 242
const uint16_t *glass_side();
void glass_release();               // frees both; for the dark and light themes

// A TRUE_COLOR image descriptor over w x h pixels (not copied). Free with free().
lv_img_dsc_t *img_dsc(const uint16_t *px, int w, int h);

// A w x h piece of a background (bg_w pixels wide) at (x, y) with per-pixel opacity alpha[w*h], as a
// TRUE_COLOR_ALPHA image. bg null means the solid color. Freed with piece_free.
lv_img_dsc_t *piece(const uint16_t *bg, int bg_w, lv_color_t solid, int x, int y, int w, int h, const uint8_t *alpha);
void piece_free(lv_img_dsc_t *d);

// Opacity of the area outside a round corner of radius r, r x r, for corner 0 top-left, 1 top-right,
// 2 bottom-left, 3 bottom-right. Static storage, valid until the next call.
const uint8_t *corner_outside(int r, int corner);

const lv_img_dsc_t *ai_glyph();      // 22x22
const uint16_t *inferno_lut();       // 256 RGB565
const lv_img_dsc_t *legend_img();    // 6x130 capsule, hot at the top
