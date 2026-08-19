// Top bar (logo, page pill, wifi + clock) and the page dropdown menu.
#include <Arduino.h>
#include <stdio.h>
#include <time.h>
#include "ui_internal.h"
#include "net.h"

static bool s_menu_open = false;

static lv_obj_t *w_time_label;
static lv_obj_t *w_wifi_icon;  // 4 bar children, recolored by link strength
static lv_obj_t *w_pill_label;
static lv_obj_t *w_pill_icon;  // MDI glyph label; text set to the active page
static lv_obj_t *w_chevron;
static lv_obj_t *w_menu_backdrop, *w_menu_box;
static lv_obj_t *w_title_dot;
static lv_obj_t *w_menu_item_auto, *w_menu_item_monitor, *w_menu_item_control, *w_menu_item_settings,
    *w_menu_item_update;
static lv_obj_t *w_menu_label_auto, *w_menu_label_monitor, *w_menu_label_control, *w_menu_label_settings,
    *w_menu_label_update;
static lv_obj_t *w_menu_icon_auto, *w_menu_icon_monitor, *w_menu_icon_control, *w_menu_icon_settings,
    *w_menu_icon_update;

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void on_toggle_menu(lv_event_t *e) {
    s_menu_open = !s_menu_open;
    topbar_refresh();
}

static void on_close_menu(lv_event_t *e) {
    s_menu_open = false;
    topbar_refresh();
}

static void on_select_auto(lv_event_t *e) { ui_set_page(PAGE_AUTO); }
static void on_select_monitor(lv_event_t *e) { ui_set_page(PAGE_MONITOR); }
static void on_select_control(lv_event_t *e) { ui_set_page(PAGE_CONTROL); }
static void on_select_settings(lv_event_t *e) { ui_set_page(PAGE_SETTINGS); }
static void on_select_update(lv_event_t *e) { ui_set_page(PAGE_UPDATE); }

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void topbar_refresh(void) {
    if (s_menu_open) {
        lv_obj_clear_flag(w_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(w_menu_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(w_menu_box, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_transform_angle(w_chevron, s_menu_open ? 2250 : 450, 0);
    // Optical offset: the rotated glyph's ink sits below center when closed (v)
    // and above center when open (^); -3 centers closed, 0 splits the difference
    // for open (user-tuned).
    lv_obj_set_style_translate_y(w_chevron, s_menu_open ? 0 : -3, 0);

    // The pill and the glyph agree on every page, this one included. They used to
    // disagree here: 펌웨어 in words over the 설정 glyph, because the firmware page
    // hung off settings and no lucide.ttf in this tree could mint a mark for it.
    // Both halves of that are gone - the page is its own dropdown entry, and
    // tools/lucide.ttf regenerates font_icons / font_icons_sm (see their Opts
    // lines), so U+E4E5 costs one glyph in two fonts and is drawn in two places.
    const char *pill_txt = (g_page == PAGE_AUTO)      ? "AI-RX"
                           : (g_page == PAGE_MONITOR) ? "모니터링"
                           : (g_page == PAGE_CONTROL) ? "컨트롤"
                           : (g_page == PAGE_UPDATE)  ? "펌웨어"
                                                      : "설정";
    lv_label_set_text(w_pill_label, pill_txt);
    lv_label_set_text(w_pill_icon, (g_page == PAGE_AUTO)      ? ICON_AUTO
                                   : (g_page == PAGE_MONITOR) ? ICON_MONITOR
                                   : (g_page == PAGE_CONTROL) ? ICON_CONTROL
                                   : (g_page == PAGE_UPDATE)  ? ICON_FIRMWARE
                                                              : ICON_SETTINGS);

    // The dot beside the product name is the one global "is the AI driving?"
    // indicator - it is visible from every page, so it tracks the switch, not
    // the current page.
    lv_obj_set_style_bg_color(w_title_dot, g_auto_control ? C_GREEN : C_SWITCH_OFF, 0);

    // AI-RX highlights with a gradient rather than a flat tint. This nav used to
    // light it in the same C_GREEN 모니터링 takes, so two of the four items lit up
    // identically and the accent carried no information about which was selected;
    // see the note on the palette's `ai` field. bg_color is the gradient's first
    // stop, which is why the inactive branch still names a colour that bg_opa then
    // hides, and GRAD_DIR_NONE is what keeps the second stop out of it.
    bool is_auto = (g_page == PAGE_AUTO);
    lv_obj_set_style_bg_color(w_menu_item_auto, is_auto ? C_AI_GRAD1 : C_SCREEN_BG, 0);
    lv_obj_set_style_bg_grad_color(w_menu_item_auto, C_AI_GRAD2, 0);
    lv_obj_set_style_bg_grad_dir(w_menu_item_auto, is_auto ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(w_menu_item_auto, is_auto ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(w_menu_label_auto, is_auto ? C_AI : C_INACTIVE_MENU, 0);
    lv_obj_set_style_text_color(w_menu_icon_auto, is_auto ? C_AI : C_INACTIVE_MENU, 0);

    bool is_mon = (g_page == PAGE_MONITOR);
    lv_obj_set_style_bg_color(w_menu_item_monitor, is_mon ? C_GREEN_TINT : C_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(w_menu_item_monitor, is_mon ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(w_menu_label_monitor, is_mon ? C_GREEN : C_INACTIVE_MENU, 0);
    lv_obj_set_style_text_color(w_menu_icon_monitor, is_mon ? C_GREEN : C_INACTIVE_MENU, 0);

    bool is_ctl = (g_page == PAGE_CONTROL);
    lv_obj_set_style_bg_color(w_menu_item_control, is_ctl ? C_BLUE_TINT : C_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(w_menu_item_control, is_ctl ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(w_menu_label_control, is_ctl ? C_BLUE : C_INACTIVE_MENU, 0);
    lv_obj_set_style_text_color(w_menu_icon_control, is_ctl ? C_BLUE : C_INACTIVE_MENU, 0);

    // Amber twice, and deliberately. The palette's note calls the scheme - green
    // acts, blue measures, amber configures, red stops - and firmware is the second
    // thing on this panel that configures rather than reports. The two greens that
    // note warns about were a real defect because 모니터링 and AI-RX are peers a
    // grower switches between; 설정 and 펌웨어 are one branch, only ever one of them
    // is lit, and giving the leaf a fifth hue would have said "new family" about a
    // page that is the same family.
    bool is_set = (g_page == PAGE_SETTINGS);
    lv_obj_set_style_bg_color(w_menu_item_settings, is_set ? C_AMBER_TINT : C_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(w_menu_item_settings, is_set ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(w_menu_label_settings, is_set ? C_AMBER : C_INACTIVE_MENU, 0);
    lv_obj_set_style_text_color(w_menu_icon_settings, is_set ? C_AMBER : C_INACTIVE_MENU, 0);

    bool is_upd = (g_page == PAGE_UPDATE);
    lv_obj_set_style_bg_color(w_menu_item_update, is_upd ? C_AMBER_TINT : C_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(w_menu_item_update, is_upd ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(w_menu_label_update, is_upd ? C_AMBER : C_INACTIVE_MENU, 0);
    lv_obj_set_style_text_color(w_menu_icon_update, is_upd ? C_AMBER : C_INACTIVE_MENU, 0);
}

// ui_set_page() closes the dropdown on page change.
void topbar_close_menu(void) { s_menu_open = false; }

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

// 1 Hz: refreshes the wifi bars and the clock / no-internet label. (net_poll()
// itself runs on its own timer, see ui.cpp.)
static void net_status_timer_cb(lv_timer_t *t) {
    NetState st = net_state();
    int strength = net_strength();
    // Bar color reflects link quality, weakest to strongest: all-stop red,
    // the live-badge red, green, blue.
    lv_color_t active_c = (strength <= 1) ? C_ALLSTOP_BORDER
                         : (strength == 2) ? C_LIVE_RED
                         : (strength == 3) ? C_GREEN
                                           : C_BLUE;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(w_wifi_icon); i++) {
        lv_obj_set_style_bg_color(lv_obj_get_child(w_wifi_icon, i),
                                  (int)i < strength ? active_c : C_PROGRESS_TRACK, 0);
    }

    // The clock slot, and the reason it is blank when it is. These two branches
    // used to be the wrong way round: "인터넷 연결 없음" sat on the not-connected
    // case, where the radio being off is already visible in the wifi bars beside
    // it, while the case that genuinely needed explaining - associated to an
    // access point and NTP never landing - showed a bare "--:--" forever with no
    // reason given. That case is not hypothetical, it is the greenhouse LAN this
    // panel is designed for: plantrx, the camera and the sensor node are all local,
    // so an install with no route to the internet works completely and its clock
    // never sets. A grower stood in front of a dead clock with nothing to read.
    //
    // The grace matters because SNTP takes a few seconds and retries on a backoff.
    // Claiming no internet inside that window would be wrong on every healthy
    // boot, so the panel waits a minute - two SNTP attempts - before it says so,
    // and "--:--" carries the first minute on its own.
    static const uint32_t NTP_GRACE_MS = 60000;
    static uint32_t s_assoc_ms = 0;
    if (st != NET_CONNECTED) {
        s_assoc_ms = 0;
    } else if (s_assoc_ms == 0) {
        s_assoc_ms = millis();
        if (s_assoc_ms == 0) s_assoc_ms = 1;   // 0 is the "not associated" sentinel
    }

    if (st == NET_CONNECTED && net_time_valid()) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char buf[12];
        // The clock timer already ticks once a second, so seconds cost
        // nothing extra — just a wider format string on the same 1Hz update.
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        lv_obj_set_style_text_font(w_time_label, &font_bold_13, 0);
        lv_obj_set_style_text_color(w_time_label, C_TEXT_DARK, 0);
        lv_label_set_text(w_time_label, buf);
    } else if (st == NET_CONNECTED && (millis() - s_assoc_ms) < NTP_GRACE_MS) {
        lv_obj_set_style_text_font(w_time_label, &font_bold_13, 0);
        lv_obj_set_style_text_color(w_time_label, C_TEXT_SECONDARY, 0);
        lv_label_set_text(w_time_label, "--:--");
    } else if (st == NET_CONNECTED) {
        // Associated, and a minute without a clock. What the panel knows is that
        // NTP is not answering; the reason it is worth naming as the internet is
        // that everything else this board talks to is on the LAN and working.
        // font_bold_12 carries the full-Hangul fallback for 인/넷/없.
        lv_obj_set_style_text_font(w_time_label, &font_bold_12, 0);
        lv_obj_set_style_text_color(w_time_label, C_TEXT_SECONDARY, 0);
        lv_label_set_text(w_time_label, "인터넷 연결 없음");
    } else {
        // Not associated. The panel knows nothing about the internet from here, so
        // it says the thing it does know - and the bars to the left already agree.
        lv_obj_set_style_text_font(w_time_label, &font_bold_12, 0);
        lv_obj_set_style_text_color(w_time_label, C_TEXT_SECONDARY, 0);
        lv_label_set_text(w_time_label, "WiFi 연결 없음");
    }
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

void topbar_build(lv_obj_t *parent) {
    lv_obj_t *bar = plain(parent);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(bar, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, C_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    flex_row(bar, 0, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 16, 0);

    lv_obj_t *left = plain(bar);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex_row(left, 8);
    w_title_dot = box(left, 8, 8, C_GREEN, LV_RADIUS_CIRCLE);
    label(left, "AI PlantRx", &font_bold_13, C_TEXT_DARK);

    lv_obj_t *right = plain(bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex_row(right, 12);
    // Radio then clock, with nothing between them. An uplink indicator used to sit
    // left of the wifi bars - a recoloured dot and a label - on the reading that
    // right to left the cluster went clock, radio, server, each meaningful only if
    // the next was up. It is gone from the bar deliberately: on a working panel the
    // label spent every second of the season restating what the green dot beside it
    // already said, and a status line that is noise for all but a few minutes of a
    // year is one a grower stops reading, including the few minutes it matters.
    //
    // Nothing became unreachable. The 설정 page's uplink block still carries the
    // state and both ages (page_settings.cpp refresh_uplink), and AI-RX still names
    // the same link states over its prescription list rather than showing a stale
    // one unannotated. The width bound this comment used to derive
    // does not need re-deriving: dropping a child from a LV_SIZE_CONTENT flex row
    // cannot widen it, and the widest label the row could hold is the one that left.
    w_wifi_icon = make_wifi_bars(right, 0, C_BLUE, C_PROGRESS_TRACK);
    w_time_label = label(right, "--:--", &font_bold_13, C_TEXT_DARK);

    lv_obj_t *pill = plain(bar);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ignore_layout(pill);
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 0);
    clickable(pill);
    lv_obj_add_event_cb(pill, on_toggle_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(pill, C_PILL_BG, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(pill, C_BORDER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_COVER, 0);
    flex_row(pill, 8);
    lv_obj_set_style_pad_left(pill, 8, 0);
    lv_obj_set_style_pad_right(pill, 10, 0);
    lv_obj_set_style_pad_ver(pill, 6, 0);
    w_pill_icon = label(pill, ICON_MONITOR, &font_icons_sm, C_TEXT_SECONDARY);
    w_pill_label = label(pill, "모니터링", &font_bold_13, C_TEXT_DARK);
    w_chevron = plain(pill);
    lv_obj_set_size(w_chevron, 8, 8);
    lv_obj_set_style_border_width(w_chevron, 2, 0);
    lv_obj_set_style_border_side(w_chevron, (lv_border_side_t)(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_border_color(w_chevron, C_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_opa(w_chevron, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(w_chevron, 4, 0);
    lv_obj_set_style_transform_pivot_y(w_chevron, 4, 0);
    // Angle and optical translate_y are owned by topbar_refresh().

    // Theme rebuild re-runs this builder; the timer must be created once.
    static lv_timer_t *s_net_timer = NULL;
    if (s_net_timer == NULL) {
        s_net_timer = lv_timer_create(net_status_timer_cb, 1000, NULL);
    }
    net_status_timer_cb(NULL);
}

static lv_obj_t *build_menu_item(lv_obj_t *parent, lv_obj_t **icon_out, lv_obj_t **label_out, const char *text,
                                 const char *icon_glyph) {
    lv_obj_t *item = plain(parent);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    flex_row(item, 10);
    lv_obj_set_style_pad_hor(item, 10, 0);
    lv_obj_set_style_pad_ver(item, 9, 0);
    lv_obj_set_style_radius(item, 8, 0);
    clickable(item);

    lv_obj_t *icon = label(item, icon_glyph, &font_icons_sm, C_INACTIVE_MENU);
    lv_obj_t *lbl = label(item, text, &font_bold_13, C_INACTIVE_MENU);

    if (icon_out) *icon_out = icon;
    if (label_out) *label_out = lbl;
    return item;
}

void menu_build(lv_obj_t *parent) {
    w_menu_backdrop = plain(parent);
    lv_obj_set_size(w_menu_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(w_menu_backdrop, LV_OPA_TRANSP, 0);
    ignore_layout(w_menu_backdrop);
    lv_obj_align(w_menu_backdrop, LV_ALIGN_TOP_LEFT, 0, 0);
    clickable(w_menu_backdrop);
    lv_obj_add_event_cb(w_menu_backdrop, on_close_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(w_menu_backdrop, LV_OBJ_FLAG_HIDDEN);

    w_menu_box = plain(parent);
    lv_obj_set_width(w_menu_box, 200);
    lv_obj_set_height(w_menu_box, LV_SIZE_CONTENT);
    ignore_layout(w_menu_box);
    lv_obj_align(w_menu_box, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(w_menu_box, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(w_menu_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(w_menu_box, C_BORDER, 0);
    lv_obj_set_style_border_width(w_menu_box, 1, 0);
    lv_obj_set_style_radius(w_menu_box, 12, 0);
    // box-shadow: 0 14px 30px rgba(0,0,0,.2)
    lv_obj_set_style_shadow_width(w_menu_box, 30, 0);
    lv_obj_set_style_shadow_ofs_y(w_menu_box, 14, 0);
    lv_obj_set_style_shadow_color(w_menu_box, lv_color_black(), 0);
    // A soft wide shadow reads as a gray halo on dark surfaces; go deeper there.
    lv_obj_set_style_shadow_opa(w_menu_box, g_dark ? 170 : 51, 0);
    pad_all(w_menu_box, 6);
    flex_col(w_menu_box, 2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(w_menu_box, LV_OBJ_FLAG_HIDDEN);

    w_menu_item_auto = build_menu_item(w_menu_box, &w_menu_icon_auto, &w_menu_label_auto, "AI-RX", ICON_AUTO);
    lv_obj_add_event_cb(w_menu_item_auto, on_select_auto, LV_EVENT_CLICKED, NULL);

    w_menu_item_monitor = build_menu_item(w_menu_box, &w_menu_icon_monitor, &w_menu_label_monitor, "모니터링", ICON_MONITOR);
    lv_obj_add_event_cb(w_menu_item_monitor, on_select_monitor, LV_EVENT_CLICKED, NULL);

    w_menu_item_control = build_menu_item(w_menu_box, &w_menu_icon_control, &w_menu_label_control, "컨트롤", ICON_CONTROL);
    lv_obj_add_event_cb(w_menu_item_control, on_select_control, LV_EVENT_CLICKED, NULL);

    w_menu_item_settings = build_menu_item(w_menu_box, &w_menu_icon_settings, &w_menu_label_settings, "설정", ICON_SETTINGS);
    lv_obj_add_event_cb(w_menu_item_settings, on_select_settings, LV_EVENT_CLICKED, NULL);

    // Last, and below 설정 rather than above it: the dropdown reads top to bottom as
    // how often a grower wants the page, and this is the only entry that is not part
    // of running a greenhouse. Bottom of the list is also the furthest the layout can
    // put it from 모니터링, which is the mis-tap the old design note cared about and
    // the only part of that argument the move does not settle by itself.
    w_menu_item_update = build_menu_item(w_menu_box, &w_menu_icon_update, &w_menu_label_update, "펌웨어", ICON_FIRMWARE);
    lv_obj_add_event_cb(w_menu_item_update, on_select_update, LV_EVENT_CLICKED, NULL);
}
