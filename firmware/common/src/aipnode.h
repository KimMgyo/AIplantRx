// AIplantRx common node module (C3 OLED board, ESP32-CAM, sensor node).
//
// One call brings up everything the S3 expects from every device (ARCHITECTURE.md 3, 6; UI.md 7-8):
//   - Wi-Fi from NVS ("net"/ssid, pass; the CAM's old "cam" namespace is migrated once). With no saved
//     network, or 2 minutes without a connection, it also opens the setup AP "AIPLANT-<name>" (WPA2,
//     AIPNODE_AP_PASS) and keeps retrying the station; the AP closes again once the station is up.
//   - UDP 4210: announces {"role","name","ver","port":80} every 5 s and keeps a table of the others.
//   - HTTP on port 80 (esp_http_server: keep-alive, Content-Length on every reply):
//       GET  /info     who am I, uptime, reset reason, memory, Wi-Fi, OTA state + the device's fields
//       POST /wifi     {"ssid","pass"}: try that network; saved only once it gives an address, else
//                      back to the old one after 30 s
//       POST /ota      {"url","sha256","size"[,"version"]}: download over HTTPS, check size and SHA-256,
//                      switch the boot partition, restart
//       GET  /reboot
//   - Rollback: a new image boots pending and is confirmed once Wi-Fi is up (and self_check() agrees)
//     within 120 s; otherwise the bootloader goes back to the previous image.
//   - espota (ArduinoOTA) and mDNS aiplantrx-<name>.local for development.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>
#include <esp_http_server.h>

#ifndef AIPNODE_AP_PASS
#define AIPNODE_AP_PASS "aiplantrx"
#endif
#define AIPNODE_UDP_PORT 4210

struct AipNodeConfig {
    const char *role = "node";      // "oled", "cam", "node"
    const char *name = "node";      // short name: hostname aiplantrx-<name>, AP AIPLANT-<name>
    const char *version = "0.0.0";
    Print *log = &Serial;           // nullptr: silent (the C3, whose USB carries the S3 relay)
    // Wi-Fi transmit power in 0.25 dBm steps (wifi_power_t), 0 = the default. The C3 SuperMini's antenna
    // is badly matched; at full power it often fails to join and 8.5 dBm (34) is the usual fix.
    int8_t tx_power = 0;
    // On the OTA task before the download: free memory, stop the radio-heavy work, put outputs in a
    // safe state. false refuses the update.
    bool (*before_ota)() = nullptr;
    void (*ota_failed)() = nullptr;     // after before_ota() succeeded and the update then failed
    bool (*self_check)() = nullptr;     // a pending image is confirmed only when this says true
    void (*info_extra)(JsonObject o) = nullptr;   // device fields for /info
};

struct AipPeer {
    char role[8];
    char name[24];
    char ver[24];
    IPAddress ip;
    uint16_t port;
    uint32_t seen_ms;   // millis() of the last announcement
};

void aipnode_begin(const AipNodeConfig &cfg);
httpd_handle_t aipnode_http();          // register the device's own handlers on it
bool aipnode_wifi_up();
String aipnode_ip();
// The most recent announcement from a device with this role, if heard within max_age_ms.
bool aipnode_peer(const char *role, AipPeer *out, uint32_t max_age_ms = 15000);
bool aipnode_ota_busy();                // a download is running (the device should keep quiet)
int aipnode_ota_pct();                  // -1 when idle
// Common console commands, for devices that read their own serial port:
//   "wifi <ssid> <pass>"  "info"  "reboot".  Returns true when the line was one of them.
bool aipnode_command(const char *line, Print &out);
void aipnode_logf(const char *fmt, ...);
// Reply helpers with Content-Length.
esp_err_t aipnode_send_json(httpd_req_t *req, JsonDocument &doc, const char *status = "200 OK");
esp_err_t aipnode_send_text(httpd_req_t *req, const char *status, const char *text);
// Reads a request body (up to cap-1 bytes) into buf; returns its length or -1.
int aipnode_read_body(httpd_req_t *req, char *buf, size_t cap);
