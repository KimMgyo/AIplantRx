#!/usr/bin/env python3
"""OLED fonts, icons and the demo thermal thumbnail for the C3 -> src/oled_assets.h.

Rasterises at 1 bit with PIL the same way ui/mockups/make_mockups.py draws the OLED mockups (UI.md 5):
Pretendard SemiBold for titles and big numbers, Galmuri11 12 px for small text (Pretendard breaks up
below 13 px at 1 bit), Framework7 icons 16 px.  Keeps only the characters that appear in the string
literals of src/oled_ui.cpp (plus printable ASCII for the two text fonts), so run it again after
changing any text there:

    python tools/gen_oled_assets.py

Pretendard comes from the user font folder (as for the mockups); Galmuri11 and the icon font from ui/fonts.
"""
import json
import os
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
UI_FONTS = os.path.join(ROOT, "ui", "fonts")
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))
FONT_DIRS = [UI_FONTS, os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Windows\Fonts"), r"C:\Windows\Fonts"]
PRET = "Pretendard-SemiBold.otf"
ICONS = ["thermometer", "drop", "scope", "drop_fill", "eyedropper_halffull", "arrow_down_circle"]


def find_font(name):
    for d in FONT_DIRS:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    raise FileNotFoundError(name)


def literal_chars():
    s = open(os.path.join(SRC, "oled_ui.cpp"), encoding="utf-8").read()
    s = re.sub(r"//[^\n]*", "", s)
    out = set()
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', s):
        out |= set(m.group(1))
    return out


def glyph(font, ch):
    """Ink of one character relative to the pen position on the baseline, and its advance."""
    adv = int(round(font.getlength(ch)))
    l, t, r, b = font.getbbox(ch, anchor="ls")
    if r <= l or b <= t:
        return dict(w=0, h=0, x=0, y=0, adv=adv, rows=[])
    pad = 2
    img = Image.new("1", (r - l + 2 * pad, b - t + 2 * pad), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((pad - l, pad - t), ch, font=font, fill=1, anchor="ls")
    bb = img.getbbox()
    if not bb:
        return dict(w=0, h=0, x=0, y=0, adv=adv, rows=[])
    ink = img.crop(bb)
    w, h = ink.size
    a = np.array(ink, dtype=bool)
    rows = []
    for y in range(h):
        bits = 0
        nb = (w + 7) // 8
        for x in range(w):
            if a[y, x]:
                bits |= 1 << (nb * 8 - 1 - x)
        rows.extend(bits.to_bytes(nb, "big"))
    return dict(w=w, h=h, x=bb[0] - pad + l, y=bb[1] - pad + t, adv=adv, rows=rows)


def font_block(name, path, size, chars):
    font = ImageFont.truetype(path, size)
    asc, desc = font.getmetrics()
    bits, glyphs = [], []
    notdef = font.getmask("￿")
    for ch in sorted(chars, key=ord):
        m = font.getmask(ch)
        if ch != " " and (not m.getbbox() or (m.size == notdef.size and bytes(m) == bytes(notdef))):
            raise SystemExit(f"{os.path.basename(path)} {size}px has no glyph for {ch!r} (U+{ord(ch):04X})")
        g = glyph(font, ch)
        assert len(bits) < 65536 and -128 <= g["x"] < 128 and -128 <= g["y"] < 128
        glyphs.append(f"{{0x{ord(ch):04X}, {len(bits)}, {g['w']}, {g['h']}, {g['x']}, {g['y']}, {g['adv']}}}")
        bits.extend(g["rows"])
    mid = int(round((asc - desc) / 2))
    out = [f"// {os.path.basename(path)} {size} px: {len(glyphs)} glyphs, {len(bits)} bytes",
           f"static const uint8_t {name}_bits[] = {{"]
    for i in range(0, len(bits), 24):
        out.append("    " + ", ".join(f"0x{v:02X}" for v in bits[i:i + 24]) + ",")
    out.append("};")
    out.append(f"static const OGlyph {name}_glyphs[] = {{")
    for i in range(0, len(glyphs), 4):
        out.append("    " + ", ".join(glyphs[i:i + 4]) + ",")
    out.append("};")
    out.append(f"static const OFont {name} = {{{name}_glyphs, {len(glyphs)}, {name}_bits, {mid}}};")
    return "\n".join(out)


def thermal_demo(w=44, h=33):
    """The mockup's thermal frame (make_mockups.thermal_data) in 3 levels: cold black, room checkered, warm white."""
    hh, ww = 24, 32
    y, x = np.mgrid[0:hh, 0:ww].astype(float)
    t = 24.4 + 0.5 * (y / hh)
    t -= 2.0 * np.exp(-(((x - 15) / 6.0) ** 2 + ((y - 9) / 4.6) ** 2))
    t -= 1.1 * np.exp(-(((x - 21) / 3.8) ** 2 + ((y - 12) / 3.2) ** 2))
    t += 2.6 * np.exp(-(((x - 16) / 5.0) ** 2 + ((y - 21) / 2.2) ** 2))
    t += 1.6 * np.exp(-(((x - 3) / 2.6) ** 2 + ((y - 2) / 2.2) ** 2))
    t += np.random.default_rng(3).normal(0, 0.08, t.shape)
    g = np.array(Image.fromarray(t.astype(np.float32)).resize((w, h), Image.BILINEAR))
    g = (g - g.min()) / (g.max() - g.min())
    yy, xx = np.mgrid[0:h, 0:w]
    on = np.where(g < 0.33, False, np.where(g > 0.62, True, (xx + yy) % 2 == 0))
    nb = (w + 7) // 8
    rows = []
    for r in range(h):
        bits = 0
        for c in range(w):
            if on[r, c]:
                bits |= 1 << (nb * 8 - 1 - c)
        rows.extend(bits.to_bytes(nb, "big"))
    return rows


def main():
    lit = literal_chars()
    text = {chr(c) for c in range(0x20, 0x7F)} | {c for c in lit if ord(c) > 0x7E}
    digits = set("0123456789.-+")
    f7 = json.load(open(os.path.join(UI_FONTS, "framework7-icons-pua.json")))
    icons = {n: chr(int(f7[n], 16)) for n in ICONS}
    pret = find_font(PRET)
    blocks = [
        font_block("F_TITLE", pret, 13, text),
        font_block("F_BIG30", pret, 30, digits),
        font_block("F_BIG24", pret, 24, digits),
        font_block("F_BIG22", pret, 22, digits),
        font_block("F_BIG20", pret, 20, digits),
        font_block("F_SMALL", find_font("Galmuri11.ttf"), 12, text),
        font_block("F_ICON", find_font("Framework7Icons-PUA.ttf"), 16, set(icons.values())),
    ]
    th = thermal_demo()
    out = ["// Generated by tools/gen_oled_assets.py from Pretendard, Galmuri11 and Framework7 Icons. Do not edit.",
           "// Glyph: codepoint, offset into bits, ink width and height, ink offset from the pen on the baseline,",
           "// advance.  Bits are row-major, each row padded to whole bytes, MSB first (Adafruit drawBitmap).",
           "#pragma once", "#include <stdint.h>", "",
           "struct OGlyph { uint32_t cp; uint16_t off; uint8_t w, h; int8_t x, y; uint8_t adv; };",
           "struct OFont { const OGlyph *g; uint16_t n; const uint8_t *bits; int8_t mid; };", ""]
    for n, ch in icons.items():
        u = ch.encode("utf-8")
        out.append(f'#define IC_{n.upper()} "' + "".join(f"\\x{b:02X}" for b in u) + f'"  // f7 {n}')
    out.append("")
    out.extend(b + "\n" for b in blocks)
    out.append("// Demo thermal thumbnail (the mockup's frame), 44 x 33")
    out.append("#define THERMAL_W 44\n#define THERMAL_H 33")
    out.append("static const uint8_t THERMAL_DEMO[] = {")
    for i in range(0, len(th), 24):
        out.append("    " + ", ".join(f"0x{v:02X}" for v in th[i:i + 24]) + ",")
    out.append("};")
    dst = os.path.join(SRC, "oled_assets.h")
    open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    print(dst, f"{len(text)} text chars, {os.path.getsize(dst) // 1024} KB")


if __name__ == "__main__":
    main()
