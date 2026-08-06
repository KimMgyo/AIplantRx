// Tiny builders, icons, shared composite widgets, and the two shared display
// policies the top bar and the settings block both have to agree on.
#include "ui_internal.h"

#include "aijudge.h"
#include "plantrx.h"

// ---------------------------------------------------------------------------
// Shared display policy
// ---------------------------------------------------------------------------

int32_t rx_content_stale_s(void) {
    // Only the server's turn is a promise about judgments. The local rule turn is
    // what still answers when the uplink is down, so holding the server to the
    // rule's three minutes would paint 판단 지연 on the bar every time the model
    // took a normal breath - and aijudge_turn_period_ms() reports the rule's
    // period whenever no server turn is live, which is exactly that mistake.
    if (!aijudge_turn_is_server()) return RX_CONTENT_STALE_CEIL_S;

    int32_t period_s = (int32_t)(aijudge_turn_period_ms() / 1000u);
    if (period_s <= 0) return RX_CONTENT_STALE_CEIL_S;

    // One whole missed turn plus the one in flight. A single late turn is a
    // transient - the model can be slow once without the panel having anything to
    // report - and two is where it stops being one. The 120s is two default polls
    // (POLL_IDLE_S, scheduler.py:36): the device may not hear of a fresh judgment
    // for one of them, and the second is the round trip inside that exchange
    // rather than a pretence that it is free.
    int32_t stale = 2 * period_s + 120;
    return stale > RX_CONTENT_STALE_CEIL_S ? RX_CONTENT_STALE_CEIL_S : stale;
}

bool rx_no_model(void) {
    // Link first: behind a dead uplink the panel knows nothing about the server's
    // key, and saying 모델 없음 there would be an assertion about a machine it
    // cannot reach. That case already has its own words (연결 없음 / 응답 대기).
    return plantrx_link() == RX_OK &&
           plantrx_content_age_s() < 0 &&
           !plantrx_model_ready();
}

// ---------------------------------------------------------------------------
// Tiny builders
// ---------------------------------------------------------------------------

lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    // lv_obj is clickable by default; decorative children would swallow taps
    // meant for their clickable ancestors (pills, tabs, menu items).
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t bg, lv_coord_t radius) {
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

lv_obj_t *card(lv_obj_t *parent, lv_color_t bg, lv_coord_t radius, bool border) {
    lv_obj_t *o = plain(parent);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    if (border) {
        lv_obj_set_style_border_color(o, C_BORDER, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    }
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

void flex_row(lv_obj_t *o, lv_coord_t gap, lv_flex_align_t main, lv_flex_align_t cross) {
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    // Single-track layout: track placement must follow the cross alignment,
    // otherwise the track (sized to the tallest child) pins to the top.
    lv_obj_set_flex_align(o, main, cross, cross);
    lv_obj_set_style_pad_column(o, gap, 0);
}

void flex_col(lv_obj_t *o, lv_coord_t gap, lv_flex_align_t main, lv_flex_align_t cross) {
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, main, cross, cross);
    lv_obj_set_style_pad_row(o, gap, 0);
}

void pad_all(lv_obj_t *o, lv_coord_t p) { lv_obj_set_style_pad_all(o, p, 0); }

void clickable(lv_obj_t *o) { lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE); }

void ignore_layout(lv_obj_t *o) { lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT); }

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

lv_obj_t *make_monitor_icon(lv_obj_t *parent, lv_color_t color) {
    lv_obj_t *outer = plain(parent);
    lv_obj_set_size(outer, 20, 15);
    lv_obj_set_style_radius(outer, 3, 0);
    lv_obj_set_style_border_width(outer, 2, 0);
    lv_obj_set_style_border_color(outer, color, 0);
    lv_obj_set_style_border_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(outer, 0, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *dot = plain(outer);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_color(dot, color, 0);
    lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
    return outer;
}

lv_obj_t *make_hamburger_icon(lv_obj_t *parent, lv_color_t color) {
    lv_obj_t *outer = plain(parent);
    lv_obj_set_size(outer, 20, 15);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(outer, 3, 0);
    for (int i = 0; i < 3; i++) {
        box(outer, 16, 2, color, 1);
    }
    return outer;
}

lv_obj_t *make_gear_icon(lv_obj_t *parent, lv_color_t color) {
    lv_obj_t *outer = plain(parent);
    lv_obj_set_size(outer, 20, 15);
    lv_obj_t *ring = plain(outer);
    lv_obj_set_size(ring, 8, 8);
    lv_obj_set_pos(ring, 6, 3);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, color, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_t *top = box(outer, 2, 3, color, 1);
    lv_obj_set_pos(top, 9, 0);
    lv_obj_t *bottom = box(outer, 2, 3, color, 1);
    lv_obj_set_pos(bottom, 9, 11);
    lv_obj_t *left_t = box(outer, 3, 2, color, 1);
    lv_obj_set_pos(left_t, 3, 6);
    lv_obj_t *right_t = box(outer, 3, 2, color, 1);
    lv_obj_set_pos(right_t, 14, 6);
    return outer;
}

lv_obj_t *make_wifi_bars(lv_obj_t *parent, int strength, lv_color_t on, lv_color_t off) {
    lv_obj_t *outer = plain(parent);
    lv_obj_set_size(outer, 18, 12);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(outer, 2, 0);
    static const int heights[4] = {5, 7, 9, 12};
    for (int i = 0; i < 4; i++) {
        box(outer, 3, heights[i], i < strength ? on : off, 1);
    }
    return outer;
}

lv_obj_t *make_badge(lv_obj_t *parent, const char *icon, lv_color_t bg, lv_color_t fg) {
    lv_obj_t *b = box(parent, 28, 28, bg, 8);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(b, icon, &font_icons, fg);
    return b;
}

// ---------------------------------------------------------------------------
// Shared composite widgets
// ---------------------------------------------------------------------------

void build_toggle(lv_obj_t *parent, ToggleWidgets &t, lv_event_cb_t cb) {
    t.track = plain(parent);
    lv_obj_set_size(t.track, 44, 24);
    lv_obj_set_style_radius(t.track, 12, 0);
    lv_obj_set_style_bg_color(t.track, C_SWITCH_OFF, 0);
    lv_obj_set_style_bg_opa(t.track, LV_OPA_COVER, 0);
    clickable(t.track);
    lv_obj_add_event_cb(t.track, cb, LV_EVENT_CLICKED, NULL);

    t.knob = plain(t.track);
    lv_obj_set_size(t.knob, 18, 18);
    lv_obj_set_style_radius(t.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(t.knob, C_WHITE, 0);
    lv_obj_set_style_bg_opa(t.knob, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(t.knob, 3, 0);
    lv_obj_set_style_shadow_ofs_y(t.knob, 1, 0);
    lv_obj_set_style_shadow_color(t.knob, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(t.knob, 64, 0);
    lv_obj_set_pos(t.knob, 3, 3);
}

void update_toggle(ToggleWidgets &t, bool on, lv_color_t on_color) {
    lv_obj_set_style_bg_color(t.track, on ? on_color : C_SWITCH_OFF, 0);
    lv_obj_set_pos(t.knob, on ? 23 : 3, 3);
}

void set_status(lv_obj_t *lbl, bool on, lv_color_t on_color, const char *txt) {
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, on ? on_color : C_TEXT_SECONDARY, 0);
}

lv_obj_t *build_device_header(lv_obj_t *card_obj, const char *icon, lv_color_t badge_bg, lv_color_t badge_fg,
                              const char *name, ToggleWidgets &t, lv_event_cb_t cb) {
    lv_obj_t *header = plain(card_obj);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    flex_row(header, 0, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = plain(header);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex_row(left, 8);
    make_badge(left, icon, badge_bg, badge_fg);
    label(left, name, &font_bold_14, C_TEXT_DARK);

    build_toggle(header, t, cb);
    return header;
}

lv_obj_t *build_status_line(lv_obj_t *card_obj, lv_obj_t **status_out, const char *key) {
    lv_obj_t *line = plain(card_obj);
    lv_obj_set_width(line, LV_SIZE_CONTENT);
    lv_obj_set_height(line, LV_SIZE_CONTENT);
    flex_row(line, 4);
    label(line, key, &font_reg_12, C_TEXT_SECONDARY);
    *status_out = label(line, "OFF", &font_bold_12, C_TEXT_SECONDARY);
    return line;
}
