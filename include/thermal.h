// MLX90640 thermal frames over ESP-NOW (S3 receive side).
//
// The sensor node owns the MLX90640 now (the CAM was already saturated by UXGA
// capture plus its 2 Mbps UART stream). The node reads the sensor, applies the
// palette itself, and broadcasts each frame as a 32x24 RGB565 image plus the
// scene peak temperature — so this side has no I2C and no MLX90640 driver, it
// only reassembles, scales and blits.
//
// ESP-NOW v1 caps one packet at 250 bytes, so the 1540-byte payload arrives as
// THERMAL_FRAG_COUNT ThermalFragMsg fragments (see camprov.h). camprov.cpp's
// single ESP-NOW recv callback hands each of them to thermal_on_frag(), which
// reassembles complete frames into a double buffer; the UI thread copies the
// newest one out via thermal_take_scaled().
#pragma once
#include <stdint.h>
#include "camprov.h"  // ThermalFragMsg, THERMAL_W / THERMAL_H

// Creates the frame mutex. Call once at boot BEFORE camprov_init(), so the
// mutex exists before the first ESP-NOW packet can be dispatched here.
void thermal_init(void);

// Called from camprov.cpp's on_recv for every ThermalFragMsg-sized packet.
// Validates the fragment header itself — nothing upstream vouches for it.
void thermal_on_frag(const ThermalFragMsg &m);

// True if a complete frame was reassembled recently. Generous: broadcasts are
// unacknowledged and one lost fragment costs the whole frame, so a short run of
// drops must not flip the panel to "offline".
bool thermal_live(void);

// The thermal scene peak in °C, or a value below -999 when there is no current
// reading - either no frame has ever arrived, or thermal_live() has gone false
// and the last peak has stopped being a measurement of anything.
//
// The liveness gate lives in the accessor and not at the call sites on purpose.
// The peak is written per frame and never reset, so a raw read of it keeps a
// dead lamp on the wall for hours; asking four call sites each to remember
// thermal_live() is how three of them came to forget. Test `> -999.0f`, the same
// way the sensornode getters are tested, and the answer is honest at any moment.
// There is deliberately no accessor for the stale value.
float thermal_max(void);

// Where the peak thermal_max() reports is, as a row-major index into the
// THERMAL_W x THERMAL_H frame, or THERMAL_PEAK_NONE.
//
// It has to come from the node: the palette is applied there and its mapping never
// crosses the wire, so no colour on this side decodes to a temperature and the
// hottest pixel cannot be found by looking at the picture. See the frame layout in
// include/camprov.h.
//
// THERMAL_PEAK_NONE means one of two things and the caller must treat both the same
// way - draw nothing: the stream is not live, or the node predates the field and
// sent a temperature with no position. Neither is a licence to guess a spot.
int32_t thermal_peak_index(void);

// Frames per second over a rolling ~2s window, for the settings page.
float thermal_fps(void);

// Bilinearly scales the newest thermal frame from THERMAL_W x THERMAL_H into a
// dst_w x dst_h target and clears the fresh flag. False if none new.
bool thermal_take_scaled(uint16_t *dst, int dst_w, int dst_h);

// Same scale without consuming the frame: it neither requires nor clears the
// fresh flag. The monitor page's thermal panel is the fresh flag's consumer, and
// a second caller on thermal_take_scaled would take frames the panel then never
// draws. False only if no frame has ever been reassembled.
bool thermal_peek_scaled(uint16_t *dst, int dst_w, int dst_h);

// Prints fragment/frame/drop counters every ~4s. `dropped` is the one to watch:
// steady drops with fragments still arriving means whole frames never assemble,
// which points at the node sending on the wrong channel or spacing its
// fragments too tightly. Call freely from loop(); it self-throttles.
void thermal_debug_tick(void);
