"""Every Korean string the panel draws must be in a font that can draw it.

LVGL stops rendering a string at the first glyph it cannot resolve and logs a miss.
Both halves of that hurt: the text is silently truncated - a label reading
"최고 24.1℃" draws as nothing at all - and the miss is logged again on every
repaint, which for a label sitting on a video panel is a blocking 115200 baud write
inside the render path about fifteen times a second.

Two bugs of exactly this shape shipped. The thermal panel's scene-peak readout was
created on font_bold_10 and written "최고 %.1f℃"; that font has `.fallback = NULL`
and a fixed Korean symbol list containing neither 최 nor 고 nor U+2103, so the one
number the legend exists to show was never on the screen. The caption beside it had
the same problem on a 10px regular subset since deleted for having no draw site
left, and it took three build-flash-measure rounds to find them because each miss
only reveals the FIRST unresolvable character.

So the rule is mechanical, and it is three rules:

  A. A label created on a fallback-less font must never be rewritten at runtime.
     Its text is fixed, so rule B can check it. A dynamic write cannot be checked
     from here, and font_bold_10 is exactly where that assumption failed.
  B. A fixed string on a fallback-less font must be inside that font's charset.
  C. A syllable either program can name in advance must be in the charset of every
     font that draws text - fallback or no fallback.

A and B are about a string reaching the screen at all. C is about it reaching the
screen at the size it asked for, and it is the rule this file used to get wrong:
`.fallback` is one fixed face - font_kr_full_12, 12px, bold - so a subset leaning
on it for ordinary UI Korean draws 12px holes in a 19px headline, and font_reg_12
draws bold ones in a regular line. Every subset was doing that for 136 to 147
syllables before tools/gen_fonts.py derived one charset for all of them, which is
why C is checked against that deriver rather than a list kept here: the test and
the generator disagreeing is the only way this can come back.

What the fallback is left holding is what neither program can name in advance: a
scanned SSID, a species name, a sentence the model wrote.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI = os.path.join(ROOT, "src", "ui")
FONTS = os.path.join(ROOT, "src", "fonts")

# Icon fonts. Their glyphs are private-use code points emitted by ICON_* macros, not
# text, and there is no Hangul fallback that would mean anything for them.
ICON_FONTS = {"font_icons", "font_icons_sm", "font_bot"}


def _font_charset(name):
    """The code points a font can draw on its own, from the generator's own Opts line.

    Read out of the .c file rather than listed here, so regenerating a font with a
    different --symbols list moves this test with it.
    """
    path = os.path.join(FONTS, name + ".c")
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    opts = re.search(r"^ \* Opts: (.*)$", src, re.M)
    assert opts, "%s has no Opts line to read its charset from" % name
    line = opts.group(1)

    points = set()
    for lo, hi in re.findall(r"-r 0x([0-9A-Fa-f]+)-0x([0-9A-Fa-f]+)", line):
        points |= set(range(int(lo, 16), int(hi, 16) + 1))
    for one in re.findall(r"-r 0x([0-9A-Fa-f]+)(?![0-9A-Fa-f-])", line):
        points.add(int(one, 16))
    syms = re.search(r"--symbols (\S+)", line)
    if syms:
        points |= {ord(c) for c in syms.group(1)}

    has_fallback = re.search(r"\.fallback = &(\w+)", src)
    return points, (has_fallback.group(1) if has_fallback else None)


def _label_sites():
    """Every `label(parent, "text", &font_x, ...)` in src/ui, with its assignment.

    Returns (file, assigned_name_or_None, text_or_None, font). `text` is None when the
    argument is not a string literal - a variable, which rule A then has to cover.
    """
    out = []
    pat = re.compile(
        r"(?:(\w+(?:\.\w+)?)\s*=\s*)?\blabel\(\s*[^,]+,\s*"
        r"(\"(?:[^\"\\]|\\.)*\"|[A-Za-z_]\w*(?:\.\w+)?)\s*,\s*&(font_\w+)")
    for fn in sorted(os.listdir(UI)):
        if not fn.endswith(".cpp"):
            continue
        with open(os.path.join(UI, fn), encoding="utf-8") as fh:
            src = fh.read()
        for m in pat.finditer(src):
            assigned, arg, font = m.group(1), m.group(2), m.group(3)
            text = arg[1:-1] if arg.startswith('"') else None
            out.append((fn, assigned, text, font))
    return out


def _rewritten_widgets():
    """Widget names that get their text set at runtime, anywhere in src/ui."""
    names = set()
    pat = re.compile(r"(?:ui_set_label_text|lv_label_set_text)\(\s*([A-Za-z_]\w*(?:[.>-]+\w+)?)")
    for fn in sorted(os.listdir(UI)):
        if not fn.endswith(".cpp"):
            continue
        with open(os.path.join(UI, fn), encoding="utf-8") as fh:
            for m in pat.finditer(fh.read()):
                raw = m.group(1)
                names.add(raw)
                names.add(re.split(r"[.>-]+", raw)[-1])
    return names


def test_no_label_draws_a_glyph_its_font_lacks():
    """Rule B: a fixed string on a fallback-less font is inside that font's charset."""
    sites = _label_sites()
    assert sites, "found no label() call sites - the parser is broken, not the UI"

    charsets = {}
    checked = 0
    broken = []
    for fn, _assigned, text, font in sites:
        if font in ICON_FONTS or text is None:
            continue
        if font not in charsets:
            charsets[font] = _font_charset(font)
        points, fallback = charsets[font]
        if fallback:
            continue
        checked += 1
        missing = sorted({c for c in text if ord(c) not in points})
        if missing:
            broken.append("%s: %r on %s cannot draw %s" % (
                fn, text, font, "".join(missing)))
    assert not broken, "labels that will render truncated:\n  " + "\n  ".join(broken)
    return "%d fixed strings on fallback-less fonts, all covered" % checked


def test_a_rewritten_label_has_a_hangul_fallback():
    """Rule A: what gets written at runtime cannot be checked, so it must fall back.

    This is the rule that was missing. The scene-peak label was created with "--",
    which passes rule B trivially, and then written Korean every refresh.
    """
    rewritten = _rewritten_widgets()
    assert rewritten, "found no runtime text writes - the parser is broken"

    charsets = {}
    offenders = []
    guarded = 0
    for fn, assigned, _text, font in _label_sites():
        if assigned is None or font in ICON_FONTS:
            continue
        short = re.split(r"[.>-]+", assigned)[-1]
        if assigned not in rewritten and short not in rewritten:
            continue
        if font not in charsets:
            charsets[font] = _font_charset(font)
        _points, fallback = charsets[font]
        if fallback:
            guarded += 1
            continue
        offenders.append("%s: %s is rewritten at runtime but sits on %s, which has "
                         "no fallback" % (fn, assigned, font))
    assert not offenders, "labels whose text cannot be verified:\n  " + "\n  ".join(offenders)
    return "%d rewritten labels, all on a fallback font" % guarded


def test_every_size_draws_its_own_hangul():
    """Rule C: nameable Korean comes from the font asked for, not from the fallback."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import gen_fonts

    need = set(gen_fonts.hangul()) | {chr(c) for c in gen_fonts.punctuation()}
    assert len(need) > 200, (
        "the charset deriver found %d glyphs, so it is not reading the sources it "
        "thinks it is - rule C would pass vacuously" % len(need))

    fonts = {font for _fn, _assigned, _text, font in _label_sites()} - ICON_FONTS
    assert fonts, "found no fonts at label sites - the parser is broken, not the UI"

    wrong_size = []
    for font in sorted(fonts):
        points, _fallback = _font_charset(font)
        missing = sorted(c for c in need if ord(c) not in points)
        if missing:
            wrong_size.append("%s: %d nameable glyphs would come from the 12px "
                              "fallback: %s" % (font, len(missing), "".join(missing)))
    assert not wrong_size, ("fonts that draw part of their own text at another "
                            "size:\n  " + "\n  ".join(wrong_size))
    return "%d drawing fonts, each covering all %d nameable glyphs" % (
        len(fonts), len(need))


if __name__ == "__main__":
    for fn in (test_no_label_draws_a_glyph_its_font_lacks,
               test_a_rewritten_label_has_a_hangul_fallback,
               test_every_size_draws_its_own_hangul):
        print("%-52s %s" % (fn.__name__, fn()))
    print("OK")
