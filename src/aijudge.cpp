// Rule engine and storage behind the judgment log declared in aijudge.h.
//
// A ring, not a growing log: every record pins two PSRAM thumbnails, so an
// unbounded log is an unbounded allocation on a board whose PSRAM already
// carries the LVGL canvases and the JPEG decode buffers. The AIJUDGE_CAP slots
// own their buffers for the lifetime of the firmware and records are written
// over them in place, so after aijudge_init() this file never allocates again —
// a panel that stays up for months can't fragment PSRAM out from under the
// camera, which is the failure a per-append malloc/free cycle would eventually
// produce.
//
// The thumbnails are frozen copies of both feeds taken at append time, through
// the *peek* APIs and never the take APIs: take clears the source's fresh flag
// and the monitor page's canvases redraw off that same flag, so capturing with
// take here would quietly steal frames from the live view.
//
// Judgment runs as a scheduled turn rather than on every tick: JUDGE_TURN_MS
// apart the rule is evaluated once, and the turn reschedules itself whether or
// not it had anything to append. That is the whole reason this is a schedule
// and not a throttle — a throttle can only ever answer "not yet", so the UI had
// nothing to count down to, while a turn is an event at a known time.
//
// Evidence is the whole snapshot, not a selection. A record carries every metric
// this hardware reports that was actually present, with the one the verdict
// turned on placed first and marked hot. It read the other way round until the
// judgment column became one detailed card instead of six one-line rows - see the
// rule block in aijudge.h for why the selection rule died with the list it was
// written for, and the chip builder in aijudge_tick() for the order.
//
// The thresholds below are the entirety of the "AI" that runs on this device,
// and every row they write is origin = JUDGE_RULE. The server's model appends to
// this same ring through aijudge_append_llm() with origin = JUDGE_LLM rather
// than replacing the rule, because the rule is what still answers when the
// uplink is down; it takes over the same turn, since the schedule is the half
// that does not care who authors the row.
//
// The rule's thresholds are the fallback and no longer the first answer. When
// the server has named a band for a metric in control.setpoints the rule scores
// that metric against the server's numbers, because the monitor tiles already
// tint the same reading against them and a log that argues with the tile beside
// it is two vocabularies on one screen. A row the compiled-in numbers decided
// says so in its own head - see the marker at the end of aijudge_tick().
//
// Two producers is why the "did the verdict change" comparison is per-origin: a
// candidate is measured against the newest row of its OWN origin, never against
// the newest row overall. Shared, whichever producer ran last would suppress the
// other, and a log missing the model's row because the rule reached the same
// conclusion is a log that lies about who said what.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "aijudge.h"
#include "camnet.h"
#include "metrics.h"
#include "net.h"
#include "reading.h"
#include "plantrx.h"
#include "sensornode.h"
#include "thermal.h"
#include "hlog.h"

static const size_t THUMB_BYTES =
    (size_t)AIJUDGE_THUMB_W * AIJUDGE_THUMB_H * sizeof(uint16_t);

// How often the rule is evaluated. A metric parked on a threshold flips every
// tick, and at 1Hz that would append a row a second — each one refreezing two
// 108x81 thumbnails and bumping the revision the UI rebuilds its card from, so
// the panel would spend its time redrawing a verdict that never settled. Three
// minutes is far shorter than the pace a greenhouse actually changes at, so
// judging this rarely costs no real event, and unlike the throttle it replaces it
// hands the UI a deadline it can show.
static const uint32_t JUDGE_TURN_MS = 180000;   // 3 min

// Upper bound on how long the first append waits for a picture and a clock.
// The CAM reached the display at ~8.5s and NTP landed alongside it on the boots
// measured here; 20s leaves margin without letting a camera that never shows up
// suppress the log entirely.
static const uint32_t EVIDENCE_GRACE_MS = 20000;

static JudgeRecord s_ring[AIJUDGE_CAP];

// s_newest / s_count / s_rev are what make a filled slot visible, and the UI
// polls them from the LVGL task while loop() writes them here. They are
// volatile so the publish at the end of an append can't be hoisted above the
// record it publishes; the ring entries themselves are only ever written by
// loop(), so the reader sees a whole record or the previous one.
// Starting s_newest one slot behind zero lets the first append land in slot 0
// with no empty-ring special case.
static volatile int s_newest = AIJUDGE_CAP - 1;
static volatile int s_count = 0;
static volatile uint32_t s_rev = 0;
// The turn schedule, volatile for the same reason as the three above: the LVGL
// task reads it to draw the 판단 column's countdown while loop() writes it here,
// and a deadline the compiler parked in a register would freeze that countdown.
// s_turn_scheduled stays false until the first turn actually runs, which is
// what makes "no schedule yet" distinguishable from "due right now".
static volatile uint32_t s_next_turn_ms = 0;
static volatile bool s_turn_scheduled = false;

// ---- the server's turn -----------------------------------------------------
//
// The server's schedule supersedes the rule's while it is fresh, because the
// column is one column and two countdowns would be two answers to the same
// question - and the wrong one would be counting toward an event that is not the
// one about to author the next row. Held as a deadline and not as the countdown
// the client passed in: converting on arrival is what keeps the column moving
// between polls instead of freezing at whatever the last response said.
// Volatile for the same reason as the rule's schedule above - the LVGL task
// reads these to draw the countdown while loop() writes them here.
static volatile uint32_t s_srv_due_ms = 0;
static volatile uint32_t s_srv_period_ms = 0;
static volatile uint32_t s_srv_seen_ms = 0;   // last refresh, for the expiry
static volatile bool s_srv_valid = false;

// How long a server turn survives without a refresh. One rule period: the rule
// is what the panel falls back to, so an override has no business outliving the
// interval the rule would have judged in anyway. It is also generous next to the
// cadence the server asks for - next_poll_s is 60s idle and a couple of seconds
// after an event - so two consecutive dropped polls still leave the column on
// the model's turn instead of bouncing to the rule and back on the next
// response. A cadence left counting down after the uplink died would promise a
// model turn that nothing is going to run, which is the failure this bounds.
static const uint32_t SERVER_TURN_STALE_MS = JUDGE_TURN_MS;

// Whether the override is in force right now. Every getter answers through this
// rather than testing s_srv_valid, so the expiry cannot be honoured by one and
// missed by another: a countdown running to the model's turn under a label that
// says rule is worse than either answer on its own.
static bool server_turn_live(void) {
    if (!s_srv_valid) return false;
    // Signed difference, like every other deadline in this file: unsigned, a
    // refresh taken just before the millis() wrap at 49 days reads as four
    // billion ms stale and the override would be dropped for good.
    return (int32_t)(millis() - s_srv_seen_ms) < (int32_t)SERVER_TURN_STALE_MS;
}

// Slot-owned storage: s_rgb_buf[i] and s_thm_buf[i] belong to ring slot i
// permanently. A record being overwritten reuses its own slot's two buffers,
// which is what keeps the "no allocation after init" property above.
static uint16_t *s_rgb_buf[AIJUDGE_CAP];
static uint16_t *s_thm_buf[AIJUDGE_CAP];

// Nearest-neighbour source maps for the RGB capture, the same idiom the monitor
// page uses. Both ends are fixed — CAMNET_W x CAMNET_H in, 108x81 out — so the
// column and row lookups are identical for every capture and are built once.
// Both are sized straight off the THUMB constants, so widening the thumbnail to
// fill the column widened these with no other change.
// (Thermal needs none of this: thermal_peek_scaled takes the target size and
// interpolates itself.)
static int16_t s_sx_map[AIJUDGE_THUMB_W];
static int16_t s_sy_map[AIJUDGE_THUMB_H];

void aijudge_init(void) {
    for (int i = 0; i < AIJUDGE_CAP; i++) {
        s_rgb_buf[i] = (uint16_t *)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM);
        s_thm_buf[i] = (uint16_t *)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM);
    }

    for (int dx = 0; dx < AIJUDGE_THUMB_W; dx++) {
        s_sx_map[dx] = (int16_t)(dx * CAMNET_W / AIJUDGE_THUMB_W);
    }
    for (int dy = 0; dy < AIJUDGE_THUMB_H; dy++) {
        s_sy_map[dy] = (int16_t)(dy * CAMNET_H / AIJUDGE_THUMB_H);
    }

    // Verdicts still have to work on a board that couldn't spare the ~102.5KB
    // (3 slots x 2 thumbs x 17,496B): a record with NULL rgb/thm is a verdict
    // without pictures, not a verdict that was lost. Say so once at boot, because
    // a card that silently comes up with two empty frames is otherwise a mystery
    // to whoever is looking at the panel.
    for (int i = 0; i < AIJUDGE_CAP; i++) {
        if (s_rgb_buf[i] == NULL || s_thm_buf[i] == NULL) {
            hlogf("[aijudge] PSRAM thumbnail alloc failed at slot %d; "
                          "log runs without thumbnails\n", i);
            break;
        }
    }
}

// ---- the one VPD ------------------------------------------------------------
//
// A faithful port of server/app/derive.py's vpd_kpa(), guards and all, and the
// only copy of this arithmetic on the device. It existed three times in this
// repo with two different coefficients - 0.6108f here, 0.61078 in derive.py, and
// a third on the monitor page that carried neither of Python's guards - so the
// panel could print one VPD in a tile and reason about another in the log.
//
// The coefficient moved to the server's and not the other way round: derive.py's
// is the value already written into every vpd_kpa row in SQLite and scored
// against every band, and it is the less-rounded Tetens constant besides.
//
//     es(T) = 0.61078 * exp(17.27 * T / (T + 237.3))   [kPa, T in degC]
//     VPD   = es(T) * (1 - RH/100)                     [kPa]
//
// Deliberately NOT rounded to 3 dp, which is the one thing derive.py does that
// this does not, and the omission is the point. Mirroring that round() was tried
// first and measured: over the -5..50 °C x 0..100 %RH grid it makes the two
// languages disagree by 0.001 at 14 points, because a float32 result sitting a
// few ulps either side of a .0005 boundary rounds the other way from Python's
// double. Rounding cannot fix that - it only decides where the disagreement
// lands - and it is derive.py's storage precision anyway, chosen for what goes
// into SQLite and not for the physics. Left unrounded, the gap between the two
// is exactly the server's own rounding: |device - server| <= 0.0005 kPa for
// every input, everywhere, which is a bound that can be proved rather than
// sampled. t=25.0, rh=60.0 is 1.267 on both sides.

float aijudge_vpd_kpa(float temp_c, float rh_pct) {
    // derive.py's _finite(): a NaN or an inf poisons every comparison downstream.
    if (!isfinite(temp_c) || !isfinite(rh_pct)) return READING_NONE;
    // ...and this side's own absent-reading sentinel, which Python sees as null.
    if (!reading_present(temp_c) || !reading_present(rh_pct)) return READING_NONE;
    // "Belt and braces against a -999 sentinel arriving as a number rather than
    // the null the firmware is supposed to send: outside this range the Tetens
    // denominator is either meaningless or, below -237.3, a division by zero."
    if (temp_c < -60.0f || temp_c > 100.0f) return READING_NONE;
    // "A capacitive RH sensor reading 101% at saturation is normal and would
    // otherwise come back as a small negative deficit."
    float rh = rh_pct < 0.0f ? 0.0f : (rh_pct > 100.0f ? 100.0f : rh_pct);
    float es = 0.61078f * expf(17.27f * temp_c / (temp_c + 237.3f));
    return es * (1.0f - rh / 100.0f);
}

// The top bar's clock format minus the seconds — a verdict is a minute-scale
// event. Gated on net_time_valid() alone and not on the link state: once NTP
// has set the clock the SoC keeps counting through a WiFi drop, and stamping a
// record "--:--" because the radio blipped would throw away a good timestamp.
static void stamp_now(char *dst, size_t cap) {
    if (!net_time_valid()) {
        snprintf(dst, cap, "--:--");
        return;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(dst, cap, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
}

// Which metric the finding turned on. The rule that fires names it, and that is
// the only thing the rule still chooses about the evidence: the chips are the
// whole snapshot now, so this decides only which one leads and wears `hot`.
enum EvidKind { EV_NORMAL, EV_VPD_HIGH, EV_VPD_LOW, EV_DT, EV_CO2, EV_TEMP };

// The snapshot, in the order a card reads it: the composite first, then the two
// directly measured numbers it is computed from, then CO2, then the scene peak
// last because it is a hint and not a measurement (see the all-caps block in
// aijudge_tick()). The deciding metric is lifted out of this order to the front
// and the rest follow it unchanged, so two cards in a row lay their context out
// identically and only the leading chip moves.
enum MetricChip { MC_VPD, MC_TEMP, MC_HUM, MC_CO2, MC_PEAK, MC_N };

// One chip's format and the value it formats. U+2103 is written as its UTF-8
// bytes in the table in aijudge_tick() because, unlike the Hangul around it, a
// mangled degree glyph is invisible in a diff.
struct ChipSrc { const char *fmt; float v; };

// Which chip a finding leads with, or -1 for an all-clear, which leads with none.
// -1 and not MC_N so the answer can never be used as an index into the table.
// Every hot answer here is a metric its own branch in aijudge_tick() already
// proved present - EV_DT fires only under have_dt and EV_CO2 only under have_co2 -
// so the leading chip can never be the one add_chip() drops for absence.
static int hot_chip_of(EvidKind kind) {
    switch (kind) {
    case EV_VPD_HIGH:
    case EV_VPD_LOW:  return MC_VPD;
    case EV_DT:       return MC_PEAK;
    case EV_CO2:      return MC_CO2;
    case EV_TEMP:     return MC_TEMP;
    case EV_NORMAL:   break;
    }
    return -1;
}

// Append one chip, pre-formatted with its unit because the UI has no business
// knowing which metric wants a decimal and which wants none. A value that never
// arrived is skipped outright instead of written as a placeholder: "--" reads
// like a broken sensor, and the snapshot is a claim about what was measured, so a
// reading that was not measured has no chip at all. n_evid therefore counts what
// was actually written, which is what the card has to size itself from rather
// than from AIJUDGE_EVID_MAX.
static void add_chip(JudgeRecord *rec, bool hot, const char *fmt, float v) {
    if (rec->n_evid >= AIJUDGE_EVID_MAX || !reading_present(v)) return;
    JudgeEvid *e = &rec->evid[rec->n_evid++];
    snprintf(e->text, sizeof(e->text), fmt, v);
    e->hot = hot;
    // A rule row has no server tone to carry, and the neutral is written rather
    // than left alone because slots are reused: an LLM row's amber would survive
    // into the rule row that lands in the same slot three verdicts later.
    e->tone = 0;   // RX_TONE_INFO
}

// Worst metric wins; equal levels keep the incumbent, which is what makes the
// call order in aijudge_tick() (vpd, then dt, then co2) the tie-break. VPD is
// computed from two directly measured values while dt leans on a scene peak
// that may not be a leaf at all, so when both flag the same severity the
// better-grounded finding is the one the row shows. CO2 comes last because
// a CO2 shortfall is the mildest of the three.
static void consider(JudgeLevel cand, const char *cand_head, EvidKind cand_evid,
                     bool cand_local, JudgeLevel *level, const char **head,
                     EvidKind *evid, bool *local) {
    if (cand > *level) {
        *level = cand;
        *head = cand_head;
        // The evidence set travels with the head, so whichever finding the row
        // ends up showing is the one that names the numbers behind it.
        *evid = cand_evid;
        // ...and so does whose numbers they were. Only the winning metric's
        // provenance describes the row that actually gets written; a row headed
        // by a server-banded finding is not the same claim as one headed by a
        // finding a compiled-in default caught.
        *local = cand_local;
    }
}

// Freeze both feeds into this slot's own buffers.
//
// peek and never take: take requires and clears the source's fresh flag, which
// is the same flag the monitor page's canvases consume, so capturing with take
// would starve the live view to fill a thumbnail nobody is looking at yet.
//
// peek succeeds on any frame ever published, including a long-dead one, so
// liveness is checked here too. A thumbnail is a claim about what the system
// was looking at when it reached this verdict; pasting in a ten-minute-old
// picture would make that claim false. NULL is the honest answer and the UI
// already renders it as an empty tile.
//
// The buffer test comes first in each condition, so a slot whose PSRAM alloc
// failed at boot never reaches the peek call with a NULL destination.
static void freeze_thumbs(JudgeRecord *rec, int slot) {
    rec->rgb = NULL;
    rec->thm = NULL;

    if (s_rgb_buf[slot] != NULL && camnet_live() &&
        camnet_peek_scaled(s_rgb_buf[slot], AIJUDGE_THUMB_W, AIJUDGE_THUMB_H,
                           s_sx_map, s_sy_map)) {
        rec->rgb = s_rgb_buf[slot];
    }
    if (s_thm_buf[slot] != NULL && thermal_live() &&
        thermal_peek_scaled(s_thm_buf[slot], AIJUDGE_THUMB_W, AIJUDGE_THUMB_H)) {
        rec->thm = s_thm_buf[slot];
    }
}

// Every turn that runs puts the next one on the clock. The deadline is written
// before the flag because the UI tests the flag first, so it can never pair a
// live schedule with a stale deadline.
static void schedule_next_turn(uint32_t now_ms) {
    s_next_turn_ms = now_ms + JUDGE_TURN_MS;
    s_turn_scheduled = true;
}

// The newest record a given producer wrote, or NULL if it has never written one.
//
// A scan, and deliberately not a cached "newest per origin" pointer: the ring is
// AIJUDGE_CAP slots deep, so this is at most three compares and it runs once per
// append, while a cached pointer goes stale the moment the other producer wraps
// onto the slot it names - it would then answer with a row of the wrong origin,
// which is exactly the confusion the per-origin rule exists to prevent. Do not
// "optimise" this into a cache.
static const JudgeRecord *newest_of_origin(JudgeOrigin origin) {
    int slot = s_newest;
    for (int i = s_count; i > 0; i--) {
        if (s_ring[slot].origin == origin) return &s_ring[slot];
        // Backwards from the newest, the same one-conditional-add wrap
        // aijudge_at() walks.
        if (--slot < 0) slot += AIJUDGE_CAP;
    }
    return NULL;
}

// The transition test. Append only on a change: the column draws one card, and a
// candidate identical to the row it would replace re-authors that card with the
// text already on it - two canvases refrozen and a widget tree rebuilt for no new
// information. Compared on (level, head, body) rather than on the numbers,
// because the numbers move constantly and the finding is what the card is about.
//
// `body` is in the comparison because it is the only place the model's actual
// reasoning lives. A model that reaches an unchanged headline about a changed
// situation and writes new prose for it HAS produced an event, and on (level,
// head) alone that row was dropped and the card went on showing the previous
// paragraph - which is the bug this field exists to fix, not a hypothetical. A
// rule row's body is "" on both sides, so the term is inert on the rule path.
//
// Against this producer's own last row and not the ring's newest: a model row
// never suppresses a rule row and a rule row never suppresses a model row, so
// whichever ran first cannot silence the other.
static bool unchanged_for_origin(JudgeOrigin origin, JudgeLevel level,
                                 const char *head, const char *body) {
    const JudgeRecord *prev = newest_of_origin(origin);
    return prev != NULL && prev->level == level &&
           strcmp(prev->head, head) == 0 && strcmp(prev->body, body) == 0;
}

// ---- whose numbers the rule judges against ----------------------------------
//
// One metric's band and where it came from. The server publishes a band per
// metric in control.setpoints and the monitor tiles already tint readings
// against it, while the rule carried its own numbers for the same metrics - so
// the log could call a reading 과다 while the tile beside it showed that reading
// sitting inside its band. Two vocabularies on one screen is the bug.
//
// Either bound may be absent, as NAN: "hold CO2 above 600" names no ceiling and
// the server says so by omitting hi. Every comparison in band_hit() is therefore
// isfinite()-guarded, because `v > NAN` is false - an unguarded test against an
// absent bound reports the reading as inside a band that does not exist, which
// is the one failure mode here that produces no symptom at all.
struct RuleBand {
    float lo, hi;          // the band; NAN on a side the band does not bound
    float lo_far, hi_far;  // where the finding escalates; NAN when it cannot
    bool  local;           // true = compiled-in fallback, the server named none
};

// The panel's own band for each metric it judges, used only when the server has
// named none. One table, and exported through aijudge.h, because two were the
// bug: this rule filed 증산 과다 경향 above 1.2 kPa while the monitor tile eight
// rows down the same screen showed that reading green until 1.5, and it warned
// above 1.0 degC of scene-peak-minus-air while the tile stayed green to 3.0. One
// panel said 정상 and 주의 about one number at one moment.
//
// Where the two disagreed, the rule's numbers won. The tile's were wider to
// absorb the scene-peak false positives the all-caps block at :481 describes -
// but a tile has no wording to hedge with, so widening the band was the tile
// answering an honesty problem by staying quiet. Amber now means exactly what the
// rule's WARN means, "this reading is outside its band", which is true of a lamp
// in frame as well: the label says 잎-공기 온도차 and never 엽온, so the tile is
// not promoted to a claim about leaves by turning amber.
//
// CO2 has a floor and no ceiling on purpose. A house above 1000 ppm is usually
// one that is dosing deliberately, so a local ceiling would tint an intentional
// operating point amber; the tile used to carry one and it is gone. A server that
// sends a co2_ppm ceiling is a different matter - it asked for it, and the HIT_HI
// branch below honours it.
struct PanelBand {
    const char *metric;
    float lo, hi;   // NAN on a side the panel has no opinion about
};

static const PanelBand PANEL_BANDS[] = {
    { METRIC_VPD,      0.5f,   1.2f },
    { METRIC_AIR_C,   18.0f,  28.0f },
    { METRIC_RH,      40.0f,  70.0f },
    { METRIC_CO2,    400.0f,   NAN  },
    { METRIC_LEAF_DT,   NAN,   1.0f },
};

bool aijudge_panel_band(const char *metric, float *lo, float *hi) {
    if (!metric) return false;
    for (unsigned i = 0; i < sizeof(PANEL_BANDS) / sizeof(PANEL_BANDS[0]); i++) {
        if (strcmp(metric, PANEL_BANDS[i].metric) != 0) continue;
        if (lo) *lo = PANEL_BANDS[i].lo;
        if (hi) *hi = PANEL_BANDS[i].hi;
        return true;
    }
    return false;
}

// The server's band for `metric` when it has named one, else the panel's own from
// the table above - which is labelled as such on the row it produces.
//
// The escalation edges are the one place the two paths have to differ. A
// setpoint is a boundary and not a ladder: lo and hi are all the server sends,
// and it says nothing about how bad "far outside" is. So a server band escalates
// at its own width - as far past the band as the band is wide - which is
// dimensionless and therefore holds for any metric in any units, and which
// against a 0.5-1.2 kPa VPD band lands on 1.9, within 0.1 of the 1.8 this file
// used to hardcode. A one-sided band has no width, so nothing is invented and
// the finding simply stays at WARN.
static RuleBand resolve_band(const char *metric, float def_lo_far, float def_hi_far) {
    RuleBand b;
    // NAN first because plantrx_band leaves both alone when it has no band, and
    // writes NAN into the one an existing band does not bound.
    float lo = NAN, hi = NAN;
    if (!plantrx_band(metric, &lo, &hi)) {
        b.lo = b.hi = NAN;
        aijudge_panel_band(metric, &b.lo, &b.hi);
        b.lo_far = def_lo_far;
        b.hi_far = def_hi_far;
        b.local = true;
        return b;
    }
    b.lo = lo;
    b.hi = hi;
    b.local = false;
    // > 0 and not >= 0 on purpose. NAN fails it, which is how a one-sided band
    // ends up with no escalation at all; and lo >= hi - which scheduler.py
    // already drops server-side - would otherwise put the escalation edge inside
    // the band and turn the first reading outside it straight into an ALERT.
    float span = hi - lo;
    b.lo_far = span > 0.0f ? lo - span : NAN;
    b.hi_far = span > 0.0f ? hi + span : NAN;
    return b;
}

// Where a reading sits against its band. The escalation edge is tested before
// the bound itself so that it wins when the band has one, and an absent bound is
// skipped outright rather than compared against.
enum BandHit { HIT_IN, HIT_LO, HIT_LO_FAR, HIT_HI, HIT_HI_FAR };

static BandHit band_hit(const RuleBand *b, float v) {
    // Strict > and <, matching derive.py's _in_band(): a reading sitting exactly
    // on a bound is holding the band, and the device scoring that same instant
    // as a breach is the disagreement this whole change exists to remove.
    if (isfinite(b->hi) && v > b->hi) {
        return (isfinite(b->hi_far) && v > b->hi_far) ? HIT_HI_FAR : HIT_HI;
    }
    if (isfinite(b->lo) && v < b->lo) {
        return (isfinite(b->lo_far) && v < b->lo_far) ? HIT_LO_FAR : HIT_LO;
    }
    return HIT_IN;
}

void aijudge_tick(void) {
    uint32_t now_ms = millis();
    bool first_turn = !s_turn_scheduled;

    if (first_turn) {
        // The first turn waits for evidence. The verdict itself is decided from
        // sensor values that arrive over ESP-NOW within a second of boot, but the
        // camera comes over WiFi and the clock over NTP - both seconds later.
        // Appending immediately produced a permanent first row stamped "--:--" with
        // an empty RGB tile, and since the verdict rarely changes again that row
        // stayed the newest one for the entire uptime. Wait for whichever arrives
        // first, the evidence or the deadline.
        if (!(camnet_live() && net_time_valid()) && now_ms < EVIDENCE_GRACE_MS) return;
    } else if ((int32_t)(now_ms - s_next_turn_ms) < 0) {
        // Not due yet. Signed difference so the comparison survives the millis()
        // wrap at 49 days: unsigned, a deadline just past the wrap reads as four
        // billion ms overdue and the turn would then fire on every tick.
        return;
    }

    // No node, no verdict. Temperature and humidity are checked separately from
    // sensornode_online() on purpose: a node whose SCD41 channel is dead still
    // reports and still counts as online, and it is still unjudgeable — VPD
    // needs both values and every threshold below is anchored to one of them.
    //
    // A turn that cannot judge is still a turn that happened, so it reschedules
    // and the countdown keeps telling the truth. The very first turn is the
    // exception: starting the clock on a turn with nothing to judge would leave
    // the log empty for a full period after the node finally reports, so an
    // offline node does not get to consume it.
    if (!sensornode_online()) {
        if (!first_turn) schedule_next_turn(now_ms);
        return;
    }

    float t = sensornode_temp();
    float h = sensornode_hum();

    // VPD is how far the air sits below saturation at its own temperature. That
    // gap is what actually drives transpiration — neither temperature nor
    // humidity says anything useful about it alone, which is why the log leads
    // with VPD instead of the two raw readings, and why every threshold below is
    // anchored to it.
    //
    // One sentinel test folds in three refusals: either reading absent, either
    // one non-finite, or a temperature outside the domain Tetens is defined on.
    // That last case used to fall straight through - a sensor reporting -80 °C
    // is a fault and not a cold greenhouse, and the arithmetic would have handed
    // a confident number to the thresholds. A turn that cannot judge is still a
    // turn that happened, so it reschedules, exactly as an absent node does.
    float vpd = aijudge_vpd_kpa(t, h);
    if (!reading_present(vpd)) {
        if (!first_turn) schedule_next_turn(now_ms);
        return;
    }

    // READ THIS BEFORE CHANGING THE dt WORDING. thermal_max() is the hottest
    // pixel in the whole 32x24 scene, NOT leaf temperature: a lamp, a heater or
    // a sunlit patch of wall anywhere in frame inflates it, and the MLX90640 has
    // no idea which pixels are plant. dt is therefore a hint, not a measurement.
    // That is why the heads it produces stay hedged ("의심" / "추세") and why the
    // evidence chip names the number 장면최고 — the scene peak — and never 엽온.
    // Promoting either to a flat claim about leaf temperature is the single
    // easiest mistake to make in this file; it needs real leaf segmentation
    // first, not a wording change.
    float peak = thermal_max();
    // One test, not two: thermal_max() carries the liveness gate itself now and
    // returns the absent sentinel once the stream stops. See the note on the
    // accessor in include/thermal.h for why the gate lives there.
    bool have_dt = reading_present(peak);
    float dt = have_dt ? peak - t : 0.0f;

    float co2 = sensornode_co2();
    bool have_co2 = reading_present(co2);

    // Resolved once per turn and not per comparison: a band arriving between two
    // comparisons would let the escalation test and the bound test disagree
    // about the same metric. Only the escalation edges are arguments now - the
    // bands themselves come from PANEL_BANDS, which the monitor tiles read too, so
    // there is one place on the whole device where a metric threshold is written
    // down. These edges stay here because they are the rule's own concept: a tile
    // has one amber and no second level to escalate to.
    //
    // leaf_air_dt_c has NO panel escalation edge, and that is the point. Its input
    // is the hottest pixel in the whole scene (see the all-caps block above), which
    // a grow lamp, a heater or a power supply in frame dominates - so on a real
    // installation it sits above the 1.0 band most of the time. With a far edge of
    // 2.0 the panel filed 엽온 상승, 기공 폐쇄 의심 as an ALERT on its very first
    // turn, and would have gone on filing it: a permanent top-level alarm over a
    // lamp, which is how a grower learns to ignore alarms. The band still WARNs,
    // because the reading really is outside it and the hedged wording can carry
    // that; what it must not do is make the panel's highest claim on a number the
    // panel itself documents as a hint. A server band is different - its width is a
    // number the server chose, so resolve_band derives an escalation from it and
    // that one is honoured.
    RuleBand vpd_b = resolve_band(METRIC_VPD,     NAN, 1.8f);
    RuleBand dt_b  = resolve_band(METRIC_LEAF_DT, NAN, NAN);
    RuleBand co2_b = resolve_band(METRIC_CO2,     NAN, NAN);
    // Air temperature, which this rule did not look at until now. The gap it
    // closes is hot AND humid: VPD is the deficit between the air and saturation,
    // so a house at 34 degC and 80 %RH sits inside the VPD band and the rule had
    // nothing to say - while the 기온 tile beside the log went amber against this
    // very band. A tile that reports a fault and a log that stays silent about it
    // is the same one-screen-two-answers problem the bands themselves were
    // unified for.
    //
    // Escalation edges, unlike leaf_air_dt_c's, are real: this is a directly
    // measured number, not a scene peak. 10 and 35 degC are where greenhouse crops
    // stop merely being uncomfortable - chilling injury below, pollen and
    // photosynthesis failure above - and both sit outside the 18-28 band by enough
    // that a normal night or a sunny afternoon does not reach them.
    RuleBand air_b = resolve_band(METRIC_AIR_C,   10.0f, 35.0f);

    JudgeLevel level = JUDGE_OK;
    const char *head = "생육 조건 양호";
    EvidKind kind = EV_NORMAL;
    // An all-clear is a claim about every band this turn actually scored, so it
    // is the server's claim only when every one of those bands was the server's.
    // Metrics whose reading never arrived are left out: a band nothing was
    // scored against did not contribute to the verdict either way.
    bool local = vpd_b.local || air_b.local ||
                 (have_dt && dt_b.local) || (have_co2 && co2_b.local);

    // Both ends of the VPD range are findings: too high is the plant losing
    // water faster than the roots replace it, too low is stalled transpiration,
    // which stops calcium moving and invites mould.
    switch (band_hit(&vpd_b, vpd)) {
    case HIT_HI_FAR:
        consider(JUDGE_ALERT, "수분 스트레스 감지", EV_VPD_HIGH, vpd_b.local,
                 &level, &head, &kind, &local);
        break;
    case HIT_HI:
        consider(JUDGE_WARN, "증산 과다 경향", EV_VPD_HIGH, vpd_b.local,
                 &level, &head, &kind, &local);
        break;
    case HIT_LO:
    case HIT_LO_FAR:
        // One level below the band and not two, exactly as the compiled-in rule
        // had it. A server band does hand this side an escalation edge, but
        // "과습 경향" was never an ALERT here, and promoting it on the day a band
        // arrives would be this change inventing a severity instead of moving a
        // threshold.
        consider(JUDGE_WARN, "증산 정체, 과습 경향", EV_VPD_LOW, vpd_b.local,
                 &level, &head, &kind, &local);
        break;
    case HIT_IN:
        break;
    }

    // Air temperature, both ends. Ordered after VPD deliberately: when both fire,
    // consider() keeps whichever came first at equal severity, and VPD is the
    // better-supported claim - it is computed from two directly measured numbers
    // and names what the plant is actually doing. Temperature wins only by being
    // worse, which is exactly when it should.
    switch (band_hit(&air_b, t)) {
    case HIT_HI_FAR:
        consider(JUDGE_ALERT, "고온 스트레스", EV_TEMP, air_b.local,
                 &level, &head, &kind, &local);
        break;
    case HIT_HI:
        consider(JUDGE_WARN, "기온 상승", EV_TEMP, air_b.local,
                 &level, &head, &kind, &local);
        break;
    case HIT_LO_FAR:
        consider(JUDGE_ALERT, "저온 장해 위험", EV_TEMP, air_b.local,
                 &level, &head, &kind, &local);
        break;
    case HIT_LO:
        consider(JUDGE_WARN, "기온 저하", EV_TEMP, air_b.local,
                 &level, &head, &kind, &local);
        break;
    case HIT_IN:
        break;
    }

    if (have_dt) {
        // The high side only. A low bound on leaf_air_dt_c would be a claim that
        // the canopy is running COOLER than the air, and thermal_max() is the
        // hottest pixel in the scene - it cannot support that claim in the cool
        // direction at all. See the all-caps block above: honouring that bound
        // needs leaf segmentation, not a branch.
        switch (band_hit(&dt_b, dt)) {
        case HIT_HI_FAR:
            // Reachable only from a server band, exactly like CO2's ceiling below:
            // the panel's own dt band has no far edge, because a scene peak cannot
            // support the panel's highest claim. A server that sends a band chose
            // its width, and resolve_band escalates at that width - so this is the
            // server insisting, not the panel guessing.
            consider(JUDGE_ALERT, "엽온 상승, 기공 폐쇄 의심", EV_DT, dt_b.local,
                     &level, &head, &kind, &local);
            break;
        case HIT_HI:
            consider(JUDGE_WARN, "엽온 상승 추세", EV_DT, dt_b.local,
                     &level, &head, &kind, &local);
            break;
        default:
            break;
        }
    }

    if (have_co2) {
        switch (band_hit(&co2_b, co2)) {
        case HIT_LO:
        case HIT_LO_FAR:
            // One level, for the same reason low VPD gets one.
            consider(JUDGE_WARN, "CO2 부족", EV_CO2, co2_b.local,
                     &level, &head, &kind, &local);
            break;
        case HIT_HI:
        case HIT_HI_FAR:
            // Reachable only from a server band - the fallback names no ceiling,
            // so its hi is NAN and band_hit skips the test entirely. Acted on
            // rather than dropped, because a band the panel reads and then
            // ignores on one side is a band the panel is not actually holding.
            consider(JUDGE_WARN, "CO2 과다", EV_CO2, co2_b.local,
                     &level, &head, &kind, &local);
            break;
        case HIT_IN:
            break;
        }
    }

    // The turn has happened, so the next one goes on the clock here — before the
    // transition test below, because the countdown runs to the next evaluation
    // and not to the next row. A turn that finds nothing new is still a turn.
    schedule_next_turn(now_ms);

    // The head is composed here, before the transition test and not after it,
    // because the test compares head strings and the stored one carries the
    // provenance marker: comparing the bare finding against a marked string
    // never matches and every turn would append.
    //
    // "(자체 기준)" is the row naming whose numbers it judged against, and its
    // absence means the server's. The panel had been printing both identically
    // while the monitor tiles beside it scored the same readings against the
    // server's bands. Feeding the transition test is deliberate too: the turn
    // where a band first arrives changes the kind of claim being made, and that
    // is an event the log should carry a row for.
    char head_buf[sizeof(s_ring[0].head)];
    snprintf(head_buf, sizeof(head_buf), local ? "%s (자체 기준)" : "%s", head);

    // Append only on a transition, measured against the rule's own last row -
    // see unchanged_for_origin(). "" is this row's body: a threshold writes no
    // prose, so the body term compares empty against empty and the test on this
    // path is the (level, head) one it has always been.
    if (unchanged_for_origin(JUDGE_RULE, level, head_buf, "")) return;

    int slot = (s_newest + 1) % AIJUDGE_CAP;
    JudgeRecord *rec = &s_ring[slot];

    stamp_now(rec->at, sizeof(rec->at));
    rec->level = level;
    rec->origin = JUDGE_RULE;
    snprintf(rec->head, sizeof(rec->head), "%s", head_buf);
    // A threshold has a finding and no prose to write, so the body stays empty -
    // and it is cleared rather than left alone because the ring reuses slots:
    // inheriting the previous occupant's paragraph would print the model's
    // reasoning under the rule's headline. A NUL and not a memset of 1024 bytes,
    // since every reader of body stops at the terminator.
    rec->body[0] = '\0';
    // A threshold rule reads sensors and never looks at a picture, so it makes
    // no claim to have seen the plant. Written, not left alone, for the reason
    // add_chip() writes its tone: this slot may hold a model row's flags.
    rec->saw_rgb = false;
    rec->saw_thm = false;

    // The evidence: the whole snapshot this verdict was reached in, with the
    // metric the rule actually fired on first and marked hot. This used to be a
    // selection - only the metrics bearing on the finding, because a reader of a
    // six-row list cannot tell which number caused which verdict - and that rule
    // died with the list; see the block in aijudge.h. A card with room for five
    // readings that shows two is withholding the state of the house.
    //
    // Absent readings drop out inside add_chip(), so n_evid is however many of the
    // five actually arrived: five with CO2 and a live thermal stream, three with
    // neither. lux and soil are not in the table at all - the BH1750 and the soil
    // probe are absent on this hardware and report the sentinel on every packet,
    // so there is no reading to lay out and a chip for them would be a permanent
    // blank.
    const ChipSrc chip[MC_N] = {
        { "VPD %.1f kPa",               vpd  },
        { "기온 %.1f \xE2\x84\x83",     t    },
        { "습도 %.0f %%RH",             h    },
        { "CO2 %.0f ppm",               co2  },
        // 장면최고 and never 엽온 — see the all-caps block above.
        { "장면최고 %.1f \xE2\x84\x83", peak },
    };
    int hot = hot_chip_of(kind);
    rec->n_evid = 0;
    if (hot >= 0) add_chip(rec, true, chip[hot].fmt, chip[hot].v);
    for (int m = 0; m < MC_N; m++) {
        if (m == hot) continue;
        add_chip(rec, false, chip[m].fmt, chip[m].v);
    }

    freeze_thumbs(rec, slot);

    // Publish last, once the record is whole: s_newest is what exposes the slot
    // to aijudge_at(), and s_rev is what tells the UI to rebuild the card.
    s_newest = slot;
    if (s_count < AIJUDGE_CAP) s_count++;
    s_rev++;

    // One line per row that actually reaches the ring, which is one per transition
    // and not one per turn - an unchanged verdict returned above without reaching
    // here. On a settled greenhouse that is a handful of lines a day. Both origins
    // come through here, which is what `by` distinguishes.
    //
    // This is the only window onto the half of the product that works with no
    // server at all, which is what a default install IS: no API key means
    // scheduler.decide() never calls a model, so every judgment on the wall came
    // from this function. plantrx.cpp's tick reports the uplink and says nothing
    // about the rule, so "the panel is standing on its own" and "the panel has
    // nothing to say" were indistinguishable from the serial line. `by` names
    // whose numbers decided it, which is the same question the tile titles answer
    // in blue and grey.
    //
    // `thumb` uses plantrx.cpp's two-character alphabet and answers a question the
    // uplink's `saw=` cannot: which of the panel's OWN two cameras had a live frame
    // to freeze into this row. saw_rgb / saw_thm say whether the MODEL was shown a
    // photo, so a row judged from an uploaded frame still draws two placeholders
    // here if the sources went dark in the seconds between.
    static const char *const LEVEL[] = { "OK", "WARN", "ALERT" };
    hlogf("[aijudge] row %s at=%s by=%s thumb=%c%c head=%s rev=%lu\n",
                  LEVEL[(int)level], rec->at, local ? "panel" : "server",
                  rec->rgb ? 'r' : '-', rec->thm ? 't' : '-',
                  rec->head, (unsigned long)s_rev);
}

// ---- the model's rows ------------------------------------------------------
//
// LOCKING: the caller holds lvgl_port_lock(-1) across this call and releases it
// immediately after, exactly as main.cpp already wraps aijudge_tick(). This
// writes ring records the LVGL thread reads, and thumbnail buffers its canvases
// point straight at, so an unlocked append can be halfway through a head string
// while the UI is drawing it. The lock belongs around the ingest alone and never
// around the HTTP round trip that produced the row: a socket wait held under the
// LVGL lock stalls the display for as long as the server takes.
bool aijudge_append_llm(const JudgeRecord *row) {
    if (row == NULL) return false;

    // The tick's rule, applied to the newest LLM row: a model that concludes the
    // same thing twice has not produced an event. `body` is one of the compared
    // fields, so new prose under an unchanged headline still appends - it has to,
    // or the card keeps showing the previous turn's paragraph forever.
    if (unchanged_for_origin(JUDGE_LLM, row->level, row->head, row->body)) {
        return false;
    }

    int slot = (s_newest + 1) % AIJUDGE_CAP;
    JudgeRecord *rec = &s_ring[slot];

    // Every string is copied into this slot's own fixed buffer. The argument
    // points into the client's parse buffer, which the next poll overwrites, so
    // keeping the pointers would leave the UI drawing whatever the following
    // response happened to contain. snprintf and not strcpy even though the
    // server clipped every field to the budget its buffer was sized from - head
    // to JUDGE_HEAD_BYTES, body to JUDGE_BODY_BYTES, chips to their own cap: a
    // device that trusts that clip is one server bug away from a corrupt ring,
    // and body is now the largest field in the record by a factor of sixteen.
    snprintf(rec->at, sizeof(rec->at), "%s", row->at);
    rec->level = row->level;
    // Ours to set, not the caller's - the header hands this file the origin, the
    // slot and the thumbnails, and ignores whatever the argument said of them.
    rec->origin = JUDGE_LLM;
    snprintf(rec->head, sizeof(rec->head), "%s", row->head);
    // The model's prose in full, which is most of what the card is made of.
    // Newlines travel through verbatim: LVGL draws them as line breaks and they
    // are the only control character the server lets into this field.
    snprintf(rec->body, sizeof(rec->body), "%s", row->body);

    // Clamped rather than trusted. n_evid arrives from a parse of a server
    // response, and a count past the end of the array is the one way an
    // otherwise well-formed row could walk off the record.
    uint8_t n = row->n_evid;
    if (n > AIJUDGE_EVID_MAX) n = AIJUDGE_EVID_MAX;
    rec->n_evid = n;
    for (uint8_t i = 0; i < n; i++) {
        snprintf(rec->evid[i].text, sizeof(rec->evid[i].text), "%s",
                 row->evid[i].text);
        rec->evid[i].hot = row->evid[i].hot;
        rec->evid[i].tone = row->evid[i].tone;
    }

    // Not ours to decide, unlike origin above: only the server knows what it was
    // holding when it reasoned, and this is that answer travelling through.
    rec->saw_rgb = row->saw_rgb;
    rec->saw_thm = row->saw_thm;

    // Frozen here from the live feeds, exactly as a rule row's are: the header
    // promises every record owns two frames captured at append time, and a model
    // row pointing at a live feed would be the one lie this whole file is built
    // to prevent. They are honest at this instant because they are the frames the
    // device uploaded for this same prescription and still the newest the feeds
    // hold - which is why the client ingests a prescription when it arrives and
    // never replays an old one. No frame, no thumbnail: rgb/thm stay NULL and the
    // UI draws an empty tile, the same as for a rule row. That is now one of two
    // reasons a tile comes up empty; saw_rgb / saw_thm above are the other, and
    // the page labels them apart because "no camera" and "the model did not look"
    // are different facts about the verdict.
    freeze_thumbs(rec, slot);

    // Publish last, once the record is whole, and touch no schedule on the way
    // out: the server owns its cadence through aijudge_set_server_turn(), and
    // the rule's countdown has to keep running so it is still there when the
    // uplink is not.
    s_newest = slot;
    if (s_count < AIJUDGE_CAP) s_count++;
    s_rev++;
    return true;
}

int aijudge_count(void) { return s_count; }

const JudgeRecord *aijudge_at(int i) {
    if (i < 0 || i >= s_count) return NULL;
    // Walk backwards from the newest, so 0 is the most recent verdict — which is
    // the only one the panel draws. One conditional add is enough to wrap:
    // both s_newest and i are inside [0, AIJUDGE_CAP).
    int slot = s_newest - i;
    if (slot < 0) slot += AIJUDGE_CAP;
    return &s_ring[slot];
}

uint32_t aijudge_revision(void) { return s_rev; }

uint32_t aijudge_turn_period_ms(void) {
    if (server_turn_live()) return s_srv_period_ms;
    return JUDGE_TURN_MS;
}

// A live server turn is a schedule in its own right, even before the rule has
// run its first one: the column has a real event to count down to, which is the
// entire question this answers.
bool aijudge_turn_scheduled(void) {
    return server_turn_live() || s_turn_scheduled;
}

// Signed remainder, clamped at zero. The tick that runs a turn can lag its own
// deadline by a frame or two, and an unsigned difference would render that
// overshoot as 49 days instead of 00:00. Zero with no schedule at all is the
// header's contract: none exists yet, and the UI draws 대기 for it.
uint32_t aijudge_turn_remaining_ms(void) {
    bool srv = server_turn_live();
    if (!srv && !s_turn_scheduled) return 0;
    // Whichever schedule owns the column owns its deadline too; crossing them
    // would count the rule's remainder under the model's label.
    uint32_t due = srv ? s_srv_due_ms : s_next_turn_ms;
    int32_t left = (int32_t)(due - millis());
    return left > 0 ? (uint32_t)left : 0;
}

bool aijudge_turn_is_server(void) { return server_turn_live(); }

void aijudge_set_server_turn(uint32_t remaining_ms, uint32_t period_ms) {
    // A zero period is not a schedule - the same rule the server's own renderer
    // applies before it will set turn.scheduled - so it retires the override
    // instead of installing a cadence the UI would have to make sense of.
    if (period_ms == 0) {
        aijudge_clear_server_turn();
        return;
    }
    uint32_t now_ms = millis();
    // Converted to a local deadline on arrival, which is the whole reason the
    // argument is a remaining and not a timestamp: the client derives it from the
    // response's own next_ts - issued_ts, both the server's numbers, so the
    // countdown works on a device whose NTP never landed.
    s_srv_due_ms = now_ms + remaining_ms;
    s_srv_period_ms = period_ms;
    s_srv_seen_ms = now_ms;
    s_srv_valid = true;   // published last, like the rule's own schedule flag
}

// The client calls this when the link drops, which retires the override at once
// instead of leaving the column on the model for another SERVER_TURN_STALE_MS.
// The flag alone: server_turn_live() is the only reader of the rest, so nothing
// else can be observed once it reads false.
void aijudge_clear_server_turn(void) {
    s_srv_valid = false;
}
