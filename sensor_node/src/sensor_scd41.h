// Sensirion SCD41 CO2/temp/humidity sensor (I2C, address 0x62).
//
// Periodic mode delivers CO2 plus temp/humidity every ~5s. The CO2
// measurement briefly draws ~200mA; a marginal supply or contact (breadboard
// contacts and an I2C multiplexer in the path were both culprits here) makes
// the sensor abandon its measurement mid-cycle, or drop off the bus entirely.
//
// The driver handles that instead of silently reporting nothing:
//   * if the sensor leaves measurement mode, it is restarted
//   * if CO2 still never arrives, it falls back to RH/T-only single shots,
//     which draw far less current, so temperature and humidity keep working
//   * from that fallback it periodically retries periodic mode, so recovery
//     needs no reboot
#pragma once

void scd41_init(void);  // wake -> stop -> settle -> start periodic
void scd41_tick(void);  // drives the state machine; call roughly once a second

// Latest values, or < -999 when that reading is unavailable.
float scd41_co2(void);   // ppm
float scd41_temp(void);  // °C
float scd41_hum(void);   // %RH

// False while the driver is in RH/T-only fallback (CO2 unavailable).
bool scd41_co2_available(void);
