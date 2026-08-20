#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "nodeagent.h"
#include "nodeota.h"
#include "nodeproto.h"
#include "srvurl.h"    // the server address, parsed once for all three firmwares

// Every number below is src/fwpull.cpp's, and the panel's comments hold the measurements behind
// them. They are repeated rather than shared because they describe an HTTP conversation against
// the same server, and a node that timed out on a different schedule from the panel would make
// one slow afternoon look like two unrelated faults.
static const uint32_t CONNECT_MS = 4000;
static const uint32_t REPLY_MS   = 8000;   // whole manifest, or the image's headers
static const uint32_t IDLE_MS    = 4000;   // a gap this long inside a small body means it ended
static const uint32_t STALL_MS   = 10000;  // no byte at all for this long is a dead transfer

// 4KB, matching SPI_FLASH_SEC_SIZE and UpdateClass's own internal buffer, so a write lands as
// exactly one flash write with nothing carried into the next call - which is what puts the yield
// below between erases instead of in the middle of one.
static const size_t CHUNK = 4096;

// Three attempts of six seconds. The retry count is provision.cpp's and so is the radio cycle
// between attempts: this AP lets the first authentication after boot expire and then refuses every
// identical retry (reason 202, then 2 - measured on the panel against this same SSID, where a
// plain retry loop failed 19 times in a row), and taking the radio to WIFI_OFF and back is what
// clears it. Six seconds rather than the panel's four because this node associates from a cold
// radio that has been hopping channels, not from a warm one.
static const uint32_t JOIN_TRY_MS = 6000;
static const int      JOIN_TRIES  = 3;

// Never let the panel go this long without a report. nodeota.cpp on the panel turns 20s of silence
// from a busy node into a failure, and a slow link can spend far longer than that between two 5%
// marks - so progress is reported on whichever of the two comes first.
static const uint32_t REPORT_MS  = 2000;
static const uint8_t  REPORT_PCT = 5;

static struct SrvUrl s_url;
static char     s_tok[NODEPROTO_TOKEN];

static const char HEXDIG[] = "0123456789abcdef";

// ---- small helpers ----------------------------------------------------------------------------

static void fail(const char *why) { nodeagent_report(NODE_PH_FAIL, NODE_PCT_NONE, why); }

// This image's identity as the manifest spells it: the first 8 bytes of app_elf_sha256 in
// lowercase hex. Eight and not thirty-two because that is what NodeRepMsg carries to the panel, so
// the string the node compares against the server is character-for-character the string the panel
// is showing beside this node's name. `out` must hold 17.
static void own_elf_hex(char *out) {
    const esp_app_desc_t *d = esp_app_get_description();
    for (int i = 0; i < 8; i++) {
        uint8_t b = d ? d->app_elf_sha256[i] : 0;
        out[i * 2]     = HEXDIG[b >> 4];
        out[i * 2 + 1] = HEXDIG[b & 0x0F];
    }
    out[16] = '\0';
}

static void lowercase(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'F') *s = (char)(*s - 'A' + 'a');
    }
}

// ---- JSON --------------------------------------------------------------------------------------
//
// Flat lookups, hand-written, no ArduinoJson - the panel's fwpull.cpp explains why and this
// manifest is the same five scalar keys at the top level, none of them a substring of another, and
// the only string values are hex, which can hold neither a quote nor an escape. Pulling a JSON
// library onto this board to read that would cost more flash than the two functions below and
// would still need every one of the length checks around them.

// "key":"value" -> value. False when the key is absent, or when the value did not fit or was cut
// short by a truncated body - a clipped hash is worse than no hash, because it compares unequal
// and would reinstall the image already running.
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

// "key": number -> number, as an integer. An integer and not a float because it has to compare
// exactly against a Content-Length, and a comparison that is only exact because of the magnitude
// it happens to be at is the kind of thing that stops being true without anybody noticing.
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

// ---- HTTP ---------------------------------------------------------------------------------------

// HTTP/1.0, and for the image that is not tidiness but the whole check: 1.0 has no chunked
// transfer encoding, so a body always arrives as plain bytes with a Content-Length in front of it.
// Update.begin() has to be told the exact size before the first byte is written, and a chunked
// body only reveals its length after the last chunk has gone past.
static void write_get_head(WiFiClient &c, const char *path, const char *accept) {
    c.print("GET "); c.print(s_url.prefix); c.print(path); c.print(" HTTP/1.0\r\n");
    c.print("Host: "); c.print(s_url.host); c.print(":"); c.print(s_url.port); c.print("\r\n");
    c.print("User-Agent: SmartFarm-Node/1.0\r\n");
    c.print("Accept: "); c.print(accept); c.print("\r\n");
    if (s_tok[0]) { c.print("Authorization: Bearer "); c.print(s_tok); c.print("\r\n"); }
    c.print("Connection: close\r\n\r\n");
    // No flush(). NetworkClient declares it as "Print::flush tx" and implements it as clear(),
    // which empties the RX buffer - so the call reads as "make sure the request is out" while
    // actually throwing away reply bytes that already arrived. print() writes to the socket
    // synchronously, which is the only completion this ever wanted.
}

// One CRLF-terminated line with the CRLF stripped. -1 when the socket closed or the budget ran out
// first. A line longer than `cap` is clipped and the rest of it is still consumed, so a header this
// file does not read cannot desynchronise the ones it does.
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
            // Case-insensitively, even though uvicorn emits it lowercase: a proxy in front of the
            // server may normalise header names and RFC 9110 says they are not case-sensitive.
            *clen = strtol(line + 15, nullptr, 10);
        }
    }
    return status;
}

// The rest of a small body, until the server closes. -1 when it did not fit - a manifest that
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

// ---- WiFi, for the length of a download and no longer ---------------------------------------
//
// WHY THIS BOARD IS NOT SIMPLY JOINED ALL THE TIME, which would make this function a no-op.
//
// It costs nothing to be right about: joining moves the radio to the AP's channel, and the panel is
// joined to that same AP, so ESP-NOW keeps working straight through the association - which is why
// the progress reports below reach the panel from inside the download, over the same air that
// carried the command that started it. The node does not go quiet while it updates.
//
// What a permanent join costs is everything else. An associated STA receives and decodes every
// beacon, wakes on its DTIM, and spends time on scan and roam work for an uplink this board uses
// for exactly one thing: fetching a firmware image, on a human's button press, perhaps twice a
// year. It also gives up the ability to hop channels, and hopping is how the sensor sweep finds a
// panel that moved (main.cpp: "the only path back if the S3 moves channel"). Between those, the
// telemetry cadence this node exists to hold is load-bearing and the WiFi is not. So the radio
// stays a pure ESP-NOW radio, and this is the one place that borrows it.

// Reports its own failure and tears down before returning false.
static bool wifi_up(void) {
    char ssid[33], pass[65];
    if (!nodeagent_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        // The one update failure a grower can fix from the panel, so it says which one it is.
        nlogf_always("[ota] no wifi credentials - panel has never provisioned this node");
        fail("WiFi 정보 없음");
        return false;
    }

    // Reported before the hold goes on, so this one frame still gets a hop onto the panel's
    // channel and the screen changes the moment the join starts. The keepalives below are sent
    // held, on whatever channel the radio was left on, and may be lost - that is what the panel's
    // 40s budget for this phase is for.
    nodeagent_report(NODE_PH_ASK, NODE_PCT_NONE, "WiFi 연결 중");

    // From here the radio must not hop. esp_wifi_set_channel() on an associated station drops the
    // association, and main.cpp's senders hop before every send.
    nodeagent_radio_hold(true);

    for (int i = 0; i < JOIN_TRIES; i++) {
        WiFi.begin(ssid, pass[0] ? pass : NULL);
        uint32_t t0 = millis(), last = millis();
        while (millis() - t0 < JOIN_TRY_MS) {
            if (WiFi.status() == WL_CONNECTED) {
                // Associating reinstates modem sleep whatever was asked for before it, and a
                // sleeping modem misses ESP-NOW frames - which here would be the panel's own
                // retries of the command being executed.
                WiFi.setSleep(false);
                nlogf_always("[ota] joined '%s' as %s (attempt %d)", ssid,
                             WiFi.localIP().toString().c_str(), i + 1);
                return true;
            }
            if (millis() - last >= REPORT_MS) {
                last = millis();
                nodeagent_report(NODE_PH_ASK, NODE_PCT_NONE, "WiFi 연결 중");
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        // The full radio cycle, not a plain retry - see JOIN_TRIES. It stops the WiFi driver, and
        // ESP-NOW does not survive that, so the peers and callbacks have to be put back or the
        // node finishes this loop unable to talk to the panel at all whether it joined or not.
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(150);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        nodeagent_radio_rearm();
    }

    nlogf_always("[ota] could not join '%s' in %d attempts", ssid, JOIN_TRIES);
    fail("WiFi 연결 실패");
    // Reported first, torn down second: the FAIL frame goes out while the radio is still where the
    // panel can hear it without a hop.
    WiFi.disconnect(false);
    nodeagent_radio_forget_channel();
    nodeagent_radio_hold(false);
    return false;
}

static void wifi_down(void) {
    // Deauth only. WiFi.disconnect(true) stops the driver, and ESP-NOW does not survive that: it
    // would take the peer list with it and this node's only link to the panel with the peer list.
    // Leaving the driver up in STA mode with nothing associated is what it was doing before.
    WiFi.disconnect(false);
    // The association moved the radio to the AP's channel behind main.cpp's cache. Clearing the
    // cache forces the next telemetry send to hop for real, instead of believing it is already
    // there and transmitting on whatever channel the AP happened to be using.
    nodeagent_radio_forget_channel();
    nodeagent_radio_hold(false);
}

// ---- the pull ------------------------------------------------------------------------------------

// GET /v1/firmware/latest?role=node and pull the three fields that decide anything out of it. The
// other two the server publishes - idf_ver and mtime - are for a person reading the manifest.
static bool fetch_manifest(char *sha, size_t shacap, char *md5, size_t md5cap, long *size) {
    char path[64];
    snprintf(path, sizeof(path), "%s%s", NODEPROTO_PATH_LATEST,
             nodeproto_role_name(NODE_ROLE_NODE));

    WiFiClient c;
    uint32_t start = millis();
    if (!c.connect(s_url.host, s_url.port, CONNECT_MS)) {
        nlogf_always("[ota] cannot reach %s:%u", s_url.host, (unsigned)s_url.port);
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
        // operator saying "no node firmware yet", not a fault - but it is still the end of this
        // update, and NodePhase has no word for "asked, nothing there", so it ends as a failure
        // whose text says exactly what happened.
        nlogf_always("[ota] server has no firmware published for role 'node' (404)");
        fail("서버에 노드 펌웨어 없음");
        return false;
    }
    if (status < 200 || status >= 300) {
        nlogf_always("[ota] manifest HTTP %d", status);
        fail("서버 응답 오류");
        return false;
    }
    if (n < 0) {
        nlogf_always("[ota] manifest body did not arrive whole");
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
        nlogf_always("[ota] manifest is not the shape this expects");
        fail("서버 응답 이상");
        return false;
    }
    lowercase(sha);
    lowercase(md5);
    return true;
}

// GET /v1/firmware/image?role=node and write it into the inactive slot. Returns only on failure,
// with the phase already reported; on success it restarts and never comes back.
static void download_and_install(const char *sha, const char *md5, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "%s%s", NODEPROTO_PATH_IMAGE,
             nodeproto_role_name(NODE_ROLE_NODE));

    // What a TLS session costs here, because this is the allocation that could not afford to be a
    // guess. The pinned esp32-arduino-libs sdkconfig has CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384
    // with ASYMMETRIC_CONTENT_LEN off, so mbedtls takes that size in BOTH directions: ~32KB of
    // record buffers plus the context, against the 4KB chunk buffer below that the comment there
    // already treats as worth freeing. It fits, and not narrowly - this image links at 66,524
    // bytes of static RAM out of the esp32's 327,680, leaving something over 200KB of heap once
    // the WiFi driver has taken its share of what is left.
    //
    // What does change is where a board that has run out finds out. The session is allocated
    // before the chunk buffer, so an exhausted heap now fails the connect below and reports
    // "서버 연결 실패", not the "메모리 부족" branch and its free-heap figure.
    WiFiClient c;
    uint32_t start = millis();
    if (!c.connect(s_url.host, s_url.port, CONNECT_MS)) {
        nlogf_always("[ota] cannot reach %s:%u for the image", s_url.host, (unsigned)s_url.port);
        fail("서버 연결 실패");
        return;
    }
    write_get_head(c, path, "application/octet-stream");

    long clen = -1;
    int status = read_head(c, start, REPLY_MS, &clen);
    if (status != 200) {
        c.stop();
        nlogf_always("[ota] image HTTP %d", status);
        fail("서버 응답 오류");
        return;
    }
    if (clen != (long)size) {
        // BEFORE a single byte is written, not after. The manifest and the image are two separate
        // requests against a file an operator can replace between them, and an image that is not
        // the one the hash and the md5 describe must never reach Update.begin() - past that point
        // the inactive slot is being erased for something nobody vouched for.
        c.stop();
        nlogf_always("[ota] image is %ld bytes, manifest said %u - refusing", clen, (unsigned)size);
        fail("크기 불일치");
        return;
    }

    // Plain internal DRAM: this is an esp32dev with no PSRAM, so there is nowhere else for it to
    // come from. Taken here and freed below rather than held for the life of the image - 4KB
    // permanently reserved for something that happens on a button press would be 4KB the thermal
    // path's frame, payload and fragment buffers do not get for the months this board stays up.
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (!buf) {
        c.stop();
        nlogf_always("[ota] no room for a %u byte chunk buffer (%u free)",
                     (unsigned)CHUNK, (unsigned)ESP.getFreeHeap());
        fail("메모리 부족");
        return;
    }

    if (!Update.begin(size, U_FLASH)) {
        nlogf_always("[ota] Update.begin(%u) refused: %s", (unsigned)size, Update.errorString());
        free(buf);
        c.stop();
        fail("설치 시작 실패");
        return;
    }
    // After begin(), never before: begin() clears the expected md5 as part of its reset, so a
    // setMD5() in front of it would be silently thrown away and the whole transfer would go
    // unverified while still looking checked.
    if (!Update.setMD5(md5)) {
        nlogf_always("[ota] Update.setMD5(%s) refused", md5);
        Update.abort();
        free(buf);
        c.stop();
        fail("설치 시작 실패");
        return;
    }

    nlogf_always("[ota] installing %u bytes, md5=%s", (unsigned)size, md5);
    nodeagent_report(NODE_PH_DL, 0, "펌웨어 내려받는 중");

    size_t got = 0;
    uint32_t last = millis();
    uint32_t last_report = millis();
    uint8_t next_pct = REPORT_PCT;

    // On 5% OR on 2 seconds, whichever comes first, and callable from inside the read as well as
    // between chunks. The percentage alone is not enough and neither is a per-chunk check: the
    // panel calls 20 seconds without a NODE_PROG from a downloading node a stall, and a link slow
    // enough to matter can spend longer than that inside ONE 4KB chunk while never being idle
    // long enough to trip STALL_MS - which would have the panel give up on a transfer that is
    // still moving. `got` lags by up to a chunk when this runs mid-read, which is 0.3% of a
    // megabyte and invisible in a percentage.
    auto report_progress = [&]() {
        uint8_t pct = (uint8_t)((got * 100) / size);
        if (pct < next_pct && millis() - last_report < REPORT_MS) return;
        next_pct = (uint8_t)(pct + REPORT_PCT);
        last_report = millis();
        nodeagent_report(NODE_PH_DL, pct, "펌웨어 내려받는 중");
    };

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
                report_progress();
                // Two ticks while starved as well as after a write. This inner loop can spin for a
                // whole RTT waiting on the next TCP segment, and it runs on the loop task - the
                // one whose core's idle task the watchdog watches. A busy-wait here is a reset.
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
        if (n == 0) break;
        if (Update.write(buf, n) != n) break;
        got += n;
        report_progress();

        // Two ticks, not one, and not none. vTaskDelay(1) wakes at the NEXT tick boundary, so it
        // can be almost no time at all, while the Update.write() above holds the CPU for tens of
        // milliseconds erasing a sector; the panel has already taken a task-watchdog reset from
        // exactly that arithmetic. Two ticks is a guaranteed full tick of slack for the idle task.
        // The throughput it costs is a couple of seconds across the whole image, against a
        // transfer that otherwise reboots the board partway through.
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    free(buf);
    // conn.stop() and not c.stop(): on an https URL the socket carries an mbedtls session, and
    // closing it would leave the ~32KB of record buffers held through the md5 pass below and the
    // restart after it, for a connection that has nothing left to say.
    c.stop();

    if (got != size) {
        // errorString() first, abort() second. abort() calls _abort(UPDATE_ERROR_ABORT), which
        // overwrites whatever Update actually failed on - so asking afterwards reports the cleanup
        // instead of the cause. When the socket died rather than the write, this reads "No Error"
        // and the byte counts carry the story.
        nlogf_always("[ota] stopped at %u/%u bytes: %s", (unsigned)got, (unsigned)size,
                     Update.errorString());
        Update.abort();
        fail("전송 끊김");
        return;
    }

    if (!Update.end(false)) {
        // No abort() here. end() has already run _abort() internally on the path that failed - an
        // md5 mismatch, or a short image - so the state is released and a second abort would do
        // nothing but replace the error this line is printing.
        nlogf_always("[ota] install failed: %s", Update.errorString());
        fail("설치 실패");
        return;
    }

    // No retry loop, deliberately. Everything that reaches here has had its size checked against
    // the manifest and its md5 checked by Update, so a failure means the bytes on the server are
    // not the bytes the manifest describes, or the flash refused them. Neither gets better by doing
    // it again, and a board that reinstalls a broken image in a loop burns erase cycles and hides
    // the fault. A person can press the button again once somebody has fixed the file.
    nlogf_always("[ota] installed elf=%.16s; restarting", sha);
    nodeagent_report(NODE_PH_DONE, 100, "설치 완료 - 재시작합니다");

    // A moment for that frame to be keyed before the radio goes away with the rest of the board.
    // No health.cpp on this node to tell the next boot the restart was intended - there is no crash
    // counter here to confuse - so this is a plain restart. What stands behind it is the md5 check
    // above and the bootloader, which verifies an image before it hands control to it and falls
    // back to the slot that still holds the previous one when it does not verify.
    delay(400);
    ESP.restart();
}

void nodeota_run(const char *base_url, const char *token) {
    nodeagent_report(NODE_PH_ASK, NODE_PCT_NONE, "업데이트 준비 중");

    if (esp_ota_get_next_update_partition(NULL) == NULL) {
        // Reported rather than attempted. A single-app-partition table is one line of
        // platformio.ini away and the failure it produces otherwise is Update.begin() refusing,
        // several seconds and one WiFi association later, with a message about sizes.
        nlogf_always("[ota] no second app partition - this image cannot install anything");
        fail("OTA 슬롯 없음");
        return;
    }
    // srvurl_parse() rather than a fourth copy of the same split; shared/srvurl.h holds the rule
    // and the reason an address nobody can parse has to read as unconfigured.
    if (!srvurl_parse(base_url, &s_url)) {
        nlogf_always("[ota] cannot parse server url '%s'", base_url);
        fail("서버 주소 오류");
        return;
    }
    strncpy(s_tok, token ? token : "", sizeof(s_tok) - 1);
    s_tok[sizeof(s_tok) - 1] = '\0';
    nlogf_always("[ota] update requested: %s:%u%s", s_url.host, (unsigned)s_url.port,
                 s_url.prefix);

    if (!wifi_up()) return;   // reported and tore down its own failure

    nodeagent_report(NODE_PH_ASK, NODE_PCT_NONE, "업데이트 확인 중");
    char sha[65], md5[33];
    long size = 0;
    if (!fetch_manifest(sha, sizeof(sha), md5, sizeof(md5), &size)) {
        wifi_down();
        return;
    }

    // Eight bytes, not thirty-two. PlatformIO stamps no app version this project controls - every
    // image it builds reports the same version string, because that string describes the Arduino
    // core - so app_elf_sha256 is the only field in the descriptor that moves with our sources.
    // Sixteen hex characters of it is what NodeRepMsg carries and what the panel displays, and a
    // collision inside eight bytes of SHA-256 is not a thing that happens to a greenhouse.
    char own[17];
    own_elf_hex(own);
    if (strncmp(sha, own, 16) == 0) {
        nlogf_always("[ota] already running elf=%s", own);
        nodeagent_report(NODE_PH_CURRENT, NODE_PCT_NONE, "이미 최신 버전입니다");
        wifi_down();
        return;
    }
    nlogf_always("[ota] server has elf=%.16s, running %s - installing", sha, own);

    download_and_install(sha, md5, (size_t)size);
    // Only reached on failure; download_and_install() restarts on success. Back to a pure ESP-NOW
    // radio either way - a failed update must not leave the node associated, half-configured, or
    // needing a reboot to report again.
    wifi_down();
}
