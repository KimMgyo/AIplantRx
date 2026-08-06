"""Machine values -> the finished Korean the device draws, and the single place
that guarantees every outgoing character can be drawn at all.

The firmware renders model-authored text with font_reg_12 and font_bold_14
(src/ui/page_auto.cpp). Both are SUBSET builds - ASCII plus a hand-listed
handful of Hangul - with font_kr_full_12 as the LVGL fallback for everything
else. A character outside that union does not raise, warn, or box-draw a
question mark: it renders as a blank rectangle. That already cost three UI rows
once, to a single middle dot. So the allowed set is derived from the fallback
font's own generator options rather than trusted to memory, and every string
this module emits goes through guard().

The second invariant is size. The device copies the judgment log into fixed C
buffers (include/aijudge.h) and the 예약 / 조치 rows into another set
(include/plantrx.h), so every field has a byte budget from schema.py rather than
a character count, and _bclip() is the only thing here allowed to cut one.

Nothing here does I/O except reading the font sources once at import.
"""

import bisect
import logging
import math
import os
import re
import unicodedata
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Optional, get_args

from .schema import (
    ACTION_ROWS_MAX,
    JUDGE_AT_BYTES,
    JUDGE_BODY_BYTES,
    JUDGE_CHIP_BYTES,
    JUDGE_CHIPS_MAX,
    JUDGE_HEAD_BYTES,
    JUDGE_ROWS_MAX,
    NOTICE_BYTES,
    PLAN_ROWS_MAX,
    ROW_AT_BYTES,
    ROW_COND_BYTES,
    ROW_DELTA_BYTES,
    ROW_HEAD_BYTES,
    ROW_TAG_BYTES,
    ROW_WBAND_BYTES,
    ROW_WLABEL_BYTES,
    ROW_WSTAT_BYTES,
    SPECIES_CONF_BYTES,
    SPECIES_SCI_BYTES,
    WINDOW_ROWS_MAX,
    ActionRow,
    Chip,
    Control,
    Display,
    JudgeRow,
    Level,
    PlanRow,
    Prescription,
    Species,
    Tone,
    Turn,
    WindowRow,
)

log = logging.getLogger("plantrx.render")

# --------------------------------------------------------------------------
# The renderable character set
# --------------------------------------------------------------------------

# Only fonts whose Opts line carries `--lv-fallback font_kr_full_12` can reach
# the fallback. font_reg_12 and font_bold_14 - the two used for model text - do,
# and both also carry `-r 0x20-0x7E` of their own, which is where ASCII comes
# from. So the allowed set is ASCII plus whatever the fallback was built with.
_FALLBACK_FONT = "font_kr_full_12.c"
_ASCII = (0x20, 0x7E)

# Snapshot of font_kr_full_12.c's -r list, used when the font sources are not on
# disk - the Dockerfile copies server/ only, so that is the normal case in
# production. Deliberately not narrower than the real thing: stripping the en
# dash and the arrow out of every prescription because a file is missing would
# be a worse failure than the one this guards against. Regenerate the fonts and
# this list has to move with them; _load() logs when the two disagree.
_BUILTIN_RANGES = (
    (0xAC00, 0xD7A3),  # Hangul syllables
    (0x00B0, 0x00B1),  # ° ±
    (0x00B7, 0x00B7),  # · - the one that shipped the blank-row bug
    (0x00D7, 0x00D7),  # ×
    (0x2013, 0x2014),  # – —
    (0x2018, 0x2019),  # ‘ ’
    (0x201C, 0x201D),  # “ ”
    (0x2022, 0x2022),  # •
    (0x2026, 0x2026),  # …
    (0x2103, 0x2103),  # ℃
    (0x2192, 0x2192),  # →
)

_OPTS_RANGE = re.compile(r"-r\s+0x([0-9A-Fa-f]+)(?:\s*-\s*0x([0-9A-Fa-f]+))?")


def _font_dirs() -> list[Path]:
    """Where the LVGL font sources might be, most specific first."""
    here = Path(__file__).resolve()
    out = []
    env = os.getenv("PLANTRX_FONT_DIR", "").strip()
    if env:
        out.append(Path(env))
    out.append(here.parents[2] / "src" / "fonts")  # server/app/ -> repo root
    out.append(Path.cwd() / "src" / "fonts")
    return out


def _parse_ranges(text: str) -> list[tuple[int, int]]:
    """Pull the -r ranges out of the generator options comment.

    lv_font_conv records its whole command line on line 4 of the file it emits,
    which makes the .c the authoritative record of its own coverage - there is
    no separate manifest to fall out of sync.
    """
    opts = next((ln for ln in text.splitlines()[:12] if "Opts:" in ln), "")
    out = []
    for lo, hi in _OPTS_RANGE.findall(opts):
        a = int(lo, 16)
        out.append((a, int(hi, 16) if hi else a))
    return out


def _merge(ranges: list[tuple[int, int]]) -> tuple[tuple[int, int], ...]:
    out: list[tuple[int, int]] = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return tuple(out)


def _load() -> tuple[tuple[tuple[int, int], ...], str]:
    for d in _font_dirs():
        f = d / _FALLBACK_FONT
        try:
            # 8 KB reaches well past the options comment without pulling in a
            # 400 KB glyph table we have no use for.
            with open(f, "r", encoding="utf-8", errors="replace") as fh:
                head = fh.read(8192)
        except OSError:
            continue
        parsed = _parse_ranges(head)
        if not parsed:
            log.warning("render: %s has no -r ranges on its Opts line", f)
            continue
        merged = _merge([_ASCII, *parsed])
        if merged != _merge([_ASCII, *_BUILTIN_RANGES]):
            log.warning("render: %s no longer matches _BUILTIN_RANGES - update it", f)
        return merged, str(f)

    log.warning(
        "render: %s not found (looked in %s); using the built-in snapshot",
        _FALLBACK_FONT,
        ", ".join(str(d) for d in _font_dirs()),
    )
    return _merge([_ASCII, *_BUILTIN_RANGES]), "built-in snapshot"


_RANGES, _SOURCE = _load()
# Parallel arrays so membership is a bisect rather than an 11k-entry set: the
# Hangul block alone is 11,172 code points and every outgoing string is scanned.
_LOS = tuple(r[0] for r in _RANGES)
_HIS = tuple(r[1] for r in _RANGES)


def _overlap(r: tuple[int, int], other: tuple[int, int]) -> int:
    return max(0, min(r[1], other[1]) - max(r[0], other[0]) + 1)


_HANGUL = (0xAC00, 0xD7A3)
_EXTRA = sum(
    (hi - lo + 1) - _overlap((lo, hi), _ASCII) - _overlap((lo, hi), _HANGUL)
    for lo, hi in _RANGES
)
ALLOWED_DESC = f"ASCII 20-7E + Hangul AC00-D7A3 + {_EXTRA} punctuation ({_SOURCE})"


def _char_ok(c: int) -> bool:
    i = bisect.bisect_right(_LOS, c) - 1
    return i >= 0 and c <= _HIS[i]


def is_renderable(s: str) -> bool:
    """True when every character in s has a glyph in the device's fonts."""
    return all(_char_ok(ord(ch)) for ch in s)


# --------------------------------------------------------------------------
# guard: degrade one label, never fail a poll
# --------------------------------------------------------------------------

# Substitutes for characters a model reaches for that the fonts do not have.
# Anything absent here is dropped: a wrong glyph is worse than a missing one,
# because the device has no way to tell the user it guessed.
_SUBS: dict[str, str] = {
    # Whitespace variants. Newline and tab are not glyphs, but a stray one in a
    # chip or a target row breaks the flex layout, so they flatten to a space.
    "\n": " ", "\r": " ", "\t": " ",
    "\u00a0": " ", "\u2007": " ", "\u202f": " ", "\u3000": " ", "\u200b": "",
    # Tilde variants - the fullwidth and wave forms are common in Korean text
    # for ranges, and ASCII '~' is renderable, but '-' reads better at 12 px.
    "\uff5e": "-", "\u301c": "-", "\u223c": "-", "\u2053": "-",
    # Dashes and minus signs that are not the two en/em dashes we do have.
    "\u2010": "-", "\u2011": "-", "\u2012": "-", "\u2015": "-",
    "\u2212": "-", "\ufe58": "-", "\ufe63": "-",
    # Punctuation with an obvious ASCII or in-set equivalent.
    "\u30fb": "\u00b7", "\uff65": "\u00b7",  # katakana middle dots -> ·
    "\u00ab": '"', "\u00bb": '"', "\u2032": "'", "\u2033": '"',
    "\u2044": "/", "\u00f7": "/", "\u2264": "<=", "\u2265": ">=", "\u2260": "!=",
    "\u2190": "<-", "\u21d2": "=>", "\u2794": "\u2192", "\u27a1": "\u2192",
    "\u00b2": "2", "\u00b3": "3", "\u00b5": "u", "\u03bc": "u",
    "\u2109": "\u00b0F",
}

# A substitute the fonts cannot draw either is not a substitute. Checked once
# here so guard() never has to re-verify per call.
_SUBS = {k: v for k, v in _SUBS.items() if is_renderable(v)}

_GAP = re.compile(r"  +")

_warned: set[tuple[str, int]] = set()
# The text being guarded is model output, so this set is grown from outside the
# process. Capping it turns an unbounded leak into bounded silence.
_WARN_CAP = 512


def _note(field: str, ch: str, rep: str) -> None:
    key = (field, ord(ch))
    if key in _warned or len(_warned) >= _WARN_CAP:
        return
    _warned.add(key)
    log.warning("render: U+%04X %r unrenderable in %s -> %r", ord(ch), ch, field, rep)


def guard(s: str, *, field: str) -> str:
    """Return s if the device can draw it, otherwise the closest thing it can.

    Never raises. A stray character should cost one label, not the poll that
    carries it - the device would keep running the previous prescription, and
    the reason would be invisible on both ends.
    """
    if not isinstance(s, str):
        s = "" if s is None else str(s)
    if is_renderable(s):
        return s

    # Korean that has been through a JSON round trip is sometimes NFD, and
    # decomposed jamo live in U+1100, outside 0xAC00-0xD7A3 - every syllable
    # would box. Composing fixes the whole string in one pass.
    s = unicodedata.normalize("NFC", s)
    if is_renderable(s):
        return s

    out = []
    for ch in s:
        c = ord(ch)
        if _char_ok(c):
            out.append(ch)
            continue
        rep = _SUBS.get(ch)
        if rep is None and 0xFF01 <= c <= 0xFF5E:
            rep = chr(c - 0xFEE0)  # fullwidth block maps 1:1 onto 0x21-0x7E
            if not is_renderable(rep):
                rep = None
        _note(field, ch, rep or "")
        if rep:
            out.append(rep)
    # Dropping a character leaves the space that was around it, and two spaces
    # in a row are a visible hole at 12 px. Only on this path: a string that
    # passed above is returned exactly as the model wrote it.
    return _GAP.sub(" ", "".join(out)).strip()


# --------------------------------------------------------------------------
# Formatting primitives
# --------------------------------------------------------------------------

# Korea has been a fixed UTC+9 with no DST since 1988, so a fixed offset is
# exactly correct and does not put a tzdata dependency in the container.
_KST = timezone(timedelta(hours=9))

# The card column is 376 px, and this is the width past which the trail's first
# line stops being glanceable. It is the last character limit in the file: every
# other field the device draws lands in a fixed C buffer, so its limit is a byte
# budget owned by schema.py.
_WHAT_MAX = 40


def _hhmm(ts: Optional[int]) -> str:
    """HH:MM in Asia/Seoul. The device clock reads 0 before NTP and the server's
    receipt time is authoritative, so a zero here means "no time", not 1970."""
    if not ts or ts <= 0:
        return "--:--"
    try:
        return datetime.fromtimestamp(int(ts), _KST).strftime("%H:%M")
    except (OverflowError, OSError, ValueError):
        return "--:--"


def _fmt(v: float, dp: int) -> str:
    t = f"{v:.{dp}f}"
    if "." in t:
        t = t.rstrip("0").rstrip(".")
    return "0" if t in ("-0", "", "-") else t


def _dur(seconds: int) -> str:
    s = max(0, int(seconds))
    if s < 60:
        return f"{s}초"
    if s < 3600:
        return f"{round(s / 60)}분"
    h, rem = divmod(s, 3600)
    m = round(rem / 60)
    return f"{h}시간 {m}분" if m else f"{h}시간"


def _num(v: Any) -> Optional[float]:
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    return float(v)


def _clip(s: str, limit: int) -> str:
    """Trim to limit, preferring a word or clause boundary."""
    if len(s) <= limit:
        return s
    cut = s[: limit - 1]
    at = max(cut.rfind(" "), cut.rfind("."), cut.rfind(","))
    if at >= limit // 2:
        cut = cut[:at]
    return cut.rstrip(" .,") + "…"


def _bcut(s: str, budget: int) -> str:
    """The longest prefix of s that fits in `budget` UTF-8 bytes."""
    b = s.encode()
    if len(b) <= budget:
        return s
    # b[budget] is the first byte that does not fit, and a UTF-8 continuation
    # byte is 0b10xxxxxx, which can never begin a code point: while the cut
    # lands on one it is inside a sequence, so backing up finds the boundary.
    # Testing the first EXCLUDED byte rather than the last kept one is what
    # makes a cut that already sits on a boundary a no-op instead of eating the
    # sequence in front of it.
    n = budget
    while n > 0 and b[n] & 0xC0 == 0x80:
        n -= 1
    return b[:n].decode()


# U+2026 costs three bytes OF the budget it marks, not three past it. Appending
# it after the fit check is exactly the overflow the check existed to prevent:
# the firmware strncpy()s into `char head[64]` and would end the buffer inside
# the ellipsis.
_ELLIPSIS = "…"
_ELLIPSIS_B = len(_ELLIPSIS.encode())


def _bclip(s: str, budget: int) -> str:
    """Trim to `budget` UTF-8 BYTES, never leaving a partial sequence behind.

    _clip counts characters, which is right for a flex label and wrong for a
    fixed buffer: 40 Hangul syllables are 40 characters and 120 bytes, so a
    character clip hands the firmware a string its strncpy ends mid-sequence,
    and the panel draws one broken glyph where a word should be. Invisible in a
    diff, obvious on the wall.
    """
    if budget <= 0:
        return ""
    if len(s.encode()) <= budget:
        return s
    room = budget - _ELLIPSIS_B
    if room <= 0:
        # No room for the mark, so the bare cut: at that size the ellipsis would
        # be most of the field.
        return _bcut(s, budget)
    return _bcut(s, room).rstrip(" .,") + _ELLIPSIS


def _fit(s: str, budget: int, *, field: str) -> str:
    """guard() first, then the byte budget, measured on what actually goes out.

    The order matters for decomposed Korean: guard() composes NFD jamo, which
    turns six bytes per syllable into three, so a clip taken beforehand would
    throw away half a head to fit a budget the composed string never exceeded.
    """
    return _bclip(guard(s, field=field), budget)


# A bare "." is not a sentence end: "2.3 °C" would split mid-number, which is
# how the 마지막 실행 row first came out reading "엽온이 기온보다 2".
_SENTENCE = re.compile(r"(?<=[.!?])\s+")


def _conclusion(s: str, limit: int) -> str:
    """The clause worth putting next to a timestamp.

    The last sentence, clipped from the front, for the same reason: Korean is
    predicate-final, so a diagnosis opens with the readings ("엽온이 기온보다
    2.3 °C 높고...") and closes with what they mean ("...수분 스트레스로
    보입니다"). The readings are already on the card as chips. Clipping the head
    off this the way _clip() does would keep only the part that is redundant.
    """
    parts = [p.strip() for p in _SENTENCE.split(s) if p.strip()]
    if not parts:
        return ""
    tail = parts[-1].rstrip(".")
    if len(tail) <= limit:
        return tail
    cut = tail[-limit:]
    at = cut.find(" ")
    # Start on a word boundary when one is close enough that dropping to it
    # costs a word rather than a clause; otherwise admit the cut with an
    # ellipsis rather than opening mid-syllable.
    return cut[at + 1:] if 0 <= at < limit // 3 else "…" + cut[1:]


# _conclusion counts characters where the buffer counts bytes, so the fallback
# head is asked for a third of the budget - a Hangul syllable is three bytes and
# this fallback is Korean prose. Letting it run to the byte figure and leaning on
# _bclip to catch the overflow would defeat the point of _conclusion: it clips
# from the FRONT to keep the predicate, and _bclip cuts the tail, so the backstop
# would remove the exact clause _conclusion went to the trouble of keeping.
_HEAD_CHARS = JUDGE_HEAD_BYTES // 3

# 3+ newlines collapse to 2. A blank line between paragraphs is worth its height
# on the card; a second blank line is not, and a model that indents its answer as
# a list can produce four in a row. Applied after the per-line guard below, whose
# strip() is what turns a line of spaces into an empty one - guard() itself leaves
# it alone, because a run of spaces is renderable and every other field on this
# wire wants it kept.
_RUNS = re.compile(r"\n{3,}")


def _body(diagnosis: str, notes: str) -> str:
    """The model's whole answer, as the card draws it.

    NOT _conclusion. That keeps the last sentence and throws the rest away, which
    is right for a 63-byte head beside a timestamp and was, until this field
    existed, the only thing the model's prose survived as: one clause of it clipped
    to _HEAD_CHARS, with notes_ko never leaving the server at all. The card has a
    column's width and the model has already been capped at 120 + 200 characters by
    brain.clamp_output, so nothing here needs to choose which sentence matters.

    Guarded per line rather than whole, because guard() has no notion of a line:
    is_renderable('\\n') is false, so a single newline anywhere would send the
    entire body down the substitution path and _SUBS flattens '\\n' to a space -
    which is correct for every other field on this wire and would silently undo the
    one thing this field is for. Splitting first means each line is guarded as the
    self-contained string it is, and the breaks are re-inserted afterwards where
    only this function can put them.
    """
    parts = [p for p in (str(diagnosis or "").strip(), str(notes or "").strip()) if p]
    if not parts:
        return ""
    # CR is a line break the model sometimes writes and _SUBS would turn into a
    # space, so it is normalised into the break that survives instead of being
    # dropped to one.
    raw = "\n".join(parts).replace("\r\n", "\n").replace("\r", "\n")
    # Stripped per line as well: a leading space indents the card's prose against
    # nothing, and a line of them between paragraphs draws as a blank line the
    # collapse above cannot see.
    guarded = (guard(line, field="judge.body").strip() for line in raw.split("\n"))
    joined = "\n".join(guarded)
    return _bclip(_RUNS.sub("\n\n", joined).strip("\n"), JUDGE_BODY_BYTES)


# The verdict words, straight off the contract so a fourth one cannot be added
# to schema.py without arriving here too.
_LEVELS = get_args(Level)


# --------------------------------------------------------------------------
# What each metric is called and how precisely it is worth showing
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class _M:
    label: str
    unit: str
    dp: int


# The font_bold_14 spelling of each unit: ° + 'C' rather than ℃ (U+2103). ℃ is
# in the allowed set, but only via the fallback, which is 12 px bold, so inside a
# 14 px bold row it renders a size and a weight off. U+00B0 is in every
# model-text font's own -r list, and it is what src/ui/page_auto.cpp draws in
# prose. Chips are 12 px regular and take the opposite decision - see _CHIP_FMT.
# A metric therefore has two unit spellings, and which one is correct depends on
# which font draws the string.
_METRICS: dict[str, _M] = {
    "vpd_kpa": _M("VPD", "kPa", 1),
    "air_c": _M("기온", "°C", 1),
    "rh_pct": _M("습도", "%RH", 0),
    "co2_ppm": _M("CO2", "ppm", 0),
    # NOT 엽온차. leaf_max_c is the hottest pixel of the whole 32x24 scene, so a
    # delta against air temperature is a delta against that peak and not against
    # a leaf; src/aijudge.cpp carries an all-caps rule about this. Calling it
    # 엽온 claims a leaf-temperature measurement the hardware cannot make, and
    # earning the word needs real leaf segmentation, not a relabel.
    "leaf_air_dt_c": _M("장면최고차", "°C", 1),
    # No lux and no soil_pct entry, and not because this board's BH1750 is dead:
    # every key that reaches this table is either a schema.MetricKey (a
    # setpoint's, a wake condition's) or a key of derive._METRIC_COLUMN (the
    # window summary's), and neither vocabulary has ever contained them. A 조도
    # row had no caller that could ask for it, so the two entries were a label
    # and a unit for a row the renderer cannot draw.
}

# Chip text, formatted byte-identically in form to the rule producer's
# (add_chip in src/aijudge.cpp). A rule row and a model row sit in the same log,
# and two spellings of one reading read as two instruments. Fixed decimals for
# that reason too: snprintf("%.1f") does not strip a trailing zero, so neither
# does this, or one row says "VPD 2 kPa" and the next "VPD 2.0 kPa". And ℃
# (U+2103) rather than ° + 'C', because chips are drawn with font_reg_12, whose
# `.fallback = &font_kr_full_12` supplies ℃ at the chip's own size.
# The pair is (which key of `current` holds the number, how it is spelled).
_CHIP_FMT: dict[str, tuple[str, str]] = {
    "vpd_kpa": ("vpd_kpa", "VPD {:.1f} kPa"),
    "air_c": ("air_c", "기온 {:.1f} ℃"),
    "rh_pct": ("rh_pct", "습도 {:.0f} %RH"),
    "co2_ppm": ("co2_ppm", "CO2 {:.0f} ppm"),
    # The chip reports the scene peak itself, not the delta that was selected: a
    # number the thermal array actually produced, where the delta is one
    # derivation further from anything the sensor saw.
    "leaf_air_dt_c": ("leaf_max_c", "장면최고 {:.1f} ℃"),
}

# Setpoint order on the card, chip order within a tone group, and - because the
# window table has four cells and the vocabulary has five metrics - which metric
# gets dropped. Follows what the diagnosis is usually about rather than sensor
# order.
#
# leaf_air_dt_c is last on purpose, and it used to be second. It is the metric this
# codebase trusts least and says so everywhere: its input is the hottest pixel of
# the whole scene, which a grow lamp or a power supply dominates, so its findings
# stay hedged (src/aijudge.cpp's all-caps block), its chip is named 장면최고 and
# never 엽온, and since the panel's own band lost its escalation edge it cannot
# raise an ALERT from the device side at all. rh_pct is the opposite: directly
# measured, half of VPD, and the thing a grower opens a vent or a mister about.
# With five reporting metrics the table was spending a slot on the hint and
# dropping the measurement - so the two swapped ends. Nothing disappears from the
# panel either way: all six readings keep their live tile on the strip above, and
# this only decides whose HISTORY is on screen.
#
# When the thermal link is down leaf_air_dt_c has no samples and is skipped
# regardless, so on those installs the table is unchanged by this.
_ORDER = ("vpd_kpa", "co2_ppm", "air_c", "rh_pct", "leaf_air_dt_c")

# The Setpoint key is air_c; the sensor field it comes from is temp_c. derive
# only adds vpd_kpa and leaf_air_dt_c, so `current` carries the sensor spelling.
_CURRENT_KEYS: dict[str, tuple[str, ...]] = {"air_c": ("air_c", "temp_c")}

_ACTUATOR_KO = {
    "fan": "팬",
    "heater": "히터",
    "mist": "미스트",
    "led": "식물등",
    "pumpA": "펌프 A",
    "pumpB": "펌프 B",
    "pumpC": "펌프 C",
}


def _act_ko(name: str) -> str:
    return _ACTUATOR_KO.get(name, name)


def _sched_ko(name: str) -> str:
    # A pump on a timer is irrigation, and that is what the row is about - there
    # is no soil probe to close the loop, which is why it is a schedule at all.
    return "관수" if name.startswith("pump") else _act_ko(name)


def _current(current: dict, key: str) -> Optional[float]:
    for k in _CURRENT_KEYS.get(key, (key,)):
        v = _num(current.get(k))
        if v is not None:
            return v
    return None


def _band(lo: Optional[float], hi: Optional[float], m: _M) -> str:
    """"0.8 – 1.2 kPa", or the one-sided forms."""
    if lo is not None and hi is not None:
        return f"{_fmt(lo, m.dp)} – {_fmt(hi, m.dp)} {m.unit}"
    if hi is not None:
        return f"{_fmt(hi, m.dp)} {m.unit} 이하"
    if lo is not None:
        return f"{_fmt(lo, m.dp)} {m.unit} 이상"
    return "-"


def _dist(v: float, lo: Optional[float], hi: Optional[float]) -> float:
    """How far outside the band, 0 when inside."""
    if lo is not None and v < lo:
        return lo - v
    if hi is not None and v > hi:
        return v - hi
    return 0.0


def _get(obj: Any, key: str, default: Any = None) -> Any:
    """Read a field off a dict or a model. wake conditions arrive as plain dicts
    from scheduler.clamp_wake today and as schema.WakeCondition if that changes;
    neither is worth coupling this module to."""
    if isinstance(obj, dict):
        return obj.get(key, default)
    return getattr(obj, key, default)


# --------------------------------------------------------------------------
# Display pieces
# --------------------------------------------------------------------------


def _species(raw: Optional[dict]) -> Optional[Species]:
    """Three shapes arrive here.

    * main._species_entry's normalised dict (text/sci/conf_pct/conf_text/source),
      which is what the live path sends whether the device, PlantNet or the
      previous prescription named the plant.
    * a fresh plantnet.SpeciesResult dump (sci/common/korean/score), which
      server/tools/fake_model_server.py still hands over directly.
    * a schema.Species dump (text/sci/conf_text) carried forward from a stored
      prescription.

    The confidence is taken from the most finished form present - conf_text,
    then conf_pct, then a raw 0..1 score - and never recomputed once one of them
    is there: the device is already showing a percentage of its own on the
    panel, and a server that re-derives the figure puts a second number against
    one measurement.
    """
    if not isinstance(raw, dict) or not raw:
        return None

    text = raw.get("text") or raw.get("korean") or raw.get("common") or raw.get("sci") or ""
    if not text:
        return None

    conf = raw.get("conf_text") or ""
    if not conf:
        pct = raw.get("conf_pct")
        if pct is None:
            score = _num(raw.get("score"))
            pct = int(score * 100 + 0.5) if score is not None else None
        if pct is not None:
            conf = f"{int(pct)}%"

    # _fit and not guard for the two the device now draws into fixed buffers:
    # src/plantrx.cpp truncates what it is sent rather than rejecting it, so a
    # string past the budget loses its tail on the wall silently. text keeps
    # guard() alone - RX_SPECIES_CAP is 128, sized off plantid.cpp's own buffer,
    # and the contract test checks that one separately.
    return Species(
        text=guard(str(text), field="species.text"),
        sci=_fit(str(raw.get("sci") or ""), SPECIES_SCI_BYTES, field="species.sci"),
        conf_text=_fit(str(conf), SPECIES_CONF_BYTES, field="species.conf_text"),
    )


def _tone(value: float, lo: Optional[float], hi: Optional[float], banded: bool) -> Tone:
    if not banded:
        return "info"
    return "warn" if _dist(value, lo, hi) > 0 else "ok"


# --------------------------------------------------------------------------
# 판단: past tense
# --------------------------------------------------------------------------


def _chips(
    evidence: list[str],
    deciding: str,
    current: dict,
    bands: dict[str, tuple],
) -> list[Chip]:
    """The selected readings, in the order they were selected.

    Selected, not dumped. The row that says "CO2 부족" gains nothing from a
    humidity reading, and a chip per sensor is what makes a log unscannable. The
    numbers are the server's own: the model chooses which metrics bear on its
    finding and never retypes their values, because a wrong number beside a
    right verdict is the one failure a grower cannot catch from the panel.
    """
    out: list[Chip] = []
    for key in evidence:
        spec = _CHIP_FMT.get(key)
        if spec is None:
            continue
        src, fmt = spec
        v = _current(current, src)
        # No chip at all rather than a chip reading 0, which is what add_chip()'s
        # present() gate does: an absent chip reads as "not part of this
        # finding", where a zero reads as a broken sensor.
        if v is None:
            continue
        # The tint is a band test, so it is measured on the metric's own reading
        # - for leaf_air_dt_c that is the delta, while the text above it prints
        # the scene peak the delta was taken from.
        own = _current(current, key)
        lo, hi = bands.get(key, (None, None))
        out.append(
            Chip(
                text=_fit(fmt.format(v), JUDGE_CHIP_BYTES, field="chip"),
                tone=_tone(own, lo, hi, key in bands) if own is not None else "info",
                # Only the deciding reading is hot, and only when there was one:
                # a finding that nothing fired on leads with its headline number
                # rather than accusing it. See EV_NORMAL in src/aijudge.cpp.
                hot=bool(deciding) and key == deciding,
            )
        )
    return out[:JUDGE_CHIPS_MAX]


def _judgments(
    *,
    head: str,
    diagnosis: str,
    notes: str,
    level: str,
    evidence: list[str],
    deciding: str,
    current: dict,
    bands: dict[str, tuple],
    evidence_ts: int,
    has_rgb: bool,
    has_thermal: bool,
) -> list[JudgeRow]:
    """The newest judgment, and only it. A list of one, because Display.judgments
    is a list and the empty case is real.

    No history, and there used to be six rows of it carried forward out of `prev`
    the way _actions still does. The 판단 column stopped being a log: it draws this
    one judgment as a card - the prose in full, the readings under it, the two
    frames beside them - and there is nowhere on it for a second row to go. Five
    rows of finished strings nobody reads is 2 KB of a response the firmware reads
    into a fixed buffer, so `prev` is not a parameter here any more; the trail a
    grower wants over time is the 조치 column, which is built from measurements
    rather than from re-issued prose.

    No origin field on the wire, and there used to be one. It was a
    Literal["rule", "llm"] whose only producer was this function writing "llm",
    which it could not have written otherwise: build_display is behind the model
    gate, and the one path that answers without a model ships zero judgments on
    purpose (build_empty_display - a measurement is not a judgment). So it was a
    field with one reachable value and no reader on the device, costing 17 bytes on
    every row of every poll while implying the server distinguishes authors it
    cannot. The firmware authors its own rule rows and labels them itself, and
    Display.model_ready is what tells the panel whether a model exists at all.
    """
    text = str(head or "").strip()
    if not text:
        # diagnosis_ko still earns its place: the model does not always fill
        # head_ko, and the last sentence of the diagnosis is the clause worth
        # putting next to a timestamp. Guarded before _conclusion because
        # dropping a character moves where that last sentence begins.
        text = _conclusion(guard(str(diagnosis or ""), field="diagnosis"), _HEAD_CHARS)

    row = JudgeRow(
        at=_fit(_hhmm(evidence_ts), JUDGE_AT_BYTES, field="judge.at"),
        # A level from outside the contract would raise out of pydantic and fail
        # the poll carrying it, which is the trade guard() refuses to make for a
        # character and will not make for a badge either.
        level=level if level in _LEVELS else "ok",
        head=_fit(text, JUDGE_HEAD_BYTES, field="judge.head"),
        # The same answer at full length, and not _fit: _fit is guard-then-clip on
        # one line, and this field is the one place a line break means something.
        body=_body(diagnosis, notes),
        chips=_chips(evidence, deciding, current, bands),
        has_rgb=has_rgb,
        has_thermal=has_thermal,
    )
    # One row, sliced against the constant anyway: JUDGE_ROWS_MAX is what the
    # response contract and its test are written against, and a producer that
    # merely happens to agree with a budget is how the two drift apart.
    return [row][:JUDGE_ROWS_MAX]


# --------------------------------------------------------------------------
# 예약: future tense
# --------------------------------------------------------------------------


def _condition(w: Any) -> str:
    """"VPD 1.8 이상 10분 지속" for one wake_when entry, "" when unusable."""
    key = str(_get(w, "metric", ""))
    m = _METRICS.get(key)
    v = _num(_get(w, "value"))
    if m is None or v is None:
        return ""
    # schema.WakeOp is "gt"/"lt"; ">"/"<" is accepted because that is what the
    # two producers spelled it before the contract said otherwise.
    op = str(_get(w, "op", ""))
    side = "이상" if op in ("gt", ">") else "이하"
    return f"{m.label} {_fmt(v, m.dp)} {side} {_dur(_get(w, 'for_s', 300))} 지속"


def _plan(control: Control, wake: Any, issued_ts: int) -> list[PlanRow]:
    """What will happen, and what makes it happen.

    Bands are not events and are not here. "VPD 0.8 – 1.2 kPa" is unjudgeable
    without the reading beside it, and the device reads the bands it holds out of
    Control.setpoints, which is where its own loop wants them anyway.
    """
    rows: list[PlanRow] = []

    # A once action inside its expiry is the only thing on the card that is a
    # command rather than a band, so it leads and it is the one row tinted warn.
    # An expired one is not a plan: the device has already ignored it.
    for a in control.once:
        if a.before_ts and a.before_ts <= issued_ts:
            continue
        rows.append(
            PlanRow(
                at=_fit(_hhmm(issued_ts), ROW_AT_BYTES, field="plan.at"),
                tag=_fit("즉시", ROW_TAG_BYTES, field="plan.tag"),
                tone="warn",
                head=_fit(f"{_act_ko(a.actuator)} {_dur(a.seconds)} 가동",
                          ROW_HEAD_BYTES, field="plan.head"),
            )
        )

    # No `at`: the period is the device's own timer, so the server cannot say
    # which tick comes next. The recurrence goes in the condition column and the
    # clock stays empty rather than naming a minute this row cannot promise.
    for sc in control.schedules:
        rows.append(
            PlanRow(
                tag=_fit("대기", ROW_TAG_BYTES, field="plan.tag"),
                head=_fit(f"{_sched_ko(sc.actuator)} {_dur(sc.duration_s)} 가동",
                          ROW_HEAD_BYTES, field="plan.head"),
                cond=_fit(f"{_dur(sc.every_s)} 주기", ROW_COND_BYTES, field="plan.cond"),
            )
        )

    # A threshold has no clock time, and inventing one claims a certainty the
    # condition does not have - so the head is the condition and `at` is empty.
    for w in list(_get(wake, "when") or []):
        text = _condition(w)
        if not text:
            continue
        rows.append(
            PlanRow(
                tag=_fit("조건", ROW_TAG_BYTES, field="plan.tag"),
                tone="info",
                head=_fit(text, ROW_HEAD_BYTES, field="plan.head"),
                cond=_fit("조건 충족 시 재진단", ROW_COND_BYTES, field="plan.cond"),
            )
        )

    return rows[:PLAN_ROWS_MAX]


def _turn(wake: Any, issued_ts: int) -> Turn:
    after = int(_num(_get(wake, "after_s")) or 0)
    # A zero period is not a schedule, and neither is a missing wake spec.
    if after <= 0:
        return Turn()
    return Turn(scheduled=True, next_ts=issued_ts + after, period_s=after)


# --------------------------------------------------------------------------
# 조치: executed tense
# --------------------------------------------------------------------------

# Choosing to leave the greenhouse alone is a decision, so it is a row - but it
# is not a completed one, and it is compared by name in two places.
_NO_ACTION = "조치 없음"


def _what(control: Control, prev: Optional[Prescription], first: bool) -> str:
    """What this prescription actually changed, in the terse form the trail uses."""
    parts: list[str] = []
    for a in control.once:
        parts.append(f"{_act_ko(a.actuator)} {_dur(a.seconds)} 가동")

    prev_ctl = prev.control if prev is not None else None
    prev_bands = {sp.key: (sp.lo, sp.hi) for sp in prev_ctl.setpoints} if prev_ctl else {}
    for sp in control.setpoints:
        m = _METRICS.get(sp.key)
        if m is None or prev_bands.get(sp.key) == (sp.lo, sp.hi):
            continue
        # New band only. An old -> new arrow between two two-number bands runs
        # past the column; the previous band is one row down in the trail.
        parts.append(f"{m.label} 목표 {_band(sp.lo, sp.hi, m)}")

    prev_sched = {s.actuator: s.every_s for s in prev_ctl.schedules} if prev_ctl else {}
    for sc in control.schedules:
        was = prev_sched.get(sc.actuator)
        if was == sc.every_s:
            continue
        label = _sched_ko(sc.actuator)
        if was is None:
            parts.append(f"{label} {_dur(sc.every_s)} 주기")
        else:
            parts.append(f"{label} {_dur(was)} → {_dur(sc.every_s)}")

    prev_policy = prev_ctl.policy if prev_ctl else {}
    for name, mode in control.policy.items():
        if prev_policy.get(name) == mode:
            continue
        parts.append(f"{_act_ko(name)} {'정지' if mode == 'off' else '자동'}")

    if not parts:
        # Not a blank: an empty row reads as a bug.
        return "초기 목표 설정" if first and control.setpoints else _NO_ACTION
    return _clip(", ".join(parts[:2]), _WHAT_MAX)


# The 조치 column's second line when the row asked for an action, and none of
# these three is a measurement - which is the point. There is no relay, so the
# server cannot observe that anything ran, and the string this replaces
# ("실행 기록 없음", returned for every once action ever prescribed because
# window["actuators"] is empty by construction) implied a log had been consulted.
# What IS observable is the grower's own switch for that actuator across the
# window, so that is what these say, in those terms.
#
# window["actuators"] is deliberately still not read here: it is measured
# hardware and it is empty. The day a relay lands, a measurement outranks a
# switch position and earns wording of its own rather than inheriting this.
#
# Widest form is 41 bytes against ROW_DELTA_BYTES=48 at the longest label the
# panel owns (미스트 / 식물등). A name from outside the panel's inventory can be
# longer and falls to _fit, like every other row on the card.
_HELD_OFF = "%s 스위치 계속 꺼짐"          # reported, and off at every poll
_HELD_ON_FLAT = "%s 스위치 켜짐, 변화 없음"  # reported on, and the metric held still
_NOT_REPORTED = "%s 스위치 상태 미보고"     # a firmware that sends no intent at all


def _outcome(prev: Optional[Prescription], window: Optional[dict]) -> tuple[str, bool, bool]:
    """What the previous prescription moved, measured over the window since it
    was issued. That window is exactly the span its action had to work in, which
    is why the delta lands on the row above rather than the one being written.

    The third value says whether the first is a measurement. The device draws a
    reading as a chip and a reason as prose, and the string does not say which.

    A prescription that asked for a once action is answered from the switch
    window first, because a metric drift on a row whose action could not have run
    would be handed to the grower as though the prescription caused it.
    """
    if prev is None or not isinstance(window, dict):
        return "", False, False

    # None until an action was asked for AND the panel reported the switch that
    # action names. That is what stops the metric branch below from crediting a
    # drift to an action nothing can be shown to have even been switched on for.
    held: Optional[float] = None
    name = ""
    asked = [a.actuator for a in prev.control.once]
    if asked:
        intent = window.get("intent") or {}
        seen = [
            (n, _num((intent.get(n) or {}).get("held_s")) or 0.0)
            for n in asked
            if isinstance(intent.get(n), dict)
        ]
        if not seen:
            return _NOT_REPORTED % _act_ko(asked[0]), False, False
        # The switch held on longest, so the row names the one that could have
        # done something rather than whichever the model happened to list first.
        name, held = max(seen, key=lambda pair: pair[1])
        if held <= 0.0:
            return _HELD_OFF % _act_ko(name), False, False

    metrics = window.get("metrics") or {}
    keys = [sp.key for sp in prev.control.setpoints] or ["vpd_kpa"]
    for key in sorted(keys, key=lambda k: _ORDER.index(k) if k in _ORDER else 99):
        m = _METRICS.get(key)
        row = metrics.get(key)
        if m is None or not isinstance(row, dict):
            continue
        first, last = _num(row.get("first")), _num(row.get("last"))
        if first is None or last is None:
            continue
        before, after = _fmt(first, m.dp), _fmt(last, m.dp)
        # Compared as drawn, not as stored: two readings a rounding apart are the
        # same number to the grower, and "VPD 1.3 → 1.3 kPa" reads as a bug where
        # the switch line reads as the finding it is.
        if held is not None and before == after:
            return _HELD_ON_FLAT % _act_ko(name), False, False
        lo, hi = _num(row.get("lo")), _num(row.get("hi"))
        banded = lo is not None or hi is not None
        d0, d1 = _dist(first, lo, hi), _dist(last, lo, hi)
        improved = banded and (d1 == 0.0 or d1 < d0)
        return f"{m.label} {before} → {after} {m.unit}", improved, True

    return "", False, False


# --------------------------------------------------------------------------
# 구간: measured tense
# --------------------------------------------------------------------------

# 유지 rather than 달성: the band was being held, not scored. The percentage is
# time in band over time sampled, which is derive's arithmetic and not this
# module's - see window_summary's in_band_pct, whose denominator excludes the
# stretches where the sensor said nothing rather than counting them as failures.
_BAND_HELD = "유지 %d%%"


def _fin(v: Any) -> Optional[float]:
    """_num, less the values that are numbers but not quantities.

    derive._finite already keeps nan and inf out of a summary it built, so this
    is about the ones it did not: a hand-assembled window, a replayed fixture, a
    future caller. int(round(nan)) raises ValueError and int(round(inf)) raises
    OverflowError, and either would be thrown from inside a poll and lose the
    whole prescription over one bad float in one cell of a four-row table.
    """
    f = _num(v)
    return f if f is not None and math.isfinite(f) else None


def _secs(v: Any) -> int:
    """A window-level span as whole seconds. Anything that is not a finite,
    non-negative number is 0, which is the block's own word for "not measured"."""
    f = _fin(v)
    return max(0, int(round(f))) if f is not None else 0


def window_block(window: Optional[dict]) -> tuple[list[WindowRow], int, int]:
    """The monitor page's table: min / mean / max per metric over the window
    measured for this reply, plus how much of that window each metric held its
    band, plus the two window-level spans.

    Public because it has three callers now, not one. build_display uses it for a
    fresh judgment; main._restate remeasures it for the whole tenure of a carried
    prescription, so a card headed 최근 구간 stops replaying an hour that ended six
    hours ago; and build_empty_display uses it for a server with no model at all,
    which measures the same hour and previously threw every row of it away.

    "each metric held its band" is conditional on there being one. With no
    prescription in force there is no band, and the row then carries an empty
    band string and in_band_pct None - not 0%, and pointedly not 100%.

    derive.window_summary computes twelve quantities per metric and five window
    ones. Five of them reached the panel before this; the rest went to the model
    and stopped there, which meant the grower could be told "VPD 2.1 -> 1.3 kPa"
    with no way to see that the window it was measured over was sampled for four
    minutes of its hour. That is what the two spans are for.

    A metric with no samples in the window is skipped rather than drawn empty:
    there are five metric keys and four row slots, and a row reading "-- / -- /
    --" would spend one of them saying nothing while a metric with real numbers
    fell off the end. Ordering is _ORDER, unconditionally, so the rows keep their
    places between polls - a table that reshuffles when a sensor drops out is
    read as a fault in the panel.
    """
    if not isinstance(window, dict):
        return [], 0, 0

    metrics = window.get("metrics")
    rows: list[WindowRow] = []
    if isinstance(metrics, dict):
        for key in _ORDER:
            if len(rows) >= WINDOW_ROWS_MAX:
                break
            m = _METRICS.get(key)
            stat = metrics.get(key)
            if m is None or not isinstance(stat, dict):
                continue
            mn, mean, mx = (_fin(stat.get(k)) for k in ("min", "mean", "max"))
            if mn is None or mean is None or mx is None:
                continue
            # Through _fmt like every other number the wall draws, so one reading
            # cannot appear as 1.3 in the trail and 1.30 in the table.
            text = "%s / %s / %s %s" % (
                _fmt(mn, m.dp), _fmt(mean, m.dp), _fmt(mx, m.dp), m.unit
            )
            # derive reports a tenth of a percent; the row has room for neither
            # the digit nor the distinction. None stays None - it means the
            # metric had no band on either side, which is not 0%.
            pct = _fin(stat.get("in_band_pct"))
            held = None if pct is None else max(0, min(100, int(round(pct))))
            rows.append(WindowRow(
                label=_fit(m.label, ROW_WLABEL_BYTES, field="window.label"),
                stat=_fit(text, ROW_WSTAT_BYTES, field="window.stat"),
                band="" if held is None else _fit(
                    _BAND_HELD % held, ROW_WBAND_BYTES, field="window.band"),
                in_band_pct=held,
            ))

    span = _secs(window.get("span_s"))
    covered = _secs(window.get("covered_s"))
    # A zero span is not a window, so it gets no rows. One telemetry row summarises
    # to "24 / 24 / 24 °C" over nothing, which is the tile above it restated as a
    # range it does not have - and the panel discards it anyway
    # (src/ui/page_monitor.cpp gates the table on span <= 0). Dropping it here is
    # what stops the wire carrying four rows of a summary over zero seconds on
    # every poll of a freshly flashed board, and it puts the server and the panel
    # on one rule instead of two that happen to agree.
    if span <= 0:
        return [], 0, 0
    # derive already guarantees this - every hold is clamped to the gap it spans
    # - but the panel divides one by the other to draw a coverage bar, and a
    # hand-built window that got it wrong would draw past 100% rather than fail.
    return rows, span, min(covered, span)


def _actions(
    control: Control,
    prev: Optional[Prescription],
    window: Optional[dict],
    issued_ts: int,
) -> list[ActionRow]:
    """The trail. Only `prev` is in hand, so it is carried forward one run at a
    time: this run's decision goes on top, and the row it pushes down gets the
    delta the window just measured for it."""
    carried = [r.model_copy(deep=True) for r in prev.display.actions] if prev else []
    if carried and not carried[0].delta:
        delta, improved, measured = _outcome(prev, window)
        if delta:
            carried[0].delta = _fit(delta, ROW_DELTA_BYTES, field="action.delta")
            carried[0].delta_is_reading = measured
            carried[0].improved = improved

    what = _what(control, prev, first=prev is None)
    idle = what == _NO_ACTION
    row = ActionRow(
        at=_fit(_hhmm(issued_ts), ROW_AT_BYTES, field="action.at"),
        # 보류 rather than 완료: a green tick on a row where nothing ran is a
        # claim, and the row exists precisely to say that nothing did.
        tag=_fit("보류" if idle else "완료", ROW_TAG_BYTES, field="action.tag"),
        tone="info" if idle else "ok",
        head=_fit(what, ROW_HEAD_BYTES, field="action.head"),
        delta="",  # nothing has been measured under this prescription yet
        delta_is_reading=False,
        improved=False,
    )
    return [row, *carried][:ACTION_ROWS_MAX]


# --------------------------------------------------------------------------
# Assembly
# --------------------------------------------------------------------------

# The response budget, in bytes on the wire - starlette dumps a JSONResponse with
# ensure_ascii=False and separators=(",", ":"), so that is how these are counted.
# The other half of this arithmetic is MAX_RESP in src/plantrx.cpp: 16384 bytes,
# read into one PSRAM buffer. Overrunning it is not a clipped field, it is a
# prescription the panel discards whole, so the sum is kept here rather than
# remembered. Every figure below was measured on a real Prescription with every
# field at its schema.py budget - each line counted with its own key name and
# comma - not estimated:
#
#     judgments  1 x (at 7 + head 63 + body 1023
#                     + 5 chips x 27)                     1520
#     plan       4 x (at 7 + tag 12 + head 63 + cond 78)   853
#     actions    4 x (at 7 + tag 12 + head 63 + delta 48)  892
#     window     4 x (label 16 + stat 24 + band 20 + pct)  455
#     species (text 126 + sci 31 + conf 4)                 207
#     notice 200 / turn / the two spans              212+63+45
#     model_ready                                           19
#     "display": and the two braces                         12
#     ------------------------------------------------- -------
#     display                                            4278
#     control    5 setpoints, 4 schedules, 4 once,
#                7 policy keys                             889
#     envelope   rx_id, issued_ts, next_poll_s,
#                want_frame, update_mode, firmware_pull,
#                mode, outer braces                        144
#     ================================================= =======
#     total                                               5311   (MAX_RESP 16384)
#
# THE BODY IS FREE, and that is the whole reason it could be 1023 bytes. It costs
# 1026 bytes on the wire at its budget, and it arrived in the same change that cut
# judgments from six rows to one: the shape this replaced measured 5857 bytes with
# six rows of three chips and no body, so the response got 546 bytes SMALLER while
# gaining the model's entire answer. Five carried rows of finished strings the card
# had no room to draw cost more than one paragraph it does.
#
# The escaped figure is the one that decides MAX_RESP, because a hop that
# re-encodes the body with \uXXXX spends six bytes on a Hangul syllable where
# UTF-8 spends three: 8215 bytes, against 8407 for the six-row shape. Both fit
# 12288, so raising MAX_RESP to 16384 buys headroom rather than fixing an overrun -
# it is what stops the next field from having to re-derive all of this.
#
# The window block is counted whole above - the rows array, its "window" key, and
# the two spans - at the schema ceiling. The largest the renderer can actually
# reach is smaller, because a label is not free text: it comes from _METRICS and is
# chosen by _ORDER, so only four label strings can ever appear and they are 30
# bytes between them. An empty block is 51 bytes, which every device with no
# history pays on every poll.
#
# tests/_measure_budget.py is the script every figure above came out of - it holds
# each field at its schema.py budget and asserts the column against the object it
# breaks down, so a field added to Display cannot quietly land in the brace line.
# tests/test_window_block.py measures the window block the same way rather than
# asserting a constant, and tests/test_display_contract.py checks every field above
# against its budget on the response itself.


def build_display(
    *,
    control: Control,
    head: str,
    diagnosis: str,
    # The model's second paragraph, and drawn: it lands in JudgeRow.body under the
    # diagnosis. It used to stop at the server - store.save_prescription kept it in
    # raw_model, which nothing on the panel reads - so the grower saw a verdict
    # whose caveats ("장면최고는 램프 영향 가능성이 있어 참고만 했습니다") existed
    # only in the database. Defaulted because a model with nothing to add writes ""
    # and that is a real answer, unlike has_rgb below, which cannot be guessed.
    notes: str = "",
    level: str,
    evidence: list[str],
    deciding: str,
    species: Optional[dict],
    current: dict,
    evidence_ts: int,
    wake: Any,
    issued_ts: int,
    prev: Optional[Prescription] = None,
    window: Optional[dict] = None,
    # Required, and inferred from nothing: these say the model was handed the
    # image, and only the caller that fetched it can know. has_thermal used to
    # fall back to "leaf_max_c is not None" - a sensor scalar, not a held image -
    # so the card claimed thermal evidence across a fleet that had never uploaded
    # a thermal frame in its life. No default, so the next caller cannot forget
    # one the way main.py forgot this one.
    has_rgb: bool,
    has_thermal: bool,
) -> Display:
    """The whole display half of a prescription, as finished strings, laid out by
    tense: judgments are past, plan is future, actions are executed, and the
    window block is measured - the one part that goes to the monitor page.

    `current` is sensors + derive.enrich; `window` is derive.window_summary;
    `wake` is a scheduler.WakeSpec. Everything written here has been through
    guard() and, where the device holds it in a C buffer, a byte budget. Text
    carried forward from the previous prescription goes through neither again -
    it was finished when it was written.
    """
    win_rows, win_span, win_covered = window_block(window)
    bands = {
        sp.key: (sp.lo, sp.hi)
        for sp in control.setpoints
        if sp.lo is not None or sp.hi is not None
    }

    return Display(
        species=_species(species),
        judgments=_judgments(
            head=head,
            diagnosis=diagnosis,
            notes=notes,
            level=level,
            evidence=evidence,
            deciding=deciding,
            current=current,
            bands=bands,
            evidence_ts=evidence_ts,
            has_rgb=has_rgb,
            has_thermal=has_thermal,
        ),
        turn=_turn(wake, issued_ts),
        plan=_plan(control, wake, issued_ts),
        actions=_actions(control, prev, window, issued_ts),
        window=win_rows,
        window_span_s=win_span,
        window_covered_s=win_covered,
        # Not brain.is_configured() read a second time, and stronger than it:
        # this function is only reached from main._run_brain after
        # brain.diagnose() returned an answer, so the model is not merely
        # configured, it has just spoken. render.py stays free of brain.py.
        model_ready=True,
    )


def build_empty_display(*, model_ready: bool, window: Optional[dict] = None) -> Display:
    """What a device with no judgment on it shows.

    A blank column reads as a broken screen, and the device cannot tell the
    difference between "no judgment yet" and "the card failed to draw", so the
    card says which one it is. The turn stays unscheduled for the same reason
    read the other way: there is no deadline to count down to, and the device
    draws that as 대기 where a 00:00 would be a lie.

    The judgment column is empty and the window block is not. Those are different
    facts and this builder used to conflate them: a keyless server sends this
    display and nothing else, forever, so the monitor page's table was blank on
    every install without an API key while the server sat on an hour of readings it
    had already summarised. A measurement needs no model. A verdict does, which is
    why `judgments` stays empty and `Control` stays empty at the call site - the
    server has nothing to say about what the numbers mean and will not pretend.

    Two notices, because two silences reach here and a grower can act on only one
    of them. A server with no key will not judge this plant tomorrow either, and
    saying 준비 중 to that is telling someone to wait for something that is not
    coming; a keyed server that has not answered yet genuinely is. Both fit
    NOTICE_BYTES (200) with room to spare - the longer is 87 bytes of the 200,
    Korean costing 3 a syllable.

    `model_ready` has no default on purpose: it is the fact that picks the notice,
    and the caller holds it already - main.telemetry() reads it once for
    scheduler.decide().
    """
    rows, span, covered = window_block(window)
    notice = ("이 서버에는 판단 모델이 없습니다. 측정만 기록합니다."
              if not model_ready else
              "아직 판단 이력이 없습니다. 측정값을 모으는 중입니다.")
    return Display(
        notice=_fit(notice, NOTICE_BYTES, field="notice"),
        model_ready=model_ready,
        window=rows,
        window_span_s=span,
        window_covered_s=covered,
    )
