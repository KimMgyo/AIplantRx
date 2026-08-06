/*******************************************************************************
 * Size: 32 px
 * Bpp: 4
 * Opts: --font .tmp_lucide.ttf -r 0xe1bb --size 32 --bpp 4 --format lvgl --lv-font-name font_bot --force-fast-kern-format -o src/fonts/font_bot.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef FONT_BOT
#define FONT_BOT 1
#endif

#if FONT_BOT

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+E1BB "" */
    0x0, 0xfc, 0x5b, 0xff, 0xdc, 0x60, 0x1f, 0xfc,
    0x94, 0x20, 0xe, 0x1b, 0x0, 0xff, 0xe4, 0xb3,
    0x15, 0x64, 0x0, 0xff, 0xe6, 0xcd, 0x56, 0x0,
    0xff, 0xf3, 0xa4, 0xd5, 0x7c, 0x0, 0x5a, 0xaf,
    0x4a, 0x80, 0x7f, 0x55, 0xb2, 0xaf, 0x90, 0x0,
    0x4a, 0xbe, 0x6a, 0xb0, 0xf, 0x9d, 0x40, 0x3f,
    0xf9, 0x9, 0x0, 0x1e, 0xa0, 0x1c, 0xff, 0xff,
    0xe2, 0x68, 0x82, 0x80, 0x78, 0x81, 0x4c, 0x3,
    0xff, 0x88, 0x4a, 0x4, 0x1, 0xf8, 0x40, 0x3f,
    0xf8, 0xc2, 0x1, 0xff, 0xe1, 0xcd, 0x10, 0xe,
    0xcd, 0x10, 0xf, 0xe1, 0x9a, 0x0, 0xf1, 0x98,
    0x9c, 0x3, 0x19, 0x89, 0xc0, 0x3d, 0x52, 0x30,
    0xca, 0x1, 0xff, 0xcc, 0x56, 0x8c, 0x0, 0xff,
    0xe8, 0x6a, 0xff, 0x80, 0x3c, 0x20, 0x2, 0x0,
    0xc2, 0x0, 0x20, 0xf, 0x7f, 0x90, 0x3, 0xf8,
    0xb7, 0x10, 0x3, 0x16, 0xe2, 0x0, 0x7f, 0xf1,
    0xc8, 0xc0, 0x3c, 0x46, 0x1, 0xff, 0xe1, 0x70,
    0xf, 0xfe, 0x33, 0x0, 0x7e, 0x50, 0x2d, 0xaa,
    0xff, 0xe2, 0x61, 0x82, 0x80, 0x7a, 0x4, 0x9,
    0x57, 0xff, 0x88, 0x60, 0x30, 0x1, 0xe2, 0xc5,
    0x0, 0xff, 0xe2, 0x8a, 0xe1, 0x0, 0x40
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 512, .box_w = 30, .box_h = 24, .ofs_x = 1, .ofs_y = 4}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 57787, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 1,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_bot = {
#else
lv_font_t font_bot = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 24,          /*The maximum line height required by the font*/
    .base_line = -4,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if FONT_BOT*/

