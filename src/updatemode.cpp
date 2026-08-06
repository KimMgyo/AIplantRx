// "Get out of the way, a firmware update is coming."
//
// WHY THIS IS A MODE. An update on this board is a 2.5MB flash write over WiFi, and it has to
// share the chip with a 20fps MJPEG pull, an ESP-NOW radio taking three broadcasts a second, a
// blocking HTTP poll, and a 7" display. That is not a theory about contention - it was measured:
// during an update the camera's JPEG decode went 22.8ms -> 66.1ms per frame, the pull fell 20fps
// -> 11.4fps, and then the board took a task-watchdog reset 2.6 seconds later, twice in four
// attempts. With the camera standing down the same update went from 40 seconds to 18 and the
// resets stopped, 19 of 20 transfers landing.
//
// So the load-shedding is not a nicety, and it does not belong hidden inside the OTA task where
// only a push update can trigger it. It is a mode the board enters, on purpose, from three
// places: the arrival of a push upload, a command from the server, and fwpull.cpp - which is
// what the panel's own "최신 펌웨어로 업데이트" button and a server-requested pull both go
// through, because fetching an image contends for this chip exactly the way being pushed one
// does. Everything below therefore has two callers to satisfy, and veil_tick() shows whichever
// of the two transfers is the one actually running.
//
// WHO STANDS DOWN, AND WHO DOES NOT. The measurement decides this, not tidiness:
//
//   - camnet, the camera pull. Core 0, priority 2, and the one that starved the idle task the
//     watchdog watches. It stops.
//   - plantrx, the server poll. Blocks for a whole HTTP round trip with a 10s timeout, on the
//     main loop. It stops.
//   - ESP-NOW. Three broadcasts a second and a receive callback on the WiFi task. Deinitialised.
//   - LVGL keeps running. It is on core 1 and was never implicated; the update contends on core
//     0. Keeping it alive is what lets the panel show progress to the person who pressed the
//     button, and it means this mode does not have to rebuild the UI to leave.
//
// WHY THE ONLY WAY OUT IS A RESTART. ESP-NOW is torn down and the whole point of an update is
// that it ends in a reboot, so there is nothing to restore. What that costs is a board sitting
// dead if the update never arrives - a mistyped address, a laptop that went to sleep - so the
// mode carries its own deadline and restarts itself. A panel that comes back on its own after
// five idle minutes is recoverable by anybody; one that needs a walk to the greenhouse with a
// USB cable is the failure this whole day's work exists to stop repeating.
#include <Arduino.h>
#include <esp_now.h>
// Every word on this screen is a fixed literal, so tools/gen_fonts.py has it in the subset of
// whatever font draws it, at that font's own size - this screen needs nothing from a fallback.
// That was not true before: the subsets held 72-83 syllables, the rest arrived from the 12px
// fallback, and this note used to name the two fonts allowed to carry Korean here because they
// were the only ones that would not simply drop it.
//
// It still does not reach for a whole-block font. One at 20px was the first thing tried and linked
// 1.5MB of glyphs for a single screen - flash went 38.9% -> 61.7% - and it had no ASCII, so it
// could not have drawn the percentage anyway. The whole-block fonts that do ship
// (font_kr_full_12 / font_kr_full_14) exist for text neither program can name in advance, and
// nothing on this screen is that.
#include "fonts.h"
#include "fwpull.h"
#include "health.h"
#include "hlog.h"
#include "lvgl_v8_port.h"
#include "ota.h"
#include "ui_colors.h"
#include "updatemode.h"

// Five minutes of nothing arriving. Long enough to cover a person walking back to a laptop and
// typing the command, short enough that a mistake is over before it needs explaining. The clock
// is reset while a transfer is actually running, so a slow upload is never the thing that trips.
static const uint32_t IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;

static bool s_active = false;
static bool s_espnow_down = false;
static uint32_t s_deadline = 0;
static char s_why[48] = "";

static lv_obj_t *s_veil = nullptr;
static lv_obj_t *s_why_label = nullptr;
static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_status = nullptr;

// Refreshes the takeover screen. Runs as an LVGL timer rather than from loop(), because loop()
// is where plantrx used to block for a whole round trip and a progress bar that freezes for ten
// seconds reads as a crashed board - which is the exact misreading this panel keeps causing.
//
// Two different things can be running behind this screen - an image being pushed at the board
// and an image the board went and fetched - and only one line of text can say so, so the choice
// is made here rather than by whoever entered the mode. A pull wins whenever it is active,
// because it is the one that knows more: a push is a percentage and nothing else, while a pull
// spends its first seconds asking the server what the newest image is and comparing it against
// the one already running. Those stages have no percentage, but they have something worth
// saying, and the alternative was the idle countdown - which in that moment is a lie told in
// the most expensive place there is, telling somebody watching a download that nothing is
// coming.
static void veil_tick(lv_timer_t *t) {
    (void)t;
    if (!s_active || s_bar == nullptr) return;

    char buf[64];

    if (fwpull_active()) {
        int pct = fwpull_progress();
        // -1 here does not mean "nothing is happening", it means "this stage has no percentage":
        // asking for the manifest, comparing hashes, restarting. The bar sits at zero and the
        // words carry the state, because a bar moved to an invented number is the worse of the
        // two ways to be wrong about a flash write in progress.
        lv_bar_set_value(s_bar, pct >= 0 ? pct : 0, LV_ANIM_OFF);
        if (pct >= 0) {
            snprintf(buf, sizeof(buf), "%s  %d%%", fwpull_status(), pct);
            lv_label_set_text(s_status, buf);
        } else {
            lv_label_set_text(s_status, fwpull_status());
        }
        return;
    }

    int pct = ota_progress();
    if (pct >= 0) {
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d%%", pct);
    } else {
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        // The countdown is shown, not hidden, because "nothing is happening" and "nothing is
        // happening and this board will fix itself in 4:12" are different things to be told.
        //
        // A finished pull gets both, for the same reason rather than in spite of it. Its last
        // word says why no new firmware is coming - already up to date, or the install failed -
        // and dropping it for a bare countdown would leave somebody watching a board that just
        // failed with no account of it; keeping it instead of the countdown would leave them
        // reading 설치 실패 with no sign the panel comes back on its own. They are two halves of
        // one sentence and the screen has room for both.
        //
        // "Has a pull happened" is asked of fwpull_status() at draw time, and not remembered by
        // watching fwpull_active() go by: that module guarantees the string is empty until the
        // first fwpull_request() and non-empty forever after, refusal included - the guarantee
        // is written at s_status (fwpull.cpp:96) and again above fwpull_status() (:615), and the
        // symbols are named here because line numbers in a file still being edited drift while
        // a grep for them does not. Sampling instead would have been wrong and not merely
        // inelegant: this timer runs every 250ms while an "이미 최신" pull is one connect and one
        // manifest round trip, so roughly a fifth of the presses on an up-to-date board would
        // begin and end between two ticks and lose the only sentence explaining why nothing was
        // installed.
        uint32_t left = updatemode_left_s();
        if (fwpull_status()[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s  %lu:%02lu", fwpull_status(),
                     (unsigned long)(left / 60), (unsigned long)(left % 60));
        } else {
            snprintf(buf, sizeof(buf), "기다리는 중  %lu:%02lu",
                     (unsigned long)(left / 60), (unsigned long)(left % 60));
        }
    }
    lv_label_set_text(s_status, buf);
}

// The takeover screen lives on the top layer, so it covers whichever page was open without the
// page system having to know this mode exists - and being opaque and clickable, it also stops
// touches reaching controls that are about to be replaced by different firmware.
static void build_veil(void) {
    lv_obj_t *top = lv_layer_top();
    s_veil = lv_obj_create(top);
    lv_obj_remove_style_all(s_veil);
    lv_obj_set_size(s_veil, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_veil, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_veil, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_veil, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_veil, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_veil, 18, 0);

    lv_obj_t *title = lv_label_create(s_veil);
    lv_label_set_text(title, "펌웨어 업데이트");
    lv_obj_set_style_text_font(title, &font_bold_14, 0);
    lv_obj_set_style_text_color(title, C_TEXT_DARK, 0);

    s_why_label = lv_label_create(s_veil);
    lv_obj_set_style_text_font(s_why_label, &font_bold_14, 0);
    lv_obj_set_style_text_color(s_why_label, C_BLUE, 0);

    s_bar = lv_bar_create(s_veil);
    lv_obj_set_size(s_bar, 420, 14);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_status = lv_label_create(s_veil);
    lv_obj_set_style_text_font(s_status, &font_bold_14, 0);
    lv_obj_set_style_text_color(s_status, C_TEXT_DARK, 0);
    lv_label_set_text(s_status, "");

    lv_obj_t *note = lv_label_create(s_veil);
    lv_label_set_text(note, "전원을 끄지 마세요. 끝나면 저절로 다시 켜집니다.");
    lv_obj_set_style_text_font(note, &font_bold_12, 0);
    lv_obj_set_style_text_color(note, C_TEXT_DARK, 0);

    lv_timer_create(veil_tick, 250, nullptr);
}

void updatemode_enter(const char *why) {
    snprintf(s_why, sizeof(s_why), "%s", why ? why : "");
    s_deadline = millis() + IDLE_TIMEOUT_MS;

    if (s_active) {
        // Already in the mode. Only the reason line changes - a push upload arriving after
        // somebody pressed the button should say so rather than start a second takeover.
        lvgl_port_lock(-1);
        if (s_why_label) lv_label_set_text(s_why_label, s_why);
        lvgl_port_unlock();
        hlogf("[update] already in update mode (%s)\n", s_why);
        return;
    }

    // Set before anything else: the subsystems below poll this, and the sooner they notice the
    // sooner the update has the core to itself.
    s_active = true;
    hlogf("[update] entering update mode (%s)\n", s_why);

    lvgl_port_lock(-1);
    build_veil();
    lv_label_set_text(s_why_label, s_why);
    lvgl_port_unlock();

    // After the veil is up, not before: deinit takes the WiFi task through a teardown, and if it
    // were to hang the panel would still be showing a normal page with nothing explaining it.
    if (!s_espnow_down) {
        s_espnow_down = true;
        esp_err_t e = esp_now_deinit();
        hlogf("[update] esp-now down (%s)\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
    }
}

bool updatemode_active(void) { return s_active; }

uint32_t updatemode_left_s(void) {
    if (!s_active) return 0;
    uint32_t now = millis();
    return (now >= s_deadline) ? 0 : (s_deadline - now) / 1000;
}

void updatemode_tick(void) {
    if (!s_active) return;

    // A transfer in progress pushes the deadline out. The timeout is for an update that never
    // came, not for one that is taking its time over a weak link.
    //
    // A pull counts as a transfer for exactly that reason, and leaving it out would be a real
    // bug and not an omission: a pull is armed from inside this mode, so the clock is already
    // running when the download starts, and 2.5MB over a marginal greenhouse link can outlast
    // five minutes with nothing wrong. The board would then restart in the middle of writing the
    // image it was told to fetch - the deadline causing the exact failure it exists to prevent.
    // fwpull_active() covers the manifest fetch and the compare as well as the download, which
    // is what we want here: those are seconds of a slow server, not evidence that nobody is
    // sending anything.
    if (ota_active() || fwpull_active()) {
        s_deadline = millis() + IDLE_TIMEOUT_MS;
        return;
    }

    if (millis() >= s_deadline) {
        hlogf("[update] nothing arrived in %lu minutes - restarting\n",
              (unsigned long)(IDLE_TIMEOUT_MS / 60000UL));
        health_restart("update mode timeout");
    }
}
