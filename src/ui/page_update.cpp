// Firmware page: three boards, one card each, one button each.
//
// WHY THREE IDENTICAL CARDS. The page used to be shaped like the panel: a column of this board's
// boot facts, a big button beside it, a 받아올 곳 card under that, and the two nodes squeezed into
// a third column with four chips apiece. Every one of those decisions was defensible on its own
// and together they said the wrong thing - that the panel is the subject and the two boards that
// cannot show anything themselves are an appendix. They are not. There are three firmwares in
// this greenhouse, exactly one question is ever asked about any of them, and the page that asks
// it should not make the grower learn a different layout per board.
//
// WHAT A CARD IS ALLOWED TO SHOW, and why the list is short. Identical cards can only carry facts
// all three boards can answer, and what all three can answer is NodeView (nodeota.h) - because
// that struct was already the vocabulary two of them report in over ESP-NOW. So the panel fills a
// NodeView about ITSELF (dev_view below) and past that call nothing here knows which board it is
// painting. Everything the old page showed that has no node equivalent went with the redesign,
// deliberately and on request: the slot address, the build stamp, the IDF version, the reset
// cause, the crash count, the revert target, and the 받아올 곳 card. They are all still in the
// serial log and the server's copy; none of them is what somebody standing here is deciding on.
//
// The four per-node chips went the same way. 업데이트 is the only control on this page now, which
// is what makes the three cards the same card - a 로그 ON/OFF that exists on two boards out of
// three is exactly the asymmetry this page was rebuilt to remove. The node log LIST went with the
// chip that fed it: without a stream to turn on it would have been twenty lines of boot noise. A
// node's newest words still reach the screen, on its own card's hint line, which is where a
// person looking at one board would look for them.
//
// ONE QUESTION AND ONE UPDATE AT A TIME, across all three. The confirmation is a single armed
// slot (s_armed), so two primed buttons cannot exist; and no card offers an update while any
// board is updating (any_update_running). The second rule closes a hole the split design had:
// the panel's own pull enters update mode, which deinitialises the ESP-NOW radio a running node
// update is reported over - so starting one during the other used to abandon it silently.
//
// The page scans for nothing, and ui_set_page() aborts the settings page's WiFi sweep on the way
// in. That guard stays where it is: the server can arm a pull while a grower is standing on the
// settings page, and that path never comes through here.
//
// THE ONLY WAY OFF THIS PAGE IS THE DROPDOWN, and that is asked for rather than overlooked. A
// ← 설정 bar sat at the top of this page and was removed: the dropdown already reaches all five
// views and lights 설정 while this page is up (topbar.cpp), so the bar was a second exit charged
// against the shortest axis on the panel.
#include <stdio.h>
#include <string.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "ui_internal.h"
#include "fwpull.h"
#include "health.h"
#include "net.h"
#include "plantrx.h"   // plantrx_configured(), plantrx_age_s() for the panel card's 서버 row
#include "nodeota.h"
#include "sitecfg.h"
#include "updatemode.h"

static lv_obj_t *s_page = NULL;   // page root; used to skip the 1Hz refresh when hidden

// ---------------------------------------------------------------------------
// One board, three ways of being one
// ---------------------------------------------------------------------------

static const char *dev_title(uint8_t role) {
    switch (role) {
        case NODE_ROLE_PANEL: return "스크린";
        case NODE_ROLE_CAM:   return "카메라 노드";
        default:              return "센서 노드";
    }
}

// The badge each device already wears elsewhere on the panel, with one forced change: the panel's
// own card used to borrow the sensor node's circuit-board glyph, which was safe only while the two
// never shared a screen. They share this one now, so the panel takes the layout-dashboard mark it
// already owns in the nav - it is, after all, the screen.
static const char *dev_icon(uint8_t role) {
    switch (role) {
        case NODE_ROLE_PANEL: return ICON_MONITOR;
        case NODE_ROLE_CAM:   return ICON_CAMERA;
        default:              return ICON_NODE;
    }
}

// Three devices, three colours, and they have to differ or the colour codes nothing. The CAM keeps
// the green it wears as a badge on the settings and monitor pages and the sensor node keeps blue -
// the palette's blue is the one that measures (see the note above `ai` in ui_colors.h) and
// measuring is the whole of what that board does. The panel takes amber, which is this page's own
// colour and the one the palette gives to configuring.
static lv_color_t dev_color(uint8_t role) {
    switch (role) {
        case NODE_ROLE_PANEL: return C_AMBER;
        case NODE_ROLE_CAM:   return C_GREEN;
        default:              return C_BLUE;
    }
}

static lv_color_t dev_tint(uint8_t role) {
    switch (role) {
        case NODE_ROLE_PANEL: return C_AMBER_TINT;
        case NODE_ROLE_CAM:   return C_GREEN_TINT;
        default:              return C_BLUE_TINT;
    }
}

// The two durations on a card are read against each other - how long since it spoke, how long it
// has been up - so they are rounded the way page_settings.cpp's rx_fmt_age rounds them: one unit
// that is always the largest meaningful one, and never a leading "0시간".
static void dev_fmt_age(char *buf, size_t cap, uint32_t ms) {
    if (ms < 60000)        snprintf(buf, cap, "%lu초 전", (unsigned long)(ms / 1000));
    else if (ms < 3600000) snprintf(buf, cap, "%lu분 전", (unsigned long)(ms / 60000));
    else                   snprintf(buf, cap, "%lu시간 전", (unsigned long)(ms / 3600000));
}

static void dev_fmt_uptime(char *buf, size_t cap, uint32_t s) {
    if (s < 3600)       snprintf(buf, cap, "%lu분", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(buf, cap, "%lu시간 %lu분",
                                 (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
    else                snprintf(buf, cap, "%lu일 %lu시간",
                                 (unsigned long)(s / 86400), (unsigned long)((s % 86400) / 3600));
}

static bool have_server(void) {
    const char *base = sitecfg_base_url();
    return base != NULL && base[0] != '\0';
}

// THE PANEL, DESCRIBED IN THE VOCABULARY ITS OWN NODES REPORT IN.
//
// Filling a NodeView rather than inventing a struct for this is the whole trick that makes one
// builder and one painter cover three boards. Every field below has the same meaning it has when
// it arrives over the air from the CAM, and each is read from the source the nodes read theirs
// from - esp_timer for uptime, ESP.getFreeHeap()'s underlying call for the heap, the ELF hash for
// the version - so the three cards can be compared down a row rather than only read across one.
//
// age_ms is the one field with no honest answer here: it means "how long since this board last
// spoke", and this board is the one doing the listening. UINT32_MAX is the painter's signal to
// draw the link state alone, which is why dev_fmt_age() no longer has a case for it.
static void panel_view(NodeView *v) {
    memset(v, 0, sizeof(*v));
    v->known   = true;            // it is the board asking the question
    v->online  = (net_state() == NET_CONNECTED);
    v->wifi    = v->online;
    v->pending = health_image_pending();
    // Computed, not assumed. It is the same call nodeagent.cpp makes to answer NODEF_CAN_OTA, so a
    // partition table without a second app slot greys this button out for the same reason and with
    // the same words as it does on a node - rather than offering a press whose only outcome is a
    // failure message.
    v->can_ota = (esp_ota_get_next_update_partition(NULL) != NULL);
    v->busy    = updatemode_active() || fwpull_active();
    v->phase   = NODE_PH_IDLE;    // fwpull reports in prose, not in this enum - see dev_refresh()
    v->pct     = fwpull_progress();
    v->uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000LL);
    v->free_heap = esp_get_free_heap_size();
    v->age_ms    = UINT32_MAX;

    const esp_app_desc_t *d = esp_app_get_description();
    if (d != NULL) {
        // Eight bytes, because that is what elf_sha[8] carries on the wire and what the server's
        // manifest is compared against (nodeproto.h's note on elf_sha[8]). Sixteen hex digits, the
        // same width the two node cards print, so all three rows are one identity in one format.
        for (int i = 0; i < 8; i++) snprintf(v->ver + i * 2, 3, "%02x", d->app_elf_sha256[i]);
    }
    // Writes "-" when the link is down, which is the sentinel NodeView.ip already documents, so
    // the painter needs no case of its own for this board.
    net_ip(v->ip, sizeof(v->ip));

    // "" until the first pull is ever requested and non-empty forever after, refusal included -
    // that is a contract fwpull.cpp states twice (the note on s_status, and again above
    // fwpull_status()). It is what lets the hint line below run the same ladder for all three:
    // empty means nothing has been asked of this board yet.
    const char *st = fwpull_status();
    if (st != NULL) snprintf(v->status, sizeof(v->status), "%s", st);
}

static void dev_view(uint8_t role, NodeView *out) {
    if (role == NODE_ROLE_PANEL) panel_view(out);
    else                         nodeota_view(role, out);
}

// No update may start while any update is running, on any of the three boards.
//
// Two of the three reasons were already enforced separately and the third was the hole. A node
// refuses a second node's update (nodeota_request), and the panel refuses its own while one is in
// flight - but the panel's pull enters update mode, and update mode deinitialises the exact
// ESP-NOW radio a running node update is reported over. So a press here used to be able to
// abandon a flash on another board, silently, with the overlay frozen on whatever phase it last
// saw. One rule for all three costs one comparison and cannot be got wrong per card.
static bool any_update_running(void) {
    return updatemode_active() || fwpull_active() || nodeota_busy_role() != NODE_ROLE_COUNT;
}

// ---------------------------------------------------------------------------
// One card
// ---------------------------------------------------------------------------

// Which of the hint line's jobs currently owns it. The colour follows from the mode alone, so
// tracking the mode is what lets the colour be written on a change instead of every second.
enum DevHintMode : uint8_t {
    HINT_NONE, HINT_ARMED, HINT_QUIET, HINT_NOOTA, HINT_SAID, HINT_GOOD, HINT_BUSY, HINT_IDLE,
};

// The 서버 row's staleness bound USED TO BE HERE. It is gone because the row no longer decides:
// plantrx.cpp owns RX_OK vs RX_STALE and this file asks it, which is one threshold instead of two
// that could disagree about the same server on two pages.

struct DevCard {
    uint8_t   role;
    lv_obj_t *link;                 // 연결 value: 온라인 · 3초 전
    lv_obj_t *ver, *ip, *up;
    lv_obj_t *srv;                  // 서버 value, PANEL only; NULL on the two node cards
    lv_obj_t *img_row;              // 검증 전 marker; hidden while this board's image is confirmed
    lv_obj_t *hint;
    lv_obj_t *btn, *btn_label;
    lv_obj_t *prog_track, *prog_fill;
    int8_t    link_shown;           // -1 never heard / 0 offline / 1 online; gates the colour write
    int8_t    srv_shown = -1;       // last 서버 state drawn; gates its colour write the same way
    uint8_t   hint_mode;            // DevHintMode
};

// Indexed by role directly. The old array skipped the panel to save a slot, which was true of a
// page where the panel had no card; it has one now and the subtraction was the only thing standing
// between sec_of() and being a bounds check.
static DevCard s_dev[NODE_ROLE_COUNT];

static inline DevCard *dev_of(uint8_t role) {
    return role < NODE_ROLE_COUNT ? &s_dev[role] : NULL;
}

static void dev_refresh(DevCard *c);

// The colour is a function of the mode alone, so it is written when the mode moves and not on
// every tick: lv_obj_set_style_text_color refreshes the style and invalidates whether or not the
// colour changed, which is the per-second waste ui_set_label_text exists to avoid.
//
// Three colours and a rule for each. Amber is spent on the two modes that are events - a question
// standing, an update running - and on nothing else: a permanent amber over a card that simply
// cannot OTA is an unactionable notice, and it teaches somebody to stop reading the line before
// the amber that matters arrives. Green is spent once, on the finished check that found the board
// already current, because that is a result and not a resting state. Everything else is
// secondary, including a failure - the words carry that, and this panel has no red that is not
// 긴급 정지's.
static void dev_hint(DevCard *c, uint8_t mode, const char *txt) {
    if (c->hint_mode != mode) {
        c->hint_mode = mode;
        lv_color_t col = C_TEXT_SECONDARY;
        if (mode == HINT_ARMED || mode == HINT_BUSY) col = C_AMBER;
        else if (mode == HINT_GOOD) col = C_GREEN;
        lv_obj_set_style_text_color(c->hint, col, 0);
    }
    ui_set_label_text(c->hint, txt);
}

// ---- the two-press confirmation -------------------------------------------
//
// A press that cannot be taken back must not be a press that can be made without meaning to, and
// these three buttons are the biggest touch targets on the panel - which is deliberate, and is
// exactly why the question is still asked. An update takes a board off the air for the length of a
// download and ends in a reboot into an image nobody has seen yet.
//
// The question goes in the button's own label, and it can here because the button is a full-width
// block: relabelling it moves nothing. That was the one reason the old per-node chips had to ask
// their question four pixels lower, on the card's hint line - a 60px chip that relabels itself to
// 정말? changes width and slides its neighbours out from under a finger already on its way down.
//
// It expires on its own for the mirror-image reason: the way out of a half-pressed update must not
// be another press on the control that starts one. Somebody who reads 정말 업데이트?, thinks
// better of it and walks away has already done everything required.
//
// ONE ARMED SLOT FOR THE WHOLE PAGE. Arming a second while the first stands would leave two primed
// buttons on one screen, and the whole point of the window is that exactly one thing is waiting.
static const int CONFIRM_SECONDS = 5;

static uint8_t s_armed = NODE_ROLE_COUNT;
static int s_armed_left;
// One timer for the life of the program, paused whenever nothing is pending. Creating it per press
// leaks a timer per tap, and this page is destroyed and rebuilt whole on every theme switch, which
// would leak the survivors again with no widget left for them to paint.
static lv_timer_t *s_armed_timer;

// All three buttons written from one place whenever the armed slot moves. Three style writes on a
// press and none on a tick, which is why this is not folded into dev_refresh().
static void confirm_apply(void) {
    for (uint8_t r = 0; r < NODE_ROLE_COUNT; r++) {
        DevCard *c = &s_dev[r];
        if (c->btn == NULL) continue;
        bool armed = (s_armed == r);
        lv_obj_set_style_bg_color(c->btn, armed ? C_AMBER_TINT : C_PILL_BG, 0);
        lv_obj_set_style_border_color(c->btn, armed ? C_AMBER : C_BORDER, 0);
        lv_obj_set_style_text_color(c->btn_label, armed ? C_AMBER : C_TEXT_DARK, 0);
        lv_label_set_text(c->btn_label, armed ? "정말 업데이트?" : "업데이트");
    }
}

static void confirm_paint_left(void) {
    DevCard *c = dev_of(s_armed);
    if (c == NULL || c->hint == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "다시 누르면 시작 · %d초 후 취소", s_armed_left);
    dev_hint(c, HINT_ARMED, buf);
}

static void confirm_clear(void) {
    uint8_t was = s_armed;
    s_armed = NODE_ROLE_COUNT;
    s_armed_left = 0;
    if (s_armed_timer != NULL) lv_timer_pause(s_armed_timer);
    confirm_apply();
    // Hand the hint line back to whoever owns it now, in this repaint rather than up to a second
    // later. A countdown frozen at "1초 후 취소" under a button that is no longer armed is the one
    // reading that makes the next press look like it did nothing.
    DevCard *c = dev_of(was);
    if (c != NULL) dev_refresh(c);
}

static void confirm_arm(uint8_t role) {
    s_armed = role;
    s_armed_left = CONFIRM_SECONDS;
    confirm_apply();
    confirm_paint_left();
    if (s_armed_timer != NULL) {
        lv_timer_reset(s_armed_timer);   // a fresh five seconds, not the remainder of an older one
        lv_timer_resume(s_armed_timer);
    }
}

static void confirm_tick(lv_timer_t *t) {
    if (--s_armed_left <= 0) {
        confirm_clear();
        return;
    }
    confirm_paint_left();
}

// ---- the press ------------------------------------------------------------

static void on_dev_update(lv_event_t *e) {
    DevCard *c = dev_of((uint8_t)(uintptr_t)lv_event_get_user_data(e));
    if (c == NULL || c->btn == NULL) return;
    // LV_STATE_DISABLED is a style state, not an input filter: lv_indev consults it on the keypad
    // path only (lv_indev.c:412), so a touch still lands here with the button greyed out. Arming a
    // confirmation on a control that cannot fire asks a question with no answer, and the answer is
    // the only thing a second press is for.
    if (lv_obj_has_state(c->btn, LV_STATE_DISABLED)) return;

    if (s_armed != c->role) {
        confirm_arm(c->role);
        return;
    }
    // The second tap inside the window. Everything past this line is one-way.
    confirm_clear();
    if (c->role == NODE_ROLE_PANEL) {
        // fwpull_request() does NOT take the panel over on its own: it asks the server what it has
        // first and enters update mode only if an install is actually going to happen. So a press
        // that finds this board already current, or cannot reach the server, leaves the page
        // exactly where it is - with the outcome on the hint line below.
        fwpull_request("버튼");
    } else {
        // "버튼" is the origin nodeota.cpp logs and shows; plantrx.cpp passes "서버" through the
        // same call for a pull the server armed, and the two are worth telling apart afterwards.
        // The overlay is NOT raised from here - see node_overlay_sync().
        nodeota_request(c->role, "버튼");
    }
    // Painted here as well as from the 1Hz poll, so a refusal reason or a first phase lands in the
    // repaint the press caused rather than up to a second later. Without it the panel looks like it
    // ignored the tap, and on this board the next thing tried is the power switch.
    dev_refresh(c);
}

// ---- one card's worth of paint --------------------------------------------

static void dev_refresh(DevCard *c) {
    if (c->hint == NULL) return;   // the poll timer can outlive a rebuild that has not run yet

    NodeView v;
    dev_view(c->role, &v);

    char buf[80];

    // The colour follows the online flag and nothing else, so it is written when that flag moves.
    // The age beside it changes on every tick and would otherwise drag a style refresh along with
    // it once a second, for a colour that moves twice a day.
    int8_t link = v.known ? (v.online ? 1 : 0) : -1;
    if (c->link_shown != link) {
        c->link_shown = link;
        lv_obj_set_style_text_color(c->link, link > 0 ? C_GREEN : C_TEXT_SECONDARY, 0);
    }
    if (!v.known) {
        ui_set_label_text(c->link, "기록 없음");
    } else if (v.age_ms == UINT32_MAX) {
        // The panel. There is no "since it last spoke" for the board doing the listening, so the
        // link state stands alone rather than being padded out to match the two cards beside it.
        ui_set_label_text(c->link, v.online ? "온라인" : "오프라인");
    } else {
        char age[24];
        dev_fmt_age(age, sizeof(age), v.age_ms);
        snprintf(buf, sizeof(buf), "%s · %s", v.online ? "온라인" : "오프라인", age);
        ui_set_label_text(c->link, buf);
    }

    ui_set_label_text(c->ver, v.ver[0] != '\0' ? v.ver : "-");

    // "no address" and "no radio" are different faults and the same blank otherwise. On the CAM and
    // the panel, off-WiFi is a failure - both of them do their work over TCP. On the sensor node it
    // is the DESIGN: that board joins for the length of an OTA download and drops straight back to
    // ESP-NOW, so a permanent 연결 안 됨 there would be a red herring on the one card where it
    // means nothing.
    //
    // This row said "WiFi 꺼짐" for the node and that was not enough. It is accurate and it still
    // reads as a fault - the first person to see it beside 온라인 asked why the node's WiFi was
    // off, which is the question the wording existed to prevent. The fix is to name what the board
    // IS doing rather than what it is not: this row answers "how is this board reachable", and for
    // the sensor node the answer is ESP-NOW, not an absence. During an OTA it joins and the real
    // address appears here, so the row stays informative in both states rather than being a
    // constant.
    if (v.wifi && v.ip[0] != '\0' && v.ip[0] != '-') {
        ui_set_label_text(c->ip, v.ip);
    } else {
        ui_set_label_text(c->ip, c->role == NODE_ROLE_NODE ? "ESP-NOW" : "연결 안 됨");
    }

    if (v.known) {
        dev_fmt_uptime(buf, sizeof(buf), v.uptime_s);
        ui_set_label_text(c->up, buf);
    } else {
        ui_set_label_text(c->up, "-");
    }

    // The one row only the panel has. Not "is the wire up" - the 연결 row above already answers
    // that from net_state() - but "is the far end still sending prescriptions", which is the failure
    // the uplink is designed to hide: the local rule keeps judging and the last prescription keeps
    // showing, so nothing on any other screen changes when the server goes away.
    //
    // State and freshness, and deliberately NOT the address. The address is a thing somebody set
    // once and can read in the console or on /admin; it is also the longest string that could go in
    // this row, on the narrowest cards in this UI. What changes, and what a person standing here can
    // act on, is whether it answered and how long ago.
    if (c->srv != NULL) {
        // plantrx_link() and not an age of my own. The first version of this row derived four
        // states from plantrx_age_s(), which reinvented an enum that already exists and got it
        // wrong in one place that matters: with no RX_ERROR case, a server whose last attempt
        // FAILED read as 지연 - a word that means "taking its time". ui_rx_word() is the one
        // vocabulary for these five states; see the table in ui_helpers.cpp.
        RxLink st = plantrx_link();
        // The age rides along on the two states where it adds something. 미설정 has nothing to be
        // old, and 대기 중 means nothing has arrived yet - an age beside either would be a number
        // describing an absence.
        int32_t age = plantrx_age_s();
        if (age >= 0 && (st == RX_OK || st == RX_STALE || st == RX_ERROR)) {
            char when[24];
            dev_fmt_age(when, sizeof(when), (uint32_t)age * 1000u);
            snprintf(buf, sizeof(buf), "%s · %s", ui_rx_word(st), when);
            ui_set_label_text(c->srv, buf);
        } else {
            ui_set_label_text(c->srv, ui_rx_word(st));
        }
        // Colour written only when the state changes, the way link_shown gates the row above:
        // lv_obj_set_style_text_color invalidates whether or not the colour moved, and this runs
        // every second.
        if (c->srv_shown != (int8_t)st) {
            c->srv_shown = (int8_t)st;
            lv_obj_set_style_text_color(c->srv, ui_rx_color(st), 0);
        }
    }

    // Drawn only while it means something: a line reading 확정 on three cards for the life of the
    // boot is three lines of nothing. Guarded on the current flag because lv_obj_add_flag and
    // lv_obj_clear_flag invalidate and dirty the parent's layout unconditionally for
    // LV_OBJ_FLAG_HIDDEN.
    bool pending = v.known && v.pending;
    if (lv_obj_has_flag(c->img_row, LV_OBJ_FLAG_HIDDEN) == pending) {
        if (pending) lv_obj_clear_flag(c->img_row, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(c->img_row, LV_OBJ_FLAG_HIDDEN);
    }

    // Every one of these is a refusal the request call would make anyway, and the hint line below
    // says which. The button is greyed as well because a live-looking control whose only possible
    // outcome is a message is precisely what NODEF_CAN_OTA exists to stop this panel offering - on
    // two devices out of three that the grower cannot walk over and look at.
    bool srv = have_server();
    bool ready = v.known && v.can_ota && srv && !any_update_running();
    if (ready) lv_obj_clear_state(c->btn, LV_STATE_DISABLED);
    else       lv_obj_add_state(c->btn, LV_STATE_DISABLED);

    // Hidden until the first byte lands: the check phase is two round trips, and a bar sitting at
    // 0% through it reads as a stall on a control whose only job then is to say "still moving".
    if (v.pct < 0) {
        if (!lv_obj_has_flag(c->prog_track, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(c->prog_track, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (lv_obj_has_flag(c->prog_track, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(c->prog_track, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_width(c->prog_fill, LV_PCT(v.pct > 100 ? 100 : v.pct));
    }

    // One line, five jobs, in the order somebody standing here needs them: the question they are
    // being asked, then the structural reason a control is dead, then whatever this board last said
    // about itself, then the instruction.
    //
    // The countdown owns the line while it is counting, so it is left alone: overwriting
    // "다시 누르면 시작 · N초 후 취소" would drop the one instruction that press depends on.
    if (s_armed == c->role) return;

    if (!v.known)          dev_hint(c, HINT_QUIET, "아직 응답 없음");
    else if (!v.can_ota)   dev_hint(c, HINT_NOOTA, "설치 영역이 없어 업데이트 불가");
    else if (!srv)         dev_hint(c, HINT_NOOTA, "서버 주소 없음");
    // NODE_PH_CURRENT earns the green: it is not "nothing happened", it is a completed check that
    // found this board already running the published image, and that is the answer the press was
    // asking for. The panel never reaches it - fwpull reports in prose and has no phase enum to
    // read - so its own already-current answer arrives in secondary, which is the honest colour
    // for a line this file cannot classify.
    else if (v.status[0] != '\0') {
        dev_hint(c, v.busy ? HINT_BUSY : (v.phase == NODE_PH_CURRENT ? HINT_GOOD : HINT_SAID),
                 v.status);
    }
    else dev_hint(c, HINT_IDLE, "누르면 확인 · 서버에서 받아 설치");
}

// ---------------------------------------------------------------------------
// The node overlay
// ---------------------------------------------------------------------------

// THIS IS NOT UPDATE MODE, and the distinction is the whole design.
//
// updatemode.cpp exists because flashing THIS board contends with a 20fps MJPEG pull, an ESP-NOW
// radio and a blocking HTTP poll on the same chip - measured, in its header comment: 22.8ms ->
// 66.1ms per decoded frame and a task-watchdog reset 2.6 seconds later. So it deinitialises
// ESP-NOW, stops camnet and plantrx, and the only way out is a restart.
//
// None of that applies here. The bytes land on another board over its own WiFi; this panel is
// spending one ESP-NOW frame every few seconds and it is the only thing watching. Entering update
// mode would tear down the exact radio the reports arrive on - the overlay would freeze on the
// first phase it saw and the panel would restart itself five minutes later having learned nothing.
// So this is a screen, on the top layer, over a page that is still running.
//
// It follows nodeota_busy_role() and nothing else. plantrx.cpp can arm a node update from a server
// poll with nobody standing at the panel (node_pull_cam / node_pull_node), so a press is not where
// the overlay can come from; the 1Hz refresh reads the state instead, and it does so before the
// hidden-page early return because an update armed from outside must show up on whatever page the
// panel happens to be sitting on.
static lv_obj_t *w_ov, *w_ov_status, *w_ov_track, *w_ov_fill;
static uint8_t s_ov_role = NODE_ROLE_COUNT;
// Dismissed for THIS update only. A full-screen modal is the right shape for a report nobody asked
// for and the wrong shape for a panel that also carries 긴급 정지: an update can take a minute, and
// a minute is a long time to be unable to stop a pump. 가리기 buys that minute back without the
// overlay having to guess when it is unwelcome.
static uint8_t s_ov_hidden = NODE_ROLE_COUNT;

static void node_overlay_drop(void) {
    if (w_ov == NULL) return;
    // Deleted asynchronously because one caller is 가리기's own click handler, and freeing an
    // object while LVGL is still walking its event list is a use-after-free. The pointers are
    // cleared here rather than in the callback, so nothing paints into the corpse in between.
    lv_obj_del_async(w_ov);
    w_ov = NULL;
    w_ov_status = NULL;
    w_ov_track = NULL;
    w_ov_fill = NULL;
    s_ov_role = NODE_ROLE_COUNT;
}

static void node_overlay_sync(void);

static void on_ov_hide(lv_event_t *e) {
    s_ov_hidden = s_ov_role;
    node_overlay_sync();
}

static void node_overlay_raise(uint8_t role) {
    node_overlay_drop();
    s_ov_role = role;

    // Dimmed to the same 120 the settings page's dialog backdrop uses, and that number is the
    // message: updatemode's takeover screen is opaque because the panel behind it is gone, and
    // this one lets the page read through because it is not. Clickable with no handler, so a tap
    // lands here instead of on a control underneath - there is no tap-to-dismiss, because a stray
    // palm must not clear the report of a flash in flight. 가리기 below is the way out.
    w_ov = plain(lv_layer_top());
    lv_obj_set_size(w_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(w_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(w_ov, 120, 0);
    clickable(w_ov);

    lv_obj_t *b = card(w_ov, C_SURFACE, 14, true);
    lv_obj_set_width(b, 420);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 0);
    flex_col(b, 12, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    pad_all(b, 20);

    lv_obj_t *hdr = plain(b);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex_row(hdr, 10, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_badge(hdr, dev_icon(role), C_AMBER_TINT, C_AMBER);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s 업데이트 중", dev_title(role));
    label(hdr, buf, &font_bold_14, C_TEXT_DARK);

    // The node's own words, and font_bold_14 specifically. tools/gen_fonts.py derives the subset
    // charset from src/, include/ and server/app - it never reads sensor_node/ or
    // esp32cam-streamer/ - so a syllable a node phrase uses and this panel's own strings never did
    // arrives from the fallback. font_bold_14 is the one subset whose fallback is 14px
    // (font_kr_full_14); every other face here would draw that syllable at 12px inside a 14px
    // line, which is the exact bug that script's header was written about.
    w_ov_status = label(b, "진행 중", &font_bold_14, C_AMBER);
    lv_obj_set_width(w_ov_status, LV_PCT(100));
    lv_label_set_long_mode(w_ov_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(w_ov_status, LV_TEXT_ALIGN_CENTER, 0);

    // A track with one child, the shape the cards' own bars and page_monitor.cpp's sensor bars
    // already use, rather than lv_bar.
    w_ov_track = box(b, LV_PCT(100), 8, C_PROGRESS_TRACK, 4);
    lv_obj_set_style_pad_all(w_ov_track, 0, 0);
    w_ov_fill = box(w_ov_track, 0, 8, C_AMBER, 4);
    lv_obj_add_flag(w_ov_track, LV_OBJ_FLAG_HIDDEN);

    label(b, "패널은 계속 동작 중입니다", &font_reg_12, C_TEXT_SECONDARY);

    lv_obj_t *hide = box(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT, C_PILL_BG, 8);
    lv_obj_set_style_pad_hor(hide, 16, 0);
    lv_obj_set_style_pad_ver(hide, 8, 0);
    clickable(hide);
    lv_obj_add_event_cb(hide, on_ov_hide, LV_EVENT_CLICKED, NULL);
    label(hide, "가리기", &font_bold_12, C_TEXT_SECONDARY);
}

static void node_overlay_sync(void) {
    uint8_t busy = nodeota_busy_role();

    // The 가리기 latch belongs to one update. Releasing it the moment the busy role changes - to
    // another board, or to none - is what stops a grower who waved off the camera's screen from
    // silently never seeing the sensor node's.
    if (s_ov_hidden != NODE_ROLE_COUNT && busy != s_ov_hidden) s_ov_hidden = NODE_ROLE_COUNT;

    if (busy == NODE_ROLE_COUNT || busy == s_ov_hidden) {
        node_overlay_drop();
        return;
    }
    if (w_ov == NULL || s_ov_role != busy) node_overlay_raise(busy);

    NodeView v;
    nodeota_view(busy, &v);
    ui_set_label_text(w_ov_status, v.status[0] != '\0' ? v.status : "진행 중");

    // Hidden until the first byte lands, exactly as a card's own bar is: NODE_PH_ASK is a manifest
    // fetch and a hash comparison, and a bar sitting at 0% through it reads as a stall on a screen
    // whose only job is to say "still moving".
    if (v.pct < 0) {
        if (!lv_obj_has_flag(w_ov_track, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(w_ov_track, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (lv_obj_has_flag(w_ov_track, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(w_ov_track, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_width(w_ov_fill, LV_PCT(v.pct > 100 ? 100 : v.pct));
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void page_update_refresh(void) {
    // BEFORE the hidden-page return, and the only thing on this page that is. plantrx.cpp arms a
    // node update straight off a server poll (node_pull_cam / node_pull_node) with nobody standing
    // here, so the overlay cannot be a thing this page raises only while it is the page being
    // looked at - it has to reach whatever the panel is showing. Everything below is a row on a
    // hidden page and is worth exactly nothing until somebody navigates back.
    node_overlay_sync();

    if (s_page != NULL && lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) return;

    for (uint8_t r = 0; r < NODE_ROLE_COUNT; r++) dev_refresh(&s_dev[r]);
}

// Called by ui_set_page() on entry. Most of what is here is a boot fact, so nothing is stale in the
// way a sensor reading is - but a page arriving unpainted for up to a second on a screen whose
// whole job is to be read reads as a hang, and the 1Hz timer skips this page while it is hidden.
void page_update_on_show(void) {
    // Never arrive on a primed button. The countdown clears itself within five seconds either way,
    // so this closes a small window rather than a hole - but an armed press is a question that was
    // asked of somebody who has since walked away, and it must not still be standing when the next
    // person arrives.
    confirm_clear();
    page_update_refresh();
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

// KEY LEFT, VALUE RIGHT, AND THE VALUE ELLIPSISES RATHER THAN BEING CUT. Same change and same
// reason as page_settings.cpp's build_info_row: two SIZE_CONTENT labels under SPACE_BETWEEN are
// correct until key + value exceeds the row, and then the card clips the value mid-glyph with
// nothing to say it happened. Three cards side by side on this panel makes each one narrow, so a
// sixteen-hex-digit version and an IPv4 address are already close to the edge.
static lv_obj_t *info_row(lv_obj_t *parent, const char *key, lv_obj_t **row_out = NULL) {
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    flex_row(row, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    label(row, key, &font_reg_12, C_TEXT_SECONDARY);
    if (row_out != NULL) *row_out = row;
    lv_obj_t *val = label(row, "-", &font_bold_12, C_TEXT_DARK);
    lv_obj_set_flex_grow(val, 1);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    return val;
}

// ONE BUILDER, THREE CARDS. The boards differ in the role they carry, the badge they wear and the
// word in the header, and every one of those is an argument or a lookup on the role. A copy of this
// per board is how a fix to the camera's card silently stops applying to the sensor node's, which
// is the same debt nodeproto.h was written to stop paying on the wire - and with the panel folded
// in there is nowhere left for the three to drift apart.
static void dev_card_build(lv_obj_t *parent, uint8_t role) {
    DevCard *c = &s_dev[role];
    c->role = role;   // set here and nowhere else, so the slot and the field cannot disagree

    // Three equal columns of (768 - 2 gaps) / 3 = 245px, full height, and the same card for all
    // three. Scrollable in the one case that overflows: a failure reason runs to NODEPROTO_TEXT and
    // wraps to eight lines at this width, and a clipped reason is the one time the reader loses the
    // answer. Nothing scrolls in the ordinary case - the rows and a one-line hint fit with 150px
    // left over for the button.
    lv_obj_t *cd = card(parent, C_SURFACE, 14, true);
    lv_obj_set_flex_grow(cd, 1);
    lv_obj_set_height(cd, LV_PCT(100));
    flex_col(cd, 6, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(cd, 14);
    lv_obj_add_flag(cd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cd, LV_DIR_VER);

    lv_obj_t *hdr = plain(cd);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    flex_row(hdr, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    make_badge(hdr, dev_icon(role), dev_tint(role), dev_color(role));
    label(hdr, dev_title(role), &font_bold_14, C_TEXT_DARK);

    c->link = info_row(cd, "연결");
    c->ver  = info_row(cd, "버전");
    // "IP" and not "IP 주소": the settings page said IP 주소 for the same fact and one of the two
    // had to go. Both are this one now - these three cards are the narrowest containers in the UI,
    // and the word 주소 buys nothing beside a dotted quad.
    c->ip   = info_row(cd, "IP");
    c->up   = info_row(cd, "가동");
    // 남은 메모리 WAS HERE AND IS NOT COMING BACK. A grower cannot act on a heap figure, and the
    // person who can read it in the console this panel pushes to the server, per second, with a
    // low-water mark and a block count beside it. A row on a 7" screen that only a developer can
    // use is a row the three people who use this screen have to scroll past.
    //
    // THE PANEL'S CARD CARRIES ONE ROW THE NODES DO NOT: whether the server is answering.
    //
    // Nine rows about the uplink used to live in the WiFi card on the settings page, where they
    // squeezed the network list down to two visible networks - see the note where they were
    // removed. This is what is left of them, and it is here rather than there because this is the
    // page where the server is already the subject: it is the thing serving the versions in the row
    // above. `online` on this card means WiFi is associated, which is a different question and the
    // reason both rows exist.
    if (role == NODE_ROLE_PANEL) {
        c->srv = info_row(cd, "서버");
    }

    // Built once with its wording and colour fixed, then only shown or hidden. The row exists to
    // say a thing that is either true or absent - there is no third text for it to hold - so
    // dev_refresh() spends a flag test on it per tick and never a string.
    lv_obj_t *img = info_row(cd, "이미지", &c->img_row);
    lv_label_set_text(img, "검증 전");
    lv_obj_set_style_text_color(img, C_AMBER, 0);
    lv_obj_add_flag(c->img_row, LV_OBJ_FLAG_HIDDEN);

    // WRAP and not DOT. This line carries each board's own phrases, which run to NODEPROTO_TEXT on
    // the two nodes - and the one time it is longest is a failure reason, which is the one time an
    // ellipsis costs the reader the answer. The card scrolls instead.
    //
    // font_reg_12, and the same face on all three cards. On the panel every string in it is one of
    // this file's or fwpull's and is in the generated subset; on the two nodes some of it comes
    // from firmwares tools/gen_fonts.py never scans and resolves through the 12px fallback, which
    // is a look rather than a truncation.
    c->hint = label(cd, "", &font_reg_12, C_TEXT_SECONDARY);
    lv_obj_set_width(c->hint, LV_PCT(100));
    lv_label_set_long_mode(c->hint, LV_LABEL_LONG_WRAP);
    // A SPACER TAKES THE SLACK, AND IT MUST NOT BE THE LABEL. Growing c->hint was tried and it
    // crash-looped this board 48 times in one flash: LV_LABEL_LONG_WRAP derives a label's HEIGHT
    // from its width by laying the text out, and flex_grow assigns its height from what is left in
    // the column - so LVGL is asked to solve a height that depends on a width that depends on the
    // height it is being handed. A plain object has no content-driven size and no opinion to
    // conflict with, so growing one is unambiguous.
    lv_obj_t *spacer = plain(cd);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_height(spacer, 0);
    lv_obj_set_flex_grow(spacer, 1);

    // SLIM, AND AT THE BOTTOM. This used to be the button that took everything left in the column -
    // flex_grow(1) with a 72px floor - on the reasoning that a consequential control should be
    // impossible to miss. It was impossible to miss and it was also most of the card: three of them
    // side by side made the page read as three buttons with some text above, on a page whose
    // subject is what each board is RUNNING.
    //
    // SIZE_CONTENT with a 44px floor instead. 44px is a deliberate number and not a small one: it
    // is the smallest touch target worth shipping, and this control still has the confirmation
    // behind it for the other half of mis-tap safety. Content-sized also means it GROWS by itself
    // while a download runs, because the progress track below is hidden until then - so the one
    // moment the button needs to be bigger is the one moment it is.
    c->btn = card(cd, C_PILL_BG, 12, true);
    lv_obj_set_width(c->btn, LV_PCT(100));
    lv_obj_set_height(c->btn, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(c->btn, 44, 0);
    flex_col(c->btn, 6, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pad_all(c->btn, 8);
    clickable(c->btn);
    // The role travels in the user data rather than in a per-button struct: the handler needs
    // exactly one number to find its card, and a pointer-sized integer is what LVGL already
    // carries for free on every event.
    lv_obj_add_event_cb(c->btn, on_dev_update, LV_EVENT_CLICKED, (void *)(uintptr_t)role);
    // Already updating means the offer is meaningless, and a control that still looks live invites
    // the press that proves it is not. Object opacity, so the label and the bar fade with the card
    // and the state needs no second style to stay consistent.
    lv_obj_set_style_opa(c->btn, LV_OPA_50, LV_STATE_DISABLED);

    // font_bold_14, like every card header on this panel, and not a larger face: this label is
    // rewritten at runtime (the question) and only fonts declaring .fallback = &font_kr_full_12 can
    // be trusted with Korean nobody checked against a subset - server/tests/test_font_coverage.py
    // fails the suite over exactly that. The wording is written by confirm_apply() at the end of
    // the build, so it lives in one place instead of being duplicated here.
    c->btn_label = label(c->btn, "", &font_bold_14, C_TEXT_DARK);

    // A track with one child, the shape page_monitor.cpp already uses for its sensor bars, rather
    // than lv_bar: two objects against that widget's internals, on a page whose whole cost comes
    // out of the internal DRAM every LVGL object is allocated from.
    c->prog_track = box(c->btn, LV_PCT(80), 6, C_PROGRESS_TRACK, 3);
    lv_obj_set_style_pad_all(c->prog_track, 0, 0);
    c->prog_fill = box(c->prog_track, 0, 6, C_AMBER, 3);
    lv_obj_add_flag(c->prog_track, LV_OBJ_FLAG_HIDDEN);

    // Every change-test cache back to "unpainted", because the widgets above are new objects with
    // default colours and a cache that survived the rebuild would suppress the first write.
    c->link_shown = -1;
    c->hint_mode = HINT_NONE;

    dev_refresh(c);
}

lv_obj_t *page_update_build(lv_obj_t *parent) {
    // The three cards ARE the page: a flex row with no wrapper, so the widths come straight off the
    // content box and there is no intermediate object to keep in agreement with it.
    lv_obj_t *page = plain(parent);
    s_page = page;
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    flex_row(page, 16, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(page, 16);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    dev_card_build(page, NODE_ROLE_PANEL);
    dev_card_build(page, NODE_ROLE_CAM);
    dev_card_build(page, NODE_ROLE_NODE);

    if (s_armed_timer == NULL) {
        s_armed_timer = lv_timer_create(confirm_tick, 1000, NULL);
        lv_timer_pause(s_armed_timer);
    }
    // Idle wording and colours, and the reset that matters across a theme switch: ui_set_dark()
    // cleans the screen and rebuilds it in one pass (ui.cpp theme_reload_cb), so no tick can land
    // on the destroyed labels - but a confirmation armed before the switch would come back as a
    // primed button nobody remembers arming, one tap from taking a board off the air.
    confirm_clear();
    // Plus one the confirmation does not have: the overlay lives on lv_layer_top(), which
    // ui_set_dark() does NOT clean (it rebuilds lv_scr_act()). So the object survives a theme
    // switch holding pointers into a palette that has been swapped underneath it. Dropping it here
    // lets the next sync raise a fresh one in the new theme, one tick later at worst.
    node_overlay_drop();

    // Theme rebuild re-runs this builder; the poll must be created once. 1Hz and not faster: the
    // only field that moves between ticks is a download bar, and a 3.5MB fetch does not need more
    // than a second's resolution to read as moving.
    static lv_timer_t *s_timer = NULL;
    if (s_timer == NULL) {
        s_timer = lv_timer_create([](lv_timer_t *t) { page_update_refresh(); }, 1000, NULL);
    }
    return page;
}
