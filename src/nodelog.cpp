// The node console, declared in include/nodelog.h: a short ring here, and a POST to the server.
//
// Structure, and why it is this shape:
//
// TWO TASKS TOUCH THIS, AND ONLY ONE OF THEM MAY BLOCK. nodelog_add() runs inside camprov's
// ESP-NOW receive callback on the WiFi task and nodelog_tick() runs on loop(). So the ring is
// guarded the way hlog.cpp guards its own - a portMUX critical section around a bounded memcpy -
// and the socket work happens nowhere near the callback. A TCP write inside an ESP-NOW recv
// handler would hold the WiFi task for a connect timeout, which stalls the very radio the reply
// has to go out on and drops every other frame on the air for four seconds. That is not a latency
// regression, it is the CAM's stream and the sensor node's telemetry going dark because a log line
// arrived.
//
// THE CURSOR IS ABSOLUTE, NOT AN INDEX. s_write counts lines ever added and s_sent counts lines
// ever forwarded, both monotonic. Every question this file answers - how many lines are held, is
// there anything to send, was a line lost before it got out - is a subtraction of those two, so
// there is one source of truth instead of three that can disagree about where the ring's head is.
//
// THE CURSOR ADVANCES ONLY ON A 2xx. A POST that failed took the window with it; rewinding is
// what makes the next tick send the same lines again rather than the server's copy of the log
// silently growing a hole. Holes still happen - a node talking faster than one POST per two
// seconds can drain wraps the ring - and the count is logged with every POST, because a log with
// unmarked gaps in it is worse than one that admits them.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdlib.h>          // atoi, for the status line
#include <string.h>
#include "nodeproto.h"
#include "srvurl.h"    // the server address, parsed once for all three firmwares
#include "nodelog.h"
#include "plantrx.h"
#include "updatemode.h"
#include "net.h"
#include "sitecfg.h"
#include "hlog.h"

// ---- the ring ---------------------------------------------------------------------------------

struct LogLine {
    uint32_t ms;                  // millis() at arrival, so age is a subtraction at read time
    uint8_t  role;                // NodeRole - which board said it
    char     text[NODELOG_TEXT];
};

// Internal DRAM on purpose, unlike hlog's 16KB ring which lives in PSRAM. Two reasons: 3.4KB is
// the same order as one LVGL card and does not move the needle on the pool the framebuffer
// competes for, and this array is memcpy'd inside a critical section from the WiFi task - PSRAM
// takes the cache miss path with interrupts off, which is exactly where a stall is least welcome.
static LogLine s_ring[NODELOG_LINES];

// Both monotonic, both read from tasks that do not write them, so both volatile for the reason
// aijudge.cpp gives at its own s_newest: the publish at the end of an append must not be hoisted
// above the record it publishes, and a cursor the compiler parked in a register would freeze the
// UI's count. The ring entries themselves are only ever consistent under s_mux.
static volatile uint32_t s_write = 0;    // lines ever added
static volatile uint32_t s_sent = 0;     // lines ever forwarded to the server
static volatile uint32_t s_dropped = 0;  // lines overwritten before they were forwarded

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void nodelog_add(uint8_t role, const char *line) {
    if (line == NULL) return;

    // Both computed before the critical section, so what runs with interrupts off is one bounded
    // memcpy and three integer updates - the same discipline as hlog's ring_put(), which formats
    // outside and copies inside.
    uint32_t now = millis();
    size_t n = strnlen(line, NODELOG_TEXT - 1);

    taskENTER_CRITICAL(&s_mux);
    uint32_t w = s_write;
    // The slot about to be reused holds line (w - NODELOG_LINES). If the uplink never got to it,
    // it is gone from the server's copy and the counter is the only trace left. s_sent is snapped
    // forward rather than left behind: a cursor pointing at a line that no longer exists would
    // make the next POST read an overwritten slot and send whatever landed there instead.
    if (w - s_sent >= NODELOG_LINES) {
        s_dropped++;
        s_sent = w - (NODELOG_LINES - 1);
    }
    LogLine *e = &s_ring[w % NODELOG_LINES];
    e->ms = now;
    e->role = role;
    memcpy(e->text, line, n);
    e->text[n] = '\0';
    s_write = w + 1;
    taskEXIT_CRITICAL(&s_mux);
}

// One line out of the ring by absolute position, copied so the caller can work on it with the
// recv callback still running. False when that position has already been overwritten or has not
// been written yet, which both readers below treat as "the window moved under me".
static bool ring_copy(uint32_t abs, LogLine *out) {
    taskENTER_CRITICAL(&s_mux);
    uint32_t w = s_write;
    bool ok = (abs < w) && (w - abs <= NODELOG_LINES);
    if (ok) memcpy(out, &s_ring[abs % NODELOG_LINES], sizeof(*out));
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}


// ---- the JSON body ---------------------------------------------------------------------------

// The escaped worst case, per byte. A byte below 0x20 becomes \u00xx, six bytes for one, and so
// does a byte that is not part of a well-formed UTF-8 sequence (see bputesc) - those two are the
// widest any single byte gets: " and \ and the four shorthands cost two, and a valid multi-byte
// sequence passes through 1:1 because JSON escapes nothing above ASCII (see the note on
// plantrx.cpp's badd_str). So a Korean line is 1:1 and only a node sending an uninitialised
// text[] reaches this bound - but it has to be the bound, because a node with a wedged I2C bus is
// exactly the node that sends garbage, and it is also the node whose log somebody needs.
static const size_t ESC_MAX = 6;
// {"role":"panel","ms":4294967295,"text":""}, measured: 42 bytes, rounded up for the comma and
// slack rather than being recounted every time the key set changes.
static const size_t LINE_OVERHEAD = 48;
static const size_t JSON_CAP = 24                                    // {"device":"","lines":[]}
                             + 18 * ESC_MAX                          // the MAC net_mac() prints
                             + NODELOG_LINES * (LINE_OVERHEAD + (NODELOG_TEXT - 1) * ESC_MAX)
                             + 1;                                    // NUL

// PSRAM, for hlog.cpp's reason and more sharply: 20KB of internal DRAM is a real bite out of the
// pool the RGB framebuffer and LVGL already live in, and this buffer is only ever touched from
// loop() where a cache miss costs nothing anybody can see. Allocated once at init instead of per
// POST so a tick can never fail on a fragmented heap.
static char *s_json = nullptr;

// The same Buf discipline plantrx.cpp uses - overflow is recorded by parking n at cap and every
// helper then does nothing - kept here rather than shared because both are file-static and a json
// module extracted for two callers would be an abstraction neither of them asked for.
struct Buf { char *p; size_t cap; size_t n; };

static bool bfull(const Buf *b) { return b->n >= b->cap; }

static void bputf(Buf *b, const char *fmt, ...) {
    if (bfull(b)) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->cap - b->n) { b->n = b->cap; return; }
    b->n += (size_t)w;
}

// The value of a string member, escaped, without the quotes. One unescaped byte does not corrupt
// one field - it ends the JSON string early and the server rejects the whole batch, so twenty
// lines are lost because one of them mentioned a Windows path.
//
// WHY THIS VALIDATES UTF-8 AND DOES NOT JUST COPY THE HIGH BYTES. A JSON body is decoded as UTF-8
// before it is parsed, so one byte that is not part of a well-formed sequence fails the batch just
// as completely as an unescaped quote does - and worse, it fails it every time: the cursor only
// advances on a 2xx, so a single stray 0xB0 from a printf that thought it was Latin-1 would stall
// the whole uplink until the ring wrapped past it. A node emitting bytes nobody validated is not a
// hypothetical here; it is the failure this subsystem exists to make visible.
//
// A byte that fails the check is emitted as \u00xx, i.e. read as the Latin-1 character it would
// be, rather than dropped or replaced with U+FFFD: the common cause is exactly a Latin-1 degree
// sign or an accented name, and "°" is what the node meant. Random garbage becomes mojibake, which
// is still a readable log line and still a batch the server accepts.
//
// The continuation ranges are per lead byte and not a blanket 0x80..0xBF, because Python's decoder
// on the far side rejects the overlong forms (0xC0/0xC1, 0xE0 0x80..0x9F, 0xF0 0x80..0x8F), the
// UTF-16 surrogate block (0xED 0xA0..0xBF) and anything past U+10FFFF (0xF4 0x90.., 0xF5..0xFF).
// A lax "count the continuations" check passes all of those and stalls the uplink anyway.
static void bputesc(Buf *b, const char *v) {
    while (*v) {
        unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\')  { bputf(b, "\\%c", (char)c); v++; }
        else if (c == '\n')         { bputf(b, "\\n"); v++; }
        else if (c == '\r')         { bputf(b, "\\r"); v++; }
        else if (c == '\t')         { bputf(b, "\\t"); v++; }
        else if (c < 0x20)          { bputf(b, "\\u%04x", (unsigned)c); v++; }
        else if (c < 0x80) {
            if (b->n + 2 < b->cap) b->p[b->n++] = (char)c;   // the common byte, no vsnprintf
            else                 { b->n = b->cap; return; }
            v++;
        } else {
            size_t need = 0;
            unsigned char lo = 0x80, hi = 0xBF;              // the FIRST continuation's range
            if (c >= 0xC2 && c <= 0xDF)      need = 1;
            else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; }
            else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F; }
            bool ok = need > 0;
            for (size_t k = 1; ok && k <= need; k++) {
                // v[k] is safe to read: a NUL fails the range and stops the loop before it.
                unsigned char cc = (unsigned char)v[k];
                if (cc < (k == 1 ? lo : 0x80) || cc > (k == 1 ? hi : 0xBF)) ok = false;
            }
            if (!ok) { bputf(b, "\\u%04x", (unsigned)c); v++; }
            else if (b->n + need + 2 < b->cap) {
                for (size_t k = 0; k <= need; k++) b->p[b->n++] = v[k];
                v += need + 1;
            } else { b->n = b->cap; return; }
        }
        if (bfull(b)) return;
    }
}

// Renders [from, to) into s_json. Returns the body length, or 0 when nothing could be built.
static size_t build_body(uint32_t from, uint32_t to) {
    char device[20];
    net_mac(device, sizeof(device));

    Buf b = { s_json, JSON_CAP, 0 };
    bputf(&b, "{\"device\":\"");
    bputesc(&b, device);
    bputf(&b, "\",\"lines\":[");

    size_t rendered = 0;
    for (uint32_t at = from; at != to; at++) {
        LogLine ln;
        // A line the recv callback overwrote while this loop was running. Skipped rather than
        // aborting the whole POST: the cursor has already been snapped past it by nodelog_add(),
        // which counted it as dropped, so there is nothing left here to be faithful to.
        if (!ring_copy(at, &ln)) continue;
        bputf(&b, "%s{\"role\":\"%s\",\"ms\":%lu,\"text\":\"",
              rendered ? "," : "", nodeproto_role_name(ln.role), (unsigned long)ln.ms);
        bputesc(&b, ln.text);
        bputf(&b, "\"}");
        if (bfull(&b)) break;
        rendered++;
    }
    bputf(&b, "]}");

    if (bfull(&b)) return 0;
    if (rendered == 0) return 0;
    return b.n;
}

// ---- HTTP ------------------------------------------------------------------------------------
//
// Hand-written against a WiFiClient, the same as fwpull.cpp and plantrx.cpp, and HTTP/1.0 for
// their reason: 1.0 has no chunked transfer encoding, so a reply is plain bytes and there is no
// de-framing pass in front of the status line.

static const uint32_t CONNECT_MS = 4000;   // fwpull.cpp's figure; same LAN, same server
static const uint32_t REPLY_MS = 6000;     // an append to a log file, not a model run
static const uint32_t IDLE_MS = 2000;      // a gap this long inside the reply means it ended

static void write_post_head(WiFiClient &c, size_t clen) {
    const char *tok = sitecfg_token();
    c.print("POST "); c.print(plantrx_srv_prefix()); c.print(NODEPROTO_PATH_LOG);
    c.print(" HTTP/1.0\r\n");
    c.print("Host: "); c.print(plantrx_srv_host()); c.print(":"); c.print(plantrx_srv_port());
    c.print("\r\n");
    c.print("User-Agent: SmartFarm-ESP32/1.0\r\n");
    c.print("Accept: application/json\r\n");
    c.print("Content-Type: application/json\r\n");
    c.print("Content-Length: "); c.print(clen); c.print("\r\n");
    if (tok[0]) { c.print("Authorization: Bearer "); c.print(tok); c.print("\r\n"); }
    c.print("Connection: close\r\n\r\n");
    // No flush(), for the reason fwpull.cpp's write_get_head() spells out: NetworkClient
    // implements it as clear(), which empties the RX buffer and would throw away the status line
    // this request is about to be judged by. print() writes to the socket synchronously.
}

// The status line, then the rest of the reply read and thrown away. Nothing the server says about
// an accepted log line is worth parsing, but the socket is still drained before stop(): a close
// with bytes outstanding arrives as an RST, and uvicorn logs that as a broken pipe for a request
// it handled perfectly. Returns the HTTP status, 0 for a reply that is not HTTP, -1 for silence.
static int read_status_and_drain(WiFiClient &c, uint32_t start) {
    // One CRLF-terminated line, CRLF stripped. Mirrors fwpull.cpp's read_line(), which is static
    // to that file; a line longer than this is clipped and the rest still consumed.
    char line[128];
    size_t i = 0;
    for (;;) {
        int ch = c.read();
        if (ch < 0) {
            if (!c.connected()) return -1;
            if (millis() - start > REPLY_MS) return -1;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (ch == '\n') break;
        if (i + 1 < sizeof(line)) line[i++] = (char)ch;
    }
    if (i > 0 && line[i - 1] == '\r') i--;
    line[i] = '\0';

    const char *sp = strchr(line, ' ');                  // "HTTP/1.1 204 No Content"
    int status = sp ? atoi(sp + 1) : 0;

    uint8_t sink[128];
    uint32_t last = millis();
    for (;;) {
        int a = c.available();
        if (a > 0) {
            c.read(sink, (size_t)a < sizeof(sink) ? (size_t)a : sizeof(sink));
            last = millis();
        } else {
            if (!c.connected()) break;                   // close = reply complete
            if (millis() - start > REPLY_MS) break;
            if (millis() - last > IDLE_MS) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return status;
}

static bool post_body(size_t len) {
    WiFiClient c;
    uint32_t start = millis();
    if (!c.connect(plantrx_srv_host(), plantrx_srv_port(), CONNECT_MS)) {
        hlogf("[nodelog] cannot reach %s:%u\n", plantrx_srv_host(),
              (unsigned)plantrx_srv_port());
        return false;
    }

    write_post_head(c, len);
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > 1460) n = 1460;                          // one MSS, as post_frame() sends
        size_t w = c.write((const uint8_t *)s_json + sent, n);
        if (w == 0) { c.stop(); return false; }
        sent += w;
    }

    int status = read_status_and_drain(c, start);
    c.stop();
    if (status < 200 || status >= 300) {
        hlogf("[nodelog] POST failed, status=%d after %lums\n", status,
              (unsigned long)(millis() - start));
        return false;
    }
    // Proof this panel's LAN uplink is alive, which is what net.h asks any successful exchange
    // with any peer to report. Pointedly not the failure half: net_note_uplink_fail() is the
    // server poll's vote alone, and a log POST losing a race with an update is not evidence the
    // radio needs cycling.
    net_note_uplink();
    return true;
}

// ---- the poll --------------------------------------------------------------------------------

// A gap between POSTs, which is batching and not throttling: a node with verbose logging on emits
// several lines a second, and one request per line would spend more time in TCP handshakes than
// the whole ring is worth.
static const uint32_t POST_GAP_MS = 2000;
// And after a failure. loop() comes back every second and a connect to a dead host costs
// CONNECT_MS, so retrying at the success cadence would add four seconds to every loop iteration
// on a panel whose server is down - which is the panel's own UI and telemetry paying for it.
static const uint32_t FAIL_GAP_MS = 15000;

static uint32_t s_next_ms = 0;

void nodelog_init(void) {
    s_json = (char *)heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    if (s_json == nullptr) {
        // The ring keeps working and the 업데이트 page keeps drawing it; only the forwarding is
        // off. Said out loud rather than inferred later from a server-side log that never fills,
        // which is indistinguishable from nodes that never said anything.
        hlogf("[nodelog] PSRAM alloc of %u failed; ring only, no uplink\n", (unsigned)JSON_CAP);
        return;
    }
    hlogf("[nodelog] ready; %d lines x %d bytes, %u byte body\n",
          NODELOG_LINES, NODELOG_TEXT, (unsigned)JSON_CAP);
}

void nodelog_tick(void) {
    // Update mode first, and before any of the cheap checks, because it is the one that is a rule
    // rather than a condition: everything that competes with an update for the network or for
    // core 0 stands down, and a log POST is both. See updatemode.cpp for who else is on the list.
    if (updatemode_active()) return;
    if (s_json == nullptr) return;
    if (plantrx_srv_host()[0] == '\0') return;           // no server: silence is correct
    if (net_state() != NET_CONNECTED) return;

    uint32_t now = millis();
    if (s_next_ms != 0 && (int32_t)(now - s_next_ms) < 0) return;

    uint32_t from = s_sent, to = s_write;
    if (from == to) return;                              // nothing new

    // The invariant nodelog_add() maintains is that s_sent never falls more than NODELOG_LINES
    // behind s_write, so this clamp is unreachable. It is here because the alternative if it ever
    // becomes reachable is a loop that walks positions the ring cannot hold, and the clamp costs
    // one comparison per POST.
    if (to - from > NODELOG_LINES) from = to - NODELOG_LINES;

    size_t len = build_body(from, to);
    if (len == 0) {
        // The window did not fit, or every line in it was overwritten mid-build. Either way the
        // cursor is moved past it: a body that cannot be built will not build on the next tick
        // either, and retrying it forever would wedge the uplink on one bad window and never
        // forward another line. Counted, because that is what the dropped figure is for.
        hlogf("[nodelog] window %lu..%lu unsendable; skipping %lu line(s)\n",
              (unsigned long)from, (unsigned long)to, (unsigned long)(to - from));
        taskENTER_CRITICAL(&s_mux);
        if ((int32_t)(to - s_sent) > 0) { s_dropped += to - s_sent; s_sent = to; }
        taskEXIT_CRITICAL(&s_mux);
        s_next_ms = now + POST_GAP_MS;
        return;
    }

    bool ok = post_body(len);
    now = millis();
    s_next_ms = now + (ok ? POST_GAP_MS : FAIL_GAP_MS);
    if (!ok) return;                                     // cursor untouched: the next tick retries

    taskENTER_CRITICAL(&s_mux);
    // Not a bare assignment. nodelog_add() may have wrapped the ring during the round trip and
    // snapped s_sent past `to` already; writing `to` here would rewind the cursor onto slots that
    // have since been reused, and the next POST would send whatever landed in them as if it were
    // the log line it replaced. Signed difference because both are monotonic and wrap.
    if ((int32_t)(to - s_sent) > 0) s_sent = to;
    taskEXIT_CRITICAL(&s_mux);

    hlogf("[nodelog] posted %lu line(s), %u bytes; dropped=%lu\n",
          (unsigned long)(to - from), (unsigned)len, (unsigned long)s_dropped);
}
