// Internal wiring for the SmartFarm UI modules.
//
//   ui.cpp            orchestrator: ui_init, page switching, device actions
//   ui_helpers.cpp    tiny builders, icons, shared composite widgets
//   topbar.cpp        top bar + dropdown menu + clock
//   page_monitor.cpp  camera panel, sensor cards, device pill strip
//   page_control.cpp  3x3 device grid, fan slider, all-stop
//   page_settings.cpp WiFi card + network list
//   page_update.cpp   firmware: running image, boot history, the pull button
//
// Widget pointers live as statics inside their module; only state and
// cross-module actions cross this boundary.
#pragma once

#include <string.h>

#include "lvgl.h"
#include "fonts.h"
#include "ui_colors.h"

// ---------------------------------------------------------------------------
// Shared state (defined in ui.cpp)
// ---------------------------------------------------------------------------

// PAGE_UPDATE is the fifth item in the dropdown, and it did not used to be. It was
// a leaf reached from a card at the bottom of the settings page, on the argument
// that promoting maintenance to top-level navigation would put the one
// irreversible action on this panel a single tap from 모니터링.
//
// That argument was written when the card WAS the action - it fired the pull
// itself. The action has since moved onto the page along with its two-tap
// confirmation (see page_update.cpp), so the menu item reaches a page and nothing
// else: menu, then the page, then two deliberate taps on a control that names what
// it is about to replace. Three presses, not one, and the first two are free.
//
// What the old arrangement actually cost was discoverability. The way in was one
// card below three scrolling report cards on a page about WiFi, which is a strange
// place to keep the answer to "what is this board running".
//
// PAGE_COUNT sizes the page array and every loop over it. The literal 4 that used
// to be written three times in ui.cpp is why this is here: adding a page had to be
// remembered in each of them, and a forgotten one lays the new page out beside the
// visible one instead of hiding it (ui.cpp's show_page note says what that costs).
enum Page { PAGE_AUTO, PAGE_MONITOR, PAGE_CONTROL, PAGE_SETTINGS, PAGE_UPDATE, PAGE_COUNT };

extern Page g_page;
extern bool g_dark;
extern bool g_fan, g_heater, g_mist, g_pumpA, g_pumpB, g_pumpC, g_led;
extern bool g_auto_control;   // AI-RX mode: 자동 실행 applies the prescription, 판단 전용 only judges
extern int g_fan_speed;

// Call after changing any of the globals above except g_page, which is transient
// navigation and deliberately not stored (see the persistence block in ui.cpp).
// Cheap and idempotent - it only raises a flag, and a 5s LVGL timer does the
// writing, so a slider drag costs one NVS write instead of one per touch move.
// The load half is ui_prefs_load() in ui.h, because main.cpp has to run it
// before ui_init() builds the first frame.
void ui_prefs_mark_dirty(void);

// ---------------------------------------------------------------------------
// Shared display policy
// ---------------------------------------------------------------------------

// How old the judgment on screen may be before the panel says so, in seconds.
// Not a constant, because the answer depends on what the server promised for
// this device: a flat seven hours let a server on a five minute turn miss
// eighty-three of them without the panel saying a word.
//
// The ceiling is still seven hours, and still the same argument. clamp_wake()
// pins every wake the model asks for into MIN_INTERVAL_S .. MAX_INTERVAL_S
// (server/app/scheduler.py:25-26, :64), so six hours is the longest gap between
// two judgments that normal operation can produce - scheduler.py:24 calls that
// ceiling a heartbeat, and it is there precisely so a model answering "wake me
// tomorrow" cannot strand the panel. The device learns of the new prescription
// on its next poll, POLL_IDLE_S = 60s (scheduler.py:36), with the model's own
// round trip inside that same exchange. Nothing stretches the gap further: 지금
// 진단, the wake conditions and the local turn (JUDGE_TURN_MS, aijudge.cpp:59)
// only ever make a call happen sooner. So the worst healthy content age is 6h +
// one poll + a round trip, and this ceiling fires a full hour past that. It
// cannot go off while the model is working, and when it does the heartbeat
// itself has missed - which is the fault it is here to name.
//
// Below the ceiling the number comes from the cadence the server actually
// published, because that is the only promise a delay can be measured against.
// See rx_content_stale_s().
//
// Not in plantrx.h: it is a display policy, not a property of the client, and
// plantrx.cpp does nothing differently at seven hours. Not inside
// page_settings.cpp either, even though that page is the only caller left: the
// top bar held the second copy until its uplink indicator was removed, and two
// copies of a threshold is how the four thermal call sites drifted apart in the
// first place. One definition costs nothing to keep and re-earns itself the first
// time a second screen wants to say 지연.
static const int32_t RX_CONTENT_STALE_CEIL_S = 7 * 3600;

// The live threshold, in seconds. While the server owns the judgment turn its
// published period is the promise the panel holds it to: one whole missed turn
// plus the one in flight, plus two default polls for the device to hear the
// answer. Falls back to the ceiling when no server turn is live - which is also
// what the clamp returns for a server on the six hour heartbeat, so a panel
// talking to a slow server behaves exactly as it did before this became dynamic.
int32_t rx_content_stale_s(void);

// Whether the judgment column is empty because the server has no model at all,
// rather than because a judgment is late.
//
// A keyless server answers every poll with a clean 200 and an empty judgment
// column forever, so "the content is stale" is permanently true on a default
// install - and the settings block spent an unactionable amber on that for the
// life of the panel, as did the top bar's uplink line while the bar still had one.
// A configuration is not an event.
//
// The settings block names the state instead (정상 (모델 없음)), because that page
// is where a grower goes to find out why nothing is arriving. The reasoning that
// kept this off the top bar outlived the bar's indicator: a permanent line about a
// key, on every page, is exactly the kind of notice that teaches somebody to stop
// reading the bar - and the indicator was eventually removed in full for that same
// reason (topbar.cpp:202).
//
// The content age has to be absent, not merely old: a server that judged and then
// lost its key still holds a real judgment that is really ageing, and that one has
// earned its amber.
bool rx_no_model(void);

// ---------------------------------------------------------------------------
// Tiny builders (ui_helpers.cpp)
// ---------------------------------------------------------------------------

lv_obj_t *plain(lv_obj_t *parent);
lv_obj_t *box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t bg, lv_coord_t radius);
lv_obj_t *card(lv_obj_t *parent, lv_color_t bg, lv_coord_t radius, bool border);
lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
void flex_row(lv_obj_t *o, lv_coord_t gap, lv_flex_align_t main = LV_FLEX_ALIGN_START,
              lv_flex_align_t cross = LV_FLEX_ALIGN_CENTER);
void flex_col(lv_obj_t *o, lv_coord_t gap, lv_flex_align_t main = LV_FLEX_ALIGN_START,
              lv_flex_align_t cross = LV_FLEX_ALIGN_START);
void pad_all(lv_obj_t *o, lv_coord_t p);
void clickable(lv_obj_t *o);
void ignore_layout(lv_obj_t *o);
float clampf(float v, float lo, float hi);

// Cached label write, shared by every page that recomputes its strings from live
// state on a timer (page_auto 300ms, page_monitor 2s, page_settings 1Hz).
// lv_label_set_text reallocates the text and invalidates the object whether or
// not anything moved, and an invalidated label costs a row re-render on the next
// LVGL refresh. The cache is the label's own text, read back through
// lv_label_get_text(): the strcmp against it is a handful of bytes and no
// allocation, the invalidate and the redraw it schedules are neither. The NULL
// guard is load-bearing - callers hold widget pointers that are NULL until the
// page is built, and refresh can run first.
inline void ui_set_label_text(lv_obj_t *lbl, const char *txt) {
    if (lbl == NULL) return;
    const char *cur = lv_label_get_text(lbl);
    if (cur != NULL && strcmp(cur, txt) == 0) return;
    lv_label_set_text(lbl, txt);
}

// ---------------------------------------------------------------------------
// Icons (ui_helpers.cpp)
// ---------------------------------------------------------------------------

lv_obj_t *make_monitor_icon(lv_obj_t *parent, lv_color_t color);
lv_obj_t *make_hamburger_icon(lv_obj_t *parent, lv_color_t color);
// Gear: child 0 = ring (border-colored), children 1..4 = teeth (bg-colored).
lv_obj_t *make_gear_icon(lv_obj_t *parent, lv_color_t color);
lv_obj_t *make_wifi_bars(lv_obj_t *parent, int strength, lv_color_t on, lv_color_t off);

// Glyphs from the Lucide icon subset (font_icons). Pass one as the `icon` for
// make_badge / build_device_header, or render directly with font_icons.
#define ICON_WIFI     "\xEE\x86\xAE"  // U+E1AE wifi
#define ICON_CAMERA   "\xEE\x86\xA5"  // U+E1A5 video
#define ICON_PUMP     "\xEE\x82\xB4"  // U+E0B4 droplet
#define ICON_FAN      "\xEE\x8D\xB9"  // U+E379 fan
#define ICON_HEATER   "\xEE\x83\x92"  // U+E0D2 flame
#define ICON_MIST     "\xEE\x92\x95"  // U+E495 spray-can
#define ICON_LED      "\xEE\x87\x82"  // U+E1C2 lightbulb
#define ICON_SUN      "\xEE\x85\xB8"  // U+E178 sun (light theme)
#define ICON_MOON     "\xEE\x84\x9E"  // U+E11E moon (dark theme)
#define ICON_MONITOR  "\xEE\x87\x81"  // U+E1C1 layout-dashboard
#define ICON_CONTROL  "\xEE\x8A\x9A"  // U+E29A sliders-horizontal
#define ICON_SETTINGS "\xEE\x85\x94"  // U+E154 settings
#define ICON_AUTO     "\xEE\x90\x92"  // U+E412 sparkles (AI autonomous control)
#define ICON_NODE     "\xEE\x90\x83"  // U+E403 circuit-board (sensor devkit node)
#define ICON_FIRMWARE "\xEE\x93\xA5"  // U+E4E5 hard-drive-download (firmware page)

lv_obj_t *make_badge(lv_obj_t *parent, const char *icon, lv_color_t bg, lv_color_t fg);

// ---------------------------------------------------------------------------
// Shared composite widgets (ui_helpers.cpp)
// ---------------------------------------------------------------------------

struct ToggleWidgets {
    lv_obj_t *track;
    lv_obj_t *knob;
};

void build_toggle(lv_obj_t *parent, ToggleWidgets &t, lv_event_cb_t cb);
void update_toggle(ToggleWidgets &t, bool on, lv_color_t on_color);
void set_status(lv_obj_t *lbl, bool on, lv_color_t on_color, const char *txt);
// Card header: [badge + name] ... [toggle]
lv_obj_t *build_device_header(lv_obj_t *card_obj, const char *icon, lv_color_t badge_bg, lv_color_t badge_fg,
                              const char *name, ToggleWidgets &t, lv_event_cb_t cb);
// "<key>  ON/OFF" line. The key is a parameter because the two pages using this
// are entitled to different claims. WiFi and the theme really are in the state
// the line reports, so they keep 현재 상태. The 제어 page's seven cards are not:
// this board has no relay, no driver and nothing that could act on a switch
// (src/plantrx.cpp says so at the top), so "현재 상태 ON" over a pump card
// asserted that a pump was running on the authority of a switch wired to
// nothing. What the switch really is, is the grower's request - which is a real
// thing, reported every poll as actuator_intent and aggregated by the server's
// window summary - so the 제어 page passes 요청 and says that instead.
lv_obj_t *build_status_line(lv_obj_t *card_obj, lv_obj_t **status_out,
                            const char *key = "현재 상태");

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

void topbar_build(lv_obj_t *scr);   // top bar + clock timer
void menu_build(lv_obj_t *scr);     // dropdown; call last for z-order
void topbar_refresh(void);          // menu open state, pill label/icon, active item
void topbar_close_menu(void);       // used by ui_set_page on page change

lv_obj_t *page_monitor_build(lv_obj_t *parent);  // also starts the sensor mock timer
void page_monitor_refresh_sensors(void);
void page_monitor_on_show(void);   // repaint the camera immediately on return

lv_obj_t *page_auto_build(lv_obj_t *parent);
void page_auto_refresh(void);

lv_obj_t *page_control_build(lv_obj_t *parent);
void page_control_refresh(void);

lv_obj_t *page_settings_build(lv_obj_t *parent);
void page_settings_refresh(void);
void page_settings_on_show(void);  // starts a WiFi scan; called on page entry

lv_obj_t *page_update_build(lv_obj_t *parent);
void page_update_refresh(void);
void page_update_on_show(void);  // paint from live state on entry, not on the next tick

// ---------------------------------------------------------------------------
// Cross-module actions (ui.cpp)
// ---------------------------------------------------------------------------

void ui_set_page(Page p);
void ui_devices_refresh(void);  // refresh monitor pills + control cards
void ui_set_dark(bool dark);        // swaps palette and rebuilds the whole UI
void ui_theme_set_dark(bool dark);  // palette pointer only (ui_theme.cpp)

// Device actions, shared by the monitor pill strip and the control cards.
void on_toggle_fan(lv_event_t *e);
void on_toggle_heater(lv_event_t *e);
void on_toggle_mist(lv_event_t *e);
void on_toggle_pumpA(lv_event_t *e);
void on_toggle_pumpB(lv_event_t *e);
void on_toggle_pumpC(lv_event_t *e);
void on_toggle_led(lv_event_t *e);
void on_all_stop(lv_event_t *e);
