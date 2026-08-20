#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_app_desc.h>
#include <esp_now.h>
#include <esp_ota_ops.h>
#include <esp_system.h>   // esp_reset_reason(), reported in NodeRepMsg.reset_reason
#include <esp_timer.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nodeagent.h"
#include "nodeota.h"
#include "nodeproto.h"
#include "sensor_protocol.h"

// One HELLO per three seconds. Fast enough that the panel's liveness ages out in a handful of
// missed frames rather than a minute, slow enough to be invisible next to the ~56 thermal
// fragments a second this radio is already carrying.
static const uint32_t HELLO_MS = 3000;

// How long the node runs before it declares its own image good. See confirm_image().
static const uint32_t PENDING_CONFIRM_MS = 30000;

// While unprovisioned, ask this often. Not the CAM's tight channel sweep: that board has nothing
// else to do until it has credentials, whereas this one is already sweeping SensorMsg to find the
// panel and gets the panel's channel back in a ChannelMsg for free. Asking on that pinned channel
// is one packet against thirteen, and it is the only channel the panel could have heard us on
// anyway.
static const uint32_t PROV_ASK_MS = 15000;

static const char HEXDIG[] = "0123456789abcdef";

// ---- identity, computed once ---------------------------------------------------------------

static uint8_t s_elf_sha[8];
static bool s_can_ota = false;
static const esp_partition_t *s_running = nullptr;

// ---- log queue -------------------------------------------------------------------------------
//
// Twelve slots, because "state" is the longest burst anything here produces (eight lines) and a
// queue that cannot hold one answer would drop half of every reply. 12 x 160 bytes is under 2KB
// on a board with no display buffer and no PSRAM pressure.
#define NLOG_SLOTS 12
static const uint8_t NLOG_PER_SEC = 4;

static char s_ring[NLOG_SLOTS][NODEPROTO_TEXT];
static uint8_t s_head = 0, s_tail = 0;
static uint16_t s_dropped = 0;
static uint8_t s_budget = NLOG_PER_SEC;
static uint32_t s_window_ms = 0;
static bool s_verbose = false;

// ---- credentials ------------------------------------------------------------------------------

static char s_ssid[33] = "";
static char s_pass[65] = "";
static bool s_have_creds = false;

// ---- panel commands ----------------------------------------------------------------------------
//
// One slot, claimed by the flag. The producer is the ESP-NOW receive callback on the WiFi task and
// the consumer is tick() on the loop task, and the writer refuses to touch a slot that is still
// pending - so the reader can never see half of one command and half of the next. A command that
// arrives into a full slot is simply the panel's next retry, which is seconds away.
static NodeCmdMsg s_cmd;
static volatile bool s_cmd_pending = false;
static ProvMsg s_prov;
static volatile bool s_prov_pending = false;

// Last seq executed, per kind. Per kind and not one shared counter because the header calls seq
// "per-command" without saying whether the panel numbers the kinds together or apart, and this
// form is correct either way: a shared counter still produces a monotonic sequence within each
// kind.
static uint8_t s_last_seq[3] = {0, 0, 0};
static bool s_have_seq[3] = {false, false, false};

static uint8_t s_seq = 0;
static uint32_t s_hello_ms = 0;
static uint32_t s_prov_ask_ms = 0;
static bool s_confirmed = false;

// ---- reports -------------------------------------------------------------------------------

static uint8_t flags_now(void) {
    uint8_t f = 0;
    // Joined AND holding an address. WL_CONNECTED goes true at association, which is before DHCP
    // has answered, and a node reporting WIFI with 0.0.0.0 beside it is a node the panel would
    // show as ready to download from a server it cannot yet route to.
    if (WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) f |= NODEF_WIFI;
    if (s_can_ota) f |= NODEF_CAN_OTA;
    if (s_verbose) f |= NODEF_VERBOSE;
    esp_ota_img_states_t st;
    if (s_running && esp_ota_get_state_partition(s_running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        f |= NODEF_PENDING;
    }
    return f;
}

static void fill_common(NodeRepMsg *m, uint8_t kind) {
    memset(m, 0, sizeof(*m));
    m->magic = NODE_MAGIC;
    m->ver = NODEPROTO_VER;
    m->kind = kind;
    m->role = NODE_ROLE_NODE;
    m->seq = ++s_seq;
    // IDLE on everything that is not a NODE_PROG, which is what NodeRepKind specifies and it
    // matters: without it a HELLO three seconds after a failed update would re-assert NODE_PH_FAIL
    // forever, and the panel would never be able to show this node as merely idle again.
    // nodeagent_report() overwrites both fields on its own frame.
    m->phase = NODE_PH_IDLE;
    m->pct = NODE_PCT_NONE;
    m->flags = flags_now();
    // Why this boot happened, beside how long it has lasted. Cheap to read - the IDF caches it
    // from the reset registers at startup - and sent on every frame rather than only on HELLO, so
    // a panel that missed the HELLO still learns it. See NodeRepMsg.reset_reason.
    m->reset_reason = (uint8_t)esp_reset_reason();
    IPAddress ip = WiFi.localIP();
    m->ip[0] = ip[0];
    m->ip[1] = ip[1];
    m->ip[2] = ip[2];
    m->ip[3] = ip[3];
    // esp_timer_get_time() and not millis(), which is the same counter truncated to 32 bits and
    // therefore wraps to zero at 49.7 days. The panel reads uptime_s going backwards during an
    // ASK or a DL as "the board restarted mid-install" and fails the row on the spot
    // (src/nodeota.cpp's crash detector), so on a node that has been up since spring a wrap that
    // lands inside a download turns a healthy update into a reported failure. A greenhouse node
    // is exactly the device that runs long enough to reach 49.7 days, and this costs one 64-bit
    // divide against a field that is sent every three seconds.
    m->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    m->free_heap = ESP.getFreeHeap();
    memcpy(m->elf_sha, s_elf_sha, sizeof(m->elf_sha));
}

// Put one line on the air right now, outside the queue and outside the rate limit. Only for
// answers that have to beat something irreversible - the reboot verb is the whole reason it
// exists, because the queue drains one line per loop iteration and there will be no more.
static void log_send_now(const char *line) {
    NodeRepMsg m;
    fill_common(&m, NODE_LOG);
    strncpy(m.text, line, sizeof(m.text) - 1);
    nodeagent_radio_send(&m, sizeof(m));
}

void nodeagent_report(uint8_t phase, uint8_t pct, const char *text) {
    NodeRepMsg m;
    fill_common(&m, NODE_PROG);
    m.phase = phase;
    m.pct = pct;
    strncpy(m.text, text, sizeof(m.text) - 1);
    nodeagent_radio_send(&m, sizeof(m));
    // Serial as well, always. A phase change is the one thing worth having in both places: over
    // the air for the grower in front of the panel, on the console for whoever has a cable in
    // when it goes wrong.
    if (pct == NODE_PCT_NONE) Serial.printf("[ota] phase %u: %s\n", (unsigned)phase, text);
    else                      Serial.printf("[ota] phase %u %u%%: %s\n", (unsigned)phase,
                                            (unsigned)pct, text);
}

// ---- logging ---------------------------------------------------------------------------------

static void nlog_put(bool always, const char *fmt, va_list ap) {
    // NODEPROTO_TEXT and not something roomier on purpose: this is the wire's own cap, so a line
    // that would be truncated on the way out is truncated here too and the console shows exactly
    // what the panel got. A diagnostic that reads differently in the two places is worse than a
    // short one.
    char line[NODEPROTO_TEXT];
    vsnprintf(line, sizeof(line), fmt, ap);
    Serial.println(line);

    if (!always && !s_verbose) return;
    uint8_t next = (uint8_t)((s_head + 1) % NLOG_SLOTS);
    if (next == s_tail) {
        // Full. Drop the NEW line rather than the oldest: a failure path that has started
        // repeating itself says everything it is going to say in its first few lines, and the
        // count below tells the reader that the rest existed.
        s_dropped++;
        return;
    }
    strcpy(s_ring[s_head], line);
    s_head = next;
}

void nlogf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nlog_put(false, fmt, ap);
    va_end(ap);
}

void nlogf_always(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nlog_put(true, fmt, ap);
    va_end(ap);
}

// One line per call, four per second. THE CAP IS THE POINT: this is a 250-byte-frame radio that
// already carries a reading every 2s and a thermal frame at ~4fps, and main.cpp's comments record
// what a second bulk sender did to it - the panel's video went from 13.6fps to 2.5 when thermal
// alone went to full rate. A log path that streamed a chatty failure loop at the speed the loop
// produces it would starve the telemetry that is the reason this board exists, and it would do it
// exactly when somebody is watching a failure and needs the sensor numbers most.
static void log_pump(void) {
    uint32_t now = millis();
    if (now - s_window_ms >= 1000) {
        s_window_ms = now;
        s_budget = NLOG_PER_SEC;
    }
    if (s_budget == 0) return;

    if (s_tail == s_head) {
        // The queue has just emptied, which is the first moment the drop count is both final and
        // readable. Reported here rather than inline with the flood, where it would have been one
        // more line competing for the budget it is describing.
        if (s_dropped) {
            char line[NODEPROTO_TEXT];
            snprintf(line, sizeof(line), "[nlog] %u lines dropped (%u/s cap)",
                     (unsigned)s_dropped, (unsigned)NLOG_PER_SEC);
            s_dropped = 0;
            s_budget--;
            log_send_now(line);
        }
        return;
    }
    log_send_now(s_ring[s_tail]);
    s_tail = (uint8_t)((s_tail + 1) % NLOG_SLOTS);
    s_budget--;
}

// ---- credentials ------------------------------------------------------------------------------

// XOR is symmetric, so the same routine obfuscates and de-obfuscates. Same key and same call shape
// as esp32cam-streamer/src/provision.cpp, because it is the same wire format.
static void xor_field(char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[i] ^= PROV_KEY[i % sizeof(PROV_KEY)];
    }
}

static void take_prov(void) {
    ProvMsg m = s_prov;
    s_prov_pending = false;

    xor_field(m.ssid, sizeof(m.ssid));
    xor_field(m.pass, sizeof(m.pass));
    m.ssid[sizeof(m.ssid) - 1] = '\0';
    m.pass[sizeof(m.pass) - 1] = '\0';
    if (!m.ssid[0]) return;   // a reply the panel sent before it had anything to hand out

    bool changed = strcmp(m.ssid, s_ssid) != 0 || strcmp(m.pass, s_pass) != 0;
    strncpy(s_ssid, m.ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, m.pass, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
    s_have_creds = true;
    if (!changed) return;

    // NVS, and only when they actually changed. Persisted because the gap between provisioning
    // and the first OTA is measured in months here - this node joins WiFi for nothing else - and a
    // grower who has to get the panel to re-push before every update has a two-step update.
    // Rewriting an identical pair on every unsolicited push would spend a flash erase per push for
    // no change at all.
    Preferences p;
    p.begin("node", false);
    p.putString("ssid", s_ssid);
    p.putString("pass", s_pass);
    p.end();
    nlogf_always("[prov] credentials for '%s' stored", s_ssid);
}

bool nodeagent_wifi_creds(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap) {
    if (!s_have_creds) return false;
    strncpy(ssid, s_ssid, ssid_cap - 1);
    ssid[ssid_cap - 1] = '\0';
    strncpy(pass, s_pass, pass_cap - 1);
    pass[pass_cap - 1] = '\0';
    return true;
}

// ---- commands ----------------------------------------------------------------------------------

static void dump_state(void) {
    char sha[17];
    for (int i = 0; i < 8; i++) {
        sha[i * 2]     = HEXDIG[s_elf_sha[i] >> 4];
        sha[i * 2 + 1] = HEXDIG[s_elf_sha[i] & 0x0F];
    }
    sha[16] = '\0';
    nlogf_always("[state] img %s up %lus heap %u flags 0x%02X", sha,
                 (unsigned long)(esp_timer_get_time() / 1000000LL), (unsigned)ESP.getFreeHeap(),
                 (unsigned)flags_now());
    nlogf_always("[state] ota %s, running '%s' @0x%06X",
                 s_can_ota ? "slot available" : "NO SECOND SLOT - cannot install",
                 s_running ? s_running->label : "?",
                 s_running ? (unsigned)s_running->address : 0u);
    // SSID only. The password is a credential and a log line is the one place on this board that
    // is designed to be read by somebody who is not standing next to it.
    if (s_have_creds) nlogf_always("[state] wifi provisioned for '%s'", s_ssid);
    else              nlogf_always("[state] wifi NOT provisioned - an update would fail");
    nodeagent_state_lines();
}

static void run_verb(const char *v) {
    if (strcmp(v, "log on") == 0) {
        s_verbose = true;
        nlogf_always("[node] log streaming on (%u lines/s cap)", (unsigned)NLOG_PER_SEC);
        return;
    }
    if (strcmp(v, "log off") == 0) {
        s_verbose = false;
        s_tail = s_head;   // queued for a listener who just left
        s_dropped = 0;
        nlogf_always("[node] log streaming off");
        return;
    }
    if (strcmp(v, "state") == 0) {
        dump_state();
        return;
    }
    if (strcmp(v, "reboot") == 0) {
        // Answered before it is obeyed, and outside the queue: a panel that watches a node go
        // quiet with nothing on the screen cannot tell its own command from a crash, and this is
        // the verb somebody reaches for when they already suspect a crash.
        Serial.println("[node] rebooting on panel request");
        log_send_now("[node] rebooting on panel request");
        delay(300);   // long enough for the radio to key the frame above
        ESP.restart();
    }
    // Not an error and not a silence. A newer panel will grow verbs this image has never heard of,
    // and the contract for that is "nothing happened" plus a line saying so.
    nlogf_always("[node] unknown debug verb '%s' - ignored", v);
}

static void run_cmd(void) {
    NodeCmdMsg c = s_cmd;
    s_cmd_pending = false;

    if (c.ver != NODEPROTO_VER) {
        // Stops here rather than reading role or kind out of it. Past the version byte the field
        // offsets are whatever the newer panel decided they are, so acting on a verb read from the
        // wrong place is precisely the undefined behaviour the header says to degrade away from.
        nlogf_always("[node] command speaks protocol v%u, this image speaks v%u - ignored",
                     (unsigned)c.ver, (unsigned)NODEPROTO_VER);
        return;
    }
    // Addressed to the CAM. Silent: this is a normal thing to overhear, not a fault, and a line
    // per overheard frame would be the flood the rate limiter exists to prevent.
    if (c.role != NODE_ROLE_NODE) return;

    if (c.kind != NODE_UPDATE && c.kind != NODE_DEBUG) {
        nlogf_always("[node] unknown command kind %u - ignored", (unsigned)c.kind);
        return;
    }
    // The panel retries a command it has not seen answered, deliberately. A seq already executed
    // is that retry arriving late, not a second instruction - and for NODE_UPDATE the difference
    // between those two readings is a node that reinstalls in a loop.
    if (s_have_seq[c.kind] && s_last_seq[c.kind] == c.seq) return;
    s_have_seq[c.kind] = true;
    s_last_seq[c.kind] = c.seq;

    // Before either field is read as a string. They arrive from a peer, and a text[] with no NUL
    // in it would walk straight into token[] and off the end of the struct.
    c.text[sizeof(c.text) - 1] = '\0';
    c.token[sizeof(c.token) - 1] = '\0';

    if (c.kind == NODE_UPDATE) {
        // The panel re-pushes credentials immediately before it sends this command, but they are
        // two frames on an unordered radio and a factory-fresh node whose very first update press
        // races that push would fail instantly with "WiFi 정보 없음" and need a second press to
        // work. One solicit and a second and a half of waiting costs nothing against the panel's
        // 40s window for this phase, and it is the difference between a first press that works
        // and a first press somebody has to learn to repeat.
        if (!s_have_creds) {
            ProvMsg req = {};
            req.magic = PROV_MAGIC;
            req.type = PROV_REQUEST;
            nodeagent_radio_send(&req, sizeof(req));
            uint32_t t0 = millis();
            while (!s_have_creds && millis() - t0 < 1500) {
                if (s_prov_pending) take_prov();
                delay(20);
            }
        }
        nodeota_run(c.text, c.token);   // blocks; does not return on a successful install
        return;
    }
    run_verb(c.text);
}

// ---- image confirmation --------------------------------------------------------------------

// Declare the running image good once it has been running for a while.
//
// With the Arduino default sdkconfig this finds ESP_OTA_IMG_VALID and does nothing, because
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is off and a freshly written slot is simply marked
// undefined. It is here for the build where that is not true: a PENDING_VERIFY image that nobody
// confirms is reverted at the next reboot, so the update would install, run, and then silently
// undo itself - the worst shape a firmware bug can take, because from the outside it looks like it
// worked. It also gives NODEF_PENDING a way to clear, and a flag that can only ever be set is a
// flag the panel would show forever.
//
// Thirty seconds of running with ESP-NOW keyed and the sensor loop turning is the whole self-test
// this board has. It is not much, but it separates "boots and works" from "boots and panics",
// which is what the check is for.
static void confirm_image(void) {
    if (s_confirmed || millis() < PENDING_CONFIRM_MS) return;
    s_confirmed = true;
    esp_ota_img_states_t st;
    if (!s_running || esp_ota_get_state_partition(s_running, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        nlogf_always("[node] image confirmed after %lus", (unsigned long)(PENDING_CONFIRM_MS / 1000));
    }
}

// ---- public ------------------------------------------------------------------------------------

bool nodeagent_on_espnow(const uint8_t *src, const uint8_t *data, int len) {
    (void)src;   // the panel finds us; we already know where it is, from ChannelMsg
    if (len == (int)sizeof(NodeCmdMsg)) {
        if (s_cmd_pending) return true;   // claimed; the panel's next retry lands in the free slot
        NodeCmdMsg c;
        memcpy(&c, data, sizeof(c));
        if (c.magic != NODE_MAGIC) return false;
        s_cmd = c;
        s_cmd_pending = true;
        return true;
    }
    if (len == (int)sizeof(ProvMsg)) {
        if (s_prov_pending) return true;
        ProvMsg p;
        memcpy(&p, data, sizeof(p));
        if (p.magic != PROV_MAGIC || p.type != PROV_REPLY) return false;
        s_prov = p;
        s_prov_pending = true;
        return true;
    }
    return false;
}

void nodeagent_init(void) {
    const esp_app_desc_t *d = esp_app_get_description();
    if (d) memcpy(s_elf_sha, d->app_elf_sha256, sizeof(s_elf_sha));
    s_running = esp_ota_get_running_partition();
    // A fact about the running image, not a build flag somebody remembered to set: esp32dev's
    // stock default.csv has two app slots, but a single-slot table is one line of platformio.ini
    // away and the panel must not offer a button whose only possible outcome is a failure message
    // on a device the grower cannot see.
    s_can_ota = esp_ota_get_next_update_partition(NULL) != NULL;

    Preferences p;
    p.begin("node", true);
    String ssid = p.getString("ssid", "");
    String pass = p.getString("pass", "");
    p.end();
    if (ssid.length()) {
        strncpy(s_ssid, ssid.c_str(), sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = '\0';
        strncpy(s_pass, pass.c_str(), sizeof(s_pass) - 1);
        s_pass[sizeof(s_pass) - 1] = '\0';
        s_have_creds = true;
    }

    char sha[17];
    for (int i = 0; i < 8; i++) {
        sha[i * 2]     = HEXDIG[s_elf_sha[i] >> 4];
        sha[i * 2 + 1] = HEXDIG[s_elf_sha[i] & 0x0F];
    }
    sha[16] = '\0';
    Serial.printf("[node] agent up: img %s, ota %s, wifi %s\n", sha,
                  s_can_ota ? "ready" : "UNAVAILABLE",
                  s_have_creds ? s_ssid : "unprovisioned");

    // One HELLO immediately. The panel cannot offer an update button for a node it has never heard
    // from, and waiting three seconds for the first frame means a panel rebooted beside a running
    // node shows it as absent for exactly as long as somebody is looking at the screen.
    s_window_ms = millis();
    NodeRepMsg m;
    fill_common(&m, NODE_HELLO);
    nodeagent_radio_send(&m, sizeof(m));
    s_hello_ms = millis();
}

void nodeagent_tick(void) {
    if (s_prov_pending) take_prov();
    if (s_cmd_pending) run_cmd();   // may not return: reboot, or an install that restarts
    confirm_image();

    // At most one HELLO and one log line per loop iteration. Both go out through the same radio
    // the sensor and thermal senders hop, and bursting them would put several hops between one
    // telemetry send and the next.
    uint32_t now = millis();
    if (now - s_hello_ms >= HELLO_MS) {
        s_hello_ms = now;
        NodeRepMsg m;
        fill_common(&m, NODE_HELLO);
        nodeagent_radio_send(&m, sizeof(m));
    } else if (!s_have_creds && now - s_prov_ask_ms >= PROV_ASK_MS) {
        s_prov_ask_ms = now;
        ProvMsg req = {};
        req.magic = PROV_MAGIC;
        req.type = PROV_REQUEST;
        nodeagent_radio_send(&req, sizeof(req));
    }
    log_pump();
}
