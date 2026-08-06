#pragma once

// The server's MetricKey vocabulary, spelled once.
//
// Three places on this device compare against these strings: the prescription
// parser's band table (src/plantrx.cpp), the panel's own fallback bands
// (src/aijudge.cpp), and the sensor tiles that ask for them
// (src/ui/page_monitor.cpp). A typo in any one of them makes plantrx_band()
// return false, which is indistinguishable from a server that never sent that
// band - so the panel would quietly fall back to its own numbers and nothing
// anywhere would say why. A literal cannot be misspelled in only one place if it
// exists in only one place.
//
// These are the server's five, verbatim: server/app/schema.py's
//   MetricKey = Literal["vpd_kpa", "air_c", "rh_pct", "co2_ppm", "leaf_air_dt_c"]
// Adding a sixth is a schema change on both sides, and this header is where the
// device half of it lands.
//
// METRIC_AIR_C is "air_c" and NOT "temp_c". temp_c is the Sensors field name for
// the same quantity - the number the node broadcasts - while air_c is the metric
// a band can be held against; server/app/derive.py:21 is where the two
// vocabularies meet, and it says so. Getting this backwards returns no band for
// air temperature forever.
//
// Macros rather than `static const char *`, so they can appear in the aggregate
// initialisers both band tables use without each translation unit carrying its
// own copy of five pointers.
#define METRIC_VPD     "vpd_kpa"
#define METRIC_AIR_C   "air_c"
#define METRIC_RH      "rh_pct"
#define METRIC_CO2     "co2_ppm"
#define METRIC_LEAF_DT "leaf_air_dt_c"
