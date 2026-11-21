// The other boards: discovery, the camera frame, the sensor node's state and thermal image.
//
// Planned interfaces (UI.md 9):
//   every board   UDP 4210 broadcast {"role":"cam|node|oled","name":..,"ver":..} every 5 s
//   CAM           GET /frame      -> JPEG (QVGA)
//   sensor node   GET /state      -> JSON (see SensorState and README.md)
//                 GET /thermal    -> 32x24 int16 little-endian, centi-degrees C, row-major
//
// Frames are triple-buffered: a fetch task on core 0 writes one buffer while the UI shows another,
// with no copy and no lock held while drawing.
#pragma once
#include <Arduino.h>

enum Role : uint8_t { ROLE_CAM = 0, ROLE_NODE = 1, ROLE_OLED = 2, ROLE_COUNT = 3 };

struct NodeInfo {
    bool online;
    uint32_t age_s;      // since the last announcement
    uint16_t port;       // HTTP port ("port" in the announcement, default 80)
    char ip[16];
    char name[24];
    char ver[24];
};

void nodes_init();
NodeInfo node_info(Role r);
int nodes_online();

// Camera (320x240 RGB565). nullptr until the first frame.
void cam_set_wanted(bool on);          // the main screen is visible
bool cam_live();
const uint16_t *cam_take(uint32_t *seq);
uint32_t cam_seq();             // sequence of the newest frame, without taking it
float cam_fps();

// Sensor node.
struct SensorState {
    bool live;
    float air_c, rh, co2, soil_pct, ec, ph;
    bool heater, light, water, nutr;
    uint16_t heater_min_today, water_ml_today;
    uint8_t nev;
    struct { uint8_t kind; uint16_t at_min, len_min; } ev[16];   // kind: 1 heater, 2 water, 3 nutrient
    uint32_t next_nutr;                 // epoch seconds, 0 unknown
};
SensorState sensor_get();

// Thermal: the 32x24 grid as palette positions scaled to the frame's own range (0..65280, the inferno
// index in 1/256), plus the numbers the overlay needs. The main screen interpolates the 320x240
// picture from it straight into the framebuffer while it draws: no image buffer, no PSRAM read.
struct ThermalInfo {
    bool live;
    float lo, hi, peak_c, leaf_c;
    int peak_x, peak_y;                 // in the 320x240 frame
};
struct ThermalGrid { uint16_t u[24 * 32]; };
bool thermal_grid(ThermalGrid *out, uint32_t *seq);   // false until the first frame
uint32_t thermal_seq();
ThermalInfo thermal_info();
