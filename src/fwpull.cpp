// "Update me to whatever the server has."
//
// WHY THIS EXISTS, when the board could already be updated over the network. ota.cpp listens for
// `pio run -t upload` and that works - it is measured and it is the path every image so far has
// arrived by. What it needs is a laptop carrying the PlatformIO toolchain, this source tree, and
// a route to the panel. The person standing in front of a greenhouse at seven in the morning
// because the panel is showing last month's thresholds has none of the three. Push puts the
// update in the hands of whoever holds the build; pull puts it in the hands of whoever is
// standing there. Those are different people, and it is the second one who notices.
//
// So the panel asks instead: what is the newest image, am I already running it, and if not,
// fetch it and write it to the other slot. Two GETs and a flash write. The interesting part is
// not the transfer, it is the two questions underneath it.
//
// WHAT COUNTS AS "THE NEWEST IMAGE", which is the part that is easy to get quietly wrong.
// PlatformIO does not stamp an app version into the image. Read the esp_app_desc_t out of a
// firmware.bin this project actually built and it says version "76b7a3f", project_name
// "arduino-lib-builder", and a date/time from whenever Espressif built the framework - all
// identical in every image this repo produces, because they describe the Arduino core and not
// our code. A client comparing version strings would find them equal after every single build
// and would therefore never update, while looking completely healthy: "already up to date" is
// what a working client says most of the time, so nothing about the failure would look wrong.
//
// The one field in that descriptor that does move is app_elf_sha256 - the hash of the ELF the
// image was linked from, which the toolchain writes in and which differs for any change to our
// sources. So that is the identity on both sides. This file reads its own out of
// esp_app_get_description(); the server reads the candidate's straight out of the .bin, where
// the descriptor sits at file offset 0x20 and app_elf_sha256 is 32 raw bytes 0x90 into it.
//
// AND IT IS NOT A VERSION: it answers "is this the same image" and it has no order, which is a
// limit that cost this project twice. Different, from a hash, covers a rebuild of unchanged
// source - so a board already running the newest code reinstalled it - and a local build flashed
// over USB, which is NEWER than anything published and which this button therefore overwrote. So
// the hash is no longer asked first. scripts/fwver.py compiles `git describe` in as FW_VERSION,
// the server publishes its own beside the hash, and shared/fwver.h orders the two: same or
// behind means there is nothing to do and the hash is never consulted. The hash keeps the case
// it is good for - one or both sides having no version at all, where it decides alone exactly as
// it did before. pull_once() has the ladder.
//
// WHY THIS IS SAFE TO SHIP AS SHORT AS IT IS. Nothing here retries and nothing here reboots on
// failure, because two things already in the tree make that unnecessary. Update mode
// (updatemode.cpp) is entered before a single byte is fetched, and it restarts the board after
// five idle minutes, so a pull that hangs somewhere ends by itself. And health.cpp puts a newly
// written image on probation: three failed boots and the bootloader goes back to the previous
// slot, which is measured. A bad image is therefore survivable without a walk to the greenhouse,
// and this file does not have to be the thing that guarantees it.
#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fwpull.h"
#include "sitecfg.h"          // the server address and shared secret, NVS-backed
#include "health.h"
#include "hlog.h"
#include "net.h"
#include "updatemode.h"
#include "srvurl.h"    // the server address, parsed once for all three firmwares
#include "srvconn.h"   // and the socket that address opens, plain or TLS as the scheme said
#include "nodeproto.h"        // the firmware paths, shared with the two node firmwares
#include "fwver.h"            // ordering the version strings, shared with the two node firmwares

// scripts/fwver.py injects this on every build of this project (see extra_scripts in
// platformio.ini). Defined away to the empty string if it ever isn't, because empty is already a
// state this file handles - fwver_cmp() calls it FWVER_UNKNOWN and the decision goes back to the
// elf hash below - and a missing version must degrade to the old behaviour rather than refuse to
// compile. A build with no git and a build with no fwver.py should reach the same panel.
#ifndef FW_VERSION
#define FW_VERSION ""
#endif

static const uint32_t CONNECT_MS = 4000;
// How long the server gets to produce a whole small reply - the manifest, or the headers in
// front of the image. Both are answered off a stat() and a dict, so anything slower than this is
// a server that is not going to answer at all.
static const uint32_t REPLY_MS = 8000;
// A gap this long inside the manifest body means the body ended; see plantrx.cpp, which needs
// the same rule because HTTP/1.0 signals the end of a body by closing and a close can be slow to
// notice.
static const uint32_t IDLE_MS = 4000;
// The image is 2.5MB across the same WiFi the camera usually saturates, so a slow stretch is
// normal and only a dead one is a failure. Ten seconds with no byte at all is dead - and the
// panel would rather stop and say so than sit at 61% until update mode's five minutes run out
// with no explanation on the screen.
static const uint32_t STALL_MS = 10000;

// 4KB, matching SPI_FLASH_SEC_SIZE and UpdateClass's own internal buffer. Update.write() copies
// into that buffer and flushes it a sector at a time, so a 4KB write lands as exactly one flash
// write with nothing carried over into the next call - which is what makes the yield below sit
// between erases instead of in the middle of one.
static const size_t CHUNK = 4096;

// ---- state ------------------------------------------------------------------

// Set by fwpull_request() on whichever task pressed the button; cleared by the worker when it
// picks the request up. Separate from s_running so that fwpull_active() can cover the gap.
static volatile bool s_request = false;
static volatile bool s_running = false;
static volatile int  s_progress = -1;

// The panel reads this from the LVGL task on core 1 while the worker writes it from core 0. It
// is a pointer to a string literal and never a buffer: a reader can only ever see the old
// pointer or the new one, and both point at something that stays valid for the life of the
// image. A shared char[] would have to be written under a lock or be read half-updated, and a
// status line is not worth a lock.
//
// AND IT IS EMPTY IF AND ONLY IF NO PULL HAS EVER BEEN REQUESTED. That is a contract, not an
// accident of the initialiser: every exit from fwpull_request() leaves this non-empty - the
// refusal path included - and nothing ever puts it back. updatemode.cpp's takeover screen
// depends on it to decide whether to caption the countdown with the outcome of a pull, and the
// alternative it would otherwise need is to sample fwpull_active() on a 250ms timer, which
// misses a fast "이미 최신" round trip about one press in five. Anything added here that clears
// the status breaks that screen silently.
static const char *volatile s_status = "";

// The same state as a NodePhase, and the reason this file now carries two of them is that a
// phrase is written for a person while a phase is what code can decide with. page_update.cpp
// paints a card's hint line from the phase: an already-current answer is a finished, successful
// check and reads green, a give-up reads as an ordinary line. It could do that for the CAM and
// the sensor node because their reports carry NodePhase, and it could not do it for the panel,
// because the panel reported in prose only - so this board's own "이미 최신" came out grey beside
// two green ones saying the same thing on the same screen. Classifying the words instead would
// mean the panel matching on its own Korean, which goes stale the first time the wording is
// edited and is exactly what shared/nodeproto.h took the wording away from each board to stop.
//
// IDLE IF AND ONLY IF s_status IS EMPTY, which is the same contract stated above in the other
// vocabulary: no pull has ever been requested. Nothing puts either of them back afterwards, so a
// finished pull's answer stays readable on the card for as long as the board is up.
//
// Written AFTER s_status at every one of those sites, never before. The worker is on core 0 and
// the panel reads both from LVGL on core 1 with no lock between them - a status line is not worth
// one, per the note above - so a refresh can land between the two stores. In this order that
// frame shows the new phrase in the old colour; in the other order it would show the OLD PHRASE
// in the new colour, which is a card claiming a stale sentence is the fresh answer.
static volatile uint8_t s_phase = NODE_PH_IDLE;

static char s_why[32] = "";
static TaskHandle_t s_task = nullptr;

// The server address, parsed here into this module's OWN SrvUrl rather than borrowed from
// plantrx_srv_url(). Two reasons, and the second is why it survived the parser moving to
// shared/srvurl.h. One: fwpull_init() runs before plantrx_init() in main.cpp, so borrowing would
// read a host that has not been parsed yet and a configured panel would report no server for pull
// updates. Two: it leaves this file able to be pointed somewhere else without touching the uplink
// - and a shared mutable host is how two files come to disagree about where the server is, which
// produces telemetry landing on one box and firmware coming from another and reads as a server
// fault from every angle except this one.
static bool     s_configured = false;
static SrvUrl   s_url;
// Borrowed from sitecfg rather than copied, for the reason plantrx.cpp gives at its own s_tok:
// the buffer outlives this file and never changes after init.
static const char *s_tok = "";

// ---- small helpers ----------------------------------------------------------

// The parse moved to shared/srvurl.h - see the block at the top of that file for what is accepted
// and why an unparseable URL reads as unconfigured. Kept as this module's OWN SrvUrl rather than
// borrowed from plantrx_srv_url(): fwpull_init() runs before plantrx_init() in main.cpp, so
// borrowing would read a host that has not been parsed yet and turn a configured panel into one
// that reports no server for pull updates.

static const char HEXDIG[] = "0123456789abcdef";

// 32 raw bytes -> 64 lowercase hex characters plus a NUL. `out` must hold 65.
static void hex32(const uint8_t *in, char *out) {
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = HEXDIG[in[i] >> 4];
        out[i * 2 + 1] = HEXDIG[in[i] & 0x0F];
    }
    out[64] = '\0';
}

static void lowercase(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'F') *s = (char)(*s - 'A' + 'a');
    }
}

// ---- JSON ------------------------------------------------------------------
//
// Flat lookups, the same ones plantid.cpp uses. plantrx.cpp needed a structural walker because
// "at" and "head" repeat inside nested rows there, and its own comment says so explicitly - the
// strstr form is enough for a flat reply. This manifest is six scalar keys at the top level, none
// of them a substring of another, and that is the flat case exactly.
//
// FIVE OF THE SIX VALUES ARE HEX OR A NUMBER, which can hold neither a quote nor an escape. The
// sixth is "version", and what the server puts there is a git tag - the character set `git
// describe` emits contains no quote and no backslash either, so the same reasoning covers it.
// What makes that safe rather than merely true today is what happens when it stops being true:
// json_str() ends a value at the first quote it meets, so a value that somehow carried one would
// arrive clipped, and a clipped version is not a parseable version - it lands in FWVER_UNKNOWN
// and the decision goes back to the hash. The failure direction is the old behaviour, which is
// not something the hash fields can say about themselves.

// "key":"value" -> value. False when the key is absent, or when the value did not fit or was cut
// short by a truncated body - a clipped hash is worse than no hash, because it compares unequal
// and would trigger a reinstall of the image already running.
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

// "key": number -> number, as an integer. Not plantid.cpp's json_num, which returns a float: this
// one is a byte count that has to compare exactly against a Content-Length, and while today's
// 2.5MB is inside the range where a float still holds every integer, a comparison that is only
// exact because of the magnitude it happens to be at is the kind of thing that stops being true
// without anybody noticing.
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

// ---- HTTP ------------------------------------------------------------------
//
// Every helper below takes a `Client &` and not a `WiFiClient &`. What it is handed is whichever
// socket the stored scheme called for, and read/write/available/connected are all virtual on
// Client, so a base reference reaches the right one. The one non-virtual call in this area is
// connect(), and it is deliberately not here: SrvConn does the connecting on the concrete type,
// because reaching connect() through a base pointer silently gets the plain implementation - a
// bare TCP connect to 443 with no handshake. See shared/srvconn.h for what that cost the last
// time.

// HTTP/1.0, for the reason plantrx.cpp gives and one more of this file's own. 1.0 has no chunked
// transfer encoding, so a body always arrives as plain bytes - and for the image that is not
// tidiness, it is the whole check. Update.begin() has to be told the exact size before the first
// byte is written, and a chunked body only reveals its length after the last chunk has gone past.
//
// AND ONE WRITE, NOT SEVENTEEN. This was a run of c.print() calls, which over plain TCP is free -
// Nagle coalesces them into one segment. Over TLS every print() becomes its OWN TLS RECORD with a
// header and a MAC of its own, so a ~250-byte head went out as 17 tiny records, and Cloudflare -
// which this server sits behind - resets a connection that does that: MBEDTLS_ERR_NET_CONN_RESET
// (-0x0050) on the first write, a few hundred milliseconds after a handshake that had succeeded.
// plantrx.cpp's write_request_head() has the measurement and the same fix. That failure would land
// hardest here of anywhere: the reset arrives before the status line, so an update check reads as
// "server unreachable" and a panel would sit on an old image with nothing saying why.
//
// 448 bytes, sized from what THIS head can hold rather than copied from plantrx.cpp's 512: a
// 48-byte prefix cap and the 64-byte `path` buffer both callers build, a 64-byte host cap and a
// 5-digit port, the longest Accept this file passes ("application/octet-stream", 24), a 96-byte
// token cap, and 112 bytes of fixed header text. 410 with the NUL, so 448 has room and no more.
static void write_get_head(Client &c, const char *path, const char *accept) {
    char head[448];
    int n = snprintf(head, sizeof(head),
                     "GET %s%s HTTP/1.0\r\n"
                     "Host: %s:%u\r\n"
                     "User-Agent: SmartFarm-ESP32/1.0\r\n"
                     "Accept: %s\r\n",
                     s_url.prefix, path, s_url.host, (unsigned)s_url.port, accept);
    if (s_tok[0] && n > 0 && n < (int)sizeof(head)) {
        n += snprintf(head + n, sizeof(head) - n, "Authorization: Bearer %s\r\n", s_tok);
    }
    if (n > 0 && n < (int)sizeof(head)) {
        n += snprintf(head + n, sizeof(head) - n, "Connection: close\r\n\r\n");
    }
    // Truncation would send a head with no blank line, which reads to the server as a request that
    // never ended. Said out loud rather than silently sent: every field here is capped elsewhere,
    // so this firing means one of those caps moved.
    if (n <= 0 || n >= (int)sizeof(head)) {
        hlogf("[fwpull] request head needs %d bytes, cap is %u\n", n, (unsigned)sizeof(head));
        return;
    }
    c.write((const uint8_t *)head, (size_t)n);
    // No flush(). NetworkClient.h declares it `void flush(); // Print::flush tx` and then
    // implements it as clear(), which empties the RX buffer - so the call reads as "make sure
    // the request is out" while actually throwing away reply bytes that already arrived. The
    // note in plantrx.cpp's post_frame() has the measurement. write() reaches the socket
    // synchronously, which is the only completion this ever wanted.
}

// One CRLF-terminated line with the CRLF stripped. Returns its length, or -1 when the socket
// closed or the budget ran out first. A line longer than `cap` is clipped and the rest of it is
// still consumed, so a header this file does not read cannot desynchronise the ones it does.
static int read_line(Client &c, char *out, size_t cap, uint32_t start, uint32_t budget) {
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

// Reads the status line and the headers, leaving the socket sitting on the first body byte.
// Returns the HTTP status, 0 for a reply that is not HTTP, or -1 when nothing arrived. `clen`
// gets the Content-Length, or -1 when the server did not send one.
static int read_head(Client &c, uint32_t start, uint32_t budget, long *clen) {
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
            // Case-insensitively, even though uvicorn emits it lowercase: a proxy in front of
            // the server is allowed to normalise header names and RFC 9110 says they are not
            // case-sensitive, so matching on the exact bytes would be a bug waiting for someone
            // to put nginx on the site LAN.
            *clen = strtol(line + 15, nullptr, 10);
        }
    }
    return status;
}

// Reads the rest of a small body until the server closes. Returns its length, or -1 when it did
// not fit - a manifest that overflows this buffer is not a manifest, and truncating it would
// hand the parser a half-written hash.
static int read_body(Client &c, char *out, size_t cap, uint32_t start, uint32_t budget) {
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

// ---- the pull ---------------------------------------------------------------

// GET /v1/firmware/latest and pull the four fields that decide anything out of it. The other two
// the server publishes - idf_ver and mtime - are for a person reading the manifest, not for this.
// Returns false with s_status already set to something a grower can read, and s_phase set to the
// phase that line belongs to - every early return below ends this update, so every one of them is
// NODE_PH_FAIL.
//
// `ver` is the one output that is allowed to come back empty on success, and that is not
// laxness: a server that has not been redeployed since the version field was added sends no such
// key, and a hand-copied image gets an empty one from a server that has. Both mean "no ordering
// available here", which is a state pull_once() handles by comparing hashes exactly as it always
// did. Refusing the manifest over it would break firmware pull against every server in the field
// on the day this shipped, to gain nothing.
static bool fetch_manifest(char *sha, size_t shacap, char *md5, size_t md5cap, long *size,
                           char *ver, size_t vercap) {
    SrvConn conn;
    uint32_t start = millis();
    Client *c = conn.connect(s_url, CONNECT_MS);
    if (c == nullptr) {
        // The mbedtls text beside the address, because on a TLS server "cannot reach" now covers
        // two completely different mornings: a host that never answered, and a host that answered
        // and whose certificate did not verify. Empty on the plain path, where there is no second
        // case to tell apart. Read before any stop(), which is where the code would be cleared.
        char why[80];
        conn.last_error(why, sizeof(why));
        s_status = "서버 연결 실패";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] cannot reach %s:%u%s%s\n", s_url.host, (unsigned)s_url.port,
              why[0] ? " - " : "", why);
        return false;
    }
    // Composed from the shared macro and this board's own role rather than spelled out, the same
    // way both node firmwares compose it (sensor_node/src/nodeota.cpp:323). The role is not
    // optional here just because the server defaults it to "panel": that default exists for
    // panels built before the query parameter did, and a build that leans on it is a build whose
    // request stops saying which image it wants.
    char path[64];
    snprintf(path, sizeof(path), "%s%s", NODEPROTO_PATH_LATEST,
             nodeproto_role_name(NODE_ROLE_PANEL));
    write_get_head(*c, path, "application/json");

    long clen = -1;
    int status = read_head(*c, start, REPLY_MS, &clen);
    char body[512];
    int n = (status > 0) ? read_body(*c, body, sizeof(body), start, REPLY_MS) : -1;
    conn.stop();

    if (status == 404) {
        // The server is up and answering, it just has nothing published. That is an operator
        // saying "no firmware yet", not a fault, and the panel says so rather than "error". The
        // phase is NODE_PH_FAIL all the same, because NodePhase has no word for "asked, nothing
        // there" and this is the end of the update either way - the words carry the difference,
        // which is what the words are for. Both node firmwares end their own 404 the same way
        // (sensor_node/src/nodeota.cpp:395).
        s_status = NODE_SAY_NOFW;
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] server has no published firmware (404)\n");
        return false;
    }
    if (status < 200 || status >= 300) {
        s_status = "서버 응답 오류";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] manifest HTTP %d\n", status);
        return false;
    }
    if (n < 0) {
        s_status = "서버 응답 이상";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] manifest body did not arrive whole\n");
        return false;
    }

    *size = json_long(body, "size", 0);
    if (!json_str(body, "elf_sha256", sha, shacap) || strlen(sha) != 64 ||
        !json_str(body, "md5", md5, md5cap) || strlen(md5) != 32 ||
        *size <= 0) {
        // Checked here rather than trusted downstream. A short hash would compare unequal
        // against a correct one and reinstall the running image every time; a wrong length md5
        // is refused by Update.setMD5() far too late, after begin() has already claimed the
        // partition.
        s_status = "서버 응답 이상";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] manifest is not the shape this expects: %s\n", body);
        return false;
    }
    lowercase(sha);
    lowercase(md5);
    // Read after the required fields and never able to fail the manifest, for the reason above
    // the signature. Cleared on a false return rather than left as json_str() found it, because
    // false covers a value that was cut short by a truncated body as well as a key that was never
    // there - and the first six characters of "v0.4.3-1-g0ab2efa" are "v0.4.3", which parses, and
    // orders, and is a different build from the one the server meant. Empty is the one honest way
    // to say "this reply did not carry a version", and it is the only one fwver_cmp() cannot
    // mistake for an answer.
    if (!json_str(body, "version", ver, vercap)) ver[0] = '\0';
    return true;
}

// GET /v1/firmware/image and write it into the inactive slot. Returns only on failure, with
// s_status and s_phase set; on success it restarts and never comes back.
static void download_and_install(const char *sha, const char *md5, size_t size) {
    SrvConn conn;
    uint32_t start = millis();
    Client *c = conn.connect(s_url, CONNECT_MS);
    if (c == nullptr) {
        char why[80];
        conn.last_error(why, sizeof(why));
        s_status = "서버 연결 실패";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] cannot reach %s:%u for the image%s%s\n", s_url.host,
              (unsigned)s_url.port, why[0] ? " - " : "", why);
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "%s%s", NODEPROTO_PATH_IMAGE,
             nodeproto_role_name(NODE_ROLE_PANEL));
    write_get_head(*c, path, "application/octet-stream");

    long clen = -1;
    int status = read_head(*c, start, REPLY_MS, &clen);
    if (status != 200) {
        conn.stop();
        s_status = "서버 응답 오류";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] image HTTP %d\n", status);
        return;
    }
    if (clen != (long)size) {
        // Before a single byte is written, not after. The manifest and the image are two
        // requests against a file an operator can replace between them, and an image that is not
        // the one the hash and the md5 describe must never reach Update.begin() - past that
        // point the inactive slot is being erased for something nobody vouched for.
        conn.stop();
        s_status = "크기 불일치";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] image is %ld bytes, manifest said %u - refusing\n",
              clen, (unsigned)size);
        return;
    }

    // PSRAM, because internal DRAM is what the flash driver and mbedTLS's MD5 underneath
    // Update.write() are about to need. Taken here and not at init: this runs at most once
    // between reboots, and 4KB held permanently for it would be 4KB the camera decode buffers
    // do not get for the months a wall panel actually stays up.
    uint8_t *buf = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        conn.stop();
        s_status = "메모리 부족";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] no PSRAM for a %u byte chunk buffer\n", (unsigned)CHUNK);
        return;
    }

    if (!Update.begin(size, U_FLASH)) {
        hlogf("[fwpull] Update.begin(%u) refused: %s\n", (unsigned)size, Update.errorString());
        heap_caps_free(buf);
        conn.stop();
        s_status = "설치 시작 실패";
        s_phase  = NODE_PH_FAIL;
        return;
    }
    // After begin(), never before: begin() clears the expected md5 as part of its reset, so a
    // setMD5() in front of it would be silently thrown away and the transfer would go unverified.
    if (!Update.setMD5(md5)) {
        hlogf("[fwpull] Update.setMD5(%s) refused\n", md5);
        Update.abort();
        heap_caps_free(buf);
        conn.stop();
        s_status = "설치 시작 실패";
        s_phase  = NODE_PH_FAIL;
        return;
    }

    s_progress = 0;
    // NODE_PH_DL from here to the end of the write, and this is the line the overlay is keyed to:
    // nodeproto_phase_flashing() is true from now on because from now on there are bytes going
    // into the inactive slot. Everything above this point is a question that costs the board
    // nothing, which is why nothing above it says DL.
    s_status = NODE_SAY_DL;
    s_phase  = NODE_PH_DL;
    hlogf("[fwpull] installing %u bytes, md5=%s\n", (unsigned)size, md5);

    size_t got = 0;
    uint32_t last = millis();
    while (got < size) {
        size_t want = size - got;
        if (want > CHUNK) want = CHUNK;
        size_t n = 0;
        while (n < want) {
            int a = c->available();
            if (a > 0) {
                int rd = c->read(buf + n, want - n);
                if (rd > 0) { n += (size_t)rd; last = millis(); }
            } else {
                if (!c->connected()) break;               // the server closed early
                if (millis() - last > STALL_MS) break;
                // Two ticks while starved as well as after a write. This inner loop can spin for
                // a whole RTT waiting on the next TCP segment and it is on the same core as the
                // idle task the watchdog watches.
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
        if (n == 0) break;
        if (Update.write(buf, n) != n) break;
        got += n;
        s_progress = (int)((got * 100) / size);

        // Two ticks, not one, and not none. This board has already taken a task-watchdog reset
        // from a core-0 task that yielded a single millisecond: vTaskDelay(1) wakes at the NEXT
        // tick boundary, so it can be almost no time at all, while the Update.write() above
        // holds the CPU for tens of milliseconds erasing a sector. Two ticks is a guaranteed
        // full tick of slack for the idle task. ota.cpp reached the same number by the same
        // route and for the same reason; this loop does the same work through the same flash
        // driver, so it gets the same yield. The throughput it costs - a couple of seconds
        // across the whole image - is repaid many times over by camnet standing down in update
        // mode, and a transfer that finishes two seconds later beats one that reboots at 24%.
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    heap_caps_free(buf);
    conn.stop();

    if (got != size) {
        // errorString() first, abort() second. abort() calls _abort(UPDATE_ERROR_ABORT), which
        // overwrites whatever Update actually failed on - so asking afterwards would report the
        // cleanup instead of the cause. When the socket died rather than the write, this reads
        // "No Error" and the byte counts carry the story.
        hlogf("[fwpull] stopped at %u/%u bytes: %s\n",
              (unsigned)got, (unsigned)size, Update.errorString());
        Update.abort();
        s_progress = -1;
        s_status = "전송 끊김";
        s_phase  = NODE_PH_FAIL;
        return;
    }

    // Still NODE_PH_DL, following the camera (esp32cam-streamer/src/nodeagent.cpp:815): the bytes
    // are all in but nothing is committed yet, so this is the last moment of the write and not a
    // state of its own - NodePhase has no word for verifying, and inventing one would mean a
    // number the two node firmwares do not send. NODE_PH_DONE waits for end() to say yes, because
    // DONE is what tells the panel a reboot is coming and a failed verify is not followed by one.
    //
    // Restated rather than left standing from the DL above, and the redundant store is the point:
    // it makes "every phrase in this file has a phase beside it" a property a grep can check, and
    // the one site that relied on a phase set eighty lines earlier is the site where the next
    // person adds a phrase and forgets.
    s_status = NODE_SAY_VERIFY;
    s_phase  = NODE_PH_DL;
    if (!Update.end(false)) {
        // No abort() here. end() has already run _abort() internally on the path that failed -
        // an md5 mismatch, or a short image - so the state is released and a second abort would
        // do nothing but replace the error this line is printing.
        hlogf("[fwpull] install failed: %s\n", Update.errorString());
        s_progress = -1;
        s_status = "설치 실패";
        s_phase  = NODE_PH_FAIL;
        return;
    }

    // And that is where this stops. No retry loop, deliberately: everything that gets here has
    // already had its size checked against the manifest and its md5 checked by Update, so a
    // failure means the bytes on the server are not the bytes the manifest describes, or the
    // flash refused them. Neither gets better by doing it again, and a board that reinstalls a
    // broken image in a loop is worse than one that stops - it burns erase cycles and it hides
    // the fault. Update mode's five minutes are what ends the wait; a person can press the
    // button again once somebody has fixed the file.
    s_progress = 100;
    s_status = NODE_SAY_DONE;
    s_phase  = NODE_PH_DONE;
    hlogf("[fwpull] installed elf=%s (%s); restarting\n", sha, s_why);
    // A moment for the panel to paint 100%, the same pause ota.cpp takes and for the same
    // reason: that number is the last thing the person in front of the screen sees.
    delay(400);
    // health_restart(), never esp_restart(). The next boot has to know this reboot was ours or
    // health.cpp counts it as a crash - and a crash counter that ticks on every successful
    // update would make a working board look like a failing one exactly when somebody is
    // watching it. See health.cpp.
    health_restart("firmware pull");
}

static void pull_once(void) {
    if (WiFi.status() != WL_CONNECTED) {
        s_status = "네트워크 없음";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] no WiFi link\n");
        return;
    }

    s_status = NODE_SAY_ASK;
    s_phase  = NODE_PH_ASK;
    char want_sha[65];
    char want_md5[33];
    long want_size = 0;
    // 48, from what the server can actually put here: a tag of the shape this project cuts plus
    // git's own "-N-g<hash>-dirty" suffix is 23 characters, so a release named twice as long as
    // ours still fits. Overflowing it is not a corruption risk - json_str() reports a value that
    // did not fit as a failure and fetch_manifest() clears it - but it would quietly cost the
    // ordering and leave the hash deciding, so the cap sits where no plausible tag reaches it.
    char want_ver[48] = "";
    if (!fetch_manifest(want_sha, sizeof(want_sha), want_md5, sizeof(want_md5), &want_size,
                        want_ver, sizeof(want_ver))) {
        return;
    }

    // TWO QUESTIONS, IN THIS ORDER, and the order is what makes the answer safe.
    //
    // First: which build is later? That is what FW_VERSION and the manifest's "version" are for -
    // `git describe` output on both sides, ordered by shared/fwver.h. It is the only question here
    // with a direction in it, and a direction is precisely what a hash cannot give. Two separate
    // failures came from asking one to. A rebuild of unchanged source moves elf_sha256 and almost
    // nothing else - measured on this project's own releases, where two images with byte-identical
    // sources differ in 68 bytes - so a board already running the newest code downloaded a
    // megabyte to install it a second time. And a USB-flashed local build differs from the
    // published image too, in the OTHER direction, which a hash reports the same way: pressing
    // this button replaced unreleased work with an older release, three times.
    //
    // Second, and only where the first has no answer: is this the same image? The descriptor's own
    // version field cannot help with either question - it reads "76b7a3f" in every image this
    // project has ever built, because it describes the Arduino core rather than our code, per the
    // note at the top of this file - so app_elf_sha256 stays the identity. That comparison is
    // untouched below, which is the compatibility story in one line: a board or a server without a
    // version behaves exactly as it did before any of this was here.
    //
    // Both questions are ONE moment on the hint line, so nothing is restated here. This used to
    // set "확인하는 중" over the "물어보는 중" set above, which is this board describing one fact
    // - it is asking the server and weighing the answer - in two ways to itself, in a line that
    // exists for a second or two. The distinction is real and it belongs in the log, which
    // already draws it: "[fwpull] armed" and then "[fwpull] running elf=" below.
    char have_sha[65];
    hex32(esp_app_get_description()->app_elf_sha256, have_sha);
    hlogf("[fwpull] running elf=%s ver=%s\n", have_sha, FW_VERSION[0] ? FW_VERSION : "(none)");
    hlogf("[fwpull] server  elf=%s ver=%s (%ld bytes)\n", want_sha,
          want_ver[0] ? want_ver : "(none)", want_size);

    const int rel = fwver_cmp(FW_VERSION, want_ver);
    if (rel == FWVER_SAME) {
        // The rebuilt-identical-source case: same release on both sides, whatever the hashes say
        // about the two builds of it. Nothing to install, and nothing has been taken from the
        // panel to find that out - update mode is still below this line.
        s_status = NODE_SAY_CURRENT;
        s_phase  = NODE_PH_CURRENT;
        hlogf("[fwpull] running %s and the server publishes %s - same release; nothing to do\n",
              FW_VERSION, want_ver);
        return;
    }
    if (rel == FWVER_OLDER) {
        // This board is AHEAD of the server, which is what a build flashed over USB looks like
        // from here: it carries commits the published tag does not. Installing would be a
        // downgrade onto work nobody has released, so it does not happen - and the status says
        // which way round it is, because NODE_SAY_CURRENT would tell the person at the panel they
        // are level with the server when they are in front of it, and that is the fact they need
        // to decide whether the release they were waiting for is actually out. NODE_SAY_AHEAD is
        // the other way round and says so about THIS BOARD, never about the server, which is the
        // rule shared/nodeproto.h states for all six phrases.
        //
        // The phase is NODE_PH_CURRENT even so, and both nodes agree (nodeota.cpp:683,
        // nodeagent.cpp:923): the enum answers "was this a finished, successful check with
        // nothing to install", which it was, and the phrase answers which of the two ways it
        // finished. A phase per phrase would put the two facts in one field and then need the
        // enum widened every time a wording is added.
        s_status = NODE_SAY_AHEAD;
        s_phase  = NODE_PH_CURRENT;
        hlogf("[fwpull] running %s, server publishes the older %s - not overwriting it\n",
              FW_VERSION, want_ver);
        return;
    }
    if (rel == FWVER_UNKNOWN) {
        // One of the two is not a version this can order: a server that has not been redeployed
        // since it started publishing the field, an image built from a tree with no tags, a zip
        // with no .git. Said out loud because it is the difference between "the versions were
        // compared and matched" and "there were no versions to compare", and from a distance the
        // two look identical on the screen. The hash below then decides, exactly as it always did.
        hlogf("[fwpull] no version ordering (mine=\"%s\" theirs=\"%s\"); comparing hashes\n",
              FW_VERSION, want_ver);
    }
    // FWVER_NEWER needs no branch: the server has something later, and what happens to something
    // later is everything that already followed this point. The hash is still asked first, because
    // a newer version whose image is byte-for-byte the one running has nothing to install either.

    if (strcmp(have_sha, want_sha) == 0) {
        s_status = NODE_SAY_CURRENT;
        s_phase  = NODE_PH_CURRENT;
        hlogf("[fwpull] already running the published image; nothing to do\n");
        return;
    }

    // ONLY NOW does the board get taken over, and the ordering is the whole point.
    //
    // The first version entered update mode inside fwpull_request(), before any of the checks
    // above had run. Someone pressed the button while the panel's WiFi was still recovering from
    // a burst of resets, the manifest fetch could not connect, and the correct answer - "서버
    // 연결 실패" - was delivered from behind a takeover screen on a board that had already shed
    // its camera feed and its ESP-NOW radio, and which then rebooted five minutes later because
    // update mode has no other exit. The panel was working. Asking it a question cost it.
    //
    // Nothing above this line needs a quiet board: it is one small GET and a string compare. The
    // 2.5MB flash write below is the only part that ever needed the core to itself, and that is
    // what was measured (see updatemode.cpp). So the mode is entered here, once an install is
    // certain, and every way of finding out there is nothing to install now leaves the panel
    // exactly as it was.
    updatemode_enter(s_why);

    download_and_install(want_sha, want_md5, (size_t)want_size);
}

// The worker. One task, its own, on core 0.
static void fwpull_task(void *arg) {
    (void)arg;
    for (;;) {
        if (s_request) {
            // Claim before clearing, so fwpull_active() never blinks false in the gap between
            // the two. The panel polls it to decide whether the takeover screen should be
            // showing a pull, and a single frame of "no pull running" in the middle of one would
            // put the wrong thing on the wall.
            s_running = true;
            s_request = false;
            pull_once();
            s_running = false;
        }
        // A quarter second between looks, where ota.cpp polls at 50ms. Nothing here is racing a
        // handshake: 250ms of latency after a button press is invisible beside the twenty
        // seconds of download behind it, and a task that checks a bool four times a second costs
        // nothing measurable. A notification would be tighter and would save that quarter
        // second exactly once in the life of a board.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ---- public API -------------------------------------------------------------

void fwpull_init(void) {
    memset(&s_url, 0, sizeof(s_url));

    const char *base = sitecfg_base_url();
    s_tok = sitecfg_token();

    if (base[0] == '\0' || !srvurl_parse(base, &s_url)) {
        // No task at all in this case. A worker that can never do anything would still cost its
        // whole stack in internal DRAM, which on this board is the scarce half of memory, and
        // fwpull_request() below turns the request away on its own.
        hlogf("[fwpull] no server configured; pull updates unavailable\n");
        return;
    }
    s_configured = true;

    // Priority 5, matched to ota.cpp rather than picked: an install that loses the race against
    // something else on the board does not fail slowly, it fails partway through, and while one
    // is running nothing else here matters.
    //
    // 8KB of stack and not the 6KB this shipped with, which was ota.cpp's number for ota.cpp's
    // path - the flash driver and mbedTLS's MD5 under Update, where 4KB had been measured as not
    // enough on this core. The pull now opens its socket through SrvConn, and against an https://
    // server that is a TLS handshake on THIS task's stack: 4.4KB of it, measured on this board
    // (shared/srvurl.h has the figures). The two peaks do not overlap - connect() has returned
    // long before Update.begin() is called - so the requirement is the larger of the two and not
    // the sum, but 4.4KB against 6144 leaves about a kilobyte for every frame beneath it, on a
    // board that has already taken one stack-canary reset. hlog.cpp went 4KB -> 8KB for exactly
    // this reason when its push moved into its own task; this is that precedent applied to the
    // one path where an overflow costs a half-written image rather than a missing log line.
    //
    // Core 0, away from LVGL on core 1: the display has to keep painting through the download,
    // which is the only thing that distinguishes an update in progress from a crashed panel to
    // the person waiting in front of it.
    xTaskCreatePinnedToCore(fwpull_task, "fwpull", 8192, nullptr, 5, &s_task, 0);
    hlogf("[fwpull] ready; server=%s:%u%s\n", s_url.host, (unsigned)s_url.port, s_url.prefix);
}

void fwpull_request(const char *why) {
    if (!s_configured || !s_task) {
        // Note what this does NOT do: enter update mode. That mode's only exit is a restart, so
        // taking the board into it for a pull that cannot happen would strand the panel behind a
        // takeover screen for five minutes and give nothing back. Saying why on the status line
        // is the whole of the correct response.
        s_status = "서버 주소 없음";
        s_phase  = NODE_PH_FAIL;
        hlogf("[fwpull] request (%s) but no server is configured\n", why ? why : "");
        return;
    }
    if (fwpull_active()) {
        hlogf("[fwpull] already pulling; ignoring request (%s)\n", why ? why : "");
        return;
    }

    snprintf(s_why, sizeof(s_why), "%s", why ? why : "");
    // Deliberately NOT entering update mode here. Asking the server what it has is one small GET
    // and costs the panel nothing; only the install does, and pull_once() enters the mode at the
    // moment an install becomes certain. Doing it here instead meant a pull that could not even
    // reach the server still cost the panel its camera, its ESP-NOW radio and five minutes behind
    // a takeover screen - see the note above updatemode_enter() in pull_once().
    // Kill any WiFi scan already in flight. A scan leaves the associated channel for its whole
    // sweep, and the settings page - where the button lives - starts one every ten seconds, so the
    // very first press had this pull racing a sweep it could not win: "서버 연결 실패" from a
    // panel whose network was fine a second earlier and fine again a second later. The page has
    // been taught not to start new ones while a pull runs; this closes the one already going.
    net_scan_abort();

    s_progress = -1;
    // The same phrase and the same phase the worker sets when it picks the request up, so the
    // quarter second before it looks (fwpull_task's vTaskDelay) reads as the check it is and not
    // as a press that did nothing.
    s_status = NODE_SAY_ASK;
    s_phase  = NODE_PH_ASK;
    s_request = true;
    hlogf("[fwpull] armed (%s)\n", s_why);
}

bool fwpull_active(void) { return s_request || s_running; }

int fwpull_progress(void) { return s_progress; }

// "" until the first fwpull_request(), non-empty from then on - see the note on s_status. That
// is what lets a caller tell "no pull has happened" from "a pull happened and this is how it
// went" without watching fwpull_active() go by.
const char *fwpull_status(void) { return s_status; }

// NODE_PH_IDLE until the first fwpull_request() and the phase of whatever happened after that -
// the same contract as fwpull_status() above, in the vocabulary the two node firmwares report in.
// It is here so the panel's own card can be classified by the same code that classifies theirs:
// see the note on s_phase for what reading the Korean instead would cost.
uint8_t fwpull_phase(void) { return s_phase; }
