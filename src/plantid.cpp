#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
#include "plantid.h"
#include "camprov.h"
#include "hlog.h"
#include "plantrx.h"
#include "srvurl.h"    // the server address, parsed once for all three firmwares
#include "srvconn.h"   // and the socket that address decides: plain or TLS, never a per-site call
#include "sitecfg.h"          // the same bearer the uplink sends

static const size_t MAX_PHOTO = 250 * 1024;   // CAM UXGA still is ~130-160KB
// The reply is one flat object: a flag, three names, a score and the quota
// triple. A few hundred bytes with the longest names it can carry, and the
// headers land in here too because the status line is read and the headers
// stripped in place rather than into a second buffer. This was 24KB when the
// device parsed PlantNet's own nb-results=3 JSON; none of that crosses the wire
// now, and a buffer sized for a body that no longer exists would hold PSRAM for
// the life of the boot to store nothing.
static const size_t MAX_RESP  = 1024;

// plantrx.cpp's CONNECT_MS. Same server, same LAN, same radio: a connect
// deadline that is right for the uplink is right for this, and a second number
// would only be a second thing to keep in step with the first.
static const uint32_t CONNECT_MS = 4000;
// The server runs the PlantNet call, the Wikipedia langlink lookup and the
// translate fallback inside this window, so the wait is bounded by its round
// trips to the internet and not by the LAN. This file used to allow PlantNet
// alone 25s from here; the work did not get shorter by moving, so the deadline
// does not either.
static const uint32_t RESP_DEADLINE_MS = 25000;
static const uint32_t RESP_IDLE_MS     = 4000;

static volatile PlantIdState s_state = PLANTID_IDLE;
static SemaphoreHandle_t s_trigger;

static uint8_t *s_photo = nullptr;   // PSRAM: the JPEG we upload
static char    *s_resp  = nullptr;   // PSRAM: the JSON reply (NUL-terminated)

static char  s_species[96];
static char  s_common[96];
static char  s_error[96];
static char  s_korean[128];  // Korean name the server resolved ("" if none)
static volatile float s_score = 0.0f;

// The quota, exactly as the server last reported it. The keys live there now,
// so this is a copy of someone else's count and not a total the device can
// compute - which is why a boot starts at -1 / -1 / not-measured rather than at
// a full allowance. Both call sites print 최대 while the flag is false.
static volatile int  s_remaining = -1;
static volatile int  s_quota = -1;
static volatile bool s_measured = false;

static void set_err(const char *msg) {
    strncpy(s_error, msg, sizeof(s_error) - 1);
    s_error[sizeof(s_error) - 1] = '\0';
    s_state = PLANTID_ERR;
}

// --- minimal JSON field extraction (no ArduinoJson) --------------------------

// Copy the first "..."-quoted string found at/after p into out. Handles \-escapes.
static bool first_quoted(const char *p, char *out, size_t outsz) {
    const char *q = strchr(p, '"');
    if (!q) return false;
    q++;
    size_t i = 0;
    while (*q && *q != '"' && i < outsz - 1) {
        if (*q == '\\' && q[1]) q++;  // skip the escape, copy the escaped char
        out[i++] = *q++;
    }
    out[i] = '\0';
    return true;
}

// "key":"value"  -> value. Search starts at buf+from.
static bool json_str(const char *buf, const char *key, char *out, size_t outsz, size_t from) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf + from, pat);
    if (!p) return false;
    const char *c = strchr(p + strlen(pat), ':');
    if (!c) return false;
    return first_quoted(c, out, outsz);
}

// "key": number  -> number. Search starts at buf+from.
static float json_num(const char *buf, const char *key, size_t from) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf + from, pat);
    if (!p) return 0.0f;
    const char *c = strchr(p + strlen(pat), ':');
    if (!c) return 0.0f;
    return (float)atof(c + 1);
}

// "key": true|false -> the bool. Search starts at buf+from. An absent key reads
// as false, which is the safe answer for every flag this reply carries.
static bool json_bool(const char *buf, const char *key, size_t from) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf + from, pat);
    if (!p) return false;
    const char *c = strchr(p + strlen(pat), ':');
    if (!c) return false;
    for (c++; *c == ' '; c++) {}
    return *c == 't';
}

// --- network -----------------------------------------------------------------

// GET http://<cam>/rgb/image into s_photo. Returns JPEG length or -1.
//
// A PLAIN WiFiClient, DELIBERATELY, IN A FILE WHOSE OTHER SOCKET IS A SrvConn. This one talks to
// the ESP32-CAM on the LAN, not to the project server: the camera serves an MJPEG endpoint with no
// certificate, and it is reached by IP address, so a TLS client would have no name to validate
// against even if the camera had one to offer. There is nothing to encrypt here either - no bearer
// goes out on this request, only a GET for a picture, and the same picture crosses the same radio
// unencrypted whenever the monitor page draws the stream. post_identify() below is the socket in
// this file that carries the token, and that one is a SrvConn for exactly that reason. Do not
// "finish the migration" by pointing this at SrvConn: it would break the identification path on
// every board, and the mistake reads as a tidy-up in a diff.
static int http_get_photo(IPAddress ip) {
    WiFiClient c;
    // Milliseconds, not seconds: Stream::setTimeout takes ms, and this one is
    // load-bearing because find() below waits on it. The old value of 6 gave the
    // CAM's headers a 6 ms budget, so the whole identification aborted on any
    // real latency and only ever worked when the reply landed in the first
    // segment. Matched to the connect deadline on the next line.
    c.setTimeout(6000);
    if (!c.connect(ip, 80, 6000)) return -1;
    c.print("GET /rgb/image HTTP/1.1\r\nHost: cam\r\nConnection: close\r\n\r\n");

    uint32_t t0 = millis();
    while (c.connected() && !c.available()) {
        if (millis() - t0 > 8000) { c.stop(); return -1; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!c.find((char *)"\r\n\r\n")) { c.stop(); return -1; }  // skip HTTP headers

    size_t got = 0;
    uint32_t last = millis();
    while (c.connected() || c.available()) {
        int n = c.available();
        if (n > 0) {
            if (got >= MAX_PHOTO) break;
            size_t room = MAX_PHOTO - got;
            int rd = c.read(s_photo + got, (size_t)n < room ? (size_t)n : room);
            if (rd > 0) { got += rd; last = millis(); }
        } else {
            if (millis() - last > 6000) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    c.stop();
    if (got < 100 || s_photo[0] != 0xFF || s_photo[1] != 0xD8) return -1;  // not a JPEG
    return (int)got;
}

// Write the request head. Same shape and the same reasons as plantrx.cpp's
// write_request_head: HTTP/1.0 because 1.0 has no chunked transfer encoding, so
// the reply arrives as plain bytes the strstr scanners above can walk without a
// de-framing pass first; Host carries the port because the server is addressed
// by IP and port; and the bearer goes on only when one is configured, because an
// empty Authorization header is a malformed credential rather than none.
//
// AND ONE WRITE, NOT EIGHTEEN, which is the reason this function was touched at
// all. It used to be a run of c.print() calls. Over plain TCP that is free -
// Nagle coalesces them into one segment - but over TLS every print() becomes its
// OWN TLS RECORD, header and MAC included, so a ~250-byte head left the board as
// 18 tiny records and Cloudflare, which this server sits behind, reset the
// connection: MBEDTLS_ERR_NET_CONN_RESET (-0x0050) on the first write, a few
// hundred milliseconds after a handshake that had succeeded. plantrx.cpp has the
// measurement. Not one header field, value or order below is different; only how
// the bytes reach the socket is.
//
// 448 bytes, sized from what THIS head can hold and not copied from plantrx.cpp:
// a 48-byte prefix cap and a 12-byte path, a 64-byte host cap and a 5-digit
// port, ten digits of Content-Length, a 96-byte token cap, and 185 bytes of
// fixed header text. 406 with the NUL, so 448 is the next round number above it.
static void write_request_head(Client &c, size_t clen) {
    const char *tok = sitecfg_token();
    char head[448];
    int n = snprintf(head, sizeof(head),
                     "POST %s/v1/identify HTTP/1.0\r\n"
                     "Host: %s:%u\r\n"
                     "User-Agent: SmartFarm-ESP32/1.0\r\n"
                     "Accept: application/json\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %u\r\n",
                     plantrx_srv_prefix(), plantrx_srv_host(),
                     (unsigned)plantrx_srv_port(), (unsigned)clen);
    if (tok[0] && n > 0 && n < (int)sizeof(head)) {
        n += snprintf(head + n, sizeof(head) - n, "Authorization: Bearer %s\r\n", tok);
    }
    if (n > 0 && n < (int)sizeof(head)) {
        n += snprintf(head + n, sizeof(head) - n, "Connection: close\r\n\r\n");
    }
    // Truncation would send a head with no blank line, which reads to the server
    // as a request that never ended. Said out loud rather than silently sent:
    // every field here is capped elsewhere, so this firing means one of those
    // caps moved.
    if (n <= 0 || n >= (int)sizeof(head)) {
        hlogf("[pid] request head needs %d bytes, cap is %u\n", n, (unsigned)sizeof(head));
        return;
    }
    c.write((const uint8_t *)head, (size_t)n);
}

// POST s_photo to <server>/v1/identify and read the reply into s_resp with its
// headers stripped. Returns the HTTP status, 0 for a reply whose status line
// could not be read, or -1 for a transport failure.
static int post_identify(size_t photo_len) {
    SrvConn sc;
    // No setTimeout() from here, and nothing left to set: SrvConn's connect() takes its own
    // millisecond deadline and sets the seconds-granularity Stream one the handshake reads, while
    // the read loop below owns the other two. A timeout written from out here would gate nothing
    // and would be in the wrong unit besides.
    const SrvUrl *u = plantrx_srv_url();
    Client *c = sc.connect(*u, CONNECT_MS);
    if (!c) {
        // mbedtls's own words appended when there are any. Gated on the URL's flag and not on
        // sc.tls(), which answers about a live socket and is therefore false on exactly this
        // path - the one path that has a handshake failure to report. last_error() leaves the
        // buffer empty whenever no handshake was attempted, so the plain socket still prints the
        // line it always did and the encrypted one says whether the chain was the problem. On
        // this board that line is the only diagnosis anybody gets.
        char err[96] = "";
        if (u->tls) sc.last_error(err, sizeof(err));
        hlogf("[pid] connect FAIL %s:%u%s%s\n", u->host, (unsigned)u->port,
              err[0] ? " - " : "", err);
        return -1;
    }

    write_request_head(*c, photo_len);
    size_t sent = 0;
    while (sent < photo_len) {
        size_t n = photo_len - sent;
        if (n > 1460) n = 1460;
        size_t w = c->write(s_photo + sent, n);
        if (w == 0) {
            hlogf("[pid] upload FAIL at %u/%u bytes\n", (unsigned)sent, (unsigned)photo_len);
            c->stop();
            return -1;
        }
        sent += w;
    }
    // No flush(), for the reason plantrx.cpp's post_frame() spells out:
    // NetworkClient implements flush() as clear(), which empties the RX buffer
    // instead of pushing the TX one, so the line would discard reply bytes that
    // had already arrived. write() above pushed to the socket synchronously.

    size_t rl = 0;
    uint32_t start = millis(), last = millis();
    for (;;) {
        int a = c->available();
        if (a > 0) {
            size_t room = MAX_RESP - 1 - rl;
            if (room == 0) break;
            int rd = c->read((uint8_t *)s_resp + rl, (size_t)a < room ? (size_t)a : room);
            if (rd > 0) { rl += (size_t)rd; last = millis(); }
        } else {
            if (!c->connected()) break;                       // close = reply complete
            if (millis() - start > RESP_DEADLINE_MS) break;
            if (rl > 0 && millis() - last > RESP_IDLE_MS) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    c->stop();
    s_resp[rl] = '\0';
    if (rl == 0) return -1;

    const char *sp = strchr(s_resp, ' ');       // "HTTP/1.1 NNN ..."
    int status = sp ? atoi(sp + 1) : 0;
    char *body = strstr(s_resp, "\r\n\r\n");
    if (!body) return 0;
    body += 4;
    memmove(s_resp, body, strlen(body) + 1);
    return status;
}

// Send the still, apply the reply.
//
// A failed identification is a field in a valid reply here, not an HTTP error:
// the server answers 200 with the same flat object whether or not it found a
// species, and `reason` carries its Korean words for why not. So the only
// statuses this has to name are the ones the device owns - the transport, and
// the framework's own refusals (401 from the auth dependency, 413 on a body it
// will not take) which never reach the JSON at all.
static void apply_identify(size_t photo_len) {
    int status = post_identify(photo_len);
    if (status < 0) { set_err("서버 연결 실패"); return; }
    if (status != 200) {
        char buf[32];
        snprintf(buf, sizeof(buf), "서버 오류 %d", status);
        set_err(buf);
        return;
    }
    if (!strstr(s_resp, "\"ok\"")) { set_err("서버 응답 오류"); return; }

    // The quota first, because the reply carries it on both branches. The server
    // knows how many calls are left whether or not this one produced a name, and
    // the AI-RX page draws that chip beside a failed identification too. Its
    // `measured` flag crosses the wire rather than being re-derived here: only
    // the side holding the keys knows whether the figure came from a key that
    // answered or from an assumption about one that has not.
    s_remaining = (int)json_num(s_resp, "remaining", 0);
    s_quota     = (int)json_num(s_resp, "quota", 0);
    s_measured  = json_bool(s_resp, "measured", 0);

    if (!json_bool(s_resp, "ok", 0)) {
        // Verbatim: the reason is already the sentence the panel should print,
        // and re-wording it here would put two vocabularies on one card. A false
        // ok with no reason is the server contradicting its own contract, so it
        // lands as a response fault rather than as an empty "식별 실패: " line.
        char reason[96];
        if (json_str(s_resp, "reason", reason, sizeof(reason), 0) && reason[0]) set_err(reason);
        else                                                                    set_err("서버 응답 오류");
        return;
    }

    // Flat, so every field is found from offset 0 and an absent optional one
    // simply leaves its buffer empty - which is what the UI already expects of
    // common and korean.
    s_species[0] = s_common[0] = s_korean[0] = '\0';
    json_str(s_resp, "sci", s_species, sizeof(s_species), 0);
    json_str(s_resp, "common", s_common, sizeof(s_common), 0);
    json_str(s_resp, "korean", s_korean, sizeof(s_korean), 0);
    s_score = json_num(s_resp, "score", 0);
    s_state = PLANTID_OK;
}

static void plantid_task(void *arg) {
    for (;;) {
        xSemaphoreTake(s_trigger, portMAX_DELAY);
        s_state = PLANTID_BUSY;

        if (WiFi.status() != WL_CONNECTED) {
            set_err("WiFi 연결 안됨");
            continue;
        }
        // Before the four second connect timeout, and before naming the network
        // as the fault: an empty host is plantrx_init() having found no usable
        // base URL, which is a panel that was never told where its server is.
        // "서버 연결 실패" would send the reader to the LAN for a problem that
        // lives in provisioning - see sitecfg.h.
        if (!plantrx_srv_host()[0]) {
            set_err("서버 주소 없음");
            continue;
        }
        uint8_t ip4[4];
        if (!camprov_cam_ip4(ip4)) {
            set_err("카메라 IP 모름");
            continue;
        }
        int plen = http_get_photo(IPAddress(ip4[0], ip4[1], ip4[2], ip4[3]));
        if (plen <= 0) {
            set_err("사진 수신 실패");
            continue;
        }
        apply_identify((size_t)plen);  // sets state OK or ERR
    }
}

void plantid_init(void) {
    s_photo = (uint8_t *)heap_caps_malloc(MAX_PHOTO, MALLOC_CAP_SPIRAM);
    s_resp  = (char *)heap_caps_malloc(MAX_RESP, MALLOC_CAP_SPIRAM);
    s_trigger = xSemaphoreCreateBinary();
    if (s_photo == nullptr || s_resp == nullptr || s_trigger == nullptr) {
        return;  // PSRAM exhausted; trigger() becomes a no-op (task never starts)
    }
    // 8KB, and no longer for the reason it was. The old note sized this against a TLS
    // handshake that does not happen here any more - the identification is two plain
    // HTTP exchanges now, the CAM still in and the JPEG back out to the LAN server.
    // The measurement that justified it was 3,260 bytes used across the whole request
    // *including* the mbedTLS session, so the remaining path cannot want more than
    // that, and 8KB is kept rather than trimmed because swapping a stack that has been
    // measured for one that has only been reasoned about is not an improvement worth
    // a boot loop. Core 0 with the other net tasks.
    xTaskCreatePinnedToCore(plantid_task, "plantid", 8192, NULL, 2, NULL, 0);
}

void plantid_trigger(void) {
    if (s_trigger == nullptr) return;
    if (s_state == PLANTID_BUSY) return;   // one at a time (protects the quota)
    xSemaphoreGive(s_trigger);
}

PlantIdState plantid_state(void) { return s_state; }
const char *plantid_species(void) { return s_species; }
const char *plantid_common(void) { return s_common; }
float plantid_score(void) { return s_score; }
const char *plantid_error(void) { return s_error; }
const char *plantid_korean(void) { return s_korean; }

// Three reports of one wire field each, and none of them a computation. Until
// the first reply of a boot lands they read -1 / -1 / false: the device holds no
// keys, so it has nothing to count and nothing to assume on their behalf. A
// false measured is what both call sites already qualify with 최대, which is the
// right sentence for a figure nothing has confirmed yet.
int  plantid_total_remaining(void) { return s_remaining; }
int  plantid_total_quota(void) { return s_quota; }
bool plantid_total_is_measured(void) { return s_measured; }
