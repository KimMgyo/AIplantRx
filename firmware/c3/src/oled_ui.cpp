// OLED panels (UI.md 5), laid out as ui/mockups/make_mockups.py draws them, with the same fonts and icons
// rasterised at 1 bit by tools/gen_oled_assets.py.  Run that script again after changing any text here:
// it keeps only the characters that appear in this file's string literals.
//
// Two-colour rule: OLED 3 is a yellow/blue module (rows 0-15 yellow, 16-63 blue, a dark gap between).
// Every panel keeps its title line inside rows 0-15 and everything else from row 18 down, so on that
// module the title is yellow and the data blue, and the other four (white) share the same layout.  For
// the same reason the burn-in shift moves the picture sideways only (UI.md 5 asks for 1 px a minute).
//
// States (UI.md 5): out of the target range -> the title line is inverted and blinks every 2 s; a pump
// running -> an inverted bottom line says so; no fresh /state for 10 s -> the last values stay and the
// bottom line says how old they are; a sensor the node reports as not answering -> "--" and a note.
#include "oled_ui.h"
#include "oled_assets.h"
#include <time.h>

static Adafruit_SSD1306 *G;
static uint32_t MS;

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

// y is the baseline; x is the left edge, the centre or the right edge of the text's advance box.
static void text(const OFont &f, int x, int y, const char *s, int align = LEFT, uint16_t color = SSD1306_WHITE) {
    if (align != LEFT) x -= align == CENTER ? text_w(f, s) / 2 : text_w(f, s);
    while (*s) {
        const OGlyph *g = find(f, next_cp(s));
        if (!g) continue;
        if (g->w) G->drawBitmap(x + g->x, y + g->y, f.bits + g->off, g->w, g->h, color);
        x += g->adv;
    }
}

static void text_mid(const OFont &f, int x, int y, const char *s, int align = LEFT, uint16_t color = SSD1306_WHITE) {
    text(f, x, y + f.mid, s, align, color);
}

static bool blink() { return (MS / 2000) % 2 == 0; }

// Title line: rows 0-15 only (the yellow band of OLED 3).  alert = out of range: inverted, blinking.
static void header(const char *icon, const char *title, const char *right, bool alert = false) {
    bool inv = alert && blink();
    uint16_t c = inv ? SSD1306_BLACK : SSD1306_WHITE;
    if (inv) G->fillRoundRect(0, 0, 128, 16, 3, SSD1306_WHITE);
    text_mid(F_ICON, inv ? 2 : 0, 8, icon, LEFT, c);
    text_mid(F_TITLE, 20, 8, title, LEFT, c);
    if (right && *right) text_mid(F_SMALL, inv ? 125 : 127, 9, right, RIGHT, c);
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
    if (!isnan(tlo) && !isnan(thi)) {
        int a = sx(tlo), b = sx(thi);
        G->fillRoundRect(a, y - 2, b - a + 1, 5, 2, SSD1306_WHITE);
    }
    if (isnan(v)) return;
    int m = sx(v);
    G->fillCircle(m, y, 5, SSD1306_BLACK);
    G->fillCircle(m, y, 4, SSD1306_WHITE);
    G->fillCircle(m, y, 2, SSD1306_BLACK);
}

// Bottom line, inverted: a pump running, a lost link or a sensor that does not answer (UI.md 5).
static void footer(const char *s) {
    G->fillRect(0, 50, 128, 14, SSD1306_BLACK);
    G->fillRoundRect(0, 51, 128, 13, 3, SSD1306_WHITE);
    text_mid(F_SMALL, 64, 57, s, CENTER, SSD1306_BLACK);
}

static void num(char *o, size_t n, float v, int dec) {
    if (isnan(v)) strlcpy(o, "--", n);
    else snprintf(o, n, dec ? "%.1f" : "%.0f", v);
}

static bool out_of(float v, float lo, float hi) { return !isnan(v) && !isnan(lo) && !isnan(hi) && (v < lo || v > hi); }

// The bottom line shared by every panel when the link is gone: values stay, their age is shown.
static bool stale_footer(const NodeView &v) {
    if (v.age_ms < 10000) return false;
    char b[32];
    uint32_t s = v.age_ms / 1000;
    if (s < 60) snprintf(b, sizeof b, "연결 대기 · %lu초 전 값", (unsigned long)s);
    else if (s < 3600) snprintf(b, sizeof b, "연결 대기 · %lu분 전 값", (unsigned long)(s / 60));
    else strlcpy(b, "연결 대기 · 1시간 넘음", sizeof b);
    footer(b);
    return true;
}

static void waiting(const char *icon, const char *title, const NodeView &v) {
    header(icon, title, "");
    text(F_SMALL, 64, 38, v.found ? "값을 기다리는 중" : "센서 노드를 찾는 중", CENTER);
}

static void air(const NodeView &v) {
    float lo = v.t_lo, hi = v.t_hi;
    bool o = out_of(v.air_c, lo, hi);
    header(IC_THERMOMETER, o ? (v.air_c > hi ? "온도 높음" : "온도 낮음") : "공기 온도", v.day ? "낮 구간" : "밤 구간", o);
    char b[8], r[16];
    num(b, sizeof b, v.air_c, 1);
    big(F_BIG30, 30, 0, 46, b, "°C");
    text(F_SMALL, 127, 31, "목표", RIGHT);
    if (!isnan(lo)) {
        snprintf(r, sizeof r, "%.0f–%.0f", lo, hi);
        text(F_SMALL, 127, 45, r, RIGHT);
    }
    if (stale_footer(v)) return;
    if (!v.ok_scd41 || isnan(v.air_c)) footer("온습도 센서 응답 없음");
    else bar(10, 35, lo, hi, v.air_c);
}

static void humid(const NodeView &v) {
    bool vent = !isnan(v.co2) && v.co2 > 1500;
    bool o = out_of(v.rh, v.rh_lo, v.rh_hi);
    header(IC_DROP, vent ? "환기 필요" : o ? (v.rh > v.rh_hi ? "습도 높음" : "습도 낮음") : "습도", "CO2", vent || o);
    char b[8], c[8];
    num(b, sizeof b, v.rh, 0);
    num(c, sizeof c, v.co2, 0);
    big(F_BIG30, 30, 0, 46, b, "%");
    text(F_BIG20, 127, 40, c, RIGHT);
    text(F_SMALL, 127, 50, "ppm", RIGHT);
    if (stale_footer(v)) return;
    if (!v.ok_scd41 || isnan(v.rh)) footer("온습도 센서 응답 없음");
    else bar(0, 100, v.rh_lo, v.rh_hi, v.rh, 1, 82);
}

// 44x33 thumbnail in three levels so a plant reads on a 1-bit panel: colder than the air black, within a
// degree a checkerboard, warmer white.
static void thumb(const NodeView &v) {
    float ref = v.air_c;
    if (isnan(ref)) {
        long s = 0;
        for (int i = 0; i < 768; i++) s += v.th[i];
        ref = s / 768 / 100.0f;
    }
    int16_t lo = (int16_t)lroundf((ref - 1) * 100), hi = (int16_t)lroundf((ref + 1) * 100);
    for (int y = 0; y < 33; y++)
        for (int x = 0; x < 44; x++) {
            int16_t t = v.th[(y * 24 / 33) * 32 + x * 32 / 44];
            bool on = t > hi || (t >= lo && ((x + y) & 1));
            if (on) G->drawPixel(x, 21 + y, SSD1306_WHITE);
        }
}

static void leaf(const NodeView &v) {
    char top[24] = "", b[8], diff[32] = "";
    if (!isnan(v.th_max)) snprintf(top, sizeof top, "최고 %.1f", v.th_max);
    bool hot = !isnan(v.leaf_c) && !isnan(v.air_c) && v.leaf_c - v.air_c > 2;
    header(IC_SCOPE, hot ? "잎이 뜨거워요" : "잎 온도", top, hot);
    if (v.have_th && v.ok_mlx) thumb(v);
    else G->drawRect(0, 21, 44, 33, SSD1306_WHITE);
    num(b, sizeof b, v.leaf_c, 1);
    big(F_BIG24, 24, 50, 42, b, "°C");
    if (!isnan(v.leaf_c) && !isnan(v.air_c)) {
        float d = v.leaf_c - v.air_c;
        snprintf(diff, sizeof diff, "공기 %s%.1f°C", d < 0 ? "−" : "+", fabsf(d));
        text(F_SMALL, 50, 57, diff);
    }
    if (stale_footer(v)) return;
    if (!v.ok_mlx) footer("열화상 응답 없음");
}

static void hhmm(char *o, size_t n, uint32_t t) {
    time_t tt = t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(o, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void soil(const NodeView &v) {
    bool low = !isnan(v.soil_pct) && !isnan(v.soil_lo) && v.soil_pct < v.soil_lo;
    char right[16] = "", b[8], r1[24], r2[24];
    if (v.last_water > 1700000000) {
        char t[8];
        hhmm(t, sizeof t, v.last_water);
        snprintf(right, sizeof right, "물 %s", t);
    }
    header(IC_DROP_FILL, low ? "흙 수분 낮음" : "흙 수분", low ? "" : right, low);
    num(b, sizeof b, v.soil_pct, 0);
    big(F_BIG30, 30, 0, 46, b, "%");
    if (isnan(v.soil_c)) strlcpy(r1, "흙 --°C", sizeof r1);
    else snprintf(r1, sizeof r1, "흙 %.1f°C", v.soil_c);
    text(F_SMALL, 127, 31, low && !v.water ? "곧 물 줌" : r1, RIGHT);
    if (!isnan(v.soil_lo)) {
        snprintf(r2, sizeof r2, "목표 %.0f–%.0f", v.soil_lo, v.soil_hi);
        text(F_SMALL, 127, 45, r2, RIGHT);
    }
    if (stale_footer(v)) return;
    if (!v.ok_soil || isnan(v.soil_pct)) footer("흙 센서 응답 없음");
    else if (v.water) footer("물 주는 중");
    else bar(0, 100, v.soil_lo, v.soil_hi, v.soil_pct);
}

static void nutri(const NodeView &v) {
    char right[24] = "", e[8], p[8];
    if (v.next_nutr > 1700000000 && v.now > 1700000000) {
        long days = ((long)v.next_nutr - (long)v.now + 86399) / 86400;
        if (days <= 0) strlcpy(right, "물약 오늘", sizeof right);
        else snprintf(right, sizeof right, "물약 %ld일 뒤", days);
    }
    bool ec_o = out_of(v.ec, v.ec_lo, v.ec_hi), ph_o = out_of(v.ph, v.ph_lo, v.ph_hi);
    const char *title = ec_o ? (v.ec > v.ec_hi ? "EC 높음" : "EC 낮음") : ph_o ? (v.ph > v.ph_hi ? "pH 높음" : "pH 낮음") : "양분";
    header(IC_EYEDROPPER_HALFFULL, title, (ec_o || ph_o) ? "" : right, ec_o || ph_o);
    num(e, sizeof e, v.ec, 1);
    num(p, sizeof p, v.ph, 1);
    text(F_SMALL, 0, 30, "EC");
    text(F_BIG22, 0, 48, e);
    text(F_SMALL, 68, 30, "pH");
    text(F_BIG22, 68, 48, p);
    G->drawFastVLine(63, 21, 27, SSD1306_WHITE);
    if (stale_footer(v)) return;
    if (!v.ok_soil || (isnan(v.ec) && isnan(v.ph))) footer("흙 센서 응답 없음");
    else if (v.nutr) footer("물약 넣는 중");
    else if (v.ph_pump) footer("pH 맞추는 중");
    else {
        bar(0, 3, v.ec_lo, v.ec_hi, v.ec, 1, 58);
        bar(4, 8, v.ph_lo, v.ph_hi, v.ph, 69, 126);
    }
}

// Burn-in: the whole picture moves 1 px sideways each minute (0, +1, 0, -1).  Not up and down: that
// would push the title line across the yellow/blue boundary of OLED 3.
static void shift(Adafruit_SSD1306 *d) {
    static const int8_t STEP[4] = {0, 1, 0, -1};
    int dx = STEP[(MS / 60000) % 4];
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

void ui_panel(Adafruit_SSD1306 *d, int idx, uint32_t ms, const NodeView &v) {
    G = d;
    MS = ms;
    d->clearDisplay();
    static const char *const TITLES[5] = {"공기 온도", "습도", "잎 온도", "흙 수분", "양분"};
    static const char *const ICONS[5] = {IC_THERMOMETER, IC_DROP, IC_SCOPE, IC_DROP_FILL, IC_EYEDROPPER_HALFFULL};
    if (idx < 0 || idx > 4) text(F_SMALL, 64, 36, "TCA 채널 2~6이 아님", CENTER);
    else if (!v.have) waiting(ICONS[idx], TITLES[idx], v);
    else switch (idx) {
        case 0: air(v); break;
        case 1: humid(v); break;
        case 2: leaf(v); break;
        case 3: soil(v); break;
        case 4: nutri(v); break;
    }
    shift(d);
}

void ui_upload(Adafruit_SSD1306 *d) {
    G = d;
    d->clearDisplay();
    header(IC_ARROW_DOWN_CIRCLE, "S3 업데이트", "");
    text(F_TITLE, 64, 38, "펌웨어 받는 중", CENTER);
    text(F_SMALL, 64, 56, "끝나면 다시 켜짐", CENTER);
}

void ui_self_update(Adafruit_SSD1306 *d, int pct) {
    G = d;
    d->clearDisplay();
    char b[24];
    snprintf(b, sizeof b, "%d%%", pct < 0 ? 0 : pct);
    header(IC_ARROW_DOWN_CIRCLE, "업데이트 중", b);
    text(F_TITLE, 64, 36, "OLED 펌웨어", CENTER);
    int w = 120 * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
    G->drawRoundRect(4, 46, 120, 8, 3, SSD1306_WHITE);
    if (w > 2) G->fillRoundRect(4, 46, w, 8, 3, SSD1306_WHITE);
}
