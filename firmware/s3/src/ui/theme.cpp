#include "theme.h"
#include <stdlib.h>

// RGB565 with rounding (lv_color_hex truncates). Grays get a green level that matches red and blue:
// green has one bit more, so a truncated #1C1C1E shows as a green-tinted (24,28,24).
static lv_color_t hx(uint32_t v) {
    int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    int r5 = (r * 31 + 127) / 255, g6 = (g * 63 + 127) / 255, b5 = (b * 31 + 127) / 255;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn <= 6) {
        float shown = (r5 * 255.0f / 31 + b5 * 255.0f / 31) / 2;
        g6 = (int)(shown * 63 / 255 + 0.5f);
    }
    lv_color_t c;
    c.full = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    return c;
}

#define H(x) hx(0x##x)
#define C(x, o) Col{hx(0x##x), (lv_opa_t)(o)}
#define SOLID(x) C(x, 255)
#define NONE Col{lv_color_black(), 0}

static Palette GLASS, DARK, LIGHT;
const Palette *P = &GLASS;

static void build() {
    GLASS = Palette{true, H(2B241D),
        SOLID(FFFFFF), C(FFFFFF, 140), C(FFFFFF, 89), C(FFFFFF, 191), C(FFFFFF, 230),
        H(2F271B), H(40362A), C(FFFFFF, 33), C(FFFFFF, 38),
        H(6B6255), H(FFFFFF), H(837C72), H(FFFFFF), H(504940),
        H(30D158), H(554E44), H(504940), H(FFFFFF), H(746C60), H(D3D1CD),
        H(2F522A), H(375830), H(30D158), H(FF9F0A),
        H(675A48), H(594E3E), H(392F23), H(493E31), H(7F7465), H(958C7F), C(FFFFFF, 26),
        H(564B3C), H(483F32), H(696155), H(5B5245), H(847D73),
        102,
        H(FF9F0A), H(64D2FF), H(BF5AF2),
        NONE};
    DARK = Palette{false, H(1C1C1E),
        SOLID(FFFFFF), C(EBEBF5, 153), C(EBEBF5, 77), C(EBEBF5, 77), C(EBEBF5, 230),
        H(2C2C2E), H(464649), SOLID(3A3A3C), NONE,
        H(3A3A3C), H(FFFFFF), H(0A84FF), H(FFFFFF), H(3E3E42),
        H(30D158), H(444448), H(3E3E42), H(0A84FF), H(48484A), H(AEAEB2),
        H(203D28), H(2D4D36), H(30D158), H(FF9F0A),
        H(2C2C2E), H(2C2C2E), H(3A3A3C), H(4A4A4D), H(48484A), H(0A84FF), NONE,
        H(2C2C2E), H(2C2C2E), H(4A4A4D), H(3A3A3C), H(0A84FF),
        128,
        H(FF9F0A), H(64D2FF), H(BF5AF2),
        SOLID(38383A)};
    LIGHT = Palette{false, H(F2F2F7),
        SOLID(000000), C(3C3C43, 153), C(3C3C43, 77), C(3C3C43, 77), C(3C3C43, 204),
        H(FFFFFF), H(E0E0E3), SOLID(FFFFFF), NONE,
        H(E3E3E9), H(007AFF), H(007AFF), H(FFFFFF), H(EFEFF0),
        H(34C759), H(E9E9EA), H(E5E5EA), H(007AFF), H(D1D1D6), H(8E8E93),
        H(D4EFDE), H(D4EFDE), H(248A3D), H(C76A00),
        H(F2F2F7), H(F2F2F7), H(FFFFFF), H(E0E0E3), H(E3E3E9), H(007AFF), NONE,
        H(D3D6DC), H(D3D6DC), H(FFFFFF), H(ADB3BC), H(007AFF),
        64,
        H(C76A00), H(0071A4), H(8944AB),
        SOLID(C6C6C8)};
}

void theme_use(uint8_t t) {
    static bool built = false;
    if (!built) { build(); built = true; }
    P = t == 1 ? &DARK : t == 2 ? &LIGHT : &GLASS;
}
