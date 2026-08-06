// MLX90640 I2C driver hooks, implemented against Arduino's Wire library so the
// sensor can share the node's one I2C bus with the SCD41 (0x62) and BH1750FVI
// (0x23). Same wire protocol as Melexis's stock driver: 16-bit register
// addressing, MSB-first, repeated start.
//
// The bus itself is owned by main.cpp (Wire.begin(21, 22, 100000)); nothing
// here configures pins or clock, so bringing the thermal camera up can never
// disturb the other two devices.
#ifndef _MLX90640_I2C_Driver_H_
#define _MLX90640_I2C_Driver_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void MLX90640_I2CInit(void);
int MLX90640_I2CGeneralReset(void);
int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data);
int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data);
void MLX90640_I2CFreqSet(int freq);

#ifdef __cplusplus
}
#endif

#endif
