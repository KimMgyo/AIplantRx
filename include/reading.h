#pragma once

// The one "no reading" convention on this device.
//
// Producers write READING_NONE. Consumers ask reading_present(). Nobody writes
// the numbers out again.
//
// PRODUCE -1000, TEST > -999, AND THE GAP IS THE POINT. A sentinel tested for
// equality is a sentinel that fails the day a value passes through a float
// conversion, an interpolation or a JSON round trip and comes back as
// -999.9999. So the test is an inequality with a thousandth of slack, and every
// producer sits a whole degree below the line rather than on it. Nothing on this
// board measures anything near -1000 - not degrees C, not %RH, not ppm, not lux,
// not kPa - so the band between them is unreachable by any real reading.
//
// WHY IT IS HERE AND NOT SPELLED OUT AT EACH SITE. This rule already existed and
// was implemented three times: has_reading() in plantrx.cpp, present() in
// aijudge.cpp, and ten open-coded `> -999.0f` tests in page_monitor.cpp, beside
// four separately named producers (VPD_NONE, SENSOR_NONE, SENSOR_ABSENT,
// THERM_NONE). One rule with eleven copies of its comparison is a rule waiting
// for a `>=` to be typed into one of them, and the failure is silent in the worst
// way: the sentinel is admitted as a reading, so the panel draws -1000.0 °C,
// tints a tile against it and files a judgment about it. There is no test on this
// device to catch that; there is only this header.
static const float READING_NONE = -1000.0f;

static inline bool reading_present(float v) { return v > -999.0f; }
