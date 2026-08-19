"""FastAPI entry point: six endpoints the device drives, and three an operator does.

The device is behind NAT wherever it happens to be plugged in, so nothing here
ever connects outward to it. It polls; the response to its telemetry IS the
current prescription. The poll interval is handed back in that same response, so
the server controls the cadence - ~60s idle, ~3s for a minute after something
happens. That interval is the push latency, and it is allowed to be seconds
because the control loop lives on the device: the server ships setpoints that
stay valid for hours, not commands that need to arrive now.

Consequences worth keeping in mind when editing this file:
  - A failed request costs nothing. The device keeps running its last
    prescription, so there is no retry storm to design around.
  - Returning a stale prescription is fine and normal. rx_id is what tells the
    device whether to redraw.
  - The operator-facing endpoint does not reach the device either, because there
    is nothing here to reach it with. It arms a flag that the next telemetry
    response carries out on its way past, so anything else added alongside it has
    to be shaped the same way: one-shot, and cleared as it is delivered rather
    than when it is acted on. See store.take_update_mode for what happens to a
    signal that is not. firmware_pull is the second thing on that rail, armed by
    the same call; node_pull_cam and node_pull_node are the third and fourth,
    armed by /device/{device}/node_update, and they are the ones where the
    clearing genuinely matters - they do not stop the panel polling, so a flag
    that outlived its delivery re-arms a node update once a minute forever.
  - The two firmware endpoints break the "a response to a poll is the whole
    conversation" shape, and they are the only things here that do. They are a
    manifest and a file, fetched by a board that has already been told to update
    and carrying no prescription at all. Three boards fetch them now - ?role=
    picks which image, defaulting to panel because every panel already deployed
    asks without it. They are still device-driven for the same NAT reason as
    everything else: the server cannot push an image any more than it can push a
    setpoint, so the board comes and gets it.
  - /nodelog is the one endpoint whose payload is about a device that cannot
    reach this server at all. The two nodes speak ESP-NOW and nothing else; the
    panel relays their log lines here so there is somewhere to read them from,
    which makes it the only POST where `device` names the forwarder rather than
    the subject.
"""

import asyncio
import logging
import os
import time
import uuid
from typing import Literal, Optional

from fastapi import APIRouter, Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse

from . import admin, brain, derive, firmware, ghfw, plantnet, render, scheduler, store
from .schema import (Control, Display, FirmwareManifest, FrameAck,
                     Identification, NodeLogBatch, NodeLogRow, Prescription,
                     Telemetry)

log = logging.getLogger("plantrx")
logging.basicConfig(level=os.getenv("PLANTRX_LOG", "INFO"))

DEVICE_TOKEN = os.getenv("PLANTRX_TOKEN", "")
MAX_FRAME_BYTES = 256 * 1024

app = FastAPI(title="AI PlantRx", docs_url="/docs", redoc_url=None)
v1 = APIRouter(prefix="/v1")

# One model call at a time. Two overlapping diagnoses would each see the other's
# window as "nothing happened yet" and both would prescribe from the same stale
# outcome.
_brain_lock = asyncio.Lock()
# Last time anything interesting happened per device, used only to decide how
# fast to tell the device to poll.
_last_event: dict[str, int] = {}


def auth(authorization: Optional[str] = Header(default=None)) -> None:
    """Bearer token, shared secret, stored in the device's NVS.

    Deliberately not per-device credentials: there is one board, and a token it
    can present over TLS is the whole threat model. The transport is what
    matters here - the firmware historically used setInsecure(), and the point
    of this server having a real certificate is that it no longer has to.
    """
    if not DEVICE_TOKEN:
        return  # unset in development; refuse to pretend it is secured
    if authorization != f"Bearer {DEVICE_TOKEN}":
        raise HTTPException(status_code=401, detail="bad token")


def _role_q(role: str = firmware.DEFAULT_ROLE) -> str:
    """?role= on the two firmware endpoints, refused before it can reach a path.

    The default is the whole backward-compatibility story: a panel flashed before fwpull.cpp
    learned to send the query builds its URL with no role at all, so "no role" has to keep
    meaning the panel's image forever. Current panel builds send ?role=panel explicitly.

    An unknown role is a 400 and not a fallback to that default, which is the one decision in
    this function. Falling through would hand an ESP32-CAM a 7" RGB panel image: a
    bootloader-valid application for the wrong hardware, written into the OTA slot of a board
    sealed in a housing on a pole with no serial console. It boots, finds no PSRAM camera
    where it expects one, and the only symptom anybody sees is a stream that stopped. A typo
    in a curl is worth a 400; that is not worth anything.
    """
    if role not in firmware.ROLES:
        log.info("firmware: refusing unknown role %r", role)
        raise HTTPException(
            status_code=400, detail="role must be one of %s" % ", ".join(firmware.ROLES))
    return role


@app.on_event("startup")
def _startup() -> None:
    store.init_db()
    log.info(
        "plantrx up | gemini=%s plantnet=%s auth=%s | charset=%s",
        brain.is_configured(),
        plantnet.is_configured(),
        bool(DEVICE_TOKEN),
        render.ALLOWED_DESC,
    )
    # After the banner, because it logs a line of its own and the order reads as
    # "here is the server, here is where its firmware comes from". Returns as soon
    # as its thread is running; the first pull happens on that thread, so a slow or
    # unreachable GitHub delays no request and fails no startup.
    ghfw.start()


@app.get("/health")
def health() -> dict:
    return {"ok": True, "ts": scheduler.now()}


def _mode(t: Telemetry) -> Literal["auto", "advisory"]:
    """The licence to act, restated on every response that carries one.

    mode is a function of the panel's own AI-RX mode switch and of server policy,
    never of the model's output, so it is never inherited. prev.model_copy()
    hands back a prescription whose mode was decided up to MAX_INTERVAL_S ago,
    and src/ui/page_auto.cpp draws a chip whenever the mode it is sent disagrees
    with the switch it is holding - so a stale mode is not a stale field, it is
    the panel accusing the server of a disagreement neither side has.
    """
    return "auto" if t.auto else "advisory"


def _restate(prev: Prescription, model_ready: bool, window: dict) -> Display:
    """prev's display with the two things on it that are not facts about prev.

    The rows, the notice and the species were true when they were written and stay
    true. Two things are not like them.

    model_ready is this server's key, now, and the panel badges three columns from
    it - so a key pulled after the last diagnosis would otherwise leave the wall
    claiming a model authored rows forever, because scheduler.decide() refuses to
    call and this prescription is replayed verbatim on every poll from here on.

    The window block is the other. It used to be frozen at the moment the
    prescription was issued and replayed for the whole of its tenure, so a panel
    standing in front of a grower for six hours showed an hour-old min/mean/max
    under a card headed 최근 구간. It is remeasured here, against the setpoints the
    device is actually holding, so the table means "the window this prescription
    has been in force for so far" - which is what the grower reads it as. The
    judgment row above it keeps its own age; the two are different facts and the
    top bar already draws the difference.
    """
    rows, span, covered = render.window_block(window)
    return prev.display.model_copy(update={
        "model_ready": model_ready,
        "window": rows,
        "window_span_s": span,
        "window_covered_s": covered,
    })


def _empty_prescription(now_ts: int, poll_s: int, want_frame: bool,
                        model_ready: bool, window: dict) -> Prescription:
    """What a device gets before it has ever been diagnosed.

    An empty card reads as a broken screen, so the display half says so in
    words rather than leaving the fields blank.

    It is no longer an empty BODY, though - only an empty judgment. `window`
    carries what the server measured, which on a keyless install is everything it
    will ever have to say: this path is the only reply such a server sends, and it
    used to send four blank table rows over an hour of readings it had already
    summarised for a model it cannot call. A measurement is not a judgment, so the
    judgments stay empty and no setpoint goes out - the server has no basis to
    prescribe without a model, and the panel's own bands are honestly the panel's.

    advisory here is server policy recomputed, not a placeholder default and not
    an echo of t.auto: the server has never looked at this plant, so it grants
    no licence to act on its authority whatever the panel's switch says - which
    is also why the body carries an empty Control(): there is nothing to act on.
    rx_id "none" is what keeps that from reading as a disagreement: the panel's
    conflict chip requires a real prescription id, so a placeholder mode is
    compared against nothing.
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


# One shape for a species, whoever found it. brain.py JSON-dumps it into the
# prompt and render._species turns it into the card, and neither has to know
# which of the three paths in _resolve_species produced it.
#
# conf_pct is the measurement: a whole percent, the unit the device reports and
# the unit the card prints. conf_text is set only on the carried-forward path,
# which remembers the string the last prescription printed rather than the
# number behind it; render prefers it when it is there so a carried card keeps
# the exact figure it was already showing.
def _species_entry(
    *,
    text: str,
    sci: str,
    source: str,
    conf_pct: Optional[int] = None,
    conf_text: str = "",
) -> Optional[dict]:
    name = (text or sci or "").strip()
    if not name:
        return None  # a species with no name to draw is not a species
    # Clamped before it is judged, so a device sending a negative percent is out
    # of range rather than confident. Zero is DeviceSpecies' default and means
    # "not reported"; drawing it as 0% would put a figure on the card that
    # neither side measured.
    if conf_pct is not None:
        conf_pct = max(0, min(100, int(conf_pct))) or None
    return {
        "text": name,
        "sci": (sci or "").strip(),
        "conf_pct": conf_pct,
        "conf_text": conf_text,
        "source": source,
    }


async def _resolve_species(
    t: Telemetry, prev: Optional[Prescription], rgb: Optional[bytes]
) -> Optional[dict]:
    """Who gets to name the plant, in order.

    1. The device. It ran the identification itself, on its own quota-limited
       keys, with the plant in front of its lens because a human pressed the
       button, and src/ui/page_auto.cpp draws that answer in preference to this
       one. So a reported species does not merely win the tie - it stops the
       server-side call from being made at all. That saving is the point: two
       identifications spend two quotas to put two names on one plant, and the
       wall would keep showing the one the server did not use.
    2. The server, on exactly the condition it used before: a frame, a key, and
       no species on the previous prescription.
    3. Whatever the last prescription said, so a poll that identifies nothing
       does not blank a card that was already right.
    """
    dev = t.species
    if dev is not None and dev.sci.strip():
        return _species_entry(
            text=dev.text,
            sci=dev.sci,
            conf_pct=dev.conf_pct,
            source="device",
        )

    if plantnet.is_configured() and rgb and (prev is None or prev.display.species is None):
        try:
            found = await plantnet.identify(rgb)
            if found:
                return _species_entry(
                    text=found.korean or found.common,
                    sci=found.sci,
                    # The same rounding render applies to a raw score, so the two
                    # paths cannot print different figures for one measurement.
                    conf_pct=int(found.score * 100 + 0.5),
                    source="server",
                )
        except Exception:  # identification is a nice-to-have, never a blocker
            log.exception("plantnet failed")

    if prev is not None and prev.display.species is not None:
        sp = prev.display.species
        # "carried" rather than re-asserting device or server: which one found it
        # is not recorded on the prescription, and telling the model the panel
        # identified a plant it did not is worse than admitting the name is
        # inherited.
        return _species_entry(
            text=sp.text, sci=sp.sci, conf_text=sp.conf_text, source="carried"
        )
    return None


# What _empty_prescription puts on the wire, and what the panel echoes back
# until it has been given a real one. A reflashed board sends the field empty or
# omits it; all three mean the panel is holding no band.
_NO_RX = ("", "none")


def _executing(
    device: str, t: Telemetry, prev: Optional[Prescription]
) -> tuple[str, Optional[Prescription]]:
    """Which prescription the panel is actually running, and the bands it holds.

    The window is measured evidence about the device, so it has to be scored
    against what the device was holding rather than what the server last issued.
    A panel still running the prescription before this one held that one's bands
    for the whole window; a panel reporting none held nothing at all. Crediting
    either with the current bands invents an outcome - in_band_pct is drawn on
    the 측정 column and read by the model as "the band was held".

    The second element is None wherever no band can be named, which
    window_summary reports as no band rather than as a band that was missed.
    """
    rx = (t.rx_id or "").strip()
    if prev is None:
        return "none", None
    if rx == prev.rx_id:
        return "current", prev
    if rx in _NO_RX:
        return "lost", None
    held = store.prescription_by_id(device, rx)
    # An id this server never issued to this device is not an older prescription
    # and must not be reported as one - it is a panel we cannot account for.
    return ("behind", held) if held is not None else ("unknown", None)


def _restart_at(rows: list[dict]) -> Optional[int]:
    """Index of the first row the device reported after its last restart.

    uptime_ms only climbs while a board stays up, so a value below the one
    before it is a restart and nothing else. Rows carrying no uptime are skipped
    rather than read as zero, which would make every one of them a restart.
    """
    at: Optional[int] = None
    last = 0
    for i, r in enumerate(rows):
        up = int(r.get("uptime_ms") or 0)
        if up <= 0:
            continue
        if up < last:
            at = i
        last = up
    return at


def _restarted(t: Telemetry, since_ts: int, now_ts: int, seen: bool) -> Optional[bool]:
    """Did the device restart inside the window - true, false, or unanswerable.

    Two witnesses, because rx_id has none of its own: a panel reporting no
    prescription may have rebooted or may simply never have applied one, and
    only its own clock separates those. `seen` is a drop between two stored
    rows, which is proof. Failing that, an uptime shorter than the window says
    the restart happened before the oldest row that survived - the case a row
    scan cannot see, because the device was off the air for it.

    None is not False. uptime_ms defaults to 0 on a payload that omitted it, and
    a board reporting no clock is not a board that did not reboot.
    """
    if seen:
        return True
    if t.uptime_ms <= 0:
        return None
    return t.uptime_ms < (now_ts - since_ts) * 1000


# How far back the window reaches for a device that has never been diagnosed.
# The same hour the model's own path used before this was factored out, and
# deliberately not a second number: a keyless server's table and a keyed one's
# have to be measured over the same span or the two installs would disagree about
# what "최근 구간" means.
WINDOW_LOOKBACK_S = 3600


class _WindowCtx:
    """Everything the window is, and what it was measured against.

    This used to live inline in _run_brain, which is why a server with no model
    shipped no window at all: window_summary() had exactly one caller and it was
    behind the model gate. The server measured the whole hour, wrote every row to
    SQLite, and told the panel nothing - so the 최근 구간 card was permanently
    empty on every install without a key, which is this project's default state.
    Factored out rather than duplicated so the keyless table cannot drift from
    the one the model reasons over: same rows, same reboot cut, same bands.
    """

    __slots__ = ("rows", "window", "held", "status", "restarted", "tenure", "since")

    def __init__(self, *, rows, window, held, status, restarted, tenure, since):
        self.rows = rows
        self.window = window
        self.held = held
        self.status = status
        self.restarted = restarted
        self.tenure = tenure
        self.since = since


def _window_ctx(device: str, t: Telemetry, now_ts: int,
                prev: Optional[Prescription]) -> _WindowCtx:
    since = prev.issued_ts if prev else now_ts - WINDOW_LOOKBACK_S
    rows = store.telemetry_since(device, since)
    status, held = _executing(device, t, prev)

    # Read before the cut below, deliberately. That cut is about readings: it is
    # the presses themselves that survive a reboot, because the grower made them
    # against the prescription still in force and the board going down afterwards
    # does not unmake them. window's own interventions block is measured on what
    # survived the cut, so it matches the bands it sits beside; this one covers
    # the whole span the prescription was in force. On a window that spans no
    # restart they are the same numbers.
    tenure = derive.interventions(rows)

    # Rows measured before a restart describe a device holding setpoints it
    # dropped on the way down, so they are not evidence about this prescription
    # and a trend extrapolated across the gap is worse than no trend at all. The
    # window starts again where the device did.
    cut = _restart_at(rows)
    if cut is not None:
        rows = rows[cut:]

    # No prescription, no bands - and so no in_band_pct on any row. That is the
    # honest shape and render._window already emits it: `band` comes out "" and
    # in_band_pct None, which the panel draws as a row with numbers and no verdict
    # rather than as a metric that held its band for 100% of an hour nobody set a
    # band for.
    held_setpoints = held.control.setpoints if held is not None else []

    return _WindowCtx(
        rows=rows,
        window=derive.window_summary(rows, held_setpoints),
        held=held,
        status=status,
        restarted=_restarted(t, since, now_ts, cut is not None),
        tenure=tenure,
        since=since,
    )


async def _run_brain(device: str, t: Telemetry, now_ts: int, reason: str,
                     ctx: Optional[_WindowCtx] = None) -> Optional[Prescription]:
    """Assemble context, ask the model, clamp the answer, store it.

    `ctx` is the window the caller already measured. telemetry() needs it whether
    or not the model runs - the panel's table is drawn from it on every poll now,
    not only on the ones that reach a model - so passing it in is what stops the
    same hour of rows being read out of SQLite and summarised twice per poll.
    """
    prev = store.latest_prescription(device)
    if ctx is None:
        ctx = _window_ctx(device, t, now_ts, prev)
    status, restarted, tenure = ctx.status, ctx.restarted, ctx.tenure

    rgb = store.latest_frame(device, "rgb")
    species = await _resolve_species(t, prev, rgb)

    inp = brain.BrainInput(
        species=species,
        now_ts=now_ts,
        current=derive.current_readings(t),
        window=ctx.window,
        # The panel's switch positions, and by their key set the actuator
        # vocabulary this answer will be validated against - see brain._once.
        actuator_intent=t.actuator_intent,
        # The finding, not the full sentence, and no longer because the head is all
        # the panel showed - JudgeRow.body now carries the whole diagnosis to the
        # card. It is the head because continuity is a question about the verdict:
        # feeding the model back its own paragraph would spend the context window
        # re-reading prose it wrote, and invite it to answer by re-wording rather
        # than by re-judging. The paragraph is kept with the prescription
        # (store.save_prescription's raw_model) for whoever reads the history.
        # judgments is one row now (schema.JUDGE_ROWS_MAX), so [0] is the last
        # judgment rather than the newest of six; the empty guard still stands
        # because a carried prescription from a model-less server has none.
        last=(
            {
                "control": prev.control.model_dump(),
                "head": prev.display.judgments[0].head if prev.display.judgments else "",
                "level": prev.display.judgments[0].level if prev.display.judgments else "ok",
                # Whether the panel is running this prescription at all. It
                # rides here rather than as a payload key of its own because
                # brain dumps `last` verbatim as last_prescription, and "you are
                # being asked to re-issue bands the device is not holding" is a
                # fact about that prescription, read in the same breath as its
                # bands.
                "running_on_device": status,
                "device_restarted_in_window": restarted,
                # The grower's own hands, which nothing else on this poll can
                # show: actuator_intent is one snapshot, so a switch flicked
                # between two polls is in none of them and 전체 정지 taken back
                # inside its undo window is in none of them either. A prescription
                # asked to judge whether its bands worked over an hour it was
                # overruled in twice is judging a window it does not own, and
                # these are the only fields that say so. Null is a firmware that
                # does not report the counters, not a panel nobody touched -
                # derive._count keeps those apart. Where the key above is true
                # these are floors and not totals, for the reason derive._movement
                # gives; that key is why no third one is needed to say it.
                "grower_switch_edges_in_window": tenure["edges"],
                "grower_all_stops_in_window": tenure["allstops"],
            }
            if prev
            else None
        ),
        links=t.links.model_dump(),
        rgb_jpeg=rgb,
        thermal_png=store.latest_frame(device, "thermal"),
    )

    # The call is spent here, not when it comes back. A request that times out,
    # 429s or returns something unparseable has already cost the quota and
    # already has to be waited out, and recording only the ones that worked is
    # what let a failing model be retried on every poll forever - see
    # store.record_llm_call and the floor in scheduler.decide.
    store.record_llm_call(device, now_ts)
    try:
        out = await brain.diagnose(inp)
    except brain.BrainError:
        log.exception("brain failed (%s)", reason)
        return None

    control = scheduler.clamp_control(out.control)
    wake = scheduler.clamp_wake(out.wake_after_s, out.wake_when)

    rx = Prescription(
        rx_id=uuid.uuid4().hex[:16],
        issued_ts=now_ts,
        next_poll_s=scheduler.POLL_ACTIVE_S,
        want_frame=False,
        mode=_mode(t),
        control=control,
        display=render.build_display(
            control=control,
            head=out.head_ko,
            diagnosis=out.diagnosis_ko,
            # Both halves of the prose, because the card draws both now. This used
            # to reach only store.save_prescription(raw_model=...) - which it still
            # does - and a caveat the model wrote about its own evidence was
            # readable in SQLite and nowhere on the panel.
            notes=out.notes_ko,
            level=out.level,
            evidence=out.evidence,
            deciding=out.deciding,
            species=inp.species,
            current=inp.current,
            evidence_ts=store.frame_ts(device, "rgb") or now_ts,
            has_rgb=rgb is not None,
            # Both from a frame actually held when the judgment was made. The
            # caller that fetched them is the only place that can know.
            has_thermal=inp.thermal_png is not None,
            wake=wake,
            issued_ts=now_ts,
            prev=prev,
            window=inp.window,
        ),
    )
    store.save_prescription(rx, raw_model=out.model_dump(mode="json"), device=device)
    _last_event[device] = now_ts
    log.info("rx %s for %s (%s)", rx.rx_id, device, reason)
    return rx


async def _prescribe(t: Telemetry) -> Prescription:
    """The prescription path, and nothing else.

    Split out of the endpoint below so update mode - the one thing a response carries that is a
    one-shot signal rather than a restatement of the standing prescription - has somewhere to be
    stamped that is not inside a function whose every branch either stores the Prescription it
    just built or replays one that was stored earlier. See telemetry().
    """
    now_ts = scheduler.now()
    device = t.device

    if scheduler.telemetry_is_sane(t):
        store.save_telemetry(t, now_ts, derive.enrich(t))
    else:
        # Still answer: a device with a dead sensor node needs its prescription
        # to keep running just as much as a healthy one.
        log.debug("no usable sensors from %s", device)

    prev = store.latest_prescription(device)
    wake = None
    if prev is not None:
        raw = store.prescription_wake(prev.rx_id)
        if raw:
            wake = scheduler.clamp_wake(raw.get("after_s", 0), raw.get("when"))

    # One read of one fact. scheduler.decide() refuses to spend a call without a
    # model, and display.model_ready is what the panel badges its columns from,
    # and those two disagreeing is a panel attributing rule output to a model
    # that was never asked. Same argument as the comment forty lines down.
    model_ready = brain.is_configured()

    # Measured once, and handed to every path below - including the ones that never
    # reach a model. That is the whole point: window_summary() used to be called
    # only from inside _run_brain, so a server without a key summarised nothing and
    # the panel's 최근 구간 card was blank for the life of the install. The rows are
    # read separately from decide()'s above because the two ask different questions
    # - decide() wants the last hour whatever happened, this wants the span the
    # standing prescription has been in force for, cut at a reboot.
    ctx = _window_ctx(device, t, now_ts, prev)

    decision = scheduler.decide(
        now_ts=now_ts,
        # Two different clocks. The wake deadline is measured from when the
        # prescription in force was issued; the floor and the budget are
        # measured from when a call was last spent, which is a row that exists
        # even when the call failed and even when no prescription ever came of
        # it. See scheduler.decide.
        last_rx_ts=prev.issued_ts if prev else None,
        last_call_ts=store.last_llm_call_ts(device),
        wake=wake,
        rows=store.telemetry_since(device, now_ts - 3600),
        calls_today=store.llm_calls_since(device, now_ts - 86400),
        ask_now=t.ask_now,
        have_prescription=prev is not None,
        model_ready=model_ready,
    )

    frame_age = store.frame_ts(device, "rgb")
    need_frame = scheduler.want_frame(decision=decision, frame_ts=frame_age, now_ts=now_ts)

    # Ask for images first and diagnose on the next poll. Reasoning from a frame
    # captured minutes ago would put a stale picture next to fresh numbers, and
    # the evidence card claims they are from the same moment.
    if decision.should_call and need_frame:
        poll_s = scheduler.POLL_ACTIVE_S
        if prev is None:
            return _empty_prescription(now_ts, poll_s, want_frame=True,
                                       model_ready=model_ready, window=ctx.window)
        return prev.model_copy(
            update={"next_poll_s": poll_s, "want_frame": True, "mode": _mode(t),
                    "display": _restate(prev, model_ready, ctx.window)}
        )

    # No second is_configured() check: decide() already refused on it, and two
    # readings of one fact is how they come to disagree.
    if decision.should_call:
        async with _brain_lock:
            fresh = await _run_brain(device, t, now_ts, decision.reason, ctx=ctx)
        if fresh is not None:
            return fresh
        # The fast-poll window was opened by the frame upload that preceded this
        # call, and its whole premise was "a verdict is one poll away". The call
        # failed, so no verdict is coming and the premise is false. Leaving the
        # window open costs 20 polls per floor cycle waiting for an answer that
        # was already lost - the device drops straight back to the heartbeat.
        _last_event.pop(device, None)

    poll_s = scheduler.poll_interval_s(
        now_ts=now_ts, last_event_ts=_last_event.get(device), ask_now=t.ask_now
    )
    if prev is None:
        return _empty_prescription(now_ts, poll_s, want_frame=need_frame,
                                   model_ready=model_ready, window=ctx.window)
    # The model failed, or was not asked. Everything in prev is still the standing
    # prescription and is carried verbatim - except mode, which is not the
    # model's to state and is therefore restated from the switch position this
    # poll just reported, display.model_ready, for the same reason, and the window
    # block, which is a measurement of the span this prescription has been in force
    # for and therefore grows under it rather than being frozen at issue. See
    # _restate.
    return prev.model_copy(
        update={"next_poll_s": poll_s, "want_frame": need_frame, "mode": _mode(t),
                "display": _restate(prev, model_ready, ctx.window)}
    )


@v1.post("/telemetry", response_model=Prescription, dependencies=[Depends(auth)])
async def telemetry(t: Telemetry) -> Prescription:
    """The poll, plus whatever one-shot signal is waiting to ride out on its answer.

    The stamping happens here, at the outermost edge, and that placement is load-bearing rather
    than stylistic. Every path through _prescribe either saves the prescription it built or
    replays one read back off disk, so a flag written into a Prescription anywhere inside it
    would be persisted with the rest of the body and then replayed on every poll after - which
    is exactly the loop the signal is one-shot to prevent, arriving by a different road. Stamped
    on the copy that goes out, the field exists on the wire and nowhere else.

    take_update_mode() clears as it reads, so this is the one response that carries it; a device
    that misses this reply gets a normal one next poll and whoever wanted the update posts again.
    It hands back both halves of the arming at once - whether there is one, and whether it asked
    for a pull - because the two are one decision and reading them apart would let a second poll
    slip between them and take half of it.

    The node armings are on the same rail and are deliberately NOT consumed on a response that
    carries the panel's own. src/plantrx.cpp refuses to dispatch nodeota_request() when
    update_mode or firmware_pull is set, because a node update is watched by the panel and this
    panel is about to stand down and reboot - so taking the node flag here would spend a one-shot
    arming on a response that is going to ignore it, and the operator's request would vanish with
    no trace but a log line. Left armed, it is delivered on the panel's first poll after the
    reboot, which is exactly when somebody is in a position to watch it.
    """
    rx = await _prescribe(t)
    # Stored before any of the one-shot arming below, and outside _prescribe, because it is the
    # only thing on this route that is a fact rather than a decision: what three boards are
    # running. Absent on firmware that predates the field, which is why it is a test and not an
    # assumption - a panel that cannot say leaves the operator page reading 모름 rather than
    # claiming a version nobody reported.
    if t.images:
        store.save_device_fw(t.device, t.images, scheduler.now())
    armed, pull = store.take_update_mode(t.device)
    if armed:
        log.info("update mode: handing the flag to %s (%s)", t.device,
                 "pull" if pull else "push")
        return rx.model_copy(update={"update_mode": True, "firmware_pull": pull})

    cam, node = store.take_node_pull(t.device)
    if cam or node:
        log.info("node update: handing %s to %s", "+".join(
            r for r, on in (("cam", cam), ("node", node)) if on), t.device)
        return rx.model_copy(update={"node_pull_cam": cam, "node_pull_node": node})
    return rx


@v1.post("/frame", response_model=FrameAck, dependencies=[Depends(auth)])
async def frame(
    request: Request,
    x_device: str = Header(...),
    x_kind: str = Header(...),
) -> FrameAck:
    """Raw JPEG body. The device pushes because the camera sits on a LAN the
    server cannot reach - there is no pulling it."""
    if x_kind not in ("rgb", "thermal"):
        raise HTTPException(status_code=400, detail="kind must be rgb|thermal")
    blob = await request.body()
    if not blob or len(blob) > MAX_FRAME_BYTES:
        raise HTTPException(status_code=413, detail="bad frame size")
    store.save_frame(x_device, x_kind, blob, scheduler.now())
    _last_event[x_device] = scheduler.now()
    return FrameAck(ok=True)


@v1.post("/identify", response_model=Identification,
         response_model_exclude_none=True, dependencies=[Depends(auth)])
async def identify(request: Request) -> Identification:
    """Raw JPEG body, and the name of the plant in it: the 식별 button on the panel.

    The device used to do this itself and could not. mbedTLS wants two 16 KB
    record buffers out of internal DRAM, and there is no 32 KB of it free on a
    board whose PSRAM is already spoken for by the camera's decode buffers.
    Shrinking the negotiated record length or pointing the mbedTLS allocator at
    PSRAM would both be global policy changes made for the sake of one button.
    Pushing the frame here instead costs the device one plain HTTP POST on the
    LAN it is already polling over - and this was the last TLS user in the
    firmware, so moving it lands the board on plain HTTP everywhere.

    Deliberately separate from _resolve_species, which is the automatic path:
    that one runs on the poll, off the stored RGB frame, and prefers whatever
    the device already reported. This is somebody standing in front of the panel
    having just taken a photo, so it always spends a request and always answers.
    Failures come back as 200 with ok=false and a Korean reason rather than as a
    status code, because the device only parses bodies; the codes raised here
    are the framework's own refusals, which it reports for itself.
    """
    blob = await request.body()
    if not blob or len(blob) > MAX_FRAME_BYTES:
        raise HTTPException(status_code=413, detail="bad frame size")

    try:
        result, reason = await plantnet.identify_ex(blob)
    except Exception:
        # _unhandled at the bottom of this file answers every unhandled error
        # with a 200 and a prescription-shaped body. That is the right answer to
        # a poll and an unreadable one here: the device's scanner would find no
        # `ok` and no `reason` in it and draw an empty card, which is the single
        # outcome this endpoint exists to prevent. So the contract is closed
        # here instead, where the quota figures below are still in reach.
        # CancelledError is a BaseException and deliberately still propagates -
        # a client that hung up does not want an answer.
        log.exception("identify: unhandled below the endpoint")
        result, reason = None, "식별 서버 오류"

    # Read after the call, not before: the request just spent is the one the
    # operator is watching, and a figure taken beforehand would read one high.
    remaining = plantnet.quota_remaining()
    quota = plantnet.quota_total()
    ident = Identification(
        ok=result is not None,
        sci=result.sci if result else None,
        common=result.common if result else None,
        korean=result.korean if result else None,
        score=result.score if result else None,
        reason=None if result else reason,
        remaining=-1 if remaining is None else remaining,
        quota=-1 if quota is None else quota,
        measured=plantnet.quota_is_measured(),
    )
    log.info("identify: %d bytes -> %s (%d/%d left%s)", len(blob),
             result.sci if result else reason, ident.remaining, ident.quota,
             "" if ident.measured else ", upper bound")
    return ident


@v1.post("/device/{device}/update_mode", dependencies=[Depends(auth)])
def request_update_mode(device: str, pull: bool = False) -> dict:
    """Arm update mode on this panel's next poll.

    The reason this exists is that the other two ways into the mode both need somebody in the
    greenhouse: the button on the panel, and an espota push landing on a board that is still
    busy enough to fumble the handshake - which is the state the mode exists to fix. So the
    remote sequence is arm here, wait one poll interval, then upload into a board that has
    already stood its camera, its poll and its ESP-NOW radio down.

    `?pull=1` arms the other half of the same idea and removes the upload step with it: the
    panel stands down as before, then fetches the published image from this server itself. That
    is the version an operator can run from a phone, which is the version that is any use when
    the greenhouse is an hour away and the laptop with the toolchain is not.

    Nothing is delivered by this call. It writes the flag and returns; the device is behind NAT
    and only ever hears from the server as the answer to a poll it made itself. Note that
    nothing here checks that an image is even published - a pull armed against an empty
    firmware directory ends as a line on the panel saying so, and pinning that check to arming
    time would only mean checking it twice, minutes apart, and believing the earlier answer.

    Behind the auth dependency for the same reason it is useful: a panel in update mode is
    unusable for five minutes and leaves only by rebooting, so an unauthenticated caller could
    hold every device in the greenhouse in a takeover screen indefinitely.

    Sync rather than async deliberately - the write is blocking sqlite, and a sync handler runs
    on anyio's worker threadpool instead of stalling the event loop the telemetry polls share.
    See the connection-per-thread note at the top of store.py.
    """
    store.set_update_mode(device, pull)
    return {"ok": True, "device": device, "firmware_pull": pull}


@v1.post("/device/{device}/node_update", dependencies=[Depends(auth)])
def request_node_update(device: str, role: str) -> dict:
    """Arm "tell node `role` to update" on this panel's next poll.

    The same rail as update_mode above, for the same NAT reason and one more: the nodes are not
    on IP at all. The ESP32-CAM has WiFi but no route from here, and the sensor node has no WiFi
    running except during a download - both are reachable only over ESP-NOW, from the panel. So
    the only way to start a node update from outside the greenhouse is to leave a flag where the
    panel's next poll will pick it up and pass it on.

    `role` is cam or node. Refusing panel here is not an oversight: the panel updates itself
    through this file's other operator endpoint (`update_mode?pull=1`), which stands its camera,
    poll and radio down first. Accepting it here would arm a device to command itself, and the
    error message says which endpoint to use instead rather than making somebody read the source.

    Nothing is delivered by this call, and nothing here checks that an image is even published
    for that role - same trade as update_mode: the node's own "already current"/"nothing
    published" answer arrives on the panel minutes later, and checking at arming time would only
    mean checking twice and believing the earlier answer.

    Sync rather than async for the reason at the top of store.py: the write is blocking sqlite,
    and a sync handler runs on anyio's worker threadpool instead of stalling the event loop the
    telemetry polls share.
    """
    if role not in store.NODE_PULL_COLS:
        raise HTTPException(
            status_code=400,
            detail="role must be one of %s (the panel updates itself via update_mode?pull=1)"
                   % ", ".join(sorted(store.NODE_PULL_COLS)))
    store.set_node_pull(device, role)
    return {"ok": True, "device": device, "node_pull": role}


@v1.get("/firmware/latest", response_model=FirmwareManifest, dependencies=[Depends(auth)])
def firmware_latest(role: str = Depends(_role_q)) -> FirmwareManifest:
    """What is published for this role, so a board can work out whether it already has it.

    404 is the ordinary answer here, not an error path. Nothing published, or something
    published that is not an ESP32 application image, both land on it, and the caller treats
    them the same way it treats a refused connection: say so and change nothing. See
    firmware.manifest for why a wrong file is the operator's mistake to read in a log rather
    than a 500 claiming the server is broken. It is the ordinary answer twice over now that
    there are three roles: a deployment that has never published a cam image answers 404 to
    every cam request it will ever get, and that is a correctly configured server.

    Sync, like the operator endpoints above and for the same reason: this reads a file, and a
    sync handler does its blocking on anyio's worker threadpool instead of on the event loop
    the telemetry polls share.
    """
    m = firmware.manifest(role)
    if m is None:
        log.info("firmware: nothing published for %s; 404 to /firmware/latest", role)
        raise HTTPException(status_code=404, detail="no firmware published")
    return FirmwareManifest(**m)


@v1.get("/firmware/image", dependencies=[Depends(auth)])
def firmware_image(role: str = Depends(_role_q)) -> FileResponse:
    """The image itself, raw, with a Content-Length the device needs before it starts.

    Update.begin() has to be told how many bytes are coming before the first one arrives - the
    ESP32 commits to an OTA partition write up front - so a chunked response would leave the
    panel unable to start at all. Starlette's FileResponse stats the file as it sends and sets
    content-length from that, which is both the header the device needs and, because it is the
    stat of the handle actually being streamed, a length that describes the bytes that really
    go out rather than the ones the manifest promised.

    Gated on the same manifest() the endpoint above answers from, so the two cannot disagree
    about whether the published file is real firmware: a panel that skipped straight to this
    URL still cannot be handed something that is not an ESP32 image. The md5 inside manifest()
    is cached against the file's stat, so this second call is a stat and a 288-byte read, not a
    second pass over 2.5MB.

    A publish landing between the two requests is not defended against here and does not need
    to be: the device sets the manifest's md5 on the Update before it writes, so an image whose
    bytes stopped matching the manifest that described them is rejected on the device rather
    than booted.
    """
    m = firmware.manifest(role)
    if m is None:
        log.info("firmware: nothing published for %s; 404 to /firmware/image", role)
        raise HTTPException(status_code=404, detail="no firmware published")
    log.info("firmware: serving %s, %d bytes, elf %s", role, m["size"], m["elf_sha256"][:12])
    return FileResponse(firmware.image_path(role), media_type="application/octet-stream")


@v1.post("/nodelog", dependencies=[Depends(auth)])
def post_nodelog(batch: NodeLogBatch) -> dict:
    """Log lines the panel heard from its nodes, on their way to somewhere readable.

    The nodes never call this. They have no route to this server - one has no WiFi except during
    a download, the other has WiFi and no path in - so the line travels node -> panel over
    ESP-NOW and the panel posts a batch of them here. That is also why the control plane is
    ESP-NOW in the first place: a node that cannot get a DHCP lease is exactly the node somebody
    needs a log line out of, and a log path that shared the failure it was reporting on would go
    quiet at the only moment it mattered.

    The receive time is stamped here, once for the batch, rather than trusted from the body. The
    body's `ms` is the forwarding PANEL's millis() and not the node's - the node's clock never
    reaches this server at millisecond resolution - so it can order lines inside one batch and
    nothing wider than that. See schema.NodeLogLine and store.save_node_logs.

    Behind the same bearer dependency as everything else on /v1, which is what stops anyone on
    the LAN writing rows into the one table an operator reads to find out what a node did.
    """
    n = store.save_node_logs(batch.device, batch.lines, scheduler.now())
    return {"ok": True, "stored": n}


@v1.get("/nodelog", response_model=list[NodeLogRow], dependencies=[Depends(auth)])
def read_nodelog(role: Optional[str] = None,
                 limit: int = store.NODE_LOG_READ_DEFAULT) -> list[NodeLogRow]:
    """The newest stored lines, so the log is readable with curl and not only with sqlite3.

    This exists because the alternative is scp'ing the database off a Dokploy volume to answer
    "what did the camera say while it was updating", which is enough friction that nobody does
    it and the write path might as well not be there.

    `role` is an unchecked filter and not the validated ?role= the firmware endpoints take - see
    store.recent_node_logs for why refusing an unknown one here would hide exactly the rows worth
    looking at. `limit` is clamped in the store rather than here, so it is bounded no matter who
    calls.
    """
    return [NodeLogRow(**r) for r in store.recent_node_logs(role, limit)]


# The only route where a server-side bug must still read as an ordinary answer, and the reason the
# body below is shaped the way it is: `{rx_id, next_poll_s}` IS a Prescription (schema.py:578-584),
# which means something to exactly one caller. Built from the router's own prefix so it cannot
# drift away from where @v1.post("/telemetry") actually mounts.
_POLL_PATH = v1.prefix + "/telemetry"


@app.exception_handler(Exception)
async def _unhandled(request: Request, exc: Exception) -> JSONResponse:
    """Never 500 a polling device into a retry loop over a server-side bug - and never hand that
    answer to anything that is not the poll.

    A 200 from this server means "here is a valid Prescription", so returning one everywhere told
    every other client that a crash had succeeded. On one route that had teeth: the panel advances
    its nodelog cursor on ANY 2xx (src/nodelog.cpp:347, :432) and increments no counter while doing
    it, so a save_node_logs that raised took twenty log lines off the panel's ring, stored none of
    them, and left no trace at either end - precisely the hole the cursor rule at
    src/nodelog.cpp:19-23 exists to prevent. /v1/identify had already had to absorb its own
    exceptions to escape this (tests/test_identify_endpoint.py:148), which was the second witness.

    Everything that is not the poll now gets a real 500, which every client in this tree already
    handles as a loud failure rather than a silent success: the firmware endpoints' hand-rolled
    parsers refuse any non-2xx (src/fwpull.cpp:325-330) and the log uplink keeps its cursor and
    retries the same window (src/nodelog.cpp:425).

    HTTPException and request validation keep their own handlers - FastAPI matches the most
    specific one - so 404 "no firmware published" and the 422 the nodelog caps raise are unchanged.
    """
    log.exception("unhandled on %s", request.url.path)
    if request.url.path == _POLL_PATH:
        return JSONResponse(status_code=200, content={"rx_id": "error", "next_poll_s": 60})
    return JSONResponse(status_code=500, content={"detail": "internal error"})


app.include_router(v1)
# Mounted after v1 so the auth dependency below is the one already declared on it, and split the
# way admin.py's two routers are: the state route joins /v1 behind the same bearer as every other
# data route, and the page joins the app open, because it is a constant with nothing in it. Both
# decisions are visible here rather than in a decorator in that file, which is where somebody
# asking "what on this server does not need the secret" will look.
app.include_router(admin.state_router, prefix="/v1", dependencies=[Depends(auth)])
app.include_router(admin.page_router)
