// Remote firmware updates for the two boards that are not this one.
//
// The panel already knows how to replace itself (fwpull.cpp). This is the same idea aimed
// outward: the CAM and the sensor node fetch their own images over WiFi, and everything else -
// deciding to start, watching it happen, saying so on the screen - happens here, because the
// panel is the only one of the three with a display and the only one a grower can reach.
//
// WHY THE PANEL DOES NOT PROXY THE DOWNLOAD. It would have to hold a megabyte it has no use for,
// on a board whose internal DRAM is already the scarce thing (see the PSRAM note in fwpull.cpp's
// download_and_install). The node has a WiFi stack and an HTTP client of its own; the only thing
// it was missing was somebody to tell it where the server is, and that fits in one 232-byte
// action frame.
//
// WHAT THIS MODULE IS AND IS NOT. It is a state machine over ESP-NOW: send a command, watch the
// reports come back, decide when a silence has become a failure. It is NOT updatemode.cpp - the
// panel must stay fully alive while a node updates, because it is the thing doing the watching.
// The big overlay this drives is a screen, not a mode.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nodeproto.h"

void nodeota_init(void);

// From camprov.cpp's on_recv, once the payload length has identified a NodeRepMsg and the magic
// has been checked. `mac` is the sender, kept so a command can be unicast back rather than
// broadcast at the whole greenhouse.
void nodeota_on_recv(const NodeRepMsg *m, const uint8_t *mac);

// From loop(). Ages the online flags, retries a command that was never acknowledged, and turns a
// node that stopped reporting mid-download into an honest failure instead of a frozen bar.
void nodeota_tick(void);

// Ask a node to update. False when there is nothing to ask - no such node has ever reported, it
// says it cannot OTA, one is already running, or this panel has no server configured to point it
// at. The refusal reason lands in nodeota_view()'s status so the button can explain itself.
//
// Callers: the update page's per-node button, and plantrx.cpp when the server sets node_pull_cam
// or node_pull_node on a poll. The second path is what "pushed from outside" means, and it is why
// the overlay is driven from state rather than from the press.
bool nodeota_request(uint8_t role, const char *why);

// Send a debug verb ("log on", "log off", "state", "reboot"). Same refusal rule: false when the
// node has never been heard from.
bool nodeota_debug(uint8_t role, const char *verb);

// Everything the UI needs about one node, copied out under no lock. The recv callback runs on the
// WiFi task and the UI reads from the LVGL task, so the fields are written in an order that makes
// a torn read harmless: nothing here is cross-checked against anything else, and the worst a
// half-updated view produces is one frame of stale text.
struct NodeView {
    bool     known;        // a report has arrived at least once since boot
    bool     online;       // ...and recently enough to believe
    bool     wifi;
    bool     pending;      // running an image that has not been confirmed
    bool     can_ota;      // a second app partition exists on that board
    bool     busy;         // an update is running: nodeproto_phase_busy(phase)
    uint8_t  phase;        // NodePhase
    int      pct;          // 0..100 while downloading, -1 otherwise
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t age_ms;       // since the last report; UINT32_MAX when never
    char     ver[17];      // elf_sha[8] as lowercase hex, "" when unknown
    char     ip[16];       // "192.168.0.42", or "-"
    char     status[NODEPROTO_TEXT];  // newest NODE_PROG phrase, or the local refusal reason
    char     log[NODEPROTO_TEXT];     // newest NODE_LOG line, or ""
};

void nodeota_view(uint8_t role, NodeView *out);

// The role whose update is running, or NODE_ROLE_COUNT when none is. Drives the full-screen
// overlay: one at a time is not a limitation to design around, it is the only thing that can
// happen - a single panel arms these and refuses a second while one runs.
uint8_t nodeota_busy_role(void);

// Prints a per-role summary every ~4s. Same shape and same self-throttling as the other
// *_debug_tick()s, so the serial console keeps being the place a developer looks first.
void nodeota_debug_tick(void);
