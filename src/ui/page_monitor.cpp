// Monitor page: both cameras side by side, sensors in a strip underneath.
//
// The two feeds used to share one canvas behind a tab bar. They come from
// different devices over different transports and answer different questions -
// "what does it look like" and "where is it hot" - so showing one at a time
// made you flip back and forth to compare them. Now each has its own canvas and
// its own liveness badge, and the tab bar is gone.
//
// Device controls are not here at all: the control page owns them, and the AI-RX
// page owns what the AI does with them.
//
// WHAT THE STRIP SHOWS. Five tiles, and every one of them has to be a number that
// exists AND that the strip is the right place for. 조도 and 토양습도 used to sit
// here drawing "--" forever - the BH1750 is broken and the soil probe was never
// wired - which on a wall panel is indistinguishable from a sensor that dropped out
// this minute, so a third of the strip was actively misleading. They are gone. In
// their place: VPD and 잎-공기 온도차. VPD is not computed here: the tile calls
// aijudge_vpd_kpa(), the device's one implementation, so it and the 판단 page cannot
// disagree about a number they both call VPD - they briefly did, with two
// coefficients and two sets of guards. 잎-공기 is a subtraction and stays local,
// ported from derive.py's leaf_air_dt_c() including its rounding.
//
// 표면온도 left for the second reason. It was a real number, but it was the same
// number the thermal panel now prints on the pixel it came from, and the tile could
// not even give it a verdict - nothing can band "whatever is warmest in frame". A
// duplicate with no verdict was costing a sixth of the strip.
//
// A 최근 구간 table used to sit under the strip: the server's window summary, four
// metrics with min/mean/max and how much of the window each spent inside its band.
// It was removed on request - it was never asked for, and a dashboard answering
// "what is happening now" does not owe the wall an hour of history. The server
// still builds and sends the block because the model reads it; the panel simply
// does not draw it, and plantrx.cpp still parses it (see plantrx_window_at).
//
// Any Korean this page draws may only use fonts that declare
// .fallback = &font_kr_full_12 - see server/tests/test_font_coverage.py, which
// fails the build's own suite if a label is written text its font cannot spell.
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "ui_internal.h"
#include "aijudge.h"
#include "camnet.h"
#include "metrics.h"
#include "plantrx.h"
#include "reading.h"
#include "sensornode.h"
#include "thermal.h"

// Each camera card fills half the 768px content row (376px, 16px gap) and the
// feed fills the card: 374x280, both panels the same size, no letterboxing.
//
// Both canvases are exactly that size, so LVGL blits them 1:1. This matters
// more than it looks: a canvas smaller than its on-screen size goes through
// lv_img_set_zoom, and LVGL 8's transform path is per-pixel C with no fast
// route. Measured in flush_callback (the only place that sees real panel
// refreshes): with zoom, one refresh spent ~87ms drawing and the panel updated
// at 4-10Hz while the link delivered 20fps. VSYNC wait was only 10-15ms, so
// neither the panel (23.9Hz: 16MHz pclk over 1064x630 total) nor PSRAM
// bandwidth was the limit - the zoom transform was.
//
// Do not size a canvas by its source resolution "to save the upscale". The
// upscale happens either way; only the cost of who does it changes, and our
// own nearest-neighbour row copy beats LVGL's transform by an order of
// magnitude. Any claim about invalidate/tick counts is not a measurement of
// this: invalidates coalesce, so they overstate what reaches the panel.
static const int CAM_W = 374;   // on-screen feed size, and the canvas size
static const int CAM_H = 280;

// The peak marker on the thermal panel. One source pixel is CAM_W/THERMAL_W = 11.7
// canvas pixels, so a 14px ring is a little over one source pixel across - big enough
// to see against a busy false-colour image and small enough that it names one pixel
// rather than a region. Hollow, because a filled marker hides the reading it points at.
//
// PEAK_TEXT_W/H are the chip the reading sits in: "100.0℃" is the widest real string
// at font_bold_12 - five digits and a point at ~7px each plus the 12px degree glyph
// from the Hangul fallback, about 47px - plus 5px padding each side. Fixed rather
// than SIZE_CONTENT because place_peak_marker() clamps against the panel edge and
// needs the width before the layout pass that would compute it. It shrank from 92
// when the label stopped saying 최고: the ring names what the number is the maximum
// of, so two syllables of chip over a live image bought nothing.
static const int PEAK_RING   = 14;
static const int PEAK_TEXT_W = 58;
static const int PEAK_TEXT_H = 20;

// Shrinking the thermal canvas to cut its blit was measured and rejected: at
// 188x141 (52KB instead of 209KB, a quarter of the traffic) the refresh rate
// stayed at 4.2Hz - identical to the full-size canvas. Whatever costs the extra
// ~100ms per refresh when both panels draw is not proportional to canvas bytes,
// so paying for it with a smaller picture buys nothing.

// Live values, refreshed from the sensor node.
static float s_co2 = 612, s_temp = 24.5f, s_hum = 58;

static lv_obj_t *s_page = NULL;         // page root; used to skip work when hidden
static lv_timer_t *s_cam_timer = NULL;  // so page_monitor_on_show() can fire it early

// last_pct / last_ok are the bar's cache. lv_obj_set_width and
// lv_obj_set_style_bg_color refresh the style and invalidate whether or not the
// value moved, which on a 2Hz tick is a dirty rectangle per tile per second for
// numbers that mostly sit still. -1 is "never applied", so a theme rebuild - which
// recreates every widget and may swap the palette - re-applies both.
struct SensorWidgets {
    // The card itself, kept so a tile whose sensor is not fitted can be hidden. Flex
    // skips a hidden child entirely, so the remaining tiles widen to fill the strip
    // rather than leaving a gap where it would have been.
    lv_obj_t *card;
    int8_t shown;           // -1 always-on; 0/1 the cached HIDDEN state of an optional tile
    int8_t titled;          // -1 title not written yet; else agrees with shown
    lv_obj_t *name_label;   // recoloured to say whose band judged the reading
    lv_obj_t *value_label;
    lv_obj_t *bar_fill;
    // The band's two edges, marked on the bar itself. Before these the panel
    // tinted a reading amber and the threshold that decided it appeared nowhere on
    // the screen - not on the tile, not in the 최근 구간 table, whose band column
    // reports how much of the window was held and not where the band is. A grower
    // could see that something was wrong and not what "right" would have been.
    lv_obj_t *tick_lo;
    lv_obj_t *tick_hi;
    // The same two edges as figures, under the bar. See band_text().
    lv_obj_t *band_label;
    int16_t last_band_lo;   // tenths; INT16_MIN = that edge is unbounded
    int16_t last_band_hi;
    int16_t last_pct;
    int16_t last_tick_lo;   // -1 hidden; else the percent it sits at
    int16_t last_tick_hi;
    int8_t last_ok;         // -2 unset, -1 no band so no verdict, 0/1 the verdict
    int8_t last_src;        // -1 unset, 0 the panel's own default, 1 the server's band
};
static SensorWidgets s_co2w, s_tempw, s_humw, s_vpdw, s_dtw, s_luxw, s_soilw;


// Which transport feeds a panel: the CAM's HTTP MJPEG stream for RGB, the sensor
// node's ESP-NOW frames for thermal. Neither has a fallback, so this doubles as the
// panel's liveness - SRC_NONE is the offline state, drawn as a grey dot and a
// placeholder. It no longer names the transport on screen; the settings page does.
enum CamSrc { SRC_NONE, SRC_NET, SRC_THERMAL };

struct CamPanel {
    bool rgb;                  // false = thermal
    int buf_w, buf_h;          // canvas size, drawn 1:1 (no zoom transform)
    lv_color_t *buf;           // PSRAM canvas backing store
    lv_obj_t *canvas;
    lv_obj_t *center_label;    // placeholder text, hidden while live
    lv_obj_t *live_dot;
    lv_obj_t *peak_label;      // thermal only: the scene peak, the image's one absolute
    int16_t last_peak_dc;      // that peak in tenths of a degree, cached; INT16_MIN unset
    lv_obj_t *peak_ring;       // thermal only: hollow marker on the hottest pixel
    int32_t last_peak_idx;     // that pixel's frame index, cached; -2 unset, -1 none
    bool stream_shown;         // canvas currently holds live video
    bool pattern_painted;      // placeholder already drawn for the idle state
    CamSrc badge_src;          // last badge state, so the badge is edge-driven
    bool draw_pending;         // scaled a frame the panel has not shown yet
    uint32_t pend_refr;        // g_lvgl_refr_count when that frame was invalidated
};

// The panel refresh counter, incremented in lvgl_v8_port.cpp's flush_callback
// once per completed panel refresh. draw_pending clears when it moves past
// pend_refr - i.e. once the refresh that drew the pending frame has flushed.
extern volatile uint32_t g_lvgl_refr_count;

// Designated rather than positional. The positional form silently shifted every
// value one slot along when a field was inserted mid-struct, which the compiler
// caught only because a pointer landed on a bool -
// a new field of a compatible type would have compiled and mis-initialised the
// panel. Unnamed members are zero-initialised, which is the wanted default for
// every pointer and counter here.
static CamPanel s_rgb = {
    .rgb = true, .buf_w = CAM_W, .buf_h = CAM_H,
    .last_peak_dc = INT16_MIN, .last_peak_idx = -2, .badge_src = (CamSrc)-1,
};
static CamPanel s_therm = {
    .rgb = false, .buf_w = CAM_W, .buf_h = CAM_H,
    // -2 and not -1: -1 is the real "no position" state, and starting there would
    // suppress the first placement, which is the one that hides the ring and parks
    // the label in the corner.
    .last_peak_dc = INT16_MIN, .last_peak_idx = -2, .badge_src = (CamSrc)-1,
};

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

// One tile's band, and whose it is.
//
// The panel used to judge these six readings against thresholds compiled into
// this file while the 최근 구간 table directly beneath them was drawn from the
// server's own bands - two answers to "is this reading in band" on one screen,
// eight rows apart. plantrx_band() is the server's answer; the numbers here are
// only what the panel falls back to when it has never been given one.
//
// A side the server left unbounded stays NAN rather than being filled in from the
// fallback. Half the server's band and half the panel's is a third band neither
// of them asked for, and it would be the same bug wearing a disguise: "hold CO2
// under 1000" says nothing about a floor, so the tile must not invent one and
// then tint a reading against it.
struct TileBand {
    float lo, hi;   // NAN on an unbounded side
    bool  server;   // the server named this band
};

// A band the server has no vocabulary for, so there is nothing to look up and no
// fallback to fall back from. Spelled out rather than reached through tile_band()
// with an invented metric name: a lookup that cannot succeed is not a lookup, and
// writing one would leave the next reader hunting schema.py for a key that was
// never there.
static TileBand panel_band(float lo, float hi) {
    TileBand b;
    b.lo = lo;
    b.hi = hi;
    b.server = false;
    return b;
}

// The band this tile judges against: the server's if it named one, else the
// panel's own from aijudge.cpp's PANEL_BANDS. No numbers of its own - a threshold
// written here is a threshold that can drift from the rule's, and it did: this
// file used to tint VPD green to 1.5 kPa while the rule filed 증산 과다 경향 above
// 1.2, on one screen, at one moment.
static TileBand tile_band(const char *metric) {
    TileBand b;
    b.lo = NAN;
    b.hi = NAN;
    b.server = false;
    if (plantrx_band(metric, &b.lo, &b.hi)) {
        b.server = true;
        return b;
    }
    aijudge_panel_band(metric, &b.lo, &b.hi);
    return b;
}

// The value range the bar draws across, which is NOT the band.
//
// Two different things, and the tile needs both: the band is what the reading
// should be, the span is what the bar can show. A bar spanning only the band could
// not draw a reading outside it, which is the one case anybody looks at the tile
// for - so every span here is wider than its band on both sides, and the six were
// chosen against what a greenhouse actually reaches rather than against the
// thresholds.
//
// Collected into one struct because the band ticks need the same value-to-percent
// mapping the fill uses. It used to be six clampf() expressions written inline at
// six call sites, so a tick computed anywhere else would have been a seventh copy
// - and a tick that disagrees with its own bar by two pixels is worse than no
// tick, because it says the threshold is somewhere the fill never stops.
struct TileScale {
    float lo, hi;
};

// 2 rather than 0 as the floor: a bar of literal zero width reads as a missing
// widget, and the tile has a separate way of saying it has no reading.
static int scale_pct(float v, const TileScale &s) {
    return (int)clampf((v - s.lo) / (s.hi - s.lo) * 100.0f, 2, 100);
}

// Where a band edge sits on the bar, or -1 when it must not be drawn.
//
// An edge outside the bar's own span gets no tick. Clamping it to the end instead
// would park a threshold marker on the last pixel and claim the band ends exactly
// where the bar does, which is a number the panel invented; and NAN is a side the
// band does not bound at all, which is not a threshold that fell off the edge but
// the absence of one. 98 is the ceiling because the tick is 3px wide inside a
// ~108px bar - at 100 it would hang off the right end and be clipped to a sliver.
static int tick_pct(float edge, const TileScale &s) {
    if (isnan(edge)) return -1;
    if (edge < s.lo || edge > s.hi) return -1;
    int p = (int)clampf((edge - s.lo) / (s.hi - s.lo) * 100.0f, 0, 98);
    return p;
}

static bool in_band(float v, const TileBand &b) {
    if (!isnan(b.lo) && v < b.lo) return false;
    if (!isnan(b.hi) && v > b.hi) return false;
    return true;
}

// Move one band tick, or hide it. Cached on the percent it already sits at, so a
// band that has not changed - which is every tick of every poll that carries the
// same prescription - costs one compare and no invalidation.
//
// The serial line fires only when a tick actually moves, which over a normal day is
// a handful of times: once per tile at boot and again whenever a prescription
// changes a band. It is here because this is the one thing on the panel that cannot
// be checked any other way - the band the tile is judging against is a number
// inside plantrx.cpp, the tick is three pixels on a five-pixel bar, and "the panel
// is drawing the threshold in the wrong place" and "the panel has the wrong
// threshold" look identical on the wall. `of` is the track's resolved width, so a
// percentage that came out as a pixel can be read straight off the line;
// plantrx.cpp's own debug tick says which metrics are banded, and this says where
// the panel put them.
static void tick_set(lv_obj_t *tick, int16_t &cache, int pct, const char *who,
                     const char *edge) {
    if (cache == (int16_t)pct) return;
    cache = (int16_t)pct;
    if (pct < 0) {
        lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
        Serial.printf("[tile] %s %s band edge: none\n", who, edge);
        return;
    }
    lv_obj_set_x(tick, lv_pct(pct));
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_HIDDEN);
    // The track's width is only a number after a layout pass, and this line fires
    // from the first refresh - which on the landing page happens before one. "?"
    // rather than the 0 it reads as, because a diagnostic that prints a wrong
    // number is worse than one that admits it does not know yet.
    int track_w = (int)lv_obj_get_width(lv_obj_get_parent(tick));
    if (track_w > 0) {
        Serial.printf("[tile] %s %s band edge at %d%% of %dpx\n",
                      who, edge, pct, track_w);
    } else {
        Serial.printf("[tile] %s %s band edge at %d%% (track not laid out yet)\n",
                      who, edge, pct);
    }
}

// One tile with a reading: the text, the bar fill, the band's edges marked on that
// bar, and the colour the band decides. Every write is cached.
//
// Four channels, and they answer four different questions a grower actually asks.
// The fill says where the reading is. The two ticks say where the band is - nothing
// on this screen said that before, so an amber tile named a problem and hid the
// number that defined it. The line under the bar says what that band IS in figures,
// because a mark on a track is a position and a position is not a number: the ticks
// shipped without it and the first person to see them had to ask what they were.
// And the title's colour says whose band it is: blue is already this panel's word
// for the server (the uplink dot spends it on RX_WAITING, the settings block on
// 정상, the judgment countdown on its chip), and it costs no width, which the strip
// does not have - "잎-공기 온도차" alone is 81px of the 101 a tile has at seven tiles.
//
// The band's two numbers, under the bar the ticks mark.
//
// The ticks alone were half a feature. They put the thresholds in the right PLACE,
// which is what makes an amber bar explainable, but reading a value off a position
// needs the scale's own end points and those are nowhere on the tile. So the marks
// said "the limit is about here" and nothing said what the limit IS - the first
// person to look at them had to ask.
//
// A one-sided band is the common case, not an edge one: CO2 has a floor and no
// ceiling because too much CO2 is not a problem, and 잎-공기 온도차 has a ceiling and
// no floor because a leaf cooler than the air is transpiration working. Those read
// "400 이상" and "1.0 이하" rather than inventing a bound to make the range symmetric.
//
// No band at all hides the line, matching the grey bar above it: nothing to hold,
// nothing to print. Cached in tenths so a tick that does not move writes nothing.
static void band_text(SensorWidgets &w, const TileBand &b) {
    int16_t lo = isnan(b.lo) ? INT16_MIN : (int16_t)lroundf(b.lo * 10.0f);
    int16_t hi = isnan(b.hi) ? INT16_MIN : (int16_t)lroundf(b.hi * 10.0f);
    if (w.last_band_lo == lo && w.last_band_hi == hi) return;
    w.last_band_lo = lo;
    w.last_band_hi = hi;

    if (lo == INT16_MIN && hi == INT16_MIN) {
        lv_obj_add_flag(w.band_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(w.band_label, LV_OBJ_FLAG_HIDDEN);

    // %g so a whole number loses its ".0": "400 이상" and not "400.0 이상", which at
    // 81px of card is the difference between fitting and ellipsizing.
    char buf[24];
    if (lo != INT16_MIN && hi != INT16_MIN) {
        snprintf(buf, sizeof(buf), "%g~%g", lo / 10.0f, hi / 10.0f);
    } else if (lo != INT16_MIN) {
        snprintf(buf, sizeof(buf), "%g 이상", lo / 10.0f);
    } else {
        snprintf(buf, sizeof(buf), "%g 이하", hi / 10.0f);
    }
    ui_set_label_text(w.band_label, buf);
}

static void tile_reading(SensorWidgets &w, const char *txt, float v,
                         const TileScale &sc, const TileBand &b, bool ok) {
    ui_set_label_text(w.value_label, txt);
    int pct = scale_pct(v, sc);
    if (w.last_pct != (int16_t)pct) {
        w.last_pct = (int16_t)pct;
        lv_obj_set_width(w.bar_fill, lv_pct(pct));
    }
    // Three states, not two. Green and amber are verdicts and a metric nobody has
    // banded has not earned one - so an empty band draws the fill grey, which is
    // the same thing the 최근 구간 table does with a null in_band_pct one card
    // down. Cached as -1 so it is distinguishable from both booleans.
    bool banded = !isnan(b.lo) || !isnan(b.hi);
    int8_t verdict = banded ? (int8_t)ok : (int8_t)-1;
    if (w.last_ok != verdict) {
        w.last_ok = verdict;
        lv_obj_set_style_bg_color(w.bar_fill,
                                  !banded ? C_GRAY : ok ? C_GREEN : C_AMBER, 0);
    }
    const char *who = lv_label_get_text(w.name_label);
    tick_set(w.tick_lo, w.last_tick_lo, tick_pct(b.lo, sc), who, "lo");
    tick_set(w.tick_hi, w.last_tick_hi, tick_pct(b.hi, sc), who, "hi");
    band_text(w, b);
    int8_t src = b.server ? 1 : 0;
    if (w.last_src != src) {
        w.last_src = src;
        lv_obj_set_style_text_color(w.name_label,
                                    b.server ? C_BLUE : C_TEXT_SECONDARY, 0);
    }
}

// One tile with nothing to say. "--" over an empty bar is this page's convention
// for a value that is not available, whatever the reason; the colour is left
// alone, because a bar clamped to 2% has no colour worth writing. The title goes
// back to the panel's grey: with no reading there is no verdict, so there is
// nobody whose band reached it and the blue would be claiming otherwise.
//
// The ticks stay. The band did not stop existing because the sensor did, and
// hiding them would make a dead node look like a metric nobody has a target for -
// which is a different fault with a different fix.
static void tile_unavailable(SensorWidgets &w) {
    ui_set_label_text(w.value_label, "--");
    if (w.last_pct != 2) {
        w.last_pct = 2;
        lv_obj_set_width(w.bar_fill, lv_pct(2));
    }
    if (w.last_src != 0) {
        w.last_src = 0;
        lv_obj_set_style_text_color(w.name_label, C_TEXT_SECONDARY, 0);
    }
}

// Scene maximum minus air temperature, degC - derive.py's leaf_air_dt_c()
// (server/app/derive.py:93-97), including its round to two decimals. Positive
// means something in frame is hotter than the air, which is far more often a lamp
// or the pot rim than a stressed leaf; the tile keeps the server's name for it so
// the two never look like different quantities.
static float leaf_air_dt_c(float leaf_max_c, float temp_c) {
    return roundf((leaf_max_c - temp_c) * 100.0f) / 100.0f;
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

// repeating-linear-gradient(45deg, c1 0 10px, c2 10px 20px): band width is
// measured along the 45-degree gradient axis, so the (x+y) period is
// 2 * 10px * sqrt(2) ~= 28 -> half-period 14.
static void paint_cam_pattern(CamPanel &p) {
    if (p.buf == NULL) return;
    lv_color_t c1 = p.rgb ? C_RGB_GRAD1 : C_THERMAL_GRAD1;
    lv_color_t c2 = p.rgb ? C_RGB_GRAD2 : C_THERMAL_GRAD2;
    for (int y = 0; y < p.buf_h; y++) {
        lv_color_t *row = &p.buf[y * p.buf_w];
        for (int x = 0; x < p.buf_w; x++) {
            row[x] = (((x + y) / 14) & 1) ? c2 : c1;
        }
    }
    if (p.canvas != NULL) lv_obj_invalidate(p.canvas);
}

static CamSrc pick_source(CamPanel &p) {
    if (p.buf == NULL) return SRC_NONE;
    if (p.rgb) return camnet_live() ? SRC_NET : SRC_NONE;
    return thermal_live() ? SRC_THERMAL : SRC_NONE;
}

// Single source of truth for what one panel shows. Called every stream tick so
// the badge and placeholder stay accurate without a separate state machine.
static CamSrc sync_camera_display(CamPanel &p) {
    CamSrc src = pick_source(p);
    bool live = (src != SRC_NONE);

    if (live) {
        if (!p.stream_shown) {
            p.stream_shown = true;
            // After this live stretch ends we must repaint even though the idle
            // pattern for this panel was already drawn once.
            p.pattern_painted = false;
        }
    } else {
        // Repaint only on a live->offline transition, not on every idle tick.
        if (p.stream_shown || !p.pattern_painted) {
            paint_cam_pattern(p);
            p.pattern_painted = true;
        }
        p.stream_shown = false;
    }

    // Assert label visibility against `live` every tick (idempotent - only
    // touches LVGL on an actual change). This survives a full-UI rebuild
    // (dark-mode toggle) that recreates the label VISIBLE while the edge-state
    // still says "streaming".
    bool is_hidden = lv_obj_has_flag(p.center_label, LV_OBJ_FLAG_HIDDEN);
    if (live && !is_hidden) {
        lv_obj_add_flag(p.center_label, LV_OBJ_FLAG_HIDDEN);
    } else if (!live && is_hidden) {
        lv_obj_clear_flag(p.center_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (src != p.badge_src) {
        p.badge_src = src;
        // The dot is the whole liveness channel now. "MJPEG LIVE" / "ESP-NOW LIVE" /
        // "OFFLINE" used to sit beside it and named the transport, which was accurate
        // - and redundant twice over. A dead feed already replaces the picture with a
        // placeholder pattern and its centre text, so nothing about an offline panel
        // depended on the word; and the transport belongs on the settings page, where
        // the camera card reports both links separately and a maintainer is actually
        // looking for it.
        lv_obj_set_style_bg_color(p.live_dot, live ? C_LIVE_RED : C_GRAY, 0);
    }
    return src;
}

// Nearest-neighbor source maps for scaling the 320x240 RGB frame up to the
// canvas. Thermal scales itself (thermal_take_scaled takes the target size).
static int16_t s_sx_map[CAM_W];
static int16_t s_sy_map[CAM_H];
static bool s_scale_maps_ready = false;

static void build_scale_maps(void) {
    for (int dx = 0; dx < CAM_W; dx++) {
        s_sx_map[dx] = (int16_t)(dx * CAMNET_W / CAM_W);
    }
    for (int dy = 0; dy < CAM_H; dy++) {
        s_sy_map[dy] = (int16_t)(dy * CAMNET_H / CAM_H);
    }
    s_scale_maps_ready = true;
}

// Put the marker on the hottest pixel and the reading next to it.
//
// The label used to be pinned bottom-left, which is where a legend goes when it has
// no position to point at. Now the node ships one (include/camprov.h), so the number
// can sit where its measurement is - the difference between "something in this frame
// is 38.4C" and "THAT is 38.4C", which for finding a hot lamp or a dry leaf is the
// whole question.
//
// Three things this has to get right:
//
// 1. The marker must not cover the pixel it names. It is a hollow ring, and the text
//    goes beside it - flipped to whichever side has room, so a peak against the right
//    edge does not push the label off the panel.
// 2. The text must stay readable over an unknown colour. The palette's hot end is
//    near-white on most maps, and white-on-white is the label being absent again, so
//    the text carries its own dark chip rather than trusting the pixels behind it.
// 3. It must not twitch. A one-source-pixel move is 11.7 canvas pixels, and the
//    argmax legitimately hops between neighbours that read the same temperature. The
//    node picks the first maximum deterministically, and this only re-places when the
//    source index actually changes - a repaint per real move, not per frame.
static void place_peak_marker(void) {
    if (s_therm.peak_ring == NULL || s_therm.peak_label == NULL) return;

    int32_t idx = thermal_peak_index();
    if (idx == s_therm.last_peak_idx) return;
    s_therm.last_peak_idx = idx;

    if (idx < 0) {
        // No position, which on this build means no live frame: the ring has nothing
        // to point at and the number has nothing to belong to, so both go. The panel
        // above already says OFFLINE and draws its placeholder - a bare "--" chip
        // floating in a corner would add a widget and no fact.
        //
        // It is also the older node's case: a temperature with no position. Hiding
        // the number loses something real there, but a legend cannot say "somewhere
        // in this frame" now that its whole grammar is a marked pixel, and the
        // 잎-공기 온도차 tile carries thermal_max() into the strip regardless.
        lv_obj_add_flag(s_therm.peak_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_therm.peak_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_therm.peak_label, LV_OBJ_FLAG_HIDDEN);

    // Source pixel -> canvas pixel, at the pixel's CENTRE. The blit maps source
    // column px to canvas columns [px*W/32, (px+1)*W/32), so the centre is half a
    // source pixel along; aligning to the left edge instead would sit the ring up
    // and left of the thing it marks by ~6px on both axes.
    int px = (int)idx % THERMAL_W, py = (int)idx / THERMAL_W;
    int cx = (px * 2 + 1) * CAM_W / (THERMAL_W * 2);
    int cy = (py * 2 + 1) * CAM_H / (THERMAL_H * 2);

    lv_obj_clear_flag(s_therm.peak_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_therm.peak_ring, LV_ALIGN_TOP_LEFT,
                 cx - PEAK_RING / 2, cy - PEAK_RING / 2);

    // Left of the ring when the ring is in the right half, right of it otherwise, so
    // the text always grows into the panel. PEAK_TEXT_W is the widest real string
    // ("100.0℃" at font_bold_12) plus the chip's padding.
    bool text_left = cx > CAM_W / 2;
    int tx = text_left ? cx - PEAK_RING / 2 - 4 - PEAK_TEXT_W
                       : cx + PEAK_RING / 2 + 4;
    if (tx < 2) tx = 2;
    if (tx + PEAK_TEXT_W > CAM_W - 2) tx = CAM_W - 2 - PEAK_TEXT_W;
    int ty = cy - PEAK_TEXT_H / 2;
    if (ty < 2) ty = 2;
    if (ty + PEAK_TEXT_H > CAM_H - 2) ty = CAM_H - 2 - PEAK_TEXT_H;
    lv_obj_align(s_therm.peak_label, LV_ALIGN_TOP_LEFT, tx, ty);
}

static void cam_stream_timer_cb(lv_timer_t *t) {
    // Skip decoding/scaling entirely unless the feed is actually on screen. Test
    // the canvas itself, not the page's HIDDEN flag: lv_obj_invalidate() discards
    // an invalidation for an object that is hidden, clipped away, or off-screen,
    // so anything the canvas can't show is work LVGL will throw away. The flag
    // alone missed the off-screen case and the page scaled frames for nothing.
    if (s_rgb.canvas != NULL && !lv_obj_is_visible(s_rgb.canvas)) {
        // Off screen: nobody consumes the decoded RGB frame, so tell the puller to
        // drop its ~31ms JPEG decode to a low keep-alive rate and stop starving the
        // WiFi stack on core 0. The raw-JPEG uplink and aijudge's periodic peek keep
        // working. Driven here, off the same visibility test that already gates
        // scaling, because ui.cpp calls no on_hide hook - only page_monitor_on_show.
        camnet_set_viewing(false);
        return;
    }
    // Visible again: full-rate decode. on_show fires this timer early, so the
    // resume lands within a frame of returning to the page rather than a tick late.
    camnet_set_viewing(true);

    // Scale a new frame only after the panel has shown the previous one.
    //
    // The source links (RGB ~20fps, thermal a few fps) run far faster than the
    // panel refreshes here, and each scale writes a full 209KB canvas on this,
    // the LVGL thread. Without a gate the timer scaled a fresh frame every tick;
    // invalidates coalesce, so ~6 of every 7 were overwritten before they were
    // ever drawn - pure cost that slowed the very refresh meant to show them,
    // which let still more frames pile up.
    //
    // draw_pending breaks that: it is set when a frame is scaled and invalidated,
    // stamped with the refresh counter at that moment, and cleared once the
    // counter has moved on - meaning the refresh that drew the frame has flushed
    // to the panel. So exactly one frame is scaled per panel per refresh. (An
    // earlier attempt cleared it from an LV_EVENT_DRAW_MAIN_END handler on the
    // canvas; that event never reached the callback, so the flag stuck and the
    // feed froze. The flush counter is the signal that actually fires.)
    //
    // Both panels are taken in the same tick on purpose. In direct_mode with two
    // framebuffers LVGL copies the previous frame's dirty area that this frame
    // does not redraw, to keep the buffers consistent (lv_refr.c refr_sync_areas);
    // alternating panels pays that 209KB inter-buffer copy every refresh, redrawing
    // both leaves an empty difference and pays nothing.
    if (s_rgb.draw_pending && g_lvgl_refr_count != s_rgb.pend_refr) s_rgb.draw_pending = false;
    if (s_therm.draw_pending && g_lvgl_refr_count != s_therm.pend_refr) s_therm.draw_pending = false;

    CamSrc rgb_src = sync_camera_display(s_rgb);
    CamSrc therm_src = sync_camera_display(s_therm);
    if (rgb_src == SRC_NET && !s_rgb.draw_pending) {
        if (!s_scale_maps_ready) build_scale_maps();
        if (camnet_take_scaled((uint16_t *)s_rgb.buf, CAM_W, CAM_H, s_sx_map, s_sy_map)) {
            lv_obj_invalidate(s_rgb.canvas);
            s_rgb.draw_pending = true; s_rgb.pend_refr = g_lvgl_refr_count;
        }
    }
    if (therm_src == SRC_THERMAL && !s_therm.draw_pending) {
        if (thermal_take_scaled((uint16_t *)s_therm.buf, s_therm.buf_w, s_therm.buf_h)) {
            lv_obj_invalidate(s_therm.canvas);
            s_therm.draw_pending = true; s_therm.pend_refr = g_lvgl_refr_count;
        }
    }

    // The image's one absolute number, kept beside the frame it describes rather
    // than on the 2s sensor tick: the peak belongs to the picture on screen, and a
    // legend that lags its image by up to two seconds is a legend for the previous
    // frame. Cached in tenths of a degree because that is the resolution it is
    // drawn at - thermal_max() jitters below that and would otherwise repaint this
    // label at the stream's own frame rate.
    if (s_therm.peak_label != NULL) {
        float peak = thermal_max();
        int16_t dc = (reading_present(peak)) ? (int16_t)lroundf(peak * 10.0f) : INT16_MIN;
        if (dc != s_therm.last_peak_dc) {
            s_therm.last_peak_dc = dc;
            // Just the number. "최고" was there when this sat in a corner with nothing
            // to point at and had to name what it was the maximum of; the ring says
            // that now, and better - it names the pixel rather than the frame. Two
            // Hangul syllables of chip over a live image, for a word the marker
            // already spoke.
            //
            // No text for the absent case either: place_peak_marker() hides the whole
            // chip when there is no position, so this branch would be writing into a
            // widget nobody sees. It still runs, because the cache must agree with
            // what the label holds if a frame arrives before the next placement.
            if (dc == INT16_MIN) {
                ui_set_label_text(s_therm.peak_label, "--");
            } else {
                char buf[16];
                snprintf(buf, sizeof(buf), "%.1f\xE2\x84\x83", dc / 10.0f);
                ui_set_label_text(s_therm.peak_label, buf);
            }
        }
        place_peak_marker();
    }
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

// The page's single refresh entry point is page_monitor_refresh_sensors() below -
// ui_refresh_all() calls it after a theme rebuild, so nothing here needs its own
// hook in ui.cpp.
//
// MOCK DATA SWITCH. Delete this and the two tiles fall back to "not fitted": hidden
// until their sensor reports, which is the right production behaviour. It is on
// because the BH1750 and the soil probe are on order and the strip is being laid out
// now, at the width it will have when they arrive.
#define MOCK_LUX_SOIL 1

// A slow deterministic drift, so a mock tile's bar moves and the layout can be judged
// with something other than a frozen number. Not random: a value that jumps is a
// value somebody will chase.
static float mock_wave(float base, float amp, uint32_t period_ms) {
    float phase = (float)(millis() % period_ms) / (float)period_ms;
    return base + amp * sinf(phase * 2.0f * (float)M_PI);
}

// One tile that is either real or mock, and says which.
//
// `fitted` is latched in sensornode.cpp on the first genuine reading, so real data
// takes over permanently the moment the sensor exists - and a sensor that then dies
// keeps its tile and draws "--", because a channel that has reported once going
// quiet is a fault to show rather than a tile to remove.
//
// The title is where 임시 goes. Not the value, which has to stay a bare number for
// the layout to be worth judging, and not the colour, which on this tile already
// answers a different question - the name's colour is whose band decided the verdict.
// A suffix on the title is the one channel that was free. At seven tiles a card is
// 101px with 81px of content, and "토양습도 임시" is 76px of it.
static void mock_or_real(SensorWidgets &w, const char *title, bool fitted, float real,
                         float mock, const char *fmt, TileScale sc,
                         char *buf, size_t n) {
    if (w.card == NULL) return;

#if MOCK_LUX_SOIL
    bool use_mock = !fitted;
#else
    bool use_mock = false;
    (void)mock;
#endif

    // Hidden only in the production case: no mock, no sensor, no tile.
    int8_t want = (fitted || use_mock) ? 1 : 0;
    if (w.shown != want) {
        w.shown = want;
        if (want) lv_obj_clear_flag(w.card, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(w.card, LV_OBJ_FLAG_HIDDEN);
    }
    if (!want) return;

    // Title text is cached by the same int8_t: -1 means "not yet written".
    if (w.titled != want) {
        w.titled = want;
        if (use_mock) {
            snprintf(buf, n, "%s 임시", title);
            ui_set_label_text(w.name_label, buf);
        } else {
            ui_set_label_text(w.name_label, title);
        }
    }

    // No band either way. MetricKey has no name for lux or soil
    // (server/app/schema.py) so no server can band them, and the panel will not
    // invent a threshold - the bar draws grey, which is this page's "no band, no
    // verdict". A mock reading could not earn a verdict in any case.
    TileBand b = panel_band(NAN, NAN);
    float v = use_mock ? mock : real;
    if (reading_present(v)) {
        snprintf(buf, n, fmt, v);
        tile_reading(w, buf, v, sc, b, in_band(v, b));
    } else {
        tile_unavailable(w);
    }
}

void page_monitor_refresh_sensors(void) {
    char buf[24];
    bool online = sensornode_online();
    s_co2 = sensornode_co2();
    s_temp = sensornode_temp();
    s_hum = sensornode_hum();

    // The server's metric spellings - air_c and not temp_c, which is the sensor
    // field name for the same quantity (server/app/derive.py:21 maps between them).
    // No numbers here: tile_band() falls back to aijudge.cpp's one table.
    TileBand temp_b = tile_band(METRIC_AIR_C);
    TileBand hum_b  = tile_band(METRIC_RH);
    TileBand co2_b  = tile_band(METRIC_CO2);

    // The bars' own spans, each comfortably outside its band so a reading that has
    // gone wrong is still on the bar rather than pinned at an end. 15-35 degC and
    // 30-85 %RH are what a greenhouse reaches on a bad day; CO2 tops out at 1500
    // because an enriched house runs to about 1200 and the tile has to show that as
    // a position and not as a stop.
    static const TileScale TEMP_SC = { 15.0f, 35.0f };
    static const TileScale HUM_SC  = { 30.0f, 85.0f };
    static const TileScale CO2_SC  = { 400.0f, 1500.0f };

    if (online && reading_present(s_temp)) {
        snprintf(buf, sizeof(buf), "%.1f °C", s_temp);
        tile_reading(s_tempw, buf, s_temp, TEMP_SC, temp_b, in_band(s_temp, temp_b));
    } else {
        tile_unavailable(s_tempw);
    }

    if (online && reading_present(s_hum)) {
        snprintf(buf, sizeof(buf), "%d %%RH", (int)s_hum);
        tile_reading(s_humw, buf, s_hum, HUM_SC, hum_b, in_band(s_hum, hum_b));
    } else {
        tile_unavailable(s_humw);
    }

    if (online && reading_present(s_co2)) {
        snprintf(buf, sizeof(buf), "%d ppm", (int)s_co2);
        tile_reading(s_co2w, buf, s_co2, CO2_SC, co2_b, in_band(s_co2, co2_b));
    } else {
        tile_unavailable(s_co2w);
    }

    // VPD needs both halves of the SCD41's reading; one without the other is not a
    // deficit, it is half a formula, so the tile says nothing rather than guessing.
    // That test is not written out here any more: aijudge_vpd_kpa() answers an
    // absent input with the same < -999 sentinel, on the same `> -999.0f` rule this
    // page uses everywhere, so repeating it locally would only be a second place to
    // get it wrong. sensornode_online() it does have to be told: a frozen node
    // still hands back its last two numbers and only the caller knows they are old.
    // The bar spans 0 - 3 kPa, which puts a saturated house at the left stop and
    // one that is drying the plant out at the right. That span is the tile's own
    // drawing decision and stays here; the band that colours it is not the tile's
    // to choose.
    float vpd = online ? aijudge_vpd_kpa(s_temp, s_hum) : -1000.0f;
    TileBand vpd_b = tile_band(METRIC_VPD);
    static const TileScale VPD_SC = { 0.0f, 3.0f };
    if (reading_present(vpd)) {
        snprintf(buf, sizeof(buf), "%.1f kPa", vpd);
        tile_reading(s_vpdw, buf, vpd, VPD_SC, vpd_b, in_band(vpd, vpd_b));
    } else {
        tile_unavailable(s_vpdw);
    }

    // The scene peak, read once for the 잎-공기 row below.
    //
    // A 표면온도 tile used to sit on the strip drawing this same number. It was
    // removed once the thermal panel started marking the pixel the peak came from:
    // the same figure, in the one place where it means something, beside the ring
    // that says WHICH pixel. The tile could only ever say "something in frame is
    // 38.4C" - and it could not even give it a verdict, because MetricKey has no
    // name for a scene peak (server/app/schema.py) so no server can band it, and
    // the panel cannot either when the number is whatever happens to be warmest.
    // A number with no verdict, duplicated from a panel that now carries it better,
    // was costing a sixth of the strip.
    //
    // Nothing is lost. Genuinely abnormal heat still reaches the log through
    // 잎-공기 온도차, which subtracts air temperature and therefore HAS a
    // defensible band - a heater fault drives that far outside it while a lamp at a
    // steady 45 degC does not.
    float tmax = thermal_max();

    // 잎-공기: the thermal scene peak against air temperature. Both halves have to
    // be current and each is gated by its own link: thermal_max() sits below -999
    // unless thermal_live() is true, and sensornode_online() says the air reading
    // it is measured against is still arriving. That pairing is the whole point -
    // an MLX90640 that dies while the SCD41 keeps broadcasting used to leave this
    // tile subtracting a temperature from noon out of one from now, drawing the
    // result as a single measurement and tinting it against a +-3 degC band. The
    // bar spans -5 .. +15 degC because the interesting failure is one-sided: a lamp
    // or a heater in frame drives it far positive, transpiration only ever pulls a
    // leaf a few degrees under. That span is the tile's; the band is not - it now
    // comes from the one table aijudge.cpp's rule reads, which is why this tile
    // goes amber above 1 degC instead of 3 and can no longer draw green over a
    // reading the log beside it filed 엽온 상승 추세 about.
    float dt = (online && reading_present(s_temp) && reading_present(tmax))
                   ? leaf_air_dt_c(tmax, s_temp) : -1000.0f;
    TileBand dt_b = tile_band(METRIC_LEAF_DT);
    static const TileScale DT_SC = { -5.0f, 15.0f };
    if (reading_present(dt)) {
        snprintf(buf, sizeof(buf), "%.1f °C", dt);
        tile_reading(s_dtw, buf, dt, DT_SC, dt_b, in_band(dt, dt_b));
    } else {
        tile_unavailable(s_dtw);
    }

    // The two sensors that are ON ORDER. Neither is broken and neither was dropped:
    // the hardware has not arrived, and both are going in.
    //
    // Until it does, these tiles carry MOCK data so the strip can be judged at its
    // real width with plausible numbers in it. Two rules make that safe rather than a
    // lie, and both are load-bearing:
    //
    //  1. The mock never leaves this function. sensornode_lux() and _soil() keep
    //     returning their absent sentinel, so plantrx.cpp:1151-1152 keeps sending
    //     null, the server keeps having no sample for them, and _setpoints keeps
    //     dropping any band on them. Not one fabricated reading reaches the wire, the
    //     database or the model. Substituting inside the getters would have been one
    //     line shorter and would have poisoned all four.
    //  2. The tile SAYS it is mock, in its title, where the reading cannot be read
    //     without it. An unlabelled plausible number is the exact failure this whole
    //     panel has been stripped of - and it would be the worst instance of it,
    //     because it would be wrong on purpose.
    //
    // Real hardware wins the moment it speaks: sensornode_has_*() latches on the
    // first genuine reading, the title drops 임시 and the values come from the node.
    // Nothing needs editing on the day the parcel arrives - but delete MOCK_LUX_SOIL
    // anyway, so the next reader is not left wondering whether it is still on.
    mock_or_real(s_luxw, "조도", sensornode_has_lux(),
                 online ? sensornode_lux() : -1000.0f,
                 mock_wave(600.0f, 500.0f, 90000), "%.0f lx",
                 (TileScale){ 0.0f, 2000.0f }, buf, sizeof(buf));
    mock_or_real(s_soilw, "토양습도", sensornode_has_soil(),
                 online ? sensornode_soil() : -1000.0f,
                 mock_wave(48.0f, 10.0f, 240000), "%.0f %%",
                 (TileScale){ 0.0f, 100.0f }, buf, sizeof(buf));
}

// Do nothing while the page is hidden. The tiles and the window table are ~20
// string and style writes every 2s, and every one of them dirties a rectangle
// LVGL then throws away for a page nobody can see. Same gate, and the same
// reason, as page_auto.cpp and page_settings.cpp.
static void sensor_timer_cb(lv_timer_t *t) {
    if (s_page != NULL && lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) return;
    page_monitor_refresh_sensors();
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

static lv_obj_t *build_sensor_card(lv_obj_t *parent, const char *title, SensorWidgets &w) {
    lv_obj_t *c = card(parent, C_SURFACE, 12, true);
    w.card = c;
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_height(c, LV_PCT(100));
    flex_col(c, 0, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START);  // title top, value mid, bar bottom
    pad_all(c, 0);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 8, 0);
    w.name_label = label(c, title, &font_bold_12, C_TEXT_SECONDARY);  // full-Hangul fallback
    w.value_label = label(c, "--", &font_bold_19, C_TEXT_DARK);
    lv_obj_t *track = box(c, LV_PCT(100), 5, C_PROGRESS_TRACK, 3);
    w.bar_fill = box(track, 0, 5, C_GREEN, 3);
    lv_obj_set_style_pad_all(track, 0, 0);

    // The band edges, after the fill so they draw over it - a tick the fill covers
    // is a tick that disappears exactly when the reading crosses the threshold,
    // which is the moment it has to be visible. C_TEXT_DARK because it is this
    // palette's maximum contrast against the surface and both themes swap it with
    // the surface: the track and the green/amber fill are the two mid-tones it has
    // to read against, and it reads against both in either theme.
    //
    // 3px wide, square, no radius. On a 133 DPI panel 2px is 0.4mm and disappears
    // at arm's length; 3px of a ~108px bar is under 3% of the span, which is finer
    // than the reading the bar is drawn from. Height matches the track exactly
    // because LVGL 8 clips a child to its parent, so a taller tick would be cut
    // back to this anyway.
    w.tick_lo = box(track, 3, 5, C_TEXT_DARK, 0);
    w.tick_hi = box(track, 3, 5, C_TEXT_DARK, 0);
    lv_obj_add_flag(w.tick_lo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w.tick_hi, LV_OBJ_FLAG_HIDDEN);

    // The band in figures, under the track the ticks sit on. font_reg_12 because
    // the one-sided forms are Korean ("400 이상") and this is a runtime write, so
    // it needs a font carrying a fallback - see server/tests/test_font_coverage.py
    // rule A. Secondary colour because this is a reference value, not a reading;
    // the number a grower reads is the big one two rows up.
    //
    // Built hidden. A metric with no band never shows it, and band_text() is what
    // reveals the line for one that has an edge.
    w.band_label = label(c, "", &font_reg_12, C_TEXT_SECONDARY);
    lv_obj_add_flag(w.band_label, LV_OBJ_FLAG_HIDDEN);

    // A rebuild recreates these widgets, so the cache must not survive it. last_src
    // starts unset rather than 0: a rebuild that lands while the server's band is
    // live must repaint the blue, and 0 would read as "already grey, nothing to do".
    // The tick caches start at a percent no tick can hold for the same reason -
    // -1 is the hidden state they are actually in, so it would suppress the first
    // placement.
    w.last_pct = -1;
    w.last_ok = -2;         // -1 is now a real state; see SensorWidgets
    w.last_src = -1;
    w.last_tick_lo = -2;
    w.last_tick_hi = -2;
    // -2 rather than INT16_MIN: INT16_MIN is the real "unbounded" value, so starting
    // there would suppress the first write for a metric that genuinely has one edge.
    w.last_band_lo = -2;
    w.last_band_hi = -2;
    return c;
}

// One camera card: nothing but the feed, with the status overlaid on it. The
// title lives in the badge rather than a header row so the picture gets the
// whole card.
static void build_cam_card(lv_obj_t *parent, CamPanel &p, const char *title,
                           const char *placeholder) {
    lv_obj_t *c = card(parent, C_SURFACE, 14, true);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_height(c, CAM_H + 2);   // + the card's 1px borders
    pad_all(c, 0);
    lv_obj_set_style_clip_corner(c, true, 0);

    lv_obj_t *img_area = plain(c);
    lv_obj_set_size(img_area, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(img_area, 0, 0);

    box(img_area, LV_PCT(100), LV_PCT(100), p.rgb ? C_RGB_GRAD1 : C_THERMAL_GRAD1, 0);

    // A theme rebuild re-runs this builder; the PSRAM buffer is allocated once.
    if (p.buf == NULL) {
        p.buf = (lv_color_t *)heap_caps_malloc(p.buf_w * p.buf_h * sizeof(lv_color_t),
                                               MALLOC_CAP_SPIRAM);
    }
    if (p.buf != NULL) {
        p.canvas = lv_canvas_create(img_area);
        lv_canvas_set_buffer(p.canvas, p.buf, p.buf_w, p.buf_h, LV_IMG_CF_TRUE_COLOR);
        // 1:1 blit: the canvas is already the on-screen size, so LVGL takes its
        // per-row copy path instead of the per-pixel zoom transform.
        lv_obj_align(p.canvas, LV_ALIGN_CENTER, 0, 0);
    }

    p.center_label = label(img_area, placeholder, &font_reg_12, C_WHITE);
    lv_obj_set_style_text_letter_space(p.center_label, 1, 0);
    lv_obj_set_style_bg_color(p.center_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p.center_label, 71, 0);  // rgba(0,0,0,.28)
    lv_obj_set_style_radius(p.center_label, 6, 0);
    lv_obj_set_style_pad_hor(p.center_label, 10, 0);
    lv_obj_set_style_pad_ver(p.center_label, 6, 0);
    lv_obj_align(p.center_label, LV_ALIGN_CENTER, 0, 0);
    ignore_layout(p.center_label);

    // Top-left: which camera this is, and whether it is live.
    lv_obj_t *badge = plain(img_area);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ignore_layout(badge);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_40, 0);
    lv_obj_set_style_radius(badge, 10, 0);
    flex_row(badge, 5, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(badge, 8, 0);
    lv_obj_set_style_pad_ver(badge, 3, 0);
    p.live_dot = box(badge, 6, 6, C_LIVE_RED, LV_RADIUS_CIRCLE);
    label(badge, title, &font_bold_12, C_WHITE);   // full-Hangul fallback

    // Thermal panel only: the scene peak, in degrees, ON the pixel it was measured at.
    //
    // This is the one number the wire carries alongside the pixels, and since the node
    // also ships where it came from (include/camprov.h) the number can sit there
    // rather than in a corner. Nothing else in the picture is a temperature: the
    // palette is applied on the sensor node and its mapping never reaches this board,
    // so no colour here decodes to degrees and two frames cannot be compared by eye.
    // That is exactly why a marked pixel is worth the widgets - it is the only place
    // in the image where the colour has a known meaning.
    //
    // The RGB frame has no measurement in it at all, so neither widget exists there;
    // an empty label would be a slot a future reader tries to fill.
    if (!p.rgb) {
        // Ring first, so the label draws over it where they touch. A 2px white border
        // with no fill: the pixel being named stays visible through the middle, which
        // a filled dot would hide. Border and not a glyph, because a ring is the one
        // shape the icon font does not have and a box() with a radius is free.
        p.peak_ring = box(img_area, PEAK_RING, PEAK_RING, lv_color_black(),
                          LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(p.peak_ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(p.peak_ring, C_WHITE, 0);
        lv_obj_set_style_border_width(p.peak_ring, 2, 0);
        lv_obj_set_style_border_opa(p.peak_ring, LV_OPA_COVER, 0);
        ignore_layout(p.peak_ring);
        lv_obj_add_flag(p.peak_ring, LV_OBJ_FLAG_HIDDEN);  // until a position arrives

        // font_bold_12, because this label is written "최고 24.1℃" and font_bold_10
        // has `.fallback = NULL` - no 최, no 고, no U+2103. LVGL stops drawing a
        // string at the first glyph it cannot resolve, so on font_bold_10 this
        // readout rendered as nothing at all while logging a miss on every one of
        // ~15 repaints a second. The number the whole legend exists to show was
        // never on the screen.
        //
        // The chip behind it is not decoration. Free-floating white text was legible
        // only because it lived on the dark bottom-left corner; over the palette's hot
        // end - near-white on most maps, and that is precisely where this label now
        // goes - it would be invisible again, in a different way. Opacity is full on
        // the text and 60% on the chip, so the picture still reads through it.
        p.peak_label = label(img_area, "--", &font_bold_12, C_WHITE);
        lv_obj_set_size(p.peak_label, PEAK_TEXT_W, PEAK_TEXT_H);
        lv_obj_set_style_bg_color(p.peak_label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(p.peak_label, LV_OPA_60, 0);
        lv_obj_set_style_radius(p.peak_label, 4, 0);
        lv_obj_set_style_pad_hor(p.peak_label, 5, 0);
        lv_obj_set_style_pad_ver(p.peak_label, 3, 0);
        lv_obj_set_style_text_align(p.peak_label, LV_TEXT_ALIGN_CENTER, 0);
        ignore_layout(p.peak_label);
        lv_obj_align(p.peak_label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    }
}

lv_obj_t *page_monitor_build(lv_obj_t *parent) {
    // A theme rebuild re-runs this and recreates the widgets fresh; clear the
    // display state so the first sync re-applies label/badge/placeholder
    // instead of trusting stale "still streaming" flags from the old widgets.
    s_rgb.stream_shown = s_therm.stream_shown = false;
    s_rgb.pattern_painted = s_therm.pattern_painted = false;
    s_rgb.badge_src = s_therm.badge_src = (CamSrc)-1;
    // The peak caches too, for the same reason: the widgets they describe no longer
    // exist. last_peak_idx back to -2 so the first placement runs whatever the
    // position is, including "none", which is what hides a freshly built ring.
    s_rgb.last_peak_dc = s_therm.last_peak_dc = INT16_MIN;
    s_rgb.last_peak_idx = s_therm.last_peak_idx = -2;

    lv_obj_t *page = plain(parent);
    s_page = page;
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    // 16px padding and a 16px row gap: two children, cameras then sensor strip.
    //
    // These were squeezed to 8 to free 62px for a 최근 구간 table that used to sit
    // below the strip. The table is gone, so the 62 goes back where it came from -
    // 16 to decoration and 46 to the strip, which is flex-grown again rather than
    // pinned at 64. The cameras never moved for it and do not move now: 374x280 is
    // the 1:1 blit size every measurement at the top of this file was taken at, and
    // a smaller canvas puts LVGL back on the per-pixel zoom path this page exists
    // to avoid.
    flex_col(page, 16, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(page, 16);

    lv_obj_t *cams = plain(page);
    lv_obj_set_width(cams, LV_PCT(100));
    lv_obj_set_height(cams, LV_SIZE_CONTENT);
    flex_row(cams, 16, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // The badge is now a dot and a name, nothing else. "RGB CAM" / "IR CAM" name the
    // DEVICE; what the pictures are is the placeholder's job, what the thermal one
    // cannot be read as is the marker's, and which transport carries either is the
    // settings page's - it reports both camera links there separately. ASCII here is
    // not a font constraint: the badge is on font_bold_12 and could spell anything.
    build_cam_card(cams, s_rgb, "RGB CAM", "RGB CAMERA FEED");
    build_cam_card(cams, s_therm, "IR CAM", "THERMAL CAMERA FEED");

    // Sensors: one horizontal strip taking whatever the cameras leave - 110px of the
    // 440px content column. Five cards over 768px with 8px gaps is 145px each and
    // seven is 101px; the widest value ("1013 ppm", 93px at font_bold_19) clears both,
    // and the longest title ("잎-공기 온도차", 81px at font_bold_12) clears both too.
    // Which of the two it is depends on the hardware, below.
    lv_obj_t *strip = plain(page);
    lv_obj_set_width(strip, LV_PCT(100));
    lv_obj_set_flex_grow(strip, 1);
    flex_row(strip, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Three measured, two derived, always present.
    build_sensor_card(strip, "온도", s_tempw);
    build_sensor_card(strip, "습도", s_humw);
    build_sensor_card(strip, "VPD", s_vpdw);
    build_sensor_card(strip, "잎-공기 온도차", s_dtw);
    build_sensor_card(strip, "CO2", s_co2w);

    // And two that appear only if this installation actually has them. Built here and
    // hidden, revealed by refresh_optional_tiles() the first time their sensor
    // reports - so the widgets and their band arithmetic are identical to every other
    // tile's, and only the visibility differs.
    //
    // Neither is fitted on this board: the BH1750 is broken and the soil probe was
    // never wired. They were on the strip drawing "--" on every tick of their lives,
    // which on a wall panel is indistinguishable from a sensor that dropped out this
    // minute - two fifths of the strip saying nothing, and saying it in the same
    // shape a real fault uses. Deleting them outright was worse in the other
    // direction: it made the panel silent about hardware this design has and put the
    // recovery behind a code change. Gating on "has ever reported" is both answers at
    // once, and it needs no inventory hard-coded anywhere.
    //
    // 표면온도 is a different case and genuinely gone: it duplicated a number the
    // thermal panel now prints on the pixel it came from.
    build_sensor_card(strip, "조도", s_luxw);
    build_sensor_card(strip, "토양습도", s_soilw);
    lv_obj_add_flag(s_luxw.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_soilw.card, LV_OBJ_FLAG_HIDDEN);
    // Both caches start at the state the widgets are actually in - hidden, title never
    // written - so mock_or_real()'s first call does the reveal and the naming rather
    // than deciding it has already happened.
    s_luxw.shown = s_soilw.shown = 0;
    s_luxw.titled = s_soilw.titled = -1;

    sync_camera_display(s_rgb);
    sync_camera_display(s_therm);

    static lv_timer_t *s_sensor_timer = NULL;
    if (s_sensor_timer == NULL) {
        // 2s, matching the node's broadcast. At 4s each a fresh reading could
        // sit a full broadcast period behind before the tiles even looked at it.
        s_sensor_timer = lv_timer_create(sensor_timer_cb, 2000, NULL);
        // Poll at the LVGL refresh period (30ms); the old 100ms tick capped
        // the on-screen video at 10fps even when the link ran faster.
        s_cam_timer = lv_timer_create(cam_stream_timer_cb, 33, NULL);
    }
    return page;
}

// Called when the monitor page becomes visible again. The stream tick skips its
// work while the page is hidden, so on the way back the canvases would keep
// showing the last frame until the timer's next natural fire - up to a full 33ms
// period on top of the page's own redraw. Fire it now instead; a decoded frame
// is already waiting.
void page_monitor_on_show(void) {
    // show_page() only cleared the HIDDEN flag; the coordinates are still the
    // stale off-screen ones until a layout pass runs. Update them now, or the
    // tick below sees an invisible canvas and skips the frame it was fired for.
    if (s_page != NULL) {
        lv_obj_update_layout(s_page);
    }
    if (s_cam_timer != NULL) {
        lv_timer_ready(s_cam_timer);
    }
    // The sensor tick is gated on this page being visible, so on the way back the
    // tiles and the window table are up to one 2s period stale. Redraw them now,
    // for the same reason the camera timer is fired early.
    page_monitor_refresh_sensors();
}
