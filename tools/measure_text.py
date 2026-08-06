"""Measure a UTF-8 string's rendered pixel width against an LVGL C font table.

Reimplements LVGL 8's width math exactly:
  lv_font_fmt_txt.c:173-179  adv_w = (glyph_dsc[gid].adv_w + 8) >> 4
  lv_txt.c:366-373           width = sum(adv) + letter_space*(n) - letter_space

Falls back through `.fallback` the way lv_font_get_glyph_dsc() does, so a
string mixing font_bold_12's subset with arbitrary Hangul measures the way it
actually draws on the panel.

    py tools/measure_text.py font_bold_12 "판단 없음" "7시간 전"
"""
import re
import sys
from pathlib import Path

FONT_DIR = Path(__file__).resolve().parent.parent / "src" / "fonts"

_ADV = re.compile(r"\.adv_w\s*=\s*(\d+)")
_CMAP = re.compile(
    r"\.range_start\s*=\s*(\d+)\s*,\s*\.range_length\s*=\s*(\d+)\s*,"
    r"\s*\.glyph_id_start\s*=\s*(\d+)\s*,\s*"
    r"\.unicode_list\s*=\s*(\w+|NULL)\s*,\s*\.glyph_id_ofs_list\s*=\s*(\w+|NULL)\s*,"
    r"\s*\.list_length\s*=\s*(\d+)\s*,\s*\.type\s*=\s*(\w+)"
)


def _block(src, decl):
    i = src.index(decl)
    i = src.index("{", i)
    depth, j = 0, i
    while True:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i : j + 1]
        j += 1


def _u16_list(src, name):
    body = _block(src, "uint16_t " + name + "[]")
    return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", body)]


class Font:
    def __init__(self, name):
        self.name = name
        src = (FONT_DIR / (name + ".c")).read_text(encoding="utf-8", errors="replace")
        gd = _block(src, "lv_font_fmt_txt_glyph_dsc_t glyph_dsc[]")
        self.adv = [int(m) for m in _ADV.findall(gd)]
        self.cmaps = []
        for m in _CMAP.finditer(_block(src, "lv_font_fmt_txt_cmap_t cmaps[]")):
            start, length, gid0, ulist, olist, llen, kind = m.groups()
            self.cmaps.append(
                {
                    "start": int(start),
                    "len": int(length),
                    "gid0": int(gid0),
                    "ulist": None if ulist == "NULL" else _u16_list(src, ulist),
                    "olist": None if olist == "NULL" else _u16_list(src, olist),
                    "type": kind,
                }
            )
        fb = re.search(r"\.fallback\s*=\s*&(\w+)", src)
        self.fallback_name = fb.group(1) if fb else None
        self._fallback = None

    @property
    def fallback(self):
        if self._fallback is None and self.fallback_name:
            self._fallback = Font(self.fallback_name)
        return self._fallback

    def _gid(self, cp):
        for c in self.cmaps:
            if not (c["start"] <= cp < c["start"] + c["len"]):
                continue
            rel = cp - c["start"]
            if c["type"].endswith("FORMAT0_TINY"):
                return c["gid0"] + rel
            if c["type"].endswith("FORMAT0_FULL"):
                return c["gid0"] + c["olist"][rel]
            # SPARSE_TINY / SPARSE_FULL: binary-searchable unicode_list
            try:
                idx = c["ulist"].index(rel)
            except ValueError:
                continue
            if c["type"].endswith("SPARSE_TINY"):
                return c["gid0"] + idx
            return c["gid0"] + c["olist"][idx]
        return None

    def adv_px(self, cp):
        """Advance in whole px, or None when neither this font nor a fallback has it."""
        gid = self._gid(cp)
        if gid is not None and gid < len(self.adv):
            return (self.adv[gid] + 8) >> 4, self.name
        if self.fallback:
            return self.fallback.adv_px(cp)
        return None

    def width(self, text, letter_space=0):
        total, n, missing = 0, 0, []
        for ch in text:
            got = self.adv_px(ord(ch))
            if got is None:
                missing.append(ch)
                continue
            w, _ = got
            if w > 0:
                total += w + letter_space
                n += 1
        if n:
            total -= letter_space
        return total, missing

    def origins(self, text):
        out = []
        for ch in text:
            got = self.adv_px(ord(ch))
            out.append((ch, None, "MISSING") if got is None else (ch, got[0], got[1]))
        return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    font = Font(sys.argv[1])
    for text in sys.argv[2:]:
        w, missing = font.width(text)
        nbytes = len(text.encode("utf-8"))
        print(f'"{text}"  {nbytes} B  {w} px')
        for ch, adv, src in font.origins(text):
            print(f"      U+{ord(ch):04X} {ch!r:6} {str(adv):>4} px  {src}")
        if missing:
            print("      MISSING GLYPHS:", missing)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
