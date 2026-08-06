// ESP-NOW sensor-node telemetry (S3 receive side).
//
// A standalone ESP32 devkit carrying an SCD41 (CO2 + temp/humidity), a
// BH1750FVI (lux) and an MLX90640 (thermal camera) broadcasts SensorMsg (see
// camprov.h) every ~2s. camprov.cpp's single ESP-NOW recv callback dispatches
// those packets here via sensornode_on_recv().
#pragma once
#include "camprov.h"  // SensorMsg

// Called from camprov.cpp's on_recv when a validated SensorMsg arrives.
void sensornode_on_recv(const SensorMsg &m);

// True if a SensorMsg has arrived within the last ~15s (the node broadcasts
// every ~2s, so this rides through several missed or late packets).
bool sensornode_online(void);

// Link health for the settings page: how stale the newest reading is, and the
// cumulative accepted/never-arrived counts since boot.
uint32_t sensornode_age_ms(void);
uint32_t sensornode_readings(void);
uint32_t sensornode_lost(void);

// Packets that arrived and carried at least one SCD41 reading outside its sensor's
// physical range - a stuck-at-zero CO2 read, a NaN, a temperature no greenhouse
// reaches. The reading is dropped to the absent sentinel (see the getters below),
// so without this figure a node whose sensor died is indistinguishable from a node
// that is simply quiet: both leave the tiles blank, and only one of them is a link
// problem. Never counts light or soil, which are absent on this installation by
// design and would pin it at every packet.
uint32_t sensornode_rejected(void);

// Latest values from the node. Each is < -999 if that specific sensor never
// reported a reading (absent/faulty on the node) — independent of the other
// fields and independent of sensornode_online(), matching the sentinel
// convention used elsewhere in this codebase (e.g. thermal_max()).
float sensornode_co2(void);   // ppm
float sensornode_temp(void);  // °C
float sensornode_hum(void);   // %RH
float sensornode_lux(void);   // lx
float sensornode_soil(void);  // % (not wired on the node yet; stays < -999)

// Whether this installation's light / soil channel has EVER produced a reading.
//
// Latched once true and never cleared, because these answer "is that sensor fitted
// here", not "is it reading now". The getters above cannot: both return the absent
// sentinel for a probe that was never wired AND for one that stopped talking a
// second ago, which is the same collapse this file's rejection counter exists to
// undo one level up.
//
// The monitor strip uses them to decide whether a tile exists at all. Both are false
// on this board - the BH1750 is broken and the soil probe was never wired - so
// neither tile is built, and neither spends a fifth of the strip drawing "--"
// forever, where a permanent absence is indistinguishable from a sensor that failed
// this minute. Repair the hardware and the tile returns on its first reading with no
// code change; that is the point of latching rather than listing the inventory here.
bool sensornode_has_lux(void);
bool sensornode_has_soil(void);

// Prints the latest received telemetry (or "no packets yet") every ~4s, so
// link problems are visible on the serial console without a debugger. Call
// freely from loop(); it self-throttles.
void sensornode_debug_tick(void);
