// The uplink: this panel's client for the plantrx server.
//
// One endpoint does everything. The device polls POST /v1/telemetry and the
// response IS the prescription - there is no second request to fetch it, and no
// connection inbound. The greenhouse is behind whatever NAT it happens to be
// plugged into, so the device drives every exchange; the server's only lever is
// `next_poll_s`, which is how it asks to be spoken to sooner or later.
//
// WHAT THIS OWNS AND WHAT IT DOES NOT. This file speaks HTTP and JSON and hands
// the parsed result to the modules that own the meaning:
//   - judgment rows go to aijudge_append_llm(), into the same ring the local
//     rule writes, because the panel has one log and not two
//   - the judgment turn goes to aijudge_set_server_turn(), which supersedes the
//     rule's countdown while the link is up
//   - display.plan / display.actions / display.notice are kept here for the UI
//     to read, since nothing else has a claim on them
// Control - setpoints, schedules, once - is deliberately NOT applied yet. The
// actuator path does not exist on this board, and a client that parsed setpoints
// into variables nobody reads would look like it was holding a band it is not.
// When actuators land, that is a separate producer reading plantrx_control().
//
// THE UPLINK IS NOT THE SYSTEM. Every failure here is silent by design: the
// local rule keeps judging, the countdown falls back to its schedule, and the
// panel keeps showing the last thing it was told. A greenhouse whose display
// goes blank because a server in a datacentre restarted is worse than one that
// admits the uplink is down in a single row of the status bar.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Row counts, mirroring PLAN_ROWS_MAX and ACTION_ROWS_MAX in
// server/app/schema.py. The server clips to them, and a mismatch silently drops
// rows the server believes are on the panel.
#define PLANTRX_PLAN_MAX   4
#define PLANTRX_ACTION_MAX 4
// The window table is four rows because that is the height left under the
// sensor strip on the monitor page, not because the server has only four
// things to say. The server picks which four metrics earned the space and
// clips the rest, for the same reason it clips the judgment ring: a table that
// scrolls on a wall panel is a table nobody reads.
#define PLANTRX_WINDOW_ROWS_MAX 4

// Buffer sizes - array extents, not byte budgets. Each is one over the server's
// ROW_* / NOTICE_BYTES budget, because these are C strings and the terminator is
// not part of the text. server/tests/test_display_contract.py parses this file
// and fails when the two stop lining up, which is the only thing keeping them
// honest: a narrowed buffer truncates mid-syllable on the wall while the server
// still believes the string fits.
//
// head gets the same 64 bytes a judgment head gets, for a measured reason: a
// prescription ordering one action against one setpoint already renders
// "미스트 1분 가동, VPD 목표 0.8 – 1.2 kPa" at 49 bytes. Note the en dash: render.py
// writes ranges with U+2013, which costs two bytes more than the ASCII hyphen it
// reads as, and copying that string with a hyphen measures 47 and makes the
// budget look roomier than it is. A 48-byte budget would have clipped the
// commonest row on the panel, and the column wraps rather than ellipsizing, so
// the wider buffer costs at most one more line.
#define PLANTRX_AT_CAP     8    // "14:20", or "" for a row with no clock time
#define PLANTRX_TAG_CAP    13   // "완료" / "대기" / "보류" / "실패"
#define PLANTRX_HEAD_CAP   64
#define PLANTRX_COND_CAP   79
#define PLANTRX_DELTA_CAP  49
#define PLANTRX_NOTICE_CAP 201

// The window row's own extents, on the same BYTES+1 rule and mirroring
// ROW_W*_BYTES in server/app/schema.py. Narrow on purpose: these are table
// cells scanned down a column, and a stat that will not fit "0.8 / 1.1 / 1.4
// kPa" is a sentence, which belongs in a judgment head and not here.
#define PLANTRX_WLABEL_CAP 17
#define PLANTRX_WSTAT_CAP  25
#define PLANTRX_WBAND_CAP  21

// The species card's other two strings, on the same BYTES+1 rule.
//
// The confidence: "100%" is the measured worst case at 4 bytes; the budget is 7
// so that a value which ever grows a suffix clips at a character boundary instead
// of losing the '%' that makes the number a percentage - a clipped "100" reads as
// a count.
//
// The binomial: PlantNet returns scientificNameWithoutAuthor, so nothing appends
// an author string. "Chrysanthemum leucanthemum" is the widest this had to
// survive at 26 bytes and 174 px in font_reg_12, which the header strip clears by
// 18 px in its tightest state - see refresh_identify() for that arithmetic.
//
// Here rather than beside RX_SPECIES_CAP in src/plantrx.cpp, which looks like
// the odd one out but is not: that cap is sized to match plantid.cpp's own
// buffer, so it is a device constraint that happens to also bound a wire field,
// and the contract test reads it with a bespoke reader for exactly that reason.
// These two are only ever server budgets, so they live with the server budgets
// and the test's generic header walker covers them for free.
#define PLANTRX_CONF_CAP   8
#define PLANTRX_SCI_CAP    32

// The tone a row is tinted with. Distinct from JudgeLevel: a level is a verdict
// about the plant and needs three grades, while these rows report what the
// system did, where "it worked" / "it did not" / "nothing to say" is the whole
// vocabulary.
enum RxTone { RX_TONE_INFO, RX_TONE_OK, RX_TONE_WARN };

// Future tense: something that will happen, and what makes it happen. `at` is
// empty for a row with no clock time - a condition fires when it fires - which
// the UI draws as a blank column rather than a fake time.
struct RxPlanRow {
    char at[PLANTRX_AT_CAP];
    char tag[PLANTRX_TAG_CAP];
    RxTone tone;
    char head[PLANTRX_HEAD_CAP];
    char cond[PLANTRX_COND_CAP];
};

// Past tense: something that was asked for, and what came of it. `delta` is the
// change measured since, and `delta_is_reading` says whether it is a number from
// a sensor or a sentence about the absence of one - the UI draws a measurement
// and an excuse differently, and conflating them is how "실행 기록 없음" ends up
// looking like a reading.
struct RxActionRow {
    char at[PLANTRX_AT_CAP];
    char tag[PLANTRX_TAG_CAP];
    RxTone tone;
    char head[PLANTRX_HEAD_CAP];
    char delta[PLANTRX_DELTA_CAP];
    bool delta_is_reading;
    bool improved;
};

// Past tense, but about the window rather than about an order: what a metric
// actually did over the hour the server just summarised. The monitor page
// draws live numbers and nothing else, so a reading sitting inside its band
// says nothing about whether it has been there all hour or arrived a minute
// ago - and that difference is the entire question a grower is asking when
// they walk up to the panel. `stat` and `band` arrive pre-rendered because the
// server owns the units, the rounding and the Korean; the device draws them.
//
// `in_band_pct` is -1, not 0, when the server sent null. 0 is a real and
// alarming answer - the metric was never once inside its band - and a metric
// that has no band at all is a different fact that must not be tinted like
// one. Anything that colours from this has to test for -1 before comparing.
struct RxWindowRow {
    char label[PLANTRX_WLABEL_CAP];
    char stat[PLANTRX_WSTAT_CAP];
    char band[PLANTRX_WBAND_CAP];
    int  in_band_pct;
};

// What the uplink is doing, for the status bar. RX_STALE is the one that matters:
// the link is up and the server answered, but nothing it said is recent enough to
// still be true, which is a different thing from being disconnected and the panel
// should not draw them the same.
enum RxLink {
    RX_OFF,        // no server configured
    RX_WAITING,    // configured, nothing exchanged yet this boot
    RX_OK,         // a prescription arrived within the last few polls
    RX_STALE,      // answered once, but not recently
    RX_ERROR,      // transport or protocol failure on the last attempt
};

// Starts the client. Call once at boot after net_init(); it does no network I/O
// itself, so a board with no WiFi credentials pays nothing for this.
void plantrx_init(void);

// Drives one step of the exchange. Call freely from loop(); it self-schedules
// against the server's next_poll_s and does nothing while the radio is down.
//
// This blocks for the length of an HTTP round trip, which is why it runs from
// loop() on core 1's Arduino task and not from an LVGL timer: a timer callback
// that waits on a socket stalls the display for as long as the server takes.
void plantrx_poll(void);

// True once a server address exists. False disables the whole client, and the UI
// draws the log as rule-authored, which is what it then is.
bool plantrx_configured(void);

RxLink plantrx_link(void);

// Seconds since the last successful exchange, or -1 if none has happened this
// boot. The 설정 page's uplink block draws this rather than a bare state, because
// "3s ago" and "40 min ago" are both RX_OK and only one of them means the link is
// current. The top bar used to draw it too and no longer carries any uplink
// readout, so this number now has exactly one reader on screen.
//
// This is TRANSPORT age and nothing else: it answers "is the conversation
// alive". It says nothing about whether what was said is new, because a server
// whose model has failed answers every poll with the previous prescription
// verbatim and every one of those is a success. plantrx_content_age_s() is the
// other half, and any widget describing the judgment wants that one.
int32_t plantrx_age_s(void);

// Seconds since the server last minted a new rx_id: the age of the judgment the
// panel is holding, as opposed to the age of the conversation. -1 when there is
// no real judgment to age - nothing has arrived yet, or what arrived carries the
// "none" placeholder id the server sends when its model has never run for this
// device. A number climbing here while plantrx_age_s() sits near zero is a dead
// model behind a healthy link, and this is the only thing on the device that
// says so; before it existed the panel drew a green dot and "3초 전" for a model
// that had not answered since breakfast.
//
// Measured from FIRST SIGHT of an id, this boot. There is no wire field saying
// when the server minted it and there deliberately is not one, so after a reboot
// a prescription the server has been replaying for five hours reads as age 0
// until its id next changes. That under-reports and never over-reports, and it
// is bounded by uptime. Deriving it from the reply's issued_ts instead was
// rejected for the reason the turn countdown uses next_ts - issued_ts rather
// than a clock: server-timestamp-minus-device-clock is wrong on a board whose
// NTP never landed, and a wrong age is worse than a young one.
int32_t plantrx_content_age_s(void);

// The id of the prescription the panel is holding; "" before any reply, never
// NULL. It is the server's own statement of content identity - a fresh judgment
// gets a new one, a replay keeps the old one - which is what makes the age above
// measurable without a second wire field.
const char *plantrx_rx_id(void);

// Whether that id names an actual judgment rather than the "none" placeholder.
// Anything comparing the panel against what "the server thinks" has to gate on
// this first: a placeholder prescription carries server defaults, and
// disagreeing with a default is not a disagreement with the server.
bool plantrx_rx_real(void);

// Whether the server put this device in auto. The switch on the panel is the
// device's own; this is what the server thinks it is, and a disagreement is worth
// showing rather than silently resolving.
bool plantrx_mode_auto(void);

// display.model_ready: whether the server has a model configured and answering,
// which is its brain.is_configured() and nothing the device can observe for
// itself. A keyless server is not a broken server - it answers every poll with a
// well-formed 200 carrying its own rule output, so plantrx_configured() and
// plantrx_link()==RX_OK are both true and neither of them means a model exists.
// The 예약 / 조치 badges used to read 모델 off exactly those two, which put the
// model's name on rows the model had never seen.
//
// FALSE when the field is absent. A server predating this key omits it, and the
// whole reason the key exists is that "no evidence of a model" must not draw as
// a model; an older keyed server therefore understates itself, which is the same
// trade plantrx_content_age_s() makes above and the same direction.
bool plantrx_model_ready(void);

// control.setpoints: the band the server asked us to hold for one metric.
//
// This is the one piece of the control half the client keeps, and it is kept
// because a band is not an action - see the note at the top of plantrx.cpp. The
// panel has to decide what to call a reading it is already drawing, and before
// this getter it decided with numbers compiled into page_monitor.cpp and
// aijudge.cpp while the window table beneath those same tiles was drawn from the
// server's bands. One screen, two answers to "is this reading in band".
//
// `metric` is the server's MetricKey vocabulary verbatim: "vpd_kpa", "air_c",
// "rh_pct", "co2_ppm", "leaf_air_dt_c" (server/app/schema.py:123). Note air_c and
// not temp_c - temp_c is the Sensors field name for the same quantity, and
// derive.py:21 is where the two vocabularies meet. Anything outside those five
// returns false, so a caller cannot invent a metric the server could never band.
//
// Returns false when the server has never named a band for this metric, leaving
// *lo and *hi untouched: the caller keeps whatever it seeded and must then say
// the threshold is its own. Either bound may be NAN even on the true path -
// "hold CO2 below 1000" names no floor, and inventing one would be the same
// fabrication this getter exists to remove.
bool plantrx_band(const char *metric, float *lo, float *hi);

// display.notice: why the judgment column is empty, in the server's words. "" when
// there is nothing to apologise for. An empty column with no explanation reads as
// a broken screen, which is the failure this field exists to prevent.
const char *plantrx_notice(void);

// The species card, resolved server-side by PlantNet. "" until identified.
const char *plantrx_species(void);

// display.species.conf_text: PlantNet's confidence in that name, pre-formatted
// as "87%" because the server owns the rounding. "" when the card carries no
// figure, which the UI draws as a hidden chip rather than as "0%" - zero is a
// real confidence and "not reported" is not.
//
// This exists for the board whose 식물 식별 button was never pressed. With a local
// result the panel draws its own name and its own score; with neither, this pair
// is the only provenance the wall has for a name it is asserting.
const char *plantrx_species_conf(void);

// display.species.sci: the binomial behind that name, "" when absent. Drawn
// beside the name rather than hidden, because `text` is not always trustworthy -
// the server machine-translates an English common name when Wikipedia has no
// Korean article (plantnet._resolve_korean), and Korean plant names collide
// across species regardless. The percentage says how sure; this says of what.
const char *plantrx_species_sci(void);

int plantrx_plan_count(void);
const RxPlanRow *plantrx_plan_at(int i);
int plantrx_action_count(void);
const RxActionRow *plantrx_action_at(int i);
int plantrx_window_count(void);
const RxWindowRow *plantrx_window_at(int i);

// How much of the window the server actually had samples for. covered_s below
// span_s means every number in the rows above was measured over a gap: a
// sensor node that dropped out for half the hour still yields a confident
// "유지 87%", and the panel has to be able to say the coverage was thin instead
// of repeating the figure. span_s of 0 is "no window was measured at all",
// which is not the same fact as a window in which nothing happened, and the UI
// draws the table only when it is non-zero.
int plantrx_window_span_s(void);
int plantrx_window_covered_s(void);

// Bumped whenever any of the above changes. The UI rebuilds its 예약 / 실행 rows
// only when this moves, for the same reason aijudge_revision() exists: rebuilding
// means destroying and re-creating widgets, which is far too costly per UI tick.
uint32_t plantrx_revision(void);

// Ask for a diagnosis on the next poll - the 지금 진단 button. The server still
// owns the decision and its own floor outranks this, so the honest reading of
// this call is "the user is asking", not "the model will run".
void plantrx_ask_now(void);

// Prints one line per exchange: the decision, the round-trip time, what changed,
// and both ages - `age` is the transport one, `cage` the content one, so a model
// that has stopped answering is visible over serial with no UI. `thm` is the
// thermal liveness gate that now decides whether leaf_max_c goes on the wire at
// all. Call freely from loop(); it self-throttles.
void plantrx_debug_tick(void);

// ---- diagnosing a dead uplink -----------------------------------------------
//
// What the settings page has to show once the status bar has said the link is
// down and the next question is why. Every one of these reads state the
// exchange already keeps for its own serial line, so the page may poll the
// whole block on a UI timer without the poll path paying for it.

// "192.168.10.20:8000", as configured; "" when no server is set and the client
// is off. The first thing to check when nothing arrives, because a panel
// pointed at the wrong host looks exactly like a server that is down.
const char *plantrx_host(void);

// The same address in the three pieces parse_base_url() split PLANTRX_BASE_URL
// into, rather than the one string above that exists to be printed. These are
// not diagnostics: src/plantid.cpp borrows them because the 식별 button POSTs
// the camera still to this same server on a connection of its own, and a second
// file parsing the same base URL would put two answers to "where is the server"
// in one firmware - which is one more than there can be, and the second one is
// always the one that goes stale.
//
// The host is "" until plantrx_init() has run, and stays "" when the base URL is
// not http://host[:port][/prefix]: an unparseable URL reads as unconfigured here
// exactly as it does for the uplink, so a borrower that finds an empty host has
// no server to reach and must not invent one. The port defaults to 80, and the
// prefix is "" for a base URL with no path - never a bare "/", because it is
// concatenated straight onto the endpoint path.
const char *plantrx_srv_host(void);
uint16_t    plantrx_srv_port(void);
const char *plantrx_srv_prefix(void);

// HTTP status of the last exchange. 0 is "nothing attempted this boot", -1 is
// "the connection or the reply never happened at all" - a 500 means the server
// is alive and answering, which is a different afternoon of debugging from
// silence. A reply whose status line could not be read also lands as 0, told
// apart from the untouched case by plantrx_last_error() being non-empty.
int plantrx_last_status(void);

// How long the last exchange took, in ms; 0 before the first one. The server
// runs its model inside this window, so seconds are normal and a number
// climbing toward the 20s deadline is the warning that comes before the polls
// start timing out. A failed attempt reports how long it took to fail, which
// is the useful half of a connect timeout: 4000 means nothing answered.
uint32_t plantrx_last_rtt_ms(void);

// Why the last exchange failed, in Korean short enough for one status line;
// "" after an exchange that succeeded. It separates a refused connection from
// silence from an HTTP error from a body that would not parse, because those
// are four different things to go and look at and the status code alone tells
// two of them apart at best.
const char *plantrx_last_error(void);

// Prescriptions that have landed since boot. Zero beside a configured host
// says the uplink has never once worked; a count that stopped moving while the
// panel stayed up says it worked and then something changed.
uint32_t plantrx_exchanges(void);

// Failures since the last success, reset to 0 by one. The backoff is computed
// from this, so it is also the answer to why a panel that is failing is
// retrying far less often than it did a minute ago.
uint32_t plantrx_failures(void);
