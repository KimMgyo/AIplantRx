// The CAM's node agent: report itself, take orders, install its own firmware.
// See nodeagent.h for why this file never initialises ESP-NOW and why reports are broadcast.
//
// THE SHAPE IS BORROWED ON PURPOSE. Everything below the command handling - the URL split, the
// flat JSON lookups, the HTTP/1.0 GET, the size-then-md5 guard, the two-tick yield inside the
// write loop - is the panel's src/fwpull.cpp with the panel-specific parts taken out. That is not
// laziness: each of those pieces exists because of a failure that was measured on this project's
// hardware, and the comments there record which. Re-deriving them here would mean re-earning
// them. What is genuinely different is documented where it appears: the role-scoped URL, the far
// more generous stall timeout this board's link needs, and the streamer stand-down.
//
// WHAT THIS FILE WILL NOT DO. It will not retry. A failure here means the bytes on the server do
// not match the manifest that described them, or the flash refused them, or the link died - and
// none of those get better by going round again. What it does instead is put the camera back and
// say why in Korean on the panel's screen, because the person who can fix it is standing in front
// of that screen and not in front of this board.
#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_now.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "nodeagent.h"
#include "nodeproto.h"

// provision.cpp registers this peer and re-adds it after every radio cycle; this is the address
// it registers. Kept local rather than exported: provision.h's constants carry a "MUST
// byte-for-byte match the S3" contract and six bytes of 0xFF does not belong under it.
static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- timing ---------------------------------------------------------------------------------

static const uint32_t HELLO_MS = 3000;   // the idle beacon; matches provision.cpp's StatusMsg
static const uint32_t TICK_MS  = 50;     // worker poll. A command waits at most this long.

// A progress frame goes out at least this often for the whole of a download, whether or not the
// percentage moved. The panel's nodeota_tick() gives each phase its own silence window and calls
// a node in NODE_PH_DL failed after 20s with no NODE_PROG, because a frozen bar is the failure
// this system most wants to avoid - and a slow stretch on this link can eat 20 seconds without
// advancing a whole percent, which is precisely the healthy-but-slow case that must not be
// mistaken for a dead one. 1500ms leaves about ten lost reports of slack inside that window.
static const uint32_t PROG_KEEPALIVE_MS = 1500;
// ...and on top of that, one every this many percent, so the bar moves smoothly on a fast link.
static const uint8_t  PROG_STEP_PCT = 5;

static const uint32_t CONNECT_MS = 4000;
static const uint32_t REPLY_MS   = 8000;   // headers or a whole manifest; both come off a stat()
static const uint32_t IDLE_MS    = 4000;   // a gap this long inside a small body means it ended

// TWENTY SECONDS, and the number is the point rather than a default.
//
// This board's WiFi link stalls for hundreds of milliseconds at a stretch as a matter of course.
// That is not a suspicion, it is why RTSP_REQUIRE_TCP=1 sits in its platformio.ini: RTP has no
// retransmission, so a UDP viewer freezes and drops the session across a stall that TCP simply
// rides out. The measurements in main.cpp's header have the scale - a 3KB socket write taking
// anywhere from 45ms to 1.3s, ping maxima of 771ms - and while standing the streamer down removes
// the camera's share of that, it does nothing about the AP, which sits on 2.4GHz channel 2
// overlapping a neighbour on 1 and another at 28% utilisation.
//
// So a healthy-but-slow stretch is normal here in a way it is not on the panel, and the trade is
// asymmetric: aborting a working download leaves the camera dark AND un-updated, while waiting
// out a dead one costs only the wait. Twice the panel's 10s, and it is still only ever reached by
// a link that has delivered nothing at all for twenty seconds.
static const uint32_t STALL_MS = 20000;

// A ceiling on the whole image transfer, because the stall timeout alone cannot bound it: a link
// trickling one segment every nineteen seconds never trips it and would keep the camera dark for
// as long as it kept doing that. Four minutes against a measured ~99-142KB/s for a 1.09MB image
// is roughly thirty times the honest duration, so nothing that is actually working reaches it.
static const uint32_t IMAGE_BUDGET_MS = 240000;

// 4KB, matching SPI_FLASH_SEC_SIZE and UpdateClass's own buffer, so one write() lands as exactly
// one flash write with nothing carried into the next call - which is what puts the yield below
// between erases instead of in the middle of one.
static const size_t CHUNK = 4096;

// ---- state ----------------------------------------------------------------------------------

// The single source of truth for "is an update running": nodeproto_phase_busy() over this. A
// separate bool would be a second thing to keep in step with the phase the panel is shown, and
// the two disagreeing is exactly how a node ends up reporting idle in the middle of a download.
// Volatile because the recv callback on the WiFi task reads it to refuse a second NODE_UPDATE.
static volatile uint8_t s_phase = NODE_PH_IDLE;
static volatile uint8_t s_pct   = NODE_PCT_NONE;
static char s_text[NODEPROTO_TEXT] = "";

static volatile bool s_verbose = false;

static uint8_t s_seq = 0;   // our outgoing report seq; worker task only

// The command slot. The recv callback fills it and returns; the worker drains it. One deep on
// purpose: a queue would let a burst of retries stack up behind a download and then execute after
// it, which is how one button press becomes three installs.
static NodeCmdMsg     s_cmd;
static volatile bool  s_cmd_pending = false;

// The last command the worker actually claimed, so a retry of it is answered rather than run
// again. The panel resends NODE_UPDATE up to three times ~700ms apart with the SAME seq until it
// sees a non-idle phase come back, so without this one press starts three downloads.
static volatile uint8_t  s_exec_seq  = 0;
static volatile bool     s_have_exec = false;
static volatile uint32_t s_exec_ms   = 0;
// ...but only for a minute. seq is eight bits and wraps, and a node that refuses command 7 forever
// because it ran a different command 7 an hour ago is worse than one that occasionally runs a
// duplicate. Sixty seconds covers three retries 700ms apart by a factor of thirty.
static const uint32_t DEDUP_MS = 60000;

// Set when a duplicate was dropped, so the worker re-sends the current phase. That resend is the
// acknowledgement the panel's retry loop is waiting for - dropping a duplicate silently would
// make it retry all three times and then give up on a node that is doing exactly what it asked.
static volatile bool s_ack_wanted = false;

// Server address, parsed out of whatever NODE_UPDATE carried. Not persisted: the panel knows
// where the server is and says so on every command, which is the whole reason a node that has to
// be reflashed to learn about a moved server is a node somebody has to physically reach.
static char     s_host[64];
static uint16_t s_port = 80;
static char     s_prefix[48];
static char     s_tok[NODEPROTO_TOKEN];

// ---- the log ring ------------------------------------------------------------------------
//
// WHY A RING WITH A DROP COUNT AND NOT A QUEUE. The failure this has to survive is a diagnostic
// printed on every pass of a loop that is failing - which is precisely when somebody has just
// turned verbose on to find out why. At 14fps a per-frame line is 14 frames a second of 192-byte
// ESP-NOW traffic on the channel the sensor node's telemetry shares, and telemetry losing to a
// log is the log breaking the thing it was opened to diagnose. So the drain rate is the limit,
// the ring absorbs bursts, and anything that does not fit is counted rather than queued: eight
// slots of history plus an honest "N dropped" beats an unbounded backlog of stale lines.
static const int LOG_SLOTS = 8;
static const int LOG_RATE  = 5;   // frames per second out of the drain

static char s_log[LOG_SLOTS][NODEPROTO_TEXT];
static volatile uint8_t  s_log_head = 0;   // producer
static volatile uint8_t  s_log_tail = 0;   // consumer (worker task)
static volatile uint16_t s_log_dropped = 0;
// A spinlock and not a mutex. The producers are the Arduino loop task (print_status, at priority
// 1) and this file's worker (at priority 5), both on core 1 - so the danger is not two cores but
// preemption, and portENTER_CRITICAL closes that by disabling interrupts for the length of one
// bounded 160-byte copy. A mutex would add a priority-inheritance round trip to something
// measured in hundreds of nanoseconds, on the path a failing board uses most.
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void log_push(const char *line) {
    portENTER_CRITICAL(&s_log_mux);
    uint8_t next = (uint8_t)((s_log_head + 1) % LOG_SLOTS);
    if (next == s_log_tail) {
        s_log_dropped++;
    } else {
        strncpy(s_log[s_log_head], line, NODEPROTO_TEXT - 1);
        s_log[s_log_head][NODEPROTO_TEXT - 1] = '\0';
        s_log_head = next;
    }
    portEXIT_CRITICAL(&s_log_mux);
}

// `force` bypasses the verbose gate: it is for the NODE_DEBUG "state" dump, which is a bounded
// handful of lines somebody explicitly asked for and would be useless if answering it first
// required a second command to turn logging on.
static void vlogf(bool force, const char *fmt, va_list ap) {
    // 384 because print_status() is the widest caller: its line formats to about 330 characters
    // once every counter is at its maximum width. Truncating that one would cut the tail, which
    // is where wifi/phy/rssi/heap live - the half a remote reader actually needs.
    char buf[384];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) return;

    // Unconditional, and byte-for-byte what Serial.printf would have written. The console is
    // still the first place a developer with a cable looks, and the radio half must never be able
    // to change what it says.
    Serial.print(buf);

    if (!force && !s_verbose) return;

    // NODE_LOG carries a line "already trimmed of its newline" (nodeproto.h). Trim both ends:
    // the boot banner leads with one, and every other caller trails with one.
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    size_t lead = 0;
    while (lead < len && (buf[lead] == '\n' || buf[lead] == '\r')) lead++;
    if (lead == len) return;

    // Split rather than truncate. print_status()'s line does not fit in 159 characters and it is
    // the single most useful thing this board emits; losing its tail to a field-width limit would
    // be a silent hole in exactly the diagnostic somebody turned logging on to read.
    const char *p = buf + lead;
    size_t remain = len - lead;
    while (remain) {
        char piece[NODEPROTO_TEXT];
        size_t take = remain > (NODEPROTO_TEXT - 1) ? (NODEPROTO_TEXT - 1) : remain;
        memcpy(piece, p, take);
        piece[take] = '\0';
        log_push(piece);
        p += take;
        remain -= take;
    }
}

void nodeagent_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlogf(false, fmt, ap);
    va_end(ap);
}

static void statef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void statef(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlogf(true, fmt, ap);
    va_end(ap);
}

// ---- reporting --------------------------------------------------------------------------------

static uint8_t current_flags(void) {
    uint8_t f = 0;
    if (WiFi.status() == WL_CONNECTED) f |= NODEF_WIFI;

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run != NULL && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        f |= NODEF_PENDING;
    }

    // Asked every time rather than cached at boot, and it is the flag that decides whether the
    // panel offers a button at all. The esp32cam board definition ships huge_app.csv - ONE app
    // partition - and a board still on it returns NULL here. There is no runtime workaround, only
    // a wired reflash with min_spiffs.csv, so the honest answer is the one that stops a grower
    // pressing a button whose only possible outcome is a failure message on a device they cannot
    // see. Computing it from the running image also means the answer cannot be a build flag
    // somebody forgot to change.
    if (esp_ota_get_next_update_partition(NULL) != NULL) f |= NODEF_CAN_OTA;

    if (s_verbose) f |= NODEF_VERBOSE;
    return f;
}

static void fill_report(NodeRepMsg *r, uint8_t kind) {
    memset(r, 0, sizeof(*r));
    r->magic = NODE_MAGIC;
    r->ver   = NODEPROTO_VER;
    r->kind  = kind;
    r->role  = NODE_ROLE_CAM;
    r->seq   = s_seq++;

    // HELLO carries NODE_PH_IDLE and no percentage, always - nodeproto.h says IDLE is "the only
    // phase HELLO ever carries", and the worker holds up the beacon entirely while an update runs
    // so that promise costs nothing. The alternative, letting HELLO report the live phase, puts a
    // 3-second heartbeat in front of the panel's phase field that can arrive between two progress
    // frames; a receiver that sees IDLE mid-download drops its overlay.
    r->phase = (kind == NODE_PROG) ? s_phase : NODE_PH_IDLE;
    r->pct   = (kind == NODE_PROG) ? s_pct   : NODE_PCT_NONE;
    r->flags = current_flags();

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        r->ip[0] = ip[0];
        r->ip[1] = ip[1];
        r->ip[2] = ip[2];
        r->ip[3] = ip[3];
    }

    // esp_timer, not millis(): millis() wraps at 49.7 days and this field's whole job is telling a
    // node in a crash loop from one that has been up since spring. A counter that resets on its
    // own would make a healthy board look like the failing one.
    r->uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000);
    r->free_heap = (uint32_t)ESP.getFreeHeap();

    memcpy(r->elf_sha, esp_app_get_description()->app_elf_sha256, sizeof(r->elf_sha));
}

static void send_rep(uint8_t kind, const char *text) {
    NodeRepMsg r;
    fill_report(&r, kind);
    if (text != NULL) {
        strncpy(r.text, text, sizeof(r.text) - 1);
        r.text[sizeof(r.text) - 1] = '\0';
    }
    esp_now_send(BROADCAST, (const uint8_t *)&r, sizeof(r));
}

// Move the phase, remember the phrase, and tell the panel in the same breath. Every state change
// in this file goes through here, so there is no path that changes what the node is doing without
// saying so.
static void publish(uint8_t phase, uint8_t pct, const char *text) {
    s_phase = phase;
    s_pct   = pct;
    strncpy(s_text, text, sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = '\0';
    s_ack_wanted = false;   // this frame IS the acknowledgement a retry was waiting for
    send_rep(NODE_PROG, s_text);
}

static void fail(const char *why) { publish(NODE_PH_FAIL, NODE_PCT_NONE, why); }

// LOG_RATE frames a second and not one more, whatever the ring holds. Called from the worker's
// tick and from inside the download loop, so a verbose install still streams its own log.
static void log_drain(void) {
    static uint32_t win_ms = 0;
    static int sent = 0;

    uint32_t now = millis();
    if (now - win_ms >= 1000) {
        win_ms = now;
        sent = 0;
    }

    while (sent < LOG_RATE) {
        char line[NODEPROTO_TEXT];
        bool have = false;
        uint16_t dropped;

        portENTER_CRITICAL(&s_log_mux);
        dropped = s_log_dropped;
        s_log_dropped = 0;
        if (dropped == 0 && s_log_tail != s_log_head) {
            memcpy(line, s_log[s_log_tail], sizeof(line));
            s_log_tail = (uint8_t)((s_log_tail + 1) % LOG_SLOTS);
            have = true;
        }
        portEXIT_CRITICAL(&s_log_mux);

        // The count goes out ahead of the backlog, not after it. Under a sustained flood the ring
        // never empties, so a notice that waited for an empty ring would never be sent - and the
        // one thing a reader needs to know about a truncated log is that it is truncated.
        if (dropped != 0) {
            snprintf(line, sizeof(line), "[node] %u log line(s) dropped, radio rate limit",
                     (unsigned)dropped);
        } else if (!have) {
            break;
        }
        send_rep(NODE_LOG, line);
        sent++;
    }
}

// ---- small helpers, lifted from fwpull.cpp ----------------------------------------------------

// Split "http://host:port/prefix". Anything that is not http:// with a host is a typo, and
// guessing at a typo points a firmware install at a server that does not exist.
static bool parse_base_url(const char *url) {
    s_host[0] = s_prefix[0] = '\0';
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

static const char HEXDIG[] = "0123456789abcdef";

// The first 8 bytes of app_elf_sha256 as 16 lowercase hex characters plus a NUL; `out` holds 17.
// Eight and not thirty-two because that is what NodeRepMsg.elf_sha carries and therefore what the
// panel can display beside this board's row - comparing on more than the panel can show would
// give two answers to "am I running that image" and one of them would be invisible.
static void hex8(const uint8_t *in, char *out) {
    for (int i = 0; i < 8; i++) {
        out[i * 2]     = HEXDIG[in[i] >> 4];
        out[i * 2 + 1] = HEXDIG[in[i] & 0x0F];
    }
    out[16] = '\0';
}

static void lowercase(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'F') *s = (char)(*s - 'A' + 'a');
    }
}

// Flat lookups. The manifest is five scalar keys at the top level, none a substring of another,
// and the only string values are hex, which can hold neither a quote nor an escape.

// False when the key is absent, or when the value did not fit or was cut short by a truncated
// body - a clipped hash is worse than no hash, because it compares unequal and would reinstall
// the image already running.
static bool json_str(const char *buf, const char *key, char *out, size_t cap) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return false;
    const char *c = strchr(p + strlen(pat), ':');
    if (!c) return false;
    const char *q = strchr(c, '"');
    if (!q) return false;
    q++;
    size_t i = 0;
    while (*q && *q != '"' && i + 1 < cap) out[i++] = *q++;
    out[i] = '\0';
    return *q == '"';
}

// As an integer, not a float: this is a byte count that has to compare exactly against a
// Content-Length, and a comparison that is only exact because of the magnitude it happens to be
// at stops being true without anybody noticing.
static long json_long(const char *buf, const char *key, long dflt) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return dflt;
    const char *c = strchr(p + strlen(pat), ':');
    if (!c) return dflt;
    char *end = nullptr;
    long v = strtol(c + 1, &end, 10);
    return (end && end != c + 1) ? v : dflt;
}

// HTTP/1.0, which has no chunked transfer encoding, so a body always arrives as plain bytes. For
// the image that is not tidiness but the whole check: Update.begin() has to be told the exact size
// before the first byte is written, and a chunked body only reveals its length after the last
// chunk has gone past.
static void write_get_head(WiFiClient &c, const char *path, const char *accept) {
    c.print("GET "); c.print(s_prefix); c.print(path); c.print(" HTTP/1.0\r\n");
    c.print("Host: "); c.print(s_host); c.print(":"); c.print(s_port); c.print("\r\n");
    c.print("User-Agent: SmartFarm-ESP32CAM/1.0\r\n");
    c.print("Accept: "); c.print(accept); c.print("\r\n");
    if (s_tok[0]) { c.print("Authorization: Bearer "); c.print(s_tok); c.print("\r\n"); }
    c.print("Connection: close\r\n\r\n");
    // No flush(). NetworkClient implements flush() as clear(), which empties the RX buffer - so
    // the call reads as "make sure the request is out" while throwing away reply bytes that
    // already arrived. print() writes synchronously, which is the only completion this wanted.
}

// One CRLF-terminated line with the CRLF stripped, or -1 when the socket closed or the budget ran
// out. A line longer than `cap` is clipped and the rest still consumed, so a header this file does
// not read cannot desynchronise the ones it does.
static int read_line(WiFiClient &c, char *out, size_t cap, uint32_t start, uint32_t budget) {
    size_t i = 0;
    for (;;) {
        int ch = c.read();
        if (ch < 0) {
            if (!c.connected()) return -1;
            if (millis() - start > budget) return -1;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (ch == '\n') break;
        if (i + 1 < cap) out[i++] = (char)ch;
    }
    if (i > 0 && out[i - 1] == '\r') i--;
    out[i] = '\0';
    return (int)i;
}

// Status line plus headers, leaving the socket on the first body byte. Returns the HTTP status, 0
// for a reply that is not HTTP, -1 when nothing arrived. `clen` gets the Content-Length, or -1.
static int read_head(WiFiClient &c, uint32_t start, uint32_t budget, long *clen) {
    *clen = -1;
    int status = 0;
    char line[192];
    for (int n = 0;; n++) {
        int len = read_line(c, line, sizeof(line), start, budget);
        if (len < 0) return n == 0 ? -1 : 0;             // died partway through the headers
        if (len == 0) break;                             // the blank line; the body follows
        if (n == 0) {
            const char *sp = strchr(line, ' ');          // "HTTP/1.1 200 OK"
            status = sp ? atoi(sp + 1) : 0;
        } else if (strncasecmp(line, "content-length:", 15) == 0) {
            // Case-insensitively, even though uvicorn emits it lowercase: RFC 9110 says header
            // names are not case-sensitive, and matching the exact bytes is a bug waiting for
            // somebody to put nginx on the site LAN.
            *clen = strtol(line + 15, nullptr, 10);
        }
    }
    return status;
}

// The rest of a small body until the server closes. -1 when it did not fit - a manifest that
// overflows this buffer is not a manifest, and truncating it hands the parser a half-written hash.
static int read_body(WiFiClient &c, char *out, size_t cap, uint32_t start, uint32_t budget) {
    size_t n = 0;
    uint32_t last = millis();
    for (;;) {
        int a = c.available();
        if (a > 0) {
            size_t room = cap - 1 - n;
            if (room == 0) return -1;
            int rd = c.read((uint8_t *)out + n, (size_t)a < room ? (size_t)a : room);
            if (rd > 0) { n += (size_t)rd; last = millis(); }
        } else {
            if (!c.connected()) break;                   // close = body complete
            if (millis() - start > budget) break;
            if (n > 0 && millis() - last > IDLE_MS) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    out[n] = '\0';
    return (int)n;
}

// ---- the update ------------------------------------------------------------------------------

// The role-scoped paths, built once per request. nodeproto_role_name() rather than a literal
// "cam": the server keys its published images off exactly this string, and a second spelling of
// this board's name in the tree is a bug waiting for a grep.
static void role_path(char *out, size_t cap, const char *base) {
    snprintf(out, cap, "%s%s", base, nodeproto_role_name(NODE_ROLE_CAM));
}

// GET the manifest and pull out the three fields that decide anything. Sets the FAIL phase and
// returns false on every unhappy path, so the caller only ever has to check the bool.
static bool fetch_manifest(char *sha, size_t shacap, char *md5, size_t md5cap, long *size) {
    char path[80];
    role_path(path, sizeof(path), NODEPROTO_PATH_LATEST);

    WiFiClient c;
    uint32_t start = millis();
    if (!c.connect(s_host, s_port, CONNECT_MS)) {
        nodeagent_logf("[ota] cannot reach %s:%u\n", s_host, (unsigned)s_port);
        fail("서버 연결 실패");
        return false;
    }
    write_get_head(c, path, "application/json");

    long clen = -1;
    int status = read_head(c, start, REPLY_MS, &clen);
    char body[512];
    int n = (status > 0) ? read_body(c, body, sizeof(body), start, REPLY_MS) : -1;
    c.stop();

    if (status == 404) {
        // The server is up and answering, it just has nothing published for this role. That is an
        // operator saying "no CAM firmware yet", not a fault - and the panel says so rather than
        // showing an error beside a board that is working perfectly well.
        nodeagent_logf("[ota] server has nothing published for role=%s (404)\n",
                       nodeproto_role_name(NODE_ROLE_CAM));
        fail("서버에 펌웨어 없음");
        return false;
    }
    if (status < 200 || status >= 300) {
        nodeagent_logf("[ota] manifest HTTP %d\n", status);
        fail("서버 응답 오류");
        return false;
    }
    if (n < 0) {
        nodeagent_logf("[ota] manifest body did not arrive whole\n");
        fail("서버 응답 이상");
        return false;
    }

    *size = json_long(body, "size", 0);
    if (!json_str(body, "elf_sha256", sha, shacap) || strlen(sha) != 64 ||
        !json_str(body, "md5", md5, md5cap) || strlen(md5) != 32 ||
        *size <= 0) {
        // Checked here rather than trusted downstream. A short hash compares unequal against a
        // correct one and would reinstall the running image every time; a wrong-length md5 is
        // refused by Update.setMD5() far too late, after begin() has claimed the partition.
        nodeagent_logf("[ota] manifest is not the shape this expects: %s\n", body);
        fail("서버 응답 이상");
        return false;
    }
    lowercase(sha);
    lowercase(md5);
    return true;
}

// Declared here because the download loop services debug verbs at its yield point - see there.
static void handle_debug(const char *verb);
static void poll_debug(void);

// GET the image and write it into the inactive slot. True only when the image is installed and
// verified; the caller restarts. False leaves the FAIL phase set and the caller puts the camera
// back.
static bool download_and_install(const char *md5, size_t size) {
    char path[80];
    role_path(path, sizeof(path), NODEPROTO_PATH_IMAGE);

    WiFiClient c;
    uint32_t start = millis();
    if (!c.connect(s_host, s_port, CONNECT_MS)) {
        nodeagent_logf("[ota] cannot reach %s:%u for the image\n", s_host, (unsigned)s_port);
        fail("서버 연결 실패");
        return false;
    }
    write_get_head(c, path, "application/octet-stream");

    long clen = -1;
    int status = read_head(c, start, REPLY_MS, &clen);
    if (status != 200) {
        c.stop();
        nodeagent_logf("[ota] image HTTP %d\n", status);
        fail("서버 응답 오류");
        return false;
    }
    if (clen != (long)size) {
        // Before a single byte is written, not after. The manifest and the image are two requests
        // against a file an operator can replace between them, and an image that is not the one
        // the hash and the md5 describe must never reach Update.begin() - past that point the
        // inactive slot is being erased for something nobody vouched for.
        c.stop();
        nodeagent_logf("[ota] image is %ld bytes, manifest said %u - refusing\n",
                       clen, (unsigned)size);
        fail("크기 불일치");
        return false;
    }

    // PSRAM, because internal DRAM is what the flash driver and the MD5 under Update.write() are
    // about to need - and on this board internal DRAM is already the scarce half, with the WiFi
    // stack and two socket paths living in it. Taken here and not at init: this runs at most once
    // between reboots.
    uint8_t *buf = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        c.stop();
        nodeagent_logf("[ota] no PSRAM for a %u byte chunk buffer\n", (unsigned)CHUNK);
        fail("메모리 부족");
        return false;
    }

    if (!Update.begin(size, U_FLASH)) {
        nodeagent_logf("[ota] Update.begin(%u) refused: %s\n", (unsigned)size, Update.errorString());
        heap_caps_free(buf);
        c.stop();
        fail("설치 시작 실패");
        return false;
    }
    // After begin() and never before: begin() clears the expected md5 as part of its reset, so a
    // setMD5() in front of it is silently thrown away and the transfer goes unverified.
    if (!Update.setMD5(md5)) {
        nodeagent_logf("[ota] Update.setMD5(%s) refused\n", md5);
        Update.abort();
        heap_caps_free(buf);
        c.stop();
        fail("설치 시작 실패");
        return false;
    }

    nodeagent_logf("[ota] installing %u bytes, md5=%s\n", (unsigned)size, md5);
    publish(NODE_PH_DL, 0, "내려받는 중");

    size_t   got = 0;
    uint32_t last = millis();
    uint32_t last_prog_ms = millis();
    uint8_t  last_prog_pct = 0;
    bool     timed_out = false;

    while (got < size) {
        size_t want = size - got;
        if (want > CHUNK) want = CHUNK;
        size_t n = 0;
        while (n < want) {
            int a = c.available();
            if (a > 0) {
                int rd = c.read(buf + n, want - n);
                if (rd > 0) { n += (size_t)rd; last = millis(); }
            } else {
                if (!c.connected()) break;               // the server closed early
                if (millis() - last > STALL_MS) break;
                // Two ticks while starved as well as after a write. This inner loop can spin for a
                // whole RTT waiting on the next TCP segment, and on this board an RTT is measured
                // in hundreds of milliseconds often enough to matter to the watchdog.
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
        if (n == 0) break;
        if (Update.write(buf, n) != n) break;
        got += n;

        uint8_t pct = (uint8_t)((got * 100) / size);
        uint32_t now = millis();
        // Both conditions, and both earn their place. The percentage step keeps the bar moving on
        // a fast link without spending a frame per 4KB chunk; the keepalive is what stops the
        // panel calling a slow-but-alive download a stall, because its nodeota_tick() fails a
        // node in NODE_PH_DL after 20 seconds with no NODE_PROG and this link can go that long
        // without advancing a whole percent.
        if (pct >= last_prog_pct + PROG_STEP_PCT || now - last_prog_ms >= PROG_KEEPALIVE_MS) {
            last_prog_pct = pct;
            last_prog_ms = now;
            publish(NODE_PH_DL, pct, "내려받는 중");
        }

        // The same yield point services the panel's debug verbs and the log ring, so "reboot" can
        // still rescue a wedged install and "log on" can watch one happening. Anything that needs
        // to reach this board while it is busy has to be here, because the worker is one task and
        // this loop owns it for the length of the download.
        poll_debug();
        log_drain();

        if (millis() - start > IMAGE_BUDGET_MS) {
            timed_out = true;
            break;
        }

        // Two ticks, not one, and not none. vTaskDelay(1) wakes at the NEXT tick boundary, so it
        // can be almost no time at all, while the Update.write() above holds the CPU for tens of
        // milliseconds erasing a sector; the panel has already taken a task-watchdog reset from a
        // task that yielded a single millisecond through this same flash driver. Two ticks is a
        // guaranteed full tick of slack for the idle task.
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    heap_caps_free(buf);
    c.stop();

    if (got != size) {
        // errorString() first, abort() second. abort() calls _abort(UPDATE_ERROR_ABORT), which
        // overwrites whatever Update actually failed on - asking afterwards reports the cleanup
        // instead of the cause. When the socket died rather than the write, this reads "No Error"
        // and the byte counts carry the story.
        nodeagent_logf("[ota] stopped at %u/%u bytes after %lums: %s\n",
                       (unsigned)got, (unsigned)size, (unsigned long)(millis() - start),
                       Update.errorString());
        Update.abort();
        fail(timed_out ? "전송 시간 초과" : "전송 끊김");
        return false;
    }

    publish(NODE_PH_DL, 100, "설치 확인 중");
    if (!Update.end(false)) {
        // No abort() here: end() has already run _abort() internally on the path that failed - an
        // md5 mismatch, or a short image - so the state is released and a second abort would do
        // nothing but replace the error this line is printing.
        nodeagent_logf("[ota] install failed: %s\n", Update.errorString());
        fail("설치 실패");
        return false;
    }
    return true;
}

static void run_update(const char *base, const char *token) {
    // The very first act, ahead of every check and every socket. The panel resends NODE_UPDATE
    // three times 700ms apart until it sees a non-idle phase, and a node that spends four seconds
    // failing to connect before it says anything collects all three resends first.
    publish(NODE_PH_ASK, NODE_PCT_NONE, "확인하는 중");

    if (esp_ota_get_next_update_partition(NULL) == NULL) {
        // NODEF_CAN_OTA already told the panel this and it should have greyed the button out, but
        // a command that arrives anyway - an older panel, or a push from the server - gets an
        // honest refusal rather than a half-started install against a partition that is not there.
        nodeagent_logf("[ota] no second app partition; this image cannot install anything\n");
        fail("설치 공간 없음");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        nodeagent_logf("[ota] no WiFi link\n");
        fail("네트워크 없음");
        return;
    }
    if (!parse_base_url(base)) {
        nodeagent_logf("[ota] cannot parse server base URL '%s'\n", base);
        fail("서버 주소 오류");
        return;
    }
    strncpy(s_tok, token, sizeof(s_tok) - 1);
    s_tok[sizeof(s_tok) - 1] = '\0';
    nodeagent_logf("[ota] server=%s:%u%s\n", s_host, (unsigned)s_port, s_prefix);

    char want_sha[65], want_md5[33];
    long want_size = 0;
    if (!fetch_manifest(want_sha, sizeof(want_sha), want_md5, sizeof(want_md5), &want_size)) {
        return;   // fetch_manifest already published why
    }

    // The comparison the whole thing turns on. app_elf_sha256 out of the running image's
    // descriptor against the one the server computed from the .bin - not the version string, which
    // is "76b7a3f" in every image this project has ever built because it describes the Arduino
    // core and not our code, and which would therefore report a match forever.
    char have_sha[17];
    hex8(esp_app_get_description()->app_elf_sha256, have_sha);
    nodeagent_logf("[ota] running elf=%s server elf=%.16s (%ld bytes)\n",
                   have_sha, want_sha, want_size);
    if (strncmp(have_sha, want_sha, 16) == 0) {
        publish(NODE_PH_CURRENT, NODE_PCT_NONE, "이미 최신");
        return;
    }

    // ONLY NOW does the board go dark, and the ordering is the point. Everything above is one
    // small GET and a string compare, none of which needs a quiet radio - and the panel's fwpull
    // learned this the hard way, taking a board over behind a takeover screen to deliver the
    // answer "cannot reach the server". Every way of finding out there is nothing to install
    // leaves this camera streaming.
    if (!camstream_stand_down()) {
        fail("카메라 정지 실패");
        return;
    }

    bool ok = download_and_install(want_md5, (size_t)want_size);
    if (!ok) {
        // Put the camera back rather than reboot. A reboot would clear whatever wedged the
        // transfer, but it also throws away the only thing anybody gets out of a failed update -
        // the phase and the reason sitting on the panel's screen - and it costs the live feed a
        // further fifteen seconds of association for no gain. The board that just refused to
        // install an image is still a perfectly good camera.
        camstream_resume();
        return;
    }

    publish(NODE_PH_DONE, 100, "다시 시작합니다");
    nodeagent_logf("[ota] installed; restarting\n");
    // Two things happen in this pause and both need it: the panel paints 100% and drops its
    // overlay on a DONE rather than on a silence, and the NODE_PROG above actually leaves the
    // radio. esp_restart() from inside the send would drop it.
    delay(400);
    esp_restart();
}

// ---- debug verbs -------------------------------------------------------------------------------

static void dump_state(void) {
    char sha[17];
    hex8(esp_app_get_description()->app_elf_sha256, sha);
    uint8_t f = current_flags();

    statef("[node] role=%s elf=%s up=%lus heap=%u psram=%u\n",
           nodeproto_role_name(NODE_ROLE_CAM), sha,
           (unsigned long)(esp_timer_get_time() / 1000000),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    statef("[node] flags=%s%s%s%s phase=%u pct=%u last=\"%s\"\n",
           (f & NODEF_WIFI) ? "WIFI " : "",
           (f & NODEF_PENDING) ? "PENDING " : "",
           (f & NODEF_CAN_OTA) ? "CAN_OTA " : "",
           (f & NODEF_VERBOSE) ? "VERBOSE" : "",
           (unsigned)s_phase, (unsigned)s_pct, s_text);

    statef("[node] wifi=%s ip=%s rssi=%ddBm ssid=%s\n",
           WiFi.status() == WL_CONNECTED ? "up" : "down",
           WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), WiFi.SSID().c_str());

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *nxt = esp_ota_get_next_update_partition(NULL);
    statef("[node] part running=%s next=%s\n",
           run ? run->label : "?", nxt ? nxt->label : "none (single-app table)");

    char sum[NODEPROTO_TEXT];
    camstream_summary(sum, sizeof(sum));
    statef("[node] %s\n", sum);
}

static void handle_debug(const char *verb) {
    if (strcmp(verb, "log on") == 0) {
        s_verbose = true;
        nodeagent_logf("[node] verbose log on\n");   // already verbose, so this line goes out too
        return;
    }
    if (strcmp(verb, "log off") == 0) {
        // Announced while verbose is still set, so the last line reaches the panel. Clearing the
        // flag first would make "log off" the one command that never confirms itself, and the
        // log going quiet is indistinguishable from the node going quiet.
        nodeagent_logf("[node] verbose log off\n");
        s_verbose = false;
        return;
    }
    if (strcmp(verb, "state") == 0) {
        dump_state();
        return;
    }
    if (strcmp(verb, "reboot") == 0) {
        statef("[node] reboot on panel request\n");
        // A half-written OTA slot is harmless - the boot partition is not switched until
        // Update.end() - but releasing it keeps the next attempt from meeting a busy UpdateClass.
        if (Update.isRunning()) Update.abort();
        // Long enough for the line above to get on the air at LOG_RATE, and short enough that the
        // panel sees a reboot rather than a node that ignored it.
        for (int i = 0; i < 6; i++) {
            log_drain();
            delay(50);
        }
        esp_restart();
    }
    // A newer panel talking to an older node must degrade to "nothing happened" and say so, not to
    // undefined behaviour.
    nodeagent_logf("[node] unknown debug verb '%s' - ignored\n", verb);
}

// ---- command intake -----------------------------------------------------------------------------

// Take whatever the recv callback left. The claim is recorded BEFORE the slot is released, so
// there is no instant in which a retry of this command looks new.
static bool claim_cmd(NodeCmdMsg *out) {
    if (!s_cmd_pending) return false;
    *out = s_cmd;
    s_exec_seq  = out->seq;
    s_exec_ms   = millis();
    s_have_exec = true;
    s_cmd_pending = false;
    return true;
}

// Debug verbs only, for calling from inside the download loop. A NODE_UPDATE is left in the slot
// rather than run - though the callback already refuses one while a phase is busy, so the only way
// to reach that branch is a bug, and leaving the command alone is the harmless half of it.
static void poll_debug(void) {
    if (!s_cmd_pending || s_cmd.kind != NODE_DEBUG) return;
    NodeCmdMsg c;
    if (!claim_cmd(&c)) return;
    c.text[sizeof(c.text) - 1] = '\0';
    handle_debug(c.text);
}

static void handle_cmd(const NodeCmdMsg *c) {
    if (c->ver != NODEPROTO_VER) {
        // Said on the screen and not only on a log line nobody has turned on. Two boards flash
        // independently and a greenhouse can easily run a month with one of them behind, so
        // "these two do not speak the same protocol" is a state a grower has to be able to see.
        nodeagent_logf("[node] command protocol v%u, this image speaks v%u - ignored\n",
                       (unsigned)c->ver, (unsigned)NODEPROTO_VER);
        fail("프로토콜 버전 불일치");
        return;
    }

    char text[NODEPROTO_URL];
    memcpy(text, c->text, sizeof(text));
    text[sizeof(text) - 1] = '\0';

    switch (c->kind) {
        case NODE_UPDATE: {
            char tok[NODEPROTO_TOKEN];
            memcpy(tok, c->token, sizeof(tok));
            tok[sizeof(tok) - 1] = '\0';
            run_update(text, tok);
            break;
        }
        case NODE_DEBUG:
            handle_debug(text);
            break;
        default:
            nodeagent_logf("[node] unknown command kind %u - ignored\n", (unsigned)c->kind);
            break;
    }
}

void nodeagent_on_cmd(const uint8_t *mac, const uint8_t *data, int len) {
    // The sender's address is deliberately unused: reports go to the broadcast peer provision.cpp
    // already maintains, and the panel keeps our MAC from the recv info to unicast back. Knowing
    // each other's addresses in both directions would be two things to re-learn after every
    // re-provision instead of none.
    (void)mac;

    if (len != (int)sizeof(NodeCmdMsg)) return;
    NodeCmdMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != NODE_MAGIC) return;
    if (m.role != NODE_ROLE_CAM) return;   // addressed to the sensor node, which hears this too

    // A repeat of the command we are already running or have just run. Answering it with a fresh
    // report is the whole point: the panel retries because it has not seen a phase come back, and
    // a silent drop makes it retry twice more and then declare a node that is working failed.
    if (s_have_exec && m.seq == s_exec_seq && millis() - s_exec_ms < DEDUP_MS) {
        s_ack_wanted = true;
        return;
    }
    // A second update while one is running. Same answer, and it must be here rather than in the
    // worker: the worker is inside download_and_install() for the whole transfer, so a command
    // queued now would be executed the moment it finished - one press, two installs.
    if (m.kind == NODE_UPDATE && nodeproto_phase_busy(s_phase)) {
        s_ack_wanted = true;
        return;
    }
    if (s_cmd_pending) return;   // the worker has not taken the last one yet; the panel will retry

    s_cmd = m;
    s_cmd_pending = true;
}

// ---- the worker ---------------------------------------------------------------------------------

static void nodeagent_task(void *arg) {
    (void)arg;

    // Straight away, not after the first HELLO_MS. A board that has just installed an image has
    // told the panel NODE_PH_DONE and then vanished, and the panel is counting: the sooner the
    // first report after a reboot lands, the smaller the window in which a successful update looks
    // like a board that never came back.
    uint32_t hello_ms = millis() - HELLO_MS;

    for (;;) {
        NodeCmdMsg cmd;
        if (claim_cmd(&cmd)) handle_cmd(&cmd);

        if (s_ack_wanted) {
            s_ack_wanted = false;
            send_rep(NODE_PROG, s_text);   // the phase we are actually in, whatever it is
        }

        // Held up entirely while an update runs, which is what keeps nodeproto.h's promise that
        // IDLE is the only phase HELLO ever carries. During a download the progress frames are the
        // heartbeat, and they are more frequent than this one.
        if (!nodeproto_phase_busy(s_phase) && millis() - hello_ms >= HELLO_MS) {
            hello_ms = millis();
            send_rep(NODE_HELLO, "");
        }

        log_drain();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void nodeagent_start(void) {
    s_host[0] = s_prefix[0] = s_tok[0] = '\0';

    // Core 1, and this is the opposite of the panel's choice for a reason. fwpull pins to core 0
    // to stay off LVGL; here core 0 is where the WiFi stack lives and where provision, rtsp and
    // http already run, and the WiFi stack is the scarce thing on this board - the whole of
    // main.cpp's header is the story of what starving it costs. During a download core 1 is
    // otherwise idle, because the capture loop is parked and the camera is off, so the transfer
    // gets a core to itself and leaves the radio's alone.
    //
    // 8192 bytes: the same path fwpull needed 6144 for - the flash driver and the MD5 under
    // Update - plus this file's 384-byte format buffer and a 512-byte manifest body.
    //
    // Priority 5, matching fwpull's and above rtsp/http at 2: an install that loses a race does
    // not fail slowly, it fails partway through, and while one is running nothing else here
    // matters. Idle, the task wakes every 50ms to check a flag and sleeps again.
    xTaskCreatePinnedToCore(nodeagent_task, "nodeagent", 8192, nullptr, 5, nullptr, 1);
}
