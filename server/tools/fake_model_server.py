"""A scripted stand-in for the diagnosis path, so the panel can be brought up
without a Gemini key and its failure modes can be produced on demand.

    cd server && python tools/fake_model_server.py --scenario healthy
    cd server && python tools/fake_model_server.py --scenario cycle --port 8000

Every body served here is a real `app.schema.Prescription` assembled by the real
`app.render`, not JSON typed out by hand. That is the whole point: a device
talking to this harness hits the same pydantic validation, the same guard()
character set and the same byte budgets it will hit in production, so a string
that fits here fits there. A dict that happened to serialise would let a
too-wide Korean head through and the truncation would only show up on the wall.

WHAT IS FAKE AND WHAT IS NOT. Fake: which finding the model reaches, and when.
Real: the schema, the rendering, scheduler.clamp_control / clamp_wake, the
window statistics, the routes, the status codes and the frame size limit. There
is no database - history is per-process and dies with it, which is what makes
this safe to point at a dev board and then kill.

Nothing here looks at the readings - the script is the script, so `healthy`
will cheerfully say 생육 상태 안정 over a 2.4 kPa VPD. The chip values and their
tints are the device's own numbers scored against the served bands, so feed it
telemetry that matches the scenario if you want a card that reads coherently.

Scenarios, selected by `--scenario` or PLANTRX_FAKE_SCENARIO. A comma-separated
list is served round-robin, one per poll:

    healthy       bands held, nothing to do; ok row, no once action; a full
                  four-row window table, one row of it unbanded
    vpd_alert     VPD past the band: an alert judgment row and a 미스트 once;
                  a three-row window table with a coverage gap in it
    dead_metric   bands on a sensor that read nothing and on one this board does
                  not have, both dropped by the real validation before the card
                  is rendered; a 3-row table with the silent metric missing
    want_frame    answers want_frame=true and reports the JPEG bytes that arrive
    truncated     a real prescription, serialised, then cut in half on the way out
    server_error  main.py's catch-all body: 200, no display, keep what you have
    slow          sleeps past the device's 20 s deadline, then answers anyway
    cycle         all of the above in that order, one per poll, forever

Cycling matters more than any single scenario: the bugs worth finding are in the
transitions - an alert row that never clears, a plan column that keeps a 즉시
row after the once action expired, a prescription that survives a truncated
reply. Point the board at `--scenario cycle` and watch the columns move.

The routes are the ones app/main.py exposes and nothing else: GET /health, POST
/v1/telemetry, POST /v1/frame (raw JPEG body, X-Device and X-Kind headers), with
the same 400 / 401 / 413 / 422 answers, the same 256 KB frame ceiling, and the
same optional bearer token.

The transport is stdlib http.server rather than uvicorn, for two reasons that
are both scenarios above: `truncated` needs to write fewer bytes than it
promised, and `slow` needs to hold a socket open past a deadline. An ASGI server
correctly refuses to do either.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable, Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from app import brain, derive, render, scheduler  # noqa: E402
from app.schema import (  # noqa: E402
    Control,
    FrameAck,
    OnceAction,
    Prescription,
    Schedule,
    Setpoint,
    Telemetry,
)

# Mirrors main.MAX_FRAME_BYTES. Imported by value rather than from the module
# because app.main pulls in the store and the genai client, and a harness whose
# whole job is to run without a Gemini key should not import one. app.brain is
# safe to import for the same reason it is safe in production without a key: the
# SDK is loaded inside the call, not at module scope.
MAX_FRAME_BYTES = 256 * 1024

# The window the trail is measured over. Long enough that a scenario change is
# still inside it after a few polls, so the 조치 column gets a real
# "VPD 2.1 → 1.3 kPa" instead of a permanent blank.
WINDOW_S = 3600
# Telemetry rows are only ever fed to derive.window_summary, which never looks
# further back than WINDOW_S. Keeping more would be a leak in a process meant to
# be left running against a board all afternoon.
ROWS_MAX = 512


# --------------------------------------------------------------------------
# Per-device state
# --------------------------------------------------------------------------


@dataclass
class DeviceState:
    """What the harness remembers about one board.

    `prev` is the last prescription the device is believed to be holding, and
    "believed" is load-bearing: a reply that was truncated or that arrived after
    the device gave up is deliberately NOT committed here, so the harness's idea
    of what is on the panel stays true and the next scenario carries forward from
    the last row the device actually saw.
    """

    rows: list[dict] = field(default_factory=list)
    prev: Optional[Prescription] = None
    last_scenario: str = ""
    polls: int = 0
    step: int = 0
    # kind -> (bytes, received_ts)
    frames: dict[str, tuple[int, int]] = field(default_factory=dict)
    # Frame bytes since the last telemetry exchange, so the poll that asked for
    # a frame and the upload that answered it appear on the same log line.
    frame_bytes: int = 0


@dataclass
class Reply:
    """A prescription plus how it should be put on the wire.

    `commit` is what separates "the device now holds this" from "the device was
    never able to read this"; see DeviceState.prev. `raw` is for the one reply
    production sends that is not a prescription at all - see server_error().
    """

    rx: Optional[Prescription]
    decision: str
    delay_s: float = 0.0
    truncate: bool = False
    commit: bool = True
    raw: Optional[bytes] = None


# --------------------------------------------------------------------------
# Prescription assembly
# --------------------------------------------------------------------------

# The bands the fake holds. Ordinary tomato-under-LED numbers; the values are
# uninteresting, what matters is that they are real Setpoints and go through the
# same clamp the model's answer does.
_BANDS = [
    Setpoint(key="vpd_kpa", lo=0.8, hi=1.2),
    Setpoint(key="air_c", lo=20.0, hi=27.0),
    Setpoint(key="rh_pct", lo=55.0, hi=80.0),
    Setpoint(key="co2_ppm", lo=600.0),
]


def _wrow(mn, mean, mx, *, lo=None, hi=None, pct=None):
    """One entry of a window_summary()["metrics"] map, in derive's own spelling.

    `first` and `last` are deliberately None. render._window draws min / mean /
    max and never looks at them, while render._outcome draws the 조치 column's
    "VPD 2.1 -> 1.3 kPa" out of exactly those two - so a scripted row shows up in
    the monitor table and cannot be mistaken for a measured transition in the
    trail. The harness is allowed to script a table; it is not allowed to invent
    a before-and-after for a board that never reported the metric.
    """
    return {"n": 12, "first": None, "last": None,
            "min": mn, "mean": mean, "max": mx,
            "lo": lo, "hi": hi, "covered_s": 900.0, "in_band_pct": pct}


def _srow():
    """The entry derive writes for a metric that returned null on every row of
    the window: the key is present and the count is zero.

    Not the same thing as an absent key, and the distinction is what the metric
    vocabulary is read off - see brain.readable_metrics.
    """
    return {"n": 0, "first": None, "last": None,
            "min": None, "mean": None, "max": None,
            "lo": None, "hi": None, "covered_s": 0.0, "in_band_pct": None}


# The metrics a scenario declares silent. This is the one place in the file that
# overrides a measurement rather than filling a gap, and it is deliberate: a
# board whose humidity sensor still works cannot demonstrate what happens to a
# band on one that does not, and waiting for a real sensor to die is not a test.
_SILENT = {"dead_metric": ("rh_pct",)}


# The monitor page's window block. derive.window_summary measures it from the
# rows this board has actually sent, which for the first several minutes after a
# flash is nothing at all - and a table that stays blank through bring-up cannot
# be told from one the firmware failed to parse. So the measurement is kept and
# only its GAPS are filled: a metric the board really reported wins outright.
#
# 장면최고차 is the unbanded row in both tables, and not by accident. _BANDS has
# no leaf_air_dt_c setpoint and never will - the delta is against the hottest
# pixel of the scene, not against a leaf - so in_band_pct is null there in the
# real measurement too, and the scripted row is the row the real path produces.
# That is what puts the panel's empty-band path on screen.
#
# `healthy` fills the first four keys of render._ORDER, so the table is always at
# PLANTRX_WINDOW_ROWS_MAX and the panel's row cap is exercised on every poll of
# it. `vpd_alert` fills three and leaves a coverage gap, so the two spans differ
# and whatever the panel draws from covered_s / span_s has something to draw.
# `dead_metric` fills three and silences 습도 outright - see _SILENT - so the
# metrics the reply may band and the rows the table draws come out of one dict
# that says the same thing twice.
_FILL = {
    "healthy": (3600, 3600, {
        "vpd_kpa": _wrow(0.82, 0.98, 1.18, lo=0.8, hi=1.2, pct=100.0),
        "leaf_air_dt_c": _wrow(0.4, 1.1, 2.3),
        "co2_ppm": _wrow(640, 812, 980, lo=600.0, pct=96.4),
        "air_c": _wrow(21.4, 23.9, 26.2, lo=20.0, hi=27.0, pct=88.0),
    }),
    "vpd_alert": (3600, 1180, {
        "vpd_kpa": _wrow(1.35, 1.92, 2.41, lo=0.8, hi=1.2, pct=4.0),
        "leaf_air_dt_c": _wrow(2.8, 4.6, 6.3),
        "rh_pct": _wrow(31, 38, 46, lo=55.0, hi=80.0, pct=0.0),
    }),
    "dead_metric": (3600, 2400, {
        "vpd_kpa": _wrow(0.90, 1.05, 1.24, lo=0.8, hi=1.2, pct=81.0),
        "air_c": _wrow(20.8, 23.1, 25.9, lo=20.0, hi=27.0, pct=100.0),
        "co2_ppm": _wrow(590, 703, 840),
    }),
}


def _window_for(name: str, measured: dict) -> dict:
    """The measured window with this scenario's gaps filled, or as measured."""
    entry = _FILL.get(name)
    if entry is None:
        return measured
    span_s, covered_s, scripted = entry

    metrics = dict(measured.get("metrics") or {})
    for key, row in scripted.items():
        have = metrics.get(key)
        if not isinstance(have, dict) or not have.get("n"):
            metrics[key] = row

    # Applied after the fill and against the same dict, so the window the reply
    # is gated with is the window the table is drawn from. If they could differ,
    # the panel would show a 습도 row beside a card the server had already taken
    # the 습도 band out of.
    for key in _SILENT.get(name, ()):
        metrics[key] = _srow()

    out = dict(measured)
    out["metrics"] = metrics
    # Floored rather than replaced: a board that has been up for two hours has a
    # longer real window than the script claims, and shortening it would be the
    # harness lying about the one number it did measure.
    out["span_s"] = max(int(measured.get("span_s") or 0), span_s)
    out["covered_s"] = min(
        max(float(measured.get("covered_s") or 0.0), covered_s), out["span_s"]
    )
    return out


def _window_now(st: DeviceState, name: str) -> dict:
    """This scenario's window: measured from the rows the board has really sent,
    with the scenario's gaps filled and its silent metrics silenced.

    Named because two callers need the same object - _prescribe renders the
    table from it, and a scenario that gates its own bands has to validate
    against the very same counts.
    """
    return _window_for(name, derive.window_summary(
        st.rows, st.prev.control.setpoints if st.prev else []
    ))


def _empty(now_ts: int, poll_s: int, want_frame: bool, *,
           window: Optional[dict] = None, model_ready: bool = True) -> Prescription:
    """Mirrors main._empty_prescription, including its advisory mode: a device
    that has never been diagnosed has not been put in auto by anybody.

    model_ready defaults True and not brain.is_configured(): this file IS the
    model, so the answer is unconditional here in a way it never is on the real
    server. The `no_model` scenario is the one place it is passed False, because
    that is the shape a keyless install actually sends and there was previously no
    way to see it without deleting somebody's API key.

    `window` is what makes this reply worth sending at all. The real server used to
    ship this display with a blank table over an hour of readings it had already
    summarised, so the harness mirrored a lie; passing the measured window here
    keeps the mirror honest.
    """
    return Prescription(
        rx_id="none",
        issued_ts=now_ts,
        next_poll_s=poll_s,
        want_frame=want_frame,
        mode="advisory",
        control=Control(),
        display=render.build_empty_display(model_ready=model_ready, window=window),
    )


def _prescribe(
    st: DeviceState,
    t: Telemetry,
    now_ts: int,
    *,
    name: str,
    head: str,
    level: str,
    evidence: list[str],
    deciding: str,
    control: Control,
    after_s: int,
    when: list[dict],
    poll_s: int,
    want_frame: bool = False,
    window: Optional[dict] = None,
) -> Prescription:
    """One prescription, through every step the real path takes after the model
    answers: clamp the control, clamp the wake, measure the window, render.

    Re-serves the previous prescription unchanged while the scenario has not
    moved on, which is what main.py does and is not a shortcut: the device
    compares rx_id and only redraws when it changes, so a harness that minted a
    fresh id every poll would hide every redraw bug it exists to expose.

    The window is measured and then gap-filled per scenario - see _FILL - so the
    monitor page has a table from the first poll instead of after the tenth. A
    scenario that had to gate its own reply against the window passes the object
    it used, so the validation and the table cannot disagree about what read.
    """
    if st.prev is not None and st.last_scenario == name:
        return st.prev.model_copy(update={"next_poll_s": poll_s, "want_frame": want_frame})

    control = scheduler.clamp_control(control)
    wake = scheduler.clamp_wake(after_s, when)
    prev = st.prev
    rgb = st.frames.get("rgb")
    window = _window_now(st, name) if window is None else window

    st.step += 1
    return Prescription(
        rx_id=f"{name}-{st.step:04d}",
        issued_ts=now_ts,
        next_poll_s=poll_s,
        want_frame=want_frame,
        mode="auto" if t.auto else "advisory",
        control=control,
        display=render.build_display(
            control=control,
            head=head,
            # The harness authors `head` directly and has no second paragraph to
            # give, so the body is the same sentence rather than "": an empty body
            # draws the 판단 card with a blank where its prose goes, which looks
            # exactly like the panel-side bug this harness exists to expose. A
            # scenario that wants the multi-line case has to write prose worth
            # wrapping, and every Korean string here is one the display font has to
            # cover - see tools/gen_fonts.py.
            diagnosis=head,
            level=level,
            evidence=evidence,
            deciding=deciding,
            species=_species_for(t),
            current={**t.sensors.model_dump(), **derive.enrich(t)},
            evidence_ts=rgb[1] if rgb else now_ts,
            has_rgb=rgb is not None,
            has_thermal=st.frames.get("thermal") is not None,
            wake=wake,
            issued_ts=now_ts,
            prev=prev,
            window=window,
        ),
    )


# A plantnet.SpeciesResult-shaped dump, so the species card is populated without
# a PlantNet key either. render._species accepts this shape directly.
_SPECIES = {
    "sci": "Solanum lycopersicum",
    "common": "Tomato",
    "korean": "토마토",
    "score": 0.86,
}


def _species_for(t: Telemetry) -> dict:
    """The board's own identification, when it sent one, in preference to the
    canned tomato.

    Mirrors main._resolve_species' first rule, and is the only way to watch a
    panel's own name make the round trip: press the identify button, poll, and
    the card that comes back should read what the panel already reads. A harness
    that always answered 토마토 would make a device that reports 아주가 look
    correct while the two halves disagreed.
    """
    sp = t.species
    if sp is None or not sp.sci.strip():
        return _SPECIES
    return {
        "text": sp.text or sp.sci,
        "sci": sp.sci,
        "conf_pct": sp.conf_pct or None,
        "source": "device",
    }


def _once_through_brain(
    raw: list[dict], now_ts: int, t: Telemetry
) -> tuple[list[OnceAction], list[str]]:
    """The once list as the real path produces it, and the names that did not
    survive it.

    brain.clamp_output is the trust boundary the model's answer crosses, and the
    vocabulary it checks an actuator against is whatever THIS poll's telemetry
    declared in actuator_intent - so a name the panel does not own is dropped
    here, before the wire. Routed through the real function rather than filtered
    with a copy of the rule: a harness that reimplements the check cannot
    demonstrate the check.

    A board that reports no switch positions declares no vocabulary, and then
    nothing is dropped - which is the older-firmware fallback, visible in the log
    as an invented actuator surviving.
    """
    once = brain.clamp_output({"once": raw}, now_ts, t.actuator_intent).control.once
    kept = {a.actuator for a in once}
    return once, sorted({str(d.get("actuator", "")) for d in raw} - kept)


def _bands_through_brain(
    raw: list[dict], now_ts: int, window: dict
) -> tuple[list[Setpoint], list[str]]:
    """The setpoint list as the real path produces it, and the keys that did not
    survive it.

    The sibling of _once_through_brain against the other vocabulary. That one is
    the switches the panel declared; this one is the metrics that actually
    returned samples in the window, which brain.readable_metrics reads off the
    counts derive measured. A band on anything else is dropped before the wire,
    and the two reasons a key dies here are worth telling apart: a metric with a
    zero count is a sensor that went quiet, while `lux` is not a metric key at
    all and never becomes a Setpoint in the first place.

    Routed through the real function rather than filtered with a copy of the
    rule, for the same reason as the once list: a harness that reimplements the
    check cannot demonstrate the check. A window with nothing measured in it
    declares no vocabulary and drops nothing, which is the fresh-boot fallback
    and shows in the log as a band on a silent sensor surviving.
    """
    kept = brain.clamp_output(
        {"setpoints": raw}, now_ts, (), brain.readable_metrics(window)
    ).control.setpoints
    survived = {sp.key for sp in kept}
    return kept, sorted({str(d.get("key", "")) for d in raw} - survived)


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------


def healthy(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """Bands held, nothing to do.

    No deciding metric, so no chip is hot - a finding that nothing fired on
    leads with its headline number rather than accusing one, the same call
    EV_NORMAL makes in src/aijudge.cpp.
    """
    rx = _prescribe(
        st,
        t,
        now_ts,
        name="healthy",
        head="생육 상태 안정, 추가 조치 불필요",
        level="ok",
        evidence=["vpd_kpa", "air_c", "co2_ppm"],
        deciding="",
        control=Control(
            setpoints=list(_BANDS),
            schedules=[Schedule(actuator="pumpA", every_s=21600, duration_s=120)],
            policy={"mist": "auto", "fan": "auto"},
        ),
        after_s=3600,
        when=[{"metric": "vpd_kpa", "op": "gt", "value": 1.8, "for_s": 600}],
        poll_s=scheduler.POLL_IDLE_S,
    )
    return Reply(rx, "hold")


def vpd_alert(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """VPD past the band: an alert row, a hot VPD chip, and a mist once action.

    The once action is the only thing on the card that is a command rather than
    a band, so this is also the scenario that puts a 즉시 row in the 예약
    column and a 완료 row in 조치 - three columns move off one reply.

    Two actions are asked for and only one is meant to survive: `mist` is on the
    panel's own switch list, `fogger` is exactly the sort of name a model invents
    for a machine nobody installed. Both go through the real validation, so the
    drop shows up on this exchange's log line, and the alternative - noticing it
    by reading the panel and finding a row missing - is not a test.
    """
    once, dropped = _once_through_brain(
        [{"actuator": "mist", "seconds": 45}, {"actuator": "fogger", "seconds": 45}],
        now_ts,
        t,
    )
    rx = _prescribe(
        st,
        t,
        now_ts,
        name="vpd_alert",
        head="VPD 급상승, 증산 스트레스 우려",
        level="alert",
        evidence=["vpd_kpa", "rh_pct", "air_c"],
        deciding="vpd_kpa",
        control=Control(
            setpoints=list(_BANDS),
            schedules=[Schedule(actuator="pumpA", every_s=21600, duration_s=120)],
            once=once,
            policy={"mist": "auto", "fan": "auto"},
        ),
        after_s=900,
        when=[{"metric": "vpd_kpa", "op": "gt", "value": 1.6, "for_s": 300}],
        poll_s=scheduler.POLL_ACTIVE_S,
    )
    kept = ",".join(a.actuator for a in once) or "none"
    lost = ",".join(dropped) or "none"
    return Reply(rx, f"alert + once kept={kept} dropped={lost}")


def dead_metric(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """Four bands asked for, two served: what a target nothing measures costs.

    The two that die, die by different rules. `rh_pct` is a perfectly good
    metric key whose sensor reported nothing across this window, so the gate
    built from the measurement itself drops it - the case that only exists at
    runtime and only on this board. `lux` is not in schema.MetricKey at all: the
    BH1750 is dead and no key was ever minted for it, so the band never becomes
    a Setpoint and dies one step earlier, at the contract rather than at the
    gate.

    Both drops land on this exchange's log line, the way vpd_alert's invented
    actuator does. On the wall the card comes back with two bands and the window
    table with no 습도 row, and those two absences are the same fact: the grower
    is never shown a target that nothing can score.
    """
    window = _window_now(st, "dead_metric")
    bands, dropped = _bands_through_brain(
        [
            {"key": "vpd_kpa", "lo": 0.8, "hi": 1.2},
            {"key": "air_c", "lo": 20.0, "hi": 27.0},
            {"key": "rh_pct", "lo": 55.0, "hi": 80.0},
            {"key": "lux", "lo": 800.0, "hi": 1200.0},
        ],
        now_ts,
        window,
    )
    rx = _prescribe(
        st,
        t,
        now_ts,
        name="dead_metric",
        head="습도 미계측, 목표 범위 축소",
        level="warn",
        evidence=["vpd_kpa", "air_c"],
        deciding="vpd_kpa",
        control=Control(
            setpoints=bands,
            schedules=[Schedule(actuator="pumpA", every_s=21600, duration_s=120)],
            policy={"fan": "auto"},
        ),
        after_s=1800,
        when=[{"metric": "vpd_kpa", "op": "gt", "value": 1.8, "for_s": 600}],
        poll_s=scheduler.POLL_IDLE_S,
        window=window,
    )
    kept = ",".join(sp.key for sp in bands) or "none"
    lost = ",".join(dropped) or "none"
    return Reply(rx, f"bands kept={kept} dropped={lost}")


def want_frame(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """Demand a JPEG and diagnose on the next poll.

    Mirrors main.py's deferral exactly, down to returning the PREVIOUS display
    untouched with only want_frame flipped. Reasoning from a frame captured
    minutes ago would put a stale picture beside fresh numbers, and the evidence
    card claims they are from the same moment - so the reply that asks for the
    frame is not allowed to also carry a new finding.

    Never committed: nothing here is a new prescription, so the device is still
    holding whatever it held before.
    """
    poll_s = scheduler.POLL_ACTIVE_S
    if st.prev is None:
        return Reply(_empty(now_ts, poll_s, True, window=_window_now(st, "want_frame")),
                     "frame first (no prescription yet)", commit=False)
    rx = st.prev.model_copy(update={"next_poll_s": poll_s, "want_frame": True})
    return Reply(rx, "frame first", commit=False)


def truncated(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """A real prescription, cut in half on the way out.

    Built through the full path first so the bytes on the wire are genuinely the
    front of a valid body - a hand-written broken string would test the device's
    parser against a shape the server can never produce. Not committed: the
    device could not have read it, and the point of the scenario is that it
    keeps the last good prescription, so the harness has to as well.
    """
    reply = healthy(st, t, now_ts)
    # healthy() re-serves prev while the scenario has not moved on, which would
    # make this a truncated copy of the row already on the panel. Force a body
    # that differs from what the device holds, so "the panel did not change" is
    # evidence of the fallback working rather than of nothing having been sent.
    reply.rx = reply.rx.model_copy(update={"rx_id": f"truncated-{st.polls:04d}"})
    return Reply(reply.rx, "truncated body", truncate=True, commit=False)


def no_model(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """A server with no API key: it measures, and it does not judge.

    This is the shape of a default install, and before this scenario existed the
    only way to see it was to delete a real key. It is worth a scenario of its own
    because it is the one reply where the panel has to draw a populated 최근 구간
    table with an EMPTY judgment column and badge every column 서버 rather than
    모델 - three things that used to be impossible together, because a keyless
    server sent a blank table and the panel badged its rows 모델 anyway.

    Never committed: an empty prescription is not something the device now holds.
    Its rx_id is "none", which is exactly what makes plantrx_rx_real() false and
    keeps the panel's conflict chip from comparing a placeholder mode against
    anything.
    """
    # Explicitly no setpoints, and NOT _window_now(), which scores against
    # st.prev's bands. This scenario is the fresh keyless install - never
    # diagnosed, so there is nothing to score against and every row must come out
    # with in_band_pct None. The other keyless state, a key pulled from an install
    # that already holds a prescription, keeps its bands and is exercised by the
    # real server's carried-forward path; conflating the two here would hide the
    # one shape this scenario exists to produce.
    window = _window_for("no_model", derive.window_summary(st.rows, []))
    return Reply(
        _empty(now_ts, scheduler.POLL_IDLE_S, False,
               window=window, model_ready=False),
        "measured only (no model configured)",
        commit=False,
    )


def slow(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """Answer correctly, far too late.

    The device gives up 20 s after the request goes out (and 4 s after the last
    byte it saw), so the default --slow-s clears both, and the panel should be in
    RX_ERROR with the local rule still filling the log by the time this lands.
    Not committed for the same reason as `truncated`: the device never read it.
    """
    reply = healthy(st, t, now_ts)
    return Reply(reply.rx, f"slept {CONFIG.slow_s:.1f}s", delay_s=CONFIG.slow_s, commit=False)


def server_error(st: DeviceState, t: Telemetry, now_ts: int) -> Reply:
    """The one body production sends that is not a prescription.

    main.py's catch-all handler answers 200 with {"rx_id","next_poll_s"} rather
    than a 500, deliberately: a polling device must not be turned into a retry
    storm by a server-side bug. So the device has to recognise a 200 carrying no
    display, keep what it is holding, and still honour the interval. That is a
    distinct path from a transport failure and it is only reachable from here.
    """
    body = json.dumps({"rx_id": "error", "next_poll_s": 60}).encode()
    return Reply(None, "server-side bug (200, no display)", commit=False, raw=body)


SCENARIOS: dict[str, Callable[[DeviceState, Telemetry, int], Reply]] = {
    "healthy": healthy,
    "vpd_alert": vpd_alert,
    "dead_metric": dead_metric,
    "want_frame": want_frame,
    "no_model": no_model,
    "truncated": truncated,
    "slow": slow,
    "server_error": server_error,
}

# Order matters: a diagnosis, then an escalation, then a reply the server takes
# two bands out of, then a frame demand against a populated card, then a keyless
# server that measures without judging, then the three failures the device has to
# survive without losing the card it is showing.
#
# no_model sits after a populated card on purpose. Its own shape - a table with
# rows and a judgment column with none - is only half the test; the other half is
# that the panel, having just been given a real judgment, correctly stops
# attributing the columns to a model when the next reply says there is not one.
CYCLE = ("healthy", "vpd_alert", "dead_metric", "want_frame", "no_model",
         "truncated", "server_error", "slow")


# --------------------------------------------------------------------------
# Transport
# --------------------------------------------------------------------------


@dataclass
class Config:
    scenarios: tuple[str, ...] = ("healthy",)
    slow_s: float = 25.0
    truncate_at: float = 0.5
    short_read: bool = False
    token: str = ""


CONFIG = Config()
STATE: dict[str, DeviceState] = {}
# ThreadingHTTPServer plus a scenario that stalls for 20 s means two exchanges
# genuinely overlap. Everything that touches STATE is short, so one lock is
# cheaper than reasoning about which field is safe.
_LOCK = threading.Lock()


def _state(device: str) -> DeviceState:
    st = STATE.get(device)
    if st is None:
        st = DeviceState()
        STATE[device] = st
    return st


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.1 so Content-Length is honoured and `short_read` is a real short
    # read rather than a connection the client closes on its own terms.
    protocol_version = "HTTP/1.1"
    server_version = "plantrx-fake/1"

    def log_message(self, fmt: str, *args) -> None:
        pass  # one line per exchange is logged from the handlers, not two

    # -- helpers ---------------------------------------------------------

    def _body(self) -> bytes:
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return b""
        return self.rfile.read(n) if n > 0 else b""

    def _send(self, status: int, payload: bytes, *, declared: Optional[int] = None) -> bool:
        """`declared` overrides Content-Length, which is the only way to make a
        client see a body end early.

        False means the client was already gone. That is not an error here - it
        is the `slow` scenario succeeding - so it is caught rather than left to
        socketserver, which would print a traceback over the log line that says
        the device gave up, which is the one line that scenario exists to
        produce.
        """
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(declared if declared is not None else len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)
            return True
        except OSError:
            return False
        finally:
            self.close_connection = True

    def _json(self, status: int, obj: dict) -> bool:
        return self._send(status, json.dumps(obj, ensure_ascii=False).encode())

    def _authorised(self) -> bool:
        if not CONFIG.token:
            return True
        return self.headers.get("Authorization") == f"Bearer {CONFIG.token}"

    # -- routes ----------------------------------------------------------

    def do_GET(self) -> None:
        if self.path == "/health":
            self._json(200, {"ok": True, "ts": scheduler.now()})
            return
        self._json(404, {"detail": "Not Found"})

    def do_POST(self) -> None:
        if self.path == "/v1/telemetry":
            self._telemetry()
        elif self.path == "/v1/frame":
            self._frame()
        else:
            self._json(404, {"detail": "Not Found"})

    def _telemetry(self) -> None:
        raw = self._body()
        if not self._authorised():
            self._json(401, {"detail": "bad token"})
            print(f"[fake] telemetry req={len(raw)}B -> 401 bad token", flush=True)
            return

        try:
            t = Telemetry.model_validate_json(raw)
        except Exception as exc:
            # FastAPI's own answer to a body that does not validate. The device
            # should treat it exactly like any other failed poll.
            self._json(422, {"detail": str(exc)[:400]})
            print(f"[fake] telemetry req={len(raw)}B -> 422 unparseable", flush=True)
            return

        now_ts = scheduler.now()
        # The server is threaded, and `slow` exists precisely to leave one
        # exchange in flight while the next arrives. Everything that reads or
        # advances device state happens under the lock, and only the sleep and
        # the write are left outside it - otherwise the poll counter is read
        # after the sleep, and the stalled exchange logs the sequence number of
        # the one that overtook it.
        with _LOCK:
            st = _state(t.device)
            st.polls += 1
            seq = st.polls
            name = CONFIG.scenarios[(seq - 1) % len(CONFIG.scenarios)]
            st.rows.append(
                {
                    "recv_ts": now_ts,
                    **t.sensors.model_dump(),
                    **derive.enrich(t),
                    "actuators": dict(t.actuators),
                    "actuator_intent": dict(t.actuator_intent),
                    # Kept as sent, None included, so derive.interventions over
                    # these rows measures what the board actually reported rather
                    # than what the harness would have defaulted to.
                    "edges": t.edges,
                    "allstops": t.allstops,
                }
            )
            st.rows = [r for r in st.rows if r["recv_ts"] >= now_ts - WINDOW_S][-ROWS_MAX:]
            reply = SCENARIOS[name](st, t, now_ts)

        if reply.delay_s > 0:
            time.sleep(reply.delay_s)

        payload = reply.raw if reply.raw is not None else reply.rx.model_dump_json().encode()
        declared = None
        if reply.truncate:
            cut = max(1, int(len(payload) * CONFIG.truncate_at))
            if CONFIG.short_read:
                declared = len(payload)  # promise the whole body, deliver half
            payload = payload[:cut]

        delivered = self._send(200, payload, declared=declared)

        # A body the client never read is not one it is holding, whatever the
        # scenario intended.
        with _LOCK:
            if reply.commit and delivered and reply.rx is not None:
                st.prev = reply.rx
                st.last_scenario = name
            frames = st.frame_bytes
            st.frame_bytes = 0

        # Only when the board sent one: an unconditional field would print an
        # empty species on every line of a run where nothing ever identified.
        sp = t.species
        said = f" species={sp.text or sp.sci}/{sp.sci} {sp.conf_pct}%" if sp else ""

        # Only the switches that are ON, so an ordinary line stays one line. An
        # empty object is a board that reports none at all, and that is not the
        # same claim as a board reporting every switch off: the first declares no
        # vocabulary and validation stands down, the second declares seven names.
        on = ",".join(f"{k}:{v}" for k, v in sorted(t.actuator_intent.items()) if v)
        sw = f" sw={on}" if on else (" sw=all-off" if t.actuator_intent else " sw=none")

        # The counters, which sw= structurally cannot show: a switch flicked
        # between two polls is in neither snapshot, and 전체 정지 taken back inside
        # its undo window is in neither either. Watch them climb across lines and
        # the presses are visible as they arrive. "-" and not 0 for a firmware
        # that sends neither field, because a printed zero the harness invented
        # reads exactly like a panel nobody touched - which is the confusion these
        # fields were added to end, not one to reproduce in the log.
        ed = "-" if t.edges is None else t.edges
        al = "-" if t.allstops is None else t.allstops
        presses = f" edges={ed} allstops={al}"

        # What the panel will actually be able to draw from this reply, which the
        # decision string does not say: a keyless server's whole contribution is
        # the table, and "win=4/600s model=no judge=0" is the shape that used to be
        # impossible - a populated table under an empty judgment column. Printed
        # for every scenario and not only no_model, because the regression to watch
        # for is the table quietly emptying on a reply that should carry one.
        shape = ""
        if reply.rx is not None:
            d = reply.rx.display
            shape = (f" win={len(d.window)}/{d.window_span_s}s"
                     f" judge={len(d.judgments)}"
                     f" model={'yes' if d.model_ready else 'no'}")

        print(
            f"[fake] #{seq:04d} {name:<12} dev={t.device} req={len(raw)}B "
            f"resp={len(payload)}B"
            + (f" (declared {declared}B)" if declared is not None else "")
            + f" rx={reply.rx.rx_id if reply.rx else '-'} decision={reply.decision!r}"
            + f" frames={frames}B{said}{sw}{presses}{shape}"
            + ("" if delivered else " CLIENT GAVE UP"),
            flush=True,
        )

    def _frame(self) -> None:
        device = self.headers.get("X-Device")
        kind = self.headers.get("X-Kind")
        blob = self._body()

        if not self._authorised():
            self._json(401, {"detail": "bad token"})
            status = 401
        elif not device or not kind:
            # main.py declares both as Header(...), so a missing one is a
            # validation error and not a 400.
            self._json(422, {"detail": "missing X-Device / X-Kind"})
            status = 422
        elif kind not in ("rgb", "thermal"):
            self._json(400, {"detail": "kind must be rgb|thermal"})
            status = 400
        elif not blob or len(blob) > MAX_FRAME_BYTES:
            self._json(413, {"detail": "bad frame size"})
            status = 413
        else:
            with _LOCK:
                st = _state(device)
                st.frames[kind] = (len(blob), scheduler.now())
                st.frame_bytes += len(blob)
                ack = FrameAck(ok=True, rx_id=st.prev.rx_id if st.prev else None)
            self._json(200, ack.model_dump())
            status = 200

        # A frame that is not a JPEG is worth saying out loud: the server never
        # decodes it, so nothing else would notice the device sent a raw buffer.
        jpeg = blob[:2] == b"\xff\xd8"
        print(
            f"[fake] {'frame':<18} dev={device} kind={kind} {len(blob)}B "
            f"jpeg={'yes' if jpeg else 'no'} -> {status}",
            flush=True,
        )


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


def _scenarios(spec: str) -> tuple[str, ...]:
    if spec == "cycle":
        return CYCLE
    names = tuple(s.strip() for s in spec.split(",") if s.strip())
    bad = [n for n in names if n not in SCENARIOS]
    if not names or bad:
        raise SystemExit(f"unknown scenario(s): {', '.join(bad) or '(empty)'}")
    return names


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--host", default="0.0.0.0", help="default listens on the LAN, since the point is a board reaching it")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument(
        "--scenario",
        default=os.getenv("PLANTRX_FAKE_SCENARIO", "healthy"),
        help="one name, a comma-separated list served round-robin, or 'cycle' for all of "
             + ",".join(CYCLE),
    )
    p.add_argument(
        "--slow-s",
        type=float,
        default=25.0,
        help="how long the 'slow' scenario holds the socket before answering; the client "
             "gives up 20 s after the request and 4 s after the last byte, so the default "
             "clears both",
    )
    p.add_argument(
        "--truncate-at",
        type=float,
        default=0.5,
        help="fraction of the body the 'truncated' scenario actually writes",
    )
    p.add_argument(
        "--short-read",
        action="store_true",
        help="'truncated' declares the full Content-Length, so the device's read loop "
             "sees the body end early rather than receiving complete but invalid JSON",
    )
    p.add_argument("--token", default=os.getenv("PLANTRX_TOKEN", ""), help="require this bearer token")
    a = p.parse_args()

    CONFIG.scenarios = _scenarios(a.scenario)
    CONFIG.slow_s = a.slow_s
    CONFIG.truncate_at = a.truncate_at
    CONFIG.short_read = a.short_read
    CONFIG.token = a.token

    httpd = ThreadingHTTPServer((a.host, a.port), Handler)
    print(
        f"[fake] listening on {a.host}:{a.port} | scenarios={','.join(CONFIG.scenarios)} "
        f"| auth={'on' if CONFIG.token else 'off'} | charset={render.ALLOWED_DESC}",
        flush=True,
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
