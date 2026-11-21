"""Generate the S3 firmware's fonts, icon table and glass background.

    python tools/gen_assets.py          # regenerate what changed
    python tools/gen_assets.py --force  # regenerate everything

Runs before every build (scripts/assets_hook.py) and only redoes what changed, so adding a Korean
string or an F7_ icon to the source is all it takes: the next build carries the glyph.

Outputs (all generated, do not edit):
  src/fonts/*.c         Pretendard text fonts and Framework7 icon fonts (lv_font_conv)
  include/fonts.h       their declarations
  include/f7.h          F7_<NAME> UTF-8 strings for every icon the source uses
  src/assets/glass.h    the glass background at 1/8 size; the S3 scales it up at boot (bg.cpp)

Text fonts come in two kinds:
  dyn    ASCII, punctuation, the 2,350 KS X 1001 syllables and every syllable in the source. For text
         the firmware cannot know in advance: plant names, AI sentences, SSIDs.
  fixed  ASCII, punctuation and the syllables in the source only.
Every text font falls back to the icon font of its size, so a label can mix F7_ icons and text.
"""
import hashlib
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # firmware/s3
REPO = os.path.dirname(os.path.dirname(ROOT))      # aiplantrx
SRC = os.path.join(ROOT, "src")
FONT_DIR = os.path.join(SRC, "fonts")
STAMP = os.path.join(FONT_DIR, "stamp.json")
UI_FONTS = os.path.join(REPO, "ui", "fonts")
FONT_DIRS = [UI_FONTS, os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Windows\Fonts"), r"C:\Windows\Fonts"]
F7_TTF = os.path.join(UI_FONTS, "Framework7Icons-PUA.ttf")
F7_MAP = os.path.join(UI_FONTS, "framework7-icons-pua.json")
GLASS_PNG = os.path.join(REPO, "ui", "mockups", "html", "assets", "bg_main_glass.png")

PUNCT = [0xB0, 0xB7, 0xD7, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2192, 0x2212, 0x2103]
ICON_SIZES = [12, 13, 14, 15, 16, 19, 40]
# name, weight file, px, kind, compressed. Nothing is compressed any more: LVGL unpacks a compressed glyph
# on every draw, which made text the slowest part of a scrolling settings page. Flash has room.
TEXT = [
    ("f_r11", "Regular", 11, "fixed", False),
    ("f_r12", "Regular", 12, "dyn", False),
    ("f_r13", "Regular", 13, "dyn", False),
    ("f_r14", "Regular", 14, "dyn", False),
    ("f_r15", "Regular", 15, "dyn", False),
    ("f_m15", "Medium", 15, "dyn", False),
    ("f_m19", "Medium", 19, "ascii", False),
    ("f_s12", "SemiBold", 12, "fixed", False),
    ("f_s13", "SemiBold", 13, "fixed", False),
    ("f_s14", "SemiBold", 14, "fixed", False),
    ("f_s16", "SemiBold", 16, "fixed", False),
    ("f_b11", "Bold", 11, "fixed", False),
    ("f_b13", "Bold", 13, "fixed", False),
    ("f_b15", "Bold", 15, "dyn", False),
    ("f_b18", "Bold", 18, "fixed", False),
    ("f_b32", "Bold", 32, "dyn", False),
]

_LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')
_F7 = re.compile(r"\bF7_([A-Z0-9_]+)\b")


def find_font(name):
    for d in FONT_DIRS:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    sys.exit("font not found: " + name)


def lv_font_conv():
    p = os.environ.get("LV_FONT_CONV")
    if p and os.path.exists(p):
        return [ "node", p ]
    cache = os.path.expandvars(r"%LOCALAPPDATA%\npm-cache\_npx")
    if os.path.isdir(cache):
        for d in sorted(os.listdir(cache)):
            js = os.path.join(cache, d, "node_modules", "lv_font_conv", "lv_font_conv.js")
            if os.path.exists(js):
                return ["node", js]
    sys.exit("lv_font_conv not found: run 'npx -y lv_font_conv --help' once, or set LV_FONT_CONV")


def scan():
    hangul, other, icons = set(), set(), set()
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames[:] = [d for d in dirnames if d not in ("fonts",)]
        for fn in filenames:
            if not fn.endswith((".c", ".cpp", ".h")):
                continue
            text = io.open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read()
            for m in _LIT.finditer(text):
                for c in m.group(0):
                    o = ord(c)
                    if 0xAC00 <= o <= 0xD7A3:
                        hangul.add(c)
                    elif o > 0x7E and o not in PUNCT and not (0xE000 <= o <= 0xF8FF):
                        other.add(c)
            icons |= set(_F7.findall(text))
    for dirpath, _d, filenames in os.walk(os.path.join(ROOT, "include")):
        for fn in filenames:
            if fn in ("f7.h", "fonts.h"):
                continue
            text = io.open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read()
            icons |= set(_F7.findall(text))
            for m in _LIT.finditer(text):
                hangul |= {c for c in m.group(0) if 0xAC00 <= ord(c) <= 0xD7A3}
    return hangul, other, icons


def ks2350():
    out = []
    for hi in range(0xB0, 0xC9):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("euc_kr"))
            except UnicodeDecodeError:
                pass
    return set(out)


def run(conv, args):
    r = subprocess.run(conv + args, capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        sys.exit("lv_font_conv failed:\n" + r.stdout + r.stderr)


def main():
    force = "--force" in sys.argv
    os.makedirs(FONT_DIR, exist_ok=True)
    stamp = json.load(open(STAMP)) if os.path.exists(STAMP) and not force else {}
    hangul, other, icon_names = scan()
    f7map = json.load(io.open(F7_MAP, encoding="utf-8"))
    names = {n.lower() for n in icon_names}
    missing = sorted(n for n in names if n not in f7map)
    if missing:
        sys.exit("unknown Framework7 icons: " + ", ".join(missing))
    cps = sorted(int(str(f7map[n]).replace("U+", "").replace("0x", ""), 16) if isinstance(f7map[n], str) else int(f7map[n]) for n in names)
    dyn = sorted(ks2350() | hangul)
    fixed = sorted(hangul)
    conv = None
    done = []

    def build(name, args):
        nonlocal conv
        key = hashlib.sha1(json.dumps(args, ensure_ascii=False).encode("utf-8")).hexdigest()
        out = os.path.join(FONT_DIR, name + ".c")
        if stamp.get(name) == key and os.path.exists(out):
            return
        conv = conv or lv_font_conv()
        run(conv, args + ["-o", out])
        stamp[name] = key
        done.append(name)

    for px in ICON_SIZES:
        args = ["--font", F7_TTF, "--size", str(px), "--bpp", "4", "--format", "lvgl", "--no-compress",
                "--lv-font-name", "i%d" % px]
        for cp in cps:
            args += ["-r", "0x%X" % cp]
        build("i%d" % px, args)

    for name, weight, px, kind, comp in TEXT:
        args = ["--font", find_font("Pretendard-%s.otf" % weight), "-r", "0x20-0x7E"]
        for cp in PUNCT:
            args += ["-r", "0x%X" % cp]
        sym = {"dyn": dyn, "fixed": fixed, "ascii": []}[kind] + sorted(other)
        if sym:
            args += ["--symbols", "".join(sym)]
        args += ["--size", str(px), "--bpp", "4", "--format", "lvgl", "--lv-font-name", name]
        if not comp:
            args.append("--no-compress")
        fb = min(ICON_SIZES, key=lambda s: abs(s - px))
        args += ["--lv-fallback", "i%d" % fb]
        build(name, args)

    # Declarations.
    decl = ["// Generated by tools/gen_assets.py. Do not edit.", "#pragma once", "#include <lvgl.h>", ""]
    for px in ICON_SIZES:
        decl.append("LV_FONT_DECLARE(i%d)" % px)
    for name, *_ in TEXT:
        decl.append("LV_FONT_DECLARE(%s)" % name)
    io.open(os.path.join(ROOT, "include", "fonts.h"), "w", encoding="utf-8", newline="\n").write("\n".join(decl) + "\n")

    # Icon strings.
    f7 = ["// Generated by tools/gen_assets.py from the F7_ names used in the source. Do not edit.", "#pragma once", ""]
    for n in sorted(names):
        cp = int(str(f7map[n]).replace("U+", "").replace("0x", ""), 16) if isinstance(f7map[n], str) else int(f7map[n])
        utf = chr(cp).encode("utf-8")
        f7.append('#define F7_%s "%s"' % (n.upper(), "".join("\\x%02X" % b for b in utf)))
    io.open(os.path.join(ROOT, "include", "f7.h"), "w", encoding="utf-8", newline="\n").write("\n".join(f7) + "\n")

    # Glass background at 1/8 size (100x60 RGB888). Blurred and smooth, so bilinear scaling at boot
    # gives back the same image for 18 KB of flash instead of 768 KB.
    out = os.path.join(SRC, "assets", "glass.h")
    gkey = hashlib.sha1(open(GLASS_PNG, "rb").read()).hexdigest() if os.path.exists(GLASS_PNG) else None
    if gkey and (stamp.get("glass") != gkey or not os.path.exists(out)):
        from PIL import Image
        im = Image.open(GLASS_PNG).convert("RGB").resize((100, 60), Image.BOX)
        data = im.tobytes()
        lines = ["// Generated by tools/gen_assets.py from ui/mockups/html/assets/bg_main_glass.png. Do not edit.",
                 "#pragma once", "#include <stdint.h>", "#define GLASS_W 100", "#define GLASS_H 60",
                 "static const uint8_t GLASS_RGB[GLASS_W * GLASS_H * 3] = {"]
        for i in range(0, len(data), 30):
            lines.append("  " + ",".join(str(b) for b in data[i:i + 30]) + ",")
        lines.append("};")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        io.open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
        stamp["glass"] = gkey
        done.append("glass")

    json.dump(stamp, open(STAMP, "w"), indent=1, sort_keys=True)
    print("[assets] %d Hangul in source, %d icons; regenerated: %s"
          % (len(hangul), len(names), ", ".join(done) if done else "nothing"))


if __name__ == "__main__":
    main()

