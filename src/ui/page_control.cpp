// Control page: 3x3 device grid (fan spans 2 columns), fan slider, all-stop.
#include <stdio.h>
#include "ui_internal.h"

static ToggleWidgets t_fan, t_heater, t_mist, t_pumpA, t_pumpB, t_pumpC, t_led;
static lv_obj_t *st_fan, *st_heater, *st_mist, *st_pumpA, *st_pumpB, *st_pumpC, *st_led;
static lv_obj_t *w_fan_slider;
static lv_obj_t *w_undo, *w_undo_left;

// The fan card prints its percentage as the on-state, so a reachable 0 lets the
// card read ON and 0% at the same time, with the slider live at a value that
// runs nothing. Flooring the range deletes the contradictory value instead of
// papering over it in the label, which keeps the toggle the only on/off control
// and leaves the slider meaning speed and nothing else. The floor sits at 20
// because that is where the number stops describing anything the user can
// observe: a slider reading 5% beside a fan that is not visibly moving air is
// the same lie the ON/0% pair was, just harder to catch.
static const int FAN_MIN_PCT = 20;

// ---------------------------------------------------------------------------
// All-stop undo
// ---------------------------------------------------------------------------

// 전체 정지 must act on the first press. A press-and-hold or a confirm dialog
// would put a delay in front of the one control that cannot have one, so the
// mis-tap is caught after the fact instead: the pre-press state is kept here
// and offered back for a few seconds while the stop itself is already done.
struct AllStopSnapshot {
    bool fan, heater, mist, pumpA, pumpB, pumpC, led;
    int fan_speed;
};

// All seven cards flip to OFF in the same frame as the press, so the window only
// has to cover notice-then-reach across a wall-mounted panel with the pill in
// the opposite corner. Six seconds does that. Longer and the pill outlives the
// interaction that produced it, which is exactly how a restore starts arriving
// as a surprise; shorter and a grower with both hands in a tray misses it.
static const int UNDO_SECONDS = 6;

static AllStopSnapshot s_undo_snap;
static bool s_undo_pending;
static int s_undo_left;
// 1Hz countdown, created once at page build and paused whenever no undo is
// pending. Re-arming it per press would leak one timer per tap of 전체 정지.
static lv_timer_t *s_undo_timer;

static void undo_clear(void) {
    s_undo_pending = false;
    s_undo_left = 0;
    lv_timer_pause(s_undo_timer);
    lv_obj_add_flag(w_undo, LV_OBJ_FLAG_HIDDEN);
}

static void undo_paint_left(void) {
    char buf[8];   // "6초" is 4 bytes; UNDO_SECONDS is single-digit by construction
    snprintf(buf, sizeof(buf), "%d초", s_undo_left);
    lv_label_set_text(w_undo_left, buf);
}

static void undo_timer_cb(lv_timer_t *t) {
    if (--s_undo_left <= 0) {
        undo_clear();
        return;
    }
    undo_paint_left();
}

static void on_undo(lv_event_t *e) {
    // The pill can be tapped in the same frame the window closes, and a restore
    // that lands after the user has moved on would turn the heater back on
    // unasked - worse than the mis-tap this exists to reverse.
    if (!s_undo_pending) return;
    g_fan = s_undo_snap.fan;
    g_heater = s_undo_snap.heater;
    g_mist = s_undo_snap.mist;
    g_pumpA = s_undo_snap.pumpA;
    g_pumpB = s_undo_snap.pumpB;
    g_pumpC = s_undo_snap.pumpC;
    g_led = s_undo_snap.led;
    g_fan_speed = s_undo_snap.fan_speed;
    ui_prefs_mark_dirty();   // the restore is state too, and it must outlive a power cut
    undo_clear();          // hide the pill in the same repaint as the restore
    ui_devices_refresh();
}

static void on_all_stop_pressed(lv_event_t *e) {
    // A second press inside the window must not re-snapshot: the state it would
    // capture is the all-off the first press produced, and the undo would then
    // restore nothing.
    if (!s_undo_pending) {
        s_undo_snap.fan = g_fan;
        s_undo_snap.heater = g_heater;
        s_undo_snap.mist = g_mist;
        s_undo_snap.pumpA = g_pumpA;
        s_undo_snap.pumpB = g_pumpB;
        s_undo_snap.pumpC = g_pumpC;
        s_undo_snap.led = g_led;
        s_undo_snap.fan_speed = g_fan_speed;
    }
    on_all_stop(e);   // the all-off itself stays in ui.cpp, beside the toggles
    s_undo_pending = true;
    s_undo_left = UNDO_SECONDS;
    undo_paint_left();
    lv_obj_clear_flag(w_undo, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_undo_timer);   // a fresh window, not the remainder of the old one
    lv_timer_resume(s_undo_timer);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

// Touching a device is the user taking manual control, and a restore must never
// fire after that: putting the heater back on because the grower switched it off
// two seconds ago is the stale-restore case. Each of these fronts ui.cpp's
// handler rather than duplicating the state flip.
static void on_dev_fan(lv_event_t *e)    { undo_clear(); on_toggle_fan(e); }
static void on_dev_heater(lv_event_t *e) { undo_clear(); on_toggle_heater(e); }
static void on_dev_mist(lv_event_t *e)   { undo_clear(); on_toggle_mist(e); }
static void on_dev_pumpA(lv_event_t *e)  { undo_clear(); on_toggle_pumpA(e); }
static void on_dev_pumpB(lv_event_t *e)  { undo_clear(); on_toggle_pumpB(e); }
static void on_dev_pumpC(lv_event_t *e)  { undo_clear(); on_toggle_pumpC(e); }
static void on_dev_led(lv_event_t *e)    { undo_clear(); on_toggle_led(e); }

static void on_fan_slider(lv_event_t *e) {
    // An all-stop leaves g_fan false, which disables the slider, so this cannot
    // currently run with an undo pending. It cancels anyway because g_fan_speed
    // is in the snapshot: a restore must never fight a hand on the slider.
    undo_clear();
    int v = lv_slider_get_value(w_fan_slider);
    v = ((v + 2) / 5) * 5;  // step=5, like the design's range input
    lv_slider_set_value(w_fan_slider, v, LV_ANIM_OFF);
    g_fan_speed = v;
    // A drag fires this on every touch move. Marking rather than writing is what
    // keeps that one NVS write per flush interval instead of one per event.
    ui_prefs_mark_dirty();
    ui_devices_refresh();
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void page_control_refresh(void) {
    // No "ON" branch for the fan: the percentage IS its on-state, and it can
    // never be 0 because the slider's range starts at FAN_MIN_PCT.
    char fan_str[8];
    if (g_fan) {
        snprintf(fan_str, sizeof(fan_str), "%d%%", g_fan_speed);
    } else {
        snprintf(fan_str, sizeof(fan_str), "OFF");
    }

    update_toggle(t_fan, g_fan, C_BLUE);
    update_toggle(t_heater, g_heater, C_AMBER);
    update_toggle(t_mist, g_mist, C_BLUE);
    update_toggle(t_pumpA, g_pumpA, C_BLUE);
    update_toggle(t_pumpB, g_pumpB, C_BLUE);
    update_toggle(t_pumpC, g_pumpC, C_BLUE);
    update_toggle(t_led, g_led, C_GREEN);

    set_status(st_fan, g_fan, C_BLUE, fan_str);
    set_status(st_heater, g_heater, C_AMBER, g_heater ? "ON" : "OFF");
    set_status(st_mist, g_mist, C_BLUE, g_mist ? "ON" : "OFF");
    set_status(st_pumpA, g_pumpA, C_BLUE, g_pumpA ? "ON" : "OFF");
    set_status(st_pumpB, g_pumpB, C_BLUE, g_pumpB ? "ON" : "OFF");
    set_status(st_pumpC, g_pumpC, C_BLUE, g_pumpC ? "ON" : "OFF");
    set_status(st_led, g_led, C_GREEN, g_led ? "ON" : "OFF");

    lv_slider_set_value(w_fan_slider, g_fan_speed, LV_ANIM_OFF);
    if (g_fan) {
        lv_obj_clear_state(w_fan_slider, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(w_fan_slider, LV_STATE_DISABLED);
    }
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

static lv_obj_t *build_device_card(lv_obj_t *parent, const char *letter, lv_color_t badge_bg,
                                   lv_color_t badge_fg, const char *name, ToggleWidgets &t, lv_obj_t **status_out,
                                   lv_event_cb_t cb) {
    lv_obj_t *c = card(parent, C_SURFACE, 14, true);
    flex_col(c, 0, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START);
    pad_all(c, 14);

    build_device_header(c, letter, badge_bg, badge_fg, name, t, cb);
    // 요청, not 현재 상태: nothing on this board acts on the switch, so the only
    // fact the card holds is what the grower asked for. See build_status_line.
    build_status_line(c, status_out, "요청");
    return c;
}

static lv_obj_t *build_fan_card(lv_obj_t *parent) {
    lv_obj_t *c = card(parent, C_SURFACE, 14, true);
    flex_col(c, 8, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START);
    pad_all(c, 14);

    build_device_header(c, ICON_FAN, C_BLUE_TINT, C_BLUE, "냉각팬", t_fan, on_dev_fan);

    lv_obj_t *bottom = plain(c);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_height(bottom, LV_SIZE_CONTENT);
    flex_col(bottom, 4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Room for the fan slider's 18px knob plus its drop shadow; overflow
    // stays visible so nothing clips at the wrapper edge.
    lv_obj_set_style_pad_ver(bottom, 10, 0);
    lv_obj_add_flag(bottom, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *speed_row = plain(bottom);
    lv_obj_set_width(speed_row, LV_PCT(100));
    lv_obj_set_height(speed_row, LV_SIZE_CONTENT);
    flex_row(speed_row, 0, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);
    label(speed_row, "속도", &font_reg_12, C_TEXT_SECONDARY);
    st_fan = label(speed_row, "65%", &font_bold_12, C_BLUE);

    w_fan_slider = lv_slider_create(bottom);
    lv_slider_set_range(w_fan_slider, FAN_MIN_PCT, 100);
    lv_slider_set_value(w_fan_slider, g_fan_speed, LV_ANIM_OFF);
    lv_obj_set_width(w_fan_slider, LV_PCT(100));
    lv_obj_set_height(w_fan_slider, 6);
    lv_obj_set_style_bg_color(w_fan_slider, C_RANGE_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(w_fan_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(w_fan_slider, 3, LV_PART_MAIN);
    // The design's range input shows a uniform track with no filled portion.
    lv_obj_set_style_bg_opa(w_fan_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(w_fan_slider, C_WHITE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(w_fan_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(w_fan_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(w_fan_slider, 6, LV_PART_KNOB);  // 6px track + 12 -> 18px knob
    lv_obj_set_style_border_width(w_fan_slider, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(w_fan_slider, C_BLUE, LV_PART_KNOB);
    lv_obj_set_style_border_color(w_fan_slider, C_GRAY, (uint32_t)LV_PART_KNOB | (uint32_t)LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(w_fan_slider, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_ofs_y(w_fan_slider, 1, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(w_fan_slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(w_fan_slider, 77, LV_PART_KNOB);
    lv_obj_add_event_cb(w_fan_slider, on_fan_slider, LV_EVENT_VALUE_CHANGED, NULL);
    return c;
}

static lv_obj_t *build_allstop_card(lv_obj_t *parent) {
    lv_obj_t *c = card(parent, C_ALLSTOP_BG, 14, true);
    lv_obj_set_style_border_color(c, C_ALLSTOP_BORDER, 0);
    flex_col(c, 6, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pad_all(c, 14);
    clickable(c);
    lv_obj_add_event_cb(c, on_all_stop_pressed, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = box(c, 32, 32, C_ALLSTOP_BORDER, 8);
    lv_obj_set_flex_flow(icon, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    box(icon, 13, 13, C_WHITE, 2);

    label(c, "전체 정지", &font_bold_13, C_ALLSTOP_TEXT);

    // The one thing a grower reaching for this button has to know, and the only
    // place on the page with vertical room to say it. This board has no relay and
    // no driver (src/plantrx.cpp, top of file), so the press withdraws all seven
    // requests and reports that to the server - it does not stop machinery,
    // because nothing here is wired to any. Saying so on the emergency control
    // rather than on each card is deliberate: this is where believing otherwise
    // costs the most, and seven copies of the same sentence would be read as
    // decoration by the third card.
    //
    // C_ALLSTOP_SUBTEXT was defined in both palettes and used by nothing until
    // now. It is the muted red that belongs under this heading - secondary grey
    // on the all-stop's own background is the one pairing the palette has no
    // contrast figure for.
    label(c, "릴레이 미연결 · 요청만 해제", &font_reg_12, C_ALLSTOP_SUBTEXT);
    return c;
}

static void build_undo_pill(lv_obj_t *page) {
    // Diagonally opposite the all-stop card (col 2, row 2) so it is never under
    // the finger that just pressed 전체 정지. Measured off the font tables it is
    // 101px wide (4x12 for 되돌리기, 8 gap, 6.625+12 for "6초", 2x12 pad, 2
    // border), and 펌프 A's toggle starts at x=187 of that 245px column, so the
    // pill covers only that card's badge and name and swallows no control.
    w_undo = card(page, C_AMBER_TINT, 12, true);
    lv_obj_set_size(w_undo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(w_undo, C_AMBER, 0);
    ignore_layout(w_undo);
    lv_obj_align(w_undo, LV_ALIGN_TOP_LEFT, 0, 0);
    flex_row(w_undo, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(w_undo, 12, 0);
    lv_obj_set_style_pad_ver(w_undo, 8, 0);
    clickable(w_undo);
    lv_obj_add_event_cb(w_undo, on_undo, LV_EVENT_CLICKED, NULL);

    // Amber, not the all-stop card's red: a second red control beside the one
    // that just fired reads as another stop, and this has to read as the way
    // back. font_bold_12 rather than the card's font_bold_13 because only the
    // 12px faces carry the font_kr_full_12 fallback: 되, 돌 and 리 are in no
    // subset, and font_bold_13 has no fallback, so there the word would render
    // as three missing glyphs. The fallback is malgunbd at 12px like
    // font_bold_12 itself, so the four glyphs match in weight and advance.
    label(w_undo, "되돌리기", &font_bold_12, C_AMBER);
    w_undo_left = label(w_undo, "", &font_reg_12, C_TEXT_SECONDARY);
}

// Cards stretch to fill their cell(s); the grid owns all sizing.
static void grid_cell(lv_obj_t *o, uint8_t col, uint8_t colspan, uint8_t row) {
    lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, col, colspan, LV_GRID_ALIGN_STRETCH, row, 1);
}

lv_obj_t *page_control_build(lv_obj_t *parent) {
    lv_obj_t *page = plain(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    pad_all(page, 16);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    // 3x3 grid with equal fractional tracks; only the fan card spans 2 columns.
    static lv_coord_t grid_tpl[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(page, grid_tpl, grid_tpl);
    lv_obj_set_style_pad_column(page, 16, 0);
    lv_obj_set_style_pad_row(page, 16, 0);

    grid_cell(build_device_card(page, ICON_PUMP, C_BLUE_TINT, C_BLUE, "펌프 A", t_pumpA, &st_pumpA, on_dev_pumpA), 0, 1, 0);
    grid_cell(build_device_card(page, ICON_PUMP, C_BLUE_TINT, C_BLUE, "펌프 B", t_pumpB, &st_pumpB, on_dev_pumpB), 1, 1, 0);
    grid_cell(build_device_card(page, ICON_PUMP, C_BLUE_TINT, C_BLUE, "펌프 C", t_pumpC, &st_pumpC, on_dev_pumpC), 2, 1, 0);

    grid_cell(build_fan_card(page), 0, 2, 1);
    grid_cell(build_device_card(page, ICON_MIST, C_BLUE_TINT, C_BLUE, "초음파 미스트", t_mist, &st_mist, on_dev_mist), 2, 1, 1);

    grid_cell(build_device_card(page, ICON_LED, C_LED_TINT, C_GREEN, "LED 식물등", t_led, &st_led, on_dev_led), 0, 1, 2);
    grid_cell(build_device_card(page, ICON_HEATER, C_AMBER_TINT, C_AMBER, "PTC 히터", t_heater, &st_heater, on_dev_heater), 1, 1, 2);
    grid_cell(build_allstop_card(page), 2, 1, 2);

    build_undo_pill(page);   // last child: it has to draw over the card it covers
    if (s_undo_timer == NULL) {
        s_undo_timer = lv_timer_create(undo_timer_cb, 1000, NULL);
    }
    // Idle state, and the reset that matters on a theme rebuild: ui_set_dark()
    // destroys every widget here, so a window left pending across it would hold
    // a snapshot with no pill on screen to spend it.
    undo_clear();
    return page;
}
