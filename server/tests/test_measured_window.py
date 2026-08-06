"""A server with no model still owes the panel its arithmetic.

    cd server && python tests/test_measured_window.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives.

derive.window_summary() had exactly one caller, inside main._run_brain, behind the
model gate. A keyless server therefore accepted telemetry every 60s, wrote every
row to SQLite, summarised none of it, and answered with a display whose window
block was four empty rows and two zeros - forever. The monitor page's 최근 구간
card was blank on every install without an API key, which is this project's
default state, over readings the server was already holding.

What this file pins down is the line between a measurement and a judgment, because
the fix is only correct if it moves one and not the other:

  - the window block is measured, needs no model, and must arrive;
  - judgments, plan rows, action rows and setpoints are a model's opinion, and a
    server with no model must not ship one;
  - with no prescription in force there is no band, so every row comes back with
    in_band_pct None - not 0, and pointedly not 100.

The third is the one worth a test of its own. A window scored against no setpoints
that reported "held its band 100% of the hour" would be the most confident lie on
the whole panel, and it would look exactly like a healthy greenhouse.

T0 is fixed rather than wall-clock, the same choice tests/test_scheduler_gates.py
makes, and safe for the reason tests/test_device_state.py sets out: store._prune_old
runs on real time one write in store.PRUNE_EVERY, which this file never reaches.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "measured-window.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is not what this file is about

from fastapi.testclient import TestClient  # noqa: E402

from app import brain, main, scheduler  # noqa: E402
from app.scheduler import MAX_INTERVAL_S  # noqa: E402
from app.schema import Control, Setpoint  # noqa: E402

T0 = 1754300000
CLOCK = {"t": T0}
scheduler.now = lambda: CLOCK["t"]
CLIENT = TestClient(main.app)

# Readings that move, so min / mean / max are three different numbers and a table
# built from one row cannot pass by accident.
_READINGS = [
    (24.0, 60.0, 700.0, 26.5),
    (26.0, 55.0, 900.0, 31.9),
    (28.0, 50.0, 1100.0, 33.4),
]


def _body(device, reading, uptime_ms, rx_id):
    temp, rh, co2, leaf = reading
    body = {
        "device": device,
        "sensors": {"temp_c": temp, "rh_pct": rh, "co2_ppm": co2,
                    "leaf_max_c": leaf},
        "links": {"node_online": True, "cam_online": True, "rgb_live": True,
                  "thermal_live": True, "thermal_fps": 4.0, "wifi_rssi": -55},
        "actuator_intent": {"fan": 0, "mist": 0},
        "auto": True,
        "ask_now": False,
        "uptime_ms": uptime_ms,
    }
    # Omitted rather than sent empty when there is nothing to echo. A board with no
    # prescription does not send the key at all, and main._executing reads its
    # absence as "not running anything" - which is a different fact from "running
    # something else" and the whole point of that helper.
    if rx_id is not None:
        body["rx_id"] = rx_id
    return body


def _poll(device, reading, uptime_ms=86_400_000, rx_id=None):
    r = CLIENT.post("/v1/telemetry", json=_body(device, reading, uptime_ms, rx_id))
    assert r.status_code == 200, r.text
    return r.json()


def _keyless():
    """brain.is_configured() False for the duration - the whole point of the file.

    Asserted rather than assumed: a machine with GEMINI_API_KEY exported would
    otherwise run every case below down the model path and pass them for the wrong
    reason.
    """
    brain.is_configured = lambda: False


# --------------------------------------------------------------------------


def test_the_first_poll_has_nothing_to_summarise():
    """One row is not a window.

    The honest answer for a board that has just been flashed is an empty table,
    and the panel already draws that as 측정된 구간 없음 rather than as four rows
    whose numbers failed to arrive. What must NOT happen is span_s coming back
    non-zero over a single sample, because the panel divides covered by span to
    draw its coverage line.
    """
    device = "aa:00:00:00:00:01"
    CLOCK["t"] = T0
    rx = _poll(device, _READINGS[0])
    d = rx["display"]
    assert rx["rx_id"] == "none"
    assert d["window"] == [], "one reading spans nothing"
    assert d["window_span_s"] == 0
    assert d["window_covered_s"] == 0
    print("first     one row -> empty table, span 0")


def test_a_keyless_server_ships_the_window_it_measured():
    """Three polls, no key, and the table the server used to throw away.

    This is the defect in one assertion: before the fix the second and third polls
    answered with the same empty block as the first, for as long as the install
    ran.
    """
    device = "aa:00:00:00:00:02"
    CLOCK["t"] = T0
    for i, reading in enumerate(_READINGS):
        CLOCK["t"] = T0 + i * 300
        rx = _poll(device, reading)

    d = rx["display"]
    assert rx["rx_id"] == "none", "no model ran, so no prescription was issued"
    assert d["model_ready"] is False
    assert d["window"], "the server measured three readings and must say so"
    assert d["window_span_s"] > 0
    assert d["window_covered_s"] <= d["window_span_s"], "coverage cannot exceed the span"

    # The numbers are the measured ones, not a placeholder: VPD at 24/60, 26/55 and
    # 28/50 is a rising series, so min and max differ on the row.
    labels = [r["label"] for r in d["window"]]
    assert labels, labels
    stats = {r["label"]: r["stat"] for r in d["window"]}
    assert any("/" in s for s in stats.values()), stats
    for label, stat in stats.items():
        parts = [p.strip() for p in stat.split("/")]
        assert len(parts) == 3, (label, stat)
        assert parts[0] != parts[2], (label, stat, "min and max identical over a moving series")
    print("keyless   %d rows measured, span %ds, cov %ds"
          % (len(d["window"]), d["window_span_s"], d["window_covered_s"]))


def test_no_prescription_means_no_band_on_any_row():
    """The one lie this must not tell.

    in_band_pct is a percentage of a window a metric spent inside a band somebody
    asked for. With no prescription nobody asked for one, so the honest answer is
    null on every row and an empty band string beside it. 100 would draw four green
    rows over a greenhouse nobody has ever set a target for.
    """
    device = "aa:00:00:00:00:03"
    CLOCK["t"] = T0
    for i, reading in enumerate(_READINGS):
        CLOCK["t"] = T0 + i * 300
        rx = _poll(device, reading)

    rows = rx["display"]["window"]
    assert rows, "nothing is being asserted if the table is empty"
    for r in rows:
        assert r["in_band_pct"] is None, r
        assert r["band"] == "", r
    print("bands     %d rows, every in_band_pct None" % len(rows))


def test_a_measurement_is_not_a_judgment():
    """The window arrives and nothing else does.

    A server with no model has nothing to say about what the numbers mean. The
    failure mode being closed here is the tempting one: having found a way to fill
    the table, filling the judgment column from the same arithmetic.
    """
    device = "aa:00:00:00:00:04"
    CLOCK["t"] = T0
    for i, reading in enumerate(_READINGS):
        CLOCK["t"] = T0 + i * 300
        rx = _poll(device, reading)

    d = rx["display"]
    assert d["window"], "the premise of this case is a populated table"
    assert d["judgments"] == [], "no model, no verdict"
    assert d["plan"] == [], "no model, no schedule"
    assert d["actions"] == [], "no model, no trail"
    assert d["turn"]["scheduled"] is False, "there is no turn to count down to"
    assert rx["control"] == {"setpoints": [], "schedules": [], "once": [],
                             "policy": {}}, rx["control"]
    assert rx["mode"] == "advisory", "a server that cannot judge grants no licence"
    assert d["notice"], "an empty column must say why"
    print("silence   table %d rows, judgments 0, control empty" % len(d["window"]))


def test_the_notice_says_which_silence_this_is():
    """Two empty columns, two different reasons, two different words.

    A keyed server that has not answered yet will answer; a keyless one will not.
    One notice for both told a grower to wait for something that was not coming.
    """
    device = "aa:00:00:00:00:05"
    CLOCK["t"] = T0
    keyless = _poll(device, _READINGS[0])["display"]["notice"]

    keep = brain.is_configured
    brain.is_configured = lambda: True
    try:
        CLOCK["t"] = T0 + 300
        keyed = _poll("aa:00:00:00:00:06", _READINGS[0])["display"]["notice"]
    finally:
        brain.is_configured = keep

    assert keyless != keyed, "no model configured must not read as no judgment yet"
    assert keyless and keyed
    # Both are drawn into char notice[201]; Korean is 3 bytes a syllable, so the
    # budget is the thing to check and not the character count.
    for n in (keyless, keyed):
        assert len(n.encode("utf-8")) <= 200, (n, len(n.encode("utf-8")))
    print("notice    keyless %dB / keyed %dB, distinguishable"
          % (len(keyless.encode()), len(keyed.encode())))


def test_a_reboot_inside_the_window_does_not_inflate_it():
    """Rows from before a restart are not evidence about now.

    main._restart_at already cuts them for the model's window; the keyless path
    goes through the same helper, so it gets the same cut. Without that, a board
    that rebooted mid-hour would have its pre-reboot readings averaged into a table
    headed 최근 구간.
    """
    device = "aa:00:00:00:00:07"
    CLOCK["t"] = T0
    for i, reading in enumerate(_READINGS):
        CLOCK["t"] = T0 + i * 300
        _poll(device, reading, uptime_ms=86_400_000 + i * 300_000)

    # The board comes back up: uptime collapses to seconds.
    CLOCK["t"] = T0 + 1200
    rx = _poll(device, _READINGS[0], uptime_ms=4_000)
    span = rx["display"]["window_span_s"]
    # One row since the restart cannot span 20 minutes. The pre-reboot rows are
    # still in SQLite; what is asserted is that they are not in this table.
    assert span < 1200, span
    print("reboot    span %ds after a restart, pre-reboot rows excluded" % span)


def test_a_carried_prescription_remeasures_its_window():
    """The window under a standing judgment grows; it is not frozen at issue.

    A prescription is replayed verbatim on every poll until the next diagnosis,
    and its display used to be replayed with it - so a panel in front of a grower
    for six hours showed the min/mean/max of the hour the judgment was made in,
    under a card headed 최근 구간. main._restate remeasures it against the
    setpoints the device is holding, which is what makes the card's name true.

    The judgment itself must NOT move: it is a past-tense claim and re-running the
    model is what a new one costs. Both halves are asserted, because a fix that
    refreshed the whole display would have re-dated a judgment nobody re-made.
    """
    device = "aa:00:00:00:00:08"

    class _Model:
        async def diagnose(self, inp):
            return brain.BrainOutput(
                diagnosis_ko="현재 생육 조건은 안정적입니다.",
                head_ko="생육 조건 양호",
                level="ok",
                evidence=["vpd_kpa"],
                deciding="",
                confidence=0.7,
                control=Control(setpoints=[Setpoint(key="vpd_kpa", lo=0.5, hi=2.5)]),
                wake_after_s=MAX_INTERVAL_S,
                wake_when=[],
                notes_ko="",
            )

    keep = (brain.is_configured, brain.diagnose)
    brain.is_configured = lambda: True
    brain.diagnose = _Model().diagnose
    try:
        # Two rows before the diagnosis so its own window is not empty, then frames
        # so want_frame does not defer the call by a poll.
        CLOCK["t"] = T0
        _poll(device, _READINGS[0])
        CLOCK["t"] = T0 + 300
        _poll(device, _READINGS[1])
        # Frames dated at the same instant as the poll that diagnoses. want_frame
        # measures their age against 120s, so a pair uploaded five minutes earlier
        # would defer the call by one poll and the assertion below would read as a
        # scheduler refusal rather than as a stale image.
        CLOCK["t"] = T0 + 600
        for kind in ("rgb", "thermal"):
            r = CLIENT.post("/v1/frame", content=b"\xff\xd8\xff\xd9" * 64,
                            headers={"X-Device": device, "X-Kind": kind})
            assert r.status_code == 200, r.text
        issued = _poll(device, _READINGS[2])
        assert issued["rx_id"] != "none", "the diagnosis never ran"
        first_span = issued["display"]["window_span_s"]
        judged = issued["display"]["judgments"]
        assert judged, "this case needs a judgment to carry"
    finally:
        brain.is_configured, brain.diagnose = keep

    # Keyless from here, so decide() refuses and every poll carries the standing
    # prescription. The window must still move. rx_id is echoed because that is what
    # a real panel does and what main._executing needs to conclude the device is
    # holding these bands - without it the server correctly scores against nothing,
    # which is a different case and has its own assertion above.
    rx = issued["rx_id"]
    CLOCK["t"] = T0 + 1800
    _poll(device, _READINGS[0], rx_id=rx)
    CLOCK["t"] = T0 + 2400
    carried = _poll(device, _READINGS[1], rx_id=rx)

    assert carried["rx_id"] == rx, "no new prescription was issued"
    assert carried["display"]["judgments"] == judged, \
        "a replayed judgment must not be re-dated or re-worded"
    assert carried["display"]["window_span_s"] > first_span, \
        "the card says 최근 구간 and must not replay the span it was issued with"
    # Scored against the prescription the device says it is holding, so the band is
    # there - this is the one path where in_band_pct is not None on a carried reply.
    pcts = [r["in_band_pct"] for r in carried["display"]["window"]]
    assert any(p is not None for p in pcts), pcts
    print("carried   span %ds -> %ds, judgment unchanged, bands kept"
          % (first_span, carried["display"]["window_span_s"]))


if __name__ == "__main__":
    _keyless()
    # Every case starts keyless; the carried one installs a stub model for exactly
    # as long as it needs one and puts the real answer back.
    assert not brain.is_configured(), "the cases below need a keyless server"
    test_the_first_poll_has_nothing_to_summarise()
    test_a_keyless_server_ships_the_window_it_measured()
    test_no_prescription_means_no_band_on_any_row()
    test_a_measurement_is_not_a_judgment()
    test_the_notice_says_which_silence_this_is()
    test_a_reboot_inside_the_window_does_not_inflate_it()
    test_a_carried_prescription_remeasures_its_window()
    print("OK")
