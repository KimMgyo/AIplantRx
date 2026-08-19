// The CAM's end of the panel <-> node control plane (shared/nodeproto.h).
//
// WHAT THIS BOARD WAS MISSING. The CAM is the one device in the greenhouse with no console
// anybody can reach: it lives on a wall, its UART0 is the flashing port, and the only evidence it
// was ever alive is a StatusMsg beacon carrying an IP. When it stops streaming there is nothing
// to look at. This file gives it the two things it never had - a way to say what it is running
// and how it is doing, and a way to be told to replace itself - both over the ESP-NOW link it
// already has, so neither depends on the WiFi that is usually the thing that broke.
//
// WHY THERE IS NO esp_now_init() IN HERE. There is exactly ONE recv callback per board:
// esp_now_register_recv_cb() replaces, it does not chain. provision.cpp owns the radio - it
// registers the callback, adds the broadcast peer, and re-arms both after every WiFi cycle
// (try_connect() stops the driver on purpose, which wipes the peer list; the comment on
// espnow_rearm() records the outage that taught us). A second init here would silently unhook
// provisioning, and the failure would look like "the S3 stopped answering", which is the last
// place anybody would look. So provision.cpp's on_recv dispatches a 232-byte payload here and
// this file never touches the radio's setup.
//
// WHY REPORTS ARE BROADCAST AND COMMANDS ARE UNICAST. Sending to FF:FF:FF:FF:FF:FF costs the CAM
// no MAC discovery at all - the panel records our address off the recv info and unicasts back.
// The alternative is a handshake this board would have to redo after every re-provision, and the
// sensor node would need the same one. One broadcast peer, already registered, covers it.
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Spawn the reporter/worker task. Call from setup() AFTER provision_start(), which is what brings
// ESP-NOW up. Safe to call before WiFi associates: HELLO travels on ESP-NOW and does not wait for
// a lease. It will not be HEARD until the radio settles on the AP's channel, which is the same
// constraint provisioning already lives with.
void nodeagent_start(void);

// Fed from provision.cpp's single ESP-NOW recv callback once the payload length says NodeCmdMsg.
// Runs on the WiFi task, so it validates, de-duplicates and hands off - it never does the work
// and it never logs (Serial at 115200 would block that task for ~9ms a line).
void nodeagent_on_cmd(const uint8_t *mac, const uint8_t *data, int len);

// Serial.printf, plus a NODE_LOG frame to the panel while verbose is on. Every diagnostic on this
// board goes through here rather than Serial.printf directly: the console is unreachable once the
// board is mounted, and a diagnostic nobody can read is not a diagnostic.
//
// The Serial half is unconditional and byte-for-byte what Serial.printf would have written. The
// radio half is rate limited and drops rather than queues - see the ring in nodeagent.cpp - so a
// failure loop printing every pass cannot flood the channel the sensor node's telemetry shares.
void nodeagent_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// --- implemented in main.cpp, called from nodeagent.cpp ----------------------------------------
//
// Standing the streamer down is main.cpp's job because main.cpp owns the three things that have
// to stop: the capture loop, rtsp_task and http_writer_task. Declared here rather than in a
// header of their own because this file is their only caller and the coupling is worth stating
// in one place.

// Park the capture loop and both serving tasks, then tear the camera driver down, and do not
// return until all three have acknowledged. False when one of them did not park inside the
// budget - in which case nothing was torn down and the board is exactly as it was, because
// deinit()ing the driver out from under a thread sitting inside esp_camera_fb_get() is a crash
// and not a download.
bool camstream_stand_down(void);

// Bring the camera and the serving tasks back. Only ever called after camstream_stand_down()
// returned true.
void camstream_resume(void);

// One line of "what is this board doing", for the NODE_DEBUG "state" dump. Written here because
// frame_count, cam_ok and the socket state are main.cpp's statics and should stay that way.
void camstream_summary(char *out, size_t cap);
