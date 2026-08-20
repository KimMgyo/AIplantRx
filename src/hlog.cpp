// The console, kept in memory and served over the network.
//
// WHY THIS EXISTS. The panel's display sits on GPIO 43/44, which are UART0's pins, and a switch
// on the board hands those either to the USB bridge or to the header. Running the display means
// running with no console. Everything else added for this - the reset reason, the crash counter,
// the coredump summary - answers questions about a run that has already ended. This answers the
// other half: what is the board saying RIGHT NOW, while it is misbehaving but not crashing.
// "The sensor node is offline" and "the sensor node is fine and the panel is not listening" look
// identical from the server, and the line that distinguishes them is already being printed to a
// UART nobody can read.
//
// So every diagnostic line goes to two places: the UART, exactly as before, and a ring in PSRAM.
// A TCP listener on port 23 hands a connecting client the ring first and then every new line as
// it happens, so `nc <panel> 23` is the console the hardware took away.
//
// WHY A RING AND NOT A FILE. The interesting output is the most recent output, and a board that
// is misbehaving may be about to reboot: a file needs flushing, an unmount, and a filesystem
// that survives the panic, while a ring needs none of those and loses nothing that matters.
//
// WHY PORT 23 IS NOT ENOUGH, AND WHAT THE SECOND READER IS. `nc <panel> 23` only exists on the
// LAN, and the greenhouse is an hour away from the laptop that has the toolchain. So the same
// ring is also pushed to the server, which serves it on the operator page beside the cards - the
// one place reachable from a phone. The push is a second reader of the same ring with its own
// cursor, deliberately: the two must never be able to disagree about what the board said, and
// sharing the ring rather than the socket is what guarantees that.
//
// WHY THE UART WRITE STAYS. A serial console is still the only thing that works when WiFi does
// not, and this file exists precisely because one diagnostic path is not enough.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdarg.h>
#include <strings.h>
#include "hlog.h"
#include "net.h"
#include "plantrx.h"
#include "sitecfg.h"
#include "srvconn.h"

// 256KB in PSRAM. Internal RAM is the scarce pool on this board - the RGB framebuffer and LVGL
// already live there - and a log ring has no latency requirement that would justify spending it.
//
// The size is a retention target, not a guess. Measured: this panel prints about 15KB a minute
// with the ordinary tags on, so 16KB held barely one minute and a fault that happened while
// nobody was attached had already scrolled out of the ring before anyone could ask about it.
// 256KB holds about 17 minutes, which covers the gap between a board misbehaving and somebody
// opening the page. PSRAM has 1.8MB free with the camera streaming, so the 240KB is affordable.
static const size_t RING_CAP = 256 * 1024;

// One line's worth. Longer lines are truncated rather than heap-allocated: the longest thing
// this project prints is plantrx's status line at about 320 characters, and a diagnostic that
// silently allocates during a crash-adjacent moment is worse than one that clips.
static const size_t LOG_LINE_MAX = 512;

static char *s_ring = nullptr;
static size_t s_head = 0;            // total bytes ever written, not an index
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped_clients = 0;

// Appends to the ring. Called from every task that logs, so the cursor update has to be atomic
// with the copy - a reader computing "how far behind am I" against a half-updated head would
// either repeat or skip a line. The critical section covers a memcpy of at most LOG_LINE_MAX bytes.
static void ring_put(const char *p, size_t n) {
    if (s_ring == nullptr || n == 0) return;
    if (n > RING_CAP) { p += n - RING_CAP; n = RING_CAP; }

    taskENTER_CRITICAL(&s_mux);
    size_t at = s_head % RING_CAP;
    size_t first = RING_CAP - at;
    if (first > n) first = n;
    memcpy(s_ring + at, p, first);
    if (n > first) memcpy(s_ring, p + first, n - first);
    s_head += n;
    taskEXIT_CRITICAL(&s_mux);
}

// Copies out the window starting at absolute position `from`. Returns bytes copied and advances
// `from`. A client that fell more than a ring behind is snapped forward to the oldest byte still
// held, because handing it stale bytes that have since been overwritten would be a lie about
// what the board said.
static size_t ring_take(size_t *from, char *out, size_t max) {
    if (s_ring == nullptr) return 0;

    taskENTER_CRITICAL(&s_mux);
    size_t head = s_head;
    size_t oldest = (head > RING_CAP) ? head - RING_CAP : 0;
    bool lost = (*from < oldest);
    if (lost) *from = oldest;

    size_t n = head - *from;
    if (n > max) n = max;
    size_t at = *from % RING_CAP;
    size_t first = RING_CAP - at;
    if (first > n) first = n;
    memcpy(out, s_ring + at, first);
    if (n > first) memcpy(out + first, s_ring, n - first);
    *from += n;
    taskEXIT_CRITICAL(&s_mux);

    if (lost) s_dropped_clients++;
    return n;
}

// The tee. Formats once, then writes the same bytes to the UART and the ring, so the two can
// never disagree about what the board said - which is the only reason to trust the remote copy.
void hlogf(const char *fmt, ...) {
    char line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;

    Serial.write((const uint8_t *)line, len);
    ring_put(line, len);
}

// The framework's own logging, which is where the panel driver, the WiFi stack and the touch
// controller report. CORE_DEBUG_LEVEL=1 keeps this to errors and the board-bringup lines, and
// those are exactly the ones worth having when a board comes up wrong at the far end of a
// greenhouse - the GT911 "Unable to initialize the I2C address" that cost an afternoon arrived
// on this path, not through Serial.
static int log_vprintf(const char *fmt, va_list ap) {
    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) return n;
    size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;

    // Forwarded by hand: installing a vprintf REPLACES the default one, so without this the
    // console loses every framework message the moment the ring gains it.
    Serial.write((const uint8_t *)line, len);
    ring_put(line, len);
    return n;
}

// ---- the push to the server ------------------------------------------------------------
//
// WHY THE BODY IS RAW BYTES AND NOT JSON. What this ships is a slice of a byte ring, not a list
// of lines: a slice can begin and end mid-line, and that is a property worth keeping rather than
// hiding. Escaping 4KB of arbitrary console text into JSON would cost a second buffer and an
// encode pass on a board whose scarce pool is internal RAM, to describe bytes that need no
// description. So the chunk goes up as text/plain and the server stores it as it arrived; the
// page concatenates chunks and never tries to reassemble lines it was not given.
//
// THIS IS THE FOURTH PLACE IN THIS FIRMWARE THAT HAND-WRITES AN HTTP HEAD - plantrx.cpp,
// plantid.cpp and nodelog.cpp are the others, and folding them is a real change that this is not.
// They differ in path, method and content type, and this one now differs in a fourth way that
// outweighs the other three: it is the only one that KEEPS its connection, so its reply reader
// has to find the end of a body instead of waiting for a close. shared/srvconn.h is shared
// because opening a socket to this server genuinely is identical everywhere; what gets written
// down it is not, and shared/srvurl.h is the precedent for where that line falls - share the part
// that is the same, leave the part that is not.

static const char *CONLOG_PATH = "/v1/conlog";
static const uint32_t PUSH_CONNECT_MS = 4000;
static const uint32_t PUSH_REPLY_MS = 4000;

// A gap between pushes, which is batching and not throttling, for the reason nodelog.cpp gives
// about its own: at 15KB a minute one request per line would spend more time in TCP handshakes
// than the log is worth. Three seconds is chosen against the page's 2s tail - the operator's view
// is then at most one push behind, which reads as live without pretending to be a stream.
static const uint32_t PUSH_GAP_MS = 3000;
// And the TLS cadence, which is a different number because the cost is a different order. Three
// seconds against a 1.84s handshake is a 60% duty cycle of setting up encryption for logs, and the
// gate in shared/srvconn.h means that time is time the telemetry poll cannot connect either. At 20s
// it is 9%, and the console is at most twenty seconds behind - still an answer to "what is the
// board saying now", which a board an hour away did not have at all before today.
//
// This is a CADENCE difference and not a security one: the same bytes, the same bearer, the same
// encryption. Nothing here opts out of TLS - see srvconn.h on why no site gets to.
static const uint32_t PUSH_GAP_TLS_MS = 20000;
static inline uint32_t push_gap_ms(void) {
    return (plantrx_srv_url() && plantrx_srv_url()->tls) ? PUSH_GAP_TLS_MS : PUSH_GAP_MS;
}
// And after a failure, for nodelog.cpp's reason: a connect to a dead host costs PUSH_CONNECT_MS,
// and retrying at the success cadence on a panel whose server is down makes the log the reason
// the log task is busy.
static const uint32_t PUSH_FAIL_GAP_MS = 20000;

// Staged in PSRAM and not on the stack, and the margin that protects has narrowed. This task has
// 8KB; the connect inside it now carries a TLS handshake, measured at 4.4KB of the caller's stack
// (shared/srvurl.h), on top of the 1KB `out` buffer hlog_task already holds for the port 23
// reader. A 4KB frame for this chunk as well is how a log task becomes the thing that panics.
static const size_t PUSH_CHUNK = 4 * 1024;
static char *s_push_buf = nullptr;
static size_t s_push_cursor = 0;
static uint32_t s_push_next_ms = 0;
static uint32_t s_push_fails = 0;

// ---- the held connection ----------------------------------------------------------------
//
// ONE connection, held across pushes, and this is the whole of this file's half of the TLS
// migration. Every other caller in this firmware opens a socket, writes one request and closes.
// This one pushes every PUSH_GAP_MS, and a TLS handshake measures 1.84s on this board WITH chain
// verification - shared/srvurl.h has the three-case probe that number came from. Handshaking per
// chunk would therefore spend roughly 60% of a core on setting up encryption for logs alone,
// which is how a diagnostic becomes the thing most worth diagnosing.
//
// Static and not a local: a stack SrvConn is a connection that cannot outlive the push that
// opened it. The Client* is remembered beside it because SrvConn hands the pointer out of
// connect() and offers no getter for it afterwards; alive() is what answers whether it is still
// good.
//
// WHAT STATIC COSTS, said out loud because internal DRAM is the scarce pool on this board.
// WiFiClientSecure's constructor allocates a sslclient_context - the mbedtls ssl, config and
// ctr_drbg contexts - and a static SrvConn therefore holds one in internal DRAM for the life of
// the image instead of for the length of one push. That is the trade this shape makes, in both
// directions: a permanent allocation bought with 1.84s of handshake every three seconds.
static SrvConn s_push_conn;
static Client *s_push_client = nullptr;

// HTTP/1.1, and this is the only request in this firmware that is not 1.0. The three siblings use
// 1.0 because 1.0 has no chunked transfer encoding, so a reply is plain bytes with no de-framing
// pass in front of the status line - a good trade when the connection is closing anyway. That
// trade is not on offer here: h11, which is what uvicorn parses with, does not support keep-alive
// with an HTTP/1.0 peer AT ALL. The rule is two lines of its _connection.py - `if getattr(event,
// "http_version", b"1.1") < b"1.1": return False` inside _keep_alive() - and the Connection
// header is not consulted on that branch. So a 1.0 request carrying `Connection: keep-alive` gets
// its reply and a FIN, alive() is false at the next push, and the connection this file exists to
// hold would be re-handshaked every three seconds while the code read as though it were reusing
// one.
//
// What 1.1 costs is that a reply MAY arrive chunked. This server's does not: the endpoint returns
// a dict, FastAPI renders it through JSONResponse, and JSONResponse sets Content-Length itself. A
// reply that arrives without one is handled rather than assumed away - see push_reply().

// Drops the held connection. Both callers have the same reason: a socket whose position in the
// byte stream is not known must never be handed to the next POST.
static void push_drop(void) {
    s_push_conn.stop();
    s_push_client = nullptr;
}

// Every failure exit. There are four ways to fail now where there were two - connect, a
// half-written request, a non-2xx, and a reply that did not parse - and all four owe the same
// three things, so they are written once. Dropping the connection is the new part and the
// important one: a request that went out incomplete leaves the server reading a body that will
// never arrive, and either way the next POST would be answered by bytes belonging to this one.
static void push_failed(int status, uint32_t start) {
    // Before push_drop(), which is where the reason would be cleared. Empty on the plain path,
    // where a refused certificate is not one of the things that can have gone wrong.
    char why[80];
    s_push_conn.last_error(why, sizeof(why));
    push_drop();

    // Said once per streak and not once per failure: this line goes into the ring it is about, so
    // a server that is down for an hour would otherwise fill the log with the news that the log
    // cannot be sent, and push the actual fault out the far end. That rule matters more now, not
    // less - a certificate this panel will not verify fails every push for as long as the board
    // is up, and one line per failure would bury the reason underneath itself.
    if (s_push_fails == 0) {
        hlogf("[hlog] push failed, status=%d after %lums%s%s; retrying every %lus\n", status,
              (unsigned long)(millis() - start), why[0] ? " - " : "", why,
              (unsigned long)(PUSH_FAIL_GAP_MS / 1000));
    }
    s_push_fails++;
    s_push_next_ms = millis() + PUSH_FAIL_GAP_MS;
}

// One CRLF-terminated line with the CRLF stripped. Returns its length, or -1 when the socket died
// or the reply budget ran out first. The budget is the whole reply's and not this line's, so a
// server dribbling a byte at a time cannot buy itself an unbounded number of lines. A line longer
// than `cap` is clipped and the rest of it is still consumed, so a header this does not read
// cannot desynchronise the ones it does.
static int push_line(Client &c, char *out, size_t cap, uint32_t start) {
    size_t i = 0;
    for (;;) {
        int ch = c.read();
        if (ch < 0) {
            if (!c.connected()) return -1;
            if (millis() - start > PUSH_REPLY_MS) return -1;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ch == '\n') break;
        if (i + 1 < cap) out[i++] = (char)ch;
    }
    if (i > 0 && out[i - 1] == '\r') i--;
    out[i] = '\0';
    return (int)i;
}

// The whole reply: the status line, the headers, and then EXACTLY the body the headers promised.
// Returns the HTTP status, 0 for a reply that is not HTTP, or -1 when nothing arrived at all.
// Nothing in the body is parsed - there is nothing in it worth parsing, the same as before - but
// it is now counted, which is a different job from draining it.
//
// *reusable IS THE POINT OF THIS FUNCTION. A held connection may only be handed to the next POST
// if this reader knows it stopped on the last byte of this reply rather than somewhere inside it.
// One byte left in the pipe becomes the first byte of the next status line, that reply then does
// not parse, and every reply after it is wrong in a way that reads as a server fault. So
// *reusable is set at exactly one place - after the body counter has reached zero - and every
// other way out of here leaves it false and gets the socket dropped.
static int push_reply(Client &c, uint32_t start, bool *reusable) {
    *reusable = false;

    char line[128];
    long clen = -1;
    int status = 0;

    for (int n = 0;; n++) {
        int len = push_line(c, line, sizeof(line), start);
        if (len < 0) return n == 0 ? -1 : 0;   // nothing arrived, or it died inside the head
        if (len == 0) break;                   // the blank line; the body follows
        if (n == 0) {
            const char *sp = strchr(line, ' ');            // "HTTP/1.1 200 OK"
            status = sp ? atoi(sp + 1) : 0;
        } else if (strncasecmp(line, "content-length:", 15) == 0) {
            // Case-insensitively, for the reason fwpull.cpp's read_head() gives: uvicorn emits it
            // lowercase, RFC 9110 says header names are not case-sensitive, and a proxy on the
            // site LAN is allowed to normalise them.
            clen = strtol(line + 15, nullptr, 10);
        }
    }

    // No Content-Length. The reply parsed and its status still answers for the chunk, so the
    // caller may act on it - but nothing here says where the body ends, so this socket's position
    // in the stream is unknown and it must not be reused. Returning not-reusable costs one
    // handshake and can never cost a desynchronised reply. A chunked reply lands here too, which
    // is the right answer to it as well: the frame markers would otherwise be counted as body.
    if (clen < 0) return status;

    long left = clen;
    char sink[64];
    while (left > 0) {
        int a = c.available();
        if (a > 0) {
            size_t want = (left < (long)sizeof(sink)) ? (size_t)left : sizeof(sink);
            int rd = c.read((uint8_t *)sink, want);
            if (rd > 0) { left -= rd; continue; }
        }
        // The body stopped short of what the headers promised. The status is still what the
        // server said, and the socket is still the wrong thing to keep.
        if (!c.connected()) return status;
        if (millis() - start > PUSH_REPLY_MS) return status;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Counted down to the byte, so the next thing in the pipe is the next reply's status line.
    *reusable = true;
    return status;
}

// Takes whatever the ring has beyond the push cursor and POSTs it. The cursor advances only on a
// 2xx, so a server that is down loses nothing that the ring still holds - and when the ring does
// overwrite unsent bytes, ring_take() snaps the cursor forward and counts it, which is why the
// dropped total travels in a header rather than being silently absent.
static void push_once(void) {
    if (s_push_buf == nullptr) return;
    const SrvUrl *url = plantrx_srv_url();
    if (url->host[0] == '\0') return;                 // no server configured, nothing to push to

    size_t at = s_push_cursor;
    size_t n = 0;
    while (n < PUSH_CHUNK) {
        size_t got = ring_take(&at, s_push_buf + n, PUSH_CHUNK - n);
        if (got == 0) break;
        n += got;
    }
    if (n == 0) return;

    uint32_t start = millis();

    // Reconnected only when the socket has actually gone, which is the entire saving. alive() and
    // not a flag of our own: the server's idle timeout, a NAT box on the way out or the radio can
    // each close this underneath us, and the only thing that knows is the socket.
    //
    // AND alive() IS FULLY HONEST ONLY ON THE TLS PATH, which is worth writing down rather than
    // rediscovering. NetworkClientSecure::connected() reads through mbedtls, so a close_notify
    // marks the client disconnected and the next push handshakes cleanly. The plain path probes
    // with recv(fd, &b, 1, MSG_DONTWAIT | MSG_PEEK), and a graceful FIN returns 0 with errno
    // untouched - which lands in that switch's `default:` and reports CONNECTED
    // (Network/src/NetworkClient.cpp:550). So on http:// a server that timed this connection out
    // during a quiet stretch costs exactly one push: the POST goes into a half-closed socket, no
    // reply comes back, and push_failed() below drops it and waits out the backoff. It
    // self-corrects, it can only happen after a gap longer than the server's keep-alive timeout,
    // and it is one more thing that is simply better on the scheme this migration exists for.
    if (!s_push_conn.alive()) {
        s_push_client = s_push_conn.connect(*url, PUSH_CONNECT_MS);
        if (s_push_client == nullptr) {
            push_failed(-1, start);
            return;
        }
    }
    Client &c = *s_push_client;

    char device[20];
    net_mac(device, sizeof(device));
    const char *tok = sitecfg_token();

    // ONE WRITE, NOT TWENTY-FOUR, and this file has the most to lose by it. The head used to be a
    // run of c.print() calls, which over plain TCP is free - Nagle coalesces them into one segment.
    // Over TLS every print() becomes its OWN TLS RECORD, each with a header and a MAC, so a
    // ~250-byte head left the board as 24 tiny records, and Cloudflare - which this server sits
    // behind - resets a connection that does that. Measured as MBEDTLS_ERR_NET_CONN_RESET
    // (-0x0050) on the first write, a few hundred milliseconds after a handshake that had already
    // succeeded; plantrx.cpp's write_request_head() carries the numbers.
    //
    // WHAT IT COSTS HERE IS NOT ONE POST. Every sibling opens a socket, writes one request and
    // closes, so a reset there loses a chunk. This connection is HELD across pushes and asks for
    // that with `Connection: keep-alive`, so a reset takes the 1.84s handshake this whole section
    // exists to avoid - and takes it again at the next push, and the one after, for as long as the
    // records stay small. The record shape is therefore load-bearing on the saving, not just on
    // the request.
    //
    // The head and only the head: the chunk below stays its own write() of the PSRAM buffer, which
    // is already one call per MSS-sized piece and was never the problem. Not one header field,
    // value or order changed - 1.1 and keep-alive included, for the h11 reason above.
    //
    // 480 bytes, sized from what THIS head can hold rather than copied from a sibling: a 48-byte
    // prefix cap and a 10-byte path, a 64-byte host cap and a 5-digit port, four digits of
    // Content-Length because the chunk is capped at PUSH_CHUNK, the 20-byte MAC buffer above, ten
    // digits of X-Dropped, a 96-byte token cap, and 177 bytes of fixed header text. 431 with the
    // NUL, so 480 has room and no more.
    char head[480];
    int head_n = snprintf(head, sizeof(head),
                          "POST %s%s HTTP/1.1\r\n"
                          "Host: %s:%u\r\n"
                          "User-Agent: SmartFarm-ESP32/1.0\r\n"
                          "Content-Type: text/plain\r\n"
                          "Content-Length: %u\r\n"
                          "X-Device: %s\r\n"
                          "X-Dropped: %lu\r\n",
                          url->prefix, CONLOG_PATH, url->host, (unsigned)url->port,
                          (unsigned)n, device, (unsigned long)s_dropped_clients);
    if (tok[0] && head_n > 0 && head_n < (int)sizeof(head)) {
        head_n += snprintf(head + head_n, sizeof(head) - head_n,
                           "Authorization: Bearer %s\r\n", tok);
    }
    if (head_n > 0 && head_n < (int)sizeof(head)) {
        head_n += snprintf(head + head_n, sizeof(head) - head_n, "Connection: keep-alive\r\n\r\n");
    }
    if (head_n <= 0 || head_n >= (int)sizeof(head)) {
        // Truncation would send a head with no terminating blank line, which reads to the server as
        // a request that never ended - and on a held connection that is worse than a lost chunk:
        // the server would sit reading a request that never finishes on the one socket this file
        // is trying to keep. So nothing goes out at all. Said out loud rather than silently sent,
        // because every field above is capped elsewhere and this can only fire if one of those caps
        // moved. Once per streak, for push_failed()'s reason: the line lands in the ring it is
        // about, and a cap that moved fails every push for as long as the board is up.
        if (s_push_fails == 0) {
            hlogf("[hlog] push head needs %d bytes, cap is %u\n", head_n, (unsigned)sizeof(head));
        }
        push_failed(-1, start);
        return;
    }
    c.write((const uint8_t *)head, (size_t)head_n);
    // No flush(), for the reason nodelog.cpp and fwpull.cpp both spell out: NetworkClient
    // implements it as clear(), which empties the RX buffer this request is about to be judged by.

    size_t sent = 0;
    while (sent < n) {
        size_t take = n - sent;
        if (take > 1460) take = 1460;                 // one MSS, as the sibling POSTs send
        size_t w = c.write((const uint8_t *)s_push_buf + sent, take);
        if (w == 0) break;
        sent += w;
    }
    if (sent != n) {
        // A head with no body behind it, on a connection the server is still holding open waiting
        // for the rest. There is no recovering the framing from here, so the socket goes.
        push_failed(-1, start);
        return;
    }

    bool reusable = false;
    int status = push_reply(c, start, &reusable);
    if (status < 200 || status >= 300) {
        push_failed(status, start);
        return;
    }

    if (!reusable) {
        // Accepted, so this is not the failure path - the chunk is on the server whatever state
        // the socket is in, and taking FAIL_GAP for it would stall a working console for twenty
        // seconds and then resend bytes the server already has. It costs one handshake next time.
        push_drop();
    }

    s_push_cursor = at;                               // only now: an unacknowledged chunk is resent
    s_push_fails = 0;

    // CAUGHT UP? THEN GIVE THE CONNECTION BACK, AND WITH IT THE TLS GATE.
    //
    // The held connection above exists to avoid a handshake per chunk, and it still does - a
    // backlog drains over one socket, because this returns immediately and the next tick finds
    // more to send. What it must NOT do is keep the socket open while idle: shared/srvconn.h
    // serialises TLS sessions on one gate because two of them do not fit in this board's internal
    // DRAM, and this task pushes every few seconds forever. Holding the gate between pushes would
    // starve the telemetry poll permanently - the log would be the reason the greenhouse stopped
    // being controlled, which is the exact inversion of what a diagnostic is for.
    //
    // So: more to send, keep it and come straight back. Nothing to send, drop it. On a quiet board
    // that is one handshake per PUSH_GAP window; on a busy one it is one per backlog.
    size_t pending;
    taskENTER_CRITICAL(&s_mux);
    pending = s_head - s_push_cursor;
    taskEXIT_CRITICAL(&s_mux);
    if (pending == 0) {
        push_drop();
        s_push_next_ms = millis() + push_gap_ms();
    } else {
        s_push_next_ms = millis();                    // straight on, same socket
    }
    // Proof this panel's LAN uplink is alive, which net.h asks any successful exchange with any
    // peer to report. Pointedly not the failure half - net_note_uplink_fail() is the server poll's
    // vote alone, and a log push losing a race is not evidence the radio needs cycling. Same
    // division nodelog.cpp draws, for the same reason.
    net_note_uplink();
}

// ---- the listener ----------------------------------------------------------------------
//
// One client at a time, and the newest connection wins. Two people reading the same log is not
// a case worth the second socket, and a stale half-open connection from a laptop that went to
// sleep must not be able to lock out the person standing in the greenhouse now.

static WiFiServer s_server(23);
static bool s_listening = false;

static void hlog_task(void *arg) {
    WiFiClient client;
    size_t cursor = 0;
    char out[1024];

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            if (s_listening) {
                s_server.end();
                s_listening = false;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!s_listening) {
            s_server.begin();
            s_server.setNoDelay(true);
            s_listening = true;
            hlogf("[hlog] console on tcp/23 at %s\n", WiFi.localIP().toString().c_str());
        }

        WiFiClient fresh = s_server.accept();
        if (fresh) {
            if (client) client.stop();
            client = fresh;
            client.setNoDelay(true);
            // Hands over the whole ring first, so the first thing a client sees is how the
            // board got into its current state rather than only what it does next.
            cursor = (s_head > RING_CAP) ? s_head - RING_CAP : 0;
            client.printf("--- smartfarm panel console, %u bytes of history ---\r\n",
                          (unsigned)(s_head - cursor));
        }

        if (client && !client.connected()) client.stop();

        if (client) {
            size_t n = ring_take(&cursor, out, sizeof(out));
            if (n > 0 && client.write((const uint8_t *)out, n) != n) client.stop();
        }

        // The second reader, on its own cursor and its own clock. Placed here rather than in
        // loop() because loop() runs the UI: a POST that stalls for PUSH_CONNECT_MS in there is
        // four seconds of frozen touch screen, and this task already exists and already reads
        // this ring for the socket above.
        if ((int32_t)(millis() - s_push_next_ms) >= 0) push_once();

        // 20ms with a client attached. Fast enough that a line appears as it is printed and slow
        // enough to stay out of the way: this board has already been reset once by a task that
        // polled at 1ms and starved the idle task the watchdog watches (see camnet.cpp). 200ms
        // idle is still well inside PUSH_GAP_MS, so the push cadence is set by its own timer and
        // not by which branch this lands in.
        vTaskDelay(pdMS_TO_TICKS(client ? 20 : 200));
    }
}

void hlog_init(void) {
    s_ring = (char *)heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM);
    if (s_ring == nullptr) {
        // No ring, no remote console - but hlogf() still reaches the UART, so the board is no
        // worse off than before this file existed. Said out loud because a silent downgrade to
        // "the console is empty" would look like the board having nothing to say.
        Serial.println("[hlog] PSRAM alloc failed; remote console off, UART only");
        return;
    }

    // The push stages here rather than on the task stack, and a failure to get it costs the
    // server copy only: the ring, the UART and port 23 all still work, so the board is degraded
    // to what it was before the push existed rather than broken.
    s_push_buf = (char *)heap_caps_malloc(PUSH_CHUNK, MALLOC_CAP_SPIRAM);
    if (s_push_buf == nullptr) {
        Serial.println("[hlog] PSRAM alloc failed for the push buffer; tcp/23 and UART only");
    }

    esp_log_set_vprintf(log_vprintf);

    // Priority 1 and core 0: below everything that matters, beside the other network tasks.
    // A log reader must never be the reason a frame is dropped or an update stalls.
    //
    // 8KB and not the 4KB this shipped with. The push opens its socket from inside this task, and
    // against an https:// server that means a TLS handshake on this stack - 4.4KB of it, measured
    // on this board (shared/srvurl.h) - beside the 1KB `out` buffer hlog_task holds and the 4KB
    // chunk that is staged in PSRAM precisely so it is not here too. 4KB was sized for a task
    // that only ever did a memcpy and a socket write. It is now paid at most once per held
    // connection rather than once every three seconds, which is the other half of why the
    // connection is held.
    // 16KB, and the third size this task has had. 4KB was for a memcpy and a socket write; 8KB
    // covered a plain-HTTP connect; the TLS handshake needs more than 8192 leaves after the frames
    // already on this stack. Measured the hard way - the handshake succeeded and the first write
    // put 0 bytes on the wire with connected() already false, which reads as a server problem
    // rather than a stack one. See the note on SET_LOOP_TASK_STACK_SIZE in main.cpp.
    xTaskCreatePinnedToCore(hlog_task, "hlog", 16384, nullptr, 1, nullptr, 0);
}

uint32_t hlog_overruns(void) { return s_dropped_clients; }
