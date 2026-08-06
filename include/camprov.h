// ESP-NOW WiFi provisioning (S3 display side — the responder).
//
// The CAM has no hardcoded WiFi creds; it broadcasts a request over ESP-NOW
// while hopping channels. This module replies with the SSID/password the user
// entered on the settings screen (cached here in RAM, kept current by net.cpp),
// so the CAM can join WiFi for its RTSP/HTTP streams without being reflashed.
//
// The constants below MUST byte-for-byte match the CAM side
// (esp32cam-streamer/src/provision.h).
#pragma once
#include <stdint.h>

static const uint32_t PROV_MAGIC   = 0x5346434D;  // 'SFCM' app id
static const uint8_t  PROV_REQUEST = 1;
static const uint8_t  PROV_REPLY   = 2;
static const uint8_t  PROV_STATUS  = 3;  // CAM -> S3: I'm online at this IP

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

struct __attribute__((packed)) StatusMsg {
    uint32_t magic;
    uint8_t  type;       // PROV_STATUS
    uint8_t  ip[4];      // STA IPv4
    int8_t   rssi;       // dBm
    uint8_t  connected;  // 1 while joined to WiFi
    char     ssid[33];   // plaintext (not a credential, no need to obfuscate)
};

// --- Sensor-node telemetry (separate device, separate ESP-NOW message family) ---
// A standalone ESP32 devkit carrying DHT11/SCD41/BH1750 sensors broadcasts this
// periodically (~4s). Distinct magic + distinct struct size from ProvMsg/
// StatusMsg above, so on_recv's size-based dispatch never confuses them.
// MUST byte-for-byte match the sensor node's own copy of this struct.
static const uint32_t SENSOR_MAGIC = 0x53464E44;  // 'SFND' app id
static const uint8_t  SENSOR_DATA  = 1;

// Reply to a SensorMsg, unicast back to the node. The node never joins WiFi, so
// it sits on channel 1 and has to broadcast telemetry on all 13 channels to be
// sure of reaching us. That is cheap for a 25-byte packet but ruinous for a
// thermal frame, which is 7 fragments — 91 packets a frame. Telling the node
// which channel we are actually listening on lets it send those 7 fragments
// once. Sent on every SensorMsg so a router channel change self-corrects.
static const uint8_t  SENSOR_CHANNEL = 2;

struct __attribute__((packed)) ChannelMsg {
    uint32_t magic;    // SENSOR_MAGIC
    uint8_t  type;     // SENSOR_CHANNEL
    uint8_t  channel;  // 1..13, our current WiFi channel
};

// Payload length decides whether this message is delivered at all. Measured on
// this hardware, same sender, same channel, thousands of broadcasts each:
//   25 bytes  -> never arrived, not once
//   26 bytes  -> arrived at boot, then stopped once the S3 associated to WiFi
//   100 bytes -> arrived continuously
//   242 bytes -> arrived continuously (the thermal fragments)
// Short 802.11 action frames are simply not carried dependably here, so the
// struct is padded to a comfortable 64. Do not shrink it.
//
// The reserved block earns its keep twice: it holds that length, and it leaves
// room to add readings (pH, EC, water level, ...) without a protocol break —
// new fields come out of it and sizeof stays 64 on both sides.
//
// seq is not padding either: the node broadcasts each reading several times
// across several channels, so the receiver needs it to tell a repeat from a new
// sample, and gaps in it are the only honest measure of telemetry loss.
struct __attribute__((packed)) SensorMsg {
    uint32_t magic;     // SENSOR_MAGIC
    uint8_t  type;      // SENSOR_DATA
    uint8_t  seq;       // increments per reading, wraps; repeats are duplicates
    float    co2_ppm;   // SCD41; < -999 = no reading yet / sensor absent
    float    temp_c;    // SCD41 RH/T element, else DHT11; < -999 = no reading
    float    hum_pct;   // same source as temp_c; < -999 = no reading
    float    lux;       // BH1750FVI; < -999 = no reading
    float    soil_pct;  // not wired yet on the node; stays < -999
    uint8_t  reserved[38];
};
static_assert(sizeof(SensorMsg) == 64, "SensorMsg must stay 64 bytes; see above");

// --- Thermal frames from the sensor node (MLX90640) ------------------------
// One frame is a palette-applied RGB565 image, the scene peak temperature, and
// where that peak is:
//
//   uint16_t rgb565[THERMAL_W * THERMAL_H];   // 1536 bytes, row-major
//   float    max_c;                           // 4 bytes, scene peak in °C
//   uint16_t peak_idx;                        // 2 bytes, 0..767, index into rgb565
//
// peak_idx is in OUTPUT space, after the node's mounting mirror - the same
// row-major index that addresses the pixels above it, so the receiver needs to
// know nothing about how the node is bolted on. It exists because the panel
// cannot recover it: the palette is applied on the node and its mapping never
// crosses the wire, so no colour on this side decodes to a temperature and the
// hottest pixel is not findable by looking. Only the node, which still has the
// float array, can say where it was.
//
// GROWING THIS PAYLOAD IS BACKWARD COMPATIBLE, and it has to be: the two boards
// flash independently. THERMAL_FRAG_COUNT is unchanged at 7 (ceil(1540/232) and
// ceil(1542/232) are both 7), so an old node's fragments still reassemble - its
// last fragment is simply 148 bytes instead of 150. thermal.cpp therefore counts
// the bytes it actually received rather than trusting the buffer's size, and
// reports "no position" for a frame that carried none. Never widen this without
// checking the fragment count, and never assume a full buffer means a full frame.
//
// ESP-NOW v1 caps a packet at ESP_NOW_MAX_DATA_LEN (250 bytes), so the payload
// arrives as THERMAL_FRAG_COUNT fragments. thermal.cpp reassembles by frame_id and
// drops any frame whose fragments don't all arrive — a lost thermal frame just
// means the last good one stays on screen.
//
// The palette is applied on the node, so this side only scales and blits.
// MUST byte-for-byte match the sensor node's own copy of this struct.
static const uint32_t THERMAL_MAGIC = 0x54484D31;  // 'THM1'
static const uint8_t  THERMAL_FRAG  = 1;

#define THERMAL_W 32
#define THERMAL_H 24
#define THERMAL_PEAK_NONE (-1)                     // no position on this frame
#define THERMAL_PIX_BYTES (THERMAL_W * THERMAL_H * 2)             // 1536
#define THERMAL_LEGACY_BYTES (THERMAL_PIX_BYTES + 4)              // 1540, pre-peak_idx
#define THERMAL_PAYLOAD_BYTES (THERMAL_LEGACY_BYTES + 2)          // 1542
#define THERMAL_FRAG_BYTES    232                                 // packet = 242 <= 250
#define THERMAL_FRAG_COUNT    ((THERMAL_PAYLOAD_BYTES + THERMAL_FRAG_BYTES - 1) / THERMAL_FRAG_BYTES)
static_assert(THERMAL_FRAG_COUNT == 7,
              "adding to the thermal payload changed the fragment count; an old "
              "node's frames will no longer reassemble - see the note above");

struct __attribute__((packed)) ThermalFragMsg {
    uint32_t magic;       // THERMAL_MAGIC
    uint8_t  type;        // THERMAL_FRAG
    uint8_t  frame_id;    // increments per frame, wraps; groups fragments
    uint8_t  frag_index;  // 0 .. frag_count-1
    uint8_t  frag_count;  // THERMAL_FRAG_COUNT
    uint16_t frag_len;    // valid bytes in data[]
    uint8_t  data[THERMAL_FRAG_BYTES];
};

void camprov_init(void);  // init ESP-NOW responder; call once after net_init()

// Cache the creds the CAM should receive. Call from net whenever they are known
// or change (safe to call before camprov_init; just fills a RAM buffer).
void camprov_set_credentials(const char *ssid, const char *pass);

// Push the given creds to the CAM right now (unsolicited), while the S3 is still
// on the OLD network's channel. Call just before switching WiFi so the CAM can
// re-join the S3's new network (ESP-NOW needs a shared channel to reach it).
void camprov_push_to_cam(const char *ssid, const char *pass);

// Latest CAM health, from its ESP-NOW status beacon. `online` is false until a
// beacon arrives and again if none has come for a while.
bool camprov_cam_online(void);
void camprov_cam_ip(char *buf, size_t n);    // "192.168.0.42" or "-"
bool camprov_cam_ip4(uint8_t out[4]);        // raw IPv4; false if offline/unknown
int  camprov_cam_rssi(void);                 // dBm, 0 if offline
void camprov_cam_mac(char *buf, size_t n);   // "AA:BB:..." or "-"
void camprov_cam_ssid(char *buf, size_t n);  // WiFi network name, or "-"

// Prints a per-kind tally of every ESP-NOW packet the radio delivered, every
// ~4s. Distinguishes "never arrived" from "arrived and was rejected". Call
// freely from loop(); it self-throttles.
void camprov_debug_tick(void);
