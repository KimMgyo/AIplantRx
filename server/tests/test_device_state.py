"""rx_id and uptime_ms, read as one signal: what the panel is actually running.

    cd server && python tests/test_device_state.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives.

Both fields used to be written to the telemetry table and read by nothing, which
left the server unable to notice the one thing it most needed to: that the panel
is not executing the prescription the server is reasoning about. Neither field
settles it alone. rx_id says the panel is holding something else and never says
why - a board that lost its state and a board that never applied the answer look
identical. uptime_ms says the board restarted and never says what it dropped, and
its own default of 0 means "not reported", so it cannot even always say that.

Read together they separate "rebooted and lost its bands" from "went quiet and
kept them", and those two want opposite handling: a quiet device's window is
still evidence of what its bands achieved, a rebooted device's window before the
reboot is evidence about setpoints it no longer holds. The pair below that proves
it is test_uptime_going_backwards_cuts_the_window_at_the_restart against
test_a_quiet_device_is_not_a_restarted_one - same rx_id, same silence in the row
history, opposite verdicts, and only the board's own clock between them.

Every case is asserted on what the model was handed: the window it scores its own
last prescription against, and the last_prescription block it reads that window
beside. Not on what a helper returned - a helper that is right and unwired is the
defect this file exists to close.

T0 is fixed rather than wall-clock, the same choice tests/test_scheduler_gates.py
makes. That is safe only because store._prune_old runs on real time and is called
one write in store.PRUNE_EVERY, which this file never reaches - a row dated
before the retention cutoff survives because retention never runs at all.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "device-state.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is not what this file is about

from fastapi.testclient import TestClient  # noqa: E402

from app import brain, main, scheduler, store  # noqa: E402
from app.scheduler import MAX_INTERVAL_S, MIN_INTERVAL_S  # noqa: E402
from app.schema import Control, Setpoint  # noqa: E402

T0 = 1754300000
CLOCK = {"t": T0}
scheduler.now = lambda: CLOCK["t"]
CLIENT = TestClient(main.app)

# Spacing between two diagnoses: past the floor so ask_now is granted, and short
# enough that frames uploaded with the poll are still inside want_frame's 120s.
# Derived from the floor rather than written as 360 so that raising the floor
# moves these timelines with it instead of silently turning every second
# diagnosis into a "min interval" refusal that asserts nothing.
GAP = MIN_INTERVAL_S + 60

# A day of uptime. Large enough that the "uptime shorter than the window"
# witness never fires by accident, so every restart in this file is one the test
# asked for.
UP0 = 86_400_000

# 26.0 degC at 55% RH derives to 1.513 kPa on every row here. HELD contains it
# and DROPPED does not, so in_band_pct comes back 100.0 or 0.0 depending only on
# which prescription the window was scored against. A band taken from the wrong
# one is then a visible number rather than a subtle one.
HELD = (1.4, 1.6)
DROPPED = (0.8, 1.0)

# Absent is not a value. A board with no prescription omits rx_id and a firmware
# that does not report a clock omits uptime_ms; spelling either as 0 or "" in the
# body would test a device that does not exist.
_OMIT = object()


def _body(device, rx_id, uptime_ms, ask_now):
    body = {
        "device": device,
        "sensors": {"temp_c": 26.0, "rh_pct": 55.0, "co2_ppm": 900.0,
                    "leaf_max_c": 31.9},
        "links": {"node_online": True, "cam_online": True, "rgb_live": True,
                  "thermal_live": True, "thermal_fps": 4.0, "wifi_rssi": -55},
        "actuator_intent": {"fan": 0, "mist": 0},
        "auto": True,
        "ask_now": ask_now,
    }
    if rx_id is not _OMIT:
        body["rx_id"] = rx_id
    if uptime_ms is not _OMIT:
        body["uptime_ms"] = uptime_ms
    return body


def _poll(device, rx_id=_OMIT, uptime_ms=_OMIT, ask_now=False, frames=False):
    """One poll at the current clock.

    `frames` uploads a pair first. Without them want_frame defers the diagnosis
    by a poll and the model is never called, which reads as a failed assertion
    about device state rather than as a missing image.
    """
    if frames:
        for kind in ("rgb", "thermal"):
            r = CLIENT.post("/v1/frame", content=b"\xff\xd8\xff\xd9" * 64,
                            headers={"X-Device": device, "X-Kind": kind})
            assert r.status_code == 200, r.text
    r = CLIENT.post("/v1/telemetry", json=_body(device, rx_id, uptime_ms, ask_now))
    assert r.status_code == 200, r.text
    return r.json()


class _Model:
    """A brain that answers with the bands it was told to and keeps every input.

    The bands are the instrument: which prescription's band comes back on the
    window is the whole observable, so each diagnosis is given a distinguishable
    one and the last is repeated for any call past the end of the list.
    """

    def __init__(self, *bands):
        self.bands = bands
        self.seen = []

    async def diagnose(self, inp):
        lo, hi = self.bands[min(len(self.seen), len(self.bands) - 1)]
        self.seen.append(inp)
        return brain.BrainOutput(
            diagnosis_ko="현재 생육 조건은 안정적입니다.",
            head_ko="생육 조건 양호",
            level="ok",
            evidence=["vpd_kpa"],
            deciding="",
            confidence=0.7,
            control=Control(setpoints=[Setpoint(key="vpd_kpa", lo=lo, hi=hi)]),
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


# --------------------------------------------------------------------------
# 1. rx_id: which prescription's bands the window is scored against
# --------------------------------------------------------------------------


def test_a_current_device_is_scored_against_its_own_prescription():
    """The negative control. Every case below is a departure from this one, and
    a fix that scored nothing at all would pass all of them but this."""
    dev = "AA:BB:CC:DD:EE:11"
    model = _Model(HELD)
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, frames=True)["rx_id"]
        assert rx != "none", "the first diagnosis never ran"

        for k in (1, 2, 3):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx, uptime_ms=UP0 + 60_000 * k)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, uptime_ms=UP0 + GAP * 1000, ask_now=True, frames=True)
    finally:
        _restore(keep)

    assert len(model.seen) == 2, "expected two diagnoses, got %d" % len(model.seen)
    last = model.seen[1].last
    window = model.seen[1].window
    assert last["running_on_device"] == "current", last
    assert last["device_restarted_in_window"] is False, last

    vpd = window["metrics"]["vpd_kpa"]
    assert (vpd["lo"], vpd["hi"]) == HELD, vpd
    assert vpd["in_band_pct"] == 100.0, vpd
    assert window["start_ts"] == T0, (
        "the window was cut on a device that never restarted: %s" % window)
    return "band %s scored %.0f%% over %d rows" % (
        HELD, vpd["in_band_pct"], window["n_rows"])


def test_a_device_one_behind_is_scored_against_the_one_it_holds():
    """The panel never applies the second prescription and keeps reporting the
    first. The server used to score the window against the bands it had issued
    rather than the ones the device was holding, which credits a prescription
    with an outcome it had no part in - and that percentage is drawn on the
    측정 column and read by the model as "the band was held".
    """
    dev = "AA:BB:CC:DD:EE:12"
    model = _Model(HELD, DROPPED)
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx1 = _poll(dev, uptime_ms=UP0, frames=True)["rx_id"]
        for k in (1, 2, 3):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx1, uptime_ms=UP0 + 60_000 * k)

        CLOCK["t"] = T0 + GAP
        rx2 = _poll(dev, rx_id=rx1, uptime_ms=UP0 + GAP * 1000,
                    ask_now=True, frames=True)["rx_id"]
        assert rx2 != rx1, "the second diagnosis never ran"

        # Not a link failure: the polls keep arriving on time, they just keep
        # naming the prescription before this one.
        for k in (1, 2, 3):
            CLOCK["t"] = T0 + GAP + 60 * k
            _poll(dev, rx_id=rx1, uptime_ms=UP0 + (GAP + 60 * k) * 1000)

        CLOCK["t"] = T0 + 2 * GAP
        _poll(dev, rx_id=rx1, uptime_ms=UP0 + 2 * GAP * 1000,
              ask_now=True, frames=True)
    finally:
        _restore(keep)

    assert len(model.seen) == 3, "expected three diagnoses, got %d" % len(model.seen)
    last = model.seen[2].last
    vpd = model.seen[2].window["metrics"]["vpd_kpa"]
    assert last["running_on_device"] == "behind", last
    # The server still reports what it issued - that half is not in dispute.
    assert last["control"]["setpoints"][0]["lo"] == DROPPED[0], last["control"]

    assert (vpd["lo"], vpd["hi"]) == HELD, (
        "the window was scored against a prescription the panel never applied: %s"
        % vpd)
    assert vpd["in_band_pct"] == 100.0, vpd
    return "server holds %s, panel holds %s, window scored on %s" % (
        rx2[:8], rx1[:8], HELD)


def test_a_device_holding_no_prescription_is_scored_against_no_band():
    """rx_id "none" is the panel saying it is holding nothing. Its uptime has
    climbed straight through, so this is not a reboot - it is a device that lost
    the prescription without losing power, and uptime_ms is what stops it being
    reported as the other thing.

    Consequence: no band is scored at all. in_band_pct None is "there was no band
    to hold", which is exactly what happened; 100.0 would be a band nobody was
    holding credited with holding itself.
    """
    dev = "AA:BB:CC:DD:EE:13"
    model = _Model(HELD)
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, frames=True)["rx_id"]
        for k in (1, 2, 3):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx, uptime_ms=UP0 + 60_000 * k)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id="none", uptime_ms=UP0 + GAP * 1000,
              ask_now=True, frames=True)
    finally:
        _restore(keep)

    last = model.seen[1].last
    vpd = model.seen[1].window["metrics"]["vpd_kpa"]
    assert last["running_on_device"] == "lost", last
    assert last["device_restarted_in_window"] is False, (
        "a device whose clock never went back was reported as restarted: %s" % last)
    assert last["control"]["setpoints"][0]["lo"] == HELD[0], last["control"]

    assert vpd["in_band_pct"] is None, (
        "a band nobody was holding was credited with %s%%" % vpd["in_band_pct"])
    assert vpd["lo"] is None and vpd["hi"] is None, vpd
    # The readings themselves are still measurements and must survive the cut:
    # only the claim about a band is unsupported, not the numbers.
    assert vpd["n"] > 0 and vpd["mean"] is not None, vpd
    return "bands dropped, %d samples and mean %.3f kept" % (vpd["n"], vpd["mean"])


# --------------------------------------------------------------------------
# 2. uptime_ms: whether the window behind the device is worth extrapolating
# --------------------------------------------------------------------------


def test_uptime_going_backwards_cuts_the_window_at_the_restart():
    """millis() only climbs while a board stays up, so a value below the one
    before it is a restart and nothing else.

    rx_id is satisfied here: by the time the diagnosis runs the panel is holding
    the current prescription again, because the poll that reported the reboot
    was answered with it. Only uptime_ms knows the rows behind that point were
    measured under setpoints the device dropped on the way down.
    """
    dev = "AA:BB:CC:DD:EE:14"
    model = _Model(HELD)
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, frames=True)["rx_id"]
        for k in (1, 2):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx, uptime_ms=UP0 + 60_000 * k)

        CLOCK["t"] = T0 + GAP - 120
        _poll(dev, rx_id="none", uptime_ms=5_000)
        CLOCK["t"] = T0 + GAP - 60
        _poll(dev, rx_id=rx, uptime_ms=65_000)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, uptime_ms=125_000, ask_now=True, frames=True)
    finally:
        _restore(keep)

    last = model.seen[1].last
    window = model.seen[1].window
    assert last["running_on_device"] == "current", last
    assert last["device_restarted_in_window"] is True, last

    stored = len(store.telemetry_since(dev, T0))
    assert stored == 6, "the timeline did not store what it meant to: %d" % stored
    assert window["start_ts"] == T0 + GAP - 120, (
        "the window still spans the restart: %s" % window)
    assert window["n_rows"] == 3, (
        "%d of %d rows survived the cut" % (window["n_rows"], stored))
    return "cut %d of %d rows; window starts %+ds" % (
        stored - window["n_rows"], stored, window["start_ts"] - T0)


def test_a_quiet_device_is_not_a_restarted_one():
    """The other half of the pair, and the reason one field cannot do this.

    This device and the one above report the same rx_id at the diagnosis and the
    same hole in the row history. Reading the hole alone - or reading rx_id alone
    - makes them the same device. The board's own clock is the only thing that
    separates them, and it says this one never went down: its window is intact
    evidence of what the standing bands achieved and must not be cut.
    """
    dev = "AA:BB:CC:DD:EE:15"
    model = _Model(HELD)
    keep = _install(model)
    quiet_s = 5 * GAP
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, uptime_ms=UP0, frames=True)["rx_id"]

        CLOCK["t"] = T0 + quiet_s
        _poll(dev, rx_id=rx, uptime_ms=UP0 + quiet_s * 1000,
              ask_now=True, frames=True)
    finally:
        _restore(keep)

    last = model.seen[1].last
    window = model.seen[1].window
    assert last["running_on_device"] == "current", last
    assert last["device_restarted_in_window"] is False, (
        "a gap in the rows was read as a reboot: %s" % last)
    assert window["start_ts"] == T0, (
        "the window was cut on a device that never went down: %s" % window)
    vpd = window["metrics"]["vpd_kpa"]
    assert vpd["in_band_pct"] == 100.0, vpd
    return "quiet %ds, window kept from %+ds, %.0f%% in band" % (
        quiet_s, window["start_ts"] - T0, vpd["in_band_pct"])


def test_an_unreported_clock_is_admitted_not_answered():
    """uptime_ms defaults to 0, and 0 is the absent case - a board that has been
    up for zero milliseconds has not polled yet.

    Answering False would put "it did not reboot" in front of the model on the
    strength of a field the device never sent, and the window would be kept on
    that authority. None says the question has no answer here, which is the only
    thing the payload supports.
    """
    dev = "AA:BB:CC:DD:EE:16"
    model = _Model(HELD)
    keep = _install(model)
    try:
        CLOCK["t"] = T0
        rx = _poll(dev, frames=True)["rx_id"]
        for k in (1, 2, 3):
            CLOCK["t"] = T0 + 60 * k
            _poll(dev, rx_id=rx)

        CLOCK["t"] = T0 + GAP
        _poll(dev, rx_id=rx, ask_now=True, frames=True)
    finally:
        _restore(keep)

    last = model.seen[1].last
    window = model.seen[1].window
    assert last["running_on_device"] == "current", last
    assert last["device_restarted_in_window"] is None, (
        "an unreported clock was answered instead of admitted: %r"
        % last["device_restarted_in_window"])
    assert window["start_ts"] == T0, (
        "absent evidence cut the window: %s" % window)
    return "no uptime reported -> %r, and the window is untouched" % (
        last["device_restarted_in_window"],)


if __name__ == "__main__":
    print("current  %s" % test_a_current_device_is_scored_against_its_own_prescription())
    print("behind   %s" % test_a_device_one_behind_is_scored_against_the_one_it_holds())
    print("lost     %s" % test_a_device_holding_no_prescription_is_scored_against_no_band())
    print("restart  %s" % test_uptime_going_backwards_cuts_the_window_at_the_restart())
    print("quiet    %s" % test_a_quiet_device_is_not_a_restarted_one())
    print("noclock  %s" % test_an_unreported_clock_is_admitted_not_answered())
    print("OK")
