// 128x64 mono OLED (SSD1306 / SH1106) on this node's existing I2C bus.
//
// WHY IT IS HERE AND NOT ON THE PANEL. It was tried there first and both routes are
// shut. The panel's rear I2C header is shared with a CH422G expander that ACKs 8-bit
// 0x40-0x4F and 0x60-0x7F; an SSD1306's 0x78 falls inside the second group, so a write
// to the display is received by the expander too - and that expander drives the panel's
// touch reset and USB mux. A private bus there needs two free GPIOs, and the only
// connector with two signal pins is behind a switch that disconnects the console and
// esptool along with them.
//
// This bus has none of those problems. It already runs at 0x62 (SCD41), 0x23 (BH1750)
// and 0x33 (MLX90640); 0x3C is free, SDA/SCL are on devkit headers, and the display
// hangs off the same two pins the sensors use - I2C is a bus, so no new pin is needed.
//
// It is also the better place on the merits: this board MEASURES the temperature. The
// panel would have been showing a number that had crossed ESP-NOW to get there, and
// would have shown a stale one whenever that link dropped. Here the reading and its
// display are one hop apart.
//
// Wire, not the IDF driver: main.cpp already owns the bus with Wire.begin(21, 22), so
// this is a guest on it and configures nothing. The panel version had to talk to a
// pre-installed IDF driver; that whole problem is absent here.
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "oled.h"

static uint8_t s_addr = 0;      // 7-bit, 0 until probed
static uint8_t s_col_off = 0;   // 2 for SH1106
static bool s_ok = false;
static uint8_t s_fb[8][128];    // staged, so a partial redraw never shows a half glyph
static uint8_t s_last_err = 0;   // last Wire.endTransmission() code, for the log

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

// Control byte 0x00 = the bytes that follow are commands, 0x40 = display data.
static bool write_bytes(uint8_t control, const uint8_t *data, size_t len) {
    if (s_addr == 0) return false;
    // One transmission, never split. Wire's default buffer is I2C_BUFFER_LENGTH = 128,
    // which is one byte short of a control byte plus a 128-byte page, so this used to
    // send a page as four chunks joined by repeated STARTs - and a repeated START
    // mid-page is not something every SSD1306 breakout tolerates. oled_init() raises the
    // buffer to 256 instead, so the whole page goes out as a single write, which is what
    // the datasheet describes.
    s_last_err = 0;   // cleared per write: a sticky code made a later SUCCESS print the
                      // failure before it, which sent one round of debugging at the wrong
                      // transfer entirely
    Wire.beginTransmission(s_addr);
    Wire.write(control);
    if (len > 0) Wire.write(data, len);
    uint8_t e = Wire.endTransmission();
    s_last_err = e;
    return e == 0;
}

// The chip's own opinion of whether it is on.
//
// This is the measurement that was missing for three rounds. Everything else here
// reports whether a WRITE succeeded, and a browned-out or desynced SSD1306 ACKs writes
// happily while showing nothing - so "flush=ok, 448 pixels staged" was compatible with a
// black panel and I kept re-reading the drawing code. Bit 6 of the status byte is the
// display-off flag; reading it asks the question directly.
//
// Returns -1 if the read did not complete. Some breakouts do not implement I2C reads at
// all, in which case this stays -1 and says so rather than pretending.
int oled_status(void) {
    if (s_addr == 0) return -1;
    if (Wire.requestFrom((int)s_addr, 1) != 1) return -1;
    return Wire.read();
}

static bool cmds(const uint8_t *c, size_t n) { return write_bytes(0x00, c, n); }

// ---------------------------------------------------------------------------
// 5x7 font, only the characters a reading needs
// ---------------------------------------------------------------------------

// Five columns per glyph, each a bitmask of 7 rows (bit 0 = top). A full ASCII table
// would be 475 bytes for glyphs nothing draws.
struct Glyph { char c; uint8_t col[5]; };
static const Glyph FONT[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    // The degree ring, so a caller can write "\xB0" in a string.
    {(char)0xB0, {0x00, 0x07, 0x05, 0x07, 0x00}},
};

// A character the table does not have draws as a solid block, not as a space.
//
// It used to draw a space, on the reasoning that a gap beats garbage. That is the wrong
// trade for this project: a silent gap makes a missing glyph look like intended layout,
// and the caller never learns that "UP 12s" lost its U and its s. A block is impossible
// to read as anything but a bug, and it is found the first time the string is displayed
// rather than the first time somebody counts characters.
static const uint8_t MISSING[5] = {0x7F, 0x7F, 0x7F, 0x7F, 0x7F};

static const uint8_t *glyph(char c) {
    for (unsigned i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].c == c) return FONT[i].col;
    }
    return (c == ' ') ? FONT[0].col : MISSING;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// START, address, STOP - nothing written. Wire's endTransmission returns 0 on ACK.
// Trustworthy here in a way it was not on the panel: this bus has three known devices
// on it and none of them decodes a range.
uint8_t oled_probe(void) {
    for (uint8_t a : {0x3C, 0x3D}) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) return a;
    }
    return 0;
}

bool oled_init(void) {
    s_addr = oled_probe();
    if (s_addr == 0) {
        s_ok = false;
            return false;
    }
    // The SSD1306 datasheet's own init flow. Charge pump before display-on, or the
    // panel stays dark with every register correct.
    static const uint8_t INIT[] = {
        0xAE,              // display off while configuring
        0xD5, 0x80,        // clock: divide 1, oscillator freq 8
        0xA8, 0x3F,        // multiplex 64 rows
        0xD3, 0x00,        // no vertical offset
        0x40,              // start line 0
        // BOTH controllers' charge-pump commands, because "12864" is sold as either and
        // they do not share this one. SSD1306 takes 0x8D 0x14; SH1106 takes 0xAD 0x8B and
        // ignores 0x8D. Get it wrong and every write still ACKs while the panel stays
        // completely black - which is exactly the failure this line was added for. Each
        // chip ignores the other's, so sending both is cheaper than detecting which.
        0x8D, 0x14,        // SSD1306: charge pump ON, internal Vcc
        0xAD, 0x8B,        // SH1106:  DC-DC ON, internal Vcc
        // PAGE addressing, not horizontal. oled_flush() positions every page with the
        // 0xB0 / 0x00 / 0x10 trio, and those three commands belong to page mode - in
        // horizontal mode (0x20 0x00) the column and page pointers are set with 0x21 and
        // 0x22 instead, and the trio does not do what it looks like it does.
        //
        // The mismatch was invisible to an all-pixels-on self-test, because a frame where
        // every page is 0xFF looks identical however the pages are ordered. Only sparse
        // content shows it - which is why the panel flashed white on demand and then
        // stayed black with text in the buffer. The self-test below draws two stripes now
        // for exactly that reason.
        //
        // Page mode is also the only mode an SH1106 has, so this is what makes one
        // driver serve both parts.
        0x20, 0x02,        // page addressing: the column pointer stays on its page
        0xA1,              // segment remap: column 127 maps to SEG0
        0xC8,              // COM scan reversed - with A1, a 180 degree rotation
        0xDA, 0x12,        // COM pins: alternative, no L/R remap (64-row part)
        // 0xCF, not 0x7F. Mid contrast on a 3.3V module with a marginal pump is dim
        // enough to look like nothing at all in a lit room.
        0x81, 0xCF,        // contrast high
        0xD9, 0xF1,        // pre-charge
        0xDB, 0x40,        // Vcomh
        0xA4,              // resume from RAM, not all-on
        0xA6,              // not inverted
        0x2E,              // stop any scroll left running from a previous boot
        0xAF,              // display on
    };
    // Init, then ASK whether it worked, and retry if it did not.
    //
    // A power cycle brings the display up correctly and a firmware upload did not, which
    // is a difference of exactly one thing: pulling the plug resets the display too,
    // while esptool resets only the ESP32. If that reset lands mid-page - and a page is
    // 129 bytes at 100kHz, so there is a wide window - the display is left waiting for
    // the rest of a GDDRAM write. Bytes that arrive next are stored as PIXELS, not
    // executed as commands, so the init sequence is consumed by the frame it is trying
    // to configure and every write still ACKs.
    //
    // A fresh START ought to resynchronise that, and mostly does. It did not reliably
    // here, and rather than keep guessing at which byte survives, this checks: bit 6 of
    // the status byte is the display-off flag, so the chip can be asked whether 0xAF took
    // effect. Three attempts, because the failure is a one-off state and not a fault -
    // the first retry clears it by draining whatever data phase was open.
    s_ok = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (!cmds(INIT, sizeof(INIT))) {
            continue;
        }
        int st = oled_status();
        if (st < 0) {
            // No read path on this breakout. Nothing to verify against, so take the
            // write at its word rather than refusing to run.
            Serial.println("[oled] init sent; module does not answer reads, cannot verify");
            s_ok = true;
            break;
        }
        if (!(st & 0x40)) {
            if (attempt > 1) Serial.printf("[oled] init took on attempt %d\n", attempt);
            s_ok = true;
            break;
        }
        // status still reports display-off; the loop tries again
    }
    if (!s_ok) return false;   // the caller retries; see main.cpp's broadcast tick

    // Two stripes, top and bottom - not every pixel on.
    //
    // All-on was the first version and it was a bad test: a frame of solid 0xFF looks the
    // same whatever order the pages arrive in, so it passed while the page addressing was
    // wrong and sent the debugging after the font instead. Filling only page 0 and page 7
    // puts the answer in the geometry: a stripe at the top AND one at the bottom means
    // the page pointer goes where it is told.
    memset(s_fb, 0x00, sizeof(s_fb));
    memset(s_fb[0], 0xFF, 128);
    memset(s_fb[7], 0xFF, 128);
    bool on = oled_flush();
    Serial.printf("[oled] self-test stripes %s\n", on ? "sent" : "FAILED");
    delay(600);
    oled_clear();
    bool off = oled_flush();

    return on && off;
}

bool oled_ready(void) { return s_ok; }
uint8_t oled_address(void) { return s_addr; }
void oled_set_col_offset(uint8_t off) { s_col_off = off; }
void oled_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

// One 5x7 glyph line at a pixel column and a PAGE row, scaled by an integer factor.
// Pages are 8 pixels tall, so scale 1 puts a line on one page and scale 2 spans two -
// which is how a reading gets to be twice the height of its caption with one font table.
void oled_text(uint8_t x, uint8_t page, const char *s, uint8_t scale) {
    if (scale < 1) scale = 1;
    for (; *s != '\0'; s++) {
        const uint8_t *g = glyph(*s);
        for (int c = 0; c < 6; c++) {                 // 5 columns + 1 of spacing
            uint8_t bits = (c < 5) ? g[c] : 0x00;
            for (int sx = 0; sx < scale; sx++) {
                int px = x + (c * scale) + sx;
                if (px < 0 || px >= 128) continue;
                for (int row = 0; row < 7; row++) {
                    if (!(bits & (1u << row))) continue;
                    for (int sy = 0; sy < scale; sy++) {
                        int py = page * 8 + row * scale + sy;
                        if (py < 0 || py >= 64) continue;
                        s_fb[py / 8][px] |= (uint8_t)(1u << (py % 8));
                    }
                }
            }
        }
        x = (uint8_t)(x + 6 * scale);
        if (x >= 128) return;
    }
}

// How many pixels the staged frame actually contains. A flush that succeeds over an
// empty buffer and a flush that never happens look the same on a dark panel, and that
// ambiguity cost a debugging round.
int oled_pixels_set(void) {
    int n = 0;
    for (int p = 0; p < 8; p++) {
        for (int c = 0; c < 128; c++) {
            uint8_t b = s_fb[p][c];
            while (b) { n += (b & 1); b >>= 1; }
        }
    }
    return n;
}

bool oled_flush(void) {
    if (!s_ok) return false;

    // Re-assert the state that survives nothing, every frame.
    //
    // The display came up correctly and then went dark and stayed dark, while every
    // write kept ACKing and the staged frame kept 455 pixels in it. That is what a
    // browned-out SSD1306 looks like from this side: its charge pump draws real current,
    // an ESP-NOW transmit burst on a shared 3V3 rail is enough to drop it, and the part
    // comes back at its power-on defaults - display OFF, pump OFF. The I2C block runs at
    // a fraction of that current and keeps answering the whole time, so the logs say
    // healthy while the panel says nothing.
    //
    // Initialising once assumed the module never loses power. Seven bytes a frame at
    // 0.5Hz buys that assumption away, and the display heals itself within one tick
    // instead of needing a reboot nobody would connect to the cause.
    // SSD1306 opcodes only, and 0xAF LAST.
    //
    // 0xAD is the SH1106's DC-DC command and is not a valid SSD1306 opcode. An unknown
    // opcode that normally takes an argument can leave the command parser expecting one,
    // and anything after it gets eaten as that argument - which would silently swallow
    // the 0xAF this whole block exists to send. Keeping the two parts' pump commands in
    // one burst was convenient and unsafe; the SH1106 form is sent separately, on its
    // own, where a desync costs nothing that follows it.
    static const uint8_t KEEP_SH1106[] = { 0xAD, 0x8B };
    static const uint8_t KEEP[] = {
        0x8D, 0x14,        // charge pump ON
        0x81, 0xCF,        // contrast
        0xAF,              // display ON - last, so nothing can absorb it
    };
    cmds(KEEP_SH1106, sizeof(KEEP_SH1106));   // ignored by an SSD1306; result unused
    if (!cmds(KEEP, sizeof(KEEP))) return false;

    for (uint8_t p = 0; p < 8; p++) {
        const uint8_t addr[] = {
            (uint8_t)(0xB0 | p),                       // page
            (uint8_t)(0x00 | (s_col_off & 0x0F)),      // column low nibble
            (uint8_t)(0x10 | (s_col_off >> 4)),        // column high nibble
        };
        if (!cmds(addr, sizeof(addr))) return false;
        if (!write_bytes(0x40, s_fb[p], 128)) return false;
    }
    return true;
}
