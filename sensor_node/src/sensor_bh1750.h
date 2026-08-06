// BH1750FVI ambient light sensor (I2C). Continuous H-resolution mode: 1 lx
// steps, ~120ms conversion time.
#pragma once

void bh1750_init(void);
float bh1750_read(void);  // lux, or < -999 on I2C error
