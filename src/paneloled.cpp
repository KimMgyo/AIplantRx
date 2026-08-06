// The panel's own 128x64 OLED, on the UART2 connector's pins.
//
// WHY THESE PINS, AND WHAT THEY COST. The finished device needs this display here: the
// panel is where a grower stands, and the sensor node lives somewhere down the greenhouse.
// Two routes off this board were tried and only one survives:
//
//  - The rear I2C header is port 0, shared with the GT911 and the CH422G expander. The
//    CH422G ACKs 8-bit 0x40-0x4F and 0x60-0x7F, and an SSD1306's 0x78 is inside the second
//    group - measured with the module physically unplugged, so it is the board and not a
//    phantom. Two devices cannot share an address.
//  - Every other free GPIO (6, 11, 12, 13, 15, 16, and probably 19/20) reaches no
//    connector. Swept all 56 ordered pairs with the display wired to the UART2 header and
//    got nothing, which is what put the pins here instead: the connector is GPIO 43/44,
//    the console's own pins, gated by a switch that hands them either to the USB-serial
//    bridge or to that header.
//
// So this costs the serial console outright. That is only acceptable because OTA works
// first - see src/ota.cpp, and note the ordering was deliberate: proving the network path
// BEFORE taking the wire one is the difference between a trade and a brick. The escape
// hatch survives either way, since esptool talks to the ROM bootloader and does not care
// what this firmware does with UART0.
//
// SERIAL MUST DIE FIRST. UART0's default pins ARE 43 and 44. Routing I2C to them through
// the GPIO matrix does not displace the UART - both peripherals end up driving the same
// pads, and the display sees a bus with a serial console shouting on it. Serial.end()
// releases them, and nothing may call Serial after that.
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "paneloled.h"
#include "sensornode.h"
#include "aijudge.h"
#include "reading.h"

#if PANEL_OLED

// SCL on TXD, SDA on RXD, matching how the module is wired to the header.
static const int SDA_IO = 44;   // RXD
static const int SCL_IO = 43;   // TXD

static uint8_t s_addr = 0;
static bool s_ok = false;
static uint8_t s_fb[8][128];


static bool write_bytes(uint8_t control, const uint8_t *data, size_t len) {
    if (s_addr == 0) return false;
    Wire1.beginTransmission(s_addr);
    Wire1.write(control);
    if (len > 0) Wire1.write(data, len);
    return Wire1.endTransmission() == 0;
}

static bool cmds(const uint8_t *c, size_t n) { return write_bytes(0x00, c, n); }

struct Glyph { char c; uint8_t col[5]; };
static const Glyph FONT[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}}, {'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x41, 0x3E}}, {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}}, {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}}, {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}}, {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {(char)0xB0, {0x00, 0x07, 0x05, 0x07, 0x00}},
};

// A character the table lacks draws as a solid block, not a space: a silent gap makes a
// missing glyph look like intended layout, and it stays missing until somebody counts
// characters. A block is unmistakably a bug, found the first time the string is shown.
static const uint8_t MISSING[5] = {0x7F, 0x7F, 0x7F, 0x7F, 0x7F};

static const uint8_t *glyph(char c) {
    for (unsigned i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].c == c) return FONT[i].col;
    }
    return (c == ' ') ? FONT[0].col : MISSING;
}

static void text(uint8_t x, uint8_t page, const char *s, uint8_t scale) {
    if (scale < 1) scale = 1;
    for (; *s != '\0'; s++) {
        const uint8_t *g = glyph(*s);
        for (int c = 0; c < 6; c++) {
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

static bool flush(void) {
    if (!s_ok) return false;
    // Re-asserted every frame, not just at init. The charge pump is the largest current
    // draw on the module and it browns out before this board does; a reset SSD1306 comes
    // back with the display and the pump OFF while its I2C block keeps ACKing, so without
    // this the panel goes dark permanently and every write still reports success.
    static const uint8_t KEEP[] = {
        0x8D, 0x14,        // charge pump ON
        0x81, 0xCF,        // contrast
        0xAF,              // display ON - last, so nothing can absorb it
    };
    static const uint8_t KEEP_SH1106[] = { 0xAD, 0x8B };
    cmds(KEEP_SH1106, sizeof(KEEP_SH1106));   // not an SSD1306 opcode; sent alone so a
                                              // parser desync cannot swallow the 0xAF
    if (!cmds(KEEP, sizeof(KEEP))) return false;

    for (uint8_t p = 0; p < 8; p++) {
        const uint8_t addr[] = {(uint8_t)(0xB0 | p), 0x00, 0x10};
        if (!cmds(addr, sizeof(addr))) return false;
        if (!write_bytes(0x40, s_fb[p], 128)) return false;
    }
    return true;
}

// The display's own opinion of whether it is on. Bit 6 of the status byte is the
// display-off flag. -1 if the module does not answer reads at all.
static int status_byte(void) {
    if (s_addr == 0) return -1;
    if (Wire1.requestFrom((int)s_addr, 1) != 1) return -1;
    return Wire1.read();
}

static const uint8_t INIT[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    0x8D, 0x14,        // SSD1306 charge pump
    0xAD, 0x8B,        // SH1106 DC-DC
    0x20, 0x02,        // PAGE addressing - what the 0xB0/0x00/0x10 trio in flush() belongs
                       // to, and the only mode an SH1106 has
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40,
    0xA4, 0xA6, 0x2E, 0xAF,
};

// Take the pins, look for the display, and GIVE THEM BACK if it is not there.
//
// UART0 owns 43/44 by default and keeps driving them, so the console has to be released
// before I2C can have the pads. That would normally mean this build has no console at all -
// and no console is exactly the wrong thing to have while finding out why a display does
// not come up. So the trade is conditional: release Serial, look, and on silence restore it.
//
// DECIDED ONCE, AT BOOT, and never revisited. An earlier version retried every 30 seconds,
// cycling Wire1.begin() and Serial.begin() each time - and Wire1.begin() without a matching
// end(), plus a setBufferSize() realloc per attempt, wrecked the heap and crash-looped the
// board on a ~70 second cycle. That is what the infinite reboots were. Flipping the switch
// now needs a reboot, which costs nothing: it is a physical switch, and the hand that moves
// it is already on the board.
//
// STRICT, because a false positive costs the console. An address ACK is not enough - with
// the switch routing these pads to the USB bridge, something on them ACKed once and this
// gave the console away for nothing. The display has to accept the init sequence AND report
// itself on afterwards before it is believed.
bool paneloled_init(void) {
    Serial.flush();
    Serial.end();

    // setBufferSize before begin, once: a page is 129 bytes against a 128-byte default, and
    // raising it after begin() takes the realloc path, where the first long transfer times
    // out while short writes keep working.
    Wire1.setBufferSize(256);
    Wire1.begin(SDA_IO, SCL_IO, 400000);

    s_addr = 0;
    for (uint8_t a : {0x3C, 0x3D}) {
        Wire1.beginTransmission(a);
        if (Wire1.endTransmission() == 0) { s_addr = a; break; }
    }

    bool believed = false;
    if (s_addr != 0 && cmds(INIT, sizeof(INIT))) {
        int st = status_byte();
        // A module with no read path cannot be verified; take the write at its word rather
        // than refusing a display that is probably there. A module that answers and says
        // it is off is not a display that took the init.
        believed = (st < 0) || !(st & 0x40);
    }

    if (!believed) {
        // Nothing here, or nothing that behaves like a display. Hand the pins back so the
        // console returns, and do not come looking again.
        Wire1.end();
        s_addr = 0;
        Serial.begin(115200);
        return false;
    }

    s_ok = true;
    memset(s_fb, 0, sizeof(s_fb));
    return flush();
}

bool paneloled_ready(void) { return s_ok; }

// 0 when no display was found at boot. The settings page reports this, which is the only
// window into it once these pads belong to I2C: there is no console to print to.
uint8_t paneloled_address(void) { return s_addr; }

void paneloled_tick(void) {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();

    // Retry forever if the display is not up. Giving up at boot leaves a dark panel for
    // the rest of the run, and the state this recovers from - an SSD1306 left NACKing its
    // own address after a reset landed mid-transfer - clears on a later attempt.
    // Nothing to do if the display was never found - the pins went back to the console at
    // boot and must not be taken again mid-run.
    if (s_addr == 0) return;

    // A display that WAS found and has stopped answering gets the init sequence again, and
    // nothing more. The bus stays up and the pins stay ours: re-running Wire1.begin() here
    // is what crash-looped this board once already. This recovers the case that actually
    // happens - the charge pump browns out, the part resets to display-off, and its I2C
    // block keeps ACKing the whole time.
    if (!s_ok) {
        s_ok = cmds(INIT, sizeof(INIT));
        if (!s_ok) return;
        memset(s_fb, 0, sizeof(s_fb));
    }

    char buf[24];
    memset(s_fb, 0, sizeof(s_fb));

    // Air temperature large: the one number worth reading from across a room. This is the
    // node's reading, arriving over ESP-NOW - so "--" here means the LINK is down, not
    // that a sensor failed, and holding the last value would hide exactly that.
    text(0, 0, "TEMP", 1);
    float t = sensornode_temp();
    if (reading_present(t)) snprintf(buf, sizeof(buf), "%.1f\xB0" "C", t);
    else                    snprintf(buf, sizeof(buf), "--");
    text(0, 2, buf, 2);

    float rh = sensornode_hum();
    float co2 = sensornode_co2();
    if (reading_present(co2)) snprintf(buf, sizeof(buf), "CO2 %.0f PPM", co2);
    else                      snprintf(buf, sizeof(buf), "CO2 --");
    text(0, 5, buf, 1);

    float vpd = (reading_present(t) && reading_present(rh)) ? aijudge_vpd_kpa(t, rh)
                                                           : -1000.0f;
    if (reading_present(rh)) snprintf(buf, sizeof(buf), "RH %.0f%%", rh);
    else                     snprintf(buf, sizeof(buf), "RH --");
    text(0, 6, buf, 1);

    // VPD, because it is the number the AI prescribes against and the one a grower cannot
    // work out in their head from the two above it.
    if (reading_present(vpd)) snprintf(buf, sizeof(buf), "VPD %.2f KPA", vpd);
    else                      snprintf(buf, sizeof(buf), "VPD --");
    text(0, 7, buf, 1);

    flush();
}

#endif  // PANEL_OLED
