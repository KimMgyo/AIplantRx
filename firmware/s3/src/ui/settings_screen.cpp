// Settings (UI.md 4): visionOS Settings without the search field. Sidebar on the left, one detail page.
// Sizes: ui/mockups/html/style.css, the visionOS section (rows 46, groups 21 apart, list padding 40...).
#include "ui_int.h"
#include "ui.h"
#include "app.h"
#include "fwupdate.h"
#include "llm.h"
#include "net.h"
#include "nodes.h"
#include "settings.h"
#include <WiFi.h>
#include <esp_mac.h>
#include <time.h>

enum Page { PG_WIFI, PG_AI, PG_PLANTNET, PG_GENERAL, PG_UPDATE, PG_DEVICES, PG_PLANT, PG_CONTROL, PG_LIGHT, PG_WATER,
            PG_CAMERA, PG_DISPLAY, PG_AITALK, PG_DEMO, PG_ABOUT, PG_COUNT };

struct Item { const char *title, *icon; uint32_t rgb; uint8_t group; };
static const int DETAIL_W = 558 - 2 * 40;   // platter width in the detail pane
static const Item ITEMS[PG_COUNT] = {
    {"Wi-Fi", F7_WIFI, 0x007AFF, 0}, {"AI 서버", F7_SPARKLES, 0x5856D6, 0}, {"PlantNet", F7_CAMERA_VIEWFINDER, 0x34C759, 0},
    {"일반", F7_GEAR_ALT_FILL, 0, 1}, {"소프트웨어 업데이트", F7_ARROW_DOWN_CIRCLE_FILL, 0, 1}, {"기기", F7_SQUARE_STACK_3D_UP_FILL, 0x007AFF, 1},
    {"식물 프로필", F7_LEAF_ARROW_CIRCLEPATH, 0x34C759, 2}, {"자동 제어", F7_THERMOMETER, 0xFF9500, 2}, {"조명", F7_LIGHTBULB_FILL, 0xFFCC00, 2},
    {"물·물약", F7_DROP_FILL, 0x32ADE6, 2}, {"카메라", F7_CAMERA_FILL, 0, 2},
    {"화면", F7_SUN_MAX_FILL, 0x007AFF, 3}, {"AI 한마디", F7_BUBBLE_LEFT_FILL, 0xAF52DE, 3}, {"시연 모드", F7_PLAY_RECTANGLE_FILL, 0xFF2D55, 3},
    {"정보", F7_INFO_CIRCLE_FILL, 0, 3},
};

static struct {
    int page, sel;
    lv_obj_t *rows[PG_COUNT], *rowval[PG_COUNT];
    lv_obj_t *list, *fade, *det, *title, *content, *last_space;
    lv_obj_t *scan_spin, *live;     // updated in place by the tick (no page rebuild)
    lv_img_dsc_t *bg, *fade_img;
    lv_img_dsc_t *side;
    lv_timer_t *tmr;
    uint32_t sig, scan_t;
} S;

static void build_page();

// ---------------------------------------------------------------- detail helpers

static lv_obj_t *space(lv_obj_t *par, int h) {
    lv_obj_t *o = box(par, 0, 0, LV_PCT(100), h);
    S.last_space = o;
    return o;
}

static lv_obj_t *plat(lv_obj_t *par) {
    lv_obj_t *o = box(par, 0, 0, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(o, P->plat, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 18, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    space(par, 18);
    // the spacer was created after the platter, so it sits below it
    return o;
}

static lv_obj_t *hdr(lv_obj_t *par, const char *t) {
    lv_obj_t *o = box(par, 0, 0, LV_PCT(100), 26);
    lv_obj_set_style_pad_left(o, 16, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(o, 8, 0);
    label(o, &f_b15, P->text, t);
    return o;
}

static void foot(lv_obj_t *par, const char *t) {
    if (S.last_space) lv_obj_set_height(S.last_space, 8);
    lv_obj_t *l = label(par, &f_r12, P->text2, t);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_pad_hor(l, 16, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(l, 3, 0);
    space(par, 18);
}

static lv_obj_t *row(lv_obj_t *pl, const char *title, const char *sub = nullptr) {
    bool first = lv_obj_get_child_cnt(pl) == 0;
    lv_obj_t *r = box(pl, 0, 0, LV_PCT(100), sub ? 56 : 49);
    lv_obj_set_style_pad_hor(r, 16, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 13, 0);
    if (!first) {
        // Full-width separator: the row's content area starts 16 px in, so it starts at -16.
        lv_obj_t *line = fill(r, 0, 0, DETAIL_W, 1, P->sep, 0);
        lv_obj_add_flag(line, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_pos(line, -16, 0);
    }
    if (title) {
        lv_obj_t *t = box(r, 0, 0, 1, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *tl = label(t, &f_m15, P->text, title);
        lv_obj_set_width(tl, LV_PCT(100));
        one_line(tl);
        if (sub) {
            lv_obj_t *s = label(t, &f_r12, P->text2, sub);
            lv_obj_set_width(s, LV_PCT(100));
            one_line(s);
        }
    }
    return r;
}

static lv_obj_t *val(lv_obj_t *r, const char *t) {
    lv_obj_t *l = label(r, &f_r15, P->text2, t);
    lv_obj_set_style_max_width(l, 300, 0);
    one_line(l);
    return l;
}

static void chev(lv_obj_t *r) { label(r, &i13, P->chev, F7_CHEVRON_RIGHT); }

static void tap(lv_obj_t *r, lv_event_cb_t cb, void *user) {
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, P->sel.c, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, P->sel.o ? P->sel.o : LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
}

static lv_obj_t *dot(lv_obj_t *r, bool on) {
    lv_obj_t *d = fill(r, 0, 0, 8, 8, on ? P->go : P->text3.c, 4);
    if (!on) lv_obj_set_style_bg_opa(d, P->text3.o, 0);
    return d;
}

static lv_obj_t *small_cap(lv_obj_t *r, const char *t, lv_event_cb_t cb, void *user, bool hi = false) {
    lv_obj_t *c = capsule(r, t, &f_s13, hi ? P->cap_hi : P->cap_plat, hi ? P->cap_hi_text : (P->glass ? lv_color_white() : P->cap_text), 28, 12);
    make_button(c, cb, user);
    return c;
}

static void restart_page() { build_page(); }

// ---------------------------------------------------------------- pages

static void wifi_toggle(lv_event_t *e) { net_set_enabled(toggle_on(lv_event_get_target(e))); }

static char s_join_ssid[33];
static void join_pass_done(const char *pass, void *) { net_join(s_join_ssid, pass); }
static void join_ssid_done(const char *ssid, void *) {
    if (!ssid[0]) return;
    strlcpy(s_join_ssid, ssid, sizeof s_join_ssid);
    char t[64];
    snprintf(t, sizeof t, "‘%s’의 암호 입력", ssid);
    sheet_text(t, "", true, join_pass_done, nullptr);
}
static void ap_tap(lv_event_t *e) {
    ApInfo *a = (ApInfo *)lv_event_get_user_data(e);
    strlcpy(s_join_ssid, a->ssid, sizeof s_join_ssid);
    if (a->secure) {
        char t[64];
        snprintf(t, sizeof t, "‘%s’의 암호 입력", a->ssid);
        sheet_text(t, "", true, join_pass_done, nullptr);
    } else net_join(a->ssid, "");
}
static void other_tap(lv_event_t *) { sheet_text("네트워크 이름", "", false, join_ssid_done, nullptr); }

static ApInfo s_aps[24];

static int bars(int rssi) { return rssi > -67 ? 2 : rssi > -78 ? 1 : 0; }

static void signal_icon(lv_obj_t *r, int rssi) {
    lv_obj_t *w = label(r, &i15, P->text, F7_WIFI);
    static const lv_opa_t OPA[3] = {LV_OPA_40, LV_OPA_70, LV_OPA_COVER};
    lv_obj_set_style_text_opa(w, OPA[bars(rssi)], 0);
}

static void page_wifi(lv_obj_t *c) {
    NetState st = net_state();
    lv_obj_t *p = plat(c);
    toggle(row(p, "Wi-Fi"), g_set.wifi_on, wifi_toggle, nullptr);
    if (st == NET_UP || st == NET_JOINING) {
        lv_obj_t *r = row(p, nullptr);
        if (st == NET_UP) label(r, &i15, P->text, F7_CHECKMARK);
        else spinner(r, 15);
        lv_obj_t *n = label(r, &f_m15, P->text, net_ssid().c_str());
        lv_obj_set_flex_grow(n, 1);
        if (st == NET_UP) {
            lv_obj_set_style_text_opa(label(r, &i12, P->text, F7_LOCK_FILL), LV_OPA_70, 0);
            signal_icon(r, net_rssi());
        } else val(r, "연결 중");
    }
    lv_obj_t *h = hdr(c, "네트워크");
    S.scan_spin = spinner(h, 13);
    show(S.scan_spin, net_scanning());
    p = plat(c);
    int n = net_aps(s_aps, 24);
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = row(p, nullptr);
        box(r, 0, 0, 15, 1);
        lv_obj_t *l = label(r, &f_m15, P->text, s_aps[i].ssid);
        lv_obj_set_flex_grow(l, 1);
        one_line(l);
        if (s_aps[i].secure) lv_obj_set_style_text_opa(label(r, &i12, P->text, F7_LOCK_FILL), LV_OPA_70, 0);
        signal_icon(r, s_aps[i].rssi);
        tap(r, ap_tap, &s_aps[i]);
    }
    lv_obj_t *o = row(p, nullptr);
    box(o, 0, 0, 15, 1);
    label(o, &f_m15, P->text, "기타...");
    tap(o, other_tap, nullptr);

    hdr(c, "기기 연결");
    p = plat(c);
    const char *nm[3] = {"카메라", "센서 노드", "OLED"};
    for (int k = 0; k < 3; k++) {
        NodeInfo ni = node_info((Role)k);
        lv_obj_t *r = row(p, nm[k]);
        dot(r, ni.online);
        val(r, ni.online ? ni.ip : "찾는 중");
    }
    foot(c, "Wi-Fi를 바꾸면 이 화면부터 옮기고, 다른 기기는 새 펌웨어에서 함께 옮깁니다.");
}

static void llm_url_done(const char *t, void *) { g_set.llm_url = t; settings_save(); restart_page(); }
static void llm_key_done(const char *t, void *) { g_set.llm_key = t; settings_save(); restart_page(); }
static void llm_url_tap(lv_event_t *) { sheet_text("AI 서버 주소", g_set.llm_url.c_str(), false, llm_url_done, nullptr); }
static void llm_key_tap(lv_event_t *) { sheet_text("AI 서버 키", g_set.llm_key.c_str(), true, llm_key_done, nullptr); }
static void llm_test_tap(lv_event_t *) { llm_test_start(); restart_page(); }

static void page_ai(lv_obj_t *c) {
    lv_obj_t *p = plat(c);
    lv_obj_t *r = row(p, "주소");
    val(r, g_set.llm_url.c_str());
    chev(r);
    tap(r, llm_url_tap, nullptr);
    r = row(p, "키");
    val(r, g_set.llm_key.length() ? "설정됨" : "없음");
    chev(r);
    tap(r, llm_key_tap, nullptr);
    p = plat(c);
    LlmTest t = llm_test();
    r = row(p, "연결 시험");
    if (t.state == LLM_RUNNING) { spinner(r, 15); val(r, "묻는 중"); }
    else small_cap(r, "시험", llm_test_tap, nullptr, true);
    if (t.state == LLM_OK || t.state == LLM_FAIL) {
        char b[96];
        if (t.state == LLM_OK) snprintf(b, sizeof b, "%.1f초 · %s", t.ms / 1000.0f, t.model);
        else snprintf(b, sizeof b, "응답 없음 · %s", t.err);
        val(row(p, "결과"), b);
        if (t.state == LLM_OK) val(row(p, "답"), t.reply);
    }
    foot(c, "PC의 llama-server(Bonsai 2 27B)에 Tailscale Funnel 주소로 연결합니다. 키는 이 기기에만 저장됩니다.");
}

static void pn_done(const char *t, void *) { g_set.plantnet_key = t; settings_save(); restart_page(); }
static void pn_tap(lv_event_t *) { sheet_text("PlantNet API 키", g_set.plantnet_key.c_str(), true, pn_done, nullptr); }

static void page_plantnet(lv_obj_t *c) {
    lv_obj_t *r = row(plat(c), "API 키");
    val(r, g_set.plantnet_key.length() ? "설정됨" : "없음");
    chev(r);
    tap(r, pn_tap, nullptr);
    foot(c, "my.plantnet.org에서 무료 키를 받을 수 있어요(하루 500회). 키는 이 기기에만 저장됩니다.");
}

static void restart_tap(lv_event_t *) { ESP.restart(); }

static void page_general(lv_obj_t *c) {
    lv_obj_t *p = plat(c);
    val(row(p, "기기 이름"), APP_HOST);
    val(row(p, "시간대"), "서울 (UTC+9)");
    char b[48];
    fmt_time(b, sizeof b);
    val(row(p, "지금 시각"), b);
    lv_obj_t *r = row(plat(c), "다시 시작");
    small_cap(r, "다시 시작", restart_tap, nullptr);
}

static void page_update(lv_obj_t *c) {
    lv_obj_t *s = box(c, 0, 0, LV_PCT(100), 118);
    lv_obj_t *ic = label(s, &i40, P->text, F7_ARROW_DOWN_CIRCLE_FILL);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *h = label(s, &f_b18, P->text, "AIplantRx " FW_VERSION);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 52);
    FwuInfo u = fwu_info();
    char b[96];
    switch (u.state) {
        case FWU_CHECKING: strlcpy(b, "새 버전을 확인하는 중", sizeof b); break;
        case FWU_LATEST: strlcpy(b, "최신 버전이에요", sizeof b); break;
        case FWU_AVAILABLE: snprintf(b, sizeof b, "새 버전 %s이 있어요", u.latest); break;
        case FWU_INSTALLING: snprintf(b, sizeof b, "%s 받는 중 %d%%", u.latest, u.pct); break;
        case FWU_RESTARTING: strlcpy(b, "다시 시작하는 중", sizeof b); break;
        case FWU_FAIL: snprintf(b, sizeof b, "확인하지 못했어요 · %s", u.err); break;
        default: strlcpy(b, u.pending ? "새 버전을 점검하는 중" : "아직 확인하지 않았어요", sizeof b); break;
    }
    lv_obj_t *t = label(s, &f_r13, P->text2, b);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t *p = plat(c);
    lv_obj_t *r = row(p, "업데이트 확인");
    if (u.state == FWU_CHECKING || u.state == FWU_INSTALLING || u.state == FWU_RESTARTING) spinner(r, 15);
    else if (u.state == FWU_AVAILABLE) small_cap(r, "지금 설치", [](lv_event_t *) { fwu_install_now(); }, nullptr, true);
    else small_cap(r, "확인", [](lv_event_t *) { fwu_check_now(); }, nullptr, true);
    if (u.checked) {
        time_t ts = u.checked;
        struct tm tm;
        localtime_r(&ts, &tm);
        snprintf(b, sizeof b, "%d월 %d일 %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        val(row(p, "마지막 확인"), b);
    }
    toggle(row(p, "자동 업데이트"), g_set.auto_update,
           [](lv_event_t *e) { g_set.auto_update = toggle_on(lv_event_get_target(e)); settings_save(); }, nullptr);
    if (u.rolled_back[0]) {
        snprintf(b, sizeof b, "%s → 이전 버전", u.rolled_back);
        val(row(p, "되돌림"), b);
    }
    foot(c, "GitHub Releases를 6시간마다 확인합니다. 자동 업데이트를 켜 두면 메인 화면을 10분 동안 아무도 만지지 않을 때 "
            "설치합니다. 시연 중에는 끄세요. 새 버전은 2분 안에 Wi-Fi에 다시 붙어야 확정되고, 아니면 이전 버전으로 돌아갑니다.");
    hdr(c, "기기");
    p = plat(c);
    val(row(p, "메인 화면", "S3 · " FW_VERSION " · 이 기기"), "현재");
    const char *nm[3] = {"카메라", "센서 노드", "OLED"};
    const char *hw[3] = {"CAM", "devkit", "C3"};
    for (int k = 0; k < 3; k++) {
        NodeInfo ni = node_info((Role)k);
        char sub[48];
        snprintf(sub, sizeof sub, "%s · %s", hw[k], ni.online ? (ni.ver[0] ? ni.ver : "버전 모름") : "찾는 중");
        val(row(p, nm[k], sub), ni.online ? "연결됨" : "-");
    }
    foot(c, "다른 기기의 업데이트(POST /ota)는 각 기기의 새 펌웨어와 함께 켭니다.");
}

static void page_devices(lv_obj_t *c) {
    lv_obj_t *p = plat(c);
    char sub[48];
    snprintf(sub, sizeof sub, "S3 · %s", net_ip().length() ? net_ip().c_str() : "주소 없음");
    lv_obj_t *r = row(p, "메인 화면", sub);
    dot(r, net_state() == NET_UP);
    const char *nm[3] = {"카메라", "센서 노드", "OLED"};
    for (int k = 0; k < 3; k++) {
        NodeInfo ni = node_info((Role)k);
        snprintf(sub, sizeof sub, "%s", ni.online ? ni.ip : "UDP 4210에서 찾는 중");
        r = row(p, nm[k], sub);
        dot(r, ni.online);
    }
    foot(c, "기기는 켜지면 UDP 4210으로 자기를 알립니다. 같은 Wi-Fi에 있으면 몇 초 안에 나타납니다.");
}

static void range(lv_obj_t *p, const char *name, float lo, float hi, const char *unit, int dec) {
    char b[40];
    snprintf(b, sizeof b, dec ? "%.1f–%.1f%s" : "%.0f–%.0f%s", lo, hi, unit);
    val(row(p, name), b);
}

static void reset_profile(lv_event_t *) {
    profile_default(g_prof);
    profile_save();
    if (g_main) main_destroy();     // rebuilt with the default profile when settings closes
    restart_page();
}

static void page_plant(lv_obj_t *c) {
    const Profile &p = g_prof;
    lv_obj_t *pl = plat(c);
    lv_obj_t *r = row(pl, p.identified ? p.name_ko : "기본 프로필", p.identified ? p.sci : "열대 관엽 · 식물을 찾으면 AI가 다시 정합니다");
    lv_obj_t *av = fill(r, 0, 0, 44, 44, P->ok_win, 22);
    lv_obj_move_to_index(av, 0);
    lv_obj_center(labelc(av, &i19, P->ok_text, F7_LEAF_ARROW_CIRCLEPATH));
    hdr(c, "목표값");
    pl = plat(c);
    range(pl, "낮 온도", p.day_lo, p.day_hi, "°C", 0);
    range(pl, "밤 온도", p.night_lo, p.night_hi, "°C", 0);
    range(pl, "습도", p.rh_lo, p.rh_hi, "%", 0);
    range(pl, "흙 수분", p.soil_lo, p.soil_hi, "%", 0);
    range(pl, "양분 EC", p.ec_lo, p.ec_hi, " mS/cm", 1);
    range(pl, "pH", p.ph_lo, p.ph_hi, "", 1);
    hdr(c, "시간표");
    pl = plat(c);
    char b[40];
    snprintf(b, sizeof b, "%02u:%02u–%02u:%02u", p.light_on_h, p.light_on_m, p.light_off_h, p.light_off_m);
    val(row(pl, "조명"), b);
    snprintf(b, sizeof b, "%u일마다 %uml", p.nutr_days, p.nutr_ml);
    val(row(pl, "물약"), b);
    snprintf(b, sizeof b, "흙 %.0f%% 아래면 %uml", p.soil_lo, p.water_ml);
    val(row(pl, "물"), b);
    if (p.identified) {
        lv_obj_t *rr = row(plat(c), "기본 프로필로 되돌리기");
        small_cap(rr, "되돌리기", reset_profile, nullptr);
    }
}

static lv_obj_t *node_row(lv_obj_t *c) {
    NodeInfo ni = node_info(ROLE_NODE);
    lv_obj_t *r = row(plat(c), "센서 노드");
    dot(r, ni.online);
    val(r, ni.online ? ni.ip : "찾는 중");
    return r;
}

static void page_control(lv_obj_t *c) {
    const Profile &p = g_prof;
    node_row(c);
    lv_obj_t *pl = plat(c);
    char b[48];
    snprintf(b, sizeof b, "낮 %.0f° · 밤 %.0f° 아래면 켬", p.day_lo, p.night_lo);
    val(row(pl, "히터"), b);
    snprintf(b, sizeof b, "흙 %.0f%% 아래면 %uml", p.soil_lo, p.water_ml);
    val(row(pl, "물 펌프"), b);
    snprintf(b, sizeof b, "%u일마다 %uml", p.nutr_days, p.nutr_ml);
    val(row(pl, "물약 펌프"), b);
    snprintf(b, sizeof b, "%.1f–%.1f 밖이면 맞춤", p.ph_lo, p.ph_hi);
    val(row(pl, "pH"), b);
    foot(c, "센서 노드가 연결되면 여기서 장치를 5초씩 시험하고, 하루 최대 물 양을 정할 수 있어요.");
}

static void page_light(lv_obj_t *c) {
    const Profile &p = g_prof;
    node_row(c);
    lv_obj_t *pl = plat(c);
    char b[24];
    snprintf(b, sizeof b, "%02u:%02u", p.light_on_h, p.light_on_m);
    val(row(pl, "켜는 시각"), b);
    snprintf(b, sizeof b, "%02u:%02u", p.light_off_h, p.light_off_m);
    val(row(pl, "끄는 시각"), b);
    int mins = (p.light_off_h * 60 + p.light_off_m) - (p.light_on_h * 60 + p.light_on_m);
    if (mins < 0) mins += 1440;
    snprintf(b, sizeof b, "%d시간 %d분", mins / 60, mins % 60);
    val(row(pl, "하루"), b);
}

static void page_water(lv_obj_t *c) {
    const Profile &p = g_prof;
    node_row(c);
    lv_obj_t *pl = plat(c);
    char b[32];
    snprintf(b, sizeof b, "흙 %.0f%% 아래면", p.soil_lo);
    val(row(pl, "물 주는 때"), b);
    snprintf(b, sizeof b, "%uml", p.water_ml);
    val(row(pl, "한 번에"), b);
    snprintf(b, sizeof b, "%u일마다 %uml", p.nutr_days, p.nutr_ml);
    val(row(pl, "물약"), b);
}

static void page_camera(lv_obj_t *c) {
    lv_obj_t *pl = plat(c);
    char b[32];
    NodeInfo cam = node_info(ROLE_CAM);
    lv_obj_t *r = row(pl, "RGB 카메라", cam.online ? cam.ip : nullptr);
    dot(r, cam_live());
    if (cam_live()) snprintf(b, sizeof b, "%.0f fps", cam_fps());
    else strlcpy(b, cam.online ? "영상 기다리는 중" : "찾는 중", sizeof b);
    S.live = val(r, b);
    ThermalInfo ti = thermal_info();
    r = row(pl, "열화상");
    dot(r, ti.live);
    val(r, ti.live ? "MLX90640 · 32×24" : "찾는 중");
    val(row(pl, "식물 상자"), "가운데 고정");
}

static void theme_tap(lv_event_t *e) {
    uint8_t t = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    // Deferred: the rebuild deletes this screen, which must not happen inside its own event.
    if (t != g_set.theme) lv_async_call([](void *u) { ui_set_theme((uint8_t)(intptr_t)u); }, (void *)(intptr_t)t);
}

static void page_display(lv_obj_t *c) {
    hdr(c, "테마");
    lv_obj_t *pl = plat(c);
    const char *nm[3] = {"유리", "다크", "라이트"};
    for (int t = 0; t < 3; t++) {
        lv_obj_t *r = row(pl, nullptr);
        lv_obj_t *ck = label(r, &i15, P->text, t == g_set.theme ? F7_CHECKMARK : "");
        lv_obj_set_width(ck, 15);
        label(r, &f_m15, P->text, nm[t]);
        tap(r, theme_tap, (void *)(intptr_t)t);
    }
    foot(c, "배치는 같고 색만 바뀝니다. 유리는 부팅 때 흐린 배경을 만들어 쓰고, 다크와 라이트는 단색으로 그립니다.");
}

static void ai_on_ev(lv_event_t *e) { g_set.ai_on = toggle_on(lv_event_get_target(e)); settings_save(); }
static void ai_flag_ev(lv_event_t *e) {
    bool *f = (bool *)lv_event_get_user_data(e);
    *f = toggle_on(lv_event_get_target(e));
    settings_save();
}

static void page_aitalk(lv_obj_t *c) {
    lv_obj_t *pl = plat(c);
    toggle(row(pl, "AI 한마디"), g_set.ai_on, ai_on_ev, nullptr);
    char b[24];
    snprintf(b, sizeof b, "%u분마다", g_set.ai_idle_min);
    val(row(pl, "아무 일 없을 때"), b);
    hdr(c, "최근 기록");
    val(row(plat(c), "아직 한 말이 없어요"), "");
    foot(c, "오늘 한 말을 모두 남깁니다. 같은 일이 생겨도 그때 값을 보고 새로 씁니다.");
    hdr(c, "바로 말할 때");
    pl = plat(c);
    toggle(row(pl, "열화상에 손이 보일 때"), g_set.ai_hand, ai_flag_ev, &g_set.ai_hand);
    toggle(row(pl, "장치가 움직였을 때"), g_set.ai_device, ai_flag_ev, &g_set.ai_device);
    toggle(row(pl, "값이 크게 바뀌었을 때"), g_set.ai_values, ai_flag_ev, &g_set.ai_values);
}

static void demo_ev(lv_event_t *e) { g_set.demo_fast = toggle_on(lv_event_get_target(e)); settings_save(); }

static void page_demo(lv_obj_t *c) {
    toggle(row(plat(c), "하루를 1분으로"), g_set.demo_fast, demo_ev, nullptr);
    foot(c, "켜면 조명과 물약 시간표가 하루를 1분으로 줄여 돌아갑니다. 시연이 끝나면 끄세요.");
}

static void page_about(lv_obj_t *c) {
    lv_obj_t *pl = plat(c);
    char b[64];
    val(row(pl, "이름"), APP_HOST);
    val(row(pl, "펌웨어"), FW_VERSION);
    val(row(pl, "Wi-Fi"), net_state() == NET_UP ? net_ssid().c_str() : "연결 안 됨");
    val(row(pl, "IP 주소"), net_state() == NET_UP ? net_ip().c_str() : "-");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    val(row(pl, "MAC"), b);
    uint32_t up = millis() / 1000;
    snprintf(b, sizeof b, "%lu시간 %lu분", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60));
    val(row(pl, "가동 시간"), b);
    snprintf(b, sizeof b, "내부 %u KB · PSRAM %.1f MB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0f);
    val(row(pl, "남은 메모리"), b);
}

typedef void (*page_fn)(lv_obj_t *);
static const page_fn PAGES[PG_COUNT] = {page_wifi, page_ai, page_plantnet, page_general, page_update, page_devices, page_plant,
                                        page_control, page_light, page_water, page_camera, page_display, page_aitalk, page_demo, page_about};

// What a page shows that can change while it is open. The page is rebuilt when this changes, so it
// holds only what changes the page's structure: numbers that tick (camera fps, the scan spinner) are
// updated in place by the tick instead.
static ApInfo s_aps_now[24];

static uint32_t hash_str(uint32_t s, const char *t) {
    while (*t) s = s * 31 + (uint8_t)*t++;
    return s;
}

static uint32_t page_sig() {
    uint32_t s = S.page;
    switch (S.page) {
        case PG_WIFI: {
            NetState st = net_state();
            s = s * 31 + st + g_set.wifi_on * 8;
            if (st == NET_UP || st == NET_JOINING) s = hash_str(s, net_ssid().c_str());
            if (st == NET_UP) s = s * 31 + bars(net_rssi());
            int n = net_aps(s_aps_now, 24);
            for (int i = 0; i < n; i++) s = hash_str(s, s_aps_now[i].ssid) * 31 + s_aps_now[i].secure * 4 + bars(s_aps_now[i].rssi);
            for (int k = 0; k < 3; k++) { NodeInfo ni = node_info((Role)k); s = ni.online ? hash_str(s * 31 + 1, ni.ip) : s * 31; }
            break;
        }
        case PG_AI: s = s * 31 + llm_test().state; break;
        case PG_UPDATE: s = s * 31 + fwu_gen(); [[fallthrough]];
        case PG_DEVICES: case PG_CONTROL: case PG_LIGHT: case PG_WATER:
            for (int k = 0; k < 3; k++) s = s * 31 + node_info((Role)k).online;
            s = s * 31 + net_state();
            break;
        case PG_CAMERA: s = s * 31 + cam_live() * 4 + thermal_info().live * 2 + node_info(ROLE_CAM).online; break;
        case PG_ABOUT: case PG_GENERAL: s = s * 31 + millis() / 60000 + net_state(); break;
        default: break;
    }
    return s;
}

// A finger on the screen, or a list still gliding after one: rebuilding now would delete the object
// under the finger and stop the scroll dead. The tick tries again on its next run.
static bool touch_busy() {
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
        if (i->proc.state == LV_INDEV_STATE_PRESSED || i->proc.types.pointer.scroll_obj) return true;
    return false;
}

static void build_page() {
    lv_coord_t y = S.content ? lv_obj_get_scroll_y(S.content) : 0;
    bool same = S.content && lv_obj_get_user_data(S.content) == (void *)(intptr_t)S.page;
    if (S.content) lv_obj_del(S.content);
    S.last_space = S.scan_spin = S.live = nullptr;
    set_text(S.title, ITEMS[S.page].title);
    S.content = lv_obj_create(S.det);
    lv_obj_remove_style_all(S.content);
    lv_obj_set_pos(S.content, 0, 60);
    lv_obj_set_size(S.content, 558, 420);
    lv_obj_set_style_pad_hor(S.content, 40, 0);
    lv_obj_set_style_pad_bottom(S.content, 12, 0);
    lv_obj_set_flex_flow(S.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(S.content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(S.content, LV_DIR_VER);
    lv_obj_set_user_data(S.content, (void *)(intptr_t)S.page);
    PAGES[S.page](S.content);
    if (same && y) { lv_obj_update_layout(S.content); lv_obj_scroll_to_y(S.content, y, LV_ANIM_OFF); }
    S.sig = page_sig();
}

// ---------------------------------------------------------------- sidebar

static void select_row(int pg) {
    // Only the old and the new row change (restyling all 15 redrew the whole sidebar).
    for (int i = 0; i < PG_COUNT; i++) {
        bool on = i == pg;
        if (S.sel >= 0 && i != pg && i != S.sel) continue;
        set_bg(S.rows[i], P->sel.c, on ? P->sel.o : LV_OPA_TRANSP);
        lv_coord_t bw = on && P->sel_line.o ? 1 : 0;
        if (lv_obj_get_style_border_width(S.rows[i], 0) != bw) lv_obj_set_style_border_width(S.rows[i], bw, 0);
    }
    S.sel = pg;
}

static void row_tap(lv_event_t *e) {
    int pg = (int)(intptr_t)lv_event_get_user_data(e);
    if (pg == S.page) return;
    S.page = pg;
    select_row(pg);
    if (pg == PG_WIFI) { net_scan(); S.scan_t = millis(); }
    build_page();
}

static void side_values() {
    for (int i = 0; i < PG_COUNT; i++) {
        if (!S.rowval[i]) continue;
        char b[40] = "";
        switch (i) {
            case PG_WIFI: strlcpy(b, net_state() == NET_UP ? net_ssid().c_str() : g_set.wifi_on ? "연결 안 됨" : "끔", sizeof b); break;
            case PG_AI: strlcpy(b, llm_ok() ? "연결됨" : "", sizeof b); break;
            case PG_PLANTNET: strlcpy(b, g_set.plantnet_key.length() ? "" : "키 없음", sizeof b); break;
            case PG_UPDATE: strlcpy(b, fwu_info().state == FWU_AVAILABLE ? "새 버전" : "", sizeof b); break;
            case PG_DEVICES: snprintf(b, sizeof b, "%d대", 1 + nodes_online()); break;
            case PG_PLANT: strlcpy(b, g_prof.identified ? g_prof.name_ko : "기본", sizeof b); break;
            case PG_DISPLAY: strlcpy(b, g_set.theme == 1 ? "다크" : g_set.theme == 2 ? "라이트" : "유리", sizeof b); break;
        }
        set_text(S.rowval[i], b);
    }
}

static void fade_ev(lv_event_t *) {
    if (S.fade) show(S.fade, lv_obj_get_scroll_y(S.list) > 2);
}

static void close_tap(lv_event_t *) { lv_async_call([](void *) { ui_show_main(); }, nullptr); }

static void tick(lv_timer_t *) {
    if (lv_scr_act() != g_settings || sheet_open()) return;
    side_values();
    if (S.page == PG_WIFI && millis() - S.scan_t > 15000) { net_scan(); S.scan_t = millis(); }
    if (S.scan_spin) show(S.scan_spin, net_scanning());
    if (S.live && cam_live()) {
        char b[16];
        snprintf(b, sizeof b, "%.0f fps", cam_fps());
        set_text(S.live, b);
    }
    if (page_sig() != S.sig && !touch_busy()) build_page();
}

lv_obj_t *settings_build(int page) {
    memset(&S, 0, sizeof(S));
    S.page = page;
    S.sel = -1;
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, P->screen, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    const uint16_t *side = P->glass ? glass_side() : nullptr;
    if (side) {
        S.bg = img_dsc(glass_bg(), SCR_W, SCR_H);
        lv_img_set_src(lv_img_create(scr), S.bg);
        S.side = img_dsc(side, SIDE_W, SCR_H);
        lv_img_set_src(lv_img_create(scr), S.side);
    }
    if (P->side_line.o) fill(scr, 241, 0, 1, SCR_H, P->side_line.c, 0);

    // sidebar
    S.list = lv_obj_create(scr);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_pos(S.list, 12, 18);
    lv_obj_set_size(S.list, 220, SCR_H - 18);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(S.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(S.list, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(S.list, 16, 0);
    lv_obj_add_event_cb(S.list, fade_ev, LV_EVENT_SCROLL, nullptr);

    lv_obj_t *acct = box(S.list, 0, 0, 220, 70);
    lv_obj_t *av = fill(acct, 4, 0, 44, 44, P->ok_win, 22);
    lv_obj_center(labelc(av, &i19, P->ok_text, F7_LEAF_ARROW_CIRCLEPATH));
    lv_obj_t *an = label(acct, &f_b15, P->text, g_prof.identified ? g_prof.name_ko : "식물 없음");
    lv_obj_set_pos(an, 57, 2);
    lv_obj_t *as = label(acct, &f_r12, P->text2, g_prof.identified ? "AIplantRx · AI 맞춤 환경\n사진과 재배 기록을 봅니다." : "AIplantRx · 기본 환경\n카메라로 식물을 찾아 주세요.");
    lv_obj_set_style_text_line_space(as, 3, 0);
    lv_obj_set_pos(as, 57, 22);

    int grp = -1;
    for (int i = 0; i < PG_COUNT; i++) {
        if (ITEMS[i].group != grp) {
            if (grp >= 0) box(S.list, 0, 0, 220, 21);
            grp = ITEMS[i].group;
        }
        lv_obj_t *r = box(S.list, 0, 0, 220, 46);
        lv_obj_set_style_radius(r, 14, 0);
        lv_obj_set_style_border_color(r, P->sel_line.c, 0);
        lv_obj_set_style_border_opa(r, P->sel_line.o, 0);
        lv_obj_set_style_pad_hor(r, 9, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 13, 0);
        vic(r, ITEMS[i].icon, ITEMS[i].rgb, 28, &i15);
        lv_obj_t *t = label(r, &f_m15, P->text, ITEMS[i].title);
        lv_obj_set_flex_grow(t, 1);
        if (i == PG_UPDATE) S.rowval[i] = nullptr;
        else {
            S.rowval[i] = label(r, &f_r13, P->text2, "");
            lv_obj_set_style_max_width(S.rowval[i], 96, 0);
            one_line(S.rowval[i]);
        }
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_tap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        S.rows[i] = r;
    }
    select_row(page);
    side_values();

    // The list fades out under the top edge once scrolled: a strip of the background with an alpha ramp.
    static uint8_t ramp[242 * 30];
    for (int j = 0; j < 30; j++) memset(ramp + j * 242, 255 - j * 255 / 29, 242);
    S.fade_img = piece(side, SIDE_W, P->screen, 0, 18, 242, 30, ramp);
    S.fade = lv_img_create(scr);
    lv_img_set_src(S.fade, S.fade_img);
    lv_obj_set_pos(S.fade, 0, 18);
    lv_obj_add_flag(S.fade, LV_OBJ_FLAG_HIDDEN);
    if (P->side_line.o) fill(scr, 241, 18, 1, 30, P->side_line.c, 0);

    // detail
    S.det = box(scr, 242, 0, SCR_W - 242, SCR_H);
    S.title = label(S.det, &f_b18, P->text, "");
    lv_obj_align(S.title, LV_ALIGN_TOP_MID, 0, 19);
    lv_obj_t *cl = fill(S.det, SCR_W - 242 - 16 - 34, 13, 34, 34, P->cap, 17);
    lv_obj_center(label(cl, &i15, P->text, F7_XMARK));
    make_button(cl, close_tap, nullptr);
    lv_obj_set_ext_click_area(cl, 10);
    build_page();

    lv_obj_update_layout(S.list);
    lv_obj_scroll_to_view(S.rows[page], LV_ANIM_OFF);
    fade_ev(nullptr);
    if (page == PG_WIFI) { net_scan(); S.scan_t = millis(); }
    S.tmr = lv_timer_create(tick, 500, nullptr);
    return scr;
}

void settings_destroy() {
    if (S.tmr) lv_timer_del(S.tmr);
    if (g_settings) lv_obj_del(g_settings);
    g_settings = nullptr;
    piece_free(S.fade_img);
    if (S.bg) free(S.bg);
    if (S.side) free(S.side);
    memset(&S, 0, sizeof(S));
}

