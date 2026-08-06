// AI-RX: the AI's reasoning, laid out by tense.
//
// Three columns, one per tense - 판단(past) / 예약(future) / 조치(executed).
// The earlier version had "마지막 실행" in one card and the same event as the
// first row of "최근 조치" in another, so two cards said the same thing at the
// same timestamp; and a 목표 row ("VPD 0.8 - 1.2 kPa") was unjudgeable because
// the current value lived on a *different* card. Splitting by tense makes that
// class of duplication structurally impossible: an entry has exactly one tense.
//
// 판단 IS ONE CARD, NOT A LIST. That column used to stack up to six rows, each a
// clipped fragment of what the model actually said: render.py kept only the last
// sentence of the diagnosis and cut it to 21 characters, the notes never left the
// server at all, and JUDGE_HEAD_BYTES byte-clipped whatever survived. Six copies
// of a mangled headline is not a history - it is one finding, repeated, with the
// reasoning behind it thrown away. The column now draws aijudge_at(0) and nothing
// else, and spends the whole width on it: the prose in full, the readings behind
// it in one wrapping chip row, and the two frames the verdict was looking at side
// by side, AIJUDGE_THUMB_W twice plus a 6px gap across the column's 222px. The
// ring still keeps older rows, but for aijudge.cpp's per-origin transition test
// and not for this page. The countdown to the next judgment moved here too, above
// the card, because what it counts to is that card's sequel.
//
// Identification is the page's header strip, not a card: it is one line of state
// (species + confidence + quota) that everything below is conditioned on. It
// also carries the AI-RX mode toggle, since the toggle's meaning is page-global.
//
// SENSOR NUMBERS ARE CHIPS, PROSE IS A LINE. A chip means "a reading, with its
// unit" - it is scannable and comparable across rows. A sentence means "a reason
// or a condition", which is not. Mixing the two into one grey line, which is what
// this page used to do, forced the reader to parse every row to find the number
// that mattered. On the 판단 card exactly one chip is `hot` - the reading the
// verdict turned on - and it takes the verdict's own colour, so the badge and the
// number that caused it are visually one thing; the rest are the metrics that
// were live at that instant, each in the server's own tone for it.
//
// WHERE EACH COLUMN'S TEXT COMES FROM. The strip is plantid.cpp / PlantNet plus
// the species the server resolved; the 판단 column is aijudge.cpp's single ring,
// which the local threshold rule and the server's model both append to; the 예약
// and 조치 columns are plantrx.cpp's parsed prescription and nothing else. So the
// provenance badge is live state, not a literal: 패널 for a row the local rule
// wrote, 모델 for one the server's model wrote, 서버 for one its own threshold
// rule wrote instead - a keyless server authors rows and is not a broken server -
// and 미연결 on the two server-only columns when no server is configured and they
// consequently have no author at all.
//
// AN EMPTY COLUMN MUST SAY WHY. "no server is configured", "the server has not
// answered yet" and "the server answered and had nothing to schedule" are three
// different facts, and a column that draws all three as blank space reads as a
// broken screen. The uplink can be down for hours - naming the state is what
// keeps the panel honest while it is.
//
// Model-authored text MUST use font_reg_12 / font_bold_14 / font_bold_12. Every
// live subset declares `.fallback = &font_kr_full_12` now, so that is no longer
// what picks these three out - tools/gen_fonts.py declares the fallback for all
// six, after four of them had it patched in by hand where a regeneration would
// have silently dropped it. What picks these three out is that the columns are
// the only place arbitrary Korean lands, and the fallback is one face: 12px bold.
// So font_reg_12 and font_bold_12 take a model sentence at its own size (reg_12
// gets it in bold), and font_bold_14 is the one place a headline can still change
// size mid-string. Fixed UI Korean does not fall back at all any more - all 244
// glyphs either program can name are in every subset at its own size, · → ℃
// included.
#include "ui_internal.h"
#include "ui.h"
#include "plantid.h"
#include "aijudge.h"
#include "plantrx.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#define ICON_BOT "\xEE\x86\xBB"  // Lucide "bot" (U+E1BB), rendered with font_bot

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static lv_obj_t *s_page = NULL;
static ToggleWidgets s_auto_toggle;
static lv_obj_t *w_mode_label;
static lv_obj_t *w_mode_conflict = NULL;  // "서버: ..." - only while the two disagree
static lv_obj_t *w_species, *w_sci, *w_conf_chip, *w_quota_chip;
static lv_obj_t *w_judge_list = NULL;
static lv_obj_t *w_plan_list = NULL, *w_action_list = NULL;
static lv_obj_t *w_judge_badge = NULL, *w_plan_badge = NULL, *w_action_badge = NULL;
static lv_obj_t *w_turn_caption = NULL;  // "다음 OO 턴": whose turn is being counted
static lv_obj_t *w_turn_label = NULL;    // that turn's countdown, MM:SS
static lv_obj_t *w_ask_btn = NULL;       // 지금 진단
static uint32_t s_judge_rev = 0;         // gates the 판단 rebuild (aijudge_revision)
static uint32_t s_rx_rev = 0;            // gates the 예약/조치 rebuild (plantrx_revision)
static PlantIdState s_id_last = (PlantIdState)-1;
static uint32_t s_id_rx_rev = 0;         // the species line moves with a prescription too
static int8_t s_ask_on = -1;             // -1: the button's enabled look is unapplied
static int8_t s_mode_conflict = -1;      // -1 unapplied, 0 hidden, 1/2 the server's mode
static char s_turn_last[8] = "";         // last string written, to skip redraws

bool g_auto_control = true;   // ON: the AI applies its own prescription; persisted, see ui.cpp

// One row of either server-fed column, flattened so both build through the same
// widget. `tail` is the second line; `tail_is_reading` decides how it renders - a
// measured delta is a chip, a triggering condition is prose - and `tail_improved`
// tints that chip, because a delta that moved away from the target is not good
// news and must not be drawn in green. `tail` is "" when there is neither,
// because "조치 없음" is a decision and has no delta.
struct AiEntry {
    const char *at;
    const char *tag;
    lv_color_t tag_bg;
    lv_color_t tag_fg;
    const char *head;
    const char *tail;
    bool tail_is_reading;
    bool tail_improved;
};

// RxTone is the server's own read of a row - it worked / it did not / nothing to
// say - mapped onto the three tint pairs this page already draws with. Warn takes
// the same amber the 판단 column gives 주의, so one colour never means two
// different things across the three columns.
static void tone_colors(RxTone t, lv_color_t *bg, lv_color_t *fg) {
    switch (t) {
        case RX_TONE_OK:   *bg = C_GREEN_TINT; *fg = C_GREEN; break;
        case RX_TONE_WARN: *bg = C_AMBER_TINT; *fg = C_AMBER; break;
        default:           *bg = C_BLUE_TINT;  *fg = C_BLUE;  break;
    }
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

// Remaining PlantNet calls across all rotated keys - the one number that limits
// how often the loop can start.
//
// The figure is the server's now, quoted back on every identify reply, so before
// the first identification of a boot there is nothing to quote and the accessors
// say so with -1. That is not a quantity and must not be printed as one: "-"
// is what the rest of the panel already uses for a value it has not been told
// yet (see camprov_cam_ip()).
//
// 최대 while any key is still unmeasured. Four keys at 500/day means a server
// that has answered once would otherwise read "2000/2000" with only the one key
// confirmed: an unused key's remaining is unknown, and the keys can be shared
// with another install. See plantid_total_is_measured().
static void refresh_quota(void) {
    char buf[24];
    int have = plantid_total_remaining(), cap = plantid_total_quota();
    if (have < 0 || cap < 0) {
        snprintf(buf, sizeof(buf), "-");
    } else if (plantid_total_is_measured()) {
        snprintf(buf, sizeof(buf), "%d/%d", have, cap);
    } else {
        snprintf(buf, sizeof(buf), "최대 %d/%d", have, cap);
    }
    lv_label_set_text(lv_obj_get_child(w_quota_chip, 1), buf);
}

// The binomial beside the name, hidden when there is nothing to add.
//
// Suppressed when it IS the name: both identification paths fall back to the
// scientific name when no Korean or common one resolved (plantid.cpp's chain and
// render.py's _species do the same thing), and a card reading
// "Ficus elastica  Ficus elastica" looks like a rendering fault rather than a
// provenance note.
static void set_sci(const char *sci, const char *name) {
    if (sci[0] == '\0' || !strcmp(sci, name)) {
        lv_obj_add_flag(w_sci, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    ui_set_label_text(w_sci, sci);
    lv_obj_clear_flag(w_sci, LV_OBJ_FLAG_HIDDEN);
}

// Species line, from three sources in a fixed order. The device's own PlantNet
// call outranks the server's: the user pressed the button with the plant in front
// of the lens, which is a more specific claim than anything the server inferred
// from an uploaded frame, and it is also the answer the user is waiting on. Only
// with no local result does plantrx_species() fill the line, and only with
// neither does the prompt come back. Letting the two overwrite each other in
// arrival order is how a button press appears to do nothing.
//
// Both sources carry a confidence and a binomial, and each branch draws its own.
// They are the same two measurements - main.py rounds a PlantNet score the way
// plantid.cpp does, against the same API - so the chip and the key mean one thing
// wherever the name came from. Drawing the server's and not the device's would
// make provenance legible as a typographic accident, which is worse than not
// showing it: the reader would learn the layout, not the fact.
static void refresh_identify(void) {
    PlantIdState st = plantid_state();
    uint32_t rx = plantrx_revision();
    // Two inputs now, so both gate the redraw.
    if (st == s_id_last && rx == s_id_rx_rev) return;
    s_id_last = st;
    s_id_rx_rev = rx;

    switch (st) {
        case PLANTID_BUSY:
            lv_label_set_text(w_species, "식별 중…");
            lv_obj_add_flag(w_sci, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(w_conf_chip, LV_OBJ_FLAG_HIDDEN);
            break;
        case PLANTID_OK: {
            const char *kr = plantid_korean();
            const char *cn = plantid_common();
            const char *sci = plantid_species();
            const char *name = kr[0] ? kr : (cn[0] ? cn : sci);
            lv_label_set_text(w_species, name);
            set_sci(sci, name);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", (int)(plantid_score() * 100.0f + 0.5f));
            lv_label_set_text(lv_obj_get_child(w_conf_chip, 0), buf);
            lv_obj_clear_flag(w_conf_chip, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case PLANTID_ERR: {
            char buf[128];
            snprintf(buf, sizeof(buf), "식별 실패: %s", plantid_error());
            lv_label_set_text(w_species, buf);
            lv_obj_add_flag(w_sci, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(w_conf_chip, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        default: {
            // The server's figures, never the device's: this branch is the
            // no-local-result path, so plantid_score() here would be a stale
            // number beside a name the device did not produce. Hidden when the
            // card carries no figure at all, because 0% is a real confidence and
            // "not reported" is not - the same distinction render.py draws by
            // leaving conf_text empty rather than writing "0%".
            const char *sp = plantrx_species();
            lv_label_set_text(w_species, sp[0] ? sp : "버튼을 눌러 식물 식별");
            // Only against a real name. With no species the line is a prompt, and
            // a binomial under an instruction to press a button names nothing.
            set_sci(sp[0] ? plantrx_species_sci() : "", sp);
            const char *cf = plantrx_species_conf();
            if (sp[0] && cf[0]) {
                lv_label_set_text(lv_obj_get_child(w_conf_chip, 0), cf);
                lv_obj_clear_flag(w_conf_chip, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(w_conf_chip, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        }
    }
    refresh_quota();   // a finished call consumed one
}

// The 판단 column's one always-live row, pinned above the card: a judgment turn
// runs on a fixed period, so this is a scheduled event, not a decoration. It sits
// in 판단 and not over the 예약 list because what it counts to is the next judgment
// - the sequel to the card under it - while 예약 holds control actions the model
// asked for, each carrying its own time. Before the first turn there is no
// schedule yet - the module is still waiting for a camera frame and a clock - and
// "대기" is the honest answer, not 00:00.
//
// The caption names who owns that turn. The server's turn supersedes the rule's
// while the uplink is fresh (see aijudge_set_server_turn), and a caption that
// always said 판단 would imply the model is about to speak on a panel whose
// uplink is down and whose local rule is what will actually run.
//
// Three authors, not two, because the server has two of its own: a keyless server
// answers every poll with a well-formed 200 carrying its own threshold rule's
// output, so "the server owns the turn" does not mean a model is going to run.
// 모델 / 서버 / 패널 name whichever one it actually is, and they are three
// two-syllable words on purpose - the badge chip they share sits at the right
// edge of a 222px column with the title already spending most of it.
static void refresh_turn(void) {
    if (w_turn_label == NULL) return;
    ui_set_label_text(w_turn_caption,
                      !aijudge_turn_is_server()  ? "다음 패널 턴"
                      : plantrx_model_ready()    ? "다음 모델 턴"
                                                 : "다음 서버 턴");
    char buf[8];
    if (!aijudge_turn_scheduled()) {
        snprintf(buf, sizeof(buf), "대기");
    } else {
        uint32_t s = aijudge_turn_remaining_ms() / 1000;
        snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
    // Only touch the label when the second actually rolled over: the UI tick is
    // 300ms, so three of every four writes would invalidate for nothing.
    if (strcmp(buf, s_turn_last) == 0) return;
    strncpy(s_turn_last, buf, sizeof(s_turn_last) - 1);
    s_turn_last[sizeof(s_turn_last) - 1] = '\0';
    lv_label_set_text(w_turn_label, buf);
}

// The server's view of the AI-RX mode, beside the switch that owns it. plantrx.h asks
// for a disagreement to be shown rather than silently resolved, so this states
// the server's value as a fact and changes nothing: the switch stays the device's
// and keeps driving what the uplink reports.
//
// Three gates, and every one is load-bearing. plantrx_mode_auto() is a plain bool
// that defaults to true at boot, so a server that has never answered would
// "disagree" with a switch the user turned off - a conflict drawn against a
// default, which is a false alarm and worse than saying nothing. Requiring a link
// past RX_WAITING and an arrived prescription (plantrx_age_s() >= 0) is what makes
// "the server said so" true. RX_ERROR still qualifies: the mode it last sent is
// the mode it believes this device is in, and a poll that failed since does not
// retract it.
//
// The third gate is plantrx_rx_real(), and the two above do not imply it. A device
// the server has never diagnosed still gets a clean 200: _empty_prescription
// (server/app/main.py:88-91) is a valid display carrying rx_id "none" and a mode
// hard-coded to "advisory". So the poll succeeds, the link is RX_OK, an age
// exists, plantrx_mode_auto() reads false against a switch that defaults to true,
// and this chip drew "서버: 자문 전용" against a value the server had never decided
// - precisely the false alarm the first two gates exist to prevent. Having
// answered and having judged are different facts, and only the second is an
// opinion worth contradicting the panel with.
//
// This cannot ride plantrx_revision(). commit() assigns s_mode_auto outside the
// staging_differs() test and the mode is not one of the fields that test compares,
// so a prescription changing only the mode never bumps the revision. Comparing
// the cached draw is what catches it, at two integer tests per tick.
static void refresh_mode_conflict(void) {
    if (w_mode_conflict == NULL) return;
    RxLink lk = plantrx_link();
    bool heard = (lk != RX_OFF && lk != RX_WAITING) && plantrx_age_s() >= 0 &&
                 plantrx_rx_real();
    bool server_auto = plantrx_mode_auto();
    // 0 hides; 1 / 2 carry the server's value, which whenever this is non-zero is
    // by construction the opposite of the switch.
    int8_t want = (!heard || server_auto == g_auto_control) ? 0 : (server_auto ? 1 : 2);
    if (want == s_mode_conflict) return;
    s_mode_conflict = want;

    if (want == 0) {
        lv_obj_add_flag(w_mode_conflict, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // The same two words w_mode_label uses for the device's own state, so the
    // contrast between the two labels is the whole message and neither has to
    // name the other as wrong.
    lv_label_set_text(lv_obj_get_child(w_mode_conflict, 0),
                      want == 1 ? "서버: 자동 실행" : "서버: 판단 전용");
    lv_obj_clear_flag(w_mode_conflict, LV_OBJ_FLAG_HIDDEN);
}

void page_auto_refresh(void) {
    if (s_page == NULL) return;
    refresh_identify();
    refresh_quota();
    refresh_turn();

    update_toggle(s_auto_toggle, g_auto_control, C_GREEN);
    // OFF is not "asleep": the AI keeps judging, it just stops acting - which is
    // what these two words say. They used to read 자문 전용 / 자동 집행, naming the
    // output as advice; the switch does not gate whether the model speaks, only
    // whether the panel carries out what it prescribed.
    lv_label_set_text(w_mode_label, g_auto_control ? "자동 실행" : "판단 전용");
    lv_obj_set_style_text_color(w_mode_label, g_auto_control ? C_GREEN : C_TEXT_SECONDARY, 0);
    refresh_mode_conflict();   // the switch just moved, so the comparison did too
}

static void on_toggle_auto(lv_event_t *e) {
    g_auto_control = !g_auto_control;
    ui_prefs_mark_dirty();
    page_auto_refresh();
    topbar_refresh();   // the title dot mirrors this across every page
}

// The uplink reports the switch to the server on every poll. A function rather
// than the extern, so nothing outside src/ui/ has to include a header that
// exports every widget pointer on the page to read one bool.
bool ui_auto_control(void) { return g_auto_control; }

static void on_identify(lv_event_t *e) {
    plantid_trigger();  // background task grabs the CAM still + queries PlantNet
}

// 지금 진단: the user asking for a diagnosis on the next poll. The server still
// owns the decision and its own floor outranks this, so nothing here claims a
// judgment is imminent - the countdown chip is what answers that question.
static void on_ask_now(lv_event_t *e) {
    plantrx_ask_now();
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

// A chip: compact pill for a short string that needs a colour.
static lv_obj_t *build_chip(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg) {
    lv_obj_t *chip = plain(parent);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chip, bg, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(chip, 8, 0);
    lv_obj_set_style_pad_ver(chip, 2, 0);
    label(chip, text, &font_reg_12, fg);
    return chip;
}

// Wrapping chip shelf. The 판단 card lays up to five reading chips across 222px
// and a 예약 / 조치 row lays one across 206, so the row has to wrap rather than
// clip whatever runs past the edge.
static lv_obj_t *chip_shelf(lv_obj_t *parent) {
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 5, 0);
    lv_obj_set_style_pad_row(row, 4, 0);
    return row;
}

// A wrapped prose line: a reason or a condition, never a reading.
static void build_prose(lv_obj_t *parent, const char *text, lv_color_t color) {
    lv_obj_t *t = label(parent, text, &font_reg_12, color);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
}

// The 판단 column's own geometry, and the reason the thumbnails are the size they
// are. 800px of screen, less 2x16 of page padding, less the two 12px gaps between
// three columns, is 744; a third is 248 a column, and a column card spends 1px of
// border and 12px of padding on each side - leaving 222. The two frames span
// exactly that: AIJUDGE_THUMB_W twice plus this gap. Derived and not written as
// 6, because the thumbnail size is aijudge.h's to set - the 64px pair this
// replaced left 86px of dead column, and a literal gap would have gone on doing
// that in silence.
#define JUDGE_COL_INNER 222
#define JUDGE_FRAME_GAP (JUDGE_COL_INNER - 2 * AIJUDGE_THUMB_W)
static_assert(JUDGE_FRAME_GAP >= 4, "AIJUDGE_THUMB_W leaves no gap inside one column");

// One frozen frame. The canvas points straight at the judgment record's PSRAM
// buffer rather than copying it: LVGL only reads the buffer while drawing, and a
// per-rebuild memcpy of AIJUDGE_THUMB_W x AIJUDGE_THUMB_H RGB565 - 17,496 bytes a
// thumbnail now - would buy nothing. The const cast is the price of
// lv_canvas_set_buffer's void* signature - nothing here draws INTO the canvas, so
// the buffer stays read-only in practice.
//
// The rounded wrapper is what gets clip_corner: setting a radius on the canvas
// itself does not reliably clip image pixels in LVGL 8, so the same
// wrapper-clips-child shape the monitor page uses is reused here.
//
// `why` non-NULL withholds the frame and captions the placeholder instead, for
// the case aijudge.h's honesty note describes: the device froze a thumbnail but
// the verdict was reached without one, and showing it would pin a picture the
// finding never saw. The caption goes inside the tile the frame would otherwise
// have filled, so the fact costs no layout. NULL keeps the silent grey
// placeholder, which is the older and different fact of having no frame to show.
static void build_thumb(lv_obj_t *parent, const uint16_t *px, const char *why) {
    lv_obj_t *wrap = plain(parent);
    lv_obj_set_size(wrap, AIJUDGE_THUMB_W, AIJUDGE_THUMB_H);
    lv_obj_set_style_radius(wrap, 6, 0);
    lv_obj_set_style_clip_corner(wrap, true, 0);
    lv_obj_set_style_bg_color(wrap, C_SKELETON, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_COVER, 0);
    if (why != NULL) {
        // Before the px test and returning: a withheld frame is withheld whether
        // or not the device happens to hold one.
        lv_obj_center(label(wrap, why, &font_reg_12, C_TEXT_SECONDARY));
        return;
    }
    if (px == NULL) return;   // no frame at judgment time: leave the placeholder
    lv_obj_t *cv = lv_canvas_create(wrap);
    lv_canvas_set_buffer(cv, (void *)px, AIJUDGE_THUMB_W, AIJUDGE_THUMB_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(cv, LV_ALIGN_CENTER, 0, 0);
}

// One 예약 / 조치 row: timestamp + tag on top, the finding below. Returns the tile
// so the caller can append what its own tense carries - a measured delta as a
// chip, a triggering condition as prose - instead of this taking a parameter per
// column. The 판단 column stopped coming through here when it became a single
// full-width card with no tile of its own; these two are what is left.
//
// The screen-bg fill is what separates entries: at ~206px inner width a
// multi-line entry needs a boundary, and a gap alone reads as one blob.
static lv_obj_t *build_entry(lv_obj_t *parent, const char *at, const char *tag,
                            lv_color_t tag_bg, lv_color_t tag_fg, const char *head) {
    lv_obj_t *tile = card(parent, C_SCREEN_BG, 8, false);
    lv_obj_set_width(tile, LV_PCT(100));
    lv_obj_set_height(tile, LV_SIZE_CONTENT);
    flex_col(tile, 5, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(tile, 8);

    lv_obj_t *meta = plain(tile);
    lv_obj_set_width(meta, LV_PCT(100));
    lv_obj_set_height(meta, LV_SIZE_CONTENT);
    flex_row(meta, 6, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);
    label(meta, at, &font_reg_12, C_TEXT_SECONDARY);
    build_chip(meta, tag, tag_bg, tag_fg);

    lv_obj_t *h = label(tile, head, &font_bold_14, C_TEXT_DARK);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    return tile;
}

static void build_row_entry(lv_obj_t *parent, const AiEntry &e) {
    lv_obj_t *tile = build_entry(parent, e.at, e.tag, e.tag_bg, e.tag_fg, e.head);
    if (e.tail == NULL || e.tail[0] == '\0') return;
    if (e.tail_is_reading) {
        // A measured before → after delta is sensor data, so it gets a chip like
        // the 판단 evidence does. Green only when it moved toward the target: the
        // server also reports the rows where it did not, and green there would
        // read as a success the plant never got.
        build_chip(chip_shelf(tile), e.tail,
                   e.tail_improved ? C_GREEN_TINT : C_AMBER_TINT,
                   e.tail_improved ? C_GREEN : C_AMBER);
    } else {
        build_prose(tile, e.tail, C_TEXT_SECONDARY);
    }
}

// Why a server-fed column is empty. `nothing` is the caller's own "the server
// answered and had nothing here" line; every other case is a property of the
// uplink and outranks it, because "there is no server" and "there is nothing
// scheduled" are opposite facts that an empty box renders identically.
static void build_rx_empty(lv_obj_t *list, const char *nothing) {
    const char *msg = nothing;
    switch (plantrx_link()) {
        case RX_OFF:     msg = "서버가 설정되지 않았습니다"; break;
        case RX_WAITING: msg = "서버 응답 대기 중"; break;
        case RX_ERROR:   msg = "서버 연결 없음"; break;
        case RX_STALE:   msg = "서버 응답이 오래되었습니다"; break;
        default: {
            // The server knows why it had nothing to say and this file does not,
            // so its own words win whenever it left any.
            const char *notice = plantrx_notice();
            if (notice[0] != '\0') msg = notice;
            break;
        }
    }
    build_prose(list, msg, C_TEXT_SECONDARY);
}

// 예약: future tense. `at` is empty on a condition-triggered row, which
// build_entry draws as a blank slot rather than a fabricated clock time.
static void build_plan_rows(lv_obj_t *list) {
    int n = plantrx_plan_count();
    if (n > PLANTRX_PLAN_MAX) n = PLANTRX_PLAN_MAX;
    int built = 0;
    for (int i = 0; i < n; i++) {
        const RxPlanRow *p = plantrx_plan_at(i);
        if (p == NULL) continue;
        AiEntry e;
        e.at = p->at;
        e.tag = p->tag;
        tone_colors(p->tone, &e.tag_bg, &e.tag_fg);
        e.head = p->head;
        e.tail = p->cond;        // a condition is a reason, never a reading
        e.tail_is_reading = false;
        e.tail_improved = false;
        build_row_entry(list, e);
        built++;
    }
    if (built == 0) build_rx_empty(list, "예약된 작업이 없습니다");
}

// 조치: past tense. delta_is_reading is the server saying whether `delta` holds a
// number or a sentence about the absence of one; conflating them is how
// "실행 기록 없음" ends up looking like a measurement.
static void build_action_rows(lv_obj_t *list) {
    int n = plantrx_action_count();
    if (n > PLANTRX_ACTION_MAX) n = PLANTRX_ACTION_MAX;
    int built = 0;
    for (int i = 0; i < n; i++) {
        const RxActionRow *a = plantrx_action_at(i);
        if (a == NULL) continue;
        AiEntry e;
        e.at = a->at;
        e.tag = a->tag;
        tone_colors(a->tone, &e.tag_bg, &e.tag_fg);
        e.head = a->head;
        e.tail = a->delta;
        e.tail_is_reading = a->delta_is_reading;
        e.tail_improved = a->improved;
        build_row_entry(list, e);
        built++;
    }
    if (built == 0) build_rx_empty(list, "실행 기록이 없습니다");
}

// The card the 판단 column is now. Built straight into the column's list and not
// into build_entry's tile, for two reasons pointing the same way: there is one
// entry, so the tile's screen-bg fill has nothing left to separate it from, and
// the tile's 8px of horizontal padding leaves 206px, sixteen short of the 222 the
// two frames span.
//
// 경고 reuses the all-stop button's red tint/text pair, which already exists in
// both palettes - a fourth accent colour would have to be designed for both.
static void build_judge_card(lv_obj_t *parent, const JudgeRecord *r) {
    const char *tag = "정상";
    lv_color_t bg = C_GREEN_TINT, fg = C_GREEN;
    if (r->level == JUDGE_WARN) {
        tag = "주의"; bg = C_AMBER_TINT; fg = C_AMBER;
    } else if (r->level == JUDGE_ALERT) {
        tag = "경고"; bg = C_ALLSTOP_BG; fg = C_ALLSTOP_TEXT;
    }

    lv_obj_t *meta = plain(parent);
    lv_obj_set_width(meta, LV_PCT(100));
    lv_obj_set_height(meta, LV_SIZE_CONTENT);
    flex_row(meta, 6, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);
    label(meta, r->at, &font_reg_12, C_TEXT_SECONDARY);
    build_chip(meta, tag, bg, fg);

    // Wraps rather than dots: this is the card's title and the card is the whole
    // column, so there is room for the second line a 63-byte head can need.
    lv_obj_t *h = label(parent, r->head, &font_bold_14, C_TEXT_DARK);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);

    // The whole of what the model said, in the primary colour because it is the
    // content now and not a caption under it. Its '\n' separates diagnosis from
    // notes and LVGL draws it as a line break, which is why the server lets that
    // one control character through. A JUDGE_RULE row leaves body empty - a
    // threshold has a finding and no prose - and then nothing is drawn at all: an
    // empty label still spends a row gap and reads as a sentence that failed.
    if (r->body[0] != '\0') {
        lv_obj_t *b = label(parent, r->body, &font_reg_12, C_TEXT_DARK);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    }

    // The readings behind the verdict, across the card. chip_shelf wraps, and at
    // five chips that is load-bearing rather than defensive: two of aijudge.cpp's
    // chip strings (장면최고 28.5 ℃, CO2 420 ppm) already fill most of a 222px
    // line, so five of them need three lines and a non-wrapping row would push the
    // overflow off the column edge.
    if (r->n_evid > 0) {
        lv_obj_t *shelf = chip_shelf(parent);
        for (uint8_t i = 0; i < r->n_evid; i++) {
            const JudgeEvid &e = r->evid[i];
            // `hot` is tested first and wins outright. A hot chip is the reading
            // the verdict turned on, so it keeps the card's own level colour - the
            // coupling schema.py documents on Chip.tone - and only a supporting
            // chip is free to be tinted by the server's read of it.
            //
            // RX_TONE_INFO stops here rather than going through tone_colors(),
            // whose default arm is blue: a reading with nothing to say must stay
            // the neutral grey it has always been, or every chip on the shelf
            // becomes coloured and the accent stops meaning anything.
            lv_color_t cbg = C_PILL_BG, cfg = C_TEXT_SECONDARY;
            if (e.hot) {
                cbg = bg;
                cfg = fg;
            } else if (e.tone != RX_TONE_INFO) {
                tone_colors((RxTone)e.tone, &cbg, &cfg);
            }
            build_chip(shelf, e.text, cbg, cfg);
        }
    }

    // Whether the card's author actually saw the plant, which is the difference
    // between a verdict backed by a picture and one reasoned from numbers alone.
    // Only a model row makes that claim: the threshold rule reads sensors and
    // never looks, so its saw_* are false by construction and gating on them
    // unconditionally would strip the frames off a rule card - whose thumbnails
    // are legitimate, frozen at append time exactly as a model row's are.
    const char *unseen = (r->origin == JUDGE_LLM) ? "판단 제외" : NULL;
    lv_obj_t *frames = plain(parent);
    lv_obj_set_width(frames, LV_PCT(100));
    lv_obj_set_height(frames, LV_SIZE_CONTENT);
    flex_row(frames, JUDGE_FRAME_GAP, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    build_thumb(frames, r->rgb, (unseen && !r->saw_rgb) ? unseen : NULL);
    build_thumb(frames, r->thm, (unseen && !r->saw_thm) ? unseen : NULL);
}

// Rebuilding means destroying and re-creating two canvases, so it happens only
// when the log actually changed - never on the 300ms UI tick.
static void refresh_judge_list(void) {
    if (w_judge_list == NULL) return;
    s_judge_rev = aijudge_revision();
    lv_obj_clean(w_judge_list);   // frees the canvases; aijudge owns the pixels

    // aijudge_at(0) whoever wrote it. Origin decides the badge and whether the
    // saw_* flags mean anything, never whether the card is drawn: a panel showing
    // its own rule's verdict is the panel working, not the panel falling back.
    const JudgeRecord *r = aijudge_at(0);
    if (r == NULL) {
        // Deliberately not build_rx_empty: this card was never the server's to
        // withhold. The local rule authors the first one whether or not an uplink
        // exists, so "서버 연결 없음" here would blame the wrong producer. A server
        // that did answer and explained itself still beats a bare 대기.
        const char *notice = plantrx_notice();
        bool speak = (plantrx_link() == RX_OK) && notice[0] != '\0';
        build_prose(w_judge_list, speak ? notice : "첫 판단 대기 중", C_TEXT_SECONDARY);
        return;
    }
    build_judge_card(w_judge_list, r);
}

// The 예약 / 조치 columns are entirely the server's, so they turn over on
// plantrx_revision() and nothing else - one revision guard per producer, for the
// same reason as above.
static void refresh_rx_lists(void) {
    if (w_plan_list == NULL || w_action_list == NULL) return;
    s_rx_rev = plantrx_revision();

    lv_obj_clean(w_plan_list);
    build_plan_rows(w_plan_list);
    lv_obj_clean(w_action_list);
    build_action_rows(w_action_list);

    // The 판단 column borrows plantrx_notice() while its own ring is empty, so a
    // new notice has to redraw it as well. Reaching across from here is only safe
    // because an empty ring means there are no canvases to destroy.
    if (aijudge_count() == 0) refresh_judge_list();
}

// The provenance badges. 판단 names whoever wrote the one row the column draws;
// with an empty ring there is no author yet, so it names whoever holds the next
// turn instead. 예약 / 조치 are
// server-authored or nothing at all, so theirs also has to admit when the author
// is absent.
//
// JUDGE_LLM does not mean a model wrote the row. aijudge_append_llm() stamps that
// origin on every row that arrived over the wire, and a keyless server ships its
// own threshold rule's output through the same display.judgments - so the badge
// read 모델 over rows no model had seen. plantrx_model_ready() is the server
// saying whether it has one; without it the panel cannot tell the server's two
// authors apart, and with it the three become 모델 / 서버 / 패널.
//
// model_ready is a fact about now and origin is a fact about then, so a server
// that loses its key demotes rows a model really did write. That is the right
// direction to be wrong in: it understates, and understating is what this whole
// badge exists to do.
static void refresh_badges(void) {
    const JudgeRecord *newest = aijudge_at(0);
    bool from_server = (newest != NULL) ? (newest->origin == JUDGE_LLM)
                                        : aijudge_turn_is_server();
    bool model = plantrx_model_ready();
    ui_set_label_text(w_judge_badge, !from_server ? "패널" : model ? "모델" : "서버");

    bool up = plantrx_configured();
    const char *srv_badge = !up ? "미연결" : model ? "모델" : "서버";
    ui_set_label_text(w_plan_badge, srv_badge);
    ui_set_label_text(w_action_badge, srv_badge);

    // A button that cannot do anything is worse than no button: drop the click
    // flag and grey it, rather than accepting a press that goes nowhere.
    if (w_ask_btn != NULL && s_ask_on != (int8_t)up) {
        s_ask_on = (int8_t)up;
        lv_obj_set_style_bg_color(w_ask_btn, up ? C_AMBER_TINT : C_GRAY_TINT, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(w_ask_btn, 0),
                                    up ? C_AMBER : C_GRAY, 0);
        if (up) lv_obj_add_flag(w_ask_btn, LV_OBJ_FLAG_CLICKABLE);
        else    lv_obj_clear_flag(w_ask_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void auto_timer_cb(lv_timer_t *t) {
    if (s_page != NULL && lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) return;
    refresh_identify();
    refresh_turn();
    refresh_mode_conflict();
    if (aijudge_revision() != s_judge_rev) refresh_judge_list();
    if (plantrx_revision() != s_rx_rev) refresh_rx_lists();
    refresh_badges();   // after the rebuilds, so a badge never names the old row
}

// One tense column; returns the scrollable list to fill. `accent` is the only
// thing distinguishing the headers: font_icons carries no clock or check glyph,
// and forcing an unrelated icon (sliders, dashboard) reads worse than a colour
// rail. `badge` is the provenance chip's starting text and `badge_out` receives
// its label, because provenance is live state now - see refresh_badges(). When
// `turn_out` is non-NULL the column gets the scheduled-turn strip pinned above its
// list, and both the caption and the countdown land there. That is the 판단
// column: the countdown runs to the next judgment, which is the sequel to the card
// under it. 예약 rows are control actions the model asked for and carry their own
// times, so a countdown over that list answered a question nobody asked there.
static lv_obj_t *build_column(lv_obj_t *parent, const char *title, lv_color_t accent,
                             const char *badge, lv_obj_t **badge_out,
                             lv_obj_t **turn_out) {
    lv_obj_t *col = card(parent, C_SURFACE, 14, true);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    flex_col(col, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(col, 12);

    lv_obj_t *head = plain(col);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    flex_row(head, 7, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dot = box(head, 8, 8, accent, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    label(head, title, &font_bold_14, C_TEXT_DARK);

    lv_obj_t *spacer = plain(head);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    lv_obj_t *bchip = build_chip(head, badge, C_GRAY_TINT, C_GRAY);
    if (badge_out != NULL) *badge_out = lv_obj_get_child(bchip, 0);

    lv_obj_t *sep = box(col, LV_PCT(100), 1, C_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    if (turn_out != NULL) {
        lv_obj_t *strip = plain(col);
        lv_obj_set_width(strip, LV_PCT(100));
        lv_obj_set_height(strip, LV_SIZE_CONTENT);
        flex_row(strip, 6, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        w_turn_caption = label(strip, "다음 패널 턴", &font_reg_12, C_TEXT_SECONDARY);
        lv_obj_t *sp = plain(strip);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_set_height(sp, 1);
        // Blue and not the column's own amber. Amber in 판단 is the 주의 verdict's
        // tint and the hot chip's, so an amber countdown sitting a few pixels above
        // an amber 주의 tag would read as a second warning. Blue is this panel's
        // scheduled-future colour and is the one thing in this column that is not a
        // verdict.
        lv_obj_t *chip = build_chip(strip, "대기", C_BLUE_TINT, C_BLUE);
        *turn_out = lv_obj_get_child(chip, 0);
    }

    lv_obj_t *list = plain(col);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    flex_col(list, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    // plain() stripped the scrollbar style, so a scrollbar would draw as a
    // zero-width invisible artefact anyway. Say so explicitly.
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

lv_obj_t *page_auto_build(lv_obj_t *parent) {
    s_id_last = (PlantIdState)-1;   // a theme rebuild must re-sync the labels
    s_turn_last[0] = '\0';
    s_ask_on = -1;                  // and re-apply the 지금 진단 button's look
    s_mode_conflict = -1;           // and re-hide the server-mode chip
    lv_obj_t *page = plain(parent);
    s_page = page;
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    flex_col(page, 12, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pad_all(page, 16);

    // -- strip: what plant, how sure, how many calls left, and the mode -------
    lv_obj_t *strip = card(page, C_SURFACE, 14, true);
    lv_obj_set_width(strip, LV_PCT(100));
    lv_obj_set_height(strip, LV_SIZE_CONTENT);
    flex_row(strip, 8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(strip, 12, 0);
    lv_obj_set_style_pad_ver(strip, 10, 0);

    lv_obj_t *btn = box(strip, 32, 32, C_GREEN, 8);
    flex_row(btn, 0, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    clickable(btn);
    lv_obj_add_event_cb(btn, on_identify, LV_EVENT_CLICKED, NULL);
    label(btn, ICON_BOT, &font_bot, C_WHITE);

    w_species = label(strip, "버튼을 눌러 식물 식별", &font_bold_14, C_TEXT_DARK);
    lv_label_set_long_mode(w_species, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(w_species, 1);

    // The binomial, secondary and unstyled: it is a key, not a heading. No grow,
    // so it takes only its content and w_species - which does grow, and dots when
    // it runs out - is what absorbs a strip too narrow for both. Worst measured
    // pair is 산세비에리아 + Chrysanthemum leucanthemum at 78+8+174 px against 274 px
    // of slack in the strip's tightest state, so the ellipsis is a guard that
    // should never fire rather than a routine outcome.
    w_sci = label(strip, "", &font_reg_12, C_TEXT_SECONDARY);
    lv_label_set_long_mode(w_sci, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(w_sci, LV_OBJ_FLAG_HIDDEN);

    w_conf_chip = build_chip(strip, "0%", C_GREEN_TINT, C_GREEN);
    lv_obj_add_flag(w_conf_chip, LV_OBJ_FLAG_HIDDEN);

    // Two labels: the caption is fixed, refresh_quota() rewrites child 1.
    w_quota_chip = build_chip(strip, "PlantNet", C_BLUE_TINT, C_BLUE);
    flex_row(w_quota_chip, 5, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    label(w_quota_chip, "-", &font_reg_12, C_BLUE);

    // 지금 진단 sits here rather than in the 판단 column's header strip: at ~222px
    // of inner column width that strip already spends ~155px on dot + title +
    // badge, and this pill needs ~68px more, which clips the badge off the edge -
    // build_column's header is a non-wrapping flex row, so nothing catches it.
    // Beside 식별 is also where the page's other "ask now" action already lives.
    // The click flag is left off here; refresh_badges() is what grants it, and
    // only when a server exists to ask.
    w_ask_btn = build_chip(strip, "지금 진단", C_AMBER_TINT, C_AMBER);
    lv_obj_set_style_pad_ver(w_ask_btn, 6, 0);   // build_chip's 2px is not a touch target
    lv_obj_set_style_text_font(lv_obj_get_child(w_ask_btn, 0), &font_bold_12, 0);
    lv_obj_add_event_cb(w_ask_btn, on_ask_now, LV_EVENT_CLICKED, NULL);

    lv_obj_t *div = box(strip, 1, 20, C_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    w_mode_label = label(strip, "자동 실행", &font_reg_12, C_GREEN);
    // The server's view, and only while it differs - see refresh_mode_conflict().
    // Between the device's own label and the switch is the only place the two read
    // as one statement. Hidden it costs no width: LVGL 8's flex layout skips
    // LV_OBJ_FLAG_HIDDEN children outright (lv_flex.c), so w_species keeps its
    // full slack until the disagreement actually exists. Amber because this is the
    // panel's 주의 tint - the same one RX_TONE_WARN and the 주의 verdict take - and
    // a disagreement is a thing to look at, not the failure the red would claim.
    w_mode_conflict = build_chip(strip, "서버: 자동 실행", C_AMBER_TINT, C_AMBER);
    lv_obj_add_flag(w_mode_conflict, LV_OBJ_FLAG_HIDDEN);
    build_toggle(strip, s_auto_toggle, on_toggle_auto);

    // -- three columns, one per tense -----------------------------------------
    lv_obj_t *row = plain(page);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    flex_row(row, 12, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Cleared before the column that owns the strip rebuilds them. On a theme
    // rebuild both still name labels the old screen deleted, and refresh_turn()'s
    // only defence is its NULL test - so the reset has to sit ahead of whichever
    // build_column call takes turn_out, which is the 판단 one now.
    w_turn_caption = NULL;
    w_turn_label = NULL;
    w_judge_list = build_column(row, "AI 판단 내역", C_AMBER, "패널", &w_judge_badge, &w_turn_label);

    w_plan_list = build_column(row, "AI 예약 내역", C_BLUE, "미연결", &w_plan_badge, NULL);
    w_action_list = build_column(row, "AI 조치 내역", C_GREEN, "미연결", &w_action_badge, NULL);

    // Both guards start one behind their producer so the first fill is
    // unconditional: a theme rebuild lands here with the live revisions already
    // wherever the last one left them.
    s_judge_rev = aijudge_revision() - 1;
    s_rx_rev = plantrx_revision() - 1;
    refresh_judge_list();
    refresh_rx_lists();

    page_auto_refresh();
    refresh_badges();

    static lv_timer_t *s_timer = NULL;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(auto_timer_cb, 300, NULL);
    }
    return page;
}
