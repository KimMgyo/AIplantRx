// The node's half of the panel <-> node control plane (shared/nodeproto.h).
//
// WHAT THIS BOARD LOOKED LIKE BEFORE. A plain ESP32 devkit in a greenhouse, no display, no
// network, and a serial console that is only a console while somebody is standing in front of it
// with a USB cable. Everything it knew about itself - which image it was running, whether the
// MLX90640 had come back, how much of its telemetry the radio was actually keying - existed for
// exactly as long as a terminal was attached. And the one time that matters is the time nobody
// is attached, because a board that needs looking at is a board that stopped doing its job.
//
// So this module gives it three things it did not have: it says who it is every three seconds
// (NODE_HELLO), it answers a poke from the panel (NODE_DEBUG), and it can be told to go and
// install a new image (NODE_UPDATE, handed straight to nodeota.cpp). The panel has the screen,
// so the panel does the showing; this side only has to be honest and cheap.
//
// CHEAP IS A REQUIREMENT AND NOT A PREFERENCE. This radio already carries a 64-byte reading
// every 2s and a 7-fragment thermal frame at ~4fps, and main.cpp's comments record what happened
// the last time something else wanted airtime: thermal at full rate took the panel's video from
// 13.6fps to 2.5. Everything here is therefore either periodic and tiny (one 192-byte HELLO per
// 3s) or hard rate-limited (nlogf, four lines a second, drops counted and reported). Nothing
// here sweeps channels, and nothing here is allowed to answer a chatty failure path by filling
// the air with it.
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- the radio, owned by main.cpp -------------------------------------------------------
//
// Declared here rather than duplicated: main.cpp holds the channel cache, the send-callback gate
// and the panel's MAC, all of them with measurements attached to why they are shaped the way they
// are. This module borrows them instead of growing a second copy that would hop the radio out
// from under the first.

// Send one frame to the panel. Unicast to its MAC when that is known - which is a deliberate
// departure from "nodes broadcast", because on this air an unacknowledged broadcast that
// collides with the CAM's stream is simply gone, and the panel cannot tell the difference
// between a node that said nothing and a node that is not there. Falls back to broadcast until
// the first ChannelMsg names the panel.
bool nodeagent_radio_send(const void *payload, size_t n);

// Stop hopping. Set for the length of an OTA download: the node is associated to the AP by then,
// and esp_wifi_set_channel() on an associated radio drops the association mid-transfer. It costs
// nothing, because joining put us on the AP's channel and that is the channel the panel - joined
// to the same AP - is already listening on.
void nodeagent_radio_hold(bool held);

// Forget which channel the radio is parked on, so the next send really hops. Called after
// anything that moved the channel behind main.cpp's back (a WiFi association, a driver restart);
// without it every later send believes it is already there and transmits on the wrong one.
void nodeagent_radio_forget_channel(void);

// Re-register the ESP-NOW callbacks and peers. ESP-NOW does not survive the WiFi driver
// stopping, and the OTA join path stops it on purpose to clear this AP's first-auth refusal -
// esp32cam-streamer/src/provision.cpp lost the panel's camera link to exactly this oversight.
// Idempotent: called on a live stack it re-registers what is already registered.
void nodeagent_radio_rearm(void);

// A handful of lines describing what main.cpp can see - the sensors, the link, the display - fed
// through nlogf_always(). Lives there because that is where the numbers are; a getter per value
// would be six accessors for one debug verb.
void nodeagent_state_lines(void);

// ---- the agent ---------------------------------------------------------------------------

void nodeagent_init(void);   // after espnow_init()
void nodeagent_tick(void);   // first thing in loop(), every iteration

// From main.cpp's ESP-NOW receive callback, for any payload it did not recognise itself. True
// when this module took the frame. Does no work beyond a length check, a magic check and a copy:
// it runs on the WiFi task, so NVS writes, HTTP and reboots all wait for tick().
bool nodeagent_on_espnow(const uint8_t *src, const uint8_t *data, int len);

// ---- logging -------------------------------------------------------------------------------
//
// Both write the line to Serial exactly as Serial.printf would, minus the newline the caller no
// longer supplies. The difference is what reaches the air:
//
//   nlogf         only while the panel has asked for it ("log on"). This is the one to reach for
//                 when converting an existing diagnostic - a node that has not been asked for its
//                 logs must cost the radio nothing.
//   nlogf_always  queued whatever the verbose state. For direct answers to a command the panel
//                 just sent: a panel that gets no reply cannot tell "ignored" from "lost", and
//                 refusing to answer "state" because streaming is off would be obtuse.
//
// Both are rate-limited together and both are safe to call before nodeagent_init(). Neither is
// safe to call from an interrupt or from the ESP-NOW receive callback: the queue has one
// producer, the loop task, and that is what keeps it lock-free.
void nlogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nlogf_always(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- for nodeota.cpp -----------------------------------------------------------------------

// Report an update phase to the panel (NODE_PROG). `text` is the Korean phrase a grower reads -
// the node's own words, because the board that knows what happened is the board that should say
// it. `pct` is 0..100 during NODE_PH_DL and NODE_PCT_NONE everywhere else.
void nodeagent_report(uint8_t phase, uint8_t pct, const char *text);

// The credentials learned from a PROV_REPLY, or from NVS on a later boot. False when the node has
// never been provisioned, which is the one OTA failure a grower can fix from the panel.
bool nodeagent_wifi_creds(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
