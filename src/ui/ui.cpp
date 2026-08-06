// Orchestrator: owns shared state, page switching, theme, and device actions.
#include "ui_internal.h"
#include "ui.h"
#include "net.h"
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

Page g_page = PAGE_AUTO;   // the AI view is the landing page; not persisted, see below
bool g_dark = true;  // dark is the default theme

// Every switch off. These booted fan/mist/pumpA/led ON, so after a power cut the
// panel came back asserting that the pump and the mister were running - a claim
// it has no basis for, and the one direction where being wrong floods a tray. A
// panel with no stored history knows nothing about what is plugged into it and
// must not claim anything is running. 전체 정지 already produces this exact
// all-off state, so it is a frame the UI is known to render.
//
// Every initializer here doubles as its own no-stored-key default:
// ui_prefs_load() hands each live value to getBool()/getInt() as the fallback,
// so this block is the single source of truth for a factory-fresh panel and
// there is no second default table to drift from it.
bool g_fan = false, g_heater = false, g_mist = false;
bool g_pumpA = false, g_pumpB = false, g_pumpC = false, g_led = false;
int g_fan_speed = 65;

static lv_obj_t *s_pages[4];

// ---------------------------------------------------------------------------
// Persistence (NVS)
// ---------------------------------------------------------------------------

// The user-set state the panel carries - the theme, the seven switches, the fan
// percentage above, plus the AI-RX mode switch over in page_auto.cpp - used to live only in
// RAM: a mains blip and the panel came back with the factory opinion.
//
// g_page is excluded on purpose. It is where the user happened to be standing,
// not a decision - restoring it would drop them on the settings page after an
// outage and start a WiFi scan nobody asked for. Do not "fix" that by adding it
// to the table below.
//
// Own namespace, not net.cpp's "net". Each module opens its own handle and a
// shared namespace would let two unrelated key sets collide silently.
static Preferences s_prefs;

// One row per persisted bool: the NVS key, the live global, and the value NVS is
// known to hold. That last field is what keeps a flush honest. The dirty flag
// only says *something* moved, so without the shadow one switch flip would
// rewrite all nine keys and a slider drag would wear the flash for the nine
// bools it never touched.
//
// Keys are frozen. NVS caps them at 15 characters and renaming one migrates
// nothing: the old entry is orphaned and the panel comes back with the
// fresh-boot default, which is the exact failure this section exists to remove.
struct BoolPref {
    const char *key;
    bool *live;
    bool stored;   // seeded by ui_prefs_load(); meaningless before that runs
};

static BoolPref s_bool_prefs[] = {
    { "dark",  &g_dark,         false },
    { "fan",   &g_fan,          false },
    { "heat",  &g_heater,       false },
    { "mist",  &g_mist,         false },
    { "pumpA", &g_pumpA,        false },
    { "pumpB", &g_pumpB,        false },
    { "pumpC", &g_pumpC,        false },
    { "led",   &g_led,          false },
    { "auto",  &g_auto_control, false },
};

static const int S_BOOL_PREF_N = (int)(sizeof(s_bool_prefs) / sizeof(s_bool_prefs[0]));

static int s_stored_fan_speed;   // the int counterpart of BoolPref::stored
static bool s_prefs_dirty;

// Set from LVGL event callbacks, cleared by the flush timer - both on the LVGL
// thread, so there is nothing to lock.
void ui_prefs_mark_dirty(void) { s_prefs_dirty = true; }

// Defined with the edge counter below, forward-declared because the seed has to
// happen here: the shadow must start at the values the restore just wrote, or the
// first refresh would report seven edges nobody made.
static void seed_switch_shadow(void);

void ui_prefs_load(void) {
    s_prefs.begin("ui", false);
    // Seeding `stored` from what was just read is what makes a boot with no
    // interaction write nothing at all: an absent key reads as the declared
    // default forever and is never written back to claim it was a choice.
    for (int i = 0; i < S_BOOL_PREF_N; i++) {
        BoolPref &p = s_bool_prefs[i];
        *p.live = s_prefs.getBool(p.key, *p.live);
        p.stored = *p.live;
    }
    g_fan_speed = s_prefs.getInt("fanpct", g_fan_speed);
    s_stored_fan_speed = g_fan_speed;
    seed_switch_shadow();
}

// 5s. The floor is a slider drag: LVGL fires VALUE_CHANGED on every touch move,
// so one adjustment is tens of events, and the interval has to outlast the whole
// gesture - including the look-at-the-fan-and-nudge-again that follows it - for
// it to collapse into a single write. The ceiling is that this interval IS the
// amount of user intent a mains blip still erases, and this module exists
// because that amount used to be all of it; at five seconds the user is by
// construction still standing at the panel.
//
// Deliberately shorter than page_control.cpp's six-second undo window, so an
// all-stop reaches flash even if the pill is still up. That is the safety
// action, it belongs in flash promptly, and the handful of keys a taken-back
// stop costs is nothing against NVS endurance - which the shadow comparison
// already ties to actual interaction rather than to this timer.
static const uint32_t PREFS_FLUSH_MS = 5000;

static void prefs_flush_cb(lv_timer_t *t) {
    if (!s_prefs_dirty) return;
    s_prefs_dirty = false;
    for (int i = 0; i < S_BOOL_PREF_N; i++) {
        BoolPref &p = s_bool_prefs[i];
        if (p.stored == *p.live) continue;
        p.stored = *p.live;
        s_prefs.putBool(p.key, p.stored);
    }
    if (s_stored_fan_speed != g_fan_speed) {
        s_stored_fan_speed = g_fan_speed;
        s_prefs.putInt("fanpct", g_fan_speed);
    }
}

// ---------------------------------------------------------------------------
// What the uplink cannot otherwise report
// ---------------------------------------------------------------------------

// The uplink samples actuator_intent once a poll, 60s apart at the idle cadence.
// A grower who turns the mister on and off again between two polls is invisible:
// both samples read identically and the server concludes nothing happened, so it
// goes on scoring a window a hand moved through as though the prescription had
// been left alone. 전체 정지 was the worst of it - the one action that means
// somebody disagreed with the prescription, and taken back inside its six-second
// undo window it left no trace anywhere.
//
// Two monotonic counters rather than flags, because a count is a fact and a flag
// is already an interpretation: the server diffs consecutive polls and gets the
// number of transitions that happened in between, without this file having to
// guess what a poll's worth of movement means. Never reset - a reboot restarts
// them at zero, which uptime_ms on the same telemetry already explains.
//
// An all-stop that switches four devices off is four edges AND one all-stop. The
// undo that reverses it is four more edges and does not decrement the all-stop
// count: the press happened, and a server told otherwise would be told a mis-tap
// never occurred.
static uint32_t s_switch_edges = 0;
static uint32_t s_allstops = 0;

// The switch states the edge count has already accounted for. Seeded by
// ui_prefs_load() from the restored values, so the restore itself is not counted
// as seven edges the user never made.
static bool s_switch_shadow[7];

static void switch_states(bool out[7]) {
    out[0] = g_fan;   out[1] = g_heater; out[2] = g_mist;  out[3] = g_pumpA;
    out[4] = g_pumpB; out[5] = g_pumpC;  out[6] = g_led;
}

static void seed_switch_shadow(void) { switch_states(s_switch_shadow); }

// Called from ui_devices_refresh(), which every path that writes a switch already
// goes through - the toggles, the all-stop, the undo restore. Counting here rather
// than at each of those is what makes the count complete by construction instead
// of by everyone remembering; a refresh with nothing changed finds no differences
// and costs seven compares.
static void count_switch_edges(void) {
    bool now[7];
    switch_states(now);
    for (int i = 0; i < 7; i++) {
        if (now[i] == s_switch_shadow[i]) continue;
        s_switch_shadow[i] = now[i];
        s_switch_edges++;
    }
}

uint32_t ui_switch_edges(void) { return s_switch_edges; }
uint32_t ui_allstop_count(void) { return s_allstops; }

// ---------------------------------------------------------------------------
// Cross-module actions
// ---------------------------------------------------------------------------

// Visibility only — no page-entry side effects (used by the theme rebuild).
static void show_page(Page p) {
    for (int i = 0; i < 4; i++) {
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_pages[p], LV_OBJ_FLAG_HIDDEN);
}

void ui_set_page(Page p) {
    g_page = p;
    topbar_close_menu();
    topbar_refresh();
    show_page(p);
    if (p == PAGE_SETTINGS) {
        page_settings_on_show();  // kick off an automatic WiFi scan
    } else {
        // Leaving settings: a scan may still be sweeping, and it holds the radio
        // off-channel long enough to stall the camera past its liveness window.
        // Stop it before anything else so the stream resumes immediately.
        net_scan_abort();
        if (p == PAGE_MONITOR) {
            page_monitor_on_show();  // paint a frame now, not on the next 33ms tick
        }
    }
}

void ui_devices_refresh(void) {
    count_switch_edges();     // before the repaint: every writer of a switch arrives here
    page_control_refresh();   // the monitor page no longer carries device pills
}

// Device actions, shared by the monitor pill strip and the control cards. Each
// marks the state dirty rather than writing it: the flush timer coalesces, so a
// flurry of taps is one write, and a tap taken back before the timer fires costs
// no write at all because the shadow still matches.
void on_toggle_fan(lv_event_t *e) { g_fan = !g_fan; ui_prefs_mark_dirty(); ui_devices_refresh(); }
void on_toggle_heater(lv_event_t *e) { g_heater = !g_heater; ui_prefs_mark_dirty(); ui_devices_refresh(); }
void on_toggle_mist(lv_event_t *e) { g_mist = !g_mist; ui_prefs_mark_dirty(); ui_devices_refresh(); }
void on_toggle_pumpA(lv_event_t *e) { g_pumpA = !g_pumpA; ui_prefs_mark_dirty(); ui_devices_refresh(); }
void on_toggle_pumpB(lv_event_t *e) { g_pumpB = !g_pumpB; ui_prefs_mark_dirty(); ui_devices_refresh(); }
void on_toggle_pumpC(lv_event_t *e) { g_pumpC = !g_pumpC; ui_prefs_mark_dirty(); ui_devices_refresh(); }
void on_toggle_led(lv_event_t *e) { g_led = !g_led; ui_prefs_mark_dirty(); ui_devices_refresh(); }

void on_all_stop(lv_event_t *e) {
    g_fan = g_heater = g_mist = g_pumpA = g_pumpB = g_pumpC = g_led = false;
    // Counted at the press and not at the state change, because an all-stop that
    // finds every switch already off moves nothing and is still somebody reaching
    // for the one control that means they disagree with the panel.
    s_allstops++;
    ui_prefs_mark_dirty();
    ui_devices_refresh();
}

// ---------------------------------------------------------------------------
// Actuator inventory (ui.h)
// ---------------------------------------------------------------------------

// One table, not a name switch beside a level switch. The two would be edited
// separately the day an eighth device lands and the uplink would then report
// the pump's position under the heater's name, which is unfalsifiable from the
// server side - every value is in range and no key is missing. Pairing the name
// with the pointer makes that drift impossible to express.
//
// `level` is null for the six on/off devices, which report 0 or 100. The fan is
// the only one with a percentage, and it reports 0 while its switch is off no
// matter where the slider sits: the slider keeps its position so the value is
// there when the fan comes back, but nothing is moving.
//
// The names are the wire vocabulary the server prompts the model with, so they
// are spelled exactly as the globals are and must not be prettified.
struct ActuatorEntry {
    const char *name;
    const bool *on;
    const int  *level;
};

static const ActuatorEntry s_actuators[] = {
    { "fan",    &g_fan,    &g_fan_speed },
    { "heater", &g_heater, nullptr },
    { "mist",   &g_mist,   nullptr },
    { "pumpA",  &g_pumpA,  nullptr },
    { "pumpB",  &g_pumpB,  nullptr },
    { "pumpC",  &g_pumpC,  nullptr },
    { "led",    &g_led,    nullptr },
};

static const int S_ACTUATOR_N = (int)(sizeof(s_actuators) / sizeof(s_actuators[0]));

int ui_actuator_count(void) { return S_ACTUATOR_N; }

const char *ui_actuator_name(int i) {
    if (i < 0 || i >= S_ACTUATOR_N) return "";
    return s_actuators[i].name;
}

// 0..100 without a clamp: g_fan_speed's only writer is the control page's
// slider, whose range is set to 20..100 at build. A three-digit ceiling is what
// plantrx.cpp sizes the request body against.
int ui_actuator_level(int i) {
    if (i < 0 || i >= S_ACTUATOR_N) return 0;
    const ActuatorEntry &a = s_actuators[i];
    if (!*a.on) return 0;
    return a.level ? *a.level : 100;
}

// ---------------------------------------------------------------------------
// Build / theme
// ---------------------------------------------------------------------------

static void ui_build(lv_obj_t *scr) {
    lv_obj_remove_style_all(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    flex_col(scr, 0, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    topbar_build(scr);

    lv_obj_t *content = plain(scr);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    flex_row(content, 0, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_pages[PAGE_AUTO] = page_auto_build(content);
    s_pages[PAGE_MONITOR] = page_monitor_build(content);
    s_pages[PAGE_CONTROL] = page_control_build(content);
    s_pages[PAGE_SETTINGS] = page_settings_build(content);

    menu_build(scr);  // last: dropdown must draw above page content

    // Exactly one page may be visible. Without this the four pages stay unhidden
    // and the flex row lays them out side by side, so the three inactive ones sit
    // off-screen to the right (the monitor page at x=800..1599 on an 800px panel)
    // instead of being hidden. They still tick: the camera page kept decoding JPEG
    // and upscaling both canvases into pixels LVGL then discarded, because
    // lv_obj_invalidate() drops an invalidation for an off-screen object.
    show_page(g_page);
}

static void ui_refresh_all(void) {
    page_monitor_refresh_sensors();
    ui_devices_refresh();
    page_settings_refresh();
    topbar_refresh();
}

static void theme_reload_cb(void *unused) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    ui_build(scr);
    ui_refresh_all();
    // Restore visibility WITHOUT re-running page-entry side effects; otherwise a
    // theme toggle on the settings page would kick off a fresh WiFi scan.
    // ui_build() already called show_page(), so nothing more is needed here.
}

void ui_set_dark(bool dark) {
    g_dark = dark;
    ui_prefs_mark_dirty();
    ui_theme_set_dark(dark);
    // Rebuild asynchronously: the toggle that fired this is destroyed by the
    // rebuild, and LVGL must not delete the widget mid-event.
    lv_async_call(theme_reload_cb, NULL);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// net_poll() advances WiFi/NTP/scan state; it's UI-agnostic but runs on the
// LVGL thread via this timer (all net_* reads happen on this thread too, so no
// locking is needed). Kept out of the topbar so network progress doesn't
// depend on that widget's timer.
//
// Polled at 250ms, not 1s: net_poll() acts on the driver's association-failure
// report, and that reaction time is the boot-to-online latency. The body early-
// returns unless something changed, so the extra ticks cost nothing measurable.
static void net_poll_timer_cb(lv_timer_t *t) { net_poll(); }

void ui_init(void) {
    ui_theme_set_dark(g_dark);  // the palette follows the restored theme, not the compiled default
    ui_build(lv_scr_act());
    ui_refresh_all();
    lv_timer_create(net_poll_timer_cb, 250, NULL);
    // Created here rather than in ui_prefs_load(): main.cpp calls that one before
    // taking the LVGL lock, and lv_timer_create() must not run outside it.
    lv_timer_create(prefs_flush_cb, PREFS_FLUSH_MS, NULL);
}
