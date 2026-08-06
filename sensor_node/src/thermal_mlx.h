// MLX90640 thermal camera (0x33) on the node's shared I2C bus. Reads the
// sensor, applies the palette here, and hands main.cpp a finished RGB565 image
// to fragment over ESP-NOW — the S3 only reassembles, scales and blits.
#pragma once
#include <stdint.h>
#include "sensor_protocol.h"

void thermal_mlx_init(void);

// Advances the read state machine; call from loop() on every iteration, it
// paces itself. Returns true when a NEW complete frame is ready, having
// written THERMAL_W*THERMAL_H RGB565 pixels into dst, the scene peak
// temperature into *max_c, and where that peak is into *peak_idx - a row-major
// index into the pixels just written, mounting mirror already applied, so the
// receiver can mark the spot without knowing how this board is oriented.
// Returns false the rest of the time, including whenever the sensor is absent
// or unresponsive; nothing is written to any output in that case.
bool thermal_mlx_poll(uint16_t *dst, float *max_c, uint16_t *peak_idx);
