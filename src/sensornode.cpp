#include <Arduino.h>
#include "sensornode.h"
#include "reading.h"
#include "hlog.h"

static const uint32_t ONLINE_TIMEOUT_MS = 15000;  // rides through several missed 2s broadcasts

static volatile uint32_t s_last_ms = 0;
static volatile uint32_t s_readings = 0;   // distinct seq values accepted
static volatile uint32_t s_dupes = 0;      // repeats of a seq already taken
static volatile uint32_t s_lost = 0;       // gaps in seq: readings that never arrived
// Packets that arrived intact and carried at least one SCD41 reading that cannot
// be a measurement. Distinct from s_lost, which counts packets that never came:
// a live link with a dead sensor and a dead link look identical without this.
static volatile uint32_t s_rejected = 0;
// Has this installation's light / soil channel EVER produced a reading. Both are
// absent on this board - a broken BH1750 and a soil probe that was never wired - and
// these latches are what let the panel tell "not fitted here" from "stopped
// reporting" without either guessing or hard-coding the board's inventory.
static volatile bool s_seen_lux = false;
static volatile bool s_seen_soil = false;
static volatile float s_co2 = -1000.0f;
static volatile float s_temp = -1000.0f;
static volatile float s_hum = -1000.0f;
static volatile float s_lux = -1000.0f;
static volatile float s_soil = -1000.0f;

// A reading outside its sensor's physical range is not a measurement, so it is
// taken as the absent sentinel every consumer on this board already handles.
//
// Nothing checked these before. Whatever the node put in the packet became
// s_temp and s_hum verbatim, and from there VPD, the monitor tiles' verdicts,
// aijudge's judgment rows and the telemetry the server prescribes from - so an
// SCD41 whose I2C read failed and reported zeroes had the panel drawing "0.0 °C",
// filing a finding about it, and the model prescribing against it. The server has
// had scheduler.telemetry_is_sane the whole time; the panel is the half that
// judges these numbers first and had nothing.
//
// The bounds are the sensors' own, not a taste in greenhouse conditions - the
// point is to catch a broken channel, not to disagree with a thermometer. 0 degC
// and 0 %RH are cold and dry but perfectly real, so neither can be rejected; 0 ppm
// CO2 cannot occur in air at all, which is what makes the CO2 floor the one that
// catches a stuck-at-zero read. The RH ceiling is 105 and not 100 on purpose: a
// capacitive element reads slightly over at saturation and aijudge_vpd_kpa()
// already clamps for exactly that (see the note at src/aijudge.cpp:214), so 101%
// is a real saturated reading and must not be blanked here.
//
// NaN falls out for free: both comparisons are false, so it takes the same path as
// an out-of-range value.
//
// `rejected` is what makes the difference visible instead of merely handled. A
// value the node already marked absent is NOT a rejection - that is the node
// saying it has no reading, which is exactly what this returns anyway - so only a
// real number that cannot be one raises the flag. Without that distinction the
// counter would read 100% on this board forever, because the light and soil
// channels are permanently absent by design.
static float in_range(float v, float lo, float hi, bool *rejected) {
    if (v >= lo && v <= hi) return v;
    if (rejected != nullptr && reading_present(v)) *rejected = true;
    return READING_NONE;
}

// The node broadcasts each reading three times on each of thirteen channels, and
// neighbouring 2.4GHz channels overlap, so several copies of the same reading
// land here. Keying on seq turns that into one accepted reading plus a duplicate
// count, and makes gaps in the sequence a genuine loss figure rather than
// something inferred from timing.
void sensornode_on_recv(const SensorMsg &m) {
    static bool have_seq = false;
    static uint8_t last_seq = 0;

    if (have_seq) {
        if (m.seq == last_seq) {
            s_dupes++;
            return;
        }
        // Wrapping subtraction: gap of 1 is the next reading, more means loss.
        uint8_t gap = (uint8_t)(m.seq - last_seq);
        if (gap > 1) s_lost += (uint32_t)(gap - 1);
    }
    have_seq = true;
    last_seq = m.seq;

    // SCD41: -10..60 degC and 0..100 %RH specified, 400..40000 ppm; the bounds here
    // are widened past those so a real extreme is never mistaken for a fault, and a
    // fault value (0 ppm, or anything NaN) still falls outside. BH1750 tops out near
    // 54k lx and the probe on this board is dead anyway; soil is a percentage.
    //
    // Only the SCD41's three feed the rejection count. Light and soil are documented
    // dead on this installation and report absent every packet, so a dead channel
    // that emitted garbage instead of a sentinel would pin the counter at every
    // packet and it would stop meaning anything. The three that stay are the ones
    // whose loss blanks a tile a grower reads and a judgment the panel files.
    bool rejected = false;
    s_co2 = in_range(m.co2_ppm, 300.0f, 40000.0f, &rejected);
    s_temp = in_range(m.temp_c, -20.0f, 70.0f, &rejected);
    s_hum = in_range(m.hum_pct, 0.0f, 105.0f, &rejected);
    s_lux = in_range(m.lux, 0.0f, 200000.0f, nullptr);
    s_soil = in_range(m.soil_pct, 0.0f, 105.0f, nullptr);
    if (rejected) s_rejected++;
    // Latched, never cleared: "this installation has that sensor" is a fact about the
    // hardware, and a sensor that reported once and then dropped out is a fault to
    // show rather than a tile to remove. The panel builds a tile for a channel the
    // moment it first hears from it, so wiring the soil probe or replacing the dead
    // BH1750 brings its tile back with no code change - and until then neither one
    // costs a slot on a strip to say something that never varies. That is the whole
    // reason these two exist: dropping the tiles outright made the panel silent
    // about hardware the design has, and keeping them drawing "--" forever made a
    // permanent absence look like a sensor that failed this minute.
    if (reading_present(s_lux)) s_seen_lux = true;
    if (reading_present(s_soil)) s_seen_soil = true;
    // The packet arrived and its sequence was new, so the link is healthy and the
    // count is a count of packets - not of usable readings. A node whose SCD41 has
    // failed is still a node that is talking, and sensornode_online() staying true
    // while every getter reads absent is the honest description of that: the tiles
    // then say "--" under an online badge rather than the node vanishing, which is
    // a different fault with a different fix. s_rejected is what lets the settings
    // page say which of the two it is instead of leaving a grower to infer it from
    // a healthy count beside six blank tiles.
    s_last_ms = millis();
    s_readings++;
}

bool sensornode_online(void) {
    return s_last_ms != 0 && (millis() - s_last_ms) < ONLINE_TIMEOUT_MS;
}

float sensornode_co2(void)  { return s_co2; }
float sensornode_temp(void) { return s_temp; }
float sensornode_hum(void)  { return s_hum; }
float sensornode_lux(void)  { return s_lux; }
float sensornode_soil(void) { return s_soil; }

// Link health, for the settings page. Age is how stale the newest reading is;
// readings/lost are cumulative since boot.
uint32_t sensornode_age_ms(void) { return s_last_ms == 0 ? 0 : millis() - s_last_ms; }
uint32_t sensornode_readings(void) { return s_readings; }
uint32_t sensornode_lost(void) { return s_lost; }

uint32_t sensornode_rejected(void) { return s_rejected; }

bool sensornode_has_lux(void) { return s_seen_lux; }
bool sensornode_has_soil(void) { return s_seen_soil; }

void sensornode_debug_tick(void) {
    static uint32_t s_print_ms = 0;
    if (millis() - s_print_ms < 4000) return;
    s_print_ms = millis();

    if (s_readings == 0) {
        hlogf("[sensornode] no packets yet (node off, out of range, or wrong WiFi channel)" "\n");
        return;
    }
    hlogf("[sensornode] readings=%lu dup=%lu lost=%lu bad=%lu age=%lums %s "
                  "| co2=%.0f temp=%.1f hum=%.0f lux=%.0f soil=%.0f\n",
                  (unsigned long)s_readings, (unsigned long)s_dupes,
                  (unsigned long)s_lost, (unsigned long)s_rejected,
                  (unsigned long)(millis() - s_last_ms),
                  sensornode_online() ? "ONLINE" : "STALE",
                  s_co2, s_temp, s_hum, s_lux, s_soil);
}
