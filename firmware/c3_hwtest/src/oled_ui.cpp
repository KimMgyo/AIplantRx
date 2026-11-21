// OLED panels, laid out as ui/mockups/make_mockups.py draws them (UI.md 5), with the same fonts and icons
// rasterised at 1 bit by tools/gen_oled_assets.py.  Run that script again after changing any text here:
// it keeps only the characters that appear in this file's string literals.
//
// Two-colour rule: OLED 3 is a yellow/blue module (rows 0-15 yellow, 16-63 blue, a dark gap between).
// Every panel keeps its title line inside rows 0-15 and everything else from row 18 down, so on that
// module the title is yellow and the data blue, and the other four (white) share the same layout.  For
// the same reason the burn-in shift moves the picture sideways only (UI.md 5 asks for 1 px a minute).
//
// The values are demo values (the basil example of the mockups) drifting slowly so a frozen OLED is easy
// to spot; OLED 4 runs through the "too dry" and "watering" states every 90 s.  Real values come later
// from the sensor node's GET /state (UI.md 5, ARCHITECTURE.md 4.4).
#include "oled_ui.h"
#include "oled_assets.h"

static Adafruit_SSD1306 *G;

static const OGlyph *find(const OFont &f, uint32_t cp) {
    int lo = 0, hi = f.n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f.g[m].cp == cp) return &f.g[m];
        if (f.g[m].cp < cp) lo = m + 1;
        else hi = m - 1;
    }
    return nullptr;
}

static uint32_t next_cp(const char *&s) {
    uint8_t c = *s++;
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> n);
    while (n-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
    return cp;
}

static int text_w(const OFont &f, const char *s) {
    int w = 0;
    while (*s) {
        const OGlyph *g = find(f, next_cp(s));
        if (g) w += g->adv;
    }
    return w;
}

enum { LEFT, CENTER, RIGHT };

// y is the baseline; x is the left edge, the centre or the right edge of the text's advance box (PIL's
// "ls", "ms", "rs" anchors).
static void text(const OFont &f, int x, int y, const char *s, int align = LEFT, uint16_t color = SSD1306_WHITE) {
    if (align != LEFT) x -= align == CENTER ? text_w(f, s) / 2 : text_w(f, s);
    while (*s) {
        const OGlyph *g = find(f, next_cp(s));
        if (!g) continue;
        if (g->w) G->drawBitmap(x + g->x, y + g->y, f.bits + g->off, g->w, g->h, color);
        x += g->adv;
    }
}

// Same, with y at the middle between ascender and descender (PIL's "lm", "rm", "mm").
static void text_mid(const OFont &f, int x, int y, const char *s, int align = LEFT,
                     uint16_t color = SSD1306_WHITE) {
    text(f, x, y + f.mid, s, align, color);
}

// Title line: rows 0-15 only (the yellow band of OLED 3).  invert = out of range (UI.md 5).
static void header(const char *icon, const char *title, const char *right, bool invert = false) {
    uint16_t c = invert ? SSD1306_BLACK : SSD1306_WHITE;
    if (invert) G->fillRoundRect(0, 0, 128, 16, 3, SSD1306_WHITE);
    text_mid(F_ICON, invert ? 2 : 0, 8, icon, LEFT, c);
    text_mid(F_TITLE, 20, 8, title, LEFT, c);
    if (right && *right) text_mid(F_SMALL, invert ? 125 : 127, 9, right, RIGHT, c);
}

static void big(const OFont &f, int size, int x, int base, const char *value, const char *unit) {
    text(f, x, base, value);
    text(F_SMALL, x + text_w(f, value) + 2, base - (size * 52 + 50) / 100, unit);
}

// Target bar: thin line, the target range as a thick segment, the current value as a ring.
static void bar(float lo, float hi, float tlo, float thi, float v, int x0 = 1, int x1 = 126, int y = 58) {
    auto sx = [&](float t) {
        t = t < lo ? lo : t > hi ? hi : t;
        return (int)lroundf(x0 + (x1 - x0) * (t - lo) / (hi - lo));
    };
    G->drawFastHLine(x0, y, x1 - x0 + 1, SSD1306_WHITE);
    int a = sx(tlo), b = sx(thi), m = sx(v);
    G->fillRoundRect(a, y - 2, b - a + 1, 5, 2, SSD1306_WHITE);
    G->fillCircle(m, y, 5, SSD1306_BLACK);
    G->fillCircle(m, y, 4, SSD1306_WHITE);
    G->fillCircle(m, y, 2, SSD1306_BLACK);
}

// Bottom line, inverted: a device running or a lost link (UI.md 5).
static void footer(const char *s) {
    G->fillRoundRect(0, 51, 128, 13, 3, SSD1306_WHITE);
    text_mid(F_SMALL, 64, 57, s, CENTER, SSD1306_BLACK);
}

static float wave(uint32_t now, float amp, uint32_t period_ms) {
    return amp * sinf(6.2831853f * (now % period_ms) / period_ms);
}

static void air(uint32_t now) {
    float t = 24.6f + wave(now, 0.3f, 47000);
    char v[8];
    snprintf(v, sizeof v, "%.1f", t);
    header(IC_THERMOMETER, "공기 온도", "낮 구간");
    big(F_BIG30, 30, 0, 46, v, "°C");
    text(F_SMALL, 127, 31, "목표", RIGHT);
    text(F_SMALL, 127, 45, "20–28", RIGHT);
    bar(10, 35, 20, 28, t);
}

static void humid(uint32_t now) {
    int h = lroundf(58 + wave(now, 1.6f, 61000)), co2 = lroundf(820 + wave(now, 18, 83000));
    char v[8], c[8];
    snprintf(v, sizeof v, "%d", h);
    snprintf(c, sizeof c, "%d", co2);
    header(IC_DROP, "습도", "CO2");
    big(F_BIG30, 30, 0, 46, v, "%");
    text(F_BIG20, 127, 40, c, RIGHT);
    text(F_SMALL, 127, 50, "ppm", RIGHT);
    bar(0, 100, 40, 60, h, 1, 82);
}

static void leaf(uint32_t now) {
    float t = 23.1f + wave(now, 0.2f, 53000), a = 24.6f + wave(now, 0.3f, 47000), d = t - a;
    char v[8], top[24], diff[32];
    snprintf(v, sizeof v, "%.1f", t);
    snprintf(top, sizeof top, "최고 %.1f", t + 3.7f);
    snprintf(diff, sizeof diff, "공기 %s%.1f°C", d < 0 ? "−" : "+", fabsf(d));
    header(IC_SCOPE, "잎 온도", top);
    G->drawBitmap(0, 21, THERMAL_DEMO, THERMAL_W, THERMAL_H, SSD1306_WHITE);
    big(F_BIG24, 24, 50, 42, v, "°C");
    text(F_SMALL, 50, 57, diff);
}

// Demo cycle, 90 s: dries from 47 to 38 % (below the 40-60 target from about 47 s: title inverted,
// blinking every 2 s), waters for 3 s at 60 s, then stays at 47 %.
static void soil(uint32_t now) {
    uint32_t p = now % 90000;
    float m = p < 60000 ? 47 - 9.0f * p / 60000 : p < 63000 ? 38 : 47;
    int mi = lroundf(m);
    char v[8], pump[32];
    snprintf(v, sizeof v, "%d", mi);
    bool low = mi < 40 && p < 60000;
    if (low) header(IC_DROP_FILL, "흙 수분 낮음", "", (now / 2000) % 2 == 0);
    else header(IC_DROP_FILL, "흙 수분", "물 09:40");
    big(F_BIG30, 30, 0, 46, v, "%");
    text(F_SMALL, 127, 31, low ? "곧 물 줌" : "흙 22.8°C", RIGHT);
    text(F_SMALL, 127, 45, "목표 40–60", RIGHT);
    if (p >= 60000 && p < 63000) {
        snprintf(pump, sizeof pump, "물 주는 중 · %lu/3초", (unsigned long)((p - 60000) / 1000 + 1));
        footer(pump);
    } else bar(0, 100, 40, 60, m);
}

static void nutri(uint32_t now) {
    float ec = 1.2f + wave(now, 0.04f, 71000), ph = 6.4f + wave(now, 0.04f, 89000);
    char e[8], p[8];
    snprintf(e, sizeof e, "%.1f", ec);
    snprintf(p, sizeof p, "%.1f", ph);
    header(IC_EYEDROPPER_HALFFULL, "양분", "물약 3일 뒤");
    text(F_SMALL, 0, 30, "EC");
    text(F_BIG22, 0, 48, e);
    text(F_SMALL, 68, 30, "pH");
    text(F_BIG22, 68, 48, p);
    G->drawFastVLine(63, 21, 27, SSD1306_WHITE);
    bar(0, 3, 1.0f, 1.6f, ec, 1, 58);
    bar(4, 8, 6.0f, 7.0f, ph, 69, 126);
}

// Burn-in: the whole picture moves 1 px sideways each minute (0, +1, 0, -1).  Not up and down: that
// would push the title line across the yellow/blue boundary of OLED 3.
static void shift(Adafruit_SSD1306 *d, uint32_t now) {
    static const int8_t STEP[4] = {0, 1, 0, -1};
    int dx = STEP[(now / 60000) % 4];
    if (!dx) return;
    uint8_t *b = d->getBuffer();
    for (int page = 0; page < 8; page++) {
        uint8_t *row = b + page * 128;
        if (dx > 0) {
            memmove(row + 1, row, 127);
            row[0] = 0;
        } else {
            memmove(row, row + 1, 127);
            row[127] = 0;
        }
    }
}

void ui_panel(Adafruit_SSD1306 *d, int idx, uint32_t now) {
    G = d;
    d->clearDisplay();
    switch (idx) {
        case 0: air(now); break;
        case 1: humid(now); break;
        case 2: leaf(now); break;
        case 3: soil(now); break;
        case 4: nutri(now); break;
        default: text(F_SMALL, 64, 36, "TCA 채널 2~6이 아님", CENTER);
    }
    shift(d, now);
}

void ui_upload(Adafruit_SSD1306 *d) {
    G = d;
    d->clearDisplay();
    header(IC_ARROW_DOWN_CIRCLE, "S3 업데이트", "");
    text(F_TITLE, 64, 38, "펌웨어 받는 중", CENTER);
    text(F_SMALL, 64, 56, "끝나면 다시 켜짐", CENTER);
}
