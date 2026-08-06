#include "sensor_scd41.h"
#include <Arduino.h>
#include <Wire.h>

static const uint8_t SCD41_ADDR = 0x62;

// Settle time after stop_periodic_measurement before starting a measurement.
// The datasheet puts the command's execution time at 500ms. This was 5000ms
// while the whole sequence blocked setup() and nobody was counting the cost;
// 1500ms is still three times the datasheet figure and it is the difference
// between the first CO2 reading landing at ~17s and at ~10s.
static const uint32_t IDLE_SETTLE_MS = 1500;

// The SCD41 needs about a second after power-up before it answers at all. Paid
// once, and no longer blocking - see scd41_init().
static const uint32_t POWER_WAIT_MS = 1000;

// How long periodic mode gets to produce a CO2 reading before falling back to
// RH/T-only. A healthy sensor delivers its first reading at ~5s.
static const uint32_t CO2_PROBATION_MS = 30000;

// From the RH/T-only fallback, how often to retry periodic mode. Long enough
// that a genuinely unusable CO2 path doesn't cost a stall every cycle.
static const uint32_t CO2_RETRY_MS = 300000;  // 5 min

// RH/T-only single shot cadence while in fallback.
static const uint32_t RHT_INTERVAL_MS = 2000;

// MODE_STARTING runs the wake/stop/settle sequence from scd41_tick() instead of
// blocking setup(). It used to be six seconds of straight delay() before loop()
// ever ran, which held up the thermal stream and the telemetry broadcast too -
// the node reported nothing at all for its first seven seconds.
enum Mode { MODE_STARTING, MODE_PERIODIC, MODE_RHT_ONLY };
enum StartStep { ST_POWER_WAIT, ST_SETTLE, ST_DONE };

static Mode s_mode = MODE_STARTING;
static StartStep s_start_step = ST_POWER_WAIT;
static uint32_t s_start_at = 0;       // millis at which the current step is due
static uint32_t s_started_ms = 0;     // periodic mode: probation clock
static uint32_t s_fellback_ms = 0;    // fallback mode: retry clock
static uint32_t s_last_rht_ms = 0;
static float s_co2 = -1000.0f, s_temp = -1000.0f, s_hum = -1000.0f;

// Sensirion CRC-8: poly 0x31, init 0xFF, no reflection — over each 2-byte word.
static uint8_t crc8(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool send_cmd(uint16_t cmd) {
    Wire.beginTransmission(SCD41_ADDR);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)(cmd & 0xFF));
    return Wire.endTransmission() == 0;
}

// Pulls the 3-word measurement frame and converts it. Each word is
// independently CRC-checked; a failed word is left untouched. Pass nullptr for
// co2 when reading an RH/T-only shot, whose CO2 word is meaningless.
// Returns false only on an outright I2C transaction failure.
static bool fetch_measurement(float *co2, float *t, float *h) {
    if (!send_cmd(0xEC05)) return false;  // read_measurement
    delay(2);
    if (Wire.requestFrom((int)SCD41_ADDR, 9) != 9) return false;
    uint8_t b[9];
    for (int i = 0; i < 9; i++) b[i] = Wire.read();
    if (co2 && crc8(&b[0], 2) == b[2]) *co2 = (float)(((uint16_t)b[0] << 8) | b[1]);
    if (crc8(&b[3], 2) == b[5]) *t = -45.0f + 175.0f * (((uint16_t)b[3] << 8) | b[4]) / 65535.0f;
    if (crc8(&b[6], 2) == b[8]) *h = 100.0f * (((uint16_t)b[6] << 8) | b[7]) / 65535.0f;
    return true;
}

static void log_serial(void) {
    if (!send_cmd(0x3682)) {  // get_serial_number
        Serial.println("[scd41] get_serial_number: no I2C ack (sensor absent or faulty)");
        return;
    }
    delay(2);
    if (Wire.requestFrom((int)SCD41_ADDR, 9) != 9) {
        Serial.println("[scd41] get_serial_number: short read");
        return;
    }
    uint8_t b[9];
    for (int i = 0; i < 9; i++) b[i] = Wire.read();
    bool ok = crc8(&b[0], 2) == b[2] && crc8(&b[3], 2) == b[5] && crc8(&b[6], 2) == b[8];
    Serial.printf("[scd41] serial %02X%02X%02X%02X%02X%02X (crc %s)\n",
                  b[0], b[1], b[3], b[4], b[6], b[7], ok ? "OK" : "BAD");
}

// wake_up -> stop -> settle -> start_periodic.
//
// reinit (0x3646) is deliberately NOT sent. It only reloads settings already
// verified to be at factory defaults here, and the bring-up that measures
// reliably on this hardware omits it.
static void start_periodic(void) {
    send_cmd(0x36F6);  // wake_up — a NACK here is normal per the datasheet
    delay(30);
    send_cmd(0x3F86);  // stop_periodic_measurement
    delay(IDLE_SETTLE_MS);
    Serial.printf("[scd41] start_periodic ack=%d\n", send_cmd(0x21B1));
    s_mode = MODE_PERIODIC;
    s_started_ms = millis();
}

// Arms the bring-up sequence and returns immediately. An MCU reset leaves the
// sensor powered and possibly still measuring, a state where get_serial_number
// is rejected, so it still needs the wake + stop + settle - it just no longer
// costs setup() six seconds of delay() to get it. tick_starting() finishes the
// job while the rest of the node is already running.
void scd41_init(void) {
    s_mode = MODE_STARTING;
    s_start_step = ST_POWER_WAIT;
    s_start_at = millis() + POWER_WAIT_MS;
}

static void tick_starting(void) {
    if ((int32_t)(millis() - s_start_at) < 0) return;
    switch (s_start_step) {
        case ST_POWER_WAIT:
            send_cmd(0x36F6);  // wake_up — a NACK here is normal per the datasheet
            delay(30);         // the one delay left, and it is 30ms
            send_cmd(0x3F86);  // stop_periodic_measurement
            s_start_step = ST_SETTLE;
            s_start_at = millis() + IDLE_SETTLE_MS;
            break;
        case ST_SETTLE:
            log_serial();
            // One RH/T-only single shot before committing to periodic mode. It
            // executes in ~50ms, against the ~5s the first periodic measurement
            // takes, so temperature and humidity are on screen almost at once
            // instead of waiting out the CO2 element's warm-up. Single-shot
            // commands only work from idle, which is exactly where we are.
            if (send_cmd(0x2196)) {   // measure_single_shot_rht_only
                delay(100);           // datasheet ~50ms execution; margin for slack
                float t = -1000.0f, h = -1000.0f;
                if (fetch_measurement(nullptr, &t, &h)) {
                    if (t > -999.0f) s_temp = t;
                    if (h > -999.0f) s_hum = h;
                    Serial.printf("[scd41] first RH/T %.1fC %.0f%%\n", s_temp, s_hum);
                }
            }
            Serial.printf("[scd41] start_periodic ack=%d\n", send_cmd(0x21B1));
            s_mode = MODE_PERIODIC;
            s_started_ms = millis();
            s_start_step = ST_DONE;
            break;
        case ST_DONE:
            break;
    }
}

// Leaves periodic mode and settles into idle, where single-shot commands work.
static void fall_back_to_rht(void) {
    Serial.println("[scd41] no CO2 within probation -> RH/T-only single shots "
                   "(will retry periodic in 5 min)");
    send_cmd(0x3F86);  // stop_periodic_measurement
    delay(600);
    s_mode = MODE_RHT_ONLY;
    s_fellback_ms = millis();
    s_co2 = -1000.0f;
    s_last_rht_ms = 0;  // measure immediately on the next tick
}

// Returns false if the status word could not be read at all.
static bool read_status(uint16_t *word) {
    if (!send_cmd(0xE4B8)) return false;  // get_data_ready_status
    delay(2);
    if (Wire.requestFrom((int)SCD41_ADDR, 3) != 3) return false;
    uint8_t hi = Wire.read(), lo = Wire.read(), crc = Wire.read();
    uint8_t w[2] = {hi, lo};
    if (crc8(w, 2) != crc) return false;
    *word = ((uint16_t)hi << 8) | lo;
    return true;
}

static void tick_periodic(void) {
    uint16_t word;
    if (!read_status(&word)) {
        if (millis() - s_started_ms > CO2_PROBATION_MS) fall_back_to_rht();
        return;
    }

    // Low 11 bits hold the ready flag (Sensirion SCD4x datasheet).
    if ((word & 0x07FF) != 0) {
        float co2 = -1000.0f, t = -1000.0f, h = -1000.0f;
        if (fetch_measurement(&co2, &t, &h) && co2 > -999.0f) {
            // Each word has its own CRC, so a word that fails arrives here
            // still holding the -1000 initialiser. Guard per field: one bad
            // temp/RH word must not discard a perfectly good cached reading.
            s_co2 = co2;
            if (t > -999.0f) s_temp = t;
            if (h > -999.0f) s_hum = h;
            s_started_ms = millis();  // healthy: probation doubles as a watchdog
        }
        return;
    }

    // Bit 15 is set throughout measurement mode and clear in idle. If it
    // clears while we believe we're measuring, the sensor abandoned the
    // measurement — the signature of a supply dip during the CO2 element's
    // current spike. Restart rather than poll a stopped sensor forever.
    if (!(word & 0x8000)) {
        Serial.println("[scd41] sensor left measurement mode -> restarting");
        start_periodic();
        return;
    }

    if (millis() - s_started_ms > CO2_PROBATION_MS) fall_back_to_rht();
}

static void tick_rht_only(void) {
    if (millis() - s_fellback_ms > CO2_RETRY_MS) {
        Serial.println("[scd41] retrying periodic mode");
        start_periodic();
        return;
    }
    if (millis() - s_last_rht_ms < RHT_INTERVAL_MS) return;
    s_last_rht_ms = millis();

    if (!send_cmd(0x2196)) return;  // measure_single_shot_rht_only
    delay(100);                     // datasheet ~50ms execution; margin for slack
    float t = -1000.0f, h = -1000.0f;
    if (fetch_measurement(nullptr, &t, &h)) {
        if (t > -999.0f) s_temp = t;
        if (h > -999.0f) s_hum = h;
    }
}

void scd41_tick(void) {
    if (s_mode == MODE_STARTING)      tick_starting();
    else if (s_mode == MODE_PERIODIC) tick_periodic();
    else                              tick_rht_only();
}

float scd41_co2(void)  { return s_co2; }
float scd41_temp(void) { return s_temp; }
float scd41_hum(void)  { return s_hum; }
bool scd41_co2_available(void) { return s_mode == MODE_PERIODIC; }
