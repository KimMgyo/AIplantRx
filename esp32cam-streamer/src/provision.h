// ESP-NOW WiFi provisioning (CAM side).
//
// On boot the CAM has no hardcoded WiFi creds. It first tries creds saved in
// its own NVS; if none (or they fail to connect) it hops WiFi channels 1..13
// broadcasting a request, and the S3 display replies with the SSID/password
// the user entered on its settings screen. Creds are cached in NVS so later
// boots skip the handshake. Runs in its own task, so setup() never waits on it.
//
// The constants below MUST byte-for-byte match the S3 side (include/camprov.h).
#pragma once
#include <stdint.h>

static const uint32_t PROV_MAGIC   = 0x5346434D;  // 'SFCM' app id
static const uint8_t  PROV_REQUEST = 1;
static const uint8_t  PROV_REPLY   = 2;
static const uint8_t  PROV_STATUS  = 3;  // CAM -> S3: I'm online at this IP

// XOR obfuscation key for the credential fields. Not strong crypto — it just
// keeps the password from being plainly sniffable on the air. MUST match S3.
static const uint8_t PROV_KEY[16] = {
    0xA5, 0x5A, 0x37, 0xF1, 0x9C, 0x42, 0xD8, 0x6B,
    0x13, 0xE0, 0x7A, 0x2F, 0xC4, 0x88, 0x51, 0x9D,
};

struct __attribute__((packed)) ProvMsg {
    uint32_t magic;
    uint8_t  type;      // PROV_REQUEST or PROV_REPLY
    char     ssid[33];  // REPLY: XOR-obfuscated
    char     pass[65];  // REPLY: XOR-obfuscated
};

// Periodic health beacon the CAM broadcasts once online, so the S3 settings
// page can show where to reach the camera. Distinct size from ProvMsg, which
// is how the receivers tell the two apart.
struct __attribute__((packed)) StatusMsg {
    uint32_t magic;
    uint8_t  type;       // PROV_STATUS
    uint8_t  ip[4];      // STA IPv4
    int8_t   rssi;       // dBm
    uint8_t  connected;  // 1 while joined to WiFi
    char     ssid[33];   // plaintext (not a credential, no need to obfuscate)
};

void provision_start(void);  // init ESP-NOW + spawn the provisioning task
