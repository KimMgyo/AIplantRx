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

// One TLS connection at a time. Each needs ~40 KB of internal RAM for mbedTLS (the prebuilt framework
// allocates it there), and two at once (Pl@ntNet upload + the update check, 2026-11-21) ran it out:
// mbedtls_ssl_setup -0x7F00. Pl@ntNet, the LLM and the firmware update take this first.
bool tls_lock(uint32_t wait_ms);
void tls_unlock();
// Times DNS, TCP and a full TLS connection to host:port (GET /tlstest). psram=false keeps mbedTLS's big
// buffers internal for that one connection, to compare. Takes the TLS lock. Returns JSON.
#include <WString.h>
String tls_probe(const char *host, uint16_t port, bool psram);
