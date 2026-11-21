// The five OLED panels of UI.md 5, drawn from the sensor node's GET /state and GET /thermal.
#pragma once
#include <Adafruit_SSD1306.h>

struct NodeView {
    bool have;              // a /state has arrived at least once
    uint32_t age_ms;        // since the last good /state
    bool found;             // the sensor node is announcing itself
    // Values (NAN when the node does not know them).
    float air_c, rh, co2, soil_c, soil_pct, ec, ph, leaf_c, th_max;
    bool day, water, nutr, ph_pump, capture;
    bool ok_scd41, ok_mlx, ok_soil;
    uint32_t now, last_water, next_nutr;    // epoch seconds (now: the node's clock, else ours), 0 unknown
    // Targets of the profile the node runs.
    float t_lo, t_hi, rh_lo, rh_hi, soil_lo, soil_hi, ec_lo, ec_hi, ph_lo, ph_hi;
    // Thermal, 32x24 centi-degrees, row-major.
    bool have_th;
    int16_t th[24 * 32];
};

// Draws panel idx (0..4 = OLED 1..5) into d's buffer; the caller sends it with d->display().
void ui_panel(Adafruit_SSD1306 *d, int idx, uint32_t ms, const NodeView &v);
// Every OLED while the C3 relays an S3 upload over USB (nothing refreshes meanwhile).
void ui_upload(Adafruit_SSD1306 *d);
// Every OLED while the C3 updates itself over Wi-Fi.
void ui_self_update(Adafruit_SSD1306 *d, int pct);
