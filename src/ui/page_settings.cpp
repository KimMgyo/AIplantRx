// Settings page: merged WiFi card (status + scan + uplink diagnostics + network
// list) and the display (dark mode) card. Networking goes through net.h; UI
// state is refreshed by a 1 Hz poll timer, never from WiFi callbacks.
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"
#include "net.h"
#include "reading.h"
#include "camprov.h"
#include "camnet.h"
#include "paneloled.h"
#include "sensornode.h"
#include "thermal.h"
#include "plantid.h"
#include "plantrx.h"
#include "updatemode.h"
#include "fwpull.h"

static ToggleWidgets t_wifi, t_dark;
static lv_obj_t *st_wifi, *st_dark;
static lv_obj_t *v_ssid, *v_ip, *v_rssi, *v_wifi_mac;   // WiFi info row value labels
static lv_obj_t *st_cam;                    // ESP-NOW camera online status
static lv_obj_t *v_cam_ip, *v_cam_rssi, *v_cam_mac, *v_cam_video;
static lv_obj_t *st_node;                   // ESP-NOW sensor-node online status
static lv_obj_t *v_node_age, *v_node_rx, *v_node_thermal, *v_node_peak;
#if PANEL_OLED
static lv_obj_t *v_oled;                    // rear OLED state; see the note where it is built
#endif
static lv_obj_t *v_plantnet;                      // PlantNet daily quota remaining
static lv_obj_t *w_net_list;               // scrollable scan-result list
static lv_obj_t *w_dlg;                    // password dialog root (NULL = closed)
static lv_obj_t *w_dlg_ta;
static char s_dlg_ssid[33];
static lv_obj_t *s_page = NULL;  // page root; used to skip the 1Hz refresh when hidden

// ---------------------------------------------------------------------------
// Password dialog (modal on the top layer)
// ---------------------------------------------------------------------------

static void dialog_close(void) {
    if (w_dlg != NULL) {
        lv_obj_del(w_dlg);
        w_dlg = NULL;
        w_dlg_ta = NULL;
    }
}

static void on_dlg_cancel(lv_event_t *e) { dialog_close(); }

static void on_dlg_connect(lv_event_t *e) {
    net_connect(s_dlg_ssid, lv_textarea_get_text(w_dlg_ta));
    dialog_close();
    page_settings_refresh();
}

static void on_kb_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        on_dlg_connect(e);
    } else if (code == LV_EVENT_CANCEL) {
        dialog_close();
    }
}

static void dialog_open(const char *ssid) {
    if (w_dlg != NULL) return;
    strncpy(s_dlg_ssid, ssid, sizeof(s_dlg_ssid) - 1);
    s_dlg_ssid[sizeof(s_dlg_ssid) - 1] = '\0';

    // Dim backdrop; tap outside cancels.
    w_dlg = plain(lv_layer_top());
    lv_obj_set_size(w_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(w_dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(w_dlg, 120, 0);
    clickable(w_dlg);
    lv_obj_add_event_cb(w_dlg, on_dlg_cancel, LV_EVENT_CLICKED, NULL);

    // Card. All Korean strings use font_bold_12 for the full-Hangul fallback.
    lv_obj_t *box_ = card(w_dlg, C_SURFACE, 14, true);
    lv_obj_set_width(box_, 420);
    lv_obj_set_height(box_, LV_SIZE_CONTENT);
    ignore_layout(box_);
    lv_obj_align(box_, LV_ALIGN_TOP_MID, 0, 36);
    flex_col(box_, 10, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(box_, 16);
    clickable(box_);  // swallow taps so the backdrop doesn't close under the card

    char title[64];
    snprintf(title, sizeof(title), "\"%s\" 비밀번호 입력", s_dlg_ssid);
    label(box_, title, &font_bold_12, C_TEXT_DARK);

    w_dlg_ta = lv_textarea_create(box_);
    lv_textarea_set_one_line(w_dlg_ta, true);
    lv_textarea_set_password_mode(w_dlg_ta, true);
    lv_textarea_set_placeholder_text(w_dlg_ta, "비밀번호");
    lv_obj_set_width(w_dlg_ta, LV_PCT(100));
    lv_obj_set_style_text_font(w_dlg_ta, &font_bold_12, 0);
    lv_obj_set_style_bg_color(w_dlg_ta, C_PILL_BG, 0);
    lv_obj_set_style_text_color(w_dlg_ta, C_TEXT_DARK, 0);
    lv_obj_set_style_border_color(w_dlg_ta, C_BORDER, 0);
    lv_obj_set_style_border_width(w_dlg_ta, 1, 0);
    lv_obj_set_style_radius(w_dlg_ta, 8, 0);

    lv_obj_t *btn_row = plain(box_);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    flex_row(btn_row, 8, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_cancel = box(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT, C_GRAY_TINT, 8);
    lv_obj_set_style_pad_hor(btn_cancel, 14, 0);
    lv_obj_set_style_pad_ver(btn_cancel, 7, 0);
    clickable(btn_cancel);
    lv_obj_add_event_cb(btn_cancel, on_dlg_cancel, LV_EVENT_CLICKED, NULL);
    label(btn_cancel, "취소", &font_bold_12, C_TEXT_SECONDARY);

    lv_obj_t *btn_ok = box(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT, C_BLUE_TINT, 8);
    lv_obj_set_style_pad_hor(btn_ok, 14, 0);
    lv_obj_set_style_pad_ver(btn_ok, 7, 0);
    clickable(btn_ok);
    lv_obj_add_event_cb(btn_ok, on_dlg_connect, LV_EVENT_CLICKED, NULL);
    label(btn_ok, "연결", &font_bold_12, C_BLUE);

    // Created per-dialog, so it always picks up the active palette.
    lv_obj_t *kb = lv_keyboard_create(w_dlg);
    lv_obj_set_size(kb, LV_PCT(100), 190);
    lv_obj_set_style_bg_color(kb, C_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(kb, C_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, C_TEXT_DARK, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, C_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    // Special keys (backspace, enter, mode switch) carry the CHECKED ctrl
    // flag and render in LV_STATE_CHECKED — style it or they stay theme-white.
    lv_obj_set_style_bg_color(kb, C_PILL_BG, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, C_TEXT_SECONDARY, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, C_BLUE_TINT, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, C_BLUE, (uint32_t)LV_PART_ITEMS | (uint32_t)LV_STATE_PRESSED);
    lv_keyboard_set_textarea(kb, w_dlg_ta);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_ALL, NULL);
}

// ---------------------------------------------------------------------------
// Network list
// ---------------------------------------------------------------------------

static void on_net_row(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const NetScanItem *it = net_scan_item(idx);
    if (it == NULL || !net_enabled()) return;
    if (net_state() == NET_CONNECTED && strcmp(net_ssid(), it->ssid) == 0) return;  // already on it
    if (it->secured) {
        dialog_open(it->ssid);
    } else {
        net_connect(it->ssid, NULL);
        page_settings_refresh();
    }
}

static int rssi_to_bars(int rssi) {
    if (rssi > -55) return 4;
    if (rssi > -65) return 3;
    if (rssi > -75) return 2;
    return 1;
}

static void build_network_row(lv_obj_t *parent, const NetScanItem *it, int idx, bool connected) {
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    flex_row(row, 0, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_set_style_radius(row, 8, 0);
    clickable(row);
    lv_obj_add_event_cb(row, on_net_row, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    if (connected) {
        lv_obj_set_style_bg_color(row, C_BLUE_TINT, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    }

    lv_obj_t *left = plain(row);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex_row(left, 8);
    make_wifi_bars(left, rssi_to_bars(it->rssi), connected ? C_BLUE : C_TEXT_SECONDARY, C_PROGRESS_TRACK);
    label(left, it->ssid, &font_bold_12, C_TEXT_DARK);

    if (connected) {
        lv_obj_t *chip = plain(row);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(chip, C_BLUE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(chip, 8, 0);
        lv_obj_set_style_pad_ver(chip, 3, 0);
        flex_row(chip, 0, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        label(chip, "연결됨", &font_bold_10, C_WHITE);
    } else {
        label(row, it->secured ? "보안" : "개방", &font_bold_12, C_TEXT_SECONDARY);
    }
}

// Skeleton placeholders while a scan is running: 7 static rows pulsed in
// unison by a 120ms timer (idle unless s_skel_active).
static bool s_skel_active = false;
#define SKEL_ROWS 7

static void build_skeleton_rows(void) {
    static const lv_coord_t name_w[SKEL_ROWS] = {120, 90, 140, 100, 130, 85, 110};
    for (int i = 0; i < SKEL_ROWS; i++) {
        lv_obj_t *row = plain(w_net_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        flex_row(row, 0, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 9, 0);

        lv_obj_t *left = plain(row);
        lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        flex_row(left, 8);
        box(left, 18, 12, C_SKELETON, 2);
        box(left, name_w[i], 10, C_SKELETON, 5);

        box(row, 30, 10, C_SKELETON, 5);
    }
}

static void skel_timer_cb(lv_timer_t *t) {
    if (!s_skel_active) return;
    static const lv_opa_t phase[6] = {90, 130, 180, 230, 180, 130};
    static uint8_t p = 0;
    p = (p + 1) % 6;
    // Pulse only the actual skeleton bars (leaf boxes). Never the group
    // containers: those are `plain()` objects whose default bg is white, and
    // raising their opacity would flash that white through in dark mode.
    for (uint32_t r = 0; r < lv_obj_get_child_cnt(w_net_list); r++) {
        lv_obj_t *row = lv_obj_get_child(w_net_list, r);
        for (uint32_t k = 0; k < lv_obj_get_child_cnt(row); k++) {
            lv_obj_t *el = lv_obj_get_child(row, k);
            if (lv_obj_get_child_cnt(el) == 0) {
                lv_obj_set_style_bg_opa(el, phase[p], 0);  // leaf bar
            } else {
                for (uint32_t m = 0; m < lv_obj_get_child_cnt(el); m++) {
                    lv_obj_set_style_bg_opa(lv_obj_get_child(el, m), phase[p], 0);
                }
            }
        }
    }
}

// True once this settings visit has rendered a completed scan; the skeleton is
// shown only for the visit's first scan, then results update in place.
static bool s_shown_results_this_visit = false;

static void rebuild_net_list(void) {
    lv_obj_clean(w_net_list);
    s_skel_active = false;
    if (!net_enabled()) {
        label(w_net_list, "WiFi가 꺼져 있습니다", &font_bold_12, C_TEXT_SECONDARY);
        return;
    }
    if (net_scanning() && !s_shown_results_this_visit) {   // this visit's first scan -> skeleton
        s_skel_active = true;
        build_skeleton_rows();
        return;
    }
    s_shown_results_this_visit = true;   // first scan done this visit: update in place from now on
    int n = net_scan_count();
    if (n == 0) {
        label(w_net_list, "주변 네트워크가 없습니다", &font_bold_12, C_TEXT_SECONDARY);
        return;
    }
    const char *cur = (net_state() == NET_CONNECTED) ? net_ssid() : "";
    // The connected network always leads, regardless of its RSSI-sorted
    // position; everything else keeps the scan's original (RSSI) order.
    int connected_idx = -1;
    if (cur[0] != '\0') {
        for (int i = 0; i < n; i++) {
            if (strcmp(cur, net_scan_item(i)->ssid) == 0) {
                connected_idx = i;
                break;
            }
        }
    }
    if (connected_idx >= 0) {
        build_network_row(w_net_list, net_scan_item(connected_idx), connected_idx, true);
    }
    for (int i = 0; i < n; i++) {
        if (i == connected_idx) continue;
        build_network_row(w_net_list, net_scan_item(i), i, false);
    }
}

// ---------------------------------------------------------------------------
// Refresh / handlers
// ---------------------------------------------------------------------------

// The uplink's diagnostics USED TO LIVE HERE - eight rows and a state word, inside the WiFi card.
// They are gone, and what replaced them is worth naming so nobody rebuilds them.
//
// They cost the thing this card exists for. The comment that justified them said it plainly: the
// scrollable network list went from ~225px to ~84px to make room, which on this panel is two
// networks and a scrollbar - so the page whose job is "join a different WiFi" could not show you
// the WiFi. That is a bad trade at any level of detail.
//
// And the detail is no longer scarce. Everything those rows carried - last exchange, last
// judgment, HTTP status, failure reason, consecutive failures, server mode - is in the console this
// panel now pushes to the server, at a resolution eight rows could never reach, readable from a
// phone. The rows were written when the alternative was a serial cable in the greenhouse.
//
// What a person standing in front of the panel still needs is one bit: is the server answering.
// That is one row on the firmware page, beside the versions it serves - see page_update.cpp's
// panel card. One fact, one place.

void page_settings_refresh(void) {
    NetState st = net_state();

    update_toggle(t_wifi, net_enabled(), C_BLUE);
    // "연결 없음" was the whole vocabulary for three different faults, and a grower
    // does three different things about them: retype the password, check the router
    // is on, or wait. net_fail() holds its tongue until the failure is trustworthy
    // (this AP refuses the first association of every boot on a correct password),
    // so an unnamed cause here means the panel does not know yet - not that it knows
    // nothing is wrong. NET_FAIL_OTHER carries the driver's own code rather than a
    // sentence invented for it, because a number a search engine can answer beats a
    // guess that sounds like a diagnosis.
    char fail_buf[40];
    const char *down_txt = "연결 없음";
    switch (net_fail()) {
    case NET_FAIL_NOT_FOUND: down_txt = "네트워크 없음"; break;
    case NET_FAIL_AUTH:      down_txt = "비밀번호 확인"; break;
    case NET_FAIL_OTHER:
        snprintf(fail_buf, sizeof(fail_buf), "연결 실패 (%u)", (unsigned)net_fail_code());
        down_txt = fail_buf;
        break;
    case NET_FAIL_NONE:      break;
    }
    const char *st_txt = (st == NET_OFF)          ? "OFF"
                         : (st == NET_CONNECTED)  ? "연결됨"
                         : net_scanning()         ? "검색 중..."
                         : (st == NET_CONNECTING) ? "연결 중..."
                                                  : down_txt;
    set_status(st_wifi, st == NET_CONNECTED, C_BLUE, st_txt);

    char buf[40];
    lv_label_set_text(v_ssid, (st == NET_CONNECTED) ? net_ssid() : "-");
    if (st == NET_CONNECTED) {
        snprintf(buf, sizeof(buf), "%d dBm", net_rssi());
        lv_label_set_text(v_rssi, buf);
    } else {
        lv_label_set_text(v_rssi, "-");
    }
    net_ip(buf, sizeof(buf));
    lv_label_set_text(v_ip, buf);
    net_mac(buf, sizeof(buf));
    lv_label_set_text(v_wifi_mac, buf);

    update_toggle(t_dark, g_dark, C_AMBER);
    set_status(st_dark, g_dark, C_AMBER, g_dark ? "다크" : "라이트");

    // ESP-NOW camera link (creds provisioning + status beacon)
    bool cam_online = camprov_cam_online();
    set_status(st_cam, cam_online, C_GREEN, cam_online ? "온라인" : "오프라인");
    char cam_ip[24];
    camprov_cam_ip(cam_ip, sizeof(cam_ip));
    lv_label_set_text(v_cam_ip, cam_ip);
    if (cam_online) {
        snprintf(buf, sizeof(buf), "%d dBm", camprov_cam_rssi());
        lv_label_set_text(v_cam_rssi, buf);
    } else {
        lv_label_set_text(v_cam_rssi, "-");
    }
    camprov_cam_mac(buf, sizeof(buf));
    lv_label_set_text(v_cam_mac, buf);

    // The video path, which is not the link the status line above describes. camnet
    // pulls MJPEG over HTTP and a session already open outlives a lapsed beacon, so
    // this can read 수신 중 under an 오프라인 badge - and that combination is the
    // truth about this device, not a contradiction: pictures are arriving and
    // provisioning is not answering. The monitor page's badge reads the same call.
    bool video = camnet_live();
    lv_label_set_text(v_cam_video, video ? "수신 중" : "수신 없음");


    // ESP-NOW sensor node (SCD41 + BH1750 + MLX90640 on an ESP32 devkit).
    bool node_online = sensornode_online();
    set_status(st_node, node_online, C_GREEN, node_online ? "온라인" : "오프라인");
    if (node_online) {
        snprintf(buf, sizeof(buf), "%lums 전", (unsigned long)sensornode_age_ms());
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    lv_label_set_text(v_node_age, buf);
    // Three figures, and the third only when it is not zero. 수신 and 유실 describe
    // the LINK; 무효 describes the SENSOR, and until it existed a node whose SCD41
    // had failed showed a healthy count beside six blank tiles and left the grower
    // to work out which half was broken. Hidden at zero for the reason 연속 실패
    // above is: the absence of a fault is not a measurement.
    uint32_t bad = sensornode_rejected();
    if (bad > 0) {
        snprintf(buf, sizeof(buf), "%lu수신 / %lu유실 / %lu무효",
                 (unsigned long)sensornode_readings(),
                 (unsigned long)sensornode_lost(), (unsigned long)bad);
    } else {
        snprintf(buf, sizeof(buf), "%lu수신 / %lu유실",
                 (unsigned long)sensornode_readings(),
                 (unsigned long)sensornode_lost());
    }
    lv_label_set_text(v_node_rx, buf);
    if (thermal_live()) {
        snprintf(buf, sizeof(buf), "%.1f fps", thermal_fps());
    } else {
        snprintf(buf, sizeof(buf), "수신 없음");
    }
    lv_label_set_text(v_node_thermal, buf);
    // No thermal_live() test beside the one on the row above, and not an omission:
    // thermal_max() returns its < -999 sentinel the moment the stream goes quiet
    // (thermal.cpp:148), so this row falls to "-" on exactly the tick that row
    // falls to "수신 없음". They cannot contradict each other any more - this one
    // used to hold the last peak the MLX90640 ever sent, indefinitely, beside a
    // row saying nothing was arriving.
    float peak = thermal_max();
    if (reading_present(peak)) {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""C", peak);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    lv_label_set_text(v_node_peak, buf);

#if PANEL_OLED
    // Three states, and they need three different actions, so they get three different
    // words. Found and drawing; found once and now silent, which is a power problem on the
    // module; and never found, which means the board's UART switch is still routing GPIO
    // 43/44 to the USB bridge rather than to the header the display is wired to.
    uint8_t oa = paneloled_address();
    if (oa == 0) {
        snprintf(buf, sizeof(buf), "없음 (UART 스위치 확인)");
    } else if (paneloled_ready()) {
        snprintf(buf, sizeof(buf), "0x%02X 표시 중", oa);
    } else {
        snprintf(buf, sizeof(buf), "0x%02X 응답 없음", oa);
    }
    lv_label_set_text(v_oled, buf);
#endif

    // PlantNet daily quota, total remaining across all keys - and 최대 until every
    // key has actually answered. The count is the server's now and rides back on
    // each identify reply, so a boot that has not identified anything yet has not
    // been told a figure: -1 means unknown and prints as "-", the same dash the
    // rows above use for a value the panel has not heard. This row printed a bare
    // "2000" once, which is four keys' allowance assumed and nothing measured; the
    // denominator the AI-RX page's chip carries is not here, so the qualifier is
    // the only thing standing between an estimate and a reading. See
    // plantid_total_is_measured().
    int pn_left = plantid_total_remaining();
    if (pn_left < 0) {
        snprintf(buf, sizeof(buf), "-");
    } else if (plantid_total_is_measured()) {
        snprintf(buf, sizeof(buf), "%d", pn_left);
    } else {
        snprintf(buf, sizeof(buf), "최대 %d", pn_left);
    }
    lv_label_set_text(v_plantnet, buf);


    // Rebuild the list on fresh scan results or when scan/link state changes.
    static bool prev_scanning = false;
    static NetState prev_state = NET_OFF;
    if (net_scan_fresh() || net_scanning() != prev_scanning || st != prev_state) {
        net_scan_clear_fresh();
        rebuild_net_list();
    }
    if (st != prev_state && st != NET_CONNECTED) {
        net_scan_start();   // re-scan the instant the link drops
    }
    prev_scanning = net_scanning();
    prev_state = st;
}

static void on_toggle_wifi(lv_event_t *e) {
    net_set_enabled(!net_enabled());
    if (net_enabled()) net_scan_start();  // radio back on -> refresh the list
    page_settings_refresh();
}

static void on_toggle_dark(lv_event_t *e) {
    bool next = !g_dark;
    // Immediate feedback: flip the toggle and show progress, then force one
    // render pass so the state is visible before the full-UI rebuild.
    update_toggle(t_dark, next, C_AMBER);
    set_status(st_dark, next, C_AMBER, "변경 중...");
    lv_refr_now(NULL);
    ui_set_dark(next);  // rebuilds the UI asynchronously
}

// Called by ui_set_page() whenever the settings page becomes visible.
void page_settings_on_show(void) {
    s_shown_results_this_visit = false;   // this visit's first scan gets the skeleton
    net_scan_start();
    rebuild_net_list();                   // paint the skeleton now, don't wait for a refresh tick
    page_settings_refresh();
}
static void settings_timer_cb(lv_timer_t *t) {
    // Only refresh while the settings page is actually on screen — the ~22
    // label writes/sec are wasted (and cause redraws) when it's hidden.
    if (s_page != NULL && lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) return;
    static uint32_t last_scan = 0;
    // 10s, not 5. Every scan costs the camera stream a ~0.6s sweep off-channel;
    // a settings list does not need to be fresher than this.
    //
    // AND NOT AT ALL WHILE A PULL IS RUNNING. That sweep leaves the associated channel, which is
    // why net.cpp's own note says a scan stalls the camera and why ui.cpp aborts one on leaving
    // this page. The camera surviving a stalled second is a fair trade for a fresh network list;
    // a firmware fetch is not. This page is where the update button lives, so its own scanning
    // was the first thing the button's HTTP had to compete with - and it lost: the first press
    // from a panel sitting on this page reported "서버 연결 실패" while every other path to the
    // same server worked. A list that stops refreshing for the twenty seconds a pull takes costs
    // nobody anything.
    // The refresh below must still run while a pull is going: the hint line is the pull's only
    // report when no takeover screen is up, so returning early here would freeze the one label
    // that is supposed to be narrating.
    if (!fwpull_active() && !updatemode_active() && lv_tick_get() - last_scan >= 10000) {
        last_scan = lv_tick_get();
        net_scan_start();
    }
    page_settings_refresh();
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

// KEY LEFT, VALUE RIGHT, AND THE VALUE ELLIPSISES RATHER THAN BEING CUT.
//
// Both labels used to be LV_SIZE_CONTENT with SPACE_BETWEEN between them, which is correct right up
// to the point where key + value exceeds the row: then the value runs past the card's content box
// and the card clips it mid-glyph, with no ellipsis to say it happened. An SSID can be 32 bytes and
// a failure reason longer, so this was reachable in normal use and looked like a rendering fault.
//
// So the value gets the slack (flex_grow) and LV_LABEL_LONG_DOT, and its text is right-aligned so a
// short value still sits against the right edge exactly where SPACE_BETWEEN used to put it. The key
// stays content-sized: it is a fixed string chosen here and never the thing that overflows.
static lv_obj_t *build_info_row(lv_obj_t *parent, const char *key,
                                const lv_font_t *keyfont = &font_reg_12) {
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    flex_row(row, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    label(row, key, keyfont, C_TEXT_SECONDARY);
    lv_obj_t *val = label(row, "-", &font_bold_12, C_TEXT_DARK);
    lv_obj_set_flex_grow(val, 1);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    return val;
}

lv_obj_t *page_settings_build(lv_obj_t *parent) {
    lv_obj_t *page = plain(parent);
    s_page = page;
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    flex_row(page, 16, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(page, 16);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    // Left: merged WiFi card — status, connection info, scan, network list
    lv_obj_t *c = card(page, C_SURFACE, 14, true);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_height(c, LV_PCT(100));
    flex_col(c, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(c, 14);
    build_device_header(c, ICON_WIFI, C_BLUE_TINT, C_BLUE, "WiFi 설정", t_wifi, on_toggle_wifi);
    build_status_line(c, &st_wifi);
    v_ssid = build_info_row(c, "네트워크");
    v_ip = build_info_row(c, "IP 주소");
    v_rssi = build_info_row(c, "신호 세기");
    v_wifi_mac = build_info_row(c, "MAC");

    box(c, LV_PCT(100), 1, C_BORDER, 0);  // divider

    w_net_list = plain(c);
    lv_obj_set_width(w_net_list, LV_PCT(100));
    lv_obj_set_flex_grow(w_net_list, 1);
    flex_col(w_net_list, 4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(w_net_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(w_net_list, LV_DIR_VER);

    // Right: display (dark mode) card
    lv_obj_t *right = plain(page);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    flex_col(right, 16, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Same shape as the control-page cards: header (badge + name + toggle)
    // then a single status line. No extra info row.
    lv_obj_t *d = card(right, C_SURFACE, 14, true);
    lv_obj_set_width(d, LV_PCT(100));
    lv_obj_set_height(d, LV_SIZE_CONTENT);
    flex_col(d, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(d, 14);
    // Icon reflects the current theme: moon in dark, sun in light. The whole UI
    // rebuilds on toggle, so picking it at build time keeps it in sync.
    build_device_header(d, g_dark ? ICON_MOON : ICON_SUN, C_AMBER_TINT, C_AMBER, "화면 설정", t_dark, on_toggle_dark);
    build_status_line(d, &st_dark);

#if PANEL_OLED
    // The rear OLED belongs on the DISPLAY card, not the sensor node's.
    //
    // It was put on the node's card first, reasoning that the numbers it shows are that
    // node's readings. Wrong reason: this row answers "is the panel's second screen
    // working", which is a fact about THIS board. And the node card already carried five
    // rows inside a scrollable box, so the row landed below the fold and could not be
    // found at all - a diagnostic nobody can see is not a diagnostic. This card is
    // LV_SIZE_CONTENT and grows to fit.
    //
    // It is also the only window into that display: it lives on GPIO 43/44, which are
    // UART0's pins, so a build that finds it has no serial console by construction.
    v_oled = build_info_row(d, "보조 화면");
#endif

    // The camera used to take the whole remaining column. It now shares that
    // space with the sensor node: two info cards, equal halves, each scrollable
    // so neither clips if the row count grows. Both are read-only - no toggle.
    //
    // The camera's "네트워크" row is gone: the CAM is provisioned onto the very
    // network the WiFi card above already names, so it only ever repeated it.
    lv_obj_t *e = card(right, C_SURFACE, 14, true);
    lv_obj_set_width(e, LV_PCT(100));
    lv_obj_set_flex_grow(e, 1);
    flex_col(e, 6, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(e, 12);
    lv_obj_add_flag(e, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(e, LV_DIR_VER);

    lv_obj_t *ehdr = plain(e);
    lv_obj_set_width(ehdr, LV_PCT(100));
    lv_obj_set_height(ehdr, LV_SIZE_CONTENT);
    flex_row(ehdr, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    make_badge(ehdr, ICON_CAMERA, C_GREEN_TINT, C_GREEN);
    label(ehdr, "카메라 노드", &font_bold_14, C_TEXT_DARK);

    build_status_line(e, &st_cam);
    v_cam_ip = build_info_row(e, "IP 주소");
    v_cam_rssi = build_info_row(e, "신호 세기");
    v_cam_mac = build_info_row(e, "MAC");
    // Two links reach this one device and only one of them was ever on this card.
    // The status line above is the ESP-NOW beacon: provisioning and presence. The
    // pictures travel over HTTP, and camnet keeps an open session running after the
    // beacon lapses - so the monitor page could read MJPEG LIVE while every row
    // here said the camera was gone. Two screens, one camera, opposite answers.
    // This row is the video path speaking for itself, which is also the fact a
    // grower came to check.
    v_cam_video = build_info_row(e, "영상 수신");
    // RGB 사진 / RGB 스트림 / RGB RTSP USED TO BE THREE ROWS HERE, each holding a whole URL built
    // from the IP address two rows above. On a card this narrow that is ~35 characters in a slot
    // sized for ten, so all three were being cut mid-path - and now that the value labels ellipsise
    // instead of clipping, all three would read "http://192.168.10.7..." which is worse: a row that
    // is legibly useless. They were derivable from 주소 anyway, by anyone who already knows the
    // paths, and nobody who does not know them was going to learn them from a truncated string.
    //
    // What survives is the question a grower came to ask - 영상 수신, whether pictures are actually
    // arriving - and where the camera is. The paths live in esp32cam-streamer's own log banner, and
    // the panel now pushes that log to the server.
    v_plantnet = build_info_row(e, "식별 API");

    // Sensor node (ESP32 devkit): the other half. Its readings already have a
    // home on the monitor page, so this card carries what only a settings page
    // wants - is the ESP-NOW link healthy, and how fast is thermal arriving.
    // Green like the camera card: both are remote devices on the ESP-NOW link,
    // and amber is spoken for by the display/theme card above.
    lv_obj_t *n = card(right, C_SURFACE, 14, true);
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_set_flex_grow(n, 1);
    flex_col(n, 6, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(n, 12);
    lv_obj_add_flag(n, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(n, LV_DIR_VER);

    lv_obj_t *nhdr = plain(n);
    lv_obj_set_width(nhdr, LV_PCT(100));
    lv_obj_set_height(nhdr, LV_SIZE_CONTENT);
    flex_row(nhdr, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    make_badge(nhdr, ICON_NODE, C_GREEN_TINT, C_GREEN);
    label(nhdr, "센서 노드", &font_bold_14, C_TEXT_DARK);

    build_status_line(n, &st_node);
    v_node_age = build_info_row(n, "마지막 수신");
    v_node_rx = build_info_row(n, "텔레메트리");
    v_node_thermal = build_info_row(n, "열화상");
    v_node_peak = build_info_row(n, "피크 온도");

    rebuild_net_list();

    // Theme rebuild re-runs this builder; the timers must be created once.
    static lv_timer_t *s_timer = NULL;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(settings_timer_cb, 1000, NULL);
        lv_timer_create(skel_timer_cb, 120, NULL);
    }
    return page;
}
