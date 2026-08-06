// 128x64 mono OLED (SSD1306 / SH1106) on this node's I2C bus. See oled.cpp for why the
// display lives here rather than on the panel.
//
// The module is silkscreened 0x78, which is the 8-bit write address; Wire takes the
// 7-bit form, so it answers at 0x3C. oled_probe() tries 0x3C and 0x3D.
//
// Guest on main.cpp's bus: Wire.begin(21, 22) has already run, so nothing here
// configures pins or speed. Call oled_init() after that, and after the sensors - a
// display that fails to appear must not stop the node broadcasting.
#pragma once
#include <stdint.h>

// Probe 0x3C then 0x3D. Returns the 7-bit address that answered, or 0. This bus has
// three known devices and none of them decodes an address range, so an ACK here means
// what it says.
uint8_t oled_probe(void);

// Probe, send the init sequence, clear. False if nothing answered - the caller treats
// that as "no display fitted", not as something to retry forever.
bool oled_init(void);

// Whether init succeeded. Every draw call is a no-op when false, so a node with no
// display needs no conditionals at the call sites.
bool oled_ready(void);
uint8_t oled_address(void);   // 0 until oled_init() succeeds

// SH1106 parts have 132 columns of RAM with the visible 128 centred, so their pages need
// a 2px offset. SSD1306 needs 0, the default. No register reports which controller it
// is; the symptom of guessing wrong is a picture shifted 2px with noise at one edge.
void oled_set_col_offset(uint8_t off);

// Staged drawing: clear and text write into RAM, flush pushes all 8 pages. Staging is
// what stops a half-drawn glyph reaching the panel.
void oled_clear(void);

// One line of 5x7 text at pixel column x, page row page (0..7, 8px each), scaled by an
// integer factor - scale 2 is 10x14 and spans two pages. Only the characters a reading
// needs are in the font (digits, . - : % a few capitals, and 0xB0 for the degree ring);
// anything else draws as a space rather than as garbage.
void oled_text(uint8_t x, uint8_t page, const char *s, uint8_t scale);

// Pixels set in the staged frame. For telling "flushed an empty buffer" from "never
// flushed" - on an unlit panel those are the same picture.
int oled_pixels_set(void);

// The display's own status byte, or -1 if it does not answer reads. Bit 6 set means the
// display is OFF - the one fact none of the write-side checks can report.
int oled_status(void);

bool oled_flush(void);
