"""The device <-> server contract. Everything else in this package depends on it.

Two rules shape these models.

1. Display strings and control values are separate. `Prescription.display` holds
   finished Korean the device draws verbatim; `Prescription.control` holds the
   numbers its local loop acts on. The device never parses prose to decide
   anything, and the server never ships a number the UI has to format. The
   firmware renders with fonts that only cover Hangul syllables, ASCII and a
   short punctuation set, so every outgoing string goes through render.py.

2. The server sends policy, not commands. Setpoints and schedules describe the
   state to hold; the device runs the fast loop and owns the safety interlocks.
   A dropped response or a dead server therefore costs nothing - the greenhouse
   keeps running the last prescription. `Control.once` is the one exception and
   is deliberately bounded: an expiry, an id for idempotency, and a duration the
   firmware clamps again on its side.
"""

from typing import Literal, Optional

from pydantic import BaseModel, Field

# --------------------------------------------------------------------------
# Device -> server
# --------------------------------------------------------------------------


class Sensors(BaseModel):
    """Raw readings. None where the sensor is absent or has not reported.

    The firmware sends its -999 sentinels as null; lux and soil are permanently
    null today (the BH1750 is dead and no soil probe is wired). Derived values
    like VPD are NOT sent - the server computes them, so changing the formula
    does not mean reflashing three boards.
    """

    co2_ppm: Optional[float] = None
    temp_c: Optional[float] = None
    rh_pct: Optional[float] = None
    lux: Optional[float] = None
    soil_pct: Optional[float] = None
    # Hottest pixel in the 32x24 thermal frame - the scene maximum, NOT leaf
    # temperature. A heater or lamp in view will dominate it. Anything reasoning
    # about this has to be told so.
    leaf_max_c: Optional[float] = None


class Links(BaseModel):
    """Health of the three radio links, so the server can tell "the plant is
    fine" from "we stopped hearing about the plant"."""

    node_online: bool = False
    node_age_ms: int = 0
    node_lost: int = 0
    cam_online: bool = False
    rgb_live: bool = False
    thermal_live: bool = False
    thermal_fps: float = 0.0
    wifi_rssi: int = 0


class Boot(BaseModel):
    """How the last run ended, and whether the current firmware is confirmed.

    The panel's display sits on UART0's pins, so a board running with it has no
    serial console - and from the server's side "quiet" and "panicking every two
    seconds" looked identical, because uptime_ms could only say that a boot was
    young, never why the last one stopped. This block is the difference.

    `crashes` is cumulative across power cycles, out of the device's NVS, so a
    board that crashes and recovers still leaves the trail. `image_pending` means
    the bootloader has not accepted the running firmware: it was delivered over
    OTA and has not proved itself yet, so a restart in that state reverts to the
    previous slot rather than keeping it.

    The whole block is Optional on Telemetry, not defaulted, and that is the point:
    firmware predating it sends nothing, and nothing must be stored as NULL rather
    than as "unknown"/0. "power" and 0 are what a HEALTHY boot reports, so writing
    them for a device that cannot say would claim the one thing this block exists
    to stop guessing at. The columns are nullable for the same reason edges and
    allstops are.
    """

    reset: str
    crashes: int
    image_pending: bool
    # The panic handler's own record of the last crash, read back out of the coredump
    # partition: the task that died, the exception PC, and a backtrace. Present only while a
    # crash has not yet been acknowledged - the device erases its copy once a post carrying
    # these is answered - so absent means "nothing new to report", NOT "nothing ever crashed".
    # reset and crashes answer that question; these answer WHERE, which nothing else can.
    crash_task: Optional[str] = None
    crash_pc: Optional[str] = None
    crash_bt: Optional[str] = None


class DeviceSpecies(BaseModel):
    """What the panel decided the plant is, from its own identification run.

    The device carries PlantNet keys of its own and identifies on a button
    press, with the plant in front of its lens, and src/ui/page_auto.cpp draws
    that answer in preference to anything the server sends. A server that
    identifies again spends a second quota on a second name for the same plant
    and then disagrees with the wall about which one it is, so the device
    reporting this is what stops the server-side identification from running at
    all.

    Sent only when the device actually has a result, and omitted entirely
    otherwise: an empty object here would be indistinguishable from a board that
    identified nothing and would suppress the server's own attempt for nothing.
    """

    sci: str = ""  # scientific name, plantid_species()
    # The string the panel is showing: Korean name, else common name, else sci.
    # Resolved on the device so the server cannot re-derive a different one.
    text: str = ""
    conf_pct: int = 0  # plantid_score() as a whole percent, 0 = not reported


class Telemetry(BaseModel):
    device: str = Field(description="STA MAC, e.g. AA:BB:CC:DD:EE:FF")
    # millis(), not a clock. 0 is "not reported" rather than "just booted" - it
    # is this field's own default, so main._restarted refuses to date a reboot
    # from it and says "cannot tell" instead.
    uptime_ms: int = 0
    sensors: Sensors = Sensors()
    links: Links = Links()
    # Absent unless the firmware reports it; see Boot. None is stored as NULL, so a
    # query can tell "this poll came from a build that cannot say why it restarted"
    # from "it restarted cleanly".
    boot: Optional[Boot] = None
    # Absent unless the device has identified the plant itself; see DeviceSpecies
    # for why its presence suppresses the server's own identification.
    species: Optional[DeviceSpecies] = None
    # Measured hardware, 0..100 (0 = off, 100 = full). Empty on every board so
    # far and for as long as that stays true: no relay is wired, so there is
    # nothing to measure. It is kept because it is the field that will carry the
    # answer to "what did the last prescription actually do" the day one is.
    actuators: dict[str, int] = {}
    # The panel's own switch positions, 0..100 - one key per device the panel
    # owns, its six on/off switches reading 0 or 100 and `fan` carrying its
    # speed. Always present with every key, because the panel always has switch
    # positions; an absent object would be a lie where an absent `species` is
    # merely an unidentified plant.
    #
    # Deliberately NOT folded into `actuators`. That field means measured
    # hardware, and reporting a held switch there would manufacture evidence
    # that an action ran - which is the exact failure this field was added to
    # remove, not one to relocate. The key set doubles as the closed actuator
    # vocabulary brain.py validates `once` and `policy` against, so no second
    # declaration field is needed and a panel that owns six devices cannot be
    # prescribed a seventh.
    actuator_intent: dict[str, int] = {}
    # What actuator_intent structurally cannot say. It is one snapshot per poll,
    # so a switch turned on and off between two polls is absent from both and the
    # server scores that window as though its prescription had been left alone.
    # These two are monotonic counts since boot (include/ui.h), so the movement
    # inside a window is the difference between its ends and never a sum - see
    # derive.interventions. 전체 정지 is counted apart from the edges it causes
    # because it is the one control that means the grower disagreed, and taken
    # back inside its undo window it leaves the switches exactly as the server
    # last saw them.
    #
    # None and not 0, which is where these part company with uptime_ms above. A
    # board cannot have been up for zero milliseconds, so that field can spend 0
    # on "not reported"; zero edges is the commonest true answer there is - a
    # panel nobody touched all hour reports it every poll. A 0 default would tell
    # the model no switch moved on the authority of a firmware that never sent
    # the field, which is the class of lie derive._count exists to refuse.
    edges: Optional[int] = None
    allstops: Optional[int] = None
    auto: bool = True  # AI-RX mode switch: false (판단 전용) = diagnose but do not act
    ask_now: bool = False  # user pressed 지금 진단
    # The prescription the panel is currently executing, which is not always the
    # one the server last issued. main._executing scores the window against the
    # bands this names, so a device running an older prescription is not
    # credited with holding the current one's.
    rx_id: Optional[str] = None


# --------------------------------------------------------------------------
# Server -> device: control half (machine-readable)
# --------------------------------------------------------------------------

MetricKey = Literal["vpd_kpa", "air_c", "rh_pct", "co2_ppm", "leaf_air_dt_c"]

# Canonical spelling for a threshold comparison. "gt"/"lt" rather than ">"/"<"
# because this travels through a JSON schema enum on the way out of the model,
# and an operator that is also a bracket is a lifetime of quoting bugs. Two
# modules had independently picked different spellings before this was written
# down, which silently emptied every wake condition.
WakeOp = Literal["gt", "lt"]


class WakeCondition(BaseModel):
    """"Wake me if `metric` stays past `value` for `for_s` seconds."

    for_s is what stops a reading that oscillates around the threshold from
    triggering a call per sample; the scheduler enforces a floor on it.
    """

    metric: MetricKey
    op: WakeOp
    value: float
    for_s: int = 300


class Setpoint(BaseModel):
    """A band to hold. The device runs hysteresis against it; the server does
    not say which actuator to use, only what to achieve.

    MetricKey is the static half of the vocabulary; the runtime half is per
    device and per window. brain._setpoints drops a band whose key returned no
    sample in the window just measured - a target nothing reads is a target
    nothing can score - so a key can be valid here and still not reach the wire.
    """

    key: MetricKey
    lo: Optional[float] = None
    hi: Optional[float] = None


class Schedule(BaseModel):
    """Time-driven action, for things no sensor closes the loop on yet -
    irrigation, with no soil probe wired."""

    actuator: str
    every_s: int
    duration_s: int


class OnceAction(BaseModel):
    """A single bounded action. The only way the model can cause something to
    happen right now, and it still cannot exceed the firmware's own limits."""

    id: str  # idempotency: the device ignores an id it already ran
    # Left as a plain str rather than an enum: the vocabulary is whatever the
    # panel declared in this poll's Telemetry.actuator_intent, and brain.py
    # checks it against that. A Literal here would be a second, static list of
    # device names to disagree with the panel about.
    actuator: str
    seconds: int
    before_ts: int  # unix seconds; ignored once expired


class Control(BaseModel):
    setpoints: list[Setpoint] = []
    schedules: list[Schedule] = []
    once: list[OnceAction] = []
    # Per actuator: "auto" lets the loop drive it, "off" holds it off. There is
    # no "on" - forcing an actuator on from the server is what interlocks exist
    # to prevent. Keys carry the same constraint OnceAction.actuator does.
    policy: dict[str, Literal["auto", "off"]] = {}


# --------------------------------------------------------------------------
# Server -> device: display half (finished strings)
# --------------------------------------------------------------------------

# The device keeps the judgment log in fixed-size C buffers (include/aijudge.h),
# so every field below has a byte budget, not a character budget: `char head[64]`
# holds 63 UTF-8 bytes, which is 21 Hangul syllables and not 63. render.py clips
# against these numbers on a code-point boundary. Clipping to a character count
# instead is what leaves half a UTF-8 sequence in the buffer and draws one broken
# glyph on the panel, which is invisible in a diff and obvious on the wall.
#
# One row, where there used to be six. The 판단 column stopped being a history
# list: it draws one card for the newest judgment - the model's prose in full, the
# readings behind it, and the two frames it was looking at - so five carried rows
# were payload with no reader, on a response the firmware reads into a 16 KB
# buffer. This narrows what the server SENDS, not what the panel keeps: the ring
# is still AIJUDGE_CAP deep because the firmware authors rule rows into it, so
# this number is bounded by that one rather than equal to it.
#
# Five chips, where there used to be three. Three was the row width at
# font_reg_12 in a list of six rows; the card gives the readings a line of their
# own across the column, and a judgment that turned on VPD is worth reading
# beside temperature, humidity, CO2 and the scene peak rather than two of them.
#
# body is the reason the card exists at all. head is 63 bytes, which is one
# clause, and it used to be everything the model's prose survived as -
# render._conclusion threw away every sentence but the last and clipped that to
# 21 characters, and notes_ko never left the server. brain.clamp_output already
# caps the model at 120 characters of diagnosis plus 200 of notes, so the worst
# case is 320 characters: 960 bytes if every one of them is Hangul at three bytes
# a syllable, plus the newline that joins the two. 1023 holds that whole, which
# makes render's clip a backstop rather than the normal path - the point of the
# field is that the prose arrives intact.
JUDGE_AT_BYTES = 7        # char at[8]
JUDGE_HEAD_BYTES = 63     # char head[64]
JUDGE_BODY_BYTES = 1023   # char body[1024]
JUDGE_CHIP_BYTES = 27     # char text[28] in JudgeEvid
JUDGE_CHIPS_MAX = 5       # AIJUDGE_EVID_MAX
JUDGE_ROWS_MAX = 1        # <= AIJUDGE_CAP; the card draws the newest row only

# The 예약 / 조치 rows land in fixed C buffers as well (include/plantrx.h), so the
# same reasoning governs their limits, and one row shape covers both columns
# because the device declares one struct shape for both. head gets the same 63
# bytes a judgment head gets, for a measured reason: a prescription ordering one
# action against one setpoint renders "미스트 1분 가동, VPD 목표 0.8 – 1.2 kPa",
# which is 49 bytes - the separator is render._band's en dash, three of them, so
# an ASCII hyphen reproduces 47 and not the figure that matters - so the 48-byte
# plan-head budget this replaces clipped the commonest row on the panel.
ROW_AT_BYTES = 7          # char at[8]
ROW_TAG_BYTES = 12        # char tag[13]
ROW_HEAD_BYTES = 63       # char head[64]
ROW_COND_BYTES = 78       # char cond[79]
ROW_DELTA_BYTES = 48      # char delta[49]
NOTICE_BYTES = 200        # char notice[201]
PLAN_ROWS_MAX = 4         # PLANTRX_PLAN_MAX
ACTION_ROWS_MAX = 4       # PLANTRX_ACTION_MAX

# The window block: what the last hour actually looked like, as a small table on
# the monitor page. Same fixed-buffer reasoning as the rows above (the device
# holds it in RxWindowRow, include/plantrx.h), so the same byte budgets.
#
# The stat column is the widest of the three and is deliberately the tightest
# fit: "0.8 / 1.1 / 1.4 kPa" is 20 bytes, and the widest realistic CO2 form
# ("1200 / 1350 / 1480 ppm") is 23, so 24 holds every reading the panel's own
# sensors can produce and clips only an implausible five-digit ppm - which is
# what _fit is for. Widening it costs 4 bytes x 4 rows on a wire the firmware
# sizes in kilobytes, but it also costs 4 px a row on a page that has none.
ROW_WLABEL_BYTES = 16     # char label[17]; widest is 장면최고차 at 15
ROW_WSTAT_BYTES = 24      # char stat[25]
ROW_WBAND_BYTES = 20      # char band[21]; "유지 100%" is 11
WINDOW_ROWS_MAX = 4       # PLANTRX_WINDOW_ROWS_MAX

# The species card's two strings beside the name.
#
# The confidence: "100%" is the widest value render can build (the percent is
# clamped to 0..100 before it is formatted), so 4 bytes is the real worst case.
# The budget is 7 because the carried path echoes whatever string the last
# prescription printed rather than re-deriving it, and a clip at 4 would cut a
# grown value down to "100" - a bare count where a percentage was meant.
#
# The binomial: PlantNet returns scientificNameWithoutAuthor, so no author string
# is ever appended. "Chrysanthemum leucanthemum" is 26 bytes and 174 px at
# font_reg_12, the widest this had to survive; 31 leaves a longer genus room and
# clips on a whole character either way, Latin being one byte per glyph.
#
# Both mirror include/plantrx.h on the usual BYTES+1 rule.
SPECIES_CONF_BYTES = 7
SPECIES_SCI_BYTES = 31

Tone = Literal["ok", "warn", "info"]

# What a whole finding amounts to. Separate from Tone because a chip is tinted by
# whether its own reading sits inside its band, while a row is badged by the
# verdict; the device draws them with different widgets and needs three levels
# for the badge where a chip only ever needs two and a neutral.
Level = Literal["ok", "warn", "alert"]


class Chip(BaseModel):
    text: str
    tone: Tone = "info"
    # The reading the verdict turned on. At most one chip per row carries it and
    # the device tints that one with the row's own level colour, so the badge and
    # the number that caused it read as one thing. A row that nothing fired on
    # has no hot chip at all - see the EV_NORMAL case in src/aijudge.cpp, which
    # leads with VPD as the headline number rather than as the culprit.
    hot: bool = False


class Species(BaseModel):
    """The name on the header strip, how sure whoever named it was, and the key
    that says of what.

    All three are drawn, and the binomial is not decoration. plantnet's
    _resolve_korean falls back to machine-translating the English common name
    when Wikipedia has no Korean article, so `text` can be a plausible-looking
    Korean string that is wrong - and Korean plant names collide across species
    anyway ("고무나무" spans most of Ficus). On the path this class exists for, where
    the server identified the plant and nobody pressed the panel's 식별 button,
    the binomial is the only thing on the card a grower can actually check.
    """

    text: str  # what the card shows: Korean name when resolved
    sci: str = ""  # scientific name, the verifiable key behind `text`
    # Pre-formatted because the server owns the rounding: the device draws this
    # string as-is on the header chip. "" is "no figure reported", which the panel
    # draws as a hidden chip - distinct from "0%", which is a real confidence.
    conf_text: str = ""  # "82%"


class JudgeRow(BaseModel):
    """The one judgment the card draws: what was concluded, when, on which
    readings, and the reasoning in the model's own words.

    EVIDENCE IS SELECTED, NOT DUMPED - the same rule the firmware's rule producer
    follows. A row carries only the metrics bearing on its own finding, so the
    reader can tell which number caused the verdict. Attaching every sensor to
    every row is what makes a log unreadable, and the row that says "CO2 부족"
    gains nothing from a humidity reading.

    Chip text is formatted by the server from the stored reading and never copied
    out of model prose: the model selects which metrics matter, the server prints
    their measured values. A number the model retyped is a number it can get
    wrong, and a wrong number beside a right verdict is the one failure a grower
    cannot detect from the panel.

    Chip order is the deciding metric first, then its context, matching the
    firmware - the hot chip is index 0 because it is what the rule fired on.

    `head` and `body` are the same answer at two lengths, and both are drawn. The
    head is the clause worth putting next to a timestamp; the body is everything
    the model said, which the panel now has the room for and which used to be
    thrown away here - one sentence of it clipped to 21 characters was the whole
    of it that ever reached the wall.
    """

    at: str = "--:--"       # HH:MM, or "--:--" when no wall clock was available
    level: Level = "ok"
    head: str = ""          # the finding, e.g. "엽온 상승, 기공 폐쇄 의심"
    # The model's full prose: diagnosis_ko, and notes_ko after it when it wrote
    # any. '\n' is the one control character that survives render's guard() into
    # this field, because LVGL draws it as a line break and the two halves are two
    # paragraphs; every other one still flattens to a space. <= JUDGE_BODY_BYTES.
    body: str = ""
    chips: list[Chip] = []  # <= JUDGE_CHIPS_MAX, at most one hot
    # Whether the server holds the frames this row was decided from. The device
    # draws its own frozen thumbnails; these say whether they are the ones that
    # were actually looked at.
    has_rgb: bool = False
    has_thermal: bool = False


class Turn(BaseModel):
    """The judgment schedule: when the next one is due.

    Drawn on the 판단 card, under the judgment it is the sequel to, and not in the
    예약 column where it used to sit. 예약 is what the model asked the actuators to
    do; when the model next speaks is a fact about the judgment, and a grower
    reading "이것이 왜 이렇게 판단되었나" reads "그리고 다음은 언제인가" in the
    same breath.

    An absolute deadline rather than a remaining count: the same prescription is
    returned on every poll until a new one is issued, so a remaining-seconds
    field would freeze at whatever it held when the row was written and the
    countdown would stop while looking alive. `scheduled` is false while no turn
    has been scheduled yet, which the device draws as 대기 rather than 00:00.

    This turn supersedes the device's own rule turn while `scheduled` holds. The
    firmware schedules a local judgment turn so an offline board still fills the
    log, but the log is one log and the column is one column: two countdowns on
    one panel would be two answers to the same question. Once a prescription
    arrives with `scheduled`, the device stops drawing its local period and draws
    this deadline, which is also when the next row will actually be authored.
    """

    scheduled: bool = False
    next_ts: int = 0  # unix seconds; meaningless to the device until NTP lands
    period_s: int = 0


class PlanRow(BaseModel):
    """Future tense: something that will happen, and what makes it happen.

    `at` is empty for a purely conditional entry - a wake threshold has no clock
    time, and inventing one would claim a certainty the condition does not have.
    """

    at: str = ""
    tag: str = "대기"
    tone: Tone = "info"
    head: str = ""  # "미스트 12분 가동"
    cond: str = ""  # "조건: VPD 1.8 이상 10분 지속"


class ActionRow(BaseModel):
    """Executed tense.

    `delta` is either a measured reading transition or the reason there is none,
    and `delta_is_reading` says which: the device draws a measurement as a chip
    and a reason as prose, and cannot tell them apart from the string.
    """

    at: str = ""
    tag: str = "완료"
    tone: Tone = "ok"
    head: str = ""  # "미스트 12분 가동"
    delta: str = ""  # "VPD 2.1 → 1.3 kPa", or "미스트 스위치 계속 꺼짐"
    delta_is_reading: bool = False
    improved: bool = False


class WindowRow(BaseModel):
    """Measured tense: one metric over the window the last prescription ran in.

    Not a tense of the 판단 card, which is why it is drawn on the monitor page
    and not beside the other three. ActionRow.delta answers "what did the last
    prescription move" with one headline; this answers "what did the last hour
    actually look like" with a table, and the two only look like duplicates
    because they quote the same instrument.

    `band` is empty - not "-", not "0%" - when the metric had no band on either
    side, because there is no holding to report and an empty string is how every
    other field on this wire says it has nothing to say. `in_band_pct` is None in
    exactly the same case, and the device tints the row from it rather than from
    the string, so the two must agree.
    """

    label: str = ""       # "VPD", "기온", "습도", "CO2"
    stat: str = ""        # "0.8 / 1.1 / 1.4 kPa" - min / mean / max, as drawn
    band: str = ""        # "유지 87%", or "" when the metric was unbanded
    in_band_pct: Optional[int] = None  # 0..100; None where band is ""


class Display(BaseModel):
    """Laid out by tense, matching src/ui/page_auto.cpp: judgments are past, plan
    is future, actions are executed.

    A field belonging to no tense does not belong on the card. `targets` used to
    live here and was removed with the UI's 목표 rows: "VPD 0.8 - 1.2 kPa" is
    unjudgeable without the current value beside it, and the bands already travel
    in Control.setpoints, which is where the device's own loop reads them.
    `diagnosis` / `last_run` / `next_run` went the same way - the first is now a
    judgment row's head, and the other two were the same event stated twice, once
    as prose and once as the first row of the trail.

    The window block is the one part of this that is not drawn on the 판단 card:
    it goes to the monitor page, where the grower is already looking at readings.
    It defaults to an empty list and two zeros rather than being optional, so a
    device polling a server that predates it, or one whose history is still a
    single row, gets a well-formed empty block instead of a missing key it has to
    have a second code path for.

    model_ready belongs to no tense either, and is on the card anyway: it is not
    an entry, it is who is entitled to have authored the entries. The panel drew
    all three columns as the model's on the strength of a server existing, so a
    keyless install badged its own rule output 모델. That is provenance for the
    whole card, which is a flag and not a row.
    """

    species: Optional[Species] = None
    # A list of one, and still a list: <= JUDGE_ROWS_MAX. The panel draws the
    # newest judgment as one card, so a second row would be payload nothing reads,
    # but the shape stays plural because the empty case is a real one - a server
    # with no model ships zero rows and says so in `notice`.
    judgments: list[JudgeRow] = []
    turn: Turn = Turn()
    plan: list[PlanRow] = []
    actions: list[ActionRow] = []
    window: list[WindowRow] = []  # <= WINDOW_ROWS_MAX, in render._ORDER order
    # How much wall clock the window covers, and how much of it was actually
    # sampled. covered_s < span_s means the device was off the air in the middle
    # and the percentages above are over less time than the header implies; the
    # panel has no other way to know that. 0 means nothing was measured at all.
    window_span_s: int = 0
    window_covered_s: int = 0
    # Drawn in place of the judgment column while it is empty. On the panel a
    # blank column and a column that failed to draw look identical.
    notice: str = ""
    # Whether a model is actually configured and answering server-side - the
    # server's own brain.is_configured(), which the device cannot observe: a
    # keyless server answers every poll with a well-formed 200 and its rules'
    # numbers in it. Both builders in render.py state this explicitly, so the
    # default only ever covers a Display built by something that is not the
    # renderer. The DEVICE defaults the same absence the other way, to false,
    # because an absent field is not evidence of a model - see
    # plantrx_model_ready() in include/plantrx.h.
    model_ready: bool = True


# --------------------------------------------------------------------------
# Server -> device: the response
# --------------------------------------------------------------------------


class Prescription(BaseModel):
    """Body of every /v1/telemetry response.

    The device compares rx_id against what it is running and only redraws on a
    change, so this can be returned in full every poll.
    """

    rx_id: str
    issued_ts: int
    # How long to wait before polling again. The server drives the cadence:
    # ~60s idle, ~2-3s just after something happened, so the link is cheap when
    # nothing is going on and feels immediate when it is. The device never opens
    # a listening socket - it is behind NAT and always will be.
    next_poll_s: int = 60
    want_frame: bool = False  # upload RGB + thermal on the next tick
    # Stand everything else down and wait for a firmware upload. One-shot on the wire: the
    # server hands this out exactly once and clears it as it does (store.take_update_mode),
    # because the mode the device enters has no exit except a restart - so a device that
    # obeyed it, took its update and rebooted would read the same flag on its very first poll
    # back and be taken over again, and again after that reboot, with nothing on the panel
    # explaining why it will not stay usable.
    #
    # False by default, and that default is the entire compatibility story. Firmware built
    # before this key existed never looks for it; a server that never arms it puts the same
    # bytes on the wire it always did; and every prescription already stored on disk - none of
    # which has the key - deserialises to the answer that leaves the panel alone.
    update_mode: bool = False
    # Which of the two things "update mode" means this time. Both ride the same one-shot
    # arming and both stand the panel down, so this is a modifier on update_mode rather than a
    # flag of its own: false is the push path the field above has always described - wait, and
    # somebody will espota an image at you - and true is the pull path, where the panel goes
    # and fetches /v1/firmware/image itself because nobody with a toolchain is coming.
    #
    # Never true with update_mode false. The server arms them together and hands them out in
    # the same statement (store.take_update_mode), because a pull that did not first stand the
    # camera, the poll and the ESP-NOW radio down would be a 2.5MB flash write sharing a board
    # with all three.
    #
    # False by default for the same compatibility reason as update_mode, and it is doing real
    # work here: firmware that predates this key sees only update_mode and takes the push path,
    # which is exactly what it should do, because it has no puller to run.
    firmware_pull: bool = False
    # Pass the update on: tell the ESP32-CAM, or the sensor node, to fetch its own image. One
    # flag per role rather than one string, because the panel reads them into
    # `bool node_pull[NODE_ROLE_COUNT]` indexed by role (parse_prescription in src/plantrx.cpp)
    # and its JSON scanner reads scalars, not a name it would then have to map back to an index.
    #
    # NOT a variant of the two fields above, and the panel enforces the difference: it dispatches
    # nodeota_request() only when neither update_mode nor firmware_pull was set on the same
    # response. A node update is watched BY the panel - the panel relays the command over
    # ESP-NOW, receives the node's NODE_PROG reports and draws them - so arming one on a panel
    # that is about to stand down and reboot would start an update with nobody left watching it,
    # on a board with no console to ask afterwards. The server keeps its side of that by not
    # consuming these on a poll that carries an update_mode arming (see main.telemetry), so the
    # request survives the panel's reboot instead of being spent on a response it will ignore.
    #
    # One-shot, like update_mode, and here the clearing is what stands between an operator and a
    # camera that reboots every minute: these do not stop the panel polling, so a flag that
    # outlived its delivery would re-arm the node's update on every poll forever. See
    # store.take_node_pull.
    #
    # False by default for the same compatibility reason as the two above, in both directions: a
    # panel built before these keys existed never looks for them, and a stored prescription that
    # predates them deserialises to "no node was asked to do anything".
    node_pull_cam: bool = False
    node_pull_node: bool = False
    mode: Literal["auto", "advisory"] = "auto"
    control: Control = Control()
    display: Display = Display()


class FrameAck(BaseModel):
    ok: bool = True
    rx_id: Optional[str] = None


class Identification(BaseModel):
    """Body of /v1/identify: the answer to the 식별 button on the 자동 page.

    The device photographs the plant and asks "what is this", and this is the
    whole answer - the name in three spellings, how sure PlantNet was, and what
    is left of today's quota so the panel can print "n회 남음" without a second
    call. On failure `reason` is the Korean line the panel prints verbatim; the
    device does not translate, map or otherwise interpret it.

    Deliberately flat, and it has to stay that way. The device parses this with
    the strstr-based scanners in src/plantid.cpp (`json_str`, `json_num`), which
    find a key anywhere in the buffer and read the scalar after it. They cannot
    walk nesting, so a nested object would either be missed or - worse - have an
    inner key of the same name silently read as an outer one. The route is
    declared with response_model_exclude_none=True so that only the fields the
    branch actually filled reach the wire: a null is a key the scanner would
    still find and then fail to read.
    """
    ok: bool
    sci: Optional[str] = None
    common: Optional[str] = None
    korean: Optional[str] = None
    score: Optional[float] = None      # 0..1, as PlantNet reports it
    reason: Optional[str] = None       # set only when ok is false
    remaining: int                     # -1 when no keys are configured
    quota: int                         # -1 when no keys are configured
    measured: bool                     # whether remaining is a count or a ceiling


# --------------------------------------------------------------------------
# Server -> device: the published firmware
# --------------------------------------------------------------------------


class FirmwareManifest(BaseModel):
    """Body of /v1/firmware/latest: what is published, described well enough to skip it.

    The panel fetches this before it fetches anything large, and the entire point of the first
    two fields is that most fetches stop here. elf_sha256 is compared against the device's own
    esp_app_get_description()->app_elf_sha256; equal means it is already running this image and
    2.5MB does not need to move.

    The rest is what makes the download safe rather than hopeful. size is passed to
    Update.begin() before the first byte arrives - the device has to commit to a partition size
    up front, which is why the image response carries a Content-Length rather than arriving
    chunked. md5 goes to Update.setMD5(), so a truncated or swapped-mid-flight image is
    rejected by the bootloader-adjacent code that wrote it instead of by three failed boots.

    idf_ver and mtime are for the human reading a log or a panel, not for any decision: they
    answer "which build is this" in a form a person recognises, which a 64-character hash does
    not.
    """

    elf_sha256: str
    size: int
    md5: str
    idf_ver: str
    mtime: int


# --------------------------------------------------------------------------
# Panel -> server: what the nodes said
# --------------------------------------------------------------------------

# A log line off the wire cannot be longer than this, because shared/nodeproto.h gives
# NodeRepMsg one `char text[NODEPROTO_TEXT]` at 160 bytes - 159 of content and a NUL - and a
# node has no way to send more. 200 rather than 160 because the panel is allowed to annotate a
# line on its way past (a role prefix, a "..." where it dropped some) without that being the
# thing that rejects a batch, and because a character is not a byte: the cap pydantic enforces
# counts code points, so a Korean line at the wire limit is 53 characters and an ASCII one is
# 159. Serial output on both nodes is English by convention, so 159 is the case that binds.
NODELOG_TEXT_MAX = 200

# Lines per POST. The panel batches whatever it heard since its last post, and a node with
# verbose logging on can talk faster than the panel posts - so this is the number that decides
# whether a runaway node fills the volume or gets refused. 64 lines is several seconds of a
# chatty update at ESP-NOW's realistic rate, and about 13KB at the text cap above, which is a
# request body the panel can build in one buffer.
NODELOG_LINES_MAX = 64


class NodeLogLine(BaseModel):
    """One line a node said, plus the panel's note of when it heard it.

    `role` and `text` are the node's, relayed untouched, so a reader can line these up against
    what the node's own serial console would have shown if anybody could reach it.

    `ms` is NOT the node's clock, and no amount of wanting it to be makes it one: NodeRepMsg has
    no millisecond field to carry one. Its only node clock is `uptime_s`, in whole seconds, and
    the log branch discards even that (shared/nodeproto.h:123, src/nodeota.cpp:396). What arrives
    here is the PANEL's millis() at the moment the line landed over ESP-NOW (src/nodelog.cpp:68,
    :82). That buys the one thing recv_ts cannot: ordering within a batch at millisecond
    resolution, since the server stamps a whole batch once. It buys no view of the node's own
    timeline - a node reboot is invisible in it - and it wraps to 0 after ~49.7 days of PANEL
    uptime.

    `role` is a plain string and not a Literal, deliberately: nodeproto_role_name() answers "?"
    for a role byte it does not recognise, and a version-skewed node saying something is exactly
    the case this whole path exists to make visible. Refusing the batch would throw away the log
    line that explains the skew.
    """

    role: str = Field(max_length=16)
    ms: int = Field(ge=0)
    text: str = Field(max_length=NODELOG_TEXT_MAX)


class NodeLogBatch(BaseModel):
    """Body of POST /v1/nodelog: what one panel heard from its nodes since it last posted.

    `device` is the panel's STA MAC, the same identity it polls with - not the node's. A node
    has no identity on this server: it never authenticates, never polls, and its MAC is known
    only to the panel that unicasts commands back to it. So a row says which panel's greenhouse
    a line came from and which kind of board said it, and that is enough to read the log.

    Both caps refuse the whole batch rather than trimming it. A batch over the line cap is not a
    node being chatty - the panel's own buffer is smaller than this - it is a caller that is not
    the panel, and silently storing the first 64 lines of it would leave a table full of rows
    nobody can account for.
    """

    device: str = Field(max_length=64)
    lines: list[NodeLogLine] = Field(default_factory=list, max_length=NODELOG_LINES_MAX)


class NodeLogRow(BaseModel):
    """One stored line, as GET /v1/nodelog hands it back.

    NodeLogLine plus the two things the server knows and the node does not: which panel
    forwarded it, and when this server received it. recv_ts is what orders rows across any
    reboot, on either board, and the read sorts by rowid which follows it. `ms` is the
    forwarding panel's arrival clock and separates lines inside one batch - it is not the node's
    clock and says nothing about a node restarting. See NodeLogLine.

    Read by a person with curl, not by a device, which is why nothing here is trimmed for a
    fixed-size C buffer the way the display half is.
    """

    device: str
    role: str
    ms: int
    recv_ts: int
    text: str
