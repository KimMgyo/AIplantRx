"""edges and allstops: what the panel did between two polls.

    cd server && python tests/test_intervention_log.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives.

actuator_intent is a snapshot, taken once per poll. At the idle cadence that is
once a minute, so a grower who turns the mister on and off again reads identically
in both samples and the server scores that window as though its prescription had
been left alone. 전체 정지 was the worst of it: it is the one control that means
somebody disagreed with the prescription, and taken back inside its undo window it
left the switches exactly as the server last saw them. Two monotonic counts since
boot close that hole - the movement between two polls is the difference between
them, never a sum.

The four cases below are the four different things a pair of counts can mean, and
three of them used to look like the fourth:

  quiet     the counters sat still: nobody touched the panel          -> 0
  pressed   two 전체 정지 and their undos, with actuator_intent
            byte-identical on every poll of the window                -> 2
  rebooted  the counters went backwards, because they restart at 0    -> a floor
  silent    a firmware that does not send the fields at all           -> None

`quiet` and `silent` are the pair that matters most, and the reason edges and
allstops default to None where uptime_ms defaults to 0. A count of 0 because the
device said 0 and a count of 0 because the device said nothing are different
facts. Zero edges is the commonest true answer there is, so 0 cannot also carry
"not reported" the way it can for a clock no board can read as 0 - defaulting
these to 0 would tell the model nobody touched the panel on the authority of a
field that never arrived.

`quiet` also fixes the shape of the arithmetic. Its counters sit at 4 and 1, not
at 0: a summing implementation reports five presses for an untouched hour and a
last-value implementation reports one, and both pass against a baseline of zero.

Every case is asserted on what the model was handed - the last_prescription block
and the window block brain._payload dumps verbatim - and not on what
derive.interventions returned. A helper that is right and unwired is the whole
class of defect these counts exist to remove.

T0 is fixed rather than wall-clock, the same choice tests/test_device_state.py
makes, and safe for the same reason: store._prune_old runs on real time and is
called one write in store.PRUNE_EVERY, which this file never reaches.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "interventions.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is not what this file is about

from fastapi.testclient import TestClient  # noqa: E402

from app import brain, main, scheduler, store  # noqa: E402
from app.scheduler import MAX_INTERVAL_S, MIN_INTERVAL_S  # noqa: E402
from app.schema import Control, Setpoint  # noqa: E402

T0 = 1754400000
CLOCK = {"t": T0}
scheduler.now = lambda: CLOCK["t"]
CLIENT = TestClient(main.app)

# Spacing between the two diagnoses, past the call floor so ask_now is granted.
# Derived from the floor for the reason tests/test_device_state.py gives: every
# intermediate poll here must fall inside it, or a third diagnosis lands and the
# window under test is not the one the timeline built.
GAP = MIN_INTERVAL_S + 60

# A day of uptime, so main._restarted's "uptime shorter than the window" witness
# never fires by accident and every restart in this file is one a test asked for.
UP0 = 86_400_000

# 26.0 degC at 55% RH derives to 1.513 kPa, inside this band on every row, so no
# escalation spends a call the timeline did not ask for.
HELD = (1.4, 1.6)

# Absent is not a value. A firmware that predates these counters omits them, and
# spelling that as 0 in the body would test the one device this file is about
# telling apart from a panel nobody touched.
_OMIT = object()

# Every poll reports the same switch positions, unchanged, in every test here.
# That is the instrument: the presses the counters report are presses no snapshot
# of this object can see, which is the whole reason the counters are on the wire.
SWITCHES = {"fan": 0, "mist": 0, "pump": 0, "led": 0}


def _body(device, rx_id, uptime_ms, edges, allstops, ask_now):
    body = {
        "device": device,
        "sensors": {"temp_c": 26.0, "rh_pct": 55.0, "co2_ppm": 900.0,
                    "leaf_max_c": 31.9},
        "links": {"node_online": True, "cam_online": True, "rgb_live": True,
                  "thermal_live": True, "thermal_fps": 4.0, "wifi_rssi": -55},
        "actuator_intent": dict(SWITCHES),
        "auto": True,
        "ask_now": ask_now,
    }
    if rx_id is not _OMIT:
        body["rx_id"] = rx_id
    if uptime_ms is not _OMIT:
        body["uptime_ms"] = uptime_ms
    if edges is not _OMIT:
        body["edges"] = edges
    if allstops is not _OMIT:
        body["allstops"] = allstops
    return body


def _poll(device, rx_id=_OMIT, uptime_ms=_OMIT, edges=_OMIT, allstops=_OMIT,
          ask_now=False, frames=False):
    """One poll at the current clock.

    `frames` uploads a pair first. Without them want_frame defers the diagnosis
    by a poll and the model is never called, which reads as a failed assertion
    about intervention counts rather than as a missing image.
    """
    if frames:
        for kind in ("rgb", "thermal"):
            r = CLIENT.post("/v1/frame", content=b"\xff\xd8\xff\xd9" * 64,
                            headers={"X-Device": device, "X-Kind": kind})
            assert r.status_code == 200, r.text
    r = CLIENT.post(
        "/v1/telemetry",
        json=_body(device, rx_id, uptime_ms, edges, allstops, ask_now),
    )
    assert r.status_code == 200, r.text
    return r.json()


class _Model:
    """A brain that answers with one band and keeps every input it was given.

    The inputs are the observable. Nothing about the answer varies here - what
    varies is the window and the last_prescription block the second call is
    handed, which is where the counts arrive.
    """

    def __init__(self):
        self.seen = []

    async def diagnose(self, inp):
        self.seen.append(inp)
        return brain.BrainOutput(
            diagnosis_ko="현재 생육 조건은 안정적입니다.",
            head_ko="생육 조건 양호",
            level="ok",
            evidence=["vpd_kpa"],
            deciding="",
            confidence=0.7,
            control=Control(setpoints=[Setpoint(key="vpd_kpa",
                                                lo=HELD[0], hi=HELD[1])]),
            # The ceiling, so no intermediate poll trips the scheduled wake and
            # spends a call this file did not ask for.
            wake_after_s=MAX_INTERVAL_S,
            wake_when=[],
            notes_ko="",
        )


def _install(model):
    keep = (brain.is_configured, brain.diagnose)
    brain.is_configured = lambda: True
    brain.diagnose = model.diagnose
    return keep


def _restore(keep):
    brain.is_configured, brain.diagnose = keep


def _second_diagnosis(model):
    """The call the timeline was built for, and proof it is the only extra one.

    A third diagnosis would mean an intermediate poll got past the floor, and the
    window the assertions read would be a shorter one nobody designed.
    """
    assert len(model.seen) == 2, (
        "%d diagnoses, not 2: the timeline slipped past the call floor"
        % len(model.seen))
    return model.seen[1].last, model.seen[1].window


def _unmoved(window):
    """True where the switch snapshots say nothing happened all window.

    Every test here holds SWITCHES constant, so this is always true and is
    asserted rather than assumed: it is the claim the counts are measured
    against. An intent block that reported movement would mean the timeline had
    accidentally built a window where the snapshot alone was enough.
    """
    return all(
        e["held_s"] == 0.0 and e["duty_s"] == 0.0 and e["last"] == 0
        for e in window["intent"].values()
    )


# --------------------------------------------------------------------------
# 1. Nothing moved, and the counters were not sitting at zero when it did not
# --------------------------------------------------------------------------


def test_still_counters_are_no_intervention_and_not_no_report():
    """The negative control, and the arithmetic check.

    Every poll reports the same 4 edges and 1 all-stop - a panel that was touched
    before this prescription was issued and not since. The movement is 0 because
    the difference is 0, which is what the model must be told. A sum of the values
    would say five presses and the last value would say one, and a baseline of
    zero would have hidden both.
    """
    dev = "AA:BB:CC:DD:EE:21"
    model = _Model()
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, edges=4, allstops=1, frames=True)["rx_id"]
        for k in (1, 2, 3):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx, uptime_ms=UP0 + 60_000 * k, edges=4, allstops=1)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, uptime_ms=UP0 + GAP * 1000, edges=4, allstops=1,
              ask_now=True, frames=True)
    finally:
        _restore(keep)

    last, window = _second_diagnosis(model)
    assert last["grower_all_stops_in_window"] == 0, (
        "a still counter was not read as movement of zero: %r"
        % last["grower_all_stops_in_window"])
    assert last["grower_switch_edges_in_window"] == 0, (
        "edges summed or read as a level instead of differenced: %r"
        % last["grower_switch_edges_in_window"])

    iv = window["interventions"]
    assert iv == {"edges": 0, "allstops": 0, "counter_reset": False}, iv
    # 0 and None are the two answers this whole file is about keeping apart, and
    # a bare == 0 would pass on either in a language where None is falsy.
    assert iv["allstops"] is not None and iv["edges"] is not None, iv
    assert _unmoved(window), window["intent"]
    return "counters held at 4/1 -> edges=%d allstops=%d" % (
        iv["edges"], iv["allstops"])


# --------------------------------------------------------------------------
# 2. Two 전체 정지 the switch snapshots cannot see
# --------------------------------------------------------------------------


def test_two_all_stops_inside_the_window_reach_the_model():
    """The case the counters were added for.

    Two all-stops, each switching four devices off and each undone before the
    next poll, so every poll of the window reports byte-identical switch
    positions. The counters climb 4 -> 8 -> 12 -> 16 and 1 -> 2 -> 2 -> 3: the
    undo at T0+120 adds four edges and does not decrement the all-stop, because
    the press happened.

    The model is being asked to judge whether its bands held over an hour it was
    overruled in twice. Before this it was handed a window that said the switches
    never moved, which was true of every snapshot and false of the hour.
    """
    dev = "AA:BB:CC:DD:EE:22"
    model = _Model()
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, edges=4, allstops=1, frames=True)["rx_id"]
        # (edges, allstops): first all-stop, its undo, then the second all-stop.
        for k, (ed, al) in enumerate(((8, 2), (12, 2), (16, 3)), start=1):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx, uptime_ms=UP0 + 60_000 * k, edges=ed, allstops=al)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, uptime_ms=UP0 + GAP * 1000, edges=16, allstops=3,
              ask_now=True, frames=True)
    finally:
        _restore(keep)

    last, window = _second_diagnosis(model)
    assert last["grower_all_stops_in_window"] == 2, (
        "the presses did not reach the model: %r"
        % last["grower_all_stops_in_window"])
    assert last["grower_switch_edges_in_window"] == 12, (
        "16 - 4 is the movement; got %r"
        % last["grower_switch_edges_in_window"])

    iv = window["interventions"]
    assert iv == {"edges": 12, "allstops": 2, "counter_reset": False}, iv
    # The point of the whole exercise: the block that reports switch state says
    # nothing happened, and it is not wrong - it never saw either press.
    assert _unmoved(window), (
        "the timeline moved a switch across a poll boundary, so the snapshot "
        "could have caught this on its own: %s" % window["intent"])
    assert window["metrics"]["vpd_kpa"]["in_band_pct"] == 100.0, (
        "the band has to read as held, or the model is not being handed a "
        "contradiction: %s" % window["metrics"]["vpd_kpa"])

    # The last mile. brain._payload is the text the model actually reads and it
    # dumps `last` verbatim, so a key that reaches the BrainInput and stops there
    # is exactly the defect this file is closing - a count nothing was told.
    payload = brain._payload(model.seen[1])
    assert '"grower_all_stops_in_window": 2' in payload, payload
    assert '"grower_switch_edges_in_window": 12' in payload, payload
    assert '"allstops": 2' in payload, payload
    return "band held 100%% with %d all-stops and %d edges inside it" % (
        iv["allstops"], iv["edges"])


# --------------------------------------------------------------------------
# 3. A window the counters restarted in
# --------------------------------------------------------------------------


def test_counters_going_backwards_are_a_floor_not_a_negative():
    """Both counters restart at 0 across a reboot, so the window's ends are the
    wrong two numbers to subtract.

    One all-stop before the board went down (5 -> 6) and one after it came back
    (0 -> 1). End to end that is 1 - 5 = -4 presses, and summing the column is
    13. Neither is a number of times anybody pressed anything.

    The tenure count is 2 and it is a floor, not a total: whatever was pressed
    between the last poll before the reboot and the reboot itself was never
    reported and is not invented here. device_restarted_in_window sits beside it
    saying so.

    The window block reports 1, not 2, and the difference is the point. It is
    measured on the rows that survived main._restart_at, because a reading from
    before a reboot is not evidence about bands the board dropped on the way
    down - but a press before that reboot was still a press against these bands,
    so the tenure count keeps it.
    """
    dev = "AA:BB:CC:DD:EE:23"
    model = _Model()
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, edges=40, allstops=5, frames=True)["rx_id"]
        CLOCK["t"] = T0 + 60
        _poll(dev, rx_id=rx, uptime_ms=UP0 + 60_000, edges=44, allstops=6)

        # Down and back up. rx_id "none" is the panel saying it lost its state.
        CLOCK["t"] = T0 + GAP - 120
        _poll(dev, rx_id="none", uptime_ms=5_000, edges=0, allstops=0)
        CLOCK["t"] = T0 + GAP - 60
        _poll(dev, rx_id=rx, uptime_ms=65_000, edges=4, allstops=1)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, uptime_ms=125_000, edges=4, allstops=1,
              ask_now=True, frames=True)
    finally:
        _restore(keep)

    last, window = _second_diagnosis(model)
    assert last["device_restarted_in_window"] is True, last

    tenure = last["grower_all_stops_in_window"]
    assert tenure == 2, (
        "a restarted counter was not walked: %r (end to end gives -4, summing "
        "the column gives 13)" % tenure)
    assert tenure >= 0, "presses cannot be negative: %r" % tenure
    assert last["grower_switch_edges_in_window"] == 8, (
        "4 before the reboot and 4 after it; got %r"
        % last["grower_switch_edges_in_window"])

    iv = window["interventions"]
    assert iv["allstops"] == 1, (
        "the window is measured after the cut and holds one press: %s" % iv)
    assert iv["edges"] == 4, iv
    # The surviving rows climb 0 -> 4 -> 4, so nothing inside them went
    # backwards. The reset happened on a row main._restart_at already removed.
    assert iv["counter_reset"] is False, iv
    assert tenure > iv["allstops"], (
        "the press before the reboot was cut away with the readings: tenure %r "
        "against window %r" % (tenure, iv["allstops"]))
    return "tenure %d presses, window %d, neither negative" % (
        tenure, iv["allstops"])


def test_a_reset_inside_the_window_is_reported_as_a_floor():
    """The same restart with no clock to catch it.

    A firmware that reports the counters but no uptime leaves main._restart_at
    nothing to cut on - it skips rows carrying no clock rather than reading them
    as zero - so the reset stays inside the window and derive has to survive it
    alone. It contributes the value the counter fell to and flags the result,
    which is the difference between an undercount that admits it and a number
    that reads as a total.
    """
    dev = "AA:BB:CC:DD:EE:24"
    model = _Model()
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, edges=40, allstops=5, frames=True)["rx_id"]
        CLOCK["t"] = T0 + 60
        _poll(dev, rx_id=rx, edges=44, allstops=6)
        CLOCK["t"] = T0 + 120
        _poll(dev, rx_id=rx, edges=0, allstops=0)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, edges=4, allstops=1, ask_now=True, frames=True)
    finally:
        _restore(keep)

    last, window = _second_diagnosis(model)
    assert last["device_restarted_in_window"] is None, (
        "a board with no clock cannot be answered, only admitted: %r"
        % last["device_restarted_in_window"])
    assert window["start_ts"] == T0, (
        "nothing should have been cut without a clock to cut on: %s" % window)

    iv = window["interventions"]
    assert iv["counter_reset"] is True, (
        "a counter went backwards inside the window and was not flagged: %s" % iv)
    assert iv["allstops"] == 2, (
        "1 before the reset and 1 after it, floored; got %s" % iv)
    assert iv["edges"] == 8, iv
    assert last["grower_all_stops_in_window"] == 2, last
    return "reset flagged, %d presses as a floor" % iv["allstops"]


# --------------------------------------------------------------------------
# 4. A firmware that never sent them
# --------------------------------------------------------------------------


def test_an_unreported_counter_is_admitted_not_answered():
    """The half of case 1 that a default of 0 would have made identical to it.

    This board omits both fields on every poll, so nothing is known about what
    the grower did. 0 would put "nobody touched the panel" in front of the model
    on the strength of a field that never arrived, and the model would then
    credit its own bands with an hour it cannot account for. None says the
    question has no answer here, which is all the payload supports.
    """
    dev = "AA:BB:CC:DD:EE:25"
    model = _Model()
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, frames=True)["rx_id"]
        for k in (1, 2, 3):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx, uptime_ms=UP0 + 60_000 * k)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, uptime_ms=UP0 + GAP * 1000, ask_now=True, frames=True)
    finally:
        _restore(keep)

    last, window = _second_diagnosis(model)
    assert last["grower_all_stops_in_window"] is None, (
        "an unreported counter was answered instead of admitted: %r"
        % last["grower_all_stops_in_window"])
    assert last["grower_switch_edges_in_window"] is None, (
        "an unreported counter was answered instead of admitted: %r"
        % last["grower_switch_edges_in_window"])

    iv = window["interventions"]
    assert iv == {"edges": None, "allstops": None, "counter_reset": False}, iv
    # The row went through SQLite between the poll and the window, and a NOT NULL
    # DEFAULT 0 column would have turned the absence into a count on the way.
    stored = store.telemetry_since(dev, T0)
    assert len(stored) == 5, "the timeline did not store what it meant to: %d" % (
        len(stored),)
    assert all(r["edges"] is None and r["allstops"] is None for r in stored), (
        "the store manufactured a count for a poll that carried none: %r"
        % [(r["edges"], r["allstops"]) for r in stored])
    return "5 rows reported nothing -> edges=%r allstops=%r" % (
        iv["edges"], iv["allstops"])


if __name__ == "__main__":
    print("quiet     %s" % test_still_counters_are_no_intervention_and_not_no_report())
    print("pressed   %s" % test_two_all_stops_inside_the_window_reach_the_model())
    print("rebooted  %s" % test_counters_going_backwards_are_a_floor_not_a_negative())
    print("noclock   %s" % test_a_reset_inside_the_window_is_reported_as_a_floor())
    print("silent    %s" % test_an_unreported_counter_is_admitted_not_answered())
    print("OK")
