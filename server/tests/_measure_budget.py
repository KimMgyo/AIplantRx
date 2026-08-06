"""Scratch: measure the worst-case Prescription on the wire. Not a test."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from app import schema  # noqa: E402

H = "가"  # 3 bytes of UTF-8, 6 bytes escaped as \uXXXX


def kr(nbytes):
    """A Hangul string filling exactly nbytes UTF-8 bytes (3 per syllable)."""
    s = H * (nbytes // 3) + ("." * (nbytes % 3))
    assert len(s.encode()) == nbytes, (len(s.encode()), nbytes)
    return s


def body(nbytes):
    """A body at its budget, with the paragraph break the field exists for."""
    s = kr(nbytes - 1)
    s = s[: len(s) // 2] + "\n" + s[len(s) // 2:]
    assert len(s.encode()) == nbytes, len(s.encode())
    return s


def size(obj, escaped=False):
    """starlette's own encoding: JSONResponse dumps with ensure_ascii=False and
    these separators. escaped=True is the same body re-encoded by a hop that
    insists on ASCII."""
    return len(json.dumps(obj, ensure_ascii=escaped,
                          separators=(",", ":")).encode())


CHIPS = [schema.Chip(text=kr(schema.JUDGE_CHIP_BYTES), tone="warn", hot=(i == 0))
         for i in range(schema.JUDGE_CHIPS_MAX)]

JUDGE = [schema.JudgeRow(
    at="18:33",
    level="alert",
    head=kr(schema.JUDGE_HEAD_BYTES),
    body=body(schema.JUDGE_BODY_BYTES),
    chips=CHIPS,
    has_rgb=True,
    has_thermal=True,
) for _ in range(schema.JUDGE_ROWS_MAX)]

PLAN = [schema.PlanRow(at="18:33", tag=kr(schema.ROW_TAG_BYTES), tone="warn",
                       head=kr(schema.ROW_HEAD_BYTES),
                       cond=kr(schema.ROW_COND_BYTES))
        for _ in range(schema.PLAN_ROWS_MAX)]

ACTIONS = [schema.ActionRow(at="18:33", tag=kr(schema.ROW_TAG_BYTES), tone="ok",
                            head=kr(schema.ROW_HEAD_BYTES),
                            delta=kr(schema.ROW_DELTA_BYTES),
                            delta_is_reading=True, improved=True)
           for _ in range(schema.ACTION_ROWS_MAX)]

WINDOW = [schema.WindowRow(label=kr(schema.ROW_WLABEL_BYTES),
                           stat=kr(schema.ROW_WSTAT_BYTES),
                           band=kr(schema.ROW_WBAND_BYTES), in_band_pct=100)
          for _ in range(schema.WINDOW_ROWS_MAX)]

# The same control half the block in render.py was measured against.
CONTROL = schema.Control(
    setpoints=[schema.Setpoint(key=k, lo=1.25, hi=98.75) for k in
               ("vpd_kpa", "air_c", "rh_pct", "co2_ppm", "leaf_air_dt_c")],
    schedules=[schema.Schedule(actuator="pumpA", every_s=21600, duration_s=240)
               for _ in range(4)],
    once=[schema.OnceAction(id="0123456789abcdef", actuator="mist", seconds=60,
                            before_ts=1754300000) for _ in range(4)],
    policy={a: "off" for a in ("mist", "fan", "pumpA", "pumpB", "heater",
                               "light", "vent")},
)

DISPLAY = schema.Display(
    species=schema.Species(text=kr(126), sci=kr(schema.SPECIES_SCI_BYTES),
                           conf_text="100%"),
    judgments=JUDGE,
    turn=schema.Turn(scheduled=True, next_ts=1754300000, period_s=1800),
    plan=PLAN,
    actions=ACTIONS,
    window=WINDOW,
    window_span_s=3600,
    window_covered_s=3600,
    notice=kr(schema.NOTICE_BYTES),
    model_ready=True,
)

RX = schema.Prescription(
    rx_id="0123456789abcdef", issued_ts=1754300000, next_poll_s=60,
    want_frame=True, update_mode=True, firmware_pull=True, mode="advisory",
    control=CONTROL, display=DISPLAY,
)

full = RX.model_dump(mode="json")
d = full["display"]

# Every line carries its own key and comma, so the column adds up to the object it
# is a breakdown of instead of to something 11 bytes short of it. The two totals at
# the bottom are asserted against the column for the same reason: a ledger with a
# "=====" rule under it that does not reconcile is worse than no ledger.
def keyed(obj, key):
    return size(obj) + len('"%s":,' % key)


rows = [("judgments", keyed(d["judgments"], "judgments")),
        ("plan", keyed(d["plan"], "plan")),
        ("actions", keyed(d["actions"], "actions")),
        ("window", keyed(d["window"], "window")),
        ("species", keyed(d["species"], "species")),
        ("notice", keyed(d["notice"], "notice")),
        ("turn", keyed(d["turn"], "turn")),
        ("spans", size(d["window_span_s"]) + size(d["window_covered_s"])
                  + len('"window_span_s":,"window_covered_s":,')),
        ("ready", keyed(d["model_ready"], "model_ready"))]

print("judgments %5d   (%d row x (at %d + head %d + body %d + %d chips x %d))"
      % (rows[0][1], schema.JUDGE_ROWS_MAX,
         schema.JUDGE_AT_BYTES, schema.JUDGE_HEAD_BYTES, schema.JUDGE_BODY_BYTES,
         schema.JUDGE_CHIPS_MAX, schema.JUDGE_CHIP_BYTES))
for name, n in rows[1:]:
    print("%-9s %5d" % (name, n))

# "display": is 11 bytes and its two braces are 2, against the one phantom comma
# the column counts for a last key that has none: +12. Written as a literal and
# asserted rather than printed as a subtraction, because a subtraction absorbs a
# Display field this script forgot to enumerate and reports it as brace overhead.
_KEY_BRACE = 12

display = keyed(d, "display")
column = sum(n for _, n in rows)
assert column + _KEY_BRACE == display, (column, _KEY_BRACE, display,
                                        "a Display field is missing from `rows`")
print("key/brace %5d   (\"display\": + 2 braces - the column's phantom comma)"
      % _KEY_BRACE)
print("display   %5d" % display)
control = keyed(full["control"], "control")
print("control   %5d" % control)
envelope = size(full) - display - control
print("envelope  %5d" % envelope)
print("TOTAL     %5d  wire (ensure_ascii=False)" % size(full))
print("TOTAL     %5d  escaped (ensure_ascii=True)" % size(full, escaped=True))
# The envelope is rx_id, issued_ts, next_poll_s, want_frame, update_mode,
# firmware_pull, mode, and the outer braces - nothing that scales with a field
# budget, so it is a constant and drifts only when the envelope gains a key.
assert envelope == 144, (envelope, "the response envelope gained or lost a field")
print()
print("body alone %d B wire, %d B escaped"
      % (size(d["judgments"][0]["body"]), size(d["judgments"][0]["body"], True)))
print("one judgment row %d B wire, %d B escaped"
      % (size(d["judgments"][0]), size(d["judgments"][0], True)))

# The shape this replaced: six rows, three chips each, no body, so the two figures
# are comparable and the claim about MAX_RESP is not a memory.
old = RX.model_dump(mode="json")
old["display"]["judgments"] = [{
    "at": "18:33", "level": "alert", "head": kr(schema.JUDGE_HEAD_BYTES),
    "chips": [{"text": kr(schema.JUDGE_CHIP_BYTES), "tone": "warn",
               "hot": i == 0} for i in range(3)],
    "has_rgb": True, "has_thermal": True,
} for _ in range(6)]
print("OLD shape (6 rows x 3 chips, no body): %d B wire, %d B escaped"
      % (size(old), size(old, escaped=True)))
print("  its judgments block: %d B wire, %d B escaped"
      % (size(old["display"]["judgments"]) + len('"judgments":,'),
         size(old["display"]["judgments"], True) + len('"judgments":,')))
