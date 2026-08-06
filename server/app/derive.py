"""Sensor maths and window statistics. Pure functions, no I/O, no globals.

Everything here is a function of its arguments, so the same code runs over a
live Telemetry and over rows replayed out of SQLite. That is what lets
window_summary score a prescription after the fact with the identical arithmetic
that produced the numbers in the first place.

Derived values are computed here rather than on the device so that changing a
formula does not mean reflashing three boards.
"""

import math
from typing import Any, Optional

from .schema import Setpoint, Telemetry

# Setpoint.key is the model's vocabulary; the telemetry row uses the sensor's
# name. Only air_c actually differs, which is exactly why it is written down -
# a missing mapping does not raise, it silently scores air temperature against
# nothing and reports "no data" for a band that was being held all along.
_METRIC_COLUMN: dict[str, str] = {
    "vpd_kpa": "vpd_kpa",
    "air_c": "temp_c",
    "rh_pct": "rh_pct",
    "co2_ppm": "co2_ppm",
    "leaf_air_dt_c": "leaf_air_dt_c",
}

# A sample is credited for the gap until the next one, not counted once: the
# poll interval is adaptive (~2s just after something happened, ~60s idle) so
# counting rows would weight a busy minute the same as an idle hour. The hold is
# capped so a device that drops off the network does not have its last reading
# credited for the whole outage - 120s lets one missed 60s poll through and
# stops there.
MAX_HOLD_S = 120.0

# How much of a window a metric has to have reported over before its in_band_pct
# is a description of that window rather than of a sliver of it.
#
# The percentage's denominator is the time THAT METRIC reported, not the window's,
# and that is the right denominator: folding a dropout into "out of band" would
# report every dead sensor as a failure to hold. But the denominator never reaches
# the panel. It is on the wire as metrics[key]["covered_s"] and render.window_block
# has nowhere to draw it - the band cell is 76px and "유지 100%" already spends 59
# of them - so a metric that reported for five minutes of an hour, in band
# throughout, drew 유지 100% beside a window-level 관측률 the reader could not
# divide it by.
#
# Withheld rather than qualified, because below this line it is not a claim about
# the window at all, and the panel already draws a null in_band_pct as a row with
# numbers and no verdict - which is exactly what a metric with too little history
# is. Half, because anything a majority of the observed window did not see cannot
# characterise it. The samples themselves are untouched and still drawn as
# min / mean / max: what is refused is only the ratio.
IN_BAND_MIN_COVERAGE = 0.5


def _finite(x: Any) -> Optional[float]:
    """None for anything that cannot take part in arithmetic - None itself, a
    non-number, or a NaN/inf that would poison every min/mean downstream."""
    if x is None or isinstance(x, bool):
        return None
    try:
        v = float(x)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def _present(x: Any) -> Optional[float]:
    """A reading the sensor actually produced, or None.

    Belt and braces against a -999 sentinel arriving as a number rather than the
    null the firmware is supposed to send. -999 is a perfectly finite float, so
    _finite passes it and every clamp downstream swallows it into something
    plausible: an RH clamp turns it into 0 %RH and answers with a confident
    full-saturation deficit for a humidity sensor that never reported.

    src/plantrx.cpp nulls the never-reported sentinel on the way out and
    src/aijudge.cpp spells this predicate `present()` as `> -999.0f`; this is the
    same test, kept here because an older firmware will keep sending the raw
    sentinel at a server that has to stay correct without it.
    """
    v = _finite(x)
    return None if v is None or v <= -999.0 else v


def vpd_kpa(temp_c: Optional[float], rh_pct: Optional[float]) -> Optional[float]:
    """Vapour pressure deficit of the air, in kPa. None if either input is.

        es(T) = 0.61078 * exp(17.27 * T / (T + 237.3))    [kPa, T in degC]
        VPD   = es(T) * (1 - RH/100)                      [kPa]

    Tetens/Magnus over liquid water. The over-ice coefficients differ, but a
    sub-zero greenhouse has problems VPD will not describe.

    This is air-to-air VPD. Leaf-to-air is the number a grower actually wants
    and it needs leaf temperature; the only thermal figure this system has is
    the hottest pixel of the 32x24 frame, which is the scene maximum and will be
    won by a heater or a lamp in view. Substituting it here was rejected - it
    would produce a confident, wrong VPD. leaf_air_dt_c reports the difference
    raw instead, under a name that says what it is.
    """
    t = _present(temp_c)
    rh = _present(rh_pct)
    if t is None or rh is None:
        return None
    # Outside this range the Tetens denominator is either meaningless or, below
    # -237.3, a division by zero. src/aijudge.cpp checks present() on both inputs
    # before this same domain test and clamps RH after it, so the two languages
    # admit and refuse exactly the same set.
    if not -60.0 <= t <= 100.0:
        return None
    # A capacitive RH sensor reading 101% at saturation is normal and would
    # otherwise come back as a small negative deficit.
    rh = min(100.0, max(0.0, rh))
    es = 0.61078 * math.exp(17.27 * t / (t + 237.3))
    return round(es * (1.0 - rh / 100.0), 3)


def leaf_air_dt_c(
    leaf_max_c: Optional[float], temp_c: Optional[float]
) -> Optional[float]:
    """Scene-maximum minus air temperature, in degC. None if either input is.

    Positive means something in frame is hotter than the air. Under transpiration
    a leaf sits *below* air temperature, so a large positive value is far more
    likely to be a lamp, a heater or the pot rim than a stressed plant. Naming it
    leaf_* is a firmware inheritance; anything reasoning about it has to be told
    what it really measures.
    """
    leaf = _present(leaf_max_c)
    air = _present(temp_c)
    if leaf is None or air is None:
        return None
    return round(leaf - air, 2)


def thermal_peak_c(t: Telemetry) -> Optional[float]:
    """The scene peak the device is measuring NOW, or None when it is not.

    src/thermal.cpp keeps a running maximum that is written on every accepted
    frame and reset by nothing, so a board whose MLX90640 has stopped keeps
    reporting the last peak it ever saw. links.thermal_live is the device's own
    10s judgement on that link and it is the only thing on the wire that can
    tell a peak measured this second from one measured an hour ago.

    The firmware is being fixed to stop sending the stale peak at all. This gate
    is not redundant with that fix: older firmware will keep sending it, and a
    server that is only correct when the device is correct has not checked
    anything. The two agree, which is the point - neither is load-bearing alone.
    """
    if not t.links.thermal_live:
        return None
    return _present(t.sensors.leaf_max_c)


def enrich(t: Telemetry) -> dict:
    """The derived half of a telemetry row, ready to hand to store.save_telemetry."""
    s = t.sensors
    return {
        "vpd_kpa": vpd_kpa(s.temp_c, s.rh_pct),
        "leaf_air_dt_c": leaf_air_dt_c(thermal_peak_c(t), s.temp_c),
    }


def current_readings(t: Telemetry) -> dict:
    """Everything this poll says is true right now: the raw half with dead links
    refused, plus the derived half. What the prompt is handed and what the card
    is rendered from.

    leaf_max_c is republished here rather than passed through from Sensors
    because two readers take the peak itself and not the delta - the model prompt
    and render._CHIP_FMT's 장면최고 chip. A peak this module has already refused
    to derive from must not reach either of them through a different door, or the
    panel prints an hour-old number beside a live one.
    """
    out = t.sensors.model_dump()
    out["leaf_max_c"] = thermal_peak_c(t)
    out.update(enrich(t))
    return out


def _in_band(v: float, lo: Optional[float], hi: Optional[float]) -> bool:
    """One-sided bands are normal: "CO2 above 600" has no upper bound."""
    if lo is not None and v < lo:
        return False
    if hi is not None and v > hi:
        return False
    return True


def _level(actuators: Any, name: str) -> int:
    """Actuator level 0..100. Absent means the firmware is not driving it, which
    is off - the device sends a full snapshot, not a delta."""
    if not isinstance(actuators, dict):
        return 0
    v = _finite(actuators.get(name))
    if v is None:
        return 0
    return int(max(0.0, min(100.0, v)))


def _integrate(
    rows: list[dict], holds: list[float], column: str, time_key: str
) -> dict[str, dict]:
    """Per-actuator time integral of one 0..100 level column.

    Two columns carry this shape - `actuators` is measured hardware, and
    `actuator_intent` is the switch positions the panel reports - so the split
    below is computed once and named by its caller. `time_key` is the caller's
    precisely because the two must not be confusable: measured run time and
    held-switch time are different claims, and the relay that would make the
    first one real has not been delivered.

    Names come from the rows rather than from a list here, so the vocabulary
    stays the device's. A key that appears halfway through the window is scored
    over the whole of it, which is correct - _level reads an absent key as off.
    """
    names: set[str] = set()
    for r in rows:
        a = r.get(column)
        if isinstance(a, dict):
            names.update(a.keys())

    out: dict[str, dict] = {}
    for name in sorted(names):
        any_s = 0.0
        duty_s = 0.0
        last = 0
        for i, r in enumerate(rows):
            last = _level(r.get(column), name)
            if last > 0:
                # any_s treats anything above zero as on; duty_s weights by
                # level. The fan is the reason both exist - it runs 0..100 and
                # "on for 40 minutes at 20%" is not the same event as "on for 40
                # minutes at full", which is what a bare on-time would report.
                any_s += holds[i]
                duty_s += holds[i] * last / 100.0
        out[name] = {
            time_key: round(any_s, 1),
            "duty_s": round(duty_s, 1),
            "last": last,
        }
    return out


def _count(x: Any) -> Optional[int]:
    """A monotonic since-boot counter as a row carries it, or None where it is
    absent.

    Absence is the only thing None means here, and it has to stay separable from
    zero for the same reason `intent` has to stay separable from `actuators`: a
    panel nobody touched reports 0 every poll for as long as it stays untouched,
    so a missing field read as 0 would answer "nothing moved" on behalf of a
    firmware that said nothing at all.

    A negative is not a counter. It is dropped rather than clamped, because
    clamped to zero it would enter the walk below as a board that had just
    rebooted.
    """
    if x is None or isinstance(x, bool):
        return None
    try:
        v = int(x)
    except (TypeError, ValueError):
        return None
    return v if v >= 0 else None


def _movement(rows: list[dict], column: str) -> tuple[Optional[int], bool]:
    """How far one since-boot counter travelled across `rows`, and whether it was
    seen to restart on the way.

    The counter is monotonic, so the movement inside the span is the difference
    between its ends and never a sum of the values - summing them would report an
    hour of an untouched panel as fifty presses.

    It is walked pair by pair rather than subtracted end to end because a reboot
    puts the counter back to zero, and `last - first` across that reads as a
    negative number of presses. A step that rose contributes its rise; a step
    that fell is a restart and contributes the value it fell TO, which is the
    movement since the board came back. Both terms are non-negative and neither
    can exceed the real movement, so no window produces a negative count and no
    reboot produces a large one.

    That makes the result a floor across a restart and not a total: whatever was
    pressed between the last poll before the reboot and the reboot itself was
    never reported, and this refuses to invent it. Undercounting a press nobody
    witnessed is honest; extrapolating one would be convenient. The flag is what
    keeps the floor from being read as a total.
    """
    total = 0
    prev: Optional[int] = None
    reset = False
    for r in rows:
        v = _count(r.get(column))
        if v is None:
            continue  # this poll reported no counter: out of the walk entirely
        if prev is not None:
            if v >= prev:
                total += v - prev
            else:
                reset = True
                total += v
        prev = v
    # prev doubles as "some row reported": a span of rows that all omitted the
    # column has no count, which is not the same claim as a count of zero.
    return (total if prev is not None else None), reset


def interventions(rows: list[dict]) -> dict:
    """The grower's own hands on the panel over `rows`.

    Returns ``{"edges", "allstops", "counter_reset"}``. The two counts are
    movement inside the span, not the counters themselves, and both are None
    where no row in the span carried them - a firmware that predates the fields,
    which is not a panel nobody touched. `counter_reset` says a counter was seen
    to go backwards, so the counts beside it are floors; see _movement.

    Public because window_summary is not the only caller that needs it.
    main._run_brain cuts its rows at a restart before summarising them, since a
    reading from before a reboot is not evidence about bands the board dropped on
    the way down - but a 전체 정지 pressed before that reboot was still pressed
    against the standing prescription, so the presses are read off the whole span
    and the readings off what survived the cut.
    """
    edges, edges_reset = _movement(rows, "edges")
    allstops, allstops_reset = _movement(rows, "allstops")
    return {
        "edges": edges,
        "allstops": allstops,
        "counter_reset": edges_reset or allstops_reset,
    }


def window_summary(rows: list[dict], setpoints: list[Setpoint]) -> dict:
    """What the last prescription actually achieved over `rows`.

    `rows` are telemetry dicts in ascending recv_ts order (what telemetry_since
    returns); `setpoints` are the bands that were in force over that span. The
    window is the span the rows cover, not the span that was asked for - the
    caller chose since_ts and can compare.

    Returns::

        {"start_ts", "end_ts", "span_s", "n_rows", "covered_s",
         "metrics":   {key: {lo, hi, in_band_pct, min, mean, max, n, covered_s}},
         "actuators": {name: {on_s, duty_s, last}},
         "intent":    {name: {held_s, duty_s, last}},
         "interventions": {edges, allstops, counter_reset}}

    `actuators` is measured hardware and is empty until a relay exists to
    measure. `intent` is the same arithmetic over the switch positions the panel
    reports, and it is the only one of the two that has anything in it today -
    which is why its time is `held_s` and not `on_s`. Reading held-switch time
    as run time is the mistake the naming is there to prevent: nothing the panel
    switches on physically moves yet.

    `interventions` is the same span read as movement rather than as state.
    `intent` above integrates the switch positions the polls reported, so a
    switch flicked on and off between two of them is in neither snapshot and
    contributes nothing to it; the counters see it. A window whose bands were
    held with two 전체 정지 presses in the middle of it is not a window the
    prescription can be credited with, and this block is the only thing that
    says so.

    Every metric key is reported, not only the ones with a setpoint, because the
    model has to see CO2 drifting even in a window where nobody asked for a CO2
    band. `in_band_pct` is None where there was no band to hold - reporting 100%
    for an absent or unbounded setpoint would read as a success that never
    happened.

    Sparse and empty inputs are ordinary: sensors drop out individually, and a
    device that has just booted has one row or none. Nothing here divides by a
    count it has not checked.
    """
    # Last setpoint wins for a duplicated key. The model should not emit two
    # bands for one metric; if it does, the later one is the one it settled on.
    bands: dict[str, tuple[Optional[float], Optional[float]]] = {
        sp.key: (sp.lo, sp.hi) for sp in setpoints
    }

    n = len(rows)
    ts = [int(r.get("recv_ts") or 0) for r in rows]

    # Zero-order hold, forward: row i describes reality until row i+1 arrives.
    # The final row therefore carries no weight, which is correct - we do not
    # know how long it held. Out-of-order rows clamp to zero rather than
    # subtracting time from the window.
    holds = [0.0] * n
    for i in range(n - 1):
        holds[i] = max(0.0, min(float(ts[i + 1] - ts[i]), MAX_HOLD_S))

    # How much of the window was observed at all, the denominator the per-metric
    # coverage floor is measured against. See IN_BAND_MIN_COVERAGE.
    window_covered = sum(holds)

    metrics: dict[str, dict] = {}
    for key, col in _METRIC_COLUMN.items():
        lo, hi = bands.get(key, (None, None))
        bounded = key in bands and (lo is not None or hi is not None)

        vals: list[float] = []
        w_total = 0.0
        w_value = 0.0
        w_in_band = 0.0
        for i, r in enumerate(rows):
            v = _finite(r.get(col))
            if v is None:
                continue  # sensor dropout: out of numerator and denominator both
            vals.append(v)
            w = holds[i]
            w_total += w
            w_value += v * w
            if bounded and _in_band(v, lo, hi):
                w_in_band += w

        if vals:
            # Time-weighted, for the same reason time-in-band is. Falls back to
            # the plain mean when every sample is weightless - a single row, or
            # rows sharing one timestamp - where the two agree anyway.
            mean = (w_value / w_total) if w_total > 0 else (sum(vals) / len(vals))
            stat: dict = {
                "n": len(vals),
                # first/last are the before and after a "VPD 2.1 -> 1.3 kPa"
                # line is made of; min/max cannot express direction, and the
                # display has no other source for it. vals follows the rows, so
                # these are the earliest and latest samples that reported.
                "first": round(vals[0], 3),
                "last": round(vals[-1], 3),
                "min": round(min(vals), 3),
                "mean": round(mean, 3),
                "max": round(max(vals), 3),
            }
        else:
            stat = {
                "n": 0,
                "first": None,
                "last": None,
                "min": None,
                "mean": None,
                "max": None,
            }

        stat["lo"] = lo
        stat["hi"] = hi
        stat["covered_s"] = round(w_total, 1)
        # The floor is against the window's observed time and not against span_s:
        # span includes stretches nobody was on the air for, and holding a metric
        # responsible for those would blank a percentage that is honest about every
        # second anybody measured. A window with no coverage at all divides by
        # nothing and answers None through the same branch.
        enough = window_covered > 0 and w_total >= IN_BAND_MIN_COVERAGE * window_covered
        stat["in_band_pct"] = (
            round(100.0 * w_in_band / w_total, 1)
            if bounded and w_total > 0 and enough else None
        )
        metrics[key] = stat

    # Both blocks stay, and the empty one is not an oversight: `actuators` is the
    # shape the wire already promises, and dropping it would make the day the
    # relays land a schema change rather than a device that starts reporting.

    return {
        "start_ts": ts[0] if n else None,
        "end_ts": ts[-1] if n else None,
        "span_s": (ts[-1] - ts[0]) if n else 0,
        "n_rows": n,
        # Less than span_s wherever a hold was capped, so the caller can tell a
        # well-sampled window from one with the device off the air in the middle.
        "covered_s": round(sum(holds), 1),
        "metrics": metrics,
        "actuators": _integrate(rows, holds, "actuators", "on_s"),
        "intent": _integrate(rows, holds, "actuator_intent", "held_s"),
        # Unweighted by `holds`, unlike the two blocks above: a press is an event
        # and not a level held for a stretch, so there is no interval to
        # integrate over and a poll gap does not dilute it.
        "interventions": interventions(rows),
    }
