// AIplantRx common node module (CAM, sensor node, OLED board).
//
// One call brings up everything the panel expects from every device (ARCHITECTURE.md 3, UI.md 7-8):
//   - Wi-Fi from NVS ("net"/ssid, pass). No connection for 2 minutes, or no credentials at all, opens
//     the setup AP "AIPLANT-<name>" (WPA2, AIPNODE_AP_PASS) while it keeps retrying the station.
//   - UDP 4210 broadcast every 5 s: {"role","name","ver","port":80}
//   - HTTP on port 80 (esp_http_server, keep-alive, Content-Length on every reply):
//       GET  /info   who am I, uptime, reset reason, memory, Wi-Fi, OTA state (+ the device's own fields)
//       POST /wifi   {"ssid","pass"}: save, then move to that network
//       POST /ota    {"url","sha256","size"}: the device downloads and verifies the image itself
//       GET  /reboot
//   - OTA rollback: a new image must pass self_check() with Wi-Fi up within 60 s or the previous one boots.
//   - espota (ArduinoOTA) for development, mDNS aiplantrx-<name>.local
//   - Serial: "wifi <ssid> <pass>", "info", "reboot"
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>

#ifndef AIPNODE_AP_PASS
#define AIPNODE_AP_PASS "aiplantrx"
#endif

struct AipNodeConfig {
    const char *role;     // "cam", "node", "oled"
    const char *name;     // short device name, also used in hostname and AP SSID
    const char *version;  // firmware version string
    // Called on the OTA task before the download starts: stop whatever eats RAM, PSRAM or radio time,
    // and put outputs in a safe state. Returns false to refuse the update.
    bool (*before_ota)() = nullptr;
    // Called when an update fails after before_ota() succeeded.
    void (*ota_failed)() = nullptr;
    // Called during the first 60 s of a freshly installed image; true confirms the image.
    bool (*self_check)() = nullptr;
    // Adds device fields to /info.
    void (*info_extra)(JsonObject o) = nullptr;
};

void aipnode_begin(const AipNodeConfig &cfg);
// The HTTP server, to register device handlers (register after aipnode_begin).
httpd_handle_t aipnode_http();
bool aipnode_wifi_up();
// Reads one line from Serial if a command is waiting and handles the common commands.
// Call from loop(); returns the line if it was not a common command (empty otherwise).
String aipnode_serial_poll();
// JSON reply helper with Content-Length.
esp_err_t aipnode_send_json(httpd_req_t *req, JsonDocument &doc, const char *status = "200 OK");
