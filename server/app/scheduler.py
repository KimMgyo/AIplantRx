"""When to wake the model, and what a prescription is allowed to ask for.

The model picks its own next wake ("2 hours, or if VPD holds above 1.8 for ten
minutes"), which is the right instinct - a stable greenhouse should not be
re-diagnosed every minute, and a struggling one should be. But a self-scheduling
loop has two failure modes and both are cheap to prevent from out here:

  silence  - "wake me in 24h" while conditions change underneath
  storm    - "wake me above 25 C" while the reading oscillates around 25

So the model proposes and this module disposes: a floor and a ceiling on the
interval, a minimum hold time on every threshold, and a daily call budget. The
same shape as the actuator interlocks on the device - the thinking part is never
the part that gets to remove a limit.
"""

import time
from dataclasses import dataclass
from typing import Optional

from .schema import Control, OnceAction, Setpoint, Telemetry

# Interval bounds. The floor keeps a flapping threshold from becoming a bill;
# the ceiling is a heartbeat, so a bad "wake me tomorrow" cannot strand us.
MIN_INTERVAL_S = 300
MAX_INTERVAL_S = 6 * 3600
# A threshold must hold this long before it counts. Without it, one noisy sample
# is enough to trigger.
MIN_HOLD_S = 300
DAILY_CALL_BUDGET = 60

# Poll cadence handed back to the device. Idle is cheap; the fast rate is used
# for a short burst after anything happens so a user action feels immediate.
# The device is behind NAT and never listens, so this interval IS the push
# latency - there is nothing else to tune.
POLL_IDLE_S = 60
POLL_ACTIVE_S = 3
POLL_ACTIVE_WINDOW_S = 60

VALID_METRICS = {"vpd_kpa", "air_c", "rh_pct", "co2_ppm", "leaf_air_dt_c"}
# An actuator can be held off from here but never forced on: forcing hardware on
# from across the internet is exactly what the device's interlocks exist to stop.
MAX_ONCE_SECONDS = 60


@dataclass
class WakeSpec:
    """The model's answer to "when should I look again"."""

    after_s: int
    when: list[dict]  # {metric, op, value, for_s}

    def deadline(self, from_ts: int) -> int:
        return from_ts + self.after_s


@dataclass
class Decision:
    should_call: bool
    reason: str


def clamp_wake(after_s: int, when: Optional[list[dict]]) -> WakeSpec:
    after = max(MIN_INTERVAL_S, min(MAX_INTERVAL_S, int(after_s or MAX_INTERVAL_S)))
    out: list[dict] = []
    for w in when or []:
        metric = str(w.get("metric", ""))
        # schema.WakeOp is "gt"/"lt". ">"/"<" are tolerated because they are the
        # obvious thing to write by hand, and silently dropping them emptied
        # every wake condition once already.
        op = {">": "gt", "<": "lt"}.get(str(w.get("op", "")), str(w.get("op", "")))
        if metric not in VALID_METRICS or op not in ("gt", "lt"):
            continue  # a metric we do not measure can never fire; drop it
        try:
            value = float(w["value"])
        except (KeyError, TypeError, ValueError):
            continue
        out.append(
            {
                "metric": metric,
                "op": op,
                "value": value,
                "for_s": max(MIN_HOLD_S, int(w.get("for_s", MIN_HOLD_S))),
            }
        )
    return WakeSpec(after_s=after, when=out[:4])


def clamp_control(c: Control) -> Control:
    """Strip anything a prescription is not allowed to contain.

    Setpoints with an inverted or absent band are dropped rather than repaired:
    a band we had to guess at is worse than no band, because the device would
    hold it.
    """
    setpoints: list[Setpoint] = []
    for sp in c.setpoints:
        if sp.key not in VALID_METRICS:
            continue
        if sp.lo is None and sp.hi is None:
            continue
        if sp.lo is not None and sp.hi is not None and sp.lo > sp.hi:
            continue
        setpoints.append(sp)

    once: list[OnceAction] = []
    for a in c.once:
        if a.seconds <= 0:
            continue
        once.append(
            OnceAction(
                id=a.id,
                actuator=a.actuator,
                seconds=min(MAX_ONCE_SECONDS, a.seconds),
                before_ts=a.before_ts,
            )
        )

    policy = {k: v for k, v in c.policy.items() if v in ("auto", "off")}
    schedules = [s for s in c.schedules if s.every_s > 0 and s.duration_s > 0]
    return Control(setpoints=setpoints, schedules=schedules, once=once, policy=policy)


def _held(rows: list[dict], metric: str, op: str, value: float, for_s: int, now_ts: int) -> bool:
    """True when `metric op value` has been continuously true for `for_s`.

    Walks backwards from the newest sample and stops at the first one that
    breaks the condition; the hold is the span from there to now. Sparse rows
    are fine - the device's poll interval is adaptive, so gaps are expected and
    the span is measured in wall time rather than sample count.
    """
    if not rows:
        return False
    span_start = now_ts
    for row in reversed(rows):
        v = row.get(metric)
        ts = row.get("recv_ts")
        if v is None or ts is None:
            return False
        ok = v > value if op == "gt" else v < value
        if not ok:
            break
        span_start = int(ts)
    return (now_ts - span_start) >= for_s


def decide(
    *,
    now_ts: int,
    last_rx_ts: Optional[int],
    last_call_ts: Optional[int],
    wake: Optional[WakeSpec],
    rows: list[dict],
    calls_today: int,
    ask_now: bool,
    have_prescription: bool,
    model_ready: bool,
) -> Decision:
    """Should we spend a model call right now?

    Two timestamps, because they are two different facts and conflating them is
    what let the storm through. `last_rx_ts` is when the prescription now in
    force was issued, and it is what a wake deadline is measured from.
    `last_call_ts` is when a call was last *spent*, successful or not, and it is
    what the floor is measured from - a call that failed cost the same as one
    that worked, and a device that has never been diagnosed has no `last_rx_ts`
    at all, which is precisely how it came to be exempt from the floor it needed
    most.

    Every affirmative answer below is under both gates. Nothing is exempt: an
    exemption here is not a special case, it is a state a broken device sits in
    forever.
    """
    # A server with no key cannot spend a model call, so it must not answer yes
    # to one. Every consequence of a yes - fetch frames, poll at POLL_ACTIVE_S,
    # defer the answer to the next tick - is work done for a call that will
    # never be made, repeated for as long as the key stays missing. See
    # want_frame, which is gated on this decision.
    if not model_ready:
        return Decision(False, "no model configured")

    if last_call_ts is not None and (now_ts - last_call_ts) < MIN_INTERVAL_S:
        # The floor outranks everything including the user: a button that can be
        # held down is a budget leak, and nothing physical changes in 5 minutes.
        return Decision(False, "min interval")

    if calls_today >= DAILY_CALL_BUDGET:
        return Decision(False, "daily budget")

    # Below here, and not above: a first prescription is worth a call, but it is
    # worth exactly one call per MIN_INTERVAL_S like every other reason. A board
    # that never gets diagnosed - no key, a model that keeps failing - is in this
    # branch on every single poll, so this is the one reason that must never
    # outrank the floor.
    if not have_prescription:
        return Decision(True, "first prescription")

    if ask_now:
        return Decision(True, "user requested")

    if wake is None:
        return Decision(False, "no wake spec")

    if last_rx_ts is not None and now_ts >= wake.deadline(last_rx_ts):
        return Decision(True, "scheduled")

    for w in wake.when:
        if _held(rows, w["metric"], w["op"], w["value"], w["for_s"], now_ts):
            return Decision(True, f"{w['metric']} {w['op']} {w['value']}")

    return Decision(False, "idle")


def poll_interval_s(*, now_ts: int, last_event_ts: Optional[int], ask_now: bool) -> int:
    """Fast for a short window after something happened, slow otherwise."""
    if ask_now:
        return POLL_ACTIVE_S
    if last_event_ts is not None and (now_ts - last_event_ts) < POLL_ACTIVE_WINDOW_S:
        return POLL_ACTIVE_S
    return POLL_IDLE_S


def want_frame(*, decision: Decision, frame_ts: Optional[int], now_ts: int) -> bool:
    """Ask for fresh images only when we are about to look at them.

    Uploading 60KB every poll would dominate the link for nothing; the frames
    are only interesting at the moment a diagnosis is made.
    """
    if not decision.should_call:
        return False
    return frame_ts is None or (now_ts - frame_ts) > 120


def now() -> int:
    return int(time.time())


# The readings that arrive over ESP-NOW from the sensor node, spelled as
# schema.Sensors spells them. src/sensornode.cpp overwrites these five on
# receipt and nothing ever clears them, so the moment the link drops the device
# keeps reporting the last packet's numbers as though they were current. lux and
# soil_pct are permanently null on this hardware and are named anyway: the day a
# probe is wired, a frozen soil reading is the same lie as a frozen CO2 one.
_NODE_SENSORS = ("co2_ppm", "temp_c", "rh_pct", "lux", "soil_pct")


def telemetry_is_sane(t: Telemetry) -> bool:
    """Reject a payload we should not store or reason from.

    A device that has not heard from the sensor node has nothing to say about
    the plant; recording those rows would dilute every window average with
    stale readings.

    Keyed on links.node_online and never on links.node_age_ms. The age is
    ambiguous by construction - sensornode_age_ms() returns 0 both for a packet
    that arrived this millisecond and for a node that has never spoken at all -
    so a server reading it would score a board that has never had a node as
    maximally fresh, which is the intent inverted. The boolean is the same fact
    with the ambiguity removed, judged on the device's own clock against its own
    15s timeout, with no round-trip latency in it.

    Never having had a node and having just lost one are different situations
    and only the second produces a lie. A board whose node has never spoken
    sends nulls, because the firmware's -999 sentinels are nulled on the way
    out; if its thermal camera is live, the scene peak in that row is a real,
    current measurement and the row is worth keeping. A board whose node died
    mid-run sends the last packet forever, and every one of those numbers would
    be averaged into the window as though it had been measured.

    A stale thermal peak is refused rather than rejected: it reaches no metric
    column (derive.enrich already declines to derive leaf_air_dt_c from a dead
    thermal link, and leaf_max_c is not itself a window metric), so it can
    dilute nothing - it simply cannot be the one live reading that justifies a
    row. Frozen node readings land in the columns window_summary averages, which
    is why those take the whole row down with them.
    """
    if not t.device:
        return False
    s = t.sensors
    node = [getattr(s, k) for k in _NODE_SENSORS]
    if not t.links.node_online:
        if any(v is not None for v in node):
            return False
        node = []
    peak = s.leaf_max_c if t.links.thermal_live else None
    return any(v is not None for v in (*node, peak))
