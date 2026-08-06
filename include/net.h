// WiFi + NTP service. UI-agnostic; poll from an LVGL timer (net_poll) and
// read state through the getters — never touch UI from WiFi callbacks.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NET_SCAN_MAX 8

enum NetState {
    NET_OFF,           // radio disabled by the user
    NET_DISCONNECTED,  // enabled, no link
    NET_CONNECTING,    // WiFi.begin() in flight (bounded by timeout)
    NET_CONNECTED,
};

// Why the last association attempt failed, for a screen rather than for a retry.
// NET_DISCONNECTED on its own cannot tell a wrong password from an absent network
// from an AP that is rebooting, and the driver knows which within a second or two.
//
// NONE is also what a not-yet-trustworthy failure reads as: net_fail() stays quiet
// until three consecutive failures, because the AP this board is deployed against
// rejects the first association of every boot with AUTH_FAIL on a correct password
// (see the note in net_poll). An absent verdict is never a claim; a wrong one is.
enum NetFail {
    NET_FAIL_NONE,       // connected, or nothing worth reporting yet
    NET_FAIL_NOT_FOUND,  // the SSID was not on the air
    NET_FAIL_AUTH,       // the PSK did not verify - the password is the suspect
    NET_FAIL_OTHER,      // a real failure with no plain-language cause; see the code
};

struct NetScanItem {
    char ssid[33];
    int rssi;
    bool secured;
};

void net_init(void);                 // NVS creds + autoconnect; call once at boot
void net_poll(void);                 // call ~1Hz from an LVGL timer

void net_set_enabled(bool en);       // radio on/off, persisted
bool net_enabled(void);

NetState net_state(void);
int net_strength(void);              // 0..4 bars (0 when not connected)
NetFail net_fail(void);              // why the last attempt failed, once it is trustworthy
uint8_t net_fail_code(void);         // raw WIFI_REASON_*, for NET_FAIL_OTHER and the log
int net_rssi(void);                  // dBm, 0 when not connected
const char *net_ssid(void);          // current/target SSID, "" if none
void net_ip(char *buf, size_t n);
void net_mac(char *buf, size_t n);   // this board's STA MAC, "AA:BB:..."
bool net_time_valid(void);           // NTP has produced a sane wall clock

// Uplink liveness. The driver keeps WiFi.status() at WL_CONNECTED through the
// "associated but no data" state it falls into after some roams/scans - ESP-NOW
// still flows, so the CAM shows online, but every outbound TCP to the LAN times
// out. net_poll's reconnect is gated on a LOST association, which this is not, so
// only a reboot cleared it by hand. These let net_poll see it and cycle the radio,
// the one thing measured to recover the link short of a power cut.
//
// A success from ANY LAN peer (a camnet frame, a plantrx reply) proves the uplink
// is alive and resets the watchdog. But only the SERVER poll votes it DEAD: a
// camera can be off or on its own bad AP while this panel's uplink is fine, and
// cycling for that would drop a working link to chase a problem the camera owns.
void net_note_uplink(void);          // a LAN TCP exchange succeeded (any peer)
void net_note_uplink_fail(void);     // the server poll's TCP connect timed out

void net_scan_start(void);
void net_scan_abort(void);           // stop a sweep so the camera gets the radio back
bool net_scanning(void);
bool net_scan_fresh(void);           // new results since last clear
void net_scan_clear_fresh(void);
int net_scan_count(void);
const NetScanItem *net_scan_item(int i);

void net_connect(const char *ssid, const char *pass);  // creds saved on success
