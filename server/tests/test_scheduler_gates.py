"""The gates: what stops a broken install from spending money, and what stops a
frozen reading from being stored as a measurement.

    cd server && python tests/test_scheduler_gates.py

Plain asserts and no test framework, like every other file in here.

Four gates, and all four failed the same way - the code did not do what its own
comment said it did, and nothing raised.

scheduler.decide answered "first prescription" *above* the min-interval floor
and the daily budget, so the one device that is in that branch on every single
poll - never diagnosed, because there is no key or because every call fails -
was the one device exempt from both. Moving the return below them is not enough
on its own: the floor was measured from the last prescription's issued_ts, which
a never-diagnosed device does not have, and the budget counted only calls that
succeeded, which a never-diagnosed device has none of. Both gates were
structurally incapable of stopping the case they were skipped for, so this file
checks the floor against the attempt ledger and checks the budget on the
first-prescription path specifically.

telemetry_is_sane promised in its docstring to reject a row from a device that
has not heard from its sensor node, and then never looked at links at all.
src/sensornode.cpp holds the last packet forever, so the row that arrives twenty
minutes after the node died carries five real-looking floats, and every one of
them was averaged into the window the panel draws.

derive.enrich computed leaf_air_dt_c from a thermal peak that src/thermal.cpp
never resets, with no reference to links.thermal_live - so the panel could draw
OFFLINE over the thermal camera and a live-looking delta underneath it.

And mode was inherited: main returned prev.model_copy() verbatim when the model
failed, carrying a mode decided up to six hours ago against a switch the grower
may have flipped since. src/ui/page_auto.cpp draws that disagreement as a chip.
"""
import inspect
import math
import os
import re
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "gates.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is not what this file is about

from fastapi.testclient import TestClient  # noqa: E402

from app import brain, derive, main, scheduler, store  # noqa: E402
from app.scheduler import DAILY_CALL_BUDGET, MIN_INTERVAL_S, WakeSpec  # noqa: E402
from app.schema import Control, Links, Sensors, Setpoint, Telemetry  # noqa: E402

T0 = 1754300000
CLOCK = {"t": T0}
scheduler.now = lambda: CLOCK["t"]
CLIENT = TestClient(main.app)


def _decide(**kw):
    """decide() with everything neutral, so each case names only what it varies.

    Neutral is a never-diagnosed device with a key, no calls on the ledger and
    nothing asked for: the state a freshly flashed board is in on its first poll.
    """
    args = dict(
        now_ts=T0,
        last_rx_ts=None,
        last_call_ts=None,
        wake=None,
        rows=[],
        calls_today=0,
        ask_now=False,
        have_prescription=False,
        model_ready=True,
    )
    args.update(kw)
    return scheduler.decide(**args)


# Every reason decide() has for saying yes, as the arguments that produce it.
# The floor and the budget have to refuse all four; a fifth affirmative return
# added above them would not be caught by a test that only knows about the four,
# which is why the reason string is asserted too - a new yes reaching these
# assertions reads as the wrong reason rather than as a pass.
_HELD_ROWS = [
    {"recv_ts": T0 - 900, "vpd_kpa": 2.1},
    {"recv_ts": T0 - 300, "vpd_kpa": 2.4},
]
_WHEN = [{"metric": "vpd_kpa", "op": "gt", "value": 1.8, "for_s": 600}]

_AFFIRMATIVE = (
    ("first prescription", dict(have_prescription=False)),
    ("user requested", dict(have_prescription=True, ask_now=True)),
    ("scheduled", dict(have_prescription=True, last_rx_ts=T0 - 1000,
                       wake=WakeSpec(after_s=300, when=[]))),
    ("vpd_kpa gt 1.8", dict(have_prescription=True, last_rx_ts=T0,
                            wake=WakeSpec(after_s=6 * 3600, when=_WHEN),
                            rows=_HELD_ROWS)),
)


# --------------------------------------------------------------------------
# 1. The floor and the budget, on every affirmative path
# --------------------------------------------------------------------------


def test_every_yes_is_under_both_gates():
    """The invariant, not the instance. An exemption here is not a special case,
    it is a state a broken device sits in forever."""
    seen = []
    for reason, kw in _AFFIRMATIVE:
        clear = _decide(**kw)
        assert clear.should_call, "%s: the case does not even fire" % reason
        assert clear.reason == reason, (clear.reason, reason)

        floored = _decide(last_call_ts=T0 - (MIN_INTERVAL_S - 1), **kw)
        assert not floored.should_call, "%s escaped the min-interval floor" % reason
        assert floored.reason == "min interval", floored.reason

        broke = _decide(calls_today=DAILY_CALL_BUDGET,
                        last_call_ts=T0 - MIN_INTERVAL_S, **kw)
        assert not broke.should_call, "%s escaped the daily budget" % reason
        assert broke.reason == "daily budget", broke.reason
        seen.append(reason)
    return "floor + budget over %s" % seen


def test_never_diagnosed_is_refused_inside_the_floor():
    """The exact bug. A device with no prescription attempted a call at T; at
    T+299 it must be refused, because "first prescription" is still the reason
    it will give on every poll until one arrives."""
    d = _decide(have_prescription=False, last_call_ts=T0 - (MIN_INTERVAL_S - 1))
    assert not d.should_call and d.reason == "min interval", d
    # One second before the boundary is inside; the boundary itself is not.
    edge = _decide(have_prescription=False, last_call_ts=T0 - MIN_INTERVAL_S)
    assert edge.should_call and edge.reason == "first prescription", edge
    return "refused at %ds, granted at %ds" % (MIN_INTERVAL_S - 1, MIN_INTERVAL_S)


def test_never_diagnosed_is_granted_once_the_floor_has_passed():
    """The floor is a rate limit, not a lockout: a board that has never been
    diagnosed still gets diagnosed, once per MIN_INTERVAL_S until it is."""
    d = _decide(have_prescription=False, last_call_ts=T0 - (MIN_INTERVAL_S + 1))
    assert d.should_call and d.reason == "first prescription", d
    fresh = _decide(have_prescription=False, last_call_ts=None)
    assert fresh.should_call and fresh.reason == "first prescription", fresh
    return "granted with no ledger and %ds after one" % (MIN_INTERVAL_S + 1)


def test_the_budget_refuses_the_first_prescription_path():
    """The path the budget check used to sit below. Asserted on its own rather
    than only inside the table above, because this is the combination that cost
    the money: no prescription, no successes, and a full day of attempts."""
    over = _decide(have_prescription=False,
                   last_call_ts=T0 - MIN_INTERVAL_S,
                   calls_today=DAILY_CALL_BUDGET)
    assert not over.should_call and over.reason == "daily budget", over
    under = _decide(have_prescription=False,
                    last_call_ts=T0 - MIN_INTERVAL_S,
                    calls_today=DAILY_CALL_BUDGET - 1)
    assert under.should_call and under.reason == "first prescription", under
    return "refused at %d/%d, granted at %d" % (
        DAILY_CALL_BUDGET, DAILY_CALL_BUDGET, DAILY_CALL_BUDGET - 1)


def test_no_key_is_not_a_reason_to_do_work():
    """A server that cannot call the model must not answer yes to calling it.
    Every consequence of a yes - fetch frames, poll at 3s, defer the answer - is
    work done for a call that will never be made, repeated forever."""
    d = _decide(have_prescription=False, model_ready=False)
    assert not d.should_call and d.reason == "no model configured", d
    assert not scheduler.want_frame(decision=d, frame_ts=None, now_ts=T0), (
        "a keyless server asked for a 60KB upload it has no use for")
    return "no key -> %r, and no frame request" % d.reason


# --------------------------------------------------------------------------
# 2. The attempt ledger
# --------------------------------------------------------------------------


class _Boom(brain.BrainError):
    pass


def _telemetry_body(device, auto=True, node=True, thermal=True, peak=31.9):
    return {
        "device": device,
        "uptime_ms": 60000,
        "sensors": {"temp_c": 26.0, "rh_pct": 55.0, "co2_ppm": 900.0,
                    "leaf_max_c": peak},
        "links": {"node_online": node, "cam_online": True, "rgb_live": True,
                  "thermal_live": thermal, "thermal_fps": 4.0, "wifi_rssi": -55},
        "actuator_intent": {"fan": 0, "mist": 0},
        "auto": auto,
    }


def _upload_frames(device, thermal=True):
    """The frames a device would actually push. thermal=False is the board with
    its thermal link down: src/plantrx.cpp uploads RGB only, and has_thermal is
    read off what the store holds rather than off a scalar that outlived it.
    """
    kinds = ("rgb", "thermal") if thermal else ("rgb",)
    for kind in kinds:
        r = CLIENT.post("/v1/frame", content=b"\xff\xd8\xff\xd9" * 64,
                        headers={"X-Device": device, "X-Kind": kind})
        assert r.status_code == 200, r.text


def _poll(device, **kw):
    r = CLIENT.post("/v1/telemetry", json=_telemetry_body(device, **kw))
    assert r.status_code == 200, r.text
    return r.json()


def _with_brain(configured, diagnose):
    """Swap brain.is_configured / brain.diagnose, returning the pair to restore."""
    keep = (brain.is_configured, brain.diagnose)
    brain.is_configured, brain.diagnose = configured, diagnose
    return keep


def _ok_output(wake_after_s=300):
    return brain.BrainOutput(
        diagnosis_ko="현재 생육 조건은 안정적입니다.",
        head_ko="생육 조건 양호",
        level="ok",
        evidence=["vpd_kpa"],
        deciding="",
        confidence=0.7,
        control=Control(setpoints=[Setpoint(key="vpd_kpa", lo=0.8, hi=1.2)]),
        wake_after_s=wake_after_s,
        wake_when=[],
        notes_ko="",
    )


def test_a_failed_call_is_counted():
    """The counter the budget reads, not the log line beside it.

    record_llm_call used to run only after a prescription was built, so
    calls_today stayed at 0 for exactly as long as every call failed - and the
    floor, measured from a prescription that never arrived, stayed vacuous. The
    second poll is the whole point: with the attempt on the ledger it is refused,
    and without it the device would call again immediately and forever.
    """
    device = "AA:00:00:00:0A:01"
    CLOCK["t"] = T0

    async def boom(inp):
        raise _Boom("no model today")

    keep = _with_brain(lambda: True, boom)
    try:
        _upload_frames(device)
        first = _poll(device)
        assert first["rx_id"] == "none", first["rx_id"]
        spent = store.llm_calls_since(device, 0)
        assert spent == 1, "a call that failed was not counted: %d" % spent

        CLOCK["t"] = T0 + 5
        _poll(device)
        again = store.llm_calls_since(device, 0)
        assert again == 1, "the floor did not hold a failing device: %d calls" % again

        CLOCK["t"] = T0 + MIN_INTERVAL_S
        _upload_frames(device)
        _poll(device)
        third = store.llm_calls_since(device, 0)
        assert third == 2, "the floor never lifts: %d calls" % third
    finally:
        brain.is_configured, brain.diagnose = keep
    return "1 attempt, held for %ds, then 2" % MIN_INTERVAL_S


# --------------------------------------------------------------------------
# 3. telemetry_is_sane
# --------------------------------------------------------------------------


def _t(node_online, thermal_live, **sensors):
    return Telemetry(
        device="AA:BB:CC:DD:EE:FF",
        sensors=Sensors(**sensors),
        links=Links(node_online=node_online, thermal_live=thermal_live),
    )


def test_a_frozen_row_is_not_a_measurement():
    """The exact case: the node is gone and the readings are still there.

    src/sensornode.cpp overwrites its five floats on receipt and clears them
    never, so the CO2 in this row was true fifteen minutes ago and is on the wire
    as though it were true now. The tiles on the panel already blank at 17s; it
    is the stored row that goes on drawing a flat hour underneath them.
    """
    frozen = _t(False, True, co2_ppm=1237.0, temp_c=27.3, rh_pct=45.0, leaf_max_c=31.9)
    assert not scheduler.telemetry_is_sane(frozen), "a frozen row was stored"
    # node_age_ms is not what this keys on and must not rescue the row: the
    # device reports 0 both for a packet that arrived this instant and for a node
    # that has never spoken, so the age cannot tell them apart.
    frozen.links.node_age_ms = 0
    assert not scheduler.telemetry_is_sane(frozen), "node_age_ms overrode node_online"
    return "node offline + populated sensors -> rejected"


def test_a_normal_row_is_stored():
    """The negative control. A gate that rejects everything is not a gate."""
    good = _t(True, True, co2_ppm=900.0, temp_c=26.0, rh_pct=55.0, leaf_max_c=28.4)
    assert scheduler.telemetry_is_sane(good), "a healthy row was dropped"
    # A node that has never spoken sends nulls, not frozen floats. Its thermal
    # peak is a real current measurement and the row is worth keeping - which is
    # what separates "never had a node" from "the node just died".
    never = _t(False, True, leaf_max_c=28.4)
    assert scheduler.telemetry_is_sane(never), "a live peak with no node was dropped"
    # And with nothing live at all there is nothing to record.
    nothing = _t(False, False, leaf_max_c=28.4)
    assert not scheduler.telemetry_is_sane(nothing), "a row of nothing was stored"
    assert not scheduler.telemetry_is_sane(_t(True, True)), "an empty row was stored"
    return "healthy kept, never-had-a-node kept on its peak, empty dropped"


# --------------------------------------------------------------------------
# 4. derive against a dead thermal link
# --------------------------------------------------------------------------


def test_a_dead_thermal_link_derives_nothing():
    """src/thermal.cpp's running maximum is never reset, so the peak on the wire
    outlives the camera. thermal_live is the only thing that can tell the
    difference, and enrich never asked."""
    live = derive.enrich(_t(True, True, temp_c=26.0, rh_pct=55.0, leaf_max_c=31.9))
    assert live["leaf_air_dt_c"] == 5.9, live
    dead = derive.enrich(_t(True, False, temp_c=26.0, rh_pct=55.0, leaf_max_c=31.9))
    assert dead["leaf_air_dt_c"] is None, dead
    # VPD is air-only and must survive a dead thermal camera untouched.
    assert dead["vpd_kpa"] == live["vpd_kpa"] and dead["vpd_kpa"] is not None, dead

    # The peak itself, not only the delta: render's 장면최고 chip prints the peak,
    # so a peak that reaches `current` through the raw sensor dump is the same
    # lie by a different door.
    cur = derive.current_readings(_t(True, False, temp_c=26.0, leaf_max_c=31.9))
    assert cur["leaf_max_c"] is None, cur
    assert cur["temp_c"] == 26.0, "a dead thermal link took the air sensor with it"
    return "delta and peak both None; vpd_kpa %s unaffected" % live["vpd_kpa"]


def test_a_dead_thermal_link_reaches_no_reader():
    """None type-checks everywhere; that is not the same as being handled.

    The refusal is followed to the wire through the real endpoint, because every
    reader of the peak is downstream of a different one of its two spellings:
    the window row reads the derived delta, the chip reads the peak itself, and
    the prompt reads both. A card that still carried either would be a
    live-looking thermal reading under an OFFLINE camera.

    The has_thermal badge is asserted here too and is no longer one of those
    readers: it says a thermal image was held when the judgment was made, not
    that a scalar outlived its camera. So the dead branch uploads no thermal
    frame - which is what the device does when thermal_live() is false - and the
    branches must run dead-first, because a frame the live branch pushed would
    still be the newest one in the store when the dead branch looked.
    """
    device = "AA:00:00:00:0A:03"
    base = T0 + 20000

    async def ok(inp):
        out = _ok_output()
        out.evidence = ["leaf_air_dt_c", "vpd_kpa"]
        return out

    keep = _with_brain(lambda: True, ok)
    try:
        cards = {}
        # Dead first, and not for tidiness: see the docstring.
        for live in (False, True):
            CLOCK["t"] = base + (0 if not live else 2 * (MIN_INTERVAL_S + 10))
            _upload_frames(device, thermal=live)
            _poll(device, thermal=live)
            # A second row so the window has a hold to integrate over, and a
            # diagnosis past the floor to draw it.
            CLOCK["t"] += MIN_INTERVAL_S + 10
            _upload_frames(device, thermal=live)
            cards[live] = _poll(device, thermal=live)["display"]
    finally:
        brain.is_configured, brain.diagnose = keep

    from app import render  # noqa: E402  - only this test needs the label tables
    label = render._METRICS["leaf_air_dt_c"].label
    chip_ko = render._CHIP_FMT["leaf_air_dt_c"][1].split()[0]

    for live, card in cards.items():
        rows = [r["label"] for r in card["window"]]
        chips = [c["text"] for c in card["judgments"][0]["chips"]]
        badge = card["judgments"][0]["has_thermal"]
        assert ("VPD" in rows) is True, "the control metric is missing too: %s" % rows
        # The dangerous direction, and the one this case is named for: a dead link
        # must not put a 장면최고차 row on the panel. Still strict.
        if not live:
            assert label not in rows, "window row %r with a dead thermal link" % rows
        else:
            # Live, and the row is absent anyway - not because the link failed but
            # because render._ORDER ranks leaf_air_dt_c last and the table has four
            # cells for five metrics, so the hint yields its slot to 습도. Asserted
            # rather than skipped: if the row DOES come back, either the cap or the
            # order moved and this test's dead-link claim above is no longer being
            # measured against a table that could have shown it.
            assert len(rows) == 4 and label not in rows, (
                "leaf_air_dt_c is expected off the full table: %r" % rows)
        # The chip and the badge are the readers that still turn on the link, and
        # they are the two that make a claim about evidence rather than about
        # history.
        assert any(c.startswith(chip_ko) for c in chips) is live, (
            "chip %r with thermal_live=%s" % (chips, live))
        assert badge is live, "has_thermal=%s with a thermal frame held=%s" % (
            badge, live)
    return ("dead: no %s row, no %s chip, badge off; live: chip + badge on, row off "
            "at the four-cell cap" % (label, chip_ko))


# --------------------------------------------------------------------------
# 5. One VPD, in two languages
# --------------------------------------------------------------------------

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_AIJUDGE = os.path.join(_ROOT, "src", "aijudge.cpp")
# The firmware's one absent-reading convention, which aijudge_vpd_kpa returns and
# tests against. It used to be spelled inside aijudge.cpp; it is a header now
# because ten other sites were open-coding the same comparison.
_READING = os.path.join(_ROOT, "include", "reading.h")

# Half of the server's last stored decimal, plus float32 slack that scales with
# the value. The device deliberately does not round: mirroring round(x, 3) was
# measured and makes the two disagree at grid points where a float32 result sits
# a few ulps either side of a .0005 boundary, so the server's own rounding is
# the entire budget.
_TOL_ABS = 5e-4
_TOL_REL = 1e-6


def _f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def _literals(src):
    """Every decimal literal in a lump of C or Python, as a magnitude.

    Unsigned because the two languages spell the same bound differently -
    `temp_c < -60.0f` against `-60.0 <= t` - and it is the number that has to
    agree, not which side of the comparison carries the minus.
    """
    return {float(v) for v in re.findall(r"\d+\.\d+", src)}


def _device_vpd(coeff, temp_c, rh_pct):
    """aijudge_vpd_kpa in float32, statement for statement, with the coefficient
    read out of the firmware rather than written down here twice."""
    t, rh_in = _f32(temp_c), _f32(rh_pct)
    if not math.isfinite(t) or not math.isfinite(rh_in):
        return None
    if not t > -999.0 or not rh_in > -999.0:
        return None
    if t < _f32(-60.0) or t > _f32(100.0):
        return None
    rh = _f32(0.0) if rh_in < 0.0 else (_f32(100.0) if rh_in > _f32(100.0) else rh_in)
    es = _f32(_f32(coeff) * _f32(math.exp(_f32(_f32(17.27) * t / _f32(t + _f32(237.3))))))
    return _f32(es * _f32(1.0 - _f32(rh / _f32(100.0))))


def test_one_vpd_in_two_languages():
    """The panel and the log must not print two different VPDs for one instant.

    This arithmetic existed three times with two coefficients. The firmware is
    now one function and this reads its constants out of the source rather than
    restating them, so moving either side's coefficient fails here rather than
    on a wall in Korea. Absent when the firmware tree is not on disk - the same
    convention test_display_contract uses for the headers.
    """
    src = ""
    if os.path.exists(_AIJUDGE):
        with open(_AIJUDGE, encoding="utf-8") as fh:
            src = fh.read()
    m = re.search(r"float aijudge_vpd_kpa\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    if m is None:
        return "no firmware tree; skipped"
    # The sentinel and the absent test are not in the function and no longer in
    # aijudge.cpp at all: the firmware collected eleven copies of one rule into
    # include/reading.h (READING_NONE + reading_present), so both halves are read
    # from there or the comparison is between two different things. Reading the
    # header rather than the old local spellings is also what makes this test pin
    # the single source instead of pinning one file's habit of restating it.
    hdr = ""
    if os.path.exists(_READING):
        with open(_READING, encoding="utf-8") as fh:
            hdr = fh.read()
    helper = re.search(r"bool reading_present\(float [^\n]+", hdr)
    none_of = re.search(r"READING_NONE\s*=[^\n;]+", hdr)
    assert helper and none_of, \
        "include/reading.h no longer spells reading_present()/READING_NONE"
    body = _literals(m.group(1)) | _literals(helper.group(0)) | _literals(none_of.group(0))
    assert "-1000.0f" in none_of.group(0), (
        "the device sentinel is not below the -999 the readers test for: %s"
        % none_of.group(0))

    # The server's numbers, taken from the code and not from its docstring - and
    # not assuming there is one, or a function without it errors here instead of
    # answering the question this test was asked.
    py = inspect.getsource(derive.vpd_kpa).split('"""')
    server = (_literals(py[0] + "".join(py[2:]))
              | _literals(inspect.getsource(derive._present)))

    for want in (17.27, 237.3, 60.0, 100.0, 999.0):
        assert want in body, "the device dropped %r" % want
        assert want in server, "the server dropped %r" % want
    coeff = sorted(v for v in body if 0.6 <= v <= 0.62)
    assert len(coeff) == 1, "the device has %d Tetens coefficients: %s" % (len(coeff), coeff)
    assert coeff[0] in server, (
        "the two languages carry different Tetens coefficients: device %r, server %s"
        % (coeff[0], sorted(v for v in server if 0.6 <= v <= 0.62)))
    c = coeff[0]

    # Golden pairs across the envelope, 25/60 being the one quoted in both
    # headers. Written down rather than only swept, so a coefficient change
    # names the value it moved instead of only the grid point it failed at.
    for t, h, want in ((5.0, 90.0, 0.087), (18.5, 55.0, 0.958), (25.0, 60.0, 1.267),
                       (25.0, 100.0, 0.0), (35.0, 30.0, 3.936), (45.0, 12.0, 8.432)):
        assert derive.vpd_kpa(t, h) == want, (t, h, derive.vpd_kpa(t, h), want)
        got = _device_vpd(c, t, h)
        assert abs(got - want) <= _TOL_ABS + _TOL_REL * abs(want), (t, h, got, want)

    n, worst = 0, (0.0, None)
    for ti in range(-1200, 2001):          # -60.0 .. 100.0 degC in 0.05 steps
        for hi in range(0, 101, 5):
            t, h = ti / 20.0, float(hi)
            d, s = _device_vpd(c, t, h), derive.vpd_kpa(t, h)
            n += 1
            assert (d is None) == (s is None), (
                "one language has a reading and the other does not at %s/%s: %r %r"
                % (t, h, d, s))
            if s is None:
                continue
            gap = abs(d - s)
            worst = max(worst, (gap, (t, h)))
            assert gap <= _TOL_ABS + _TOL_REL * abs(s), (
                "t=%s rh=%s device %r server %r gap %.2e" % (t, h, d, s, gap))

    # The guards, which is where two ports of one formula actually diverge.
    for name, t, h in (("absent temp", -999.0, 60.0), ("absent rh", 25.0, -999.0),
                       ("below domain", -80.0, 50.0), ("above domain", 150.0, 50.0),
                       ("nan temp", float("nan"), 50.0), ("inf rh", 25.0, float("inf"))):
        assert derive.vpd_kpa(t, h) is None, "%s: the server answered" % name
        assert _device_vpd(c, t, h) is None, "%s: the device answered" % name
    # A capacitive sensor at saturation, and the reading no non-negative
    # sentinel could survive: saturated air really is 0.00 kPa.
    assert derive.vpd_kpa(25.0, 101.0) == derive.vpd_kpa(25.0, 100.0) == 0.0
    assert _device_vpd(c, 25.0, 101.0) == 0.0
    return "coeff %r agrees over %d points, worst %.1e at %s" % (c, n, worst[0], worst[1])


# --------------------------------------------------------------------------
# 6. mode is never inherited
# --------------------------------------------------------------------------


def test_mode_on_the_fallthrough_is_this_polls_switch():
    """The model failed, so the standing prescription is carried verbatim - but
    mode is not the model's to state.

    prev was issued under AI-RX 자동 실행. The grower turns it off and the next poll
    fails to diagnose. Returning prev unchanged tells the panel "auto" against a
    switch reading off, and src/ui/page_auto.cpp draws that as a conflict chip.
    """
    device = "AA:00:00:00:0A:02"
    CLOCK["t"] = T0 + 10000

    async def ok(inp):
        return _ok_output()

    async def boom(inp):
        raise _Boom("model down")

    keep = _with_brain(lambda: True, ok)
    try:
        _upload_frames(device)
        good = _poll(device, auto=True)
        assert good["mode"] == "auto", good["mode"]
        assert good["rx_id"] != "none", "the diagnosis never ran"

        brain.diagnose = boom
        CLOCK["t"] = T0 + 10000 + MIN_INTERVAL_S + 100
        _upload_frames(device)
        after = _poll(device, auto=False)
    finally:
        brain.is_configured, brain.diagnose = keep

    assert after["rx_id"] == good["rx_id"], (
        "this is not the fallthrough path: %r" % after["rx_id"])
    assert after["mode"] == "advisory", (
        "mode was inherited from the prescription, not recomputed: %r" % after["mode"])
    stored = store.latest_prescription(device)
    assert stored is not None and stored.mode == "auto", (
        "the stored prescription was rewritten instead of the response")
    return "carried rx %s, mode auto -> advisory" % after["rx_id"]


if __name__ == "__main__":
    for fn in (test_every_yes_is_under_both_gates,
               test_never_diagnosed_is_refused_inside_the_floor,
               test_never_diagnosed_is_granted_once_the_floor_has_passed,
               test_the_budget_refuses_the_first_prescription_path,
               test_no_key_is_not_a_reason_to_do_work,
               test_a_failed_call_is_counted,
               test_a_frozen_row_is_not_a_measurement,
               test_a_normal_row_is_stored,
               test_a_dead_thermal_link_derives_nothing,
               test_a_dead_thermal_link_reaches_no_reader,
               test_one_vpd_in_two_languages,
               test_mode_on_the_fallthrough_is_this_polls_switch):
        print("%-52s %s" % (fn.__name__, fn()))
    print("gates floor=%ds budget=%d/day; poll idle=%ds active=%ds"
          % (MIN_INTERVAL_S, DAILY_CALL_BUDGET,
             scheduler.POLL_IDLE_S, scheduler.POLL_ACTIVE_S))
    print("OK")
