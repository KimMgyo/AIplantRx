"""The display half, end to end, with the model stubbed.

    cd server && python tests/test_display_contract.py

Plain asserts and no test framework: the image installs requirements.txt, and a
test runner in there would ship to production for nothing. pytest collects this
file unchanged if one is ever added.

Four modules have to agree on the display contract - brain.py names the finding,
render.py assembles the rows, schema.py bounds them, main.py wires them - and
nothing at runtime notices when they stop agreeing. A renamed field does not
raise: pydantic fills the default and a column goes quietly blank on the panel,
which is the failure this file exists to make loud. The byte budgets are checked
against the response rather than against render.py's own clip, because the
buffers they have to fit are `char[N]` in include/aijudge.h and include/plantrx.h
and a device does not get to reject an overlong string.
"""
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "contract.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is not what this file is about

from fastapi.testclient import TestClient  # noqa: E402
from app import brain, main, plantnet, render, schema, scheduler  # noqa: E402

T0 = 1754300000  # 2025-08-04 18:33 KST, so the 시각 fields are readable above
CLOCK = {"t": T0}
scheduler.now = lambda: CLOCK["t"]
CLIENT = TestClient(main.app)


def _telemetry(device, temp=None, rh=None, co2=None, peak=None, species=None):
    sensors = {"temp_c": temp, "rh_pct": rh, "co2_ppm": co2, "leaf_max_c": peak}
    body = {
        "device": device,
        "uptime_ms": 60000,
        "sensors": {k: v for k, v in sensors.items() if v is not None},
        "links": {"node_online": True, "cam_online": True, "rgb_live": True,
                  "thermal_live": True, "thermal_fps": 4.0, "wifi_rssi": -55},
        "actuators": {"mist": 0, "fan": 40},
        "auto": True,
    }
    # Omitted, not empty. An empty object is what a board that identified
    # nothing would be indistinguishable from, and it would suppress the
    # server's own identification for a name that does not exist.
    if species is not None:
        body["species"] = species
    return body


def _upload_frames(device):
    """Two frames dated now, so the diagnosis is not deferred for a fresh one."""
    for kind in ("rgb", "thermal"):
        r = CLIENT.post("/v1/frame", content=b"\xff\xd8\xff\xd9" * 64,
                        headers={"X-Device": device, "X-Kind": kind})
        assert r.status_code == 200, r.text


def _poll(device, **kw):
    r = CLIENT.post("/v1/telemetry", json=_telemetry(device, **kw))
    assert r.status_code == 200, r.text
    return r.json()


# Fields the tense-split display replaced. Their names are still spelled here on
# purpose: pydantic ignores unknown keys on the way out, so the only way to catch
# a half-finished rename is to name the corpses.
REMOVED = {"diagnosis", "evidence", "targets", "last_run", "next_run"}


# The one string on this wire that carries a character the fonts have no glyph for,
# and carries it on purpose: JudgeRow.body's '\n' is a line break to LVGL's label
# renderer, not a character to draw. Exempted by path rather than granted to every
# string, because every other field flattens it (render._SUBS) - a newline in a
# chip breaks the flex row the chip sits in, and one in a plan row's head breaks
# the two-column grid.
_BREAKS_OK = re.compile(r"^display\.judgments\[\d+\]\.body$")

# Every control character except that one. '\r' is the dangerous member: a model
# writing CRLF puts one at the end of every line, it has no glyph and no meaning
# to the renderer, and it would still spend a byte of `char body[1024]`.
_CTRL = re.compile(r"[\x00-\x09\x0b-\x1f\x7f]")

# Three newlines is two blank lines. render._body collapses runs to one blank line
# because the card's whole point is that the column finally has room for the prose,
# and a second empty line is a hole in it.
_RUNS = re.compile(r"\n{3,}")


def _check_drawable(node, path, bad):
    """Every string on the wire has to exist in the device's fonts.

    The panel's floor is font_kr_full_12, the 12px fallback every subset declares:
    it carries the whole U+AC00-D7A3 block, so any Hangul reaches the screen and
    what this checks is the rest - a code point outside that block lands as an
    empty box, and a row of boxes is worse than a row that was never sent. Size is
    a separate question and a separate test: test_font_coverage.py rule C is what
    keeps a nameable syllable off the fallback so it draws at the size the label
    asked for. render.guard is supposed to have already run.
    """
    if isinstance(node, str):
        text = node.replace("\n", " ") if _BREAKS_OK.match(path) else node
        if not render.is_renderable(text):
            missing = sorted({hex(ord(c)) for c in text if not render.is_renderable(c)})
            bad.append((path, node, missing))
    elif isinstance(node, dict):
        for key, value in node.items():
            _check_drawable(value, path + "." + key, bad)
    elif isinstance(node, list):
        for i, value in enumerate(node):
            _check_drawable(value, path + "[%d]" % i, bad)


def _check_budgets(display, bad):
    rows = display["judgments"]
    if len(rows) > schema.JUDGE_ROWS_MAX:
        bad.append(("judgments", len(rows), schema.JUDGE_ROWS_MAX))
    for i, row in enumerate(rows):
        for field, budget in (("at", schema.JUDGE_AT_BYTES),
                              ("head", schema.JUDGE_HEAD_BYTES),
                              ("body", schema.JUDGE_BODY_BYTES)):
            n = len(row[field].encode())
            if n > budget:
                bad.append(("judgments[%d].%s" % (i, field), n, budget))
        # body is the field the card was redesigned around, and the only one on
        # this wire allowed a control character - allowed exactly one. It lands in
        # `char body[1024]` like the rest, so the byte budget above is the same
        # kind of claim; these two are what '\n' surviving guard() costs in extra
        # checking.
        stray = sorted({hex(ord(c)) for c in set(_CTRL.findall(row["body"]))})
        if stray:
            bad.append(("judgments[%d].body control chars" % i, stray, "\\n only"))
        runs = _RUNS.findall(row["body"])
        if runs:
            bad.append(("judgments[%d].body newline run" % i,
                        max(len(r) for r in runs), 2))
        if len(row["chips"]) > schema.JUDGE_CHIPS_MAX:
            bad.append(("judgments[%d].chips" % i, len(row["chips"]),
                        schema.JUDGE_CHIPS_MAX))
        for j, chip in enumerate(row["chips"]):
            n = len(chip["text"].encode())
            if n > schema.JUDGE_CHIP_BYTES:
                bad.append(("judgments[%d].chips[%d]" % (i, j), n,
                            schema.JUDGE_CHIP_BYTES))

    # The 예약 / 조치 columns land in plantrx.h's buffers, which truncate rather
    # than reject, so an overlong row is a silent half-syllable on the wall. The
    # rows are checked here on the response for the same reason the judgments
    # are: render.py clipping correctly is not the claim, the wire is.
    for col, cap in (("plan", schema.PLAN_ROWS_MAX), ("actions", schema.ACTION_ROWS_MAX)):
        rows = display[col]
        if len(rows) > cap:
            bad.append((col, len(rows), cap))
        for i, row in enumerate(rows):
            for field, budget in (("at", schema.ROW_AT_BYTES),
                                  ("tag", schema.ROW_TAG_BYTES),
                                  ("head", schema.ROW_HEAD_BYTES),
                                  ("cond", schema.ROW_COND_BYTES),
                                  ("delta", schema.ROW_DELTA_BYTES)):
                if field not in row:
                    continue  # cond is the plan row's, delta the action row's
                n = len(row[field].encode())
                if n > budget:
                    bad.append(("%s[%d].%s" % (col, i, field), n, budget))

    # The window block lands in RxWindowRow (include/plantrx.h) and is drawn on
    # the monitor page rather than the 판단 card, which is exactly why it is
    # checked here with the rest: a block nothing else audits is a block that
    # drifts. band and in_band_pct are two spellings of one fact and the device
    # tints from the number while the grower reads the string, so they are
    # checked against each other and not only against their budgets.
    rows = display["window"]
    if len(rows) > schema.WINDOW_ROWS_MAX:
        bad.append(("window", len(rows), schema.WINDOW_ROWS_MAX))
    for i, row in enumerate(rows):
        for field, budget in (("label", schema.ROW_WLABEL_BYTES),
                              ("stat", schema.ROW_WSTAT_BYTES),
                              ("band", schema.ROW_WBAND_BYTES)):
            n = len(row[field].encode())
            if n > budget:
                bad.append(("window[%d].%s" % (i, field), n, budget))
        pct = row["in_band_pct"]
        if (pct is None) != (row["band"] == ""):
            bad.append(("window[%d] band/in_band_pct disagree" % i, row["band"], pct))
        if pct is not None and not 0 <= pct <= 100:
            bad.append(("window[%d].in_band_pct" % i, pct, 100))
    if display["window_covered_s"] > display["window_span_s"]:
        bad.append(("window_covered_s", display["window_covered_s"],
                    display["window_span_s"]))

    n = len(display["notice"].encode())
    if n > schema.NOTICE_BYTES:
        bad.append(("notice", n, schema.NOTICE_BYTES))

    # The species card's two carried strings, on the wire for the same reason as
    # the rows above: they land in PLANTRX_SCI_CAP / PLANTRX_CONF_CAP, which
    # truncate rather than reject. `text` is not here - its buffer is the device's
    # own RX_SPECIES_CAP and _device_species_cap() checks it against that.
    sp = display["species"]
    if sp is not None:
        for field, budget in (("sci", schema.SPECIES_SCI_BYTES),
                              ("conf_text", schema.SPECIES_CONF_BYTES)):
            n = len(sp[field].encode())
            if n > budget:
                bad.append(("species.%s" % field, n, budget))


def _audit(prescription, where):
    display = prescription["display"]
    leaked = REMOVED & set(display)
    assert not leaked, "%s: fields removed by the tense split are back: %s" % (where, leaked)
    bad = []
    _check_budgets(display, bad)
    _check_drawable(display, "display", bad)
    assert not bad, "%s: %s" % (where, bad)


# --------------------------------------------------------------------------


def test_never_diagnosed():
    """A freshly flashed board, before anything has looked at the plant.

    Both empty paths in telemetry() are walked. The first poll is asked for
    images and defers the diagnosis. The second has the images but no API key,
    which is what a self-hosted install without a Gemini key looks like on every
    poll. Both have to answer a Prescription rather than an error - the
    greenhouse keeps holding whatever it last held, and an HTTP error would only
    teach the device to retry a loop.

    The key is stubbed present for the first poll and absent for the second,
    because the two are different reasons for an empty card and used to be
    conflated. Deferring for images is about not having a picture; it is not a
    reason to ask a keyless server's device to encode and upload two JPEGs every
    120s forever for a diagnosis that is never coming, which is what a
    should_call that ignores whether a call is possible produced.

    The sanity gate is deliberately not what is exercised here: it decides
    whether a reading is stored, not whether a device is answered, and a board
    with a dead sensor node still needs its prescription back.
    """
    device = "11:22:33:44:55:66"
    keep = brain.is_configured
    brain.is_configured = lambda: True
    try:
        first = _poll(device, temp=27.3, rh=45.0, co2=1237.0)
    finally:
        brain.is_configured = keep
    assert first["rx_id"] == "none"
    assert first["want_frame"] is True, "images come before the first diagnosis"
    assert first["mode"] == "advisory", "never diagnosed is never allowed to actuate"

    display = first["display"]
    assert display["judgments"] == []
    assert display["turn"] == {"scheduled": False, "next_ts": 0, "period_s": 0}, display["turn"]
    assert display["notice"], "an empty column must say why; blank reads as a broken screen"
    _audit(first, "never diagnosed")

    # The frames arrive, so nothing further can be blamed on missing evidence -
    # and with no key configured the column stays empty for the honest reason.
    assert not brain.is_configured(), "this path needs an unconfigured brain"
    _upload_frames(device)
    second = _poll(device, temp=27.3, rh=45.0, co2=1237.0)
    assert second["rx_id"] == "none"
    assert second["want_frame"] is False, "no diagnosis is coming; the frames were enough"
    # The two silences must not read the same. The first poll ran with a key, so its
    # empty column is "no judgment yet" and waiting will fix it; this one has no key
    # and waiting will never fix it. One notice for both told a grower to wait for
    # something that was not coming.
    assert second["display"]["notice"] != display["notice"], \
        "no model configured must not read as no judgment yet"
    assert "없습니다" in second["display"]["notice"]
    assert second["display"]["model_ready"] is False
    assert display["model_ready"] is True
    _audit(second, "no model configured")
    return first


def test_two_turns_by_tense():
    """Two turns, one card, and the three tenses drawn from the newest.

    The stub stands in for the model so the assertions can be exact: a stressed
    turn that fires on VPD and prescribes, then a recovered turn that fires on
    nothing. What is being checked is that the judgment the model authored survives
    the trip WHOLE - its level tints the card, its deciding metric is the chip the
    device highlights, its prose arrives as `body` in full rather than as one
    clipped clause, and the turn it asked for is the deadline the card counts down
    to.

    The second turn is what proves the column stopped being a log. It used to
    append, and the first row rode along underneath it on every poll afterwards;
    now the card draws one judgment, so the newest replaces its predecessor
    outright and the response carries exactly one row. The stressed turn is
    therefore asserted on the first poll, where it is the newest, and asserted
    absent from the second.
    """
    device = "AA:BB:CC:DD:EE:FF"
    calls = {"n": 0}

    async def stub(inp):
        calls["n"] += 1
        if calls["n"] == 1:
            return brain.BrainOutput(
                diagnosis_ko="포차가 높아 수분 스트레스로 보입니다. 미스트를 가동했습니다.",
                head_ko="수분 스트레스 감지",
                level="alert",
                evidence=["vpd_kpa", "rh_pct", "air_c"],
                deciding="vpd_kpa",
                confidence=0.82,
                control=schema.Control(
                    setpoints=[schema.Setpoint(key="vpd_kpa", lo=0.8, hi=1.2)],
                    schedules=[schema.Schedule(actuator="pumpA", every_s=21600,
                                               duration_s=240)],
                    once=[schema.OnceAction(id="x1", actuator="mist", seconds=60,
                                            before_ts=CLOCK["t"] + 600)],
                    policy={"heater": "off"},
                ),
                wake_after_s=900,
                wake_when=[{"metric": "vpd_kpa", "op": "gt", "value": 1.8, "for_s": 600}],
                notes_ko="장면최고는 램프 영향 가능성이 있어 참고만 했습니다.",
            )
        return brain.BrainOutput(
            diagnosis_ko="VPD가 목표 범위로 돌아왔습니다.",
            head_ko="생육 조건 양호",
            level="ok",
            evidence=["vpd_kpa", "air_c"],
            deciding="",
            confidence=0.7,
            control=schema.Control(
                setpoints=[schema.Setpoint(key="vpd_kpa", lo=0.8, hi=1.2)],
                schedules=[schema.Schedule(actuator="pumpA", every_s=21600,
                                           duration_s=240)],
            ),
            wake_after_s=1800,
            wake_when=[],
            notes_ko="",
        )

    keep = (brain.is_configured, brain.diagnose)
    brain.is_configured, brain.diagnose = (lambda: True), stub
    try:
        CLOCK["t"] = T0
        _upload_frames(device)
        first = _poll(device, temp=27.3, rh=45.0, co2=1237.0, peak=31.9)
        assert calls["n"] == 1, "a device with fresh frames and no history gets diagnosed"
        assert first["mode"] == "auto"
        assert len(first["display"]["judgments"]) == 1
        _audit(first, "first judgment")
        stressed = first["display"]["judgments"][0]

        # Past the wake the model asked for, with frames dated now.
        CLOCK["t"] = T0 + 1000
        _upload_frames(device)
        second = _poll(device, temp=26.4, rh=62.0, co2=900.0, peak=28.1)
        assert calls["n"] == 2, "the wake it asked for has to actually wake it"
    finally:
        brain.is_configured, brain.diagnose = keep

    display = second["display"]
    _audit(second, "second judgment")

    # The stressed turn, asserted where it is the newest judgment.
    assert stressed["at"] == "18:33", stressed
    assert stressed["level"] == "alert", stressed
    assert sum(c["hot"] for c in stressed["chips"]) == 1, stressed["chips"]
    assert stressed["chips"][0]["hot"], "aijudge.cpp reads the hot chip at index 0"
    assert stressed["chips"][0]["text"].startswith("VPD"), stressed["chips"][0]

    # A chip carries the number with its unit, which is what makes it comparable
    # across turns; a metric the model never cited does not get one.
    texts = [c["text"] for c in stressed["chips"]]
    assert [t.split()[0] for t in texts] == ["VPD", "습도", "기온"], texts
    assert not any(t.startswith("CO2") for t in texts), "uncited metrics draw no chip"

    # The prose, whole, which is the reason this column was redesigned. head is the
    # clause worth putting beside a timestamp; body is everything the model said -
    # the diagnosis, then the notes, joined by the one newline the field keeps.
    # Asserted as an equality and not a prefix test on purpose: render._conclusion
    # used to reduce all of this to its last sentence clipped to 21 characters, and
    # notes_ko never left the server at all, so a body that merely CONTAINED the
    # diagnosis would pass while the caveat stayed invisible.
    assert stressed["head"] == "수분 스트레스 감지", stressed
    assert stressed["body"] == (
        "포차가 높아 수분 스트레스로 보입니다. 미스트를 가동했습니다.\n"
        "장면최고는 램프 영향 가능성이 있어 참고만 했습니다."), stressed["body"]

    rows = display["judgments"]
    # One row, and the case is named for it: the stressed turn above is GONE from
    # the wire rather than carried underneath. Five more rows of finished strings
    # the card cannot draw were 2 KB of a response the firmware reads into a fixed
    # buffer, and the history a grower wants over time is the 조치 column, which is
    # built from measurements rather than from re-issued prose.
    assert len(rows) == 1, rows
    assert rows[0]["at"] == "18:50", rows[0]
    assert rows[0]["level"] == "ok", rows[0]
    assert rows[0]["head"] != stressed["head"], "the card is replaying the old turn"
    # No origin key at all, and that is the assertion. It was a Literal with one
    # reachable value - build_display is behind the model gate and the model-less
    # path ships no judgments - so it spent 17 bytes a row implying the server
    # distinguishes authors it cannot. The device never parsed it; the firmware
    # stamps its own JUDGE_RULE / JUDGE_LLM and reads Display.model_ready for
    # whether a model exists. Asserted absent so the day somebody re-adds it they
    # have to give it a producer that can say something else.
    assert "origin" not in rows[0], rows[0]
    assert rows[0]["has_rgb"] and rows[0]["has_thermal"], "both frames were sent"

    # The recovered turn named no culprit, so nothing may be highlighted.
    assert not any(c["hot"] for c in rows[0]["chips"]), rows[0]["chips"]

    # This turn wrote no notes, so the body is the diagnosis alone and holds no
    # separator: a trailing newline would draw as a blank line under the prose, and
    # a leading one would indent the card's first sentence for nothing.
    assert rows[0]["body"] == "VPD가 목표 범위로 돌아왔습니다.", rows[0]["body"]
    assert "\n" not in rows[0]["body"], rows[0]["body"]

    turn = display["turn"]
    assert turn["scheduled"] is True
    assert turn["period_s"] == 1800, turn
    assert turn["next_ts"] == second["issued_ts"] + 1800, (turn, second["issued_ts"])

    # Future tense: the standing schedule survives, the wake condition does not,
    # because the second turn asked for no condition.
    assert len(display["plan"]) == 1, display["plan"]
    assert display["plan"][0]["cond"] == "6시간 주기", display["plan"][0]

    # Past tense: the mist that was ordered is reported against what the window
    # actually recorded, and the device reported the actuator never ran.
    assert display["actions"][-1]["tag"] == "완료", display["actions"]
    assert display["actions"][-1]["improved"] is False
    assert display["notice"] == "", "a populated display has nothing to apologise for"
    return second


def test_the_body_keeps_one_control_character_and_no_others():
    """A model that formats its answer, against the only break the field allows.

    None of this mess is invented. Models end lines with CRLF, indent with tabs,
    space paragraphs out with several blank lines, and reach for an emoji. All of
    it used to be harmless because the panel saw one clause of the diagnosis and
    never saw notes_ko at all; now both are drawn out of `char body[1024]`, so the
    sanitising every other field on this wire gets has to hold here too - minus the
    one substitution that would defeat the field, _SUBS flattening '\\n' to a space.

    The blank-line collapse is the part worth a case of its own: LVGL's label draws
    every '\\n' it is given, so four in a row are four line heights of nothing on a
    card 222 px wide, and the two frames underneath are what would be pushed off it.
    """
    device = "AA:BB:CC:DD:EE:41"

    async def stub(inp):
        return brain.BrainOutput(
            # CRLF, a tab inside a line, an emoji, and a trailing space left by
            # dropping it - four different producers of the same broken glyph.
            diagnosis_ko="습도가 낮습니다.\r\n미스트를\t가동했습니다. \U0001f331",
            head_ko="수분 스트레스 감지",
            level="warn",
            evidence=["rh_pct"],
            deciding="rh_pct",
            confidence=0.7,
            control=schema.Control(),
            wake_after_s=1800,
            wake_when=[],
            # A list the model indented, and the run of blank lines it left behind.
            notes_ko="근거:\n\n\n\n장면최고 31.9 \u2103",
        )

    keep = (brain.is_configured, brain.diagnose)
    brain.is_configured, brain.diagnose = (lambda: True), stub
    try:
        CLOCK["t"] = T0 + 5000
        _upload_frames(device)
        got = _poll(device, temp=24.0, rh=31.0, co2=800.0, peak=25.0)
    finally:
        brain.is_configured, brain.diagnose = keep

    _audit(got, "messy model")
    body = got["display"]["judgments"][0]["body"]

    # The CR is gone as a break that survived rather than as a character dropped:
    # _SUBS maps it to a space for every other field, and losing the line here
    # would run two sentences together.
    assert body == ("습도가 낮습니다.\n"
                    "미스트를 가동했습니다.\n"
                    "근거:\n"
                    "\n"
                    "장면최고 31.9 \u2103"), body
    assert "\r" not in body and "\t" not in body, body
    assert "\n\n\n" not in body, "a run of blank lines is card height spent on nothing"
    assert "\U0001f331" not in body, "the font draws a box, so guard() drops it"
    assert not any(line != line.strip() for line in body.split("\n")), body
    return got


def _species_run(device, reported, seen):
    """One diagnosis with the model stubbed and PlantNet configured, recording
    what the brain was handed and whether identification was reached.

    PlantNet is configured on purpose: "the server did not identify" has to mean
    it chose not to, not that it could not. plantnet._keys() re-reads the
    environment on every call, so setting the variable here is enough.
    """
    async def stub(inp):
        seen["brain"] += 1
        seen["species"] = inp.species
        return brain.BrainOutput(
            diagnosis_ko="현재 생육 조건은 안정적입니다.",
            head_ko="생육 조건 양호",
            level="ok",
            evidence=["vpd_kpa", "air_c"],
            deciding="",
            confidence=0.7,
            control=schema.Control(
                setpoints=[schema.Setpoint(key="vpd_kpa", lo=0.8, hi=1.2)]
            ),
            wake_after_s=1800,
            wake_when=[],
            notes_ko="",
        )

    async def identify(jpeg):
        seen["identify"] += 1
        return plantnet.SpeciesResult(sci="Ficus elastica", common="Rubber plant",
                                      korean="인도고무나무", score=0.91)

    keep = (brain.is_configured, brain.diagnose, plantnet.identify,
            os.environ.get("PLANTNET_API_KEYS"))
    brain.is_configured, brain.diagnose = (lambda: True), stub
    plantnet.identify = identify
    os.environ["PLANTNET_API_KEYS"] = "test-key"
    try:
        _upload_frames(device)
        return _poll(device, temp=25.2, rh=58.0, co2=800.0, species=reported)
    finally:
        brain.is_configured, brain.diagnose, plantnet.identify = keep[:3]
        if keep[3] is None:
            os.environ.pop("PLANTNET_API_KEYS", None)
        else:
            os.environ["PLANTNET_API_KEYS"] = keep[3]


def _device_species_cap():
    """RX_SPECIES_CAP less its NUL: the panel's own buffer for the name it draws.

    None when the firmware tree is absent, on the same terms as _header below.
    Hung off _INCLUDE so the repo root is spelled once. The budget is checked on
    the wire rather than on render.py's clip for the reason the other budgets
    are: src/plantrx.cpp truncates what it is sent, it does not reject it.
    """
    path = os.path.join(os.path.dirname(_INCLUDE), "src", "plantrx.cpp")
    if not os.path.exists(path):
        return None
    m = re.search(r"#define\s+RX_SPECIES_CAP\s+(\d+)",
                  open(path, encoding="utf-8").read())
    assert m, "src/plantrx.cpp no longer defines RX_SPECIES_CAP"
    return int(m.group(1)) - 1


def test_device_species_wins():
    """The panel identified the plant, so the server must not identify it again.

    The device carries PlantNet keys of its own and identifies on a button
    press, with the plant in front of its lens, and src/ui/page_auto.cpp draws
    that answer in preference to the server's. Identifying anyway spends a
    second quota to put a second name on one plant, and the wall keeps showing
    the one the model never reasoned about. So a reported species is not a
    tie-break: it is what stops the call from being made, which is why the
    identify counter and not just the rendered name is asserted here.
    """
    device = "AA:00:00:00:00:01"
    seen = {"identify": 0, "brain": 0, "species": None}
    CLOCK["t"] = T0 + 2000
    rx = _species_run(device, {"sci": "Ajuga genevensis", "text": "아주가",
                               "conf_pct": 87}, seen)

    assert seen["brain"] == 1, "the diagnosis never ran; nothing below is being tested"
    assert seen["identify"] == 0, "a PlantNet request was spent on an already identified plant"

    sp = rx["display"]["species"]
    assert sp["text"] == "아주가", sp
    assert sp["sci"] == "Ajuga genevensis", sp
    # The panel is already showing a percentage. A server that re-derived one
    # from a score of its own would put two numbers against one measurement.
    assert sp["conf_text"] == "87%", sp
    assert seen["species"]["source"] == "device", seen["species"]

    cap = _device_species_cap()
    if cap is not None:
        n = len(sp["text"].encode())
        assert n <= cap, "species.text is %dB against the panel's %dB buffer" % (n, cap)
    _audit(rx, "device species")
    return rx


def test_server_species_when_the_panel_is_silent():
    """No species on the telemetry: the old path, unchanged, and then carried.

    A board whose identify button has never been pressed - or one running
    firmware that does not know the field - still gets the server's
    identification, on the same condition as before: a frame, a key, and no
    species on the previous prescription. The second diagnosis then proves the
    third rule, that a poll which identifies nothing carries the name forward
    instead of blanking a card that was already right, and does not spend a
    second request to do it.
    """
    device = "AA:00:00:00:00:02"
    seen = {"identify": 0, "brain": 0, "species": None}
    CLOCK["t"] = T0 + 3000
    rx = _species_run(device, None, seen)

    assert seen["brain"] == 1, "the diagnosis never ran; nothing below is being tested"
    assert seen["identify"] == 1, "an unidentified plant with a frame and a key was not identified"

    sp = rx["display"]["species"]
    assert sp["text"] == "인도고무나무", sp
    assert sp["sci"] == "Ficus elastica", sp
    assert sp["conf_text"] == "91%", sp
    assert seen["species"]["source"] == "server", seen["species"]
    _audit(rx, "server species")

    # Past the wake the stub asked for, so a second diagnosis really runs.
    CLOCK["t"] = T0 + 3000 + 1810
    again = _species_run(device, None, seen)
    assert seen["brain"] == 2, "the wake it asked for has to actually wake it"
    assert seen["identify"] == 1, "the plant was identified again on the next poll"
    assert seen["species"]["source"] == "carried", seen["species"]
    assert again["display"]["species"] == sp, (again["display"]["species"], sp)
    return rx


_INCLUDE = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "include")


def _header(name):
    """The header's text, or None when the firmware tree is not on disk.

    Absent is skipped, not failed - the container ships app/ alone and a missing
    firmware tree is not a contract violation.
    """
    path = os.path.join(_INCLUDE, name)
    if not os.path.exists(path):
        return None
    return open(path, encoding="utf-8").read()


def _array(src, header, field):
    m = re.search(r"char\s+%s\s*\[\s*(\d+)\s*\]" % field, src)
    assert m, "%s no longer declares char %s[]" % (header, field)
    return int(m.group(1))


def _define(src, header, name):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, src)
    assert m, "%s no longer defines %s" % (header, name)
    return int(m.group(1))


# Every budget a header is the other half of, so the negative control can walk
# the same list the check does rather than a hand-copied second one.
_JUDGE_KEYS = ("JUDGE_AT_BYTES", "JUDGE_HEAD_BYTES", "JUDGE_BODY_BYTES",
               "JUDGE_CHIP_BYTES", "JUDGE_CHIPS_MAX", "JUDGE_ROWS_MAX")
_ROW_KEYS = ("ROW_AT_BYTES", "ROW_TAG_BYTES", "ROW_HEAD_BYTES", "ROW_COND_BYTES",
             "ROW_DELTA_BYTES", "NOTICE_BYTES", "PLAN_ROWS_MAX", "ACTION_ROWS_MAX",
             "ROW_WLABEL_BYTES", "ROW_WSTAT_BYTES", "ROW_WBAND_BYTES",
             "WINDOW_ROWS_MAX", "SPECIES_CONF_BYTES", "SPECIES_SCI_BYTES")


def _budgets(**drift):
    """schema.py's numbers, with named ones perturbed for the negative control."""
    out = {k: getattr(schema, k) for k in _JUDGE_KEYS + _ROW_KEYS}
    out.update(drift)
    return out


def _check_aijudge(src, budgets):
    """Complaints about include/aijudge.h, and the line to print when there are
    none. The buffers are `char` arrays there, so the extent is read off the
    declaration."""
    bad = []
    # One byte short of the array: these are C strings and the NUL is not optional.
    for field, key in (("at", "JUDGE_AT_BYTES"),
                       ("head", "JUDGE_HEAD_BYTES"),
                       ("body", "JUDGE_BODY_BYTES"),
                       ("text", "JUDGE_CHIP_BYTES")):
        cap = _array(src, "aijudge.h", field)
        if budgets[key] != cap - 1:
            bad.append("char %s[%d] leaves %d bytes for text, schema.py budgets %d"
                       % (field, cap, cap - 1, budgets[key]))

    # A count, not a length: no terminator to leave room for. A chip past the array
    # is not stored, so the server sending a sixth is the server lying about what
    # the panel shows.
    n = _define(src, "aijudge.h", "AIJUDGE_EVID_MAX")
    if budgets["JUDGE_CHIPS_MAX"] != n:
        bad.append("aijudge.h AIJUDGE_EVID_MAX is %d, schema.py budgets %d"
                   % (n, budgets["JUDGE_CHIPS_MAX"]))

    # The ring bounds the row count without being equal to it, and this pair used
    # to be checked for equality. It cannot be: the panel draws the newest judgment
    # as one card, so the server sends one row, while the ring stays deeper than
    # that because the FIRMWARE authors rule rows into the same store and a
    # server-sent row must not evict all of them. Sending more than the ring holds
    # is still the server lying about what the panel shows, so the bound is checked
    # in the direction that can lie.
    cap = _define(src, "aijudge.h", "AIJUDGE_CAP")
    if budgets["JUDGE_ROWS_MAX"] > cap:
        bad.append("aijudge.h AIJUDGE_CAP holds %d rows, schema.py sends %d"
                   % (cap, budgets["JUDGE_ROWS_MAX"]))

    return bad, ("aijudge.h parity: at[%d] head[%d] body[%d] text[%d] evid=%d "
                 "cap=%d >= rows=%d"
                 % (_array(src, "aijudge.h", "at"), _array(src, "aijudge.h", "head"),
                    _array(src, "aijudge.h", "body"),
                    _array(src, "aijudge.h", "text"),
                    _define(src, "aijudge.h", "AIJUDGE_EVID_MAX"),
                    _define(src, "aijudge.h", "AIJUDGE_CAP"),
                    budgets["JUDGE_ROWS_MAX"]))


def _check_plantrx(src, budgets):
    """Complaints about include/plantrx.h, which spells its buffer sizes as
    PLANTRX_*_CAP defines rather than in the struct - the plan row and the action
    row share one set, so the extent has one name and two users."""
    bad = []
    # The CAP is the array extent and the budget is the text that fits in it, so
    # the two differ by the NUL exactly. Widening the buffer without telling the
    # server buys nothing; narrowing it truncates mid-syllable on the wall.
    for name, key in (("PLANTRX_AT_CAP", "ROW_AT_BYTES"),
                      ("PLANTRX_TAG_CAP", "ROW_TAG_BYTES"),
                      ("PLANTRX_HEAD_CAP", "ROW_HEAD_BYTES"),
                      ("PLANTRX_COND_CAP", "ROW_COND_BYTES"),
                      ("PLANTRX_DELTA_CAP", "ROW_DELTA_BYTES"),
                      ("PLANTRX_NOTICE_CAP", "NOTICE_BYTES"),
                      ("PLANTRX_WLABEL_CAP", "ROW_WLABEL_BYTES"),
                      ("PLANTRX_WSTAT_CAP", "ROW_WSTAT_BYTES"),
                      ("PLANTRX_WBAND_CAP", "ROW_WBAND_BYTES"),
                      ("PLANTRX_CONF_CAP", "SPECIES_CONF_BYTES"),
                      ("PLANTRX_SCI_CAP", "SPECIES_SCI_BYTES")):
        cap = _define(src, "plantrx.h", name)
        if budgets[key] != cap - 1:
            bad.append("plantrx.h %s is %d, leaving %d bytes for text, schema.py budgets %d"
                       % (name, cap, cap - 1, budgets[key]))

    # Counts, not lengths, for the same reason as the judgment ring: a row past
    # the array is dropped on arrival and never drawn.
    for name, key in (("PLANTRX_PLAN_MAX", "PLAN_ROWS_MAX"),
                      ("PLANTRX_ACTION_MAX", "ACTION_ROWS_MAX"),
                      ("PLANTRX_WINDOW_ROWS_MAX", "WINDOW_ROWS_MAX")):
        n = _define(src, "plantrx.h", name)
        if budgets[key] != n:
            bad.append("plantrx.h %s is %d, schema.py budgets %d" % (name, n, budgets[key]))

    return bad, ("plantrx.h parity: at[%d] tag[%d] head[%d] cond[%d] delta[%d] "
                 "notice[%d] wlabel[%d] wstat[%d] wband[%d] conf[%d] sci[%d] "
                 "plan=%d action=%d window=%d"
                 % tuple(_define(src, "plantrx.h", n) for n in (
                     "PLANTRX_AT_CAP", "PLANTRX_TAG_CAP", "PLANTRX_HEAD_CAP",
                     "PLANTRX_COND_CAP", "PLANTRX_DELTA_CAP", "PLANTRX_NOTICE_CAP",
                     "PLANTRX_WLABEL_CAP", "PLANTRX_WSTAT_CAP", "PLANTRX_WBAND_CAP",
                     "PLANTRX_CONF_CAP", "PLANTRX_SCI_CAP",
                     "PLANTRX_PLAN_MAX", "PLANTRX_ACTION_MAX",
                     "PLANTRX_WINDOW_ROWS_MAX")))


_HEADERS = (("aijudge.h", _check_aijudge, _JUDGE_KEYS),
            ("plantrx.h", _check_plantrx, _ROW_KEYS))


def test_budgets_match_the_device_header():
    """schema.py's byte budgets against the buffers they are budgets for.

    The firmware declares fixed sizes because both stores live in PSRAM at a
    known size: include/aijudge.h holds the judgment ring, include/plantrx.h the
    예약 / 조치 rows and the notice. Nothing connects either to schema.py but a
    comment, and a firmware author widening `head` gains nothing until the server
    is told, while narrowing it truncates mid-syllable on the panel with the
    server still believing it fits. render.py already reads the font source for
    the same reason: the firmware is the ground truth, so parse it rather than
    remember it.
    """
    for name, check, _keys in _HEADERS:
        src = _header(name)
        if src is None:
            print("skip: %s not present" % os.path.join(_INCLUDE, name))
            continue
        bad, line = check(src, _budgets())
        assert not bad, "%s: %s" % (name, bad)
        print(line)


def _drift(src, key):
    """The perturbation the check has to notice, for one key.

    One is enough for a parity: a budget that is the array less its NUL disagrees
    with the header as soon as it moves. JUDGE_ROWS_MAX is not a parity - it is
    bounded by AIJUDGE_CAP and sits well under it - so +1 leaves it legal and a
    negative control built on +1 alone would pass while checking nothing. What has
    to be caught there is a row count past the ring.
    """
    if key == "JUDGE_ROWS_MAX":
        return _define(src, "aijudge.h", "AIJUDGE_CAP") + 1
    return getattr(schema, key) + 1


def test_budget_drift_is_caught():
    """The negative control. An agreement test that cannot disagree is a comment,
    and this one is parsing regexes out of a file that is allowed to be absent -
    exactly the shape that silently passes on nothing. Every pair is perturbed one
    at a time, because a check that only notices the field it was written for is how
    the rest of the set stops being checked.
    """
    for name, check, keys in _HEADERS:
        src = _header(name)
        if src is None:
            continue
        for key in keys:
            bad, _line = check(src, _budgets(**{key: _drift(src, key)}))
            assert bad, "%s: a %s drift went unnoticed" % (name, key)


if __name__ == "__main__":
    test_budgets_match_the_device_header()
    test_budget_drift_is_caught()
    empty = test_never_diagnosed()
    full = test_two_turns_by_tense()
    messy = test_the_body_keeps_one_control_character_and_no_others()
    mine = test_device_species_wins()
    theirs = test_server_species_when_the_panel_is_silent()
    print(json.dumps(full["display"], ensure_ascii=False, indent=1))
    print("\nempty  notice=%r turn=%s" % (empty["display"]["notice"], empty["display"]["turn"]))
    print("rows   %s" % [(r["at"], r["level"], r["head"],
                          sum(c["hot"] for c in r["chips"])) for r in full["display"]["judgments"]])
    print("body   %d B, %d line(s): %r"
          % (len(full["display"]["judgments"][0]["body"].encode()),
             full["display"]["judgments"][0]["body"].count("\n") + 1,
             full["display"]["judgments"][0]["body"]))
    print("messy  %d B, %d line(s): %r"
          % (len(messy["display"]["judgments"][0]["body"].encode()),
             messy["display"]["judgments"][0]["body"].count("\n") + 1,
             messy["display"]["judgments"][0]["body"]))
    print("budget judge at<=%d head<=%d body<=%d chip<=%d chips<=%d rows<=%d; "
          "row at<=%d tag<=%d head<=%d cond<=%d delta<=%d notice<=%d; "
          "window label<=%d stat<=%d band<=%d rows<=%d; "
          "species sci<=%d conf<=%d, all within; "
          "body carries '\\n' and no other control character; "
          "every string drawable; no removed field on the wire"
          % (schema.JUDGE_AT_BYTES, schema.JUDGE_HEAD_BYTES, schema.JUDGE_BODY_BYTES,
             schema.JUDGE_CHIP_BYTES,
             schema.JUDGE_CHIPS_MAX, schema.JUDGE_ROWS_MAX,
             schema.ROW_AT_BYTES, schema.ROW_TAG_BYTES, schema.ROW_HEAD_BYTES,
             schema.ROW_COND_BYTES, schema.ROW_DELTA_BYTES, schema.NOTICE_BYTES,
             schema.ROW_WLABEL_BYTES, schema.ROW_WSTAT_BYTES, schema.ROW_WBAND_BYTES,
             schema.WINDOW_ROWS_MAX,
             schema.SPECIES_SCI_BYTES, schema.SPECIES_CONF_BYTES))
    print("window %s span=%ds covered=%ds"
          % (full["display"]["window"], full["display"]["window_span_s"],
             full["display"]["window_covered_s"]))
    print("species device %s -> server did not identify; "
          "silent %s -> server identified, then carried"
          % (mine["display"]["species"], theirs["display"]["species"]))
    print("OK")
