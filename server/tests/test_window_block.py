"""The window block: derive.window_summary -> render.window_block -> the wire.

    cd server && python tests/test_window_block.py

Plain asserts and no test framework, like every other file in here.

The block is the first thing the panel draws that is neither a judgment nor a
row of the trail: it is the last window's readings as a table, on the monitor
page, next to the live sensors. Four things can go wrong with it and none of
them raises.

The strings land in RxWindowRow (include/plantrx.h), which truncates rather than
rejects, so an overlong stat is a half-syllable on the wall and not an error.
The row order is what the grower's eye learns, so a table that reshuffles when a
sensor drops out reads as a fault in the panel rather than in the greenhouse.
`band` and `in_band_pct` are the same fact twice - the device tints the row from
the number and the grower reads the string - so a metric with no band has to
come back as ("", None) and not as ("", 0), which would claim a band was held
0% of the time. And a device with no history at all has to get a well-formed
empty block, because the alternative is a second code path on the firmware for a
key that is missing rather than empty.

test_display_contract.py checks the same budgets on a real response. This file
checks them at the worst case, which no scripted response reaches.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from app import derive, render, schema  # noqa: E402
from app.schema import Setpoint  # noqa: E402


def _metric(lo=None, hi=None, mn=None, mean=None, mx=None, pct=None, **extra):
    """One entry of window_summary()["metrics"], spelled the way derive spells it.

    Written out by hand rather than run through derive for the cases that need a
    value derive would take a contrived row set to produce - a five-digit CO2, a
    perfect 100% hold. The end-to-end shape is covered by test_from_derive below,
    which runs the real function, so the hand-built dicts cannot drift into a
    shape render.window_block would never see in production.
    """
    row = {"n": 1, "first": mn, "last": mx, "min": mn, "mean": mean, "max": mx,
           "lo": lo, "hi": hi, "covered_s": 600.0, "in_band_pct": pct}
    row.update(extra)
    return row


def _window(metrics, span_s=3600, covered_s=3000):
    return {"start_ts": 1754300000, "end_ts": 1754300000 + span_s,
            "span_s": span_s, "n_rows": 60, "covered_s": covered_s,
            "metrics": metrics, "actuators": {}, "intent": {}}


# --------------------------------------------------------------------------


def test_budgets_at_the_worst_case():
    """Every string inside its declared budget, measured in UTF-8 bytes.

    The worst case is not the typical one: the widest label the table can draw
    is 장면최고차 (five syllables, fifteen bytes against a sixteen-byte budget),
    and the widest stat is a five-digit CO2 triple, which is 25 bytes and does
    NOT fit - so this also pins down that the overflow is clipped rather than
    shipped. A byte count is the only measure that matters here: the firmware
    strncpy()s into char stat[25] and fifteen bytes of Hangul is five characters.
    """
    rows, _span, _covered = render.window_block(_window({
        # 장면최고차, the longest label in render._METRICS.
        "leaf_air_dt_c": _metric(lo=0.0, hi=3.0, mn=-12.75, mean=-3.5, mx=18.25,
                                 pct=99.94),
        # 25 bytes as "10000 / 12500 / 15000 ppm": one past the budget.
        "co2_ppm": _metric(lo=600, mn=10000, mean=12500, mx=15000, pct=100.0),
    }))
    assert len(rows) == 2, rows

    widest = {}
    for row in rows:
        for field, budget in (("label", schema.ROW_WLABEL_BYTES),
                              ("stat", schema.ROW_WSTAT_BYTES),
                              ("band", schema.ROW_WBAND_BYTES)):
            n = len(getattr(row, field).encode())
            assert n <= budget, "%s %r is %d bytes, budget %d" % (
                field, getattr(row, field), n, budget)
            widest[field] = max(widest.get(field, 0), n)

    # Located by name, not by position: which slot a metric lands in is
    # render._ORDER's business and has already been changed once (leaf_air_dt_c
    # moved to last so the four-cell table stops dropping 습도), while what this
    # case is about - the widest label the table can draw fitting its budget - is
    # the same fact wherever the row sits.
    by = {r.label: r for r in rows}
    assert set(by) == {"장면최고차", "CO2"}, sorted(by)
    assert len("장면최고차".encode()) == 15, "the widest label changed width"
    # The unclipped form is 25 bytes, so _fit had to cut it. It cut on a code
    # point boundary and marked the cut: a bare truncation would be indefensible
    # here because the number it drops is the one the row is about.
    co2 = by["CO2"].stat
    assert len("10000 / 12500 / 15000 ppm".encode()) == 25
    assert co2 != "10000 / 12500 / 15000 ppm", co2
    assert co2.endswith("…"), co2
    assert len(co2.encode()) <= schema.ROW_WSTAT_BYTES, co2
    return "worst case: label %d/%d stat %d/%d band %d/%d bytes, co2 clipped to %r" % (
        widest["label"], schema.ROW_WLABEL_BYTES,
        widest["stat"], schema.ROW_WSTAT_BYTES,
        widest["band"], schema.ROW_WBAND_BYTES, co2)


def test_order_is_the_display_order():
    """Rows come back in render._ORDER whatever order the summary was built in.

    dicts preserve insertion order, and window_summary builds its metrics in
    _METRIC_COLUMN order, so a table that just iterated the dict would look
    correct until someone reordered that map - at which point the panel's rows
    swap places between two polls with no code change anywhere near the UI.
    """
    metrics = {}
    # Deliberately the reverse of _ORDER, and one key _ORDER does not rank last.
    for key in ("rh_pct", "air_c", "co2_ppm", "leaf_air_dt_c", "vpd_kpa"):
        metrics[key] = _metric(mn=1.0, mean=2.0, mx=3.0)
    rows, _span, _covered = render.window_block(_window(metrics))

    got = [r.label for r in rows]
    want = [render._METRICS[k].label for k in render._ORDER
            if k in metrics][:schema.WINDOW_ROWS_MAX]
    assert got == want, (got, want)
    # Five metrics into four slots: the one _ORDER ranks last is the one that falls
    # off, and it falls off the end rather than out of the middle. That is
    # leaf_air_dt_c, deliberately - the scene-peak hint yields the slot so 습도, a
    # directly measured reading a grower vents on, keeps its history. Asserted by
    # name and not by "whatever is last" so the day someone reorders _ORDER again
    # this states which metric they just took off the panel.
    assert len(rows) == schema.WINDOW_ROWS_MAX, rows
    assert render._METRICS["leaf_air_dt_c"].label not in got, got
    assert render._METRICS["rh_pct"].label in got, got
    return "order %s (leaf_air_dt_c dropped at the cap, not reshuffled)" % got


def test_unbanded_metric_says_nothing():
    """No band on either side: no percentage, and an empty string beside it.

    "유지 0%" would be a measurement of a band that was never set, and 0 is what
    the device would draw a red row from. The empty string is how every other
    field on this wire says it has nothing to report, so the block uses it too.
    """
    rows, _span, _covered = render.window_block(_window({
        "vpd_kpa": _metric(mn=0.9, mean=1.1, mx=1.4),  # lo and hi both None
    }))
    assert len(rows) == 1, rows
    assert rows[0].in_band_pct is None, rows[0]
    assert rows[0].band == "", rows[0]
    assert rows[0].stat == "0.9 / 1.1 / 1.4 kPa", rows[0].stat
    return "unbanded -> band=%r in_band_pct=%r stat=%r" % (
        rows[0].band, rows[0].in_band_pct, rows[0].stat)


def test_fully_in_band_reads_as_a_hold():
    """100% of a sampled window inside the band, and the string that says so.

    derive reports a tenth of a percent; the row has room for neither the digit
    nor the distinction, so it rounds - but 100 has to survive the rounding as
    100 and not as a clamped 99, because "유지 100%" is the one value in this
    column a grower reads as an answer rather than as a number.
    """
    rows, _span, _covered = render.window_block(_window({
        "vpd_kpa": _metric(lo=0.8, hi=1.2, mn=0.85, mean=1.0, mx=1.15, pct=100.0),
    }))
    assert rows[0].in_band_pct == 100, rows[0]
    assert rows[0].band == "유지 100%", rows[0].band

    # And the rounding, in both directions, plus the clamp on a summary that
    # somehow reports past the ends.
    cases = ((99.94, 100), (99.44, 99), (0.04, 0), (0.5, 0), (0.6, 1),
             (-3.0, 0), (140.0, 100))
    seen = []
    for pct, want in cases:
        row = render.window_block(_window({"vpd_kpa": _metric(
            lo=0.8, hi=1.2, mn=0.9, mean=1.0, mx=1.1, pct=pct)}))[0][0]
        assert row.in_band_pct == want, (pct, row.in_band_pct, want)
        assert row.band == "유지 %d%%" % want, row.band
        seen.append((pct, row.in_band_pct))
    return "in band 100 -> %r; rounding %s" % (rows[0].band, seen)


def test_no_window_is_an_empty_block():
    """A board on its first poll. Nothing measured, nothing to say, no exception.

    None is the ordinary case here and not an error: main.py passes the window it
    has, and a device with one telemetry row in the database has a summary with
    no metrics in it. Both have to come back as the same well-formed empty block,
    or the firmware needs a second code path for a key that is absent rather than
    empty.
    """
    out = []
    for arg in (None, {}, {"metrics": {}}, {"metrics": None}, "not a window",
                _window({}, span_s=0, covered_s=0),
                derive.window_summary([], [])):
        rows, span, covered = render.window_block(arg)
        assert rows == [], (arg, rows)
        assert span == 0, (arg, span)
        assert covered == 0, (arg, covered)
        out.append(type(arg).__name__)

    # And the same block through the model that carries it, so an empty list is
    # what the wire says too rather than a null the device has to test for.
    d = schema.Display()
    assert d.window == [] and d.window_span_s == 0 and d.window_covered_s == 0
    dumped = d.model_dump()
    assert dumped["window"] == [], dumped["window"]
    assert dumped["window_span_s"] == 0 and dumped["window_covered_s"] == 0
    return "empty for %s, and Display() defaults to an empty block" % out


def test_covered_never_exceeds_span():
    """covered_s <= span_s, which the panel divides one by the other to draw.

    derive already guarantees it - every hold is clamped to the gap it spans, so
    the sum cannot pass the end-to-end span - but the guarantee is arithmetic in
    another module, and the consequence of losing it is a coverage bar drawn past
    its own width. So it is asserted here against derive's real output AND
    against a hand-built summary that gets it wrong.
    """
    ts = 1754300000
    # A device that drops off the air for an hour in the middle: MAX_HOLD_S caps
    # what the last row before the gap can be credited with, so covered lands far
    # under span. That gap is the whole reason the two numbers travel separately.
    rows = ([{"recv_ts": ts + i * 30, "vpd_kpa": 1.0 + i * 0.1} for i in range(5)]
            + [{"recv_ts": ts + 3600 + i * 30, "vpd_kpa": 1.0} for i in range(5)])
    summary = derive.window_summary(rows, [Setpoint(key="vpd_kpa", lo=0.8, hi=1.2)])
    _rows, span, covered = render.window_block(summary)
    assert span == summary["span_s"], (span, summary["span_s"])
    assert 0 < covered < span, (covered, span)

    # The negative control: a summary that claims more coverage than window.
    _rows, span, covered = render.window_block(_window({}, span_s=100, covered_s=9999))
    assert (span, covered) == (100, 100), (span, covered)

    # And a summary whose spans are junk rather than merely wrong.
    for bad in (None, "600", float("nan"), -50):
        _rows, span, covered = render.window_block(
            {"metrics": {}, "span_s": bad, "covered_s": bad})
        assert span >= 0 and covered >= 0 and covered <= span, (bad, span, covered)
    return "gap window span=%ds covered=%ds; over-claimed coverage clamped" % (
        summary["span_s"], round(summary["covered_s"]))


def test_from_derive_end_to_end():
    """The real function's real keys. If window_summary renames min/mean/max/
    in_band_pct/span_s/covered_s, every hand-built dict above keeps passing and
    the panel goes blank - so one case reads the genuine article."""
    ts = 1754300000
    rows = [{"recv_ts": ts + i * 60,
             "vpd_kpa": 0.9 + (i % 3) * 0.2,
             "temp_c": 24.0 + i * 0.1,
             "rh_pct": 60 - i,
             "co2_ppm": 800 + i * 10,
             "leaf_air_dt_c": 1.5} for i in range(10)]
    summary = derive.window_summary(rows, [
        Setpoint(key="vpd_kpa", lo=0.8, hi=1.2),
        Setpoint(key="co2_ppm", lo=600.0, hi=None),
    ])
    out, span, covered = render.window_block(summary)
    by = {r.label: r for r in out}
    # 습도 and not 장면최고차: five reporting metrics into four cells, and _ORDER
    # ranks the scene-peak hint last so the measured reading keeps its history.
    assert set(by) == {"VPD", "CO2", "기온", "습도"}, sorted(by)
    # Banded and held for part of the window; unbanded and silent about it.
    assert by["VPD"].in_band_pct is not None and by["VPD"].band, by["VPD"]
    assert by["CO2"].in_band_pct == 100 and by["CO2"].band == "유지 100%", by["CO2"]
    assert by["기온"].in_band_pct is None and by["기온"].band == "", by["기온"]
    # Time-weighted mean between min and max, drawn with the metric's decimals.
    assert by["기온"].stat == "24 / 24.4 / 24.9 °C", by["기온"].stat
    assert covered <= span and span == 540, (covered, span)
    return "derive -> %s span=%ds covered=%ds" % (
        [(r.label, r.stat, r.band) for r in out], span, covered)


def test_a_percentage_needs_most_of_the_window_behind_it():
    """in_band_pct is withheld for a metric that barely reported.

    Its denominator is the time THAT METRIC reported, not the window's, and that
    is the right denominator: folding a dropout into "out of band" would report
    every dead sensor as a failure to hold. But the denominator never reaches the
    panel - render.window_block has 76px for the band cell and "유지 100%" spends
    59 of them - so a metric that reported for two minutes of a nine minute
    window, in band throughout, drew 유지 100% under a 관측률 line the reader had
    no way to divide it by.

    Below derive.IN_BAND_MIN_COVERAGE of the observed window the percentage is
    withheld instead of qualified: it is not a claim about the window, and a null
    in_band_pct is already drawn as a row with numbers and no verdict. The samples
    survive - what is refused is only the ratio.

    Both sides of the line are asserted from one window, so a change that
    withholds everything or withholds nothing fails here rather than reading as a
    stricter or looser threshold.
    """
    ts = 1754300000
    # Ten rows a minute apart: nine forward holds of 60s, so 540s observed. co2
    # reports on every row; air_c on the first three. A reporting row is credited
    # with its own forward gap whether or not the NEXT row reported, so that is
    # 3 x 60 = 180s of held time - 33% of the window, under the floor.
    rows = []
    for i in range(10):
        r = {"recv_ts": ts + i * 60, "co2_ppm": 800.0, "temp_c": None}
        if i < 3:
            r["temp_c"] = 22.0
        rows.append(r)
    summary = derive.window_summary(rows, [
        Setpoint(key="co2_ppm", lo=400.0, hi=1200.0),
        Setpoint(key="air_c", lo=18.0, hi=28.0),
    ])
    co2 = summary["metrics"]["co2_ppm"]
    air = summary["metrics"]["air_c"]

    assert summary["covered_s"] == 540.0, summary["covered_s"]
    # The well-covered metric keeps its percentage: 100% of 540s of 540s.
    assert co2["covered_s"] == 540.0 and co2["in_band_pct"] == 100.0, co2
    # The sparse one loses only the ratio. Its samples, its extremes and its
    # weighted mean are all still measurements and all still drawn.
    assert air["in_band_pct"] is None, air
    assert air["n"] == 3 and air["covered_s"] == 180.0, air
    assert air["min"] == 22.0 and air["mean"] == 22.0 and air["max"] == 22.0, air
    assert air["lo"] == 18.0 and air["hi"] == 28.0, air

    # And the panel draws exactly that: numbers, no band cell, no tint to read.
    out, span, covered = render.window_block(summary)
    by = {r.label: r for r in out}
    assert by["CO2"].band == "유지 100%", by["CO2"]
    assert by["기온"].band == "" and by["기온"].in_band_pct is None, by["기온"]
    assert by["기온"].stat.startswith("22 / 22 / 22"), by["기온"].stat

    # The boundary itself, from the same shape: air_c reporting on six of the ten
    # rows holds 360s of 540s, which is over half and keeps its percentage. Without
    # this the floor could be any value at all and the case above would still pass.
    for i in range(10):
        rows[i]["temp_c"] = 22.0 if i < 6 else None
    wide = derive.window_summary(rows, [Setpoint(key="air_c", lo=18.0, hi=28.0)])
    wair = wide["metrics"]["air_c"]
    assert wair["covered_s"] == 360.0, wair
    assert wair["in_band_pct"] == 100.0, wair

    return "33%% of window -> withheld, 67%% -> 100.0 (floor %.0f%%, span %ds)" % (
        derive.IN_BAND_MIN_COVERAGE * 100, span)


# --------------------------------------------------------------------------
# What the block costs on the wire
# --------------------------------------------------------------------------


def _worst_prescription():
    """A Prescription carrying the largest window block render.window_block can emit.

    Built by driving the renderer rather than by hand-filling WindowRow to the
    schema's ceiling, because those are two different numbers and only one of
    them can ever go out. The label is not free text: it comes from _METRICS and
    is chosen by _ORDER, so the widest four labels the table can show together
    are 장면최고차 / CO2 / 기온 / 습도 - vpd_kpa is the one left out, because it
    outranks 습도 in _ORDER and is three bytes narrower. Every stat is driven
    past the budget so _fit clips it to exactly 24, and every band is the widest
    유지 form there is.
    """
    wide = {"min": -12345.6, "mean": -12345.6, "max": -12345.6,
            "lo": 0.0, "hi": 1.0, "in_band_pct": 100.0}
    rows, span, covered = render.window_block({
        "span_s": 999999, "covered_s": 999999,
        "metrics": {k: dict(wide) for k in
                    ("leaf_air_dt_c", "co2_ppm", "air_c", "rh_pct")},
    })
    return schema.Prescription(
        rx_id="0123456789abcdef", issued_ts=1754300000,
        display=schema.Display(window=rows, window_span_s=span,
                               window_covered_s=covered),
    ), rows


def _size(obj):
    """starlette's own encoding: JSONResponse dumps with ensure_ascii=False and
    these separators, so this is bytes on the wire and not bytes in a repr."""
    return len(json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode())


def test_wire_cost_is_measured_not_guessed():
    """The block's cost on the wire, at the worst case, as bytes.

    Measured against a real Prescription because the number it has to be
    compared with is MAX_RESP in src/plantrx.cpp, and a response one byte over
    that is not a clipped field - it is a prescription the panel throws away
    whole. Three figures, because three of them get quoted: the block with its
    own keys, the rows alone, and what the block costs when it is empty, which
    is what every device with no history pays on every poll.
    """
    rx, rows = _worst_prescription()
    assert len(rows) == schema.WINDOW_ROWS_MAX, rows
    # A set: the worst case is about which four metrics are present, because the
    # widest label (장면최고차, 15B) has to be one of them for the byte figure to be
    # the worst one. Their order is render._ORDER's business and has moved once
    # already; it costs the same bytes whichever way round they sit.
    assert {r.label for r in rows} == {"장면최고차", "CO2", "기온", "습도"}, rows
    for r in rows:
        assert len(r.stat.encode()) == schema.ROW_WSTAT_BYTES, r.stat
        assert r.band == "유지 100%", r.band

    full = rx.model_dump()
    gone = rx.model_dump()
    for key in ("window", "window_span_s", "window_covered_s"):
        gone["display"].pop(key)
    empty = rx.model_dump()
    empty["display"].update(window=[], window_span_s=0, window_covered_s=0)

    block = _size(full) - _size(gone)
    bare = _size(empty) - _size(gone)
    # Not an assertion about a magic number - an assertion that the block is
    # bounded and small next to the 12 KB (MAX_RESP) the firmware holds for a
    # whole prescription. A row growing a field fails this and the budget gets
    # re-derived rather than re-remembered.
    assert block < 1024, block
    escaped = (len(json.dumps(full, separators=(",", ":")).encode())
               - len(json.dumps(gone, separators=(",", ":")).encode()))
    return ("largest block %d B on the wire (rows %d B, empty block %d B); "
            "%d B if something re-encodes it with \\uXXXX escapes"
            % (block, _size(full["display"]["window"]), bare, escaped))


if __name__ == "__main__":
    for fn in (test_budgets_at_the_worst_case,
               test_order_is_the_display_order,
               test_unbanded_metric_says_nothing,
               test_fully_in_band_reads_as_a_hold,
               test_no_window_is_an_empty_block,
               test_covered_never_exceeds_span,
               test_from_derive_end_to_end,
               test_a_percentage_needs_most_of_the_window_behind_it,
               test_wire_cost_is_measured_not_guessed):
        print("%-34s %s" % (fn.__name__, fn()))
    print("budget label<=%d stat<=%d band<=%d rows<=%d"
          % (schema.ROW_WLABEL_BYTES, schema.ROW_WSTAT_BYTES,
             schema.ROW_WBAND_BYTES, schema.WINDOW_ROWS_MAX))
    print("OK")
