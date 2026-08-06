"""The metric vocabulary: derive.window_summary -> brain.clamp_output's gate.

    cd server && python tests/test_metric_vocab.py

Plain asserts and no test framework, like every other file in here.

A setpoint is a band the panel is told to hold, and the panel is judged on
holding it: the trail scores it, the window table draws how much of the window
it was inside, and the card gives it a row. All three of those need a reading.
A band on a metric that returned nothing all window can be none of them - it is
a row that says "hold 조도 800-1200 lx" beside an instrument this board does not
have, and the grower has no way to tell it apart from a band being missed.

So the gate: a metric is prescribable iff the window just measured reported
samples for it. It is deliberately the same shape as the actuator gate one field
over, including the escape hatch - an empty vocabulary means nothing has been
measured yet (a fresh boot, an empty database), not that nothing is measurable,
and dropping every band on the first poll of a new install would be worse than
the bug. The two gates run off different inputs in the same call, so this file
asserts both at once: a change that made either one read the other's vocabulary
would pass every test that looked at one alone.

test_window_block.py covers the summary these counts are read from.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from app import brain, derive, schema  # noqa: E402
from app.schema import Setpoint, Telemetry  # noqa: E402

NOW = 1754300000


def _stat(n, mn=1.0, mean=1.1, mx=1.2):
    """One entry of window_summary()["metrics"]. `n` is the sample count the
    gate reads; the rest is there so the dict is the shape derive builds."""
    if not n:
        return {"n": 0, "first": None, "last": None, "min": None, "mean": None,
                "max": None, "lo": None, "hi": None, "covered_s": 0.0,
                "in_band_pct": None}
    return {"n": n, "first": mn, "last": mx, "min": mn, "mean": mean, "max": mx,
            "lo": None, "hi": None, "covered_s": 900.0, "in_band_pct": None}


def _window(metrics):
    return {"start_ts": NOW - 3600, "end_ts": NOW, "span_s": 3600, "n_rows": 60,
            "covered_s": 3000.0, "metrics": metrics, "actuators": {}, "intent": {}}


def _reply(setpoints=(), once=(), policy=None):
    """A raw model answer, in the flat wire layout clamp_output takes."""
    return {
        "diagnosis_ko": "정상 범위",
        "head_ko": "생육 조건 양호",
        "level": "ok",
        "confidence": 0.8,
        "setpoints": [dict(sp) for sp in setpoints],
        "once": [dict(o) for o in once],
        "policy": policy or {},
        "wake_after_s": 900,
    }


def _keys(out):
    return [sp.key for sp in out.control.setpoints]


# --------------------------------------------------------------------------


def test_a_band_on_a_reading_metric_survives():
    """The ordinary case: the sensor reported, so the band is prescribable.

    Asserted through clamp_output rather than through _setpoints, because the
    band still has to survive scheduler.clamp_control afterwards - a gate that
    passed a setpoint the outer interlock then dropped would look identical from
    inside the gate and be invisible on the wire.
    """
    readable = brain.readable_metrics(_window({
        "vpd_kpa": _stat(12), "air_c": _stat(12),
    }))
    out = brain.clamp_output(
        _reply([{"key": "vpd_kpa", "lo": 0.8, "hi": 1.2}]), NOW, (), readable
    )
    assert _keys(out) == ["vpd_kpa"], _keys(out)
    assert out.control.setpoints[0].lo == 0.8 and out.control.setpoints[0].hi == 1.2
    return "readable %s -> kept %s" % (sorted(readable), _keys(out))


def test_a_band_on_a_silent_metric_is_dropped():
    """The metric is in the summary and in MetricKey, and still has no data.

    n == 0 is what derive writes for a sensor that returned null on every row of
    the window - the case that is NOT a missing key, and the one a gate written
    against `in metrics` instead of against the count would wave through.
    """
    readable = brain.readable_metrics(_window({
        "vpd_kpa": _stat(12), "co2_ppm": _stat(0),
    }))
    assert "co2_ppm" not in readable, sorted(readable)
    out = brain.clamp_output(
        _reply([
            {"key": "vpd_kpa", "lo": 0.8, "hi": 1.2},
            {"key": "co2_ppm", "lo": 600.0},
        ]),
        NOW, (), readable,
    )
    assert _keys(out) == ["vpd_kpa"], _keys(out)
    return "co2_ppm n=0 -> dropped, kept %s" % _keys(out)


def test_an_empty_vocabulary_drops_nothing():
    """First poll of a new install: no window, so no evidence, so no gate.

    Three spellings of "nothing measured" - the default argument, an explicit
    empty set, and a window that is genuinely empty - because a device with no
    history reaches this through all three depending on the caller.
    """
    empty = brain.readable_metrics(_window({}))
    assert empty == frozenset(), empty
    assert brain.readable_metrics(None) == frozenset()
    assert brain.readable_metrics({}) == frozenset()

    bands = [{"key": "vpd_kpa", "lo": 0.8, "hi": 1.2}, {"key": "co2_ppm", "lo": 600.0}]
    for label, out in (
        ("default", brain.clamp_output(_reply(bands), NOW)),
        ("explicit", brain.clamp_output(_reply(bands), NOW, (), frozenset())),
        ("empty window", brain.clamp_output(_reply(bands), NOW, (), empty)),
    ):
        assert _keys(out) == ["vpd_kpa", "co2_ppm"], (label, _keys(out))

    # And the sample count really is what makes the difference: the same reply
    # against a window where one metric read is gated, so the case above is the
    # escape hatch firing and not the gate being unreachable.
    one = brain.readable_metrics(_window({"vpd_kpa": _stat(12)}))
    assert _keys(brain.clamp_output(_reply(bands), NOW, (), one)) == ["vpd_kpa"]
    return "empty -> both kept; {vpd_kpa} -> co2_ppm dropped"


def test_the_two_gates_do_not_interfere():
    """One call, two vocabularies, four outcomes - and no crosstalk.

    The failure this guards is a gate that reads the wrong argument: with only
    one of the two asserted per call, a `_setpoints` that checked `inventory`
    would still pass every setpoint test whose fixture happened to declare the
    metric as a switch name.
    """
    readable = brain.readable_metrics(_window({"vpd_kpa": _stat(12), "rh_pct": _stat(0)}))
    inventory = {"mist": 0, "fan": 40}

    out = brain.clamp_output(
        _reply(
            setpoints=[
                {"key": "vpd_kpa", "lo": 0.8, "hi": 1.2},  # readable
                {"key": "rh_pct", "lo": 55.0, "hi": 80.0},  # measured nothing
            ],
            once=[
                {"actuator": "mist", "seconds": 45},  # owned
                {"actuator": "fogger", "seconds": 45},  # invented
            ],
            policy={"fan": "auto", "fogger": "off"},
        ),
        NOW, inventory, readable,
    )

    assert _keys(out) == ["vpd_kpa"], _keys(out)
    assert [a.actuator for a in out.control.once] == ["mist"], out.control.once
    assert out.control.policy == {"fan": "auto"}, out.control.policy

    # The vocabularies are not each other's: a metric name is not an actuator
    # name and vice versa, so swapping the two arguments has to break something.
    swapped = brain.clamp_output(
        _reply(
            setpoints=[{"key": "vpd_kpa", "lo": 0.8, "hi": 1.2}],
            once=[{"actuator": "mist", "seconds": 45}],
        ),
        NOW, readable, inventory,
    )
    assert _keys(swapped) == [], _keys(swapped)
    assert swapped.control.once == [], swapped.control.once
    return "kept vpd_kpa + mist + fan; swapping the vocabularies keeps nothing"


def test_every_band_dropped_is_still_a_prescription():
    """The whole control block gated away, and the answer is still well formed.

    This is the poll after a sensor node dies: the model, running on the last
    window it was told about, prescribes bands nothing can read any more. An
    empty Control is a legal Control - the card falls back to what it can draw -
    and the diagnosis, the level and the wake are untouched, because none of
    them depends on a band having survived.
    """
    readable = brain.readable_metrics(_window({"vpd_kpa": _stat(4)}))
    out = brain.clamp_output(
        _reply([{"key": "air_c", "lo": 20.0, "hi": 27.0},
                {"key": "rh_pct", "lo": 55.0, "hi": 80.0}]),
        NOW, (), readable,
    )
    assert isinstance(out, brain.BrainOutput)
    assert out.control.setpoints == [], out.control.setpoints
    assert out.head_ko == "생육 조건 양호" and out.level == "ok"
    assert out.wake_after_s == 900, out.wake_after_s
    # And it serialises: this is what store.save_prescription writes and what
    # render.build_display is handed.
    assert out.model_dump(mode="json")["control"]["setpoints"] == []
    return "0 bands, head %r, wake %ds" % (out.head_ko, out.wake_after_s)


def test_the_prompt_names_the_readable_metrics():
    """The model is told, not just corrected.

    A gate that silently deletes output every poll is a gate that argues with
    the model forever; the payload carries the vocabulary so the model can stop
    asking. Asserted on the built prompt string, so a refactor that stops
    telling it fails here rather than on the wall.
    """
    inp = brain.BrainInput(
        now_ts=NOW,
        window=_window({"vpd_kpa": _stat(12), "air_c": _stat(12), "rh_pct": _stat(0)}),
    )
    text = brain._payload(inp)
    assert '"readable_metrics": [' in text or '"readable_metrics":[' in text, text[:400]
    assert '"air_c"' in text and '"vpd_kpa"' in text
    # The silent one is named nowhere in the vocabulary block - it is still in
    # the window summary, with its zero count, which is a different claim.
    head = text[: text.index("window_since_last_prescription")]
    assert "rh_pct" not in head, head

    # The static half: the system prompt has to explain what the list means, or
    # a model that sees an unfamiliar key ignores it.
    assert "readable_metrics" in brain.SYSTEM_PROMPT

    # An empty window says so with null rather than with an empty list, the way
    # actuator_inventory does - "[]" reads as a board with no sensors at all.
    bare = brain._payload(brain.BrainInput(now_ts=NOW))
    assert '"readable_metrics": null' in bare, bare[:400]
    return "payload lists vpd_kpa,air_c; rh_pct (n=0) absent; empty -> null"


def test_from_derive_end_to_end():
    """The real summary's real counts. If derive renames `n`, this fails.

    Every other case in this file hand-builds the metric dict; this one runs
    telemetry rows through window_summary, so the key the gate reads cannot
    drift from the key derive writes without a test going red.
    """
    rows = [
        {"recv_ts": NOW - 600, "temp_c": 22.0, "rh_pct": None,
         "co2_ppm": 700.0, "vpd_kpa": 1.0, "leaf_air_dt_c": None},
        {"recv_ts": NOW - 300, "temp_c": 23.0, "rh_pct": None,
         "co2_ppm": 720.0, "vpd_kpa": 1.1, "leaf_air_dt_c": None},
    ]
    summary = derive.window_summary(rows, [Setpoint(key="vpd_kpa", lo=0.8, hi=1.2)])
    assert summary["metrics"]["rh_pct"]["n"] == 0, summary["metrics"]["rh_pct"]
    readable = brain.readable_metrics(summary)
    assert readable == frozenset({"vpd_kpa", "air_c", "co2_ppm"}), sorted(readable)

    out = brain.clamp_output(
        _reply([{"key": "air_c", "lo": 20.0, "hi": 27.0},
                {"key": "rh_pct", "lo": 55.0, "hi": 80.0}]),
        NOW, (), readable,
    )
    assert _keys(out) == ["air_c"], _keys(out)

    # A device that sent no usable row at all is the escape hatch, reached the
    # way production reaches it: store.telemetry_since returned nothing.
    assert brain.readable_metrics(derive.window_summary([], [])) == frozenset()
    return "derive n -> %s; rh_pct silent -> dropped" % sorted(readable)


def test_the_dead_sensors_are_out_of_the_vocabulary_entirely():
    """lux and soil_pct cannot be prescribed, gate or no gate.

    MetricKey does not contain them, so the band never becomes a Setpoint and
    _models drops it one step before the gate - which is why render._METRICS no
    longer carries a 조도 label either: no caller can reach it. The gate is the
    per-device, per-window half of the same rule and it is not what stops these.
    """
    assert "lux" not in getattr(schema.MetricKey, "__args__", ())
    assert "soil_pct" not in getattr(schema.MetricKey, "__args__", ())

    # Telemetry still carries both, and that is correct: the wire reports the
    # sensor as null rather than pretending the field is gone.
    assert "lux" in Telemetry.model_fields["sensors"].annotation.model_fields

    readable = brain.readable_metrics(_window({"vpd_kpa": _stat(12)}))
    out = brain.clamp_output(
        _reply([{"key": "lux", "lo": 800.0, "hi": 1200.0},
                {"key": "vpd_kpa", "lo": 0.8, "hi": 1.2}]),
        NOW, (), readable,
    )
    assert _keys(out) == ["vpd_kpa"], _keys(out)
    return "lux/soil_pct outside MetricKey; a lux band never reaches the gate"


_METRICS_H = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "include", "metrics.h")


def test_the_firmware_knows_the_same_five_metrics():
    """The device's copy of MetricKey, pinned to this one.

    control.setpoints only reaches the panel's tiles and its local rule through
    plantrx_band(), which matches the band's key against a fixed table - and that
    table is five string literals in include/metrics.h. Nothing tied it to
    MetricKey. Add a sixth metric here, ship a band for it, and the device returns
    false for that key and quietly judges the reading against its own compiled-in
    number instead: no error, no log line, and a tile whose title stays grey while
    the server believes it prescribed something. That is the exact failure the
    header was written to prevent inside the firmware, and it had no counterpart
    across the wire.

    Spelling matters as much as membership. air_c is the metric; temp_c is the
    Sensors field for the same quantity (derive._METRIC_COLUMN maps between them),
    and a device that pinned temp_c would return no band for temperature forever
    while looking correct in every review.

    Absent when the firmware tree is not on disk, the same convention
    test_display_contract and the VPD parity case in test_scheduler_gates use.
    """
    if not os.path.exists(_METRICS_H):
        return "no firmware tree; skipped"
    with open(_METRICS_H, encoding="utf-8") as fh:
        src = fh.read()
    # #define METRIC_<NAME> "<key>", ignoring anything in the prose above them.
    device = set(re.findall(r'^#define\s+METRIC_[A-Z0-9_]+\s+"([a-z_0-9]+)"', src, re.M))
    server = set(schema.MetricKey.__args__)
    assert device, "no METRIC_* defines found in include/metrics.h"
    assert device == server, (
        "the firmware and the schema disagree: device only %s, server only %s"
        % (sorted(device - server), sorted(server - device)))

    # And the device's band table has a slot for each of them. BAND_N is derived
    # from the array's own size, so the count is what proves the array was extended
    # rather than only the header.
    plantrx = os.path.join(os.path.dirname(_METRICS_H), "..", "src", "plantrx.cpp")
    if os.path.exists(plantrx):
        with open(plantrx, encoding="utf-8") as fh:
            body = fh.read()
        m = re.search(r"BAND_METRICS\[\]\s*=\s*\{(.*?)\}", body, re.S)
        assert m, "plantrx.cpp no longer spells BAND_METRICS[] = { ... }"
        used = set(re.findall(r"METRIC_[A-Z0-9_]+", m.group(1)))
        assert len(used) == len(server), (
            "BAND_METRICS carries %d of the %d metrics: %s"
            % (len(used), len(server), sorted(used)))

    return "firmware and schema agree on %d metrics: %s" % (
        len(server), " ".join(sorted(server)))


_AIJUDGE_CPP = os.path.join(os.path.dirname(_METRICS_H), "..", "src", "aijudge.cpp")


def test_the_prompt_quotes_the_panels_real_fallback_bands():
    """The numbers the prompt tells the model to compare its view against.

    Omitting a band is not "no opinion" - the panel judges that metric against a
    compiled-in fallback instead, and marks the tile as its own. The prompt says so
    and quotes the figures, because a model that cannot tell whether its view
    differs from the fallback cannot decide whether sending a band is worth it.

    Quoting them makes the prompt a third copy of PANEL_BANDS, after aijudge.cpp's
    table and the monitor tiles that read it - and the tiles at least read the
    table. Prose cannot, so it is pinned here: move a band on the device and this
    fails instead of the prompt quietly describing a panel that no longer exists.

    Only the figures the prompt actually names are checked. It is guidance, not a
    dump of the table, and requiring every bound to be quoted would push it into
    being one.
    """
    if not os.path.exists(_AIJUDGE_CPP):
        return "no firmware tree; skipped"
    with open(_AIJUDGE_CPP, encoding="utf-8") as fh:
        src = fh.read()
    m = re.search(r"PANEL_BANDS\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    assert m, "aijudge.cpp no longer spells PANEL_BANDS[] = { ... };"

    bands = {}
    for key, lo, hi in re.findall(
            r"\{\s*(METRIC_[A-Z0-9_]+)\s*,\s*([-\w.]+)f?\s*,\s*([-\w.]+)f?\s*\}",
            m.group(1)):
        bands[key] = (lo.rstrip("f"), hi.rstrip("f"))
    assert len(bands) == len(schema.MetricKey.__args__), bands

    prompt = brain.SYSTEM_PROMPT
    # What the prompt names, and the table entry each claim has to match.
    claims = [("0.5-1.2 kPa VPD", "METRIC_VPD", ("0.5", "1.2")),
              ("18-28 degC", "METRIC_AIR_C", ("18.0", "28.0")),
              ("40-70 %RH", "METRIC_RH", ("40.0", "70.0")),
              ("400 ppm CO2 floor", "METRIC_CO2", ("400.0", "NAN"))]
    for text, key, want in claims:
        assert text in prompt, "the prompt no longer says %r" % text
        assert bands[key] == want, (
            "the prompt says %r but PANEL_BANDS has %s = %s"
            % (text, key, bands[key]))

    # And the one the prompt deliberately does not quote: the scene-peak delta's
    # band is one-sided and the metric is a hint, so naming a figure for it would
    # invite the model to prescribe against the hottest pixel in the room.
    assert bands["METRIC_LEAF_DT"][0] == "NAN", bands["METRIC_LEAF_DT"]
    assert "leaf_air_dt" not in prompt.split("compiled-in fallback")[-1][:400]

    return "prompt and PANEL_BANDS agree on %d quoted bands" % len(claims)


def test_no_key_reaches_the_model_unexplained():
    """Every field in `last_prescription` is named in the prompt, or it is not sent.

    main.py's `last` dict is dumped verbatim into the payload, so anything added to
    it becomes a key the model has to interpret from its name alone. Several were:
    running_on_device, device_restarted_in_window and the two intervention counters
    all arrived to change what a careful reading concludes and arrived undocumented,
    which changes nothing - "behind" and "lost" are a coin flip to a reader who was
    never told they are a fixed vocabulary. A raw prescription id went the other
    way and was removed: it had no reading the model could act on, and
    running_on_device already carries the only relationship that matters.

    The keys are read out of main.py rather than listed here, so a new one fails
    this test on the poll it is added rather than being silently guessed at.
    """
    main_py = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "app", "main.py")
    with open(main_py, encoding="utf-8") as fh:
        src = fh.read()
    # The `last=(...)` block: from the dict literal to the `if prev` that closes it.
    m = re.search(r"last=\(\s*\{(.*?)\n\s*\}\s*\n\s*if prev", src, re.S)
    assert m, "main.py no longer builds `last` as a dict literal"
    keys = set(re.findall(r'^\s*"([a-z_]+)":', m.group(1), re.M))
    assert keys, "no keys found in the `last` block"

    prompt = brain.SYSTEM_PROMPT
    # control / head / level predate the payload contract and are named in the
    # prose as the previous prescription's own fields rather than as key spellings.
    obvious = {"control", "head", "level"}
    missing = sorted(k for k in keys - obvious if k not in prompt)
    assert not missing, (
        "these reach the model with nothing telling it what they mean: %s" % missing)
    return "%d keys on last_prescription, %d explained by name" % (
        len(keys), len(keys - obvious))


def test_no_window_key_reaches_the_model_unexplained():
    """The same rule as above, one block over, where it mattered more.

    `window` is derive.window_summary's dict, dumped whole. The coverage pair was
    the case worth pinning: covered_s and span_s decide whether in_band_pct comes
    back a percentage or null, and the model was reading that null with nothing
    telling it the difference between "not enough history" and "your band missed" -
    opposite conclusions from the same field. n_rows, interventions and
    counter_reset arrived undocumented beside them.

    Built from a real window rather than a hand-written key list, so a field derive
    starts emitting fails here instead of being guessed at.
    """
    ts = 1754300000
    rows = [{"recv_ts": ts + i * 60, "temp_c": 24.0, "rh_pct": 60.0,
             "co2_ppm": 800.0, "uptime_ms": 100000 + i * 60000} for i in range(5)]
    window = derive.window_summary(rows, [Setpoint(key="vpd_kpa", lo=0.8, hi=1.2)])

    def walk(node):
        if isinstance(node, dict):
            out = set(node)
            for v in node.values():
                out |= walk(v)
            return out
        if isinstance(node, list):
            out = set()
            for v in node:
                out |= walk(v)
            return out
        return set()

    keys = walk(window)
    assert "covered_s" in keys and "in_band_pct" in keys, (
        "this test is measuring the wrong dict: %s" % sorted(keys))

    prompt = brain.SYSTEM_PROMPT
    # Metric names are the vocabulary, pinned by the tests above. Timestamps and the
    # min/mean/max triple carry their meaning in their names and in the prose that
    # already tells the model to use the timestamps it is given.
    obvious = set(schema.MetricKey.__args__) | {
        "start_ts", "end_ts", "min", "mean", "max", "first", "last",
        "lo", "hi", "n", "metrics", "actuators", "intent",
    }
    missing = sorted(k for k in keys - obvious if k not in prompt)
    assert not missing, (
        "these reach the model with nothing telling it what they mean: %s" % missing)
    return "%d window keys, %d explained by name" % (len(keys), len(keys - obvious))


def test_a_band_nothing_can_satisfy_is_dropped():
    """lo above hi is not a strict band, it is a target that can never be hit.

    Left in, it is worse than a bad band: derive scores it 0.0% for a window that
    may have been perfect, the tile holds amber until the prescription is replaced,
    and the next cycle is handed that zero as evidence its own band failed. One
    transposition, and every reader downstream agrees on the wrong answer.

    Dropped and not swapped, because lo=30/hi=20 does not say which bound the model
    meant. And dropped alone: the sane bands in the same reply survive, so a slip on
    one metric does not cost a grower the whole prescription.
    """
    readable = frozenset(["vpd_kpa", "air_c"])

    crossed = brain._setpoints([{"key": "vpd_kpa", "lo": 1.2, "hi": 0.8}], readable)
    assert crossed == [], "a band nothing can be inside reached the wire: %s" % crossed

    # A point target is degenerate but satisfiable, and one-sided bands have no
    # second bound to cross. Neither is this test's business and both must live.
    for raw in ({"key": "vpd_kpa", "lo": 1.0, "hi": 1.0},
                {"key": "air_c", "lo": 18.0},
                {"key": "air_c", "hi": 28.0}):
        assert brain._setpoints([raw], readable), "wrongly dropped %s" % raw

    mixed = brain._setpoints(
        [{"key": "vpd_kpa", "lo": 1.2, "hi": 0.8},
         {"key": "air_c", "lo": 18.0, "hi": 28.0}], readable)
    assert [s.key for s in mixed] == ["air_c"], (
        "one crossed band should not take a good one with it: %s" % mixed)

    # The firmware refuses the same shape, because the server gate cannot reach a
    # prescription already stored from an older run and replayed by id.
    plantrx = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "src", "plantrx.cpp")
    with open(plantrx, encoding="utf-8") as fh:
        body = fh.read()
    assert "lo > hi) continue" in body, (
        "src/plantrx.cpp accepts a crossed band the server now refuses to send")
    return "crossed dropped, %d degenerate kept, firmware agrees" % 3


if __name__ == "__main__":
    for fn in (test_a_band_on_a_reading_metric_survives,
               test_a_band_on_a_silent_metric_is_dropped,
               test_an_empty_vocabulary_drops_nothing,
               test_the_two_gates_do_not_interfere,
               test_every_band_dropped_is_still_a_prescription,
               test_the_prompt_names_the_readable_metrics,
               test_from_derive_end_to_end,
               test_the_dead_sensors_are_out_of_the_vocabulary_entirely,
               test_the_firmware_knows_the_same_five_metrics,
               test_the_prompt_quotes_the_panels_real_fallback_bands,
               test_no_key_reaches_the_model_unexplained,
               test_a_band_nothing_can_satisfy_is_dropped,
               test_no_window_key_reaches_the_model_unexplained):
        print("%-52s %s" % (fn.__name__, fn()))
    print("metrics %s" % sorted(schema.MetricKey.__args__))
    print("OK")
