// Wi-Fi station, clock and mDNS. Everything runs on its own task on core 0; the UI only reads.
#pragma once
#include <Arduino.h>

enum NetState : uint8_t { NET_OFF, NET_IDLE, NET_JOINING, NET_UP };

struct ApInfo {
    char ssid[33];
    int8_t rssi;
    bool secure;
};

void net_init();
NetState net_state();
String net_ssid();              // the network we are on or joining
int net_rssi();
String net_ip();
bool net_time_ok();             // NTP has set the clock

void net_set_enabled(bool on);
void net_scan();                // starts a scan (no-op if one is running)
bool net_scanning();
int net_aps(ApInfo *out, int max);   // last scan, strongest first, duplicates and our own SSID removed
uint32_t net_gen();             // bumps whenever state or scan results change

// Join a network. The saved one is replaced only after this one gives us an address.
void net_join(const char *ssid, const char *pass);
