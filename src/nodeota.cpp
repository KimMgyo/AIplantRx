// nodeota.h carries the design: why the panel drives updates it does not perform, and why the
// control plane is ESP-NOW while the bytes are WiFi. This file carries the two things a header
// cannot - the numbers, and the concurrency.
//
// THE NUMBERS, AND WHERE THEY CAME FROM. Every constant below is a silence this module has to
// tell apart from a failure, and each one was sized against a measured worst case on the two
// boards rather than picked round:
//
//   online          10s   HELLO is ~3s from both nodes, so three consecutive misses.
//   stall, DL       20s   the node is associated and reports progress every 1.5-2s (CAM: every
//                         5% or 1500ms; sensor node: every 5% or 2000ms), so this is ~10 lost
//                         reports in a row - RF, not a stalled download.
//   stall, ASK      40s   ASK covers the sensor node's WiFi join, and it is ESP-NOW-only in
//                         normal operation: this AP's first-association quirk can make it cycle
//                         the radio up to 3 times at ~6s, and its ASK keepalives go out while
//                         the radio is off-channel scanning, so they are expected to be LOST
//                         rather than merely late. Add the manifest's own 4s connect + 8s reply
//                         budget and the honest worst case is ~30s of legitimate quiet.
//   stall, DONE     30s   after DONE the node writes nothing more, restarts, and cannot be
//                         heard again until it is back on the AP's channel - ~17s worst case on
//                         the CAM for the same auth quirk.
//
// The direction of the error matters more than its size. Too long and a dead node sits behind a
// frozen progress bar for another ten seconds; too short and EVERY sensor-node update reports a
// failure that did not happen, on a board nobody can see, which teaches a grower that the button
// is broken. So each window is the measured worst case with margin, and the bar keeps moving
// because the nodes keep reporting - not because this file waits long enough for anything.
//
// THE CONCURRENCY. Three tasks touch the table below: the WiFi task (nodeota_on_recv, out of
// camprov.cpp's recv callback), the loop task (nodeota_tick, nodeota_debug_tick) and the LVGL
// task (nodeota_view, and the buttons that call nodeota_request/nodeota_debug). There is no lock,
// which nodeota.h states as a contract and not an oversight: nothing here is cross-checked
// against anything else, so the worst a torn read produces is one frame of stale text or one
// extra 232-byte send that the node de-duplicates on seq. A mutex around a status line would be
// a lock held on the LVGL task by a callback that runs on the WiFi task, which is a much worse
// trade than a wrong word for 16ms.
#include <Arduino.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <string.h>
#include "nodeota.h"
#include "camprov.h"
#include "nodelog.h"
#include "health.h"   // health_reset_name_of() for the reason a node sends over the wire
#include "sitecfg.h"
#include "hlog.h"

static const uint32_t NODE_ONLINE_MS     = 10000;
static const uint32_t NODE_STALL_MS      = 20000;
static const uint32_t NODE_STALL_ASK_MS  = 40000;
static const uint32_t NODE_STALL_DONE_MS = 30000;

// A single 232-byte action frame is not carried dependably on this air - see the measurements in
// camprov.h, where a 25-byte broadcast never arrived once - and a firmware button that does
// nothing is worse than one that sends twice, because the node ignores a seq it has already run.
// Three sends 700ms apart, stopped the moment any non-idle phase comes back, which on the CAM is
// retry #1 at the latest: it publishes NODE_PH_ASK before it does any network I/O.
static const uint32_t NODE_RETRY_MS = 700;
static const uint8_t  NODE_TRIES    = 3;

// Every phrase a grower can end up reading that this panel wrote itself. Named rather than
// written inline (which is what fwpull.cpp does) for one reason: several of them are used from
// more than one place, and a named literal is one address in .rodata no matter how many call
// sites assign it, so a reader comparing pointers cannot be tripped up by whether the linker
// happened to merge two identical strings.
//
// The node's own phrases are NOT here. A NODE_PROG carries the words with it, because the board
// that knows what happened is the board that should say it. What this panel says is only what
// only this panel knows: that it refused, or that a silence has outlived its explanation.
static const char K_UNKNOWN[]    = "장치 응답 없음";
static const char K_OFFLINE[]    = "장치가 응답하지 않습니다";
static const char K_VER[]        = "프로토콜 버전 불일치";
static const char K_NO_SLOT[]    = "업데이트 슬롯 없음";
static const char K_BUSY[]       = "이미 업데이트 중";
static const char K_OTHER_BUSY[] = "다른 장치 업데이트 중";
static const char K_NO_SERVER[]  = "서버 주소 없음";
static const char K_ARMED[]      = "업데이트 요청함";
static const char K_STALLED[]    = "응답 끊김";
static const char K_LOST_REBOOT[] = "재부팅 후 응답 없음";
static const char K_REBOOTED[]   = "업데이트 완료, 재시작됨";
static const char K_RESTARTED[]  = "업데이트 중 재시작됨";

// ---- state ------------------------------------------------------------------

struct NodeState {
    uint8_t  mac[6];
    bool     have_mac;
    uint32_t last_ms;        // newest report of ANY kind; 0 = never heard from
    uint32_t prog_ms;        // newest NODE_PROG, or the press that armed one

    // Health, absorbed from every kind - a log line still says the node is up.
    uint8_t  flags;
    uint8_t  ip[4];
    uint32_t uptime_s;
    uint32_t free_heap;
    uint8_t  reset_reason;   // as sent; 0 = unknown, which is also what a pre-field board sends
    uint8_t  elf_sha[8];
    char     ver[17];        // elf_sha as lowercase hex, written only when elf_sha changes

    uint8_t  phase;          // NodePhase
    uint8_t  pct;            // as sent: 0..100 or NODE_PCT_NONE
    bool     ver_bad;        // last report was a nodeproto version this panel cannot read
    bool     said_ver;       // ...and the console has been told once
    bool     uptime_seen;

    // The newest phrase for this role. A pointer and never a buffer, which is fwpull.cpp's
    // discipline and for its reason: a reader on another task can only ever see the old pointer
    // or the new one. It points either at one of the K_* literals above or at this role's own
    // text[], and both outlive every reader.
    const char *volatile status;
    char     text[NODEPROTO_TEXT];   // backing store for a node-supplied `status`
    char     log[NODEPROTO_TEXT];    // newest NODE_LOG line

    // seq de-duplication, advanced by NODE_HELLO and NODE_PROG only. NODE_LOG is deliberately
    // not deduplicated and deliberately does not advance this: a duplicated line renders twice,
    // which is cosmetic, while a line dropped because it shared a seq with a heartbeat is the
    // line that explained the crash.
    uint8_t  seq_last;
    bool     seq_seen;
    uint32_t last_uptime_s;

    // The one command in flight for this role, kept so nodeota_tick can resend it.
    uint8_t  cmd_kind;       // NodeCmdKind, 0 when nothing is armed
    uint8_t  cmd_seq;
    uint8_t  cmd_left;       // resends still owed
    uint32_t cmd_last_ms;
    char     cmd_text[NODEPROTO_URL];
    char     cmd_token[NODEPROTO_TOKEN];
};

// Indexed by NodeRole, slot NODE_ROLE_PANEL included so that indexing by a validated role is
// direct and cannot run off the end. Nothing ever targets that slot: the panel updates itself
// through fwpull.cpp, which can do things this module cannot (enter update mode, stand the UI
// down) precisely because it is the board being replaced.
static NodeState s_st[NODE_ROLE_COUNT];

static volatile uint32_t s_rx_hello = 0, s_rx_prog = 0, s_rx_log = 0, s_rx_dup = 0;
static volatile uint32_t s_rx_badver = 0, s_rx_badrole = 0, s_rx_badkind = 0, s_rx_panel = 0;
static volatile uint32_t s_tx_cmd = 0, s_tx_retry = 0, s_tx_fail = 0;

// ---- small helpers ----------------------------------------------------------

// text[] came off the radio and carries NO promise of a terminator: a node with a bug, or a
// sender that disagreed about the length, hands this 160 bytes of whatever. strncpy would then
// copy all 160 and leave `dst` unterminated too, and the first reader walks off the end of a
// static into whatever the linker put next to it. So the length is bounded on BOTH sides and the
// terminator is this function's own.
//
// The last byte of `dst` only ever receives the terminator - data bytes stop at cap-2 - so it is
// zero for the life of the image. That is what makes a concurrent reader safe: it may see a new
// prefix against an old tail, garbled for one frame, which nodeota.h allows - but it can never
// see an unterminated buffer and run past the end of it.
static void copy_text(char *dst, size_t cap, const char *src, size_t srcmax) {
    size_t n = 0;
    while (n < srcmax && n + 1 < cap && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

// 8 raw bytes -> 16 lowercase hex characters plus a NUL. `out` must hold 17. Lowercase because
// that is what the server publishes and what fwpull.cpp compares against; a case difference
// would read as a different image.
static void hex8(const uint8_t *in, char *out) {
    static const char HEXDIG[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[2 * i]     = HEXDIG[(in[i] >> 4) & 0xF];
        out[2 * i + 1] = HEXDIG[in[i] & 0xF];
    }
    out[16] = '\0';
}

// Whether this role has been heard from recently enough to be called online.
//
// Derived at every point of use rather than cached in a field aged by nodeota_tick(). It was a
// field, and the two answers disagreed for exactly as long as it took the next tick to run: a node
// that had just sent its first report printed "STALE age=165ms" on the console and drew
// "오프라인 · 0초 전" on the update page - a row contradicting itself in the same breath, on the one
// screen whose job is to say whether the board is reachable. One predicate, one source.
static bool node_online(const NodeState &st, uint32_t now) {
    return st.last_ms != 0 && (now - st.last_ms) < NODE_ONLINE_MS;
}

// Serial only, so English - the Korean is for the screen.
static const char *phase_name(uint8_t ph) {
    switch (ph) {
        case NODE_PH_IDLE:    return "idle";
        case NODE_PH_ASK:     return "ask";
        case NODE_PH_CURRENT: return "current";
        case NODE_PH_DL:      return "dl";
        case NODE_PH_DONE:    return "done";
        case NODE_PH_FAIL:    return "fail";
        default:              return "?";
    }
}

// Unicast the armed command to the MAC the role last reported from. Peer bookkeeping mirrors
// camprov.cpp's send_reply: channel 0 means "whatever channel this panel is on", which is the
// only value that works when net.cpp can move the radio between two sends.
static void send_cmd(NodeState &st, uint8_t role) {
    if (!st.have_mac || st.cmd_kind == 0) return;

    NodeCmdMsg c = {};
    c.magic = NODE_MAGIC;
    c.ver   = NODEPROTO_VER;
    c.kind  = st.cmd_kind;
    c.role  = role;
    c.seq   = st.cmd_seq;
    // strncpy and not copy_text: both sources are this panel's own terminated statics, and the
    // zero-initialised struct means the padding is already clean.
    strncpy(c.text, st.cmd_text, sizeof(c.text) - 1);
    strncpy(c.token, st.cmd_token, sizeof(c.token) - 1);

    if (!esp_now_is_peer_exist(st.mac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, st.mac, 6);
        peer.channel = 0;
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            s_tx_fail++;
            return;
        }
    }
    if (esp_now_send(st.mac, (const uint8_t *)&c, sizeof(c)) != ESP_OK) {
        // Almost always ESP_ERR_ESPNOW_NOT_INIT: net.cpp takes the radio to WIFI_OFF to recover
        // a failed association and that deinitialises ESP-NOW underneath us. camprov_init() is
        // called again when the radio returns, and the retry below is what covers the gap.
        s_tx_fail++;
        return;
    }
    s_tx_cmd++;
}

// Replace whatever was in flight for this role and send once immediately. The press has to act
// now; nodeota_tick owns the resends.
//
// One slot per role, so a debug poke does displace an update's remaining resends. That is the
// right way round: the poke is the newer intent, and by the time a person reaches for one the
// update has almost always been acknowledged already (which cleared the resends anyway).
static void arm_cmd(NodeState &st, uint8_t role, uint8_t kind, const char *text,
                    const char *token, uint8_t tries) {
    st.cmd_kind = kind;
    st.cmd_seq++;            // a new command, so a new seq: the node ignores one it just ran
    st.cmd_text[0] = '\0';
    st.cmd_token[0] = '\0';
    if (text)  copy_text(st.cmd_text, sizeof(st.cmd_text), text, sizeof(st.cmd_text));
    if (token) copy_text(st.cmd_token, sizeof(st.cmd_token), token, sizeof(st.cmd_token));
    st.cmd_left = tries > 0 ? (uint8_t)(tries - 1) : 0;
    st.cmd_last_ms = millis();
    send_cmd(st, role);
}

// ---- public API -------------------------------------------------------------

void nodeota_init(void) {
    // No memset, and that is what makes the ordering against camprov_init() a matter of tidiness
    // rather than a race: s_st is a static and therefore already zero, so a report that arrived
    // in the microseconds after the recv callback was registered is not wiped by this call.
    //
    // What does need seeding is the command sequence. A node ignores a seq it has just run -
    // that is what makes the resends safe - so a panel that always started counting at 1 would
    // hand a node a seq it still remembers, and the first press after a panel restart would do
    // nothing at all, silently, exactly once. That is not hypothetical: arming one update and
    // then updating the panel itself is a completely ordinary afternoon. A random start moves the
    // collision from "whenever the last command was the first one" to 1 in 256.
    for (uint8_t r = 0; r < NODE_ROLE_COUNT; r++) {
        s_st[r].cmd_seq = (uint8_t)esp_random();
    }
    hlogf("[nodeota] watching %s and %s\n",
          nodeproto_role_name(NODE_ROLE_CAM), nodeproto_role_name(NODE_ROLE_NODE));
}

void nodeota_on_recv(const NodeRepMsg *m, const uint8_t *mac) {
    if (m == nullptr || mac == nullptr) return;
    uint32_t now = millis();

    if (m->role >= NODE_ROLE_COUNT) {
        // Cannot be indexed, cannot be shown, and cannot be asked about - a role this build has
        // no name for is a newer node than this panel. Counted so it is not a mystery.
        s_rx_badrole++;
        return;
    }
    NodeState &st = s_st[m->role];

    if (m->ver != NODEPROTO_VER) {
        // The whole point of the version byte. Everything past it may have moved, so nothing
        // past it is believed - but the sender, the role and the fact that SOMETHING is out
        // there are all still true, and that is what turns an empty row into a row that
        // explains itself. Two boards flash independently and a greenhouse can easily run a
        // month with one of them behind; "프로토콜 버전 불일치" is a person's next five minutes,
        // a blank row is their afternoon.
        memcpy(st.mac, mac, 6);
        st.have_mac = true;
        st.last_ms  = now;
        st.ver_bad  = true;
        st.status   = K_VER;
        s_rx_badver++;
        if (!st.said_ver) {
            // Once. It arrives every 3s and the console is the one place that must stay readable.
            st.said_ver = true;
            hlogf("[nodeota] %s speaks nodeproto v%u, this panel speaks v%u - reports ignored\n",
                  nodeproto_role_name(m->role), (unsigned)m->ver, (unsigned)NODEPROTO_VER);
        }
        return;
    }
    if (st.ver_bad) {
        // Reflashed. Re-arm the console notice so a later regression is heard again.
        st.ver_bad  = false;
        st.said_ver = false;
    }

    memcpy(st.mac, mac, 6);
    st.have_mac = true;
    st.last_ms  = now;       // before the de-duplication below: a repeat is still proof of life

    if (m->role == NODE_ROLE_PANEL) {
        // A second panel in range, or a node with its role wrong. Recorded above so its MAC and
        // liveness are visible to anything that asks, but never acted on - see s_st.
        s_rx_panel++;
        return;
    }

    if (m->kind == NODE_HELLO || m->kind == NODE_PROG) {
        if (st.seq_seen && m->seq == st.seq_last) {
            s_rx_dup++;
            return;
        }
        st.seq_last = m->seq;
        st.seq_seen = true;
    }

    // Health rides on every kind, so it is absorbed before the kind is examined.
    st.flags     = m->flags;
    st.ip[0]     = m->ip[0];
    st.ip[1]     = m->ip[1];
    st.ip[2]     = m->ip[2];
    st.ip[3]     = m->ip[3];
    st.uptime_s  = m->uptime_s;
    st.free_heap = m->free_heap;
    st.reset_reason = m->reset_reason;
    // Rewritten only when the image actually changed. It is 17 bytes read from another task, and
    // re-encoding the same hash every 3s would be 17 bytes of tearing window every 3s in
    // exchange for nothing.
    if (memcmp(st.elf_sha, m->elf_sha, sizeof(st.elf_sha)) != 0) {
        memcpy(st.elf_sha, m->elf_sha, sizeof(st.elf_sha));
        hex8(st.elf_sha, st.ver);
    }

    // A restart nobody asked for, caught by uptime going backwards. Checked against the phase
    // this role was in BEFORE this report, and only for the two phases where a restart is not
    // the expected ending: an update finishes with one (NODE_PH_DONE, handled under HELLO), but
    // a restart during ASK or DL is the download dying. The stall timer in nodeota_tick would
    // reach the same verdict 20 to 40 seconds later, and every one of those seconds is a
    // progress bar that has stopped moving with nothing on the screen to say why.
    if (st.uptime_seen && m->uptime_s < st.last_uptime_s &&
        (st.phase == NODE_PH_ASK || st.phase == NODE_PH_DL)) {
        st.phase   = NODE_PH_FAIL;
        st.pct     = NODE_PCT_NONE;
        st.status  = K_RESTARTED;
        st.cmd_left = 0;
        hlogf("[nodeota] %s restarted mid-update (uptime %lus -> %lus): install lost\n",
              nodeproto_role_name(m->role), (unsigned long)st.last_uptime_s,
              (unsigned long)m->uptime_s);
    }
    st.last_uptime_s = m->uptime_s;
    st.uptime_seen   = true;

    switch (m->kind) {
    case NODE_HELLO:
        s_rx_hello++;
        // A HELLO's phase field is not information: nodeproto.h says NODE_PH_IDLE is the only
        // value it ever carries, so believing it would drop the takeover overlay on the first
        // heartbeat that interleaved with a download. The one thing a heartbeat does say about
        // an update is that the node is running again, and that is what closes out DONE.
        if (st.phase == NODE_PH_DONE) {
            st.phase  = NODE_PH_IDLE;
            st.pct    = NODE_PCT_NONE;
            // This panel's own sentence, and legitimately so: the node cannot say it. It wrote
            // its image, said DONE and restarted, and only the thing that watched both halves
            // knows that the board which came back is the one that went away.
            st.status = K_REBOOTED;
            hlogf("[nodeota] %s is back after an update, running %s\n",
                  nodeproto_role_name(m->role), st.ver);
        }
        break;

    case NODE_LOG:
        s_rx_log++;
        // Both places, because they are read by different people. nodelog.cpp's ring is what
        // somebody reconstructs a 4am failure from once it has reached the server; this copy is
        // the line under the progress bar for whoever is standing at the panel right now.
        //
        // The local copy is made first and handed on, so nodelog_add() never sees the radio
        // buffer - it takes a `const char *` and would have no way to know it was not terminated.
        copy_text(st.log, sizeof(st.log), m->text, NODEPROTO_TEXT);
        nodelog_add(m->role, st.log);
        break;

    case NODE_PROG: {
        s_rx_prog++;
        // THE UPDATE'S OWN NARRATIVE, INTO THE LOG RING AS WELL AS ONTO THE CARD.
        //
        // A node emits nothing on NODE_LOG unless somebody turned its stream on, and the control
        // that did that was removed when the firmware page became three identical cards. So the
        // one sequence the log table exists for - what a board said either side of an update -
        // was the one sequence never reaching it, while every word of it was already arriving
        // here on this branch. The node needs no change for this; both of them are already
        // saying it.
        //
        // ON A CHANGE, and the change test is what makes it affordable. The download reports
        // ~40 times with a moving pct and a FIXED string ("펌웨어 내려받는 중"), so comparing the
        // words collapses the whole transfer to one line and leaves the percentage to the
        // progress bar, which is whose job it is. What survives is the five lines that are
        // actually events: preparing, joining WiFi, checking, downloading, and the verdict -
        // installed, already current, or a failure reason. That fits a twenty-line ring with room
        // for the other board's update beside it.
        //
        // The comparison happens before the copy below overwrites st.text, and it is bounded to
        // NODEPROTO_TEXT because m->text came off the radio and is not promised a terminator.
        bool said_something_new = (m->phase != st.phase) ||
                                  strncmp(st.text, m->text, NODEPROTO_TEXT) != 0;
        copy_text(st.text, sizeof(st.text), m->text, NODEPROTO_TEXT);
        st.status  = st.text;    // published after the copy, never before
        st.phase   = m->phase;
        st.pct     = m->pct;
        st.prog_ms = now;
        // The node's own words, unprefixed. A phase marker would be panel prose in a table whose
        // whole value is that every line in it came off another board, and NODELOG_TEXT is exactly
        // NODEPROTO_TEXT - so a prefix would buy a label by truncating the sentence it labels.
        if (said_something_new) nodelog_add(m->role, st.text);
        // The acknowledgement the resends were waiting for. Any non-idle phase proves the node
        // heard a command and started acting on it - which is a stronger statement than a
        // heartbeat, and the reason the resends are not cleared by one.
        if (m->phase != NODE_PH_IDLE) st.cmd_left = 0;
        break;
    }

    default:
        // A kind this build does not know. Degrade to nothing happened, exactly as nodeproto.h
        // requires of a node receiving an unknown verb.
        s_rx_badkind++;
        break;
    }
}

void nodeota_tick(void) {
    uint32_t now = millis();

    for (uint8_t r = NODE_ROLE_CAM; r < NODE_ROLE_COUNT; r++) {
        NodeState &st = s_st[r];

        if (st.cmd_left > 0 && (now - st.cmd_last_ms) >= NODE_RETRY_MS) {
            st.cmd_left--;
            st.cmd_last_ms = now;
            s_tx_retry++;
            send_cmd(st, r);
        }

        if (!nodeproto_phase_busy(st.phase)) continue;

        // A frozen progress bar is the failure this repo most wants to avoid, so a silence that
        // has outlived its explanation becomes a sentence. The budget depends on which phase is
        // waiting because the phases are quiet for different reasons - see the table at the top
        // of this file; shortening any of them turns a legitimate WiFi join into a lie.
        uint32_t budget = st.phase == NODE_PH_ASK  ? NODE_STALL_ASK_MS
                        : st.phase == NODE_PH_DONE ? NODE_STALL_DONE_MS
                                                   : NODE_STALL_MS;
        if ((now - st.prog_ms) <= budget) continue;

        uint8_t was = st.phase;
        st.phase    = NODE_PH_FAIL;
        st.pct      = NODE_PCT_NONE;
        st.status   = was == NODE_PH_DONE ? K_LOST_REBOOT : K_STALLED;
        st.cmd_left = 0;
        hlogf("[nodeota] %s went quiet for %lums in %s - reporting failure\n",
              nodeproto_role_name(r), (unsigned long)(now - st.prog_ms), phase_name(was));
    }
}

bool nodeota_request(uint8_t role, const char *why) {
    if (role != NODE_ROLE_CAM && role != NODE_ROLE_NODE) {
        // No status to write it into, and nothing to write: the panel is not a target (fwpull.cpp
        // is its own update path) and a role outside the enum has no row.
        hlogf("[nodeota] update request for role %u refused: not an updatable node\n",
              (unsigned)role);
        return false;
    }
    NodeState &st = s_st[role];
    uint32_t now = millis();
    const char *name = nodeproto_role_name(role);

    if (!st.have_mac || st.last_ms == 0) {
        st.status = K_UNKNOWN;
        hlogf("[nodeota] %s update refused: never reported\n", name);
        return false;
    }
    if (st.ver_bad) {
        st.status = K_VER;
        hlogf("[nodeota] %s update refused: nodeproto version mismatch\n", name);
        return false;
    }
    if (!node_online(st, now)) {
        // Refused rather than attempted, because watching is the whole job. A node that is not
        // reporting cannot be watched, so the attempt would consist of three sends into the dark
        // followed by a manufactured failure 20 seconds later - and "장치가 응답하지 않습니다"
        // now is the same answer, arrived at honestly.
        st.status = K_OFFLINE;
        hlogf("[nodeota] %s update refused: silent for %lums\n",
              name, (unsigned long)(now - st.last_ms));
        return false;
    }
    if (!(st.flags & NODEF_CAN_OTA)) {
        // The node computed this from esp_ota_get_next_update_partition(), so it is a fact about
        // the image that is running and not a build flag somebody remembered to set. Offering
        // the button anyway would spend a minute to reach a failure that was knowable up front.
        st.status = K_NO_SLOT;
        hlogf("[nodeota] %s update refused: no second app partition on that board\n", name);
        return false;
    }
    if (nodeproto_phase_busy(st.phase)) {
        st.status = K_BUSY;
        hlogf("[nodeota] %s update refused: already in %s\n", name, phase_name(st.phase));
        return false;
    }
    uint8_t other = nodeota_busy_role();
    if (other != NODE_ROLE_COUNT && other != role) {
        // One at a time. Two nodes pulling megabytes through the same AP while the panel tries to
        // keep a camera stream alive is how all three fail together, and the overlay this drives
        // has room for one device anyway.
        st.status = K_OTHER_BUSY;
        hlogf("[nodeota] %s update refused: %s is updating\n",
              name, nodeproto_role_name(other));
        return false;
    }
    const char *base = sitecfg_base_url();
    if (base == nullptr || base[0] == '\0') {
        // Same wording fwpull.cpp uses for the same condition, deliberately: an unconfigured
        // panel is a working greenhouse controller, not an error state, and it should say so with
        // one voice wherever a person meets it.
        st.status = K_NO_SERVER;
        hlogf("[nodeota] %s update refused: no server configured\n", name);
        return false;
    }

    // Credentials first, and only for the sensor node. It has never needed WiFi for anything
    // else - telemetry is ESP-NOW and always was - so a node provisioned before this feature
    // existed, or never provisioned at all, would fail the download for want of a password this
    // panel has been holding the whole time. One frame, no delay; see camprov_reprovision_node().
    if (role == NODE_ROLE_NODE) camprov_reprovision_node();

    // The overlay has to come up on the press and not on the node's first answer. Up to 700ms of
    // "nothing happened" after a firmware button is how a person learns to press it twice, and
    // arming the phase here does a second job: it starts the stall clock, so a node that never
    // answers at all becomes a readable failure instead of an overlay with no way out.
    st.phase   = NODE_PH_ASK;
    st.pct     = NODE_PCT_NONE;
    st.status  = K_ARMED;
    st.prog_ms = now;

    arm_cmd(st, role, NODE_UPDATE, base, sitecfg_token(), NODE_TRIES);
    hlogf("[nodeota] %s: update requested (%s), seq=%u, base=%s\n",
          name, why ? why : "", (unsigned)st.cmd_seq, base);
    return true;
}

bool nodeota_debug(uint8_t role, const char *verb) {
    if (role != NODE_ROLE_CAM && role != NODE_ROLE_NODE) {
        hlogf("[nodeota] debug verb for role %u refused: not a node\n", (unsigned)role);
        return false;
    }
    if (verb == nullptr || verb[0] == '\0') return false;

    NodeState &st = s_st[role];
    const char *name = nodeproto_role_name(role);

    if (!st.have_mac || st.last_ms == 0) {
        // The only refusal here, and it is a hard one: without a MAC there is nowhere to unicast.
        st.status = K_UNKNOWN;
        hlogf("[nodeota] %s '%s' refused: never reported\n", name, verb);
        return false;
    }
    if (st.ver_bad) {
        st.status = K_VER;
        hlogf("[nodeota] %s '%s' refused: nodeproto version mismatch\n", name, verb);
        return false;
    }
    // Deliberately NOT gated on online, and not on a running update either. A wedged node is by
    // definition one that has stopped reporting, and "reboot" is how it is recovered without a
    // walk to the greenhouse - a liveness check in front of it would refuse exactly the case it
    // exists for. Both nodes service these verbs during a download too, so "log on" can be used
    // to watch one and "reboot" to abandon it.
    bool is_reboot = strcmp(verb, "reboot") == 0;
    arm_cmd(st, role, NODE_DEBUG, verb, "", is_reboot ? NODE_TRIES : (uint8_t)1);
    // Nothing written to `status` on success. It holds the outcome of an update or a refusal
    // (nodeota.h), and overwriting that with "sent a debug verb" would throw away the one line
    // somebody is reading while they poke at the node. The console has this instead.
    hlogf("[nodeota] %s: '%s' sent%s, seq=%u\n",
          name, verb, is_reboot ? " (x3, recovery path)" : "", (unsigned)st.cmd_seq);
    return true;
}

void nodeota_view(uint8_t role, NodeView *out) {
    if (out == nullptr) return;
    memset(out, 0, sizeof(*out));
    out->pct    = -1;
    out->age_ms = UINT32_MAX;
    out->ip[0]  = '-';           // the memset already terminated it
    if (role >= NODE_ROLE_COUNT) return;

    const NodeState &st = s_st[role];

    // Copied even for a role that has never reported: a refusal written by nodeota_request()
    // against exactly that case is the sentence the button needs to show.
    const char *status = st.status;
    if (status) snprintf(out->status, sizeof(out->status), "%s", status);
    if (st.last_ms == 0) return;

    // One `now` for both the freshness verdict and the age printed beside it. Two millis() calls
    // would let a row say "오프라인" next to an age below the threshold that decided it.
    uint32_t now   = millis();
    out->known     = true;
    out->online    = node_online(st, now);
    out->wifi      = (st.flags & NODEF_WIFI) != 0;
    out->pending   = (st.flags & NODEF_PENDING) != 0;
    out->can_ota   = (st.flags & NODEF_CAN_OTA) != 0;
    out->busy      = nodeproto_phase_busy(st.phase);
    out->phase     = st.phase;
    // Downloading only, which is what nodeota.h specifies. The node does send 100 with DONE, and
    // it is tempting to pass that through so the bar reaches the end - but a caller that wants
    // 100% at the finish line has `phase` to read, and two rules for one field is how a bar ends
    // up at 47% on a board that is idle.
    out->pct       = (st.phase == NODE_PH_DL && st.pct <= 100) ? (int)st.pct : -1;
    out->uptime_s  = st.uptime_s;
    out->free_heap = st.free_heap;
    out->age_ms    = now - st.last_ms;
    snprintf(out->ver, sizeof(out->ver), "%s", st.ver);
    if ((st.ip[0] | st.ip[1] | st.ip[2] | st.ip[3]) != 0) {
        snprintf(out->ip, sizeof(out->ip), "%u.%u.%u.%u",
                 st.ip[0], st.ip[1], st.ip[2], st.ip[3]);
    }
    snprintf(out->log, sizeof(out->log), "%s", st.log);
}

uint8_t nodeota_busy_role(void) {
    for (uint8_t r = NODE_ROLE_CAM; r < NODE_ROLE_COUNT; r++) {
        if (nodeproto_phase_busy(s_st[r].phase)) return r;
    }
    return NODE_ROLE_COUNT;
}

void nodeota_debug_tick(void) {
    static uint32_t s_print_ms = 0;
    if (millis() - s_print_ms < 4000) return;
    s_print_ms = millis();

    uint32_t now = millis();
    for (uint8_t r = NODE_ROLE_CAM; r < NODE_ROLE_COUNT; r++) {
        const NodeState &st = s_st[r];
        const char *name = nodeproto_role_name(r);
        if (st.last_ms == 0) {
            hlogf("[nodeota] %-4s no reports yet (board off, out of range, or wrong channel)\n",
                  name);
            continue;
        }
        // Read off the state and not through nodeota_view(): NODEF_VERBOSE has no field in
        // NodeView because no screen needs it, and whether log streaming is actually on is the
        // first thing to check when the lines stop arriving.
        char fl[5];
        int n = 0;
        if (st.flags & NODEF_WIFI)    fl[n++] = 'W';
        if (st.flags & NODEF_PENDING) fl[n++] = 'P';
        if (st.flags & NODEF_CAN_OTA) fl[n++] = 'O';
        if (st.flags & NODEF_VERBOSE) fl[n++] = 'V';
        fl[n] = '\0';
        // reset= sits beside up=, because those two answer one question together and neither
        // answers it alone: uptime says a board restarted, the reason says whether somebody
        // pulled its power or its watchdog fired. Reading only the first is how a power cut gets
        // diagnosed as a firmware bug.
        hlogf("[nodeota] %-4s %s age=%lums ph=%s pct=%d ip=%u.%u.%u.%u heap=%lu up=%lus "
              "reset=%s ver=%s flags=%s%s\n",
              name, node_online(st, now) ? "ONLINE" : "STALE",
              (unsigned long)(now - st.last_ms), phase_name(st.phase),
              (st.phase == NODE_PH_DL && st.pct <= 100) ? (int)st.pct : -1,
              st.ip[0], st.ip[1], st.ip[2], st.ip[3],
              (unsigned long)st.free_heap, (unsigned long)st.uptime_s,
              health_reset_name_of(st.reset_reason),
              st.ver[0] ? st.ver : "-", fl[0] ? fl : "-",
              st.cmd_left > 0 ? " (cmd retrying)" : "");
    }
    hlogf("[nodeota] rx hello=%lu prog=%lu log=%lu dup=%lu badver=%lu badrole=%lu badkind=%lu "
          "panel=%lu | tx cmd=%lu retry=%lu fail=%lu\n",
          (unsigned long)s_rx_hello, (unsigned long)s_rx_prog, (unsigned long)s_rx_log,
          (unsigned long)s_rx_dup, (unsigned long)s_rx_badver, (unsigned long)s_rx_badrole,
          (unsigned long)s_rx_badkind, (unsigned long)s_rx_panel,
          (unsigned long)s_tx_cmd, (unsigned long)s_tx_retry, (unsigned long)s_tx_fail);
}
