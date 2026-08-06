// MLX90640 reader driven entirely from loop() — deliberately no task, no mutex.
//
// The sensor shares Wire with the SCD41 (0x62) and the BH1750FVI (0x23), and
// Arduino's Wire is not thread-safe. A reader task would therefore need a lock
// held across every transfer on the bus, and an MLX frame read preempted
// mid-transfer is exactly how an I2C bus gets wedged. Running from loop()
// removes the hazard instead of guarding it, and the node can afford the
// serialization: it has no latency-critical work. The SCD41 latches its
// data_ready until read, so servicing it a few hundred ms late costs nothing,
// and the telemetry broadcast is on a 4s cadence. What loop() cannot tolerate
// is an unbounded stall, hence the timed waits below and the split that reads
// one subpage per call rather than a whole frame.
#include <Arduino.h>
#include <Wire.h>
extern "C" {
#include "MLX90640_API.h"
#include "MLX90640_I2C_Driver.h"
}
#include "thermal_mlx.h"

static const uint8_t MLX_ADDR = 0x33;
// 16Hz: one subpage every ~62.5ms. Chess mode rewrites half the pixels per
// subpage, so once both halves have been seen every read publishes a complete
// image and the frame rate equals the subpage rate. The frame rate is set here,
// not by a software timer - the sensor is the pacer.
static const uint8_t MLX_REFRESH = 0x05;
static const uint32_t SUBPAGE_PERIOD_MS = 63;

// A subpage is ~1.7KB. At the bus's normal 100kHz that read costs ~150ms and at
// 200kHz ~75ms - both longer than the 62.5ms in which the next subpage arrives,
// so the read itself, not the sensor, would cap the frame rate. 400kHz brings it
// to ~37ms, comfortably inside the period. That is standard I2C fast mode, and
// the SCD41 sharing this bus - rated only to 100kHz - never sees it: the clock
// is raised only for MLX transfers and dropped straight back.
static const uint32_t MLX_I2C_HZ = 400000;
static const uint32_t BUS_I2C_HZ = 100000;

// How long one call sits on the data-ready flag before handing the rest of
// loop() its turn. Longer than a subpage period, so a healthy sensor is
// normally serviced in a single call; a miss is harmless and simply retries.
static const uint32_t READY_POLL_MS = 200;
// A subpage overdue by this much is not merely late, the sensor is wedged.
// Eight subpage periods, so ordinary contention from the other sensors on the
// bus never trips it.
static const uint32_t SUBPAGE_LATE_MS = 8 * SUBPAGE_PERIOD_MS;
// Absent sensor: how long to stay off the bus between re-probe attempts.
static const uint32_t ACQUIRE_BACKOFF_MS = 1000;

static paramsMLX90640 s_params;
static uint16_t s_frame[834];
static float s_to[THERMAL_W * THERMAL_H];

static bool s_sensor_ok = false;
static int s_fail_streak = 0;
// Which chess-mode subpages have been folded into s_to at least once. Chess
// mode rewrites only half the pixels per subpage and leaves the rest alone, so
// s_to needs both halves once before it describes a whole scene — but only
// once. After that every single subpage read yields a complete image, half of
// it one period old, and s_primed switches to publishing per subpage. That
// doubles the frame rate for free; discarding every other read, as an
// accumulate-two scheme does, throws away a finished image every time.
//
// The mask keys on the subpage number the sensor reports rather than counting
// reads: if loop() is late the same subpage can be served twice, and counting
// would then declare priming complete with one half never written — a
// permanent checkerboard.
static uint8_t s_subpage_mask = 0;
static bool s_primed = false;
static uint32_t s_retry_at = 0;    // earliest millis() for the next acquire attempt
static uint32_t s_subpage_at = 0;  // millis() the current subpage started waiting

static uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Classic thermal gradient: black -> violet -> magenta -> orange -> yellow -> white.
static uint16_t palette(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    static const float stops[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    static const uint8_t col[6][3] = {
        {0, 0, 0}, {40, 0, 90}, {140, 10, 80}, {220, 60, 10}, {255, 160, 0}, {255, 255, 200},
    };
    int i = 0;
    while (i < 4 && v > stops[i + 1]) i++;
    float t = (v - stops[i]) / (stops[i + 1] - stops[i]);
    int r = (int)(col[i][0] + t * (col[i + 1][0] - col[i][0]));
    int g = (int)(col[i][1] + t * (col[i + 1][1] - col[i][1]));
    int b = (int)(col[i][2] + t * (col[i + 1][2] - col[i][2]));
    return rgb565(r, g, b);
}

// Melexis's stock GetFrameData() busy-polls the status register with no delay
// between checks, which at 2Hz would burn the CPU for up to half a second per
// subpage. Poll gently instead: check status, then back off before retrying.
// Returns 1 when the flag is up, 0 on a plain timeout (sensor present, data
// simply not ready yet) and -1 on an I2C error, i.e. the sensor is gone.
static int wait_data_ready(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    for (;;) {
        uint16_t status = 0;
        if (MLX90640_I2CRead(MLX_ADDR, MLX90640_STATUS_REG, 1, &status) != 0) {
            return -1;
        }
        if (MLX90640_GET_DATA_READY(status)) {
            return 1;
        }
        if (millis() - t0 >= timeout_ms) {
            return 0;
        }
        delay(15);
    }
}

// One full sensor (re)configuration + calibration load. A freshly (re)plugged
// MLX90640 powers up at its defaults, so refresh rate and chess mode must be
// set every time we (re)acquire it, and the EEPROM calibration re-dumped.
static bool mlx_configure(void) {
    static uint16_t ee[MLX90640_EEPROM_DUMP_NUM];
    MLX90640_SetRefreshRate(MLX_ADDR, MLX_REFRESH);
    MLX90640_SetChessMode(MLX_ADDR);
    if (MLX90640_DumpEE(MLX_ADDR, ee) != 0) return false;
    if (MLX90640_ExtractParameters(ee, &s_params) != 0) return false;
    return true;
}

void thermal_mlx_init(void) {
    MLX90640_I2CInit();
    // Nothing is probed here: the sensor may well be absent at boot and
    // plugged in later, so acquisition lives entirely in the poll path.
    s_retry_at = millis();
}

// Raises the bus for MLX transfers and always drops it back, so an early return
// can never leave the SCD41 being clocked faster than it is rated for.
struct BusSpeed {
    BusSpeed() { Wire.setClock(MLX_I2C_HZ); }
    ~BusSpeed() { Wire.setClock(BUS_I2C_HZ); }
};

bool thermal_mlx_poll(uint16_t *dst, float *max_c, uint16_t *peak_idx) {
    const uint32_t now = millis();
    BusSpeed fast;

    if (!s_sensor_ok) {
        if ((int32_t)(now - s_retry_at) < 0) return false;
        s_retry_at = now + ACQUIRE_BACKOFF_MS;
        // No I2C general-call reset here, unlike the CAM version that had the
        // MLX to itself: a general call resets every device that implements
        // it, and the SCD41 does — an absent camera would silently restart the
        // CO2 sensor's periodic measurement once a second. Re-running the
        // config below is what actually recovers a replugged MLX90640 anyway.
        if (!mlx_configure()) return false;
        s_sensor_ok = true;
        s_fail_streak = 0;
        s_subpage_mask = 0;
        s_primed = false;
        s_subpage_at = millis();
        return false;
    }

    int ready = wait_data_ready(READY_POLL_MS);
    if (ready == 0) {
        if ((int32_t)(now - (s_subpage_at + SUBPAGE_LATE_MS)) < 0) return false;
        ready = -1;  // long overdue: treat it as a dead sensor
    }
    if (ready < 0 || MLX90640_GetFrameData(MLX_ADDR, s_frame) < 0) {
        // Isolated misses happen; a run of them means the sensor was pulled, so
        // drop back to the re-acquire path (which also re-arms it). Either way
        // the half-built frame is discarded.
        s_subpage_mask = 0;
        s_primed = false;
        if (++s_fail_streak >= 3) s_sensor_ok = false;
        return false;
    }
    s_fail_streak = 0;
    s_subpage_at = millis();

    int sp = MLX90640_GetSubPageNumber(s_frame);
    if (sp < 0 || sp > 1) return false;
    float ta = MLX90640_GetTa(s_frame, &s_params);
    MLX90640_CalculateTo(s_frame, &s_params, 0.95f, ta - 8.0f, s_to);

    // Until both halves have been written once, s_to is still part blank. After
    // that it always holds a whole scene, so every subpage publishes.
    if (!s_primed) {
        s_subpage_mask |= (uint8_t)(1u << sp);
        if (s_subpage_mask != 0x03) return false;
        s_primed = true;
    }

    // argmax alongside the min/max, because the panel cannot recover it. The index
    // is in SENSOR space here; the mounting mirror below moves it to output space,
    // where it addresses the pixels that ship beside it. First maximum wins, which
    // matters on a flat hot region: a deterministic pick keeps the marker still
    // instead of dithering between neighbours that read the same temperature.
    float mn = 1e9f, mx = -1e9f;
    int argmax = 0;
    for (int i = 0; i < THERMAL_W * THERMAL_H; i++) {
        float t = s_to[i];
        if (t < mn) mn = t;
        if (t > mx) { mx = t; argmax = i; }
    }
    *max_c = mx;
    // out(x,y) = sensor(W-1-x, H-1-y), so the inverse is the same mapping applied
    // to the sensor coordinates - it is its own inverse. Written out rather than
    // reused from the loop below because getting this backwards puts the marker in
    // the opposite corner and nothing about the picture would look wrong.
    {
        int sx = argmax % THERMAL_W, sy = argmax / THERMAL_W;
        *peak_idx = (uint16_t)((THERMAL_H - 1 - sy) * THERMAL_W + (THERMAL_W - 1 - sx));
    }
    float span = mx - mn;
    if (span < 1.0f) span = 1.0f;  // near-uniform scene: avoid divide blowup

    // The MLX90640's native geometry is exactly the wire format's, so no
    // rescaling happens here — only the mounting correction. Net orientation =
    // horizontal + vertical mirror: out(x,y) = sensor(W-1-x, H-1-y).
    for (int y = 0; y < THERMAL_H; y++) {        // output rows 0..23
        for (int x = 0; x < THERMAL_W; x++) {    // output cols 0..31
            float n = (s_to[(THERMAL_H - 1 - y) * THERMAL_W + (THERMAL_W - 1 - x)] - mn) / span;
            dst[y * THERMAL_W + x] = palette(n);
        }
    }

    // The mirror above and the inverse mirror that produced *peak_idx are two
    // separate pieces of arithmetic that must agree, and if they do not the marker
    // lands in the OPPOSITE CORNER of the panel while every pixel in the picture is
    // still correct - nothing about the image would look wrong and nobody would
    // notice. So check it here, where it is exact rather than a judgement: at the
    // argmax n is (mx-mn)/span, which is 1.0 whenever span was not clamped, so that
    // output pixel must be the palette's top colour. One comparison per frame at
    // ~4fps, silent unless somebody breaks the transform.
    if (mx - mn >= 1.0f && dst[*peak_idx] != palette(1.0f)) {
        Serial.printf("[thermal] BUG: peak_idx %u is not the hottest output pixel "
                      "- the mounting mirror and its inverse disagree\n",
                      (unsigned)*peak_idx);
    }
    return true;
}
