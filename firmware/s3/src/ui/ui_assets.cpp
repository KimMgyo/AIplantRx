#include "ui_assets.h"
#include "app.h"
#include "../assets/glass.h"
#include <math.h>

static const float BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static inline uint16_t dither565(float r, float g, float b, int x, int y) {
    float t = (BAYER[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
    int R = (int)floorf(r / 255.0f * 31 + 0.5f + t), G = (int)floorf(g / 255.0f * 63 + 0.5f + t), B = (int)floorf(b / 255.0f * 31 + 0.5f + t);
    R = R < 0 ? 0 : R > 31 ? 31 : R;
    G = G < 0 ? 0 : G > 63 ? 63 : G;
    B = B < 0 ? 0 : B > 31 ? 31 : B;
    return (R << 11) | (G << 5) | B;
}

// The sidebar tint of the mockup (bake.css .t-glass .side): rgba stops down the screen.
struct Stop { float at, r, g, b, a; };
static const Stop SIDE[] = {{0, 22, 16, 9, .42f}, {.30f, 22, 16, 9, .36f}, {.58f, 96, 92, 88, .24f}, {.85f, 22, 16, 9, .30f}, {1, 22, 16, 9, .40f}};

static void side_tint(float t, float *r, float *g, float *b, float *a) {
    int i = 0;
    while (i < 3 && t > SIDE[i + 1].at) i++;
    float u = (t - SIDE[i].at) / (SIDE[i + 1].at - SIDE[i].at);
    *r = SIDE[i].r + (SIDE[i + 1].r - SIDE[i].r) * u;
    *g = SIDE[i].g + (SIDE[i + 1].g - SIDE[i].g) * u;
    *b = SIDE[i].b + (SIDE[i + 1].b - SIDE[i].b) * u;
    *a = SIDE[i].a + (SIDE[i + 1].a - SIDE[i].a) * u;
}

static void build_glass(uint16_t *out, int w, bool settings) {
    const float k = (float)GLASS_W / SCR_W;
    for (int y = 0; y < SCR_H; y++) {
        float sy = (y + 0.5f) * k - 0.5f;
        int y0 = sy < 0 ? 0 : (int)sy;
        int y1 = y0 + 1 < GLASS_H ? y0 + 1 : GLASS_H - 1;
        float fy = sy < 0 ? 0 : sy - y0;
        float tr = 0, tg = 0, tb = 0, ta = 0;
        if (settings) side_tint((float)y / (SCR_H - 1), &tr, &tg, &tb, &ta);
        for (int x = 0; x < w; x++) {
            float sx = (x + 0.5f) * k - 0.5f;
            int x0 = sx < 0 ? 0 : (int)sx;
            int x1 = x0 + 1 < GLASS_W ? x0 + 1 : GLASS_W - 1;
            float fx = sx < 0 ? 0 : sx - x0;
            float c[3];
            for (int ch = 0; ch < 3; ch++) {
                float a = GLASS_RGB[(y0 * GLASS_W + x0) * 3 + ch], b = GLASS_RGB[(y0 * GLASS_W + x1) * 3 + ch];
                float cc = GLASS_RGB[(y1 * GLASS_W + x0) * 3 + ch], d = GLASS_RGB[(y1 * GLASS_W + x1) * 3 + ch];
                float top = a + (b - a) * fx, bot = cc + (d - cc) * fx;
                c[ch] = top + (bot - top) * fy;
            }
            if (settings) {
                c[0] = c[0] * (1 - ta) + tr * ta;
                c[1] = c[1] * (1 - ta) + tg * ta;
                c[2] = c[2] * (1 - ta) + tb * ta;
            }
            out[y * w + x] = dither565(c[0], c[1], c[2], x, y);
        }
    }
}

static uint16_t *s_glass, *s_side;

const uint16_t *glass_bg() {
    if (!s_glass && (s_glass = (uint16_t *)ps_malloc_or_null(SCR_W * SCR_H * 2))) build_glass(s_glass, SCR_W, false);
    return s_glass;
}

const uint16_t *glass_side() {
    if (!s_side && (s_side = (uint16_t *)ps_malloc_or_null(SIDE_W * SCR_H * 2))) build_glass(s_side, SIDE_W, true);
    return s_side;
}

void glass_release() {
    free(s_glass);
    free(s_side);
    s_glass = s_side = nullptr;
}

lv_img_dsc_t *img_dsc(const uint16_t *px, int w, int h) {
    if (!px) return nullptr;
    lv_img_dsc_t *d = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.w = w;
    d->header.h = h;
    d->data_size = w * h * 2;
    d->data = (const uint8_t *)px;
    return d;
}

lv_img_dsc_t *piece(const uint16_t *bg, int bg_w, lv_color_t solid, int x, int y, int w, int h, const uint8_t *alpha) {
    uint8_t *px = (uint8_t *)ps_malloc_or_null(w * h * 3);
    lv_img_dsc_t *d = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
    if (!px || !d) return nullptr;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            uint16_t c = bg ? bg[(sy < 0 ? 0 : sy >= SCR_H ? SCR_H - 1 : sy) * bg_w + (sx < 0 ? 0 : sx >= bg_w ? bg_w - 1 : sx)] : solid.full;
            uint8_t *p = px + (j * w + i) * 3;
            p[0] = c & 0xFF;
            p[1] = c >> 8;
            p[2] = alpha[j * w + i];
        }
    d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d->header.w = w;
    d->header.h = h;
    d->data_size = w * h * 3;
    d->data = px;
    return d;
}

void piece_free(lv_img_dsc_t *d) {
    if (!d) return;
    free((void *)d->data);
    free(d);
}

const uint8_t *corner_outside(int r, int corner) {
    static uint8_t m[40 * 40];
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int in = 0;
            for (int sj = 0; sj < 4; sj++)
                for (int si = 0; si < 4; si++) {
                    float px = i + (si + 0.5f) / 4, py = j + (sj + 0.5f) / 4;
                    float dx = r - px, dy = r - py;     // distance to the corner circle's centre (top-left case)
                    in += dx * dx + dy * dy <= r * r;
                }
            int ii = (corner & 1) ? r - 1 - i : i, jj = (corner & 2) ? r - 1 - j : j;
            m[jj * r + ii] = (uint8_t)(255 - in * 255 / 16);
        }
    return m;
}

const lv_img_dsc_t *ai_glyph() {
    static lv_img_dsc_t d;
    static uint8_t px[22 * 22 * 3];
    if (d.data) return &d;
    static const uint8_t ST[6][3] = {{0xFF, 0x6B, 0x9A}, {0xB6, 0x6C, 0xFF}, {0x5A, 0xA9, 0xFF}, {0x42, 0xDF, 0xC0}, {0xFF, 0xC0, 0x5A}, {0xFF, 0x6B, 0x9A}};
    for (int j = 0; j < 22; j++)
        for (int i = 0; i < 22; i++) {
            float r = 0, g = 0, b = 0;
            int in = 0;
            for (int s = 0; s < 16; s++) {
                float x = i + ((s & 3) + 0.5f) / 4 - 11, y = j + ((s >> 2) + 0.5f) / 4 - 11;
                if (x * x + y * y > 121) continue;
                in++;
                float ang = atan2f(x, -y) * 180 / (float)M_PI;     // CSS conic: 0 at the top, clockwise
                float u = fmodf(ang - 210 + 720, 360) / 360 * 5;
                int k = (int)u;
                if (k > 4) k = 4;
                float f = u - k;
                r += ST[k][0] + (ST[k + 1][0] - ST[k][0]) * f;
                g += ST[k][1] + (ST[k + 1][1] - ST[k][1]) * f;
                b += ST[k][2] + (ST[k + 1][2] - ST[k][2]) * f;
            }
            uint8_t *p = px + (j * 22 + i) * 3;
            lv_color_t c = in ? lv_color_make(r / in, g / in, b / in) : lv_color_black();
            p[0] = c.full & 0xFF;
            p[1] = c.full >> 8;
            p[2] = in * 255 / 16;
        }
    d.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d.header.w = 22;
    d.header.h = 22;
    d.data_size = sizeof(px);
    d.data = px;
    return &d;
}

const uint16_t *inferno_lut() {
    static uint16_t lut[256];
    static bool done = false;
    if (done) return lut;
    static const uint8_t ST[8][3] = {{0, 0, 4}, {40, 11, 84}, {101, 21, 110}, {159, 42, 99}, {212, 72, 66}, {245, 125, 21}, {250, 193, 39}, {252, 255, 164}};
    for (int i = 0; i < 256; i++) {
        float u = i / 255.0f * 7;
        int k = (int)u;
        if (k > 6) k = 6;
        float f = u - k;
        lut[i] = lv_color_make(ST[k][0] + (ST[k + 1][0] - ST[k][0]) * f, ST[k][1] + (ST[k + 1][1] - ST[k][1]) * f,
                               ST[k][2] + (ST[k + 1][2] - ST[k][2]) * f).full;
    }
    done = true;
    return lut;
}

const lv_img_dsc_t *legend_img() {
    static lv_img_dsc_t d;
    static uint8_t px[6 * 130 * 3];
    if (d.data) return &d;
    const uint16_t *lut = inferno_lut();
    for (int j = 0; j < 130; j++)
        for (int i = 0; i < 6; i++) {
            int in = 0;
            for (int s = 0; s < 16; s++) {
                float x = i + ((s & 3) + 0.5f) / 4, y = j + ((s >> 2) + 0.5f) / 4;
                float cy = y < 3 ? 3 : y > 127 ? 127 : y;
                in += (x - 3) * (x - 3) + (y - cy) * (y - cy) <= 9;
            }
            uint16_t c = lut[255 - j * 255 / 129];
            uint8_t *p = px + (j * 6 + i) * 3;
            p[0] = c & 0xFF;
            p[1] = c >> 8;
            p[2] = in * 255 / 16;
        }
    d.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d.header.w = 6;
    d.header.h = 130;
    d.data_size = sizeof(px);
    d.data = px;
    return &d;
}
