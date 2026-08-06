// The uplink client declared in plantrx.h: one POST, one prescription.
//
// Structure, and why it is this shape:
//
// PARSE FULLY, THEN APPLY UNDER ONE LOCK. Every field lands in a staging copy
// first and is only committed once the whole body has been walked. A body that
// is truncated a third of the way through - the exact thing a server restart
// mid-response produces - would otherwise leave the 예약 column from the new
// prescription beside the 조치 column from the old one, and nothing on the
// panel would say which row belonged to which. A half-applied prescription is
// worse than a stale one, because a stale one is at least internally
// consistent. The commit then happens with the LVGL lock held, for the same
// reason main.cpp wraps aijudge_tick(): the UI thread reads these rows.
//
// CONTROL IS NOT ACTUATED, BUT SETPOINTS ARE READ. There is no actuator on this
// board: no relay, no driver, nothing that could act on a schedule - so
// `schedules`, `once` and `policy` are still parsed past and dropped on the
// floor, and that is not an oversight. Storing a misting schedule nobody can run
// would make the client look like it was holding one, and the first person to
// debug a mister that never fires would spend the afternoon here instead of in
// the wiring. When relays land, that is a new producer reading a
// plantrx_control() this file will grow - not a bug fix.
//
// `setpoints` is the exception, and it is one because a band is not an action.
// It is the server's answer to "what counts as too dry", and the panel was
// already answering that question with numbers compiled into two of its own
// files - so the tiles and the local rule judged the same readings against
// thresholds the server had never agreed to, directly beneath a window table
// drawn from the server's own bands. Two vocabularies on one screen is the bug;
// plantrx_band() exists so there is one. Nothing here acts on a band, and
// nothing can: it only decides what the panel calls the reading it is already
// drawing.
//
// PLAIN HTTP, ON PURPOSE. The server is on the greenhouse LAN, reached over the
// same radio the camera already streams unencrypted MJPEG across, so TLS here
// would protect the one link on the board that is already the least exposed -
// and would cost an mbedtls context per poll on a board whose PSRAM is spoken
// for by the decode buffers. plantrx_config.h documents this; a non-http base
// URL reads as unconfigured rather than being quietly downgraded.
//
// EVERY FAILURE IS SILENT AND KEEPS THE LAST GOOD ANSWER. No path here clears a
// row. The worst any failure does is move RxLink and let the status bar say so.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plantrx.h"
#include "sitecfg.h"          // the server address and shared secret, NVS-backed
#include "aijudge.h"
#include "camnet.h"
#include "camprov.h"
#include "lvgl_v8_port.h"
#include "metrics.h"
#include "net.h"
#include "health.h"
#include "plantid.h"
#include "hlog.h"
#include "reading.h"
#include "sensornode.h"
#include "thermal.h"
#include "ui.h"
#include "updatemode.h"
#include "fwpull.h"

// The request is a few hundred bytes; the reply carries one judgment row, four
// plan rows, four action rows, the four-row window table and the control half.
// Measured, not estimated: server/tests/test_window_block.py dumps a worst-case
// Prescription at 5828B, of which the window block is 434B - a window label is
// not free text, it comes from render._METRICS, so the four longest labels that
// can appear total 30B against the 4x16 the schema permits. A hop that
// re-encodes the body with \uXXXX escapes lands at 8336B, still inside. The
// headers land in here too, since read_reply() strips them in place rather than
// paying for a second buffer.
//
// 16KB and no longer 12KB. The one judgment row now carries the model's prose in
// full - up to JUDGE_BODY_BYTES = 1023 bytes, which is ~2KB once a proxy
// \uXXXX-escapes the Hangul at 6 bytes a syllable - where the five extra short
// rows it replaced were a few hundred bytes between them. 12KB would have held
// it, but with the control half free to grow this is the buffer that would
// silently truncate a valid prescription, and 4KB of PSRAM-backed scratch is the
// cheapest thing on this board.
static const size_t MAX_REQ  = 2 * 1024;
static const size_t MAX_RESP = 16 * 1024;

// MAX_FRAME_BYTES in server/app/main.py. Refused here rather than posted and
// 413'd, because a rejected upload still costs the whole transfer.
static const size_t MAX_FRAME = 256 * 1024;

static const uint32_t CONNECT_MS = 4000;
// The telemetry POST is where the server runs its model, so this deadline is
// bounded by inference and not by the network. plantid.cpp allows PlantNet 25s
// for the same reason. It blocks loop(), never the LVGL task.
static const uint32_t RESP_DEADLINE_MS = 20000;
static const uint32_t RESP_IDLE_MS     = 4000;
static const uint32_t FRAME_DEADLINE_MS = 8000;

// Floor and ceiling on the server's next_poll_s. The floor stops a server bug
// from turning the panel into a load generator; the ceiling stops a bad value
// from parking the uplink for a day with no way back short of a reboot.
static const uint32_t POLL_MIN_MS = 2000;
static const uint32_t POLL_MAX_MS = 300000;

// Backoff after a failed exchange: 5s doubling to 2 min. Retrying at the idle
// cadence against a server that is down is 1440 pointless connects a day, and
// each one costs the radio time the camera puller wants.
static const uint32_t FAIL_BASE_MS = 5000;
static const uint32_t FAIL_MAX_MS  = 120000;

// Five idle polls. One missed poll is a dropped packet, not staleness, and
// flipping the status bar on every one of those would make the indicator noise.
static const uint32_t STALE_AFTER_MS = 300000;

// How long a requested frame is waited for before the arm is abandoned. The
// puller publishes on its next complete frame, ~70ms at 15fps, so anything past
// a couple of seconds means the CAM stopped; 10s is generous and still lets the
// next want_frame re-arm rather than sitting armed forever.
static const uint32_t FRAME_WAIT_MS = 10000;

// Matches plantid.cpp's s_korean, so a name that fit the on-device
// identification path also fits the server-resolved card.
#define RX_SPECIES_CAP 128

// ---- state ------------------------------------------------------------------

static bool     s_configured = false;
static char     s_host[64];
static uint16_t s_port = 80;
static char     s_prefix[48];      // path prefix from the base URL, "" for none
// "host:port" as one string. Built at init rather than per call because the
// settings page polls it on a UI timer, and an snprintf per tick to rebuild a
// string that cannot change is work the panel pays for as long as it is up.
static char     s_host_disp[72];
// Borrowed from sitecfg, not copied: its buffers outlive every caller and never
// change after init, so a second copy of a secret in DRAM would buy nothing.
// "" until plantrx_init() runs, which keeps write_request_head() honest if it is
// ever reached before then.
static const char *s_tok = "";

static char    *s_req  = nullptr;  // PSRAM: the request body
static char    *s_resp = nullptr;  // PSRAM: the reply (NUL-terminated)

// Live prescription, read by the UI thread under the LVGL lock.
static RxPlanRow   s_plan[PLANTRX_PLAN_MAX];
static RxActionRow s_action[PLANTRX_ACTION_MAX];
static int         s_plan_n = 0, s_action_n = 0;
// The window table and its coverage, alongside them for the same reason: the
// UI thread reads all of it under one lock and it is committed as one set.
static RxWindowRow s_window[PLANTRX_WINDOW_ROWS_MAX];
static int         s_window_n = 0;
static int         s_window_span_s = 0, s_window_covered_s = 0;
static char        s_notice[PLANTRX_NOTICE_CAP];
static char        s_species[RX_SPECIES_CAP];
static char        s_species_conf[PLANTRX_CONF_CAP];
static char        s_species_sci[PLANTRX_SCI_CAP];
static uint32_t    s_rev = 0;
static bool        s_mode_auto = true;
static char        s_rx_id[48];
// millis() when s_rx_id last became a *different* id: the content clock, and
// deliberately not s_last_ok_ms. That one moves on every successful exchange,
// including the ones where main.py's model threw and it replayed the previous
// prescription verbatim - so a model that has been dead for six hours answers
// those polls indistinguishably from a healthy one. rx_id is the server's own
// statement of content identity: a fresh judgment mints a new one, a replay
// keeps the old one, and that is the whole mechanism. No new wire field.
static uint32_t    s_rx_changed_ms = 0;

// Staging. Filled by the parser, copied over the live set only once the whole
// body has been walked without a fault.
static RxPlanRow   t_plan[PLANTRX_PLAN_MAX];
static RxActionRow t_action[PLANTRX_ACTION_MAX];
static int         t_plan_n = 0, t_action_n = 0;
static RxWindowRow t_window[PLANTRX_WINDOW_ROWS_MAX];
static int         t_window_n = 0;
static int         t_window_span_s = 0, t_window_covered_s = 0;
static char        t_notice[PLANTRX_NOTICE_CAP];
static char        t_species[RX_SPECIES_CAP];
static char        t_species_conf[PLANTRX_CONF_CAP];
static char        t_species_sci[PLANTRX_SCI_CAP];
static JudgeRecord t_judge[AIJUDGE_CAP];
static int         t_judge_n = 0;
static bool        t_turn_sched = false;
static uint32_t    t_turn_remain_ms = 0, t_turn_period_ms = 0;
static bool        t_mode_auto = true;
static char        t_rx_id[48];

// control.setpoints, as a fixed row per metric rather than a list, because the
// vocabulary is closed: MetricKey is five names (server/app/schema.py:123) and a
// sixth cannot arrive without a schema change that would come through here
// anyway. A table indexed by that vocabulary makes the lookup a strcmp walk over
// five entries with no allocation and no cap to overflow, and it makes "the
// server sent a band for a metric we do not know" impossible to store rather
// than merely unlikely.
//
// These are the server's spellings and not the panel's. air_c is the metric; the
// sensor field carrying it is temp_c (derive.py:21 maps between them and says
// so), and getting that backwards would silently return no band for temperature
// forever - a band that is missing looks exactly like a server that never sent
// one.
static const char *const BAND_METRICS[] = {
    METRIC_VPD, METRIC_AIR_C, METRIC_RH, METRIC_CO2, METRIC_LEAF_DT,
};
#define BAND_N ((int)(sizeof(BAND_METRICS) / sizeof(BAND_METRICS[0])))

struct RxBand {
    bool  have;    // the server named this metric at all
    float lo, hi;  // NAN on a side it did not bound
};
static RxBand s_band[BAND_N];
static RxBand t_band[BAND_N];

static bool s_model_ready = false;
static bool t_model_ready = false;

static uint32_t s_next_ms = 0;       // millis() of the next attempt
static uint32_t s_last_ok_ms = 0;    // millis() when the last prescription landed
static bool     s_have_ok = false;
static bool     s_last_failed = false;
static uint32_t s_fails = 0;
static uint32_t s_exchanges = 0;   // prescriptions landed since boot, never reset
static bool     s_ask_now = false;
static bool     s_link_was_up = false;

static bool     s_want_frame = false;
static bool     s_frame_armed = false;
static uint32_t s_frame_armed_ms = 0;

// What the last exchange did. Printed once per attempt by plantrx_debug_tick()
// and read live by the settings page through the getters at the foot of this
// file, which is why the status and the round trip outlive the attempt that
// set them instead of being cleared when the next one starts.
static uint32_t s_seq = 0, s_dbg_seq = 0;
static int      s_dbg_status = 0;
static uint32_t s_dbg_rtt = 0;
static int      s_dbg_appended = 0;
static bool     s_dbg_changed = false;
static size_t   s_dbg_frame_len = 0;
static const char *s_dbg_why = "";
// The newest ingested judgment's frame flags as "rt" / "r-" / "-t" / "--", and
// "?" when the exchange carried no judgments at all. Two characters on the
// serial line, because "both false" and "the keys never arrived" are different
// facts about a reply and the log is where that gets caught.
static char     s_dbg_saw[4] = "?";
// Its chips' tones, one character each, i/o/w. "-" for a row with no chips.
static char     s_dbg_tone[AIJUDGE_EVID_MAX + 1] = "-";

// The last failure in the panel's own words, "" after a success. Set from the
// same tag s_dbg_why carries so the serial line and the settings page cannot
// disagree about why the uplink is down.
static const char *s_last_err = "";

// ---- request building -------------------------------------------------------
//
// A cursor over one preallocated buffer. String concatenation would put a heap
// churn of a dozen temporaries in a path that runs every poll for months.
struct Buf { char *p; size_t cap; size_t n; };

// Overflow is sticky: once a field would not fit, nothing further is written
// and `n` parks one short of cap. The caller checks and abandons the poll,
// which is better than shipping a body that silently lost its middle.
static void badd(Buf *b, const char *fmt, ...) {
    if (b->n + 1 >= b->cap) { b->n = b->cap; return; }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->cap - b->n) { b->n = b->cap; return; }
    b->n += (size_t)w;
}

static bool boverflow(const Buf *b) { return b->n >= b->cap; }

// Close an object/array, dropping the trailing comma the field helpers leave.
static void bend(Buf *b, char close) {
    if (!boverflow(b) && b->n && b->p[b->n - 1] == ',') b->n--;
    badd(b, "%c", close);
}

// "key":"value", with value escaped.
//
// Two strings reach this: the MAC this board printed, and an rx_id the server
// minted that we are echoing back. Escaping the ASCII controls plus " and \ is
// sufficient for both, and for any UTF-8 that ever arrives here, because every
// byte of a multi-byte sequence is >= 0x80 - JSON requires no escaping above
// ASCII and none of those bytes can collide with a delimiter. A byte this
// cannot represent does not exist; there is no silent-drop case.
static void badd_str(Buf *b, const char *key, const char *v) {
    badd(b, "\"%s\":\"", key);
    for (; *v; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\')  badd(b, "\\%c", (char)c);
        else if (c == '\n')         badd(b, "\\n");
        else if (c == '\r')         badd(b, "\\r");
        else if (c == '\t')         badd(b, "\\t");
        else if (c < 0x20)          badd(b, "\\u%04x", (unsigned)c);
        else if (b->n + 2 < b->cap) b->p[b->n++] = (char)c;   // the common byte, no vsnprintf
        else                      { b->n = b->cap; return; }
        if (boverflow(b)) return;
    }
    badd(b, "\",");
}

static void badd_sensor(Buf *b, const char *key, float v, int dp) {
    if (reading_present(v)) badd(b, "\"%s\":%.*f,", key, dp, v);
    else                badd(b, "\"%s\":null,", key);
}

// ---- JSON walking -----------------------------------------------------------
//
// plantid.cpp's strstr lookup is enough for a flat reply. This one is not flat:
// "at" and "head" appear in judgments, in plan and in actions, so a search that
// ignores structure would answer a plan row's clock to a judgment row's query
// and nothing downstream could tell. These walk the value tree and never leave
// the object they were handed.

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// One past the end of the value at p, or NULL when it is malformed or the body
// was cut off inside it - which is how truncation is detected at all, since a
// truncated body is otherwise just a shorter string.
static const char *val_end(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        return *p == '"' ? p + 1 : nullptr;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {                       // a string can hold braces
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                if (!*p) return nullptr;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) return p + 1;
        }
        return nullptr;
    }
    const char *q = p;                              // number, true, false, null
    while (*q && !strchr(",}] \t\r\n", *q)) q++;
    return q == p ? nullptr : q;
}

// Value of `key` directly inside the object at `obj`. Depth-1 only: a nested
// object is skipped whole, which is what keeps a judgment row's fields from
// answering for the row's chips.
//
// Member names are compared raw. Every key in this schema is a Python
// identifier, so none of them can contain an escape.
static const char *obj_get(const char *obj, const char *key) {
    if (!obj) return nullptr;
    const char *p = skip_ws(obj);
    if (*p != '{') return nullptr;
    p = skip_ws(p + 1);
    size_t klen = strlen(key);
    while (*p && *p != '}') {
        if (*p != '"') return nullptr;
        const char *ks = p + 1;
        const char *ke = val_end(p);                // just past the closing quote
        if (!ke) return nullptr;
        const char *c = skip_ws(ke);
        if (*c != ':') return nullptr;
        const char *v = skip_ws(c + 1);
        if ((size_t)(ke - 1 - ks) == klen && strncmp(ks, key, klen) == 0) return v;
        const char *ve = val_end(v);
        if (!ve) return nullptr;
        p = skip_ws(ve);
        if (*p == ',') p = skip_ws(p + 1);
    }
    return nullptr;
}

static const char *arr_first(const char *arr) {
    if (!arr) return nullptr;
    const char *p = skip_ws(arr);
    if (*p != '[') return nullptr;
    p = skip_ws(p + 1);
    return (*p == ']' || !*p) ? nullptr : p;
}

static const char *arr_next(const char *el) {
    const char *e = val_end(el);
    if (!e) return nullptr;
    e = skip_ws(e);
    if (*e != ',') return nullptr;
    e = skip_ws(e + 1);
    return (*e == ']' || !*e) ? nullptr : e;
}

// Whole code point or nothing, for the \u path. That is what makes the clip
// below safe without a UTF-8 back-off pass: half a sequence never enters the
// buffer in the first place, so there is no partially written tail to walk back
// over - and no uninitialised bytes to inspect while doing it.
static bool utf8_put(char *out, size_t cap, size_t *i, uint32_t cp) {
    size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (*i + need + 1 > cap) return false;
    if (need == 1) { out[(*i)++] = (char)cp; return true; }
    static const uint8_t lead[5] = { 0, 0, 0xC0, 0xE0, 0xF0 };
    out[(*i)++] = (char)(lead[need] | (cp >> (6 * (need - 1))));
    for (size_t k = need - 1; k > 0; k--) out[(*i)++] = (char)(0x80 | ((cp >> (6 * (k - 1))) & 0x3F));
    return true;
}

// The body is already UTF-8, so a multi-byte sequence is copied verbatim rather
// than decoded and re-encoded: treating each byte as its own code point is how
// every Korean string comes out double-encoded, which is invisible on a byte
// count and only shows up as mojibake on the wall. Same whole-or-nothing rule,
// enforced on the run instead of on a code point. Returns false on a sequence
// that will not fit or that the body cut short.
static bool utf8_run(char *out, size_t cap, size_t *i, const char **v) {
    const unsigned char *s = (const unsigned char *)*v;
    size_t need = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : 2;
    for (size_t k = 1; k < need; k++) {
        if ((s[k] & 0xC0) != 0x80) return false;    // truncated or not UTF-8 at all
    }
    if (*i + need + 1 > cap) return false;
    for (size_t k = 0; k < need; k++) out[(*i)++] = (char)s[k];
    *v += need;
    return true;
}

static int hex4(const char *p) {
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p[k];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    return v;
}

// Copy the JSON string at v into out, unescaping. Always NUL-terminates.
//
// The server clips every field to a budget one byte under `cap`, so running out
// of room here means the two sides have drifted apart. What is already decoded
// is kept and the rest is dropped on a code-point boundary: half a Hangul
// syllable in the buffer draws as one broken glyph on the wall and is
// completely invisible in a diff, which is the failure server/app/schema.py's
// own comment warns about from the other side.
//
// \u is decoded even though Starlette renders with ensure_ascii=False and so
// never emits one - it is twenty lines against a whole class of "someone put a
// proxy in front of the server" bug.
static bool str_copy(const char *v, char *out, size_t cap) {
    if (!v) return false;
    v = skip_ws(v);
    if (*v != '"') return false;
    v++;
    size_t i = 0;
    while (*v && *v != '"') {
        uint32_t cp;
        if (*v == '\\') {
            v++;
            if (!*v) return false;
            switch (*v) {
                case 'n': cp = '\n'; v++; break;
                case 'r': cp = '\r'; v++; break;
                case 't': cp = '\t'; v++; break;
                case 'b': cp = '\b'; v++; break;
                case 'f': cp = '\f'; v++; break;
                case 'u': {
                    int hi = hex4(v + 1);
                    if (hi < 0) return false;
                    v += 5;
                    cp = (uint32_t)hi;
                    if (cp >= 0xD800 && cp < 0xDC00 && v[0] == '\\' && v[1] == 'u') {
                        int lo = hex4(v + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                            v += 6;
                        }
                    }
                    break;
                }
                default: cp = (unsigned char)*v++; break;   // \" \\ \/ and anything else
            }
        } else if ((unsigned char)*v >= 0xC0) {
            if (!utf8_run(out, cap, &i, &v)) break;
            continue;
        } else {
            cp = (unsigned char)*v++;    // ASCII, or a stray byte we pass through
        }
        if (!utf8_put(out, cap, &i, cp)) break;
    }
    out[i] = '\0';
    return true;
}

static bool str_get(const char *obj, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    return str_copy(obj_get(obj, key), out, cap);
}

static int int_get(const char *obj, const char *key, int def) {
    const char *v = obj_get(obj, key);
    if (!v || *v == 'n') return def;                // absent, or JSON null
    return atoi(v);
}

static bool bool_get(const char *obj, const char *key, bool def) {
    const char *v = obj_get(obj, key);
    if (!v) return def;
    if (*v == 't') return true;
    if (*v == 'f') return false;
    return def;
}

// NAN for both "absent" and "JSON null", because for a Setpoint bound those are
// the same fact: the server named no edge on that side. int_get folds them onto a
// caller-supplied default instead, which works for a percentage but not here -
// any float this could default to is a band edge the server never asked for.
static float float_get(const char *obj, const char *key) {
    const char *v = obj_get(obj, key);
    if (!v || *v == 'n') return NAN;
    return (float)atof(v);
}

static RxTone tone_of(const char *obj) {
    char t[8];
    if (!str_get(obj, "tone", t, sizeof(t))) return RX_TONE_INFO;
    if (!strcmp(t, "ok"))   return RX_TONE_OK;
    if (!strcmp(t, "warn")) return RX_TONE_WARN;
    return RX_TONE_INFO;
}

// JudgeEvid.tone stores this enum in a uint8_t so the chip array does not grow
// an alignment hole (see include/aijudge.h). That narrowing is only safe while
// the neutral is the zero value, because a rule row and a server that omits the
// key both leave a zeroed byte there. Asserted rather than commented: the two
// headers are edited independently.
static_assert(RX_TONE_INFO == 0, "JudgeEvid.tone's zero default must be neutral");
static_assert(RX_TONE_WARN < 256, "RxTone must fit the uint8_t in JudgeEvid");

static JudgeLevel level_of(const char *obj) {
    char t[8];
    if (!str_get(obj, "level", t, sizeof(t))) return JUDGE_OK;
    if (!strcmp(t, "alert")) return JUDGE_ALERT;
    if (!strcmp(t, "warn"))  return JUDGE_WARN;
    return JUDGE_OK;
}

// ---- HTTP -------------------------------------------------------------------

// HTTP/1.0, matching plantid.cpp's https_get and for the same reason: 1.0 has
// no chunked transfer encoding, so the body always arrives as plain bytes that
// the parser above can walk without a de-framing pass first.
static void write_request_head(WiFiClient &c, const char *path, const char *ctype,
                               size_t clen, const char *xdev, const char *xkind) {
    c.print("POST "); c.print(s_prefix); c.print(path); c.print(" HTTP/1.0\r\n");
    c.print("Host: "); c.print(s_host); c.print(":"); c.print(s_port); c.print("\r\n");
    c.print("User-Agent: SmartFarm-ESP32/1.0\r\n");
    c.print("Accept: application/json\r\n");
    c.print("Content-Type: "); c.print(ctype); c.print("\r\n");
    c.print("Content-Length: "); c.print(clen); c.print("\r\n");
    if (s_tok[0]) { c.print("Authorization: Bearer "); c.print(s_tok); c.print("\r\n"); }
    if (xdev)  { c.print("X-Device: "); c.print(xdev); c.print("\r\n"); }
    if (xkind) { c.print("X-Kind: ");   c.print(xkind); c.print("\r\n"); }
    c.print("Connection: close\r\n\r\n");
}

// Read until the socket closes, then strip the headers in place. Returns the
// HTTP status, 0 for a malformed reply, or -1 for a transport failure.
// `truncated` says the reply filled the buffer, which is indistinguishable from
// a body cut short and has to fail the same way.
static int read_reply(WiFiClient &c, char *out, size_t outsz,
                      uint32_t deadline_ms, bool *truncated) {
    size_t n = 0;
    uint32_t start = millis(), last = millis();
    *truncated = false;
    for (;;) {
        int a = c.available();
        if (a > 0) {
            size_t room = outsz - 1 - n;
            if (room == 0) { *truncated = true; break; }
            int rd = c.read((uint8_t *)out + n, (size_t)a < room ? (size_t)a : room);
            if (rd > 0) { n += (size_t)rd; last = millis(); }
        } else {
            if (!c.connected()) break;                    // close = reply complete
            if (millis() - start > deadline_ms) break;
            if (n > 0 && millis() - last > RESP_IDLE_MS) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    out[n] = '\0';
    if (n == 0) return -1;

    const char *sp = strchr(out, ' ');                    // "HTTP/1.1 NNN ..."
    int status = sp ? atoi(sp + 1) : 0;
    char *body = strstr(out, "\r\n\r\n");
    if (!body) return 0;
    body += 4;
    memmove(out, body, strlen(body) + 1);
    return status;
}

// ---- the frame upload -------------------------------------------------------

// POST one image to /v1/frame. The device pushes because the cameras are on a
// LAN the server cannot reach. Returns true on a 2xx.
static bool post_frame(const char *device, const char *kind, const char *ctype,
                       const uint8_t *body, size_t len, uint32_t deadline_ms) {
    WiFiClient c;
    if (!c.connect(s_host, s_port, CONNECT_MS)) return false;

    write_request_head(c, "/v1/frame", ctype, len, device, kind);
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > 1460) n = 1460;
        size_t w = c.write(body + sent, n);
        if (w == 0) { c.stop(); return false; }
        sent += w;
    }
    // No flush() here, and pointedly not the clear() the deprecation warning
    // suggests. NetworkClient.h documents `void flush(); // Print::flush tx` and
    // then implements it as `flush() { clear(); }`, and clear() empties the RX
    // buffer - so this line read as "make sure the request is out" while actually
    // discarding any reply bytes that had already arrived. On a LAN with a 77ms
    // round trip that is a race the panel keeps winning rather than a race it
    // cannot lose. Nothing is needed in its place: write() above pushes to the
    // socket synchronously and returns what it sent, which is the only completion
    // this path was ever asking about.

    bool trunc = false;
    int status = read_reply(c, s_resp, MAX_RESP, deadline_ms, &trunc);
    c.stop();
    return status >= 200 && status < 300;
}

// ---- the thermal frame, as a PNG --------------------------------------------
//
// server/app/brain.py asks the model for a thermal image and this file only ever
// sent the visible one, so the badge on the judgment card was decoration. The
// sink holds RGB565 and there is no JPEG encoder on the board - JPEGDEC decodes
// only - but a JPEG would be the wrong answer even with one: 32x24 of false
// colour through 4:2:0 chroma averages the palette over 16x12 blocks, and the
// palette is the entire message. PNG is lossless and at this size costs nothing:
// 2328 raw bytes fit one *stored* deflate block, so there is no compressor here,
// only a header, a CRC and an Adler.
static const size_t TH_ROW = 1 + (size_t)THERMAL_W * 3;     // filter byte + RGB888
static const size_t TH_RAW = (size_t)THERMAL_H * TH_ROW;    // what deflate carries
static const size_t TH_PNG = 8                              // signature
                           + 12 + 13                        // IHDR
                           + 12 + 2 + 5 + TH_RAW + 4        // IDAT: zlib, block, adler
                           + 12;                            // IEND
static_assert(TH_RAW <= 65535, "one stored deflate block caps at 65535 bytes");
static_assert(TH_PNG <= MAX_FRAME, "the thermal frame clears the same ceiling as RGB");

static uint8_t *png_be32(uint8_t *p, uint32_t v) {
    *p++ = (uint8_t)(v >> 24); *p++ = (uint8_t)(v >> 16);
    *p++ = (uint8_t)(v >> 8);  *p++ = (uint8_t)v;
    return p;
}

// Nibble-wise CRC-32: 64 bytes of table against 1KB for the byte-wide one, over
// 2.4KB of input once per diagnosis.
static uint32_t png_crc32(const uint8_t *p, size_t n) {
    static const uint32_t tab[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ tab[crc & 0x0F];
        crc = (crc >> 4) ^ tab[crc & 0x0F];
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t png_adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a += p[i]; if (a >= 65521u) a -= 65521u;   // one subtract is enough:
        b += a;    if (b >= 65521u) b -= 65521u;   // neither can reach 2*65521
    }
    return (b << 16) | a;
}

// Renders the newest thermal frame into a PNG, or NULL when the sink has never
// published one. Native 32x24, not the upscale the monitor page draws: the
// interpolation is there to fill a 374px panel, and sending it would hand the
// model spatial detail an MLX90640 never measured.
static const uint8_t *build_thermal_png(void) {
    static uint16_t rgb565[THERMAL_W * THERMAL_H];
    static uint8_t  png[TH_PNG];

    // peek, not take: the monitor page owns the fresh flag and consuming it here
    // would cost that page a frame it never got to draw.
    if (!thermal_peek_scaled(rgb565, THERMAL_W, THERMAL_H)) return nullptr;

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t *p = png;
    memcpy(p, sig, sizeof(sig)); p += sizeof(sig);

    uint8_t *ihdr = p;
    p = png_be32(p, 13);
    memcpy(p, "IHDR", 4); p += 4;
    p = png_be32(p, THERMAL_W);
    p = png_be32(p, THERMAL_H);
    *p++ = 8;                                  // bits per channel
    *p++ = 2;                                  // truecolour, no alpha
    *p++ = 0; *p++ = 0; *p++ = 0;              // deflate, adaptive filter, no interlace
    p = png_be32(p, png_crc32(ihdr + 4, 4 + 13));

    uint8_t *idat = p;
    p = png_be32(p, (uint32_t)(2 + 5 + TH_RAW + 4));
    memcpy(p, "IDAT", 4); p += 4;
    *p++ = 0x78; *p++ = 0x01;                  // zlib: 32K window, no preset dict
    *p++ = 0x01;                               // deflate: final block, stored
    *p++ = (uint8_t)TH_RAW;    *p++ = (uint8_t)(TH_RAW >> 8);
    *p++ = (uint8_t)(~TH_RAW); *p++ = (uint8_t)(~TH_RAW >> 8);

    uint8_t *raw = p;
    for (int y = 0; y < THERMAL_H; y++) {
        *p++ = 0;                              // filter: none
        const uint16_t *src = &rgb565[y * THERMAL_W];
        for (int x = 0; x < THERMAL_W; x++) {
            uint16_t c = src[x];
            // High bits replicated into the low ones, so full scale stays full
            // scale: a 5-bit 0x1F has to land on 0xFF and not 0xF8, or the hot
            // end of the palette arrives dimmer than the node drew it.
            *p++ = (uint8_t)(((c >> 8) & 0xF8) | ((c >> 13) & 0x07));
            *p++ = (uint8_t)(((c >> 3) & 0xFC) | ((c >> 9)  & 0x03));
            *p++ = (uint8_t)(((c << 3) & 0xF8) | ((c >> 2)  & 0x07));
        }
    }
    p = png_be32(p, png_adler32(raw, TH_RAW));
    p = png_be32(p, png_crc32(idat + 4, 4 + 2 + 5 + TH_RAW + 4));

    uint8_t *iend = p;
    p = png_be32(p, 0);
    memcpy(p, "IEND", 4); p += 4;
    png_be32(p, png_crc32(iend + 4, 4));       // lands exactly on png + TH_PNG
    return png;
}

// ---- what the server asked for ----------------------------------------------

// Send the frames the server asked for, if the puller has published one yet.
// Every exit path releases: the puller will not overwrite the keep buffer while
// `ready` holds, so an early return that skipped the release would freeze the
// tap and no later want_frame could ever be served.
static void service_frame_request(const char *device) {
    if (!s_want_frame) return;
    if (!s_frame_armed) {
        // Either the arm was never placed because the CAM was down when the
        // request arrived, or it timed out waiting. The server still wants a
        // picture, so ask the puller again the moment it has one to give.
        if (!camnet_live()) return;
        camnet_jpeg_request();
        s_frame_armed = true;
        s_frame_armed_ms = millis();
        return;
    }
    if (!camnet_jpeg_ready()) {
        if (millis() - s_frame_armed_ms > FRAME_WAIT_MS) s_frame_armed = false;
        return;
    }

    // One FRAME_DEADLINE_MS spans both uploads rather than one each. This runs
    // inside exchange(), which blocks loop(), and the telemetry POST and its own
    // 20s still have to fit after it.
    uint32_t t0 = millis();

    size_t len = 0;
    const uint8_t *jpeg = camnet_jpeg(&len);
    bool rgb_sent = false;
    if (jpeg && len > 0 && len <= MAX_FRAME) {
        rgb_sent = post_frame(device, "rgb", "image/jpeg", jpeg, len, FRAME_DEADLINE_MS);
        if (rgb_sent) { s_dbg_frame_len = len; s_want_frame = false; }
    }
    camnet_jpeg_release();
    s_frame_armed = false;

    // Thermal is strictly second. Everything RGB owns is already settled - sent,
    // scored, s_want_frame cleared, keep buffer handed back - so nothing below
    // can reach it, and the thermal result is dropped rather than retried. It has
    // to be after the release, too: the puller cannot publish while `ready`
    // holds, so a round trip inside that hold stalls the live camera for its
    // whole length. Gated on rgb_sent because main.py pairs the newest of each
    // kind, and a thermal frame posted on a cycle whose RGB never landed would
    // sit beside the previous cycle's picture and be read as one moment. A
    // stopped sink is not a failure - the model then gets the visible frame
    // alone, exactly what it got before any of this existed.
    if (!rgb_sent || !thermal_live()) return;
    uint32_t spent = millis() - t0;
    if (spent >= FRAME_DEADLINE_MS) return;
    const uint8_t *png = build_thermal_png();
    if (png) {
        post_frame(device, "thermal", "image/png", png, TH_PNG, FRAME_DEADLINE_MS - spent);
    }
}

// ---- parsing the prescription -----------------------------------------------

// Walk the whole body into the staging copies. False leaves staging garbage and
// the live prescription untouched, which is the entire point of staging.
static bool parse_prescription(const char *body, int *next_poll_s, bool *want_frame,
                               bool *update_mode, bool *firmware_pull) {
    const char *root = skip_ws(body);
    if (*root != '{') return false;
    if (!val_end(root)) return false;               // unbalanced = cut short

    // main.py's global exception handler answers 200 with {"rx_id":"error",
    // "next_poll_s":60} - a valid reply that is not a Prescription. Honour the
    // cadence it asks for, but treat the absence of `display` as the failure it
    // is rather than blanking three columns over a server-side bug.
    const char *disp = obj_get(root, "display");
    *next_poll_s = int_get(root, "next_poll_s", 60);
    if (!disp || *disp != '{') return false;
    *want_frame = bool_get(root, "want_frame", false);
    // Read exactly the way want_frame above is, and deliberately on the same side of the
    // `display` gate: the body main.py's catch-all answers a server-side bug with is valid JSON
    // and would parse this key just as happily, and a mode whose only exit is a reboot is not
    // something a reply this file has already decided is not a prescription gets to reach. An
    // older server never sends the key at all, and a client that does not know about it must
    // keep behaving as it did - so the default is the one that leaves the panel alone.
    *update_mode = bool_get(root, "update_mode", false);
    // The same key, one step further: update_mode asks the board to stand still and wait to be
    // pushed an image, firmware_pull asks it to go and fetch one. Read the same way and defaulted
    // the same way, and for the same reason - a server that predates the pull path never sends
    // this key, and the reading of its absence has to be the one that changes nothing.
    *firmware_pull = bool_get(root, "firmware_pull", false);

    char mode[12];
    str_get(root, "mode", mode, sizeof(mode));
    t_mode_auto = (mode[0] == '\0') || !strcmp(mode, "auto");
    str_get(root, "rx_id", t_rx_id, sizeof(t_rx_id));

    // FALSE as the default, not true: see plantrx_model_ready() in plantrx.h for
    // why an older server that omits the key has to understate itself here.
    t_model_ready = bool_get(disp, "model_ready", false);

    // control.setpoints. A band whose key is outside MetricKey is dropped rather
    // than stored under a name nothing asks for, and a band with neither bound is
    // dropped too - `have` has to mean "there is an edge here", or resolve_band()
    // in aijudge.cpp would take the server's side and then find nothing to compare
    // against. Reset every parse: a prescription that stops sending a band has
    // withdrawn it, and a stale band is the panel judging against a number the
    // server has moved on from.
    memset(t_band, 0, sizeof(t_band));
    const char *ctl = obj_get(root, "control");
    if (ctl && *ctl == '{') {
        for (const char *e = arr_first(obj_get(ctl, "setpoints")); e; e = arr_next(e)) {
            if (*e != '{') return false;
            char key[16];
            str_get(e, "key", key, sizeof(key));
            int idx = -1;
            for (int i = 0; i < BAND_N; i++) {
                if (!strcmp(key, BAND_METRICS[i])) { idx = i; break; }
            }
            if (idx < 0) continue;
            float lo = float_get(e, "lo"), hi = float_get(e, "hi");
            if (isnan(lo) && isnan(hi)) continue;
            // A band whose bounds cross cannot be held by anything, so it is refused
            // here as well as at the server that should never have sent it. Both ends
            // matter: the server gate stops one being written, this one stops an old
            // stored prescription replaying one, and a device that draws lo=30/hi=20
            // paints an amber tile that no reading in the greenhouse can clear. The
            // metric then falls to the compiled-in fallback in resolve_band(), which
            // is a band that at least admits a value.
            if (!isnan(lo) && !isnan(hi) && lo > hi) continue;
            // Last one wins, and duplicates are not an error worth failing the whole
            // prescription over: brain._setpoints builds this list from a dict, so a
            // repeat means the server changed shape, not that this reply is corrupt.
            t_band[idx].have = true;
            t_band[idx].lo = lo;
            t_band[idx].hi = hi;
        }
    }

    memset(t_plan, 0, sizeof(t_plan));
    memset(t_action, 0, sizeof(t_action));
    memset(t_judge, 0, sizeof(t_judge));
    memset(t_window, 0, sizeof(t_window));
    t_plan_n = t_action_n = t_judge_n = t_window_n = 0;
    t_window_span_s = t_window_covered_s = 0;
    t_notice[0] = t_species[0] = t_species_conf[0] = t_species_sci[0] = '\0';

    str_get(disp, "notice", t_notice, sizeof(t_notice));
    const char *sp = obj_get(disp, "species");
    if (sp && *sp == '{') {
        str_get(sp, "text", t_species, sizeof(t_species));
        // Both only alongside a name. A confidence with nothing to be confident
        // about is a number on the strip with no subject, and a binomial with no
        // name is the card drawing its own footnote; render.py returns no species
        // at all when the name is empty, so this pairing is the wire's already.
        if (t_species[0]) {
            str_get(sp, "conf_text", t_species_conf, sizeof(t_species_conf));
            str_get(sp, "sci", t_species_sci, sizeof(t_species_sci));
        }
    }

    for (const char *e = arr_first(obj_get(disp, "plan")); e; e = arr_next(e)) {
        if (t_plan_n >= PLANTRX_PLAN_MAX) break;
        if (*e != '{') return false;
        RxPlanRow *r = &t_plan[t_plan_n];
        str_get(e, "at",   r->at,   sizeof(r->at));
        str_get(e, "tag",  r->tag,  sizeof(r->tag));
        str_get(e, "head", r->head, sizeof(r->head));
        str_get(e, "cond", r->cond, sizeof(r->cond));
        r->tone = tone_of(e);
        t_plan_n++;
    }

    for (const char *e = arr_first(obj_get(disp, "actions")); e; e = arr_next(e)) {
        if (t_action_n >= PLANTRX_ACTION_MAX) break;
        if (*e != '{') return false;
        RxActionRow *r = &t_action[t_action_n];
        str_get(e, "at",    r->at,    sizeof(r->at));
        str_get(e, "tag",   r->tag,   sizeof(r->tag));
        str_get(e, "head",  r->head,  sizeof(r->head));
        str_get(e, "delta", r->delta, sizeof(r->delta));
        r->tone = tone_of(e);
        r->delta_is_reading = bool_get(e, "delta_is_reading", false);
        r->improved = bool_get(e, "improved", false);
        t_action_n++;
    }

    // The window table. Missing whole on a server with no summary to give - a
    // cold start, or a first hour that has not elapsed - and missing is the
    // same as empty here, exactly as it is for plan and actions: the loop never
    // runs, the count stays 0, and the commit below draws the table away rather
    // than leaving last hour's numbers sitting under this hour's readings.
    for (const char *e = arr_first(obj_get(disp, "window")); e; e = arr_next(e)) {
        if (t_window_n >= PLANTRX_WINDOW_ROWS_MAX) break;
        if (*e != '{') return false;
        RxWindowRow *r = &t_window[t_window_n];
        str_get(e, "label", r->label, sizeof(r->label));
        str_get(e, "stat",  r->stat,  sizeof(r->stat));
        str_get(e, "band",  r->band,  sizeof(r->band));
        // int_get answers the default for an absent key and for a JSON null
        // alike, which here is the same fact: no band, so nothing to tint from.
        // Clamped because a percentage out of range would tint a row a colour
        // the palette does not have, and -1 already means something else.
        r->in_band_pct = int_get(e, "in_band_pct", -1);
        if (r->in_band_pct < 0)   r->in_band_pct = -1;
        if (r->in_band_pct > 100) r->in_band_pct = 100;
        t_window_n++;
    }

    // Coverage above span is arithmetic that cannot be true, and a UI that has
    // to defend against it is a UI that will forget to. Clamped once, here.
    t_window_span_s    = int_get(disp, "window_span_s", 0);
    t_window_covered_s = int_get(disp, "window_covered_s", 0);
    if (t_window_span_s < 0)    t_window_span_s = 0;
    if (t_window_covered_s < 0) t_window_covered_s = 0;
    if (t_window_covered_s > t_window_span_s) t_window_covered_s = t_window_span_s;

    // The array the server sends is capped at JUDGE_ROWS_MAX = 1 and AIJUDGE_CAP
    // is 3, so the break below is defence and not a truncation the panel expects
    // to hit; it stays because a server that ships more rows than it promised must
    // not be able to walk off t_judge.
    for (const char *e = arr_first(obj_get(disp, "judgments")); e; e = arr_next(e)) {
        if (t_judge_n >= AIJUDGE_CAP) break;
        if (*e != '{') return false;
        JudgeRecord *r = &t_judge[t_judge_n];
        str_get(e, "at",   r->at,   sizeof(r->at));
        str_get(e, "head", r->head, sizeof(r->head));
        // The model's prose in full, and the reason MAX_RESP grew. str_copy decodes
        // \n into a real newline, which is the one control character schema.py lets
        // through and the one LVGL draws as a line break. An absent key leaves the
        // "" the memset already wrote, which is also what a JUDGE_RULE row carries.
        str_get(e, "body", r->body, sizeof(r->body));
        r->level = level_of(e);
        r->n_evid = 0;
        // JUDGE_CHIPS_MAX is 5 and so is AIJUDGE_EVID_MAX, so a full snapshot
        // arrives intact and the break is the same defence as the row cap above.
        for (const char *ch = arr_first(obj_get(e, "chips")); ch; ch = arr_next(ch)) {
            if (r->n_evid >= AIJUDGE_EVID_MAX) break;
            if (*ch != '{') return false;
            JudgeEvid *ev = &r->evid[r->n_evid];
            str_get(ch, "text", ev->text, sizeof(ev->text));
            ev->hot = bool_get(ch, "hot", false);
            // Same helper the plan and action rows use, so an omitted key lands
            // on the same neutral here as it does there - and it is the value the
            // memset above already wrote, so a chip cannot end up half-toned.
            ev->tone = (uint8_t)tone_of(ch);
            r->n_evid++;
        }
        // What the model was holding when it decided. bool_get's false default is
        // the honest reading of an absent key: a server that does not send these
        // is not claiming to have seen anything.
        r->saw_rgb = bool_get(e, "has_rgb", false);
        r->saw_thm = bool_get(e, "has_thermal", false);
        t_judge_n++;
    }

    // Turn. period_s of 0 is not a schedule (render.py's own rule), and the
    // remaining count is derived from the server's own two timestamps rather
    // than a wall clock - their difference is a duration, valid on a board
    // whose NTP never landed.
    const char *turn = obj_get(disp, "turn");
    t_turn_sched = false;
    t_turn_remain_ms = t_turn_period_ms = 0;
    if (turn && *turn == '{' && bool_get(turn, "scheduled", false)) {
        int period_s = int_get(turn, "period_s", 0);
        int next_ts  = int_get(turn, "next_ts", 0);
        int issued   = int_get(root, "issued_ts", 0);
        if (period_s > 0) {
            int remain = next_ts - issued;
            if (remain < 0) remain = 0;
            if (remain > period_s) remain = period_s;
            t_turn_sched = true;
            t_turn_remain_ms = (uint32_t)remain * 1000u;
            t_turn_period_ms = (uint32_t)period_s * 1000u;
        }
    }
    return true;
}

// True when staging differs from what the panel is showing. Rows are memset
// before being filled on both sides, so the padding a compiler inserts after
// the char arrays is deterministically zero and memcmp answers about content.
static bool staging_differs(void) {
    if (t_plan_n != s_plan_n || t_action_n != s_action_n) return true;
    if (t_window_n != s_window_n) return true;
    if (t_window_span_s != s_window_span_s ||
        t_window_covered_s != s_window_covered_s) return true;
    if (strcmp(t_notice, s_notice) || strcmp(t_species, s_species) ||
        strcmp(t_species_conf, s_species_conf) ||
        strcmp(t_species_sci, s_species_sci)) return true;
    if (memcmp(t_plan, s_plan, sizeof(RxPlanRow) * (size_t)t_plan_n)) return true;
    if (memcmp(t_action, s_action, sizeof(RxActionRow) * (size_t)t_action_n)) return true;
    if (memcmp(t_window, s_window, sizeof(RxWindowRow) * (size_t)t_window_n)) return true;
    return false;
}

// Whether an rx_id names an actual judgment. "none" is the placeholder
// _empty_prescription carries (server/app/main.py): a well-formed 200 with an
// empty display, sent when the model has never run for this device or is not
// configured at all. That is the server saying it has nothing, not the server
// having decided something, and telling the two apart is the entire question -
// so the literal lives here once, and the UI asks through plantrx_rx_real()
// instead of carrying its own copy of the protocol's placeholder.
//
// "none" is the only placeholder this has to know about, and that is an
// invariant rather than an accident. main.py's global exception handler also
// answers 200 with an id - {"rx_id":"error"} - but parse_prescription bails on
// the missing `display` at :618, three lines before it would read rx_id at
// :624, so commit() never runs and "error" cannot reach s_rx_id. If that guard
// is ever relaxed, fix it there: an error body is a failed exchange and must
// stay one, not a third magic string accreted onto this predicate.
static inline bool rx_is_real(const char *id) {
    return id[0] != '\0' && strcmp(id, "none") != 0;
}

// Commit staging and hand the judgment rows to the ring. Called with the LVGL
// lock held: these rows and the ring are read by the UI thread.
static void commit(void) {
    s_mode_auto = t_mode_auto;
    // Beside s_mode_auto and outside staging_differs() for the same reason: these
    // are facts about the reply, not about the rows. A reply that repeats every
    // display string while withdrawing a band has changed what the panel is
    // allowed to claim, and gating that on the row diff would leave the tiles
    // judging against a band the server no longer holds.
    memcpy(s_band, t_band, sizeof(s_band));
    s_model_ready = t_model_ready;
    // The content clock hangs off rx_id and off nothing else, and it is updated
    // here - outside the staging_differs() test, beside s_mode_auto - because
    // both of these are facts about the reply rather than about the rows.
    //
    // Every other place it could go is wrong, each in its own way. Inside the
    // staging_differs() branch: a server that re-runs its model and reaches the
    // same wording has issued a fresh judgment with a new id and identical rows,
    // and the clock would never move for it. Beside s_last_ok_ms in exchange():
    // that is s_last_ok_ms again, which is the bug being fixed - main.py replays
    // the previous prescription verbatim when the model throws, so the exchange
    // succeeds and the transport clock resets while nothing was decided. And
    // this is the only point in the file where the previous id is still
    // readable, which is what makes "changed" answerable at all.
    if (strcmp(s_rx_id, t_rx_id) != 0) {
        snprintf(s_rx_id, sizeof(s_rx_id), "%s", t_rx_id);
        s_rx_changed_ms = millis();
    }
    if (staging_differs()) {
        memcpy(s_plan, t_plan, sizeof(s_plan));
        memcpy(s_action, t_action, sizeof(s_action));
        memcpy(s_window, t_window, sizeof(s_window));
        s_plan_n = t_plan_n;
        s_action_n = t_action_n;
        s_window_n = t_window_n;
        s_window_span_s = t_window_span_s;
        s_window_covered_s = t_window_covered_s;
        snprintf(s_notice, sizeof(s_notice), "%s", t_notice);
        snprintf(s_species, sizeof(s_species), "%s", t_species);
        snprintf(s_species_conf, sizeof(s_species_conf), "%s", t_species_conf);
        snprintf(s_species_sci, sizeof(s_species_sci), "%s", t_species_sci);
        s_rev++;
        s_dbg_changed = true;
    } else {
        s_dbg_changed = false;
    }

    // display.judgments is newest-first and the ring is append-only, so they go
    // in backwards: appending oldest-first is what leaves aijudge_at(0) holding
    // the newest row.
    s_dbg_appended = 0;
    for (int i = t_judge_n - 1; i >= 0; i--) {
        if (aijudge_append_llm(&t_judge[i])) s_dbg_appended++;
    }

    // Read off the staging copy and not the ring: an unchanged finding is not
    // appended, and the serial line is answering what this reply said rather
    // than what the log already held.
    if (t_judge_n > 0) {
        const JudgeRecord *n = &t_judge[0];
        s_dbg_saw[0] = n->saw_rgb ? 'r' : '-';
        s_dbg_saw[1] = n->saw_thm ? 't' : '-';
        s_dbg_saw[2] = '\0';
        uint8_t k = 0;
        for (; k < n->n_evid && k < AIJUDGE_EVID_MAX; k++) {
            s_dbg_tone[k] = n->evid[k].tone == RX_TONE_OK   ? 'o'
                          : n->evid[k].tone == RX_TONE_WARN ? 'w' : 'i';
        }
        if (k == 0) s_dbg_tone[k++] = '-';
        s_dbg_tone[k] = '\0';
    } else {
        snprintf(s_dbg_saw, sizeof(s_dbg_saw), "?");
        snprintf(s_dbg_tone, sizeof(s_dbg_tone), "-");
    }

    if (t_turn_sched) aijudge_set_server_turn(t_turn_remain_ms, t_turn_period_ms);
    else              aijudge_clear_server_turn();
}

// ---- the exchange -----------------------------------------------------------

static void schedule_ok(int next_poll_s) {
    uint32_t ms = (uint32_t)(next_poll_s > 0 ? next_poll_s : 60) * 1000u;
    if (ms < POLL_MIN_MS) ms = POLL_MIN_MS;
    if (ms > POLL_MAX_MS) ms = POLL_MAX_MS;
    s_next_ms = millis() + ms;
}

// The tags schedule_fail() is called with, in the panel's words. One table
// rather than a second classification at the getter: a tag that gained a case
// in one place and not the other would put a stale reason on the wall while
// the serial line said something else. Each string is under 16 bytes so the
// settings page can drop it into a one-line slot without measuring it.
static const char *why_ko(const char *why) {
    if (!strcmp(why, "connect"))      return "연결 실패";
    if (!strcmp(why, "no-reply"))     return "응답 없음";
    if (!strcmp(why, "http"))         return "서버 오류";
    if (!strcmp(why, "truncated"))    return "응답 잘림";
    if (!strcmp(why, "malformed"))    return "형식 오류";
    if (!strcmp(why, "req-overflow")) return "요청 초과";
    return "알 수 없음";
}

// A peer that answered gets to set the cadence even when its answer was
// unusable; a peer that did not answer at all gets the backoff, because there
// is nobody there to ask.
static void schedule_fail(int server_poll_s, const char *why) {
    s_fails++;
    s_last_failed = true;
    s_dbg_why = why;
    s_last_err = why_ko(why);
    if (server_poll_s > 0) { schedule_ok(server_poll_s); return; }
    uint32_t ms = FAIL_BASE_MS << (s_fails > 5 ? 5 : s_fails - 1);
    if (ms > FAIL_MAX_MS) ms = FAIL_MAX_MS;
    s_next_ms = millis() + ms;
}

static bool build_request(const char *device) {
    Buf b = { s_req, MAX_REQ, 0 };
    badd(&b, "{");
    badd_str(&b, "device", device);
    badd(&b, "\"uptime_ms\":%lu,", (unsigned long)millis());

    badd(&b, "\"sensors\":{");
    badd_sensor(&b, "co2_ppm",    sensornode_co2(),  0);
    badd_sensor(&b, "temp_c",     sensornode_temp(), 1);
    badd_sensor(&b, "rh_pct",     sensornode_hum(),  1);
    badd_sensor(&b, "lux",        sensornode_lux(),  0);
    badd_sensor(&b, "soil_pct",   sensornode_soil(), 0);
    badd_sensor(&b, "leaf_max_c", thermal_max(),     1);
    bend(&b, '}');
    badd(&b, ",");

    badd(&b, "\"links\":{");
    badd(&b, "\"node_online\":%s,", sensornode_online() ? "true" : "false");
    badd(&b, "\"node_age_ms\":%lu,", (unsigned long)sensornode_age_ms());
    badd(&b, "\"node_lost\":%lu,", (unsigned long)sensornode_lost());
    badd(&b, "\"cam_online\":%s,", camprov_cam_online() ? "true" : "false");
    badd(&b, "\"rgb_live\":%s,", camnet_live() ? "true" : "false");
    badd(&b, "\"thermal_live\":%s,", thermal_live() ? "true" : "false");
    badd(&b, "\"thermal_fps\":%.1f,", thermal_fps());
    badd(&b, "\"wifi_rssi\":%d,", net_rssi());
    bend(&b, '}');
    badd(&b, ",");

    // WHY THE DEVICE REPORTS ITS OWN RESTARTS.
    //
    // uptime_ms already told the server how long this boot has lasted, and that was enough to
    // see a board rebooting - but not to see WHY, and the difference matters. A panel whose
    // display sits on UART0's pins has no console to ask, and a crash loop looked exactly
    // like a quiet network from out here. Hours went into that once.
    //
    // reset says what ended the last run in one word a human reads without a header file;
    // crashes counts panics across power cycles, out of NVS, so a board that crashes and
    // recovers still leaves a trail; image_pending says the bootloader has not accepted this
    // firmware yet, which means a restart would take it back.
    badd(&b, "\"boot\":{");
    badd(&b, "\"reset\":\"%s\",", health_reset_name());
    badd(&b, "\"crashes\":%lu,", (unsigned long)health_crashes());
    badd(&b, "\"image_pending\":%s", health_image_pending() ? "true" : "false");
    // The panic handler's own record, when there is one. Only ever present after a crash the
    // server has not yet been told about, so its absence is not a claim that nothing crashed -
    // `reset` and `crashes` answer that. What this adds is WHERE, which is the part no amount
    // of watching telemetry from outside could ever recover.
    if (health_have_crash()) {
        badd(&b, ",\"crash_task\":\"%s\"", health_crash_task());
        badd(&b, ",\"crash_pc\":\"%s\"", health_crash_pc());
        badd(&b, ",\"crash_bt\":\"%s\"", health_crash_bt());
    }
    bend(&b, '}');
    badd(&b, ",");

    // The device identified this plant itself, with the user holding the panel
    // in front of it; the server has no camera on this pot and must not re-run
    // PlantNet against a frame that may not even show the same plant. Omitted
    // whole in every other PlantIdState - not {}, not null - because an empty
    // object reads as "identification ran and came back with nothing", which is
    // a different fact from "the button has not been pressed". `text` is the
    // string the panel is already showing, korean before common before the
    // scientific name, so the server displays what the wall displays rather
    // than re-deriving a second answer from the same three fields.
    if (plantid_state() == PLANTID_OK && plantid_species()[0]) {
        const char *sci  = plantid_species();
        const char *text = plantid_korean();
        if (!text[0]) text = plantid_common();
        if (!text[0]) text = sci;
        // Floored at 1, not 0: render.py reads a 0 as "no confidence reported"
        // and draws no percentage at all, so a real identification that scored
        // under 0.5% would round down into looking like it never happened.
        int pct = (int)(plantid_score() * 100.0f + 0.5f);
        if (pct < 1)   pct = 1;
        if (pct > 100) pct = 100;
        badd(&b, "\"species\":{");
        badd_str(&b, "sci", sci);      // PlantNet authored these, not this board:
        badd_str(&b, "text", text);    // a quote in one would end the body early
        badd(&b, "\"conf_pct\":%d,", pct);
        bend(&b, '}');
        badd(&b, ",");
    }

    // Two objects, and conflating them is the bug this exists to avoid.
    //
    // `actuators` is measured hardware. There is no relay, no driver, nothing
    // that could report what a device actually did, so it is empty and stays
    // empty. An invented 0 here would read as "the mister is off" and the
    // server would reason about an actuator that is not on the board at all -
    // and render.py's 조치 column would print a deviation it never measured.
    //
    // `actuator_intent` is what the user's switches on the 제어 page say. That
    // is real panel state and the model needs it, but it is a request, not an
    // observation. Folding it into `actuators` would turn the honest
    // "실행 기록 없음" into a false measurement.
    //
    // Always all seven keys, iterated through ui.h's accessors so no device
    // name is spelled in this file: the key set is the device's declared
    // inventory, and a missing key would have to mean "no such device" rather
    // than "off".
    badd(&b, "\"actuators\":{},");
    badd(&b, "\"actuator_intent\":{");
    for (int i = 0, n = ui_actuator_count(); i < n; i++) {
        badd(&b, "\"%s\":%d,", ui_actuator_name(i), ui_actuator_level(i));
    }
    bend(&b, '}');
    badd(&b, ",");
    badd(&b, "\"auto\":%s,", ui_auto_control() ? "true" : "false");
    badd(&b, "\"ask_now\":%s,", s_ask_now ? "true" : "false");
    // Movement between polls, which actuator_intent above cannot carry: it is one
    // sample and a switch that went on and off again reads the same in two of them.
    // See ui.h for why these are counts and not flags.
    badd(&b, "\"edges\":%lu,", (unsigned long)ui_switch_edges());
    badd(&b, "\"allstops\":%lu,", (unsigned long)ui_allstop_count());
    // rx_id last, and bend() rather than a bare '}': the server reads this one -
    // it is how main.py tells a device running the current prescription from one
    // still on an older id - and it is conditional, so whichever member ends up
    // final must not leave its comma behind. A "fw" string used to sit here as an
    // unconditional terminator and hid that; the server never read it, so it was
    // a version stamp nothing could act on, and uptime_ms going backwards beside
    // an rx_id reverting to "none" already witnesses the one event it could have.
    if (s_rx_id[0]) badd_str(&b, "rx_id", s_rx_id);
    bend(&b, '}');
    return !boverflow(&b);
}

static void exchange(void) {
    char device[20];
    net_mac(device, sizeof(device));
    s_seq++;                            // one debug line per attempt, failures included
    // s_dbg_status and s_dbg_rtt are deliberately not cleared here. The
    // settings page reads them from the UI task while this function blocks for
    // up to a 20s round trip, and a zeroed status would read as "never
    // attempted" for the whole of an exchange that is in fact in flight. Every
    // path below sets both before it returns.
    s_dbg_appended = 0;
    s_dbg_changed = false;

    // The frame the server asked for goes first: main.py stores it, then
    // diagnoses on the poll that follows, so uploading after the telemetry POST
    // would cost a whole extra cycle before the model ever sees the picture.
    s_dbg_frame_len = 0;
    service_frame_request(device);

    // Nothing left the device, so there is no status and no round trip to
    // report: 0/0 is the honest pair for an attempt that never reached the wire.
    if (!build_request(device)) {
        s_dbg_status = 0;
        s_dbg_rtt = 0;
        schedule_fail(0, "req-overflow");
        return;
    }

    uint32_t t0 = millis();
    WiFiClient c;
    // No setTimeout(): connect() takes its own millisecond timeout and the read
    // loop below owns a deadline of its own, so Stream's timeout gates nothing
    // here. The neighbouring files set it in seconds against a millisecond API;
    // copying that would just plant the same dead line in a third place.
    if (!c.connect(s_host, s_port, CONNECT_MS)) {
        s_dbg_status = -1;
        s_dbg_rtt = millis() - t0;
        net_note_uplink_fail();  // LAN TCP connect timed out; feeds net_poll's watchdog
        schedule_fail(0, "connect");
        return;
    }
    write_request_head(c, "/v1/telemetry", "application/json", strlen(s_req), nullptr, nullptr);
    c.print(s_req);
    // No flush(): it discards the RX buffer despite its name and its own header
    // comment - see the note in post_frame(). c.print() has already written the
    // body.

    bool trunc = false;
    int status = read_reply(c, s_resp, MAX_RESP, RESP_DEADLINE_MS, &trunc);
    c.stop();
    s_dbg_status = status;
    if (status > 0) net_note_uplink();  // a reply came back - the uplink is alive
    s_dbg_rtt = millis() - t0;

    if (status != 200) { schedule_fail(0, status < 0 ? "no-reply" : "http"); return; }
    if (trunc)         { schedule_fail(0, "truncated"); return; }

    int next_poll_s = 0;
    bool want_frame = false;
    bool update_mode = false;
    bool firmware_pull = false;
    if (!parse_prescription(s_resp, &next_poll_s, &want_frame, &update_mode, &firmware_pull)) {
        schedule_fail(next_poll_s, "malformed");
        return;
    }

    lvgl_port_lock(-1);
    commit();
    lvgl_port_unlock();

    // Cleared only now: the request that carried it was answered, so a press
    // during the round trip is still pending and rides the next one.
    s_ask_now = false;
    s_fails = 0;
    s_last_failed = false;
    s_have_ok = true;
    s_last_ok_ms = millis();
    s_dbg_why = "ok";
    s_last_err = "";
    s_exchanges++;

    // The crash record rode this request and the server answered 200, so the copy in flash
    // has done its job and the partition is free for the next one. Deliberately here and not
    // at boot: see health_crash_reported().
    health_crash_reported();
    schedule_ok(next_poll_s);

    // The server has asked for the board. Entered down here rather than the moment the flag was
    // parsed, because everything above is what makes this exchange accounted for - the
    // prescription committed, the crash record released, the cadence set - and a mode that ends
    // in a reboot must not leave any of that half-written behind it.
    //
    // This runs once per entry and not once per poll, which matters more than it looks:
    // updatemode_enter() re-arms its five-minute deadline on every call, so a flag the server
    // kept re-sending would push that deadline out forever and the timeout that exists to
    // rescue a board nobody ever uploads to would never fire. Two independent things already
    // guarantee the single call - main.cpp stops calling plantrx_poll() the instant
    // updatemode_active() goes true, so there is no second poll to re-enter from, and the
    // server clears the flag as it puts it on the wire (store.take_update_mode) so a poll that
    // did somehow happen would not see it again. Neither side is leaning on the other to be the
    // one that remembers, because the failure of that arrangement is silent: the panel would sit
    // in a takeover screen that never times out, and nothing on it would say why.
    //
    // The pull wins when the server asks for both, and it does not fall through to the plain
    // mode afterwards: fwpull_request() enters update mode itself, so taking this branch loses
    // nothing the other one would have done, while taking the other one first would only cost a
    // duplicate entry and a second reason line on the veil. The server can legitimately send
    // both - "update_mode=1&pull=1" is one operator action - and the honest reading of that pair
    // is the more specific of the two, not both in sequence.
    if (firmware_pull) {
        fwpull_request("서버");
        return;   // same reason as the update_mode arm below: nothing will publish a frame now
    }
    if (update_mode) {
        updatemode_enter("서버");
        // Returning before the frame arm below, not after. The puller stands down as soon as
        // updatemode_active() goes true, so a request placed now is one nothing will ever
        // publish against - it would just leave s_frame_armed set for the few minutes this
        // board has left.
        return;
    }

    // Arm for the next exchange rather than waiting here: the puller publishes
    // on its own task and blocking loop() on it would stall the UI's own work
    // for a frame period on top of the round trip we just paid for.
    s_want_frame = want_frame;
    if (want_frame && !s_frame_armed && camnet_live()) {
        camnet_jpeg_request();
        s_frame_armed = true;
        s_frame_armed_ms = millis();
    }
}

// ---- public API -------------------------------------------------------------

// Split "http://host:port/prefix" into its parts. Anything that is not http://
// with a host reads as unconfigured: a base URL nobody can parse is a typo, and
// guessing at it would point the panel at a server that does not exist.
static bool parse_base_url(const char *url) {
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return false;
    p += 7;
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < sizeof(s_host)) s_host[i++] = *p++;
    s_host[i] = '\0';
    if (!s_host[0]) return false;
    while (*p && *p != ':' && *p != '/') p++;          // an over-long host is a typo
    s_port = 80;
    if (*p == ':') {
        int port = atoi(p + 1);
        if (port <= 0 || port > 65535) return false;
        s_port = (uint16_t)port;
        while (*p && *p != '/') p++;
    }
    i = 0;
    while (*p && i + 1 < sizeof(s_prefix)) s_prefix[i++] = *p++;
    s_prefix[i] = '\0';
    size_t n = strlen(s_prefix);                        // "…:8000/" would double the slash
    if (n && s_prefix[n - 1] == '/') s_prefix[n - 1] = '\0';
    return true;
}

void plantrx_init(void) {
    s_host[0] = s_prefix[0] = s_host_disp[0] = '\0';
    s_notice[0] = s_species[0] = s_species_conf[0] = s_species_sci[0] = '\0';
    s_rx_id[0] = '\0';
    memset(s_plan, 0, sizeof(s_plan));
    memset(s_action, 0, sizeof(s_action));
    memset(s_window, 0, sizeof(s_window));

    const char *base = sitecfg_base_url();
    s_tok = sitecfg_token();

    if (base[0] == '\0') {
        hlogf("[plantrx] no server configured; rule engine only" "\n");
        return;
    }
    if (!parse_base_url(base)) {
        hlogf("[plantrx] base URL is not http://host[:port][/prefix]; uplink off" "\n");
        return;
    }

    // Allocated once for the life of the firmware. A per-poll allocation on a
    // board that also holds the JPEG decode buffers is how PSRAM fragments over
    // the months a wall panel actually stays up.
    s_req  = (char *)heap_caps_malloc(MAX_REQ, MALLOC_CAP_SPIRAM);
    s_resp = (char *)heap_caps_malloc(MAX_RESP, MALLOC_CAP_SPIRAM);
    if (!s_req || !s_resp) {
        if (s_req) { heap_caps_free(s_req); s_req = nullptr; }
        if (s_resp) { heap_caps_free(s_resp); s_resp = nullptr; }
        hlogf("[plantrx] PSRAM alloc failed; uplink off" "\n");
        return;
    }

    s_configured = true;
    snprintf(s_host_disp, sizeof(s_host_disp), "%s:%u", s_host, (unsigned)s_port);
    // Not immediately: the radio has not associated at boot, and a first poll
    // that fails on a link that was simply not up yet starts the backoff from
    // one for no reason.
    s_next_ms = millis() + 3000;
    hlogf("[plantrx] server=%s%s\n", s_host_disp, s_prefix);
}

void plantrx_poll(void) {
    if (!s_configured) return;

    if (net_state() != NET_CONNECTED) {
        // The rule owns the countdown while the uplink is down. Waiting for the
        // server turn to expire on its own would leave the column counting
        // toward an event nobody is going to run.
        if (s_link_was_up) {
            s_link_was_up = false;
            lvgl_port_lock(-1);
            aijudge_clear_server_turn();
            lvgl_port_unlock();
        }
        // Nothing was attempted, so nothing here is a failure: the backoff, the
        // deadline and RxLink all stay exactly where the last real exchange left
        // them, and a link that comes back after a long outage finds a deadline
        // already in the past and polls at once.
        return;
    }
    s_link_was_up = true;

    if ((int32_t)(millis() - s_next_ms) < 0) return;
    exchange();
}

bool plantrx_configured(void) { return s_configured; }

RxLink plantrx_link(void) {
    if (!s_configured) return RX_OFF;
    // ERROR outranks STALE: both can be true after a server goes away, and "the
    // last attempt failed" is the one a grower can act on.
    if (s_last_failed) return RX_ERROR;
    if (!s_have_ok) return RX_WAITING;
    return (millis() - s_last_ok_ms > STALE_AFTER_MS) ? RX_STALE : RX_OK;
}

int32_t plantrx_age_s(void) {
    if (!s_have_ok) return -1;
    return (int32_t)((millis() - s_last_ok_ms) / 1000u);
}

// Seconds since the server last minted a NEW rx_id, or -1 when the current id is
// not a real judgment - "" before any reply, "none" for the placeholder. -1 and
// not 0 for "none" on purpose: the placeholder arriving is a change of id, and
// reporting that as "decided 0 seconds ago" would be the same lie in a new place.
//
// Not plantrx_age_s(), and the pair is what makes the panel honest. That one is
// the age of the last successful exchange; this is the age of the last actual
// judgment. When _run_brain() fails, main.py answers with the previous
// prescription verbatim, so the transport age resets to zero on a model that has
// stopped answering and only this number keeps climbing.
int32_t plantrx_content_age_s(void) {
    if (!rx_is_real(s_rx_id)) return -1;
    return (int32_t)((millis() - s_rx_changed_ms) / 1000u);
}

const char *plantrx_rx_id(void) { return s_rx_id; }

// Whether the panel is holding a judgment at all rather than a placeholder. The
// AI-RX conflict chip gates on this: mode="advisory" is hard-coded into
// _empty_prescription, so a never-diagnosed device whose polls all succeed was
// drawing a conflict against a value the server had never decided.
bool plantrx_rx_real(void) { return rx_is_real(s_rx_id); }

bool plantrx_mode_auto(void) { return s_mode_auto; }
bool plantrx_model_ready(void) { return s_model_ready; }

bool plantrx_band(const char *metric, float *lo, float *hi) {
    if (!metric) return false;
    for (int i = 0; i < BAND_N; i++) {
        if (strcmp(metric, BAND_METRICS[i]) != 0) continue;
        if (!s_band[i].have) return false;
        // Written only on the true path. A caller that seeded NAN and got false
        // still holds its NANs; one that seeded its own defaults still holds those,
        // which is what lets resolve_band() in aijudge.cpp take the fallback branch
        // without a second copy of the defaults.
        if (lo) *lo = s_band[i].lo;
        if (hi) *hi = s_band[i].hi;
        return true;
    }
    return false;
}
const char *plantrx_notice(void) { return s_notice; }
const char *plantrx_species(void) { return s_species; }
const char *plantrx_species_conf(void) { return s_species_conf; }
const char *plantrx_species_sci(void) { return s_species_sci; }

int plantrx_plan_count(void) { return s_plan_n; }
const RxPlanRow *plantrx_plan_at(int i) {
    return (i >= 0 && i < s_plan_n) ? &s_plan[i] : nullptr;
}
int plantrx_action_count(void) { return s_action_n; }
const RxActionRow *plantrx_action_at(int i) {
    return (i >= 0 && i < s_action_n) ? &s_action[i] : nullptr;
}
int plantrx_window_count(void) { return s_window_n; }
const RxWindowRow *plantrx_window_at(int i) {
    return (i >= 0 && i < s_window_n) ? &s_window[i] : nullptr;
}
int plantrx_window_span_s(void) { return s_window_span_s; }
int plantrx_window_covered_s(void) { return s_window_covered_s; }

uint32_t plantrx_revision(void) { return s_rev; }

void plantrx_ask_now(void) {
    if (!s_configured) return;
    s_ask_now = true;
    // Pull the next poll forward. The button says 지금, and waiting out a 60s
    // idle cadence before the request even leaves would read as a dead button -
    // the server still owns the decision once it arrives.
    s_next_ms = millis();
}

void plantrx_debug_tick(void) {
    if (!s_configured) return;
    if (s_seq == s_dbg_seq) return;
    s_dbg_seq = s_seq;

    static const char *LINK[] = { "OFF", "WAITING", "OK", "STALE", "ERROR" };
    uint32_t wait_ms = (int32_t)(s_next_ms - millis()) > 0 ? s_next_ms - millis() : 0;
    // Which metrics the reply banded, as one character each in BAND_METRICS order
    // (v/a/h/c/d for vpd_kpa, air_c, rh_pct, co2_ppm, leaf_air_dt_c; '-' for a
    // metric the server said nothing about). Without this the round's central
    // claim - that the tiles and the rule now judge against the server's numbers -
    // is unobservable on hardware: a tile tinted from a server band and one tinted
    // from the panel's fallback differ only by the colour of a 12px title.
    char bands[BAND_N + 1];
    static const char BAND_TAG[] = "vahcd";
    for (int i = 0; i < BAND_N; i++) bands[i] = s_band[i].have ? BAND_TAG[i] : '-';
    bands[BAND_N] = '\0';

    hlogf("[plantrx] %s http=%d rtt=%lums why=%s rev=%lu%s rows=%d/%d "
                  "win=%d cov=%d/%ds judge+%d saw=%s tone=%s conf=%s sci=%s "
                  "frame=%luB next=%lus age=%lds cage=%lds thm=%s band=%s model=%s\n",
                  LINK[(int)plantrx_link()], s_dbg_status,
                  (unsigned long)s_dbg_rtt, s_dbg_why,
                  (unsigned long)s_rev, s_dbg_changed ? "*" : "",
                  s_plan_n, s_action_n,
                  s_window_n, s_window_covered_s, s_window_span_s,
                  s_dbg_appended, s_dbg_saw, s_dbg_tone,
                  s_species_conf[0] ? s_species_conf : "-",
                  s_species_sci[0] ? s_species_sci : "-",
                  (unsigned long)s_dbg_frame_len,
                  (unsigned long)(wait_ms / 1000u), (long)plantrx_age_s(),
                  // The two ages side by side. cage climbing while age stays low
                  // is a dead model behind a healthy link; cage=-1 is a panel
                  // holding no judgment at all. thm is the gate thermal_max()
                  // now applies, so leaf_max_c going null on the wire has a
                  // visible cause on the same line.
                  (long)plantrx_content_age_s(), thermal_live() ? "live" : "off",
                  bands, s_model_ready ? "yes" : "no");
}

// ---- diagnosing a dead uplink -----------------------------------------------
//
// Every one of these reads state the exchange already keeps for the serial
// line. Nothing here is computed on demand, so the settings page can poll the
// whole block on a UI timer without costing the poll path anything.

const char *plantrx_host(void) { return s_host_disp; }
int         plantrx_last_status(void) { return s_dbg_status; }
uint32_t    plantrx_last_rtt_ms(void) { return s_dbg_rtt; }
const char *plantrx_last_error(void) { return s_last_err; }
uint32_t    plantrx_exchanges(void) { return s_exchanges; }
uint32_t    plantrx_failures(void) { return s_fails; }

// ---- the address, lent out -------------------------------------------------
//
// Not diagnostics: this is the parsed base URL, and it is public only because
// plantid.cpp's identify POST goes to the same server on its own connection.
// Returning the statics directly is safe for the same reason the block above
// is - plantrx_init() writes them once and nothing writes them again, so a
// borrower on another task cannot catch them half-updated.

const char *plantrx_srv_host(void) { return s_host; }
uint16_t    plantrx_srv_port(void) { return s_port; }
const char *plantrx_srv_prefix(void) { return s_prefix; }
