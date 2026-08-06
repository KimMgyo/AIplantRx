#include "sensor_bh1750.h"
#include <Wire.h>

static const uint8_t BH1750_ADDR = 0x23;              // ADDR pin low/floating
static const uint8_t BH1750_CONT_H_RES_MODE = 0x10;  // 1 lx resolution, ~120ms conversion

static bool s_started = false;

void bh1750_init(void) {
    Wire.beginTransmission(BH1750_ADDR);
    Wire.write(BH1750_CONT_H_RES_MODE);
    s_started = (Wire.endTransmission() == 0);
}

float bh1750_read(void) {
    if (!s_started) {
        bh1750_init();
        if (!s_started) return -1000.0f;
    }
    if (Wire.requestFrom((int)BH1750_ADDR, 2) != 2) {
        s_started = false;  // re-issue the continuous-mode command next call
        return -1000.0f;
    }
    uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
    return raw / 1.2f;  // datasheet conversion factor
}
