// ESP-NOW sensor telemetry protocol (sensor-node side).
//
// MUST byte-for-byte match the S3's copy of these structs in
// smartfarm/include/camprov.h. Broadcast (not unicast) so the S3 doesn't need
// to know this node's MAC address ahead of time.
#pragma once
#include <stdint.h>

static const uint32_t SENSOR_MAGIC = 0x53464E44;  // 'SFND' app id
static const uint8_t  SENSOR_DATA  = 1;

// Reply from the S3, unicast to this node. This node never joins WiFi, so it
// sits on channel 1 and has to broadcast telemetry on all 13 channels to be
// sure of reaching the S3. That is cheap for a 25-byte packet but ruinous for a
// thermal frame, which is 7 fragments — 91 packets a frame. Once the S3 tells
// us which channel it listens on, those 7 fragments go out once. Sent on every
// SensorMsg, so a router channel change self-corrects.
static const uint8_t  SENSOR_CHANNEL = 2;

struct __attribute__((packed)) ChannelMsg {
    uint32_t magic;    // SENSOR_MAGIC
    uint8_t  type;     // SENSOR_CHANNEL
    uint8_t  channel;  // 1..13, the S3's current WiFi channel
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
// seq is not padding either: each reading is broadcast several times across
// several channels, so the receiver needs it to tell a repeat from a new sample,
// and gaps in it are the only honest measure of telemetry loss.
struct __attribute__((packed)) SensorMsg {
    uint32_t magic;     // SENSOR_MAGIC
    uint8_t  type;      // SENSOR_DATA
    uint8_t  seq;       // increments per reading, wraps; repeats are duplicates
    float    co2_ppm;   // SCD41; < -999 = no reading yet / sensor absent
    float    temp_c;    // SCD41 RH/T element, else DHT11; < -999 = no reading
    float    hum_pct;   // same source as temp_c; < -999 = no reading
    float    lux;       // BH1750FVI; < -999 = no reading
    float    soil_pct;  // not wired yet on this node; always < -999
    uint8_t  reserved[38];
};
static_assert(sizeof(SensorMsg) == 64, "SensorMsg must stay 64 bytes; see above");

// --- Thermal frames (MLX90640) ---------------------------------------------
// One frame is a palette-applied RGB565 image, the scene peak temperature, and
// where that peak is:
//
//   uint16_t rgb565[THERMAL_W * THERMAL_H];   // 1536 bytes, row-major
//   float    max_c;                           // 4 bytes, scene peak in °C
//   uint16_t peak_idx;                        // 2 bytes, 0..767, index into rgb565
//
// peak_idx is in OUTPUT space - after the mounting mirror applied in
// thermal_mlx.cpp, so it indexes the pixels shipped beside it and the S3 needs to
// know nothing about how this board is bolted on. It ships because the S3 cannot
// work it out: the palette is applied HERE and its mapping never crosses the wire,
// so on that side no colour decodes to a temperature. This board still has the
// float array; it is the only one that can say where the maximum was.
//
// GROWING THIS PAYLOAD IS BACKWARD COMPATIBLE, and it has to be: the two boards
// flash independently. THERMAL_FRAG_COUNT is unchanged at 7 (ceil(1540/232) and
// ceil(1542/232) are both 7), so an S3 still running the older receiver reassembles
// these frames fine and simply ignores the last two bytes. Never widen this without
// re-checking the fragment count.
//
// ESP-NOW v1 caps a packet at ESP_NOW_MAX_DATA_LEN (250 bytes), so the payload ships
// as THERMAL_FRAG_COUNT fragments. The receiver reassembles by frame_id and drops
// any frame whose fragments don't all arrive — a lost thermal frame just means the
// last good one stays on screen.
//
// The palette is applied on THIS side so the S3 only has to scale and blit.
// MUST byte-for-byte match include/camprov.h in the panel project.
static const uint32_t THERMAL_MAGIC = 0x54484D31;  // 'THM1'
static const uint8_t  THERMAL_FRAG  = 1;

#define THERMAL_W 32
#define THERMAL_H 24
#define THERMAL_PIX_BYTES (THERMAL_W * THERMAL_H * 2)             // 1536
#define THERMAL_PAYLOAD_BYTES (THERMAL_PIX_BYTES + 4 + 2)         // 1542
#define THERMAL_FRAG_BYTES    232                                 // packet = 242 <= 250
#define THERMAL_FRAG_COUNT    ((THERMAL_PAYLOAD_BYTES + THERMAL_FRAG_BYTES - 1) / THERMAL_FRAG_BYTES)
static_assert(THERMAL_FRAG_COUNT == 7,
              "adding to the thermal payload changed the fragment count; an S3 "
              "running the older receiver will stop assembling frames");

struct __attribute__((packed)) ThermalFragMsg {
    uint32_t magic;       // THERMAL_MAGIC
    uint8_t  type;        // THERMAL_FRAG
    uint8_t  frame_id;    // increments per frame, wraps; groups fragments
    uint8_t  frag_index;  // 0 .. frag_count-1
    uint8_t  frag_count;  // THERMAL_FRAG_COUNT
    uint16_t frag_len;    // valid bytes in data[]
    uint8_t  data[THERMAL_FRAG_BYTES];
};
