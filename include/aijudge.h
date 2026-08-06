// The judgment log: what the system concluded, when, on what evidence, and the
// frames it was looking at. Three deep, and the panel draws the newest record
// alone as one detailed card - the depth is there for the transition test and not
// for the screen. See AIJUDGE_CAP.
//
// Append-only ring. Each record owns two frozen thumbnails (RGB + thermal)
// captured at append time, never a live feed: a diagnosis and a live picture
// contradict each other the moment the plant moves, and the whole point of the
// thumbnail is to show what the verdict was actually made from.
//
// The verdict text comes from a threshold rule over live sensor values. There is
// no LLM in this firmware. `origin` records which producer wrote a row, so the
// card can name whose judgment it is showing instead of letting one producer's
// wording pass for the other's.
//
// Thumbnails are captured through the *peek* frame APIs, never the take APIs:
// take requires and clears the source's `fresh` flag, so using it here would
// steal frames from the monitor page's canvases.
//
// EVIDENCE IS THE WHOLE SNAPSHOT, and this rule used to say the exact opposite.
// A record carried only the metrics its own finding turned on, because a reader
// scanning a LIST of six one-line rows cannot tell which number caused which
// verdict, and the row that said "CO2 부족" gained nothing from a humidity
// reading. That premise is gone: the column draws one detailed card for the
// newest judgment alone, so there is no neighbouring row for a chip to be
// confused with, and withholding 습도 from a CO2 finding now hides the state the
// verdict was reached in rather than protecting a scan. The full snapshot is the
// point. `hot` still marks the one deciding metric and it is still exactly one
// chip, placed first, so nothing about "which number caused this" was traded
// away by showing the rest.
//
// JUDGMENT IS A SCHEDULED TURN. Evaluation runs on a fixed period rather than
// continuously, so "다음 판단 턴" is a real countdown to a real event that the UI
// can show. It is also the slot an LLM turn will occupy when one exists: the
// schedule does not change, only who authors the row.
#pragma once
#include <stdint.h>

// 4:3, matching both sources (camnet 320x240, thermal 32x24) - but the number is
// set by the column, not by the sources. 108 + a 6 px gap + 108 = 222, the
// measured inner width of one panel column, so the RGB and thermal frames sit
// side by side and fill it edge to edge. The buffer size IS the layout decision
// here: an lv_canvas has to be created at exactly its on-screen size or LVGL 8
// takes lv_img_set_zoom's per-pixel transform path, measured at ~87ms a refresh.
#define AIJUDGE_THUMB_W 108
#define AIJUDGE_THUMB_H 81

// Ring capacity. 108x81 RGB565 is 17,496B a thumbnail and every slot owns two, so
// three slots hold 104,976B (~102.5KB) of PSRAM for the lifetime of the firmware.
//
// Three and not six because the ring stopped being a displayed log: the UI draws
// aijudge_at(0) and nothing else. What still needs depth is the per-origin
// transition test in aijudge.cpp's unchanged_for_origin(), which measures a
// candidate against the newest row of its OWN origin and so needs one surviving
// row per producer - and there are two producers. Three keeps both with a slot
// spare; six would double the PSRAM above for records nothing draws.
//
// Evicting a producer's last row costs one redundant repaint and never a wrong
// card: with no row of that origin left the comparison fails open, so the
// candidate appends and the card is re-authored with the same content.
#define AIJUDGE_CAP 3

// Five: CO2, 기온, 습도, VPD and the thermal scene peak - every metric this
// hardware actually reports, since the BH1750 and the soil probe are absent and
// send the -1000 sentinel on every packet. It is also what 222 px of column fits
// now that one judgment owns the whole width instead of six rows sharing it.
#define AIJUDGE_EVID_MAX 5

enum JudgeLevel { JUDGE_OK, JUDGE_WARN, JUDGE_ALERT };
enum JudgeOrigin { JUDGE_RULE, JUDGE_LLM };

// One sensor reading rendered as a chip, pre-formatted with its unit because the
// UI has no business knowing which metric wants one decimal and which wants
// none. `hot` is the metric the verdict turned on; the UI accents exactly one.
//
// `tone` is the server's read of the reading itself - inside its band, outside
// it, or nothing to say - and it is an RxTone (include/plantrx.h) stored as a
// byte rather than as the enum. Narrowed on purpose and not silently: the full
// enum is int-aligned, which would pad this struct from 30 bytes to 36 and cost
// 6 bytes x AIJUDGE_EVID_MAX x AIJUDGE_CAP in each of the two records arrays,
// and it would also drag the uplink's header into this one for a three-value
// field. 0 is RX_TONE_INFO, which is what a rule row's chips and a server that
// omits the key both leave here; src/plantrx.cpp static_asserts that pairing so
// the two headers cannot drift apart in silence.
struct JudgeEvid {
    char text[28];
    bool hot;
    uint8_t tone;
};

// `body` is the model's prose in full: `diagnosis` first, then `notes` after a
// single '\n' when there are notes. '\n' is the only control character it can
// hold - everything else went through the server's sanitiser on the way out - and
// LVGL draws it as a line break, which is the entire reason it is allowed to
// survive. 1024 is the wire's JUDGE_BODY_BYTES = 1023 plus the NUL. A JUDGE_RULE
// row leaves it empty, because a threshold has a finding and no prose to write:
// the card then shows that row's head and chips and nothing else. `head` is
// untouched and is still the headline; the body is the half the panel used to
// throw away before it ever reached a widget.
//
// `saw_rgb` / `saw_thm` are has_rgb / has_thermal off the wire: whether the
// producer of this row actually held those frames when it decided. They are
// about the verdict's evidence and not about `rgb` / `thm` below, which are the
// device's own frozen thumbnails - see the honesty note on aijudge_append_llm.
// Meaningful only on a JUDGE_LLM row: a threshold rule reads sensors and never
// looks at a picture, so both stay false on a JUDGE_RULE row and the UI reads
// them only after testing `origin`.
struct JudgeRecord {
    char at[8];            // "14:20", or "--:--" when NTP has no wall clock yet
    JudgeLevel level;
    JudgeOrigin origin;
    char head[64];         // the finding, e.g. "엽온 상승, 기공 폐쇄 의심"
    char body[1024];       // the prose in full, "" on a JUDGE_RULE row
    JudgeEvid evid[AIJUDGE_EVID_MAX];
    uint8_t n_evid;        // 0..AIJUDGE_EVID_MAX
    bool saw_rgb;          // the row's author held an RGB frame
    bool saw_thm;          // ... and a thermal one
    const uint16_t *rgb;   // AIJUDGE_THUMB_W x AIJUDGE_THUMB_H RGB565, or NULL
    const uint16_t *thm;   // same, or NULL when no thermal frame existed
};

// Allocates the ring's thumbnail buffers in PSRAM. Call once at boot, after
// thermal_init() / camnet_init().
void aijudge_init(void);

// Runs a *rule* judgment turn when one is due, appending a record if the verdict
// changed. Call freely from loop(); it self-schedules. The model's rows arrive
// through aijudge_append_llm instead, and the two producers do not suppress each
// other: see the per-origin transition rule below.
void aijudge_tick(void);

// ---- the model's rows ------------------------------------------------------
//
// Append a server-authored row. The caller fills what it parsed - at, level,
// head, evid, n_evid, saw_rgb, saw_thm - and this file supplies what only it
// can: origin, the slot, and the thumbnails, which are frozen from the live
// feeds here exactly as a rule row's are. `origin`, `rgb` and `thm` in the
// argument are ignored.
//
// The thumbnails are honest because of when this is called: the frames the
// server reasoned from are the ones the device uploaded for that same
// prescription, and they are still the newest frames the feeds hold. A row
// ingested minutes late would pin a picture the verdict never saw, so the client
// must ingest a prescription when it arrives, not replay an old one.
//
// That timing argument is now checked rather than assumed. It fails whenever the
// server never held a frame at all - no upload had landed yet, or the CAM was
// down - and the device froze one anyway, which pins a picture beside a verdict
// reached without it. saw_rgb / saw_thm are the server answering that question
// directly, and the UI withholds a thumbnail the verdict did not see.
//
// Returns true when a row was appended. False means the finding is unchanged
// from the newest LLM row - the same transition rule the rule producer follows,
// because the log is a list of changes and a model that concludes the same thing
// twice has not produced an event. The comparison is per-origin: a model row
// never suppresses a rule row and a rule row never suppresses a model row, so
// whichever ran first cannot silence the other.
bool aijudge_append_llm(const JudgeRecord *row);

int aijudge_count(void);                    // 0..AIJUDGE_CAP
const JudgeRecord *aijudge_at(int i);       // 0 = newest; NULL if out of range

// Bumped on every append. The UI rebuilds the card only when this changes -
// rebuilding means re-creating two 108x81 canvases, which is far too costly to
// do on a UI tick.
uint32_t aijudge_revision(void);

// The turn schedule, for the countdown in the 판단 column. It belongs there and
// not beside the 예약 rows because the event it counts to is the one that
// authors the next judgment: a reader looking at the newest verdict is the reader
// asking when the next one lands, and the 예약 column is about scheduled control
// actions, which this is not. `remaining` is 0 while the first turn is still
// waiting on evidence (no schedule exists yet), which the UI renders as 대기
// rather than 00:00. These three answer for whichever schedule currently owns the
// countdown - see aijudge_set_server_turn.
uint32_t aijudge_turn_period_ms(void);
uint32_t aijudge_turn_remaining_ms(void);
bool aijudge_turn_scheduled(void);

// ---- whose turn the countdown belongs to ------------------------------------
//
// The server's judgment turn supersedes the local rule turn while it is fresh.
// The log is one log and the column is one column, so two countdowns would be
// two answers to the same question - and the wrong one would be counting down to
// an event that is not the one that will actually author the next row.
//
// `remaining_ms` is converted to a local deadline on arrival, which is what keeps
// the countdown moving between polls instead of freezing at whatever the last
// response happened to say. The client derives it from the response's own
// next_ts - issued_ts, so it needs no wall clock: both are the server's numbers
// and their difference is a duration, valid on a device whose NTP never landed.
//
// Freshness is not optional. The rule is what still answers when the uplink is
// down, so a server turn that stops being refreshed expires after one rule
// period and the column falls back to the rule's own schedule rather than
// counting toward an event nobody is going to run. The client also clears it
// explicitly when the link drops, which is faster than waiting for the expiry.
void aijudge_set_server_turn(uint32_t remaining_ms, uint32_t period_ms);
void aijudge_clear_server_turn(void);

// Which schedule the three getters above are reporting, so the UI can label the
// countdown honestly instead of implying the model is about to speak when the
// uplink is down and the rule is what will actually run.
bool aijudge_turn_is_server(void);

// ---- the one VPD ------------------------------------------------------------
//
// Vapour pressure deficit of the air in kPa: how far the air sits below
// saturation at its own temperature. That gap is what drives transpiration, and
// neither temperature nor humidity says anything useful about it alone - which
// is why both the rule engine and the monitor tile want it, and why it lives in
// a header rather than as a static in whichever file needed it first.
//
// One implementation, because there were three. Same coefficient, same guards
// and the same arithmetic as server/app/derive.py's vpd_kpa(), so the panel and
// the server cannot print two different VPDs for one instant: t=25.0 °C,
// rh=60.0 %RH is 1.267 kPa in both languages. The single difference is that the
// server rounds its answer to 3 dp on the way into SQLite and this does not, so
// |device - server| <= 0.0005 kPa for every input - see the note in aijudge.cpp
// for why mirroring that round() here made the agreement worse rather than
// exact.
//
// Returns below -999 - the absent-value sentinel this codebase tests for with
// `> -999.0f`, the same as thermal_max() and the sensornode getters - when
// either input is absent or non-finite, or when temperature is outside
// [-60, 100] °C, where the Tetens denominator stops meaning anything and below
// -237.3 divides by zero. The sentinel is not 0 and not -1 for a measured
// reason: saturated air has a real VPD of exactly 0.00 kPa, so any non-negative
// sentinel hides a valid reading. Humidity is clamped to [0, 100] first, because
// a capacitive sensor reporting 101% at saturation is normal and would otherwise
// come back as a small negative deficit.
//
// Air-to-air, not leaf-to-air. The only thermal figure this system has is the
// scene maximum, which a lamp in frame wins; substituting it here would produce
// a confident, wrong number. derive.py rejected the same substitution for the
// same reason, and reports the raw gap separately instead.
float aijudge_vpd_kpa(float temp_c, float rh_pct);

// ---- the panel's own bands --------------------------------------------------
//
// What this device thinks each metric's band is, for use only when the server has
// named none through control.setpoints. Exported for the same reason
// aijudge_vpd_kpa() is: the monitor tiles judge the same readings this rule does,
// and they used to do it against a second, wider set of numbers - so the log said
// 증산 과다 경향 about a VPD the tile beside it was drawing green. One table, read
// by both.
//
// Returns false for a metric the panel holds no opinion about, leaving *lo and
// *hi untouched; the thermal scene peak is the live example, since MetricKey has
// no name for it either. Either bound may be NAN on the true path - CO2 has a
// floor and deliberately no ceiling.
bool aijudge_panel_band(const char *metric, float *lo, float *hi);
