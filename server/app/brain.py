"""The Gemini call: prompt assembly, structured output, and distrust of the result.

Pure function of its inputs - no SQLite, no FastAPI, no clock. Everything the
model sees arrives in BrainInput, so a bad diagnosis can be reproduced by
replaying one stored row.

Two things here are load-bearing and non-obvious.

The model does not answer in schema.Control. Control.policy is a dict[str, ...],
and the Gemini Developer API rejects any schema carrying additionalProperties
(measured: t_schema raises ValueError on schema.Control, and on list[dict]).
So the model fills in a flat _Wire model built from the contract's own Setpoint,
Schedule and WakeCondition, and _control() folds that back into a Control. The
model also never writes OnceAction.id or .before_ts: an idempotency key it
invented is not idempotent, and an expiry it invented is anchored to a clock it
cannot see.

Nothing the model returns is trusted. clamp_output() is the trust boundary, and
the limits it enforces are imported from scheduler rather than retyped - when
the two disagree, the server logs and displays one deadline while acting on
another, which reads as the UI lying about when it will look again.
"""

import hashlib
import json
import os
from datetime import datetime, timezone
from typing import Any, Collection, Literal, Optional

from pydantic import BaseModel, Field, ValidationError

from . import schema
from .scheduler import (
    MAX_INTERVAL_S,
    MAX_ONCE_SECONDS,
    MIN_HOLD_S,
    MIN_INTERVAL_S,
    VALID_METRICS,
    clamp_control,
)

DEFAULT_MODEL = "gemini-2.5-flash"

# A once-action the device picks up late is a once-action nobody asked for: ten
# minutes covers several poll intervals of a healthy device and expires long
# before one that dropped off wifi comes back.
ONCE_TTL_S = 600

# Thinking tokens count against max_output_tokens on the 2.5 series, so a tight
# cap buys a MAX_TOKENS finish with an empty body rather than a cheaper call.
# The ceiling is not a budget - only real usage is billed.
MAX_OUTPUT_TOKENS = 8192
REQUEST_TIMEOUT_MS = 60_000

# Deterministic beats varied here: the same readings twice should read the same
# way, and the grower is comparing today's card against yesterday's.
TEMPERATURE = 0.3


class BrainError(Exception):
    """The one exception this module raises.

    diagnose() raises rather than inventing a fallback prescription, because the
    device is already holding a good one: a failed call costs nothing as long as
    the caller keeps serving the last result, and a synthesised "everything is
    fine" would silently overwrite it.
    """


# --------------------------------------------------------------------------
# Inputs
# --------------------------------------------------------------------------


class BrainInput(BaseModel):
    """Everything the model sees. The dicts are passed through as-is; their
    shapes belong to store/derive and are not re-specified here."""

    now_ts: int
    species: Optional[dict] = None  # main._species_entry's shape; None = unidentified
    current: dict = {}  # latest readings, including derived vpd_kpa / leaf_air_dt_c
    # derive.window_summary since the last prescription. Its per-metric sample
    # counts are the metric vocabulary, the way actuator_intent's keys below are
    # the actuator one: readable_metrics() reads them off it, the prompt names
    # them, and `setpoints` is checked against them on the way back out. No
    # window measured yet is permissive rather than fatal, same as an empty
    # inventory.
    window: dict = {}
    # The panel's switch positions this poll, straight off Telemetry. Its key set
    # is the actuator vocabulary: what the prompt names as the inventory and what
    # `once` and `policy` are checked against on the way back out. Empty means a
    # firmware that does not report them, which is permissive rather than fatal.
    actuator_intent: dict[str, int] = {}
    last: Optional[dict] = None  # previous control block + its diagnosis
    links: dict = {}  # node/cam/thermal liveness
    rgb_jpeg: Optional[bytes] = None
    # PNG, and not for want of a JPEG encoder: the panel sends 32x24 of false
    # colour, where JPEG's 4:2:0 chroma would average the palette over 16x12
    # blocks and smear away the only signal the image carries.
    thermal_png: Optional[bytes] = None


def readable_metrics(window: Any) -> frozenset[str]:
    """The metrics this installation actually measures, read off the window.

    A metric is readable iff derive.window_summary reported a nonzero sample
    count for it - `stat["n"]`, the length of the value list it built, which is
    zero exactly when the sensor returned null for every row of the window. That
    count is the only evidence the server has: a dead BH1750, an unwired probe
    and a node that dropped off the bus are the same null on the wire, and none
    of the three can hold a band.

    Empty is "nothing measured yet", never "nothing measurable" - see
    clamp_output, which stands down on it.
    """
    metrics = window.get("metrics") if isinstance(window, dict) else None
    if not isinstance(metrics, dict):
        return frozenset()
    return frozenset(
        str(key)
        for key, stat in metrics.items()
        if isinstance(stat, dict)
        and isinstance(stat.get("n"), int)
        and not isinstance(stat.get("n"), bool)
        and stat["n"] > 0
    )


# --------------------------------------------------------------------------
# Outputs
# --------------------------------------------------------------------------


class BrainOutput(BaseModel):
    """The clamped result. The bounds live on the fields so that a path which
    skipped clamp_output() cannot construct one of these at all."""

    diagnosis_ko: str = Field(default="", max_length=120)
    # max_length counts characters, and the device's head buffer counts bytes -
    # 63 Hangul syllables are 189 of them, and would pass this. The bound is a
    # sanity net against a runaway string, not the device limit: render.py does
    # the byte clip against schema.JUDGE_HEAD_BYTES and is the only thing that
    # can, so do not read this number as the panel's.
    head_ko: str = Field(default="", max_length=63)
    level: schema.Level = "ok"
    # MetricKey values, at most schema.JUDGE_CHIPS_MAX of them - the row's chip
    # capacity, which _evidence() cuts to - the deciding one first. Neither field
    # carries a bound of its own: the pair's invariant - deciding is "" or
    # exactly evidence[0] - is held by _evidence(), and nothing else may write
    # either of them.
    evidence: list[str] = []
    deciding: str = ""
    confidence: float = Field(default=0.0, ge=0.0, le=1.0)
    control: schema.Control = schema.Control()
    wake_after_s: int = Field(default=MAX_INTERVAL_S, ge=MIN_INTERVAL_S, le=MAX_INTERVAL_S)
    wake_when: list[dict] = []  # schema.WakeCondition dumps: {metric, op, value, for_s}
    notes_ko: str = Field(default="", max_length=200)


# --------------------------------------------------------------------------
# Wire model - what the API is actually asked for
# --------------------------------------------------------------------------


class _WireOnce(BaseModel):
    actuator: str
    seconds: int


class _WirePolicy(BaseModel):
    """Control.policy is a dict, which the Developer API will not accept in a
    response schema. Same information, as a list."""

    actuator: str
    mode: Literal["auto", "off"]


class _Wire(BaseModel):
    diagnosis_ko: str
    head_ko: str
    level: schema.Level
    evidence_metrics: list[schema.MetricKey] = []
    # A "none" member rather than a nullable field: nullable enums are where the
    # Developer API's response schema support gives out, the same class of
    # problem as _WirePolicy's dict, and a model with no way to say "nothing
    # decided this" names a metric instead of leaving the key out. Composed from
    # MetricKey so the metric spelling here cannot drift from the contract's.
    deciding_metric: Literal[schema.MetricKey, "none"]
    notes_ko: str
    confidence: float
    setpoints: list[schema.Setpoint] = []
    schedules: list[schema.Schedule] = []
    once: list[_WireOnce] = []
    policy: list[_WirePolicy] = []
    wake_after_s: int
    wake_when: list[schema.WakeCondition] = []


# --------------------------------------------------------------------------
# Prompt
# --------------------------------------------------------------------------

# Instructions in English, output in Korean: the constraints below are followed
# more reliably stated in English, and none of this text is ever displayed.
SYSTEM_PROMPT = """\
You are the diagnostic brain of a small smart-greenhouse controller, "AI PlantRx".
Each cycle you get the current readings, a summary of the window since your last
prescription, the prescription you issued then, and usually a visible and a
thermal camera frame. You return one prescription: a short Korean diagnosis and
the finding it rests on, target bands for the device to hold, and when to look
again.

HARDWARE FACTS. Getting these wrong is what produces confidently wrong advice.

* leaf_max_c is the HOTTEST PIXEL of a 32x24 thermal frame. It is the maximum of
  the entire scene, not leaf temperature. Anything warm in view - a grow lamp, a
  heater, a hand, a power supply - dominates it completely. Read it as leaf
  temperature only if the visible frame shows nothing else warm in the field of
  view, and say in notes_ko that you did. leaf_air_dt_c is derived from it and
  carries exactly the same caveat.
* The thermal image is a false-colour render whose palette is applied on the
  sensor node before the frame is broadcast, and the mapping is not carried on the
  wire: one frame is 32x24 RGB565 pixels plus a single scene-peak float
  (include/camprov.h:90-101), and nothing downstream of the node - not the panel,
  not this server - can turn a colour back into a temperature. Whether the node
  stretches per frame or holds a fixed range is not stated anywhere either side
  can read, so two frames cannot be compared by colour: not because the stretch is
  known to move, but because nothing here knows that it does not. Colour tells you
  where the hot region is, never how hot it is. Absolute temperature comes only
  from the numeric fields.
* lux and soil moisture do not exist on this installation. The light sensor is
  dead and no soil probe is wired, so both are permanently null. Do not reason
  from them, do not estimate them, and never ask for them.
* Which metrics can be measured is a property of this installation, not of the
  plant: the observations list under `readable_metrics` exactly the metrics that
  returned at least one sample in the window just ended. `setpoints` may name
  only those, spelled exactly as listed - a band on any other metric is dropped
  on arrival and the grower's card comes back without it, so a target nobody can
  measure is a target nobody can hold or score. When `readable_metrics` is null
  nothing has been measured yet - a board on its first poll, or one whose sensor
  node has not come up - and then no band is dropped; prescribe as usual and say
  in notes_ko that the window was empty.
* No actuators are connected: the relays have not been delivered. Nothing will
  physically move, so this prescription cannot act on the house.
* Your setpoints are NOT inert, though, and this is the part worth being careful
  about. The panel reads them and judges every live reading against them: each
  band you send becomes the amber-or-green threshold on that metric's tile, the
  two marks drawn on its bar, and the boundary the panel's own threshold rule
  files its findings against. A grower looking at the wall is looking at your
  numbers.
* Omitting a band is therefore not "no opinion". The panel keeps a compiled-in
  fallback for every metric - roughly 0.5-1.2 kPa VPD, 18-28 degC, 40-70 %RH, a
  400 ppm CO2 floor - and it judges against that instead, marking the tile as its
  own rather than yours. So a metric you leave out is a metric the panel decides
  by itself. Send a band when you have a view; leave it out when the panel's
  generic one is genuinely better than anything this plant's data supports.
* The panel does own a fixed set of switches, listed in the observations under
  `actuator_inventory` with their current positions under `actuator_intent`.
  Those numbers are 0..100 and they are the GROWER'S OWN SWITCH POSITIONS, never
  measurements: the fan carries its speed percentage, every other switch reads 0
  or 100. A switch held on still does not mean anything ran, so do not report it
  as an action having taken effect. `once` and `policy` may name only the
  actuators in `actuator_inventory`, spelled exactly as listed - any other name
  is dropped on arrival and the action is silently lost, and a name this panel
  does not own would be drawn on its wall as a device that is not there.
  When `actuator_inventory` is null the panel reported no switches at all: issue
  no `once` and no `policy`, and say what you would have asked for in notes_ko.
* The device clock may read zero before it syncs. Use the timestamps given to
  you here and ignore any time you think you can infer from the images.

WHAT THE PANEL WAS ACTUALLY DOING. These live on `last_prescription` and they say
whether the window you are about to score is evidence about your own bands. Each
one was added because the answer changes what a careful reading concludes; none of
them is decoration.

* `running_on_device` is which prescription the panel reports holding, compared
  against the one you are being shown. "current" - it is running your last bands,
  and the window is evidence about them. "behind" - it is still running an OLDER
  prescription, so the window was scored against THAT one's bands and says nothing
  about the ones you are looking at; do not read it as your bands having failed.
  "lost" - the panel reports no prescription at all, usually a reboot, so nothing
  was held and in_band_pct comes back null rather than zero. "unknown" - the panel
  named an id this server never issued to it, which is a panel we cannot account
  for; treat its window as unattributable. "none" - nothing has ever been issued.
  When it is anything but "current", say so in notes_ko rather than claiming an
  outcome.
* `device_restarted_in_window` is true when the board's own clock went backwards
  inside the window. The readings from before that restart are excluded already -
  a panel that rebooted dropped the bands it was holding, so averaging across the
  gap would credit a prescription with time it was not in force. Null means the
  firmware did not report a clock, so it is unanswerable, not false.
* `grower_switch_edges_in_window` and `grower_all_stops_in_window` count what the
  grower's hands did between polls. `actuator_intent` is one snapshot per poll, so
  a switch turned on and off again is in none of them, and 전체 정지 taken back
  inside its undo window is in none of them either. A nonzero all-stop count is
  somebody reaching for the one control that means they disagreed with you: weigh
  the window accordingly and say in notes_ko that it was overruled. Null is a
  firmware that does not report the counters - NOT a panel nobody touched. Where
  `device_restarted_in_window` is true these are floors rather than totals,
  because presses made before the reboot were never reported.

HOW MUCH OF THE WINDOW YOU ACTUALLY SAW. `window` describes a stretch of time, and
the device is not guaranteed to have been on the air for all of it. These fields
say how much of it is evidence, and they are the difference between a number you
can act on and one that only looks like a measurement.

* `span_s` is the wall-clock length of the window. `covered_s` is how much of that
  length the device was actually reporting for, and `n_rows` is how many samples
  it took. A device offline for most of an hour still produces a window an hour
  long: span is the question, covered is the answer.
* When `covered_s` is far below `span_s`, per-metric `in_band_pct` comes back null
  rather than a percentage. That is deliberate and it is not a missing feature: a
  band held over four minutes of a sixty-minute window is not a band held for the
  window, and quoting a percentage measured over a sliver as though it described
  the whole would understate every dropout as a failure to hold. Read null there
  as "not enough history to say", never as zero, and never as a band that missed.
  `min`/`mean`/`max` are still real: they are what the samples that DID arrive say.
* `interventions` is the same pair as the two `grower_*` counts above, scoped to
  this window's rows. `counter_reset` true means a counter was seen going
  backwards, so `edges` and `allstops` beside it are floors rather than totals.
* Judge coverage before you judge the plant. A window you barely saw is a reason to
  re-issue and say so in notes_ko, not a reason to escalate against numbers that
  were never there.

JUDGEMENT.

* If species is null the plant has not been identified. Give generic advice and
  do not name a species from the image; a separate identification path owns that
  card and a guess here would contradict it. When it is not null, its `source`
  says who identified it: "device" is the panel's own camera-side run, already
  drawn on the wall in front of the grower. Reason about the plant it names and
  do not re-identify from the frame or correct the name - that card is not
  yours, whichever side filled it in.
* If a frame is missing or a link is down, say what you could not see instead of
  describing it. Lower confidence accordingly.
* Prefer changing nothing. Re-issuing last cycle's bands is the right answer
  whenever the window shows them being held.

OUTPUT RULES.

* diagnosis_ko: one plain Korean sentence, at most 120 characters, describing the
  plant's state right now. No preamble, no markdown, no bullet points.
* head_ko: the same finding as a short noun phrase, at most 20 Hangul syllables,
  no trailing period. It is drawn as one bold 14 px line one card column wide, so
  a sentence wraps and the row stops being glanceable. Match the register of the
  device's own rule-written headings: "생육 조건 양호", "수분 스트레스 감지",
  "증산 과다 경향", "증산 정체, 과습 경향", "엽온 상승, 기공 폐쇄 의심", "CO2 부족".
  Never put a sensor reading in it. The server prints the numbers itself, from
  what it measured; a number you retype is a number you can get wrong, and a
  wrong number beside a right verdict is the one failure the grower cannot catch
  from the panel.
* level: ok, warn or alert. It is what colours the row's badge, so set it from
  the severity of the finding, not from how sure you are - confidence is its own
  field.
* evidence_metrics: at most 5, the deciding one first, and only the metrics that
  bear on this finding. Evidence is selected, not dumped: a row that says
  "CO2 부족" gains nothing from a humidity reading, and attaching every sensor to
  every row destroys the reader's ability to tell which number caused the
  verdict. The metric names are the same ones setpoints uses.
* deciding_metric: the one reading the verdict turned on, or "none" when nothing
  is wrong and the row is only reporting a healthy state - the device's own
  healthy row deliberately highlights no number.
* notes_ko: at most 200 characters of Korean - the evidence you used and the
  reading you rejected. Drawn on the card as the paragraph under diagnosis_ko, so
  it is read by the grower and not only by the log.
* CHARACTER SET. Korean text may contain only Hangul syllables, ASCII, and these
  punctuation marks: \u00b7 \u00b1 \u00d7 \u00b0 \u2013 \u2014 \u2018 \u2019 \u201c \u201d \u2022 \u2026 \u2192 \u2103
  Nothing else, ever. No emoji, no Chinese characters, no kana, no other symbols.
  The display font has no glyph for anything else and draws a blank box in its
  place. Use \u2103 for temperature and % for percent.
* setpoints: bands for the device to hold, from vpd_kpa, air_c, rh_pct, co2_ppm,
  leaf_air_dt_c only, and of those only the ones `readable_metrics` lists - see
  HARDWARE FACTS. Set lo and hi for a band, or leave one null for a one-sided
  limit. Omit any metric you have no opinion about: an invented band is worse
  than no band, because the device will hold it.
* once: only when something must happen within minutes. 60 seconds maximum -
  anything longer is discarded rather than shortened, so express a long action as
  a schedule instead.
* wake_after_s: when to reconsider, between 300 and 21600 seconds. Short while
  something is moving, long while it is stable; every wake costs a model call.
* wake_when: at most 3 threshold conditions, each with for_s of 300 seconds or
  more, so a reading oscillating around the threshold cannot storm you awake.
* confidence: 0.0 to 1.0, and it is displayed. A missing frame, a dead link or a
  contaminated thermal maximum are all reasons to be honest and go lower.
"""


def _payload(inp: BrainInput) -> str:
    """The observation block, as JSON the model reads verbatim."""
    when = datetime.fromtimestamp(inp.now_ts, tz=timezone.utc).isoformat(timespec="seconds")
    body = {
        "server_now_ts": inp.now_ts,
        "server_now_utc": when,  # UTC: no greenhouse timezone is plumbed through yet
        "species": inp.species,
        # The closed actuator vocabulary, and the switch positions its keys were
        # read off. Sent as the panel reported them rather than written into the
        # system prompt: a board that owns six devices must not be told it has
        # seven, and null here is the firmware that reports none.
        "actuator_inventory": sorted(inp.actuator_intent) or None,
        "actuator_intent": inp.actuator_intent or None,
        "current": inp.current,
        # The metrics that actually returned readings in the window just ended,
        # and by that key set the vocabulary `setpoints` is validated against on
        # the way back out. Measured rather than declared: the panel never says
        # which of its sensors work, and a probe that has stopped reporting
        # cannot hold a band whatever its datasheet says. Null is a window with
        # nothing measured in it, where nothing is gated - see _setpoints.
        "readable_metrics": sorted(readable_metrics(inp.window)) or None,
        "window_since_last_prescription": inp.window,
        "last_prescription": inp.last,
        "links": inp.links,
    }
    return "OBSERVATIONS\n" + json.dumps(body, ensure_ascii=False, indent=1, default=str)


def _contents(inp: BrainInput) -> list:
    """Text and inline image parts. Frames are ~60KB, well under the inline
    request limit, so the Files API would only add a round trip and a lifetime
    to manage."""
    from google.genai import types

    out: list[Any] = [_payload(inp)]

    # An absent frame is stated rather than omitted. Left unmentioned, the model
    # will describe leaves it never saw.
    if inp.rgb_jpeg:
        out.append("RGB frame, visible light, captured this cycle:")
        out.append(types.Part.from_bytes(data=inp.rgb_jpeg, mime_type="image/jpeg"))
    else:
        out.append("No RGB frame this cycle. Do not describe the plant's appearance.")

    if inp.thermal_png:
        out.append(
            "Thermal frame, the sensor's native 32x24, false colour. The palette "
            "mapping is applied on the node and is not on the wire, so no colour "
            "here decodes to a temperature:"
        )
        out.append(types.Part.from_bytes(data=inp.thermal_png, mime_type="image/png"))
    else:
        out.append("No thermal frame this cycle.")

    out.append(
        "Diagnose the plant and prescribe. Weigh the window summary against the "
        "last prescription: say whether what you asked for last time worked."
    )
    return out


# --------------------------------------------------------------------------
# Clamping - the trust boundary
# --------------------------------------------------------------------------

_OP_ALIASES = {
    ">": "gt",
    ">=": "gt",
    "above": "gt",
    "over": "gt",
    "<": "lt",
    "<=": "lt",
    "below": "lt",
    "under": "lt",
}


def _fit(text: Any, limit: int) -> str:
    """Collapse to one line and cut to `limit` characters.

    Slicing a str is codepoint-wise, and the character set is precomposed Hangul
    plus ASCII, so there is no sequence to cut in half. The whitespace collapse
    matters more: a newline in a diagnosis breaks a fixed-height card.
    """
    s = " ".join(str(text or "").split())
    if len(s) <= limit:
        return s
    return s[: limit - 1].rstrip() + "\u2026"


def _confidence(raw: Any) -> float:
    try:
        v = float(raw)
    except (TypeError, ValueError):
        return 0.0
    # Asked for 0..1, a model will still sometimes answer 85. Clamping that to
    # 1.0 would display total certainty, which is the opposite of what it said.
    if 1.0 < v <= 100.0:
        v /= 100.0
    return min(1.0, max(0.0, v))


def _rows(raw: Any) -> list[dict]:
    """The list-of-objects fields, as dicts, or nothing.

    Both halves are checked rather than assumed: a field the model answered with
    a bare string still iterates, into characters, and dict("n") then raises
    instead of dropping the junk. Rows are the caller's own dicts, not copies -
    read them, do not write to them.
    """
    if not isinstance(raw, (list, tuple)):
        return []
    return [
        item.model_dump() if isinstance(item, BaseModel) else item
        for item in raw
        if isinstance(item, (BaseModel, dict))
    ]


def _wake_when(raw: Any) -> list[dict]:
    out: list[dict] = []
    for d in _rows(raw):
        op = _OP_ALIASES.get(str(d.get("op", "")).strip().lower(), d.get("op"))
        try:
            # WakeCondition owns the vocabulary, so an unmeasured metric or an
            # unknown operator is rejected here without a second list to keep in
            # step with schema.py.
            cond = schema.WakeCondition.model_validate({**d, "op": op})
        except ValidationError:
            continue
        cond.for_s = max(MIN_HOLD_S, int(cond.for_s))
        out.append(cond.model_dump())
    return out


def _setpoints(raw: Any, readable: frozenset[str]) -> list[schema.Setpoint]:
    """The bands, less the ones naming a metric this installation cannot read.

    The same gate as _once, one field over. A band is a promise the device is
    asked to hold, and holding is measured: a target on a metric that returned
    no sample all window can never be scored, never be drawn as held or missed,
    and only costs a row on the card that a band with a reading behind it could
    have had. The dead BH1750 is the plain case - "조도 800-1200 lx" names an
    instrument this board does not have.

    An empty readable set is a window with nothing measured in it - a fresh
    boot, an empty database, a sensor node that has not come up - and not a
    board with no sensors. Gating on it would drop every band on the first poll
    of a new install, which is worse than the bug this exists to fix, so it
    stands down exactly the way an empty actuator inventory does.

    A band whose bounds cross is dropped for a harder reason than an unreadable
    key: nothing can be inside lo=30, hi=20, so the device holds a target it can
    never hit. That is not an unhelpful band, it is a permanent false verdict -
    the tile stays amber for as long as the prescription lives, in_band_pct comes
    back 0.0 for a window that may have been perfect, and the next cycle reads
    that zero as its own band having failed and escalates against it. Two
    transposed numbers, and every downstream reader agrees on the wrong answer.
    Dropped rather than swapped: lo=30/hi=20 does not say which of the two the
    model meant, and a guess would put a bound on the wall that nobody wrote.
    The metric then draws unbanded and grey - no band, no verdict - which is the
    state the panel already has a word for.
    """
    out: list[schema.Setpoint] = []
    for sp in _models(raw, schema.Setpoint):
        if readable and sp.key not in readable:
            continue
        if sp.lo is not None and sp.hi is not None and sp.lo > sp.hi:
            continue
        out.append(sp)
    return out


def _policy(raw: Any, inventory: frozenset[str]) -> dict:
    """Accepts the wire list of {actuator, mode} or an already-folded dict."""
    pairs = (
        raw.items()
        if isinstance(raw, dict)
        else ((d.get("actuator"), d.get("mode")) for d in _rows(raw))
    )
    out: dict[str, str] = {}
    for raw_name, mode in pairs:
        name = str(raw_name or "").strip()
        if not name or mode not in ("auto", "off"):
            continue
        # Same gate as _once, for the same reason: a policy names a device on the
        # panel's control page, and one the panel does not own is a row the
        # grower cannot match to anything in front of them.
        if inventory and name not in inventory:
            continue
        out[name] = mode
    return out


def _once(
    raw: Any, now_ts: int, inventory: frozenset[str]
) -> list[schema.OnceAction]:
    out: list[schema.OnceAction] = []
    for i, d in enumerate(_rows(raw)):
        actuator = str(d.get("actuator", "")).strip()
        try:
            seconds = int(d.get("seconds", 0))
        except (TypeError, ValueError):
            continue
        # Dropped, not shortened. A request for ten minutes of misting cut down
        # to sixty seconds is a different action than the one that was reasoned
        # about, and it would be logged as if it were the same one.
        if not actuator or seconds <= 0 or seconds > MAX_ONCE_SECONDS:
            continue
        # Dropped for the same reason, and only against an inventory the panel
        # actually declared: the model invents actuator names, and an invented
        # one reaching the device puts a machine that does not exist on the wall
        # next to a claim that it ran. An empty inventory is an older firmware
        # that reports no switches, and gating on a field the device may not send
        # would turn a firmware skew into the loss of every action.
        if inventory and actuator not in inventory:
            continue
        # Content-derived so a retried call produces the same id and the device
        # dedupes it instead of running the action twice.
        ident = hashlib.blake2s(
            f"{now_ts}:{i}:{actuator}:{seconds}".encode(), digest_size=6
        ).hexdigest()
        out.append(
            schema.OnceAction(
                id=ident,
                actuator=actuator,
                seconds=seconds,
                before_ts=now_ts + ONCE_TTL_S,
            )
        )
    return out


def _models(raw: Any, cls: type) -> list:
    """Validate each item against a contract model, dropping what does not fit."""
    out = []
    for item in raw if isinstance(raw, (list, tuple)) else ():
        if isinstance(item, cls):
            out.append(item)
            continue
        try:
            out.append(cls.model_validate(item))
        except ValidationError:
            continue
    return out


def _level(raw: Any) -> str:
    """ok / warn / alert, defaulted rather than rejected.

    The badge is drawn by looking the level up, so a fourth word does not draw a
    fourth colour - it draws nothing, and the row joins the log with its severity
    silently missing. "ok" is the quiet default: a badge we invented is a
    severity the model never assigned.
    """
    v = str(raw or "").strip().lower()
    return v if v in ("ok", "warn", "alert") else "ok"


def _evidence(raw: Any, deciding_raw: Any) -> tuple[list[str], str]:
    """The metrics behind one judgment row, the deciding one first.

    Returned as a pair because the invariant belongs to the pair: `deciding` is
    "" or exactly evidence[0], and it holds by construction - the deciding metric
    is filtered out of the list and put back at its head, so there is no second
    ordering for the two to disagree about.

    Naming a deciding metric and then leaving it out of the list is the common
    malformed answer, and it is repaired by inserting rather than by dropping:
    the row exists to say which number the verdict turned on, and dropping that
    over a bookkeeping slip costs the row its point.
    """
    picked: list[str] = []
    for item in raw if isinstance(raw, (list, tuple)) else ():
        key = str(item or "").strip()
        if key in VALID_METRICS and key not in picked:
            picked.append(key)

    # One test for the wire's literal "none" and for anything invented alike.
    deciding = str(deciding_raw or "").strip()
    if deciding not in VALID_METRICS:
        deciding = ""
    else:
        picked = [deciding, *(k for k in picked if k != deciding)]

    # Cut last, so the insert above cannot be undone by it, and cut to the row's
    # chip capacity rather than to a number of our own.
    return picked[: schema.JUDGE_CHIPS_MAX], deciding


def clamp_output(
    raw: dict,
    now_ts: int,
    inventory: Collection[str] = (),
    readable: Collection[str] = (),
) -> BrainOutput:
    """Fold a raw model answer into a BrainOutput that is inside every limit.

    Takes a dict rather than a _Wire so the same path serves the structured
    result, a hand-repaired JSON body, and a stored prescription being replayed.
    Both layouts are accepted: the flat wire one, and a BrainOutput-shaped one
    with the control fields nested under "control".

    `inventory` is the actuator names the panel declared in this poll's
    Telemetry.actuator_intent. Empty is the older firmware that declares none,
    and then nothing is gated on it - see _once.

    `readable` is the metric names that returned samples in this poll's window,
    as readable_metrics() reads them off it. Empty is a device with no measured
    window at all, and then nothing is gated on it either - see _setpoints. The
    two vocabularies are independent: one names machines the panel owns, the
    other names instruments it can read, and neither stands in for the other.
    """
    src = raw.get("control") if isinstance(raw.get("control"), dict) else raw
    known = frozenset(inventory)
    measured = frozenset(readable)

    control = schema.Control(
        setpoints=_setpoints(src.get("setpoints"), measured),
        schedules=_models(src.get("schedules"), schema.Schedule),
        once=_once(src.get("once"), now_ts, known),
        policy=_policy(src.get("policy"), known),
    )

    # The outer interlock, applied here too: what gets logged and drawn is then
    # exactly what the device will be handed. clamp_control is idempotent, so
    # the caller running it again on the way out costs nothing.
    control = clamp_control(control)

    # Matches scheduler.clamp_wake: no opinion means fall back to the heartbeat
    # ceiling rather than to a guess of our own.
    try:
        after = int(raw.get("wake_after_s") or MAX_INTERVAL_S)
    except (TypeError, ValueError):
        after = MAX_INTERVAL_S

    # Wire spelling first, stored spelling second: this path also takes a
    # prescription being replayed, which arrives under BrainOutput's names, and
    # reading only the wire keys would strip a replayed row of its chips.
    evidence, deciding = _evidence(
        raw.get("evidence_metrics", raw.get("evidence")),
        raw.get("deciding_metric", raw.get("deciding")),
    )

    return BrainOutput(
        diagnosis_ko=_fit(raw.get("diagnosis_ko"), 120),
        head_ko=_fit(raw.get("head_ko"), 63),
        level=_level(raw.get("level")),
        evidence=evidence,
        deciding=deciding,
        confidence=_confidence(raw.get("confidence")),
        control=control,
        wake_after_s=min(MAX_INTERVAL_S, max(MIN_INTERVAL_S, after)),
        wake_when=_wake_when(raw.get("wake_when")),
        notes_ko=_fit(raw.get("notes_ko"), 200),
    )


# --------------------------------------------------------------------------
# The call
# --------------------------------------------------------------------------


def _api_key() -> str:
    # Read per call, not at import: the container sets this before start, and a
    # rotated key should take effect without a restart.
    return os.environ.get("GEMINI_API_KEY", "").strip()


def is_configured() -> bool:
    return bool(_api_key())


def _model_name() -> str:
    return os.environ.get("PLANTRX_MODEL", "").strip() or DEFAULT_MODEL


def _sdk():
    """Imported lazily so this module loads on a box with no SDK and no key -
    main.py has to be able to import it to report that it is unconfigured."""
    try:
        from google import genai
        from google.genai import errors, types
    except ImportError as e:  # pragma: no cover - deployment error, not a code path
        raise BrainError("google-genai is not installed") from e
    return genai, types, errors


_cached_client: Optional[tuple] = None


def _client(key: str):
    """One client, and therefore one HTTPS connection pool, per key. A race here
    just builds a second one and drops it."""
    global _cached_client
    if _cached_client is None or _cached_client[0] != key:
        genai, _, _ = _sdk()
        # api_key passed explicitly: the SDK would otherwise prefer GOOGLE_API_KEY
        # from the environment, and a stray one in the container would quietly
        # take over from the key this server is configured with.
        _cached_client = (key, genai.Client(api_key=key))
    return _cached_client[1]


def _stop_reason(resp: Any) -> str:
    """Why an empty response was empty - safety block, token ceiling, or nothing
    generated at all."""
    feedback = getattr(resp, "prompt_feedback", None)
    blocked = getattr(feedback, "block_reason", None) if feedback else None
    if blocked:
        return f"prompt blocked: {blocked}"
    candidates = getattr(resp, "candidates", None) or []
    if not candidates:
        return "no candidates returned"
    return f"finish_reason={getattr(candidates[0], 'finish_reason', 'unknown')}"


async def diagnose(inp: BrainInput) -> BrainOutput:
    """One prescription. Raises BrainError on any failure; never an SDK type."""
    key = _api_key()
    if not key:
        raise BrainError("GEMINI_API_KEY is not set")

    _, types, errors = _sdk()
    config = types.GenerateContentConfig(
        system_instruction=SYSTEM_PROMPT,
        response_mime_type="application/json",
        response_schema=_Wire,
        temperature=TEMPERATURE,
        max_output_tokens=MAX_OUTPUT_TOKENS,
        http_options=types.HttpOptions(timeout=REQUEST_TIMEOUT_MS),  # milliseconds
    )

    try:
        resp = await _client(key).aio.models.generate_content(
            model=_model_name(), contents=_contents(inp), config=config
        )
    except errors.APIError as e:
        raise BrainError(f"gemini call failed: {e}") from e
    except Exception as e:
        # httpx and aiohttp errors are not wrapped by the SDK, and a transport
        # timeout is the most likely failure of the lot.
        raise BrainError(f"gemini call failed: {type(e).__name__}: {e}") from e

    parsed = getattr(resp, "parsed", None)
    if isinstance(parsed, _Wire):
        return clamp_output(
            parsed.model_dump(),
            inp.now_ts,
            inp.actuator_intent,
            readable_metrics(inp.window),
        )

    # .parsed is None whenever validation failed, which includes a body truncated
    # at the token ceiling. The text is still worth one attempt before giving up.
    text = getattr(resp, "text", None)
    if not text:
        raise BrainError(f"gemini returned no content ({_stop_reason(resp)})")
    try:
        raw = json.loads(text)
    except ValueError as e:
        raise BrainError(f"gemini returned unparseable JSON ({_stop_reason(resp)})") from e
    if not isinstance(raw, dict):
        raise BrainError(f"gemini returned {type(raw).__name__}, expected an object")
    return clamp_output(
        raw, inp.now_ts, inp.actuator_intent, readable_metrics(inp.window)
    )
