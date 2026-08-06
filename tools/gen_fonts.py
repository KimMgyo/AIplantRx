"""Regenerate the UI subset fonts, with the charset derived from the source tree.

    py tools/gen_fonts.py            # regenerate all, report flash delta
    py tools/gen_fonts.py --check    # fail if any font is out of date, write nothing

WHY THIS EXISTS

Every subset font carries `.fallback = &font_kr_full_12`, and that fallback is one
fixed size: 12px, from malgunbd. A glyph the subset lacks is therefore drawn at
12px no matter what size the label asked for, so font_bold_19 rendered 19px text
with 12px holes in it and font_bold_10 rendered 10px text with 12px holes - the
same syllable visibly larger than the word around it. On a panel of fixed Korean
UI text that is not a rare edge: the subsets held 72-83 syllables while the
firmware's own string literals used 213, so 136-147 syllables per font came out
the wrong size. font_reg_12 escaped the size jump and not the weight one, because
it is generated from malgun (regular) and falls back to a bold face.

The subsets were also hand-maintained and had drifted apart - font_bold_14's
--symbols list had gained 센서/노드/마지막/수신/텔레메트리 that font_bold_12's had
not, for no reason anybody could state - and four of them (reg_10, bold_10,
bold_13, bold_19) had `.fallback` patched in BY HAND after generation, so their
recorded Opts line said `--lv-fallback` was never passed. Regenerating one of
those from its own header would silently drop the fallback and take LVGL from
"draws that syllable too big" to "stops the string at that syllable and logs a
miss every repaint", which is the failure test_font_coverage.py was written for.

So the charset is not a list anybody edits. It is read out of the string literals
of the two programs that put Korean on this panel, every font gets the same one,
and the fallback is declared here where it is visible.

WHAT IS IN THE CHARSET

  ASCII 0x20-0x7E    every font already had it
  punctuation        the set font_kr_full_12 carries, so ℃ / → / × / · / ± / …
                     stop being 12px holes in a 14px string for the same reason
                     the Hangul did
  Hangul             every U+AC00-D7A3 syllable appearing in a string literal
                     anywhere in src/ or include/ (fonts excluded), and in
                     server/app/*.py

The server half is not optional. Half the Korean on the panel is minted server
side, and only half of THAT is model prose: render.py and schema.py name the
metrics, the units, the actuator, and the shape of every judgment line, and those
are fixed strings the panel draws at 13px and 14px. Deriving from the firmware
alone left 값 계 관 구 높 으 즉 직 짐 충 측 량 키 out, so a headline reading
"관수 값이 높으므로" drew four syllables of it a size smaller than the rest -
the same bug, arriving over the wire instead of out of flash.

Serial-log strings and model-prompt text are swept up with the drawn ones.
Telling them apart needs dataflow this does not have, and a glyph nobody draws
costs ~60 bytes of flash, which is the wrong side of the trade to spend care on.
What still belongs to the fallback is only what neither side can name in
advance: a scanned SSID, a species name, a sentence the model wrote.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(ROOT, "src", "fonts")
TTF_DIR = os.environ.get("WINDIR", "C:/Windows") + "/Fonts"

# name, ttf, px, fallback. Only fonts something actually draws with.
#
# font_bold_14 is the exception in the fallback column, and the reason the 14px
# fallback exists at all: it is the one size that draws text neither program can
# name - a prescription headline and a species name - so a syllable outside the
# shared charset would arrive 12px inside a 14px line. font_bold_13 and
# font_bold_19 draw only nameable text, and font_reg_12 / font_bold_12 are
# already the fallback's own size, so the 12px fallback costs them nothing.
FONTS = [
    ("font_bold_10", "malgunbd.ttf", 10, "font_kr_full_12"),
    ("font_reg_12",  "malgun.ttf",   12, "font_kr_full_12"),
    ("font_bold_12", "malgunbd.ttf", 12, "font_kr_full_12"),
    ("font_bold_13", "malgunbd.ttf", 13, "font_kr_full_12"),
    ("font_bold_14", "malgunbd.ttf", 14, "font_kr_full_14"),
    ("font_bold_19", "malgunbd.ttf", 19, "font_kr_full_12"),
]

# The whole-block fonts the subsets fall back to: every U+AC00-D7A3 syllable, no
# ASCII (that always resolves in the subset that asked), no fallback of their own.
# Generated here rather than kept as hand-made artifacts, because a fallback whose
# punctuation drifts from PUNCTUATION is the same bug one level down.
FALLBACKS = [
    ("font_kr_full_12", "malgunbd.ttf", 12),
    ("font_kr_full_14", "malgunbd.ttf", 14),
]

# Everything non-ASCII the UI draws outside the Hangul block: degree and its
# neighbours, the dashes and quotes the server's prose uses, bullet, ellipsis,
# arrow, and ℃. Every subset AND every fallback gets all of it, so a unit or a
# quotation mark cannot be the one glyph that changes size in a line.
PUNCTUATION = (0xB0, 0xB1, 0xB7, 0xD7, 0x2013, 0x2014, 0x2018, 0x2019,
               0x201C, 0x201D, 0x2022, 0x2026, 0x2192, 0x2103)

_LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
# Triple quotes first, or the opening pair of a docstring matches as an empty
# string and its Korean body is read as code.
_PY_LITERAL = re.compile(
    r'"""(?:.|\n)*?"""' r"|'''(?:.|\n)*?'''"
    r'|"(?:[^"\\\n]|\\.)*"' r"|'(?:[^'\\\n]|\\.)*'"
)


def punctuation():
    """PUNCTUATION, as the callers want it: sorted code points."""
    return sorted(PUNCTUATION)


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def hangul():
    """Every Hangul syllable either program can name in advance, in code order.

    Both literal grammars, because both halves reach the same screen: C string
    literals under src/ and include/, and Python ones - including the triple
    quoted prompt blocks - under server/app/.
    """
    found = set()
    for src, pat in ((os.path.join(ROOT, "src"), _LITERAL),
                     (os.path.join(ROOT, "include"), _LITERAL),
                     (os.path.join(ROOT, "server", "app"), _PY_LITERAL)):
        for dirpath, dirnames, filenames in os.walk(src):
            dirnames[:] = [d for d in dirnames if d not in ("fonts", "__pycache__")]
            for fn in filenames:
                if not fn.endswith((".c", ".cpp", ".h", ".hpp", ".py")):
                    continue
                for m in pat.finditer(_read(os.path.join(dirpath, fn))):
                    found |= {c for c in m.group(0) if 0xAC00 <= ord(c) <= 0xD7A3}
    return "".join(sorted(found))


def argv_for(name, ttf, px, out, syms=None, fallback=None):
    """lv_font_conv's argv for one font.

    `syms` given makes a subset: ASCII plus the punctuation plus those syllables.
    `syms` omitted makes a whole-block fallback: the Hangul block plus the same
    punctuation, and no ASCII - ASCII always resolves in the subset that asked, so
    putting it here would only be 95 glyphs nobody reaches.
    """
    args = ["--font", TTF_DIR + "/" + ttf]
    args += ["-r", "0x20-0x7E"] if syms is not None else ["-r", "0xAC00-0xD7A3"]
    for cp in punctuation():
        args += ["-r", "0x%X" % cp]
    if syms is not None:
        args += ["--symbols", syms]
    args += ["--size", str(px), "--bpp", "4", "--format", "lvgl",
             "--lv-font-name", name]
    if fallback:
        args += ["--lv-fallback", fallback]
    return args + ["-o", out]


def _generate(name, argv):
    path = os.path.join(FONT_DIR, name + ".c")
    before = os.path.getsize(path) if os.path.exists(path) else 0
    r = subprocess.run(["npx", "--no-install", "lv_font_conv"] + argv,
                       cwd=ROOT, shell=(os.name == "nt"),
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("%s failed:\n%s\n%s" % (name, r.stdout, r.stderr))
    return before, os.path.getsize(path)


def _stale(name, syms, fallback):
    """Why `name` is out of date, or None. Read from the generator's own Opts line."""
    src = _read(os.path.join(FONT_DIR, name + ".c"))
    opts = re.search(r"^ \* Opts: (.*)$", src, re.M)
    if not opts:
        return "no Opts line: not generated by this tool"
    declared = set(re.findall(r"-r 0x([0-9A-Fa-f]+)(?![0-9A-Fa-f-])", opts.group(1)))
    lacks_punct = [cp for cp in PUNCTUATION if "%X" % cp not in {d.upper() for d in declared}]
    if lacks_punct:
        return "%d punctuation marks come from elsewhere: %s" % (
            len(lacks_punct), "".join(chr(cp) for cp in lacks_punct))
    have = re.search(r"--symbols (\S+)", opts.group(1))
    if syms is not None:
        missing = sorted(set(syms) - set(have.group(1) if have else ""))
        if missing:
            return "%d syllables it cannot draw at its own size: %s" % (
                len(missing), "".join(missing))
    actual = re.search(r"\.fallback = &(\w+)", src)
    actual = actual.group(1) if actual else None
    if actual != fallback:
        return "falls back to %s, not %s" % (actual, fallback)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="report fonts whose charset is stale; write nothing")
    args = ap.parse_args()

    syms = hangul()
    print("charset: %d ASCII + %d punctuation + %d Hangul syllables; "
          "fallbacks carry the whole block at %s"
          % (0x7E - 0x20 + 1, len(PUNCTUATION), len(syms),
             ", ".join("%dpx" % px for _n, _t, px in FALLBACKS)))

    work = [(n, t, px, syms, fb) for n, t, px, fb in FONTS] + \
           [(n, t, px, None, None) for n, t, px in FALLBACKS]

    if args.check:
        stale = [(n, why) for n, _t, _px, s, fb in work
                 for why in [_stale(n, s, fb)] if why]
        for name, why in stale:
            print("STALE %s: %s" % (name, why))
        if stale:
            sys.exit(1)
        print("OK: %d fonts, every one covering the sources' Hangul at its own size"
              % len(work))
        return 0

    for name, ttf, px, s, fb in work:
        out = os.path.join(FONT_DIR, name + ".c")
        before, after = _generate(name, argv_for(name, ttf, px, out, s, fb))
        print("  %-16s %2dpx  %8d -> %8d bytes of C  %s"
              % (name, px, before, after, "-> " + fb if fb else "whole block"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
