// MLX90640 I2C driver hooks over Arduino Wire. See the header for why this
// exists instead of Melexis's ESP-IDF reference driver.
#include "MLX90640_I2C_Driver.h"
#include <Arduino.h>
#include <Wire.h>

// Wire hands i2cRead() its own rxBuffer, sized I2C_BUFFER_LENGTH (128 bytes by
// default) and NOT bounds-checked against the requested length — asking for
// more than that silently scribbles past the allocation. 64 words is exactly
// 128 bytes, so the largest read this driver issues fits the buffer precisely.
// The MLX90640 auto-increments its address pointer, so splitting a long read
// into chunks costs only one extra address phase each.
#define MLX_READ_CHUNK 64  // words per transaction

extern "C" void MLX90640_I2CInit(void) {
    // Deliberately empty. main.cpp already did Wire.begin(21, 22, 100000) for
    // the shared bus; a second begin() here (or a setClock/setTimeOut) would
    // reconfigure the bus out from under the SCD41 and BH1750.
}

extern "C" int MLX90640_I2CGeneralReset(void) {
    Wire.beginTransmission(0x00);  // general-call address
    Wire.write(0x06);              // reset command
    return (Wire.endTransmission() == 0) ? 0 : -1;
}

extern "C" int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data) {
    uint16_t done = 0;
    while (done < nMemAddressRead) {
        uint16_t n = nMemAddressRead - done;
        if (n > MLX_READ_CHUNK) n = MLX_READ_CHUNK;
        uint16_t addr = startAddress + done;

        Wire.beginTransmission(slaveAddr);
        Wire.write((uint8_t)(addr >> 8));
        Wire.write((uint8_t)(addr & 0xFF));
        if (Wire.endTransmission(false) != 0) return -1;  // repeated start, no stop

        size_t got = Wire.requestFrom(slaveAddr, (size_t)(n * 2), true);
        if (got != (size_t)(n * 2)) return -1;
        for (uint16_t i = 0; i < n; i++) {
            uint8_t hi = Wire.read();
            uint8_t lo = Wire.read();
            data[done + i] = ((uint16_t)hi << 8) | lo;
        }
        done += n;
    }
    return 0;
}

extern "C" int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data) {
    Wire.beginTransmission(slaveAddr);
    Wire.write((uint8_t)(writeAddress >> 8));
    Wire.write((uint8_t)(writeAddress & 0xFF));
    Wire.write((uint8_t)(data >> 8));
    Wire.write((uint8_t)(data & 0xFF));
    if (Wire.endTransmission() != 0) return -1;

    // Read the register back to confirm the write took (matches Melexis stock).
    uint16_t check = 0;
    if (MLX90640_I2CRead(slaveAddr, writeAddress, 1, &check) != 0) return -1;
    return (check == data) ? 0 : -2;
}

extern "C" void MLX90640_I2CFreqSet(int freq) {
    (void)freq;  // the shared bus clock is main.cpp's to set, not this device's
}
