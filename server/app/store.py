"""SQLite persistence. stdlib sqlite3 only - no ORM, no migrations.

The device is the source of truth for what is happening right now; this file
exists so the server can answer "what did the last prescription actually
achieve" and so a restart does not lose the thread. It is one file on the
Dokploy volume, small enough to scp off and open locally when something looks
wrong, which is the whole reason for not putting a database server in the way.

Nothing here computes anything. Derived values arrive already computed from
derive.py and are stored as columns so a window query does not have to
recalculate a fortnight of VPD.
"""

import json
import logging
import os
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any, Optional

from pydantic import ValidationError

from .schema import NodeLogLine, Prescription, Telemetry

log = logging.getLogger(__name__)

# Telemetry is the only table that grows with time rather than with the number
# of devices. Two weeks is long enough to look back at a plant's week and short
# enough that the file stays in the tens of MB at a 2s poll.
TELEMETRY_TTL_S = 14 * 24 * 3600

# Prune on a counter, not on every insert: at a 2s poll this is one delete scan
# per ~7 minutes instead of one per row, and nothing cares whether a
# fortnight-old row survives another few hundred writes.
PRUNE_EVERY = 200

# Frames are the only rows big enough to matter (an RGB JPEG is tens of KB
# against ~200 bytes for a telemetry row). Eight per device per kind is enough
# to show the model a short before/after strip without the file growing without
# bound.
FRAME_KEEP = 8

FRAME_KINDS = ("rgb", "thermal")

# Node log lines are the shortest-lived rows in this file, and they get their own cutoff rather
# than telemetry's fortnight because they are written at a completely different rate: a node with
# verbose logging on emits lines as fast as it has something to say, where telemetry is one row
# per poll. Three days is long enough to read back what a node said during last night's failed
# update and short enough that a "log on" somebody forgot to turn off cannot quietly fill the
# volume the database and the firmware images share.
NODE_LOG_TTL_S = 3 * 24 * 3600

# What GET /v1/nodelog hands back when it is not told, and the most it will hand back when it is
# asked for more. The ceiling is enforced in recent_node_logs rather than in the route, so a
# second caller cannot ask for the whole table by skipping the clamp: these rows are read into a
# JSON array in memory, and "give me everything" against three days of a chatty node is the one
# request that turns a debugging aid into an outage.
NODE_LOG_READ_DEFAULT = 200
NODE_LOG_READ_MAX = 1000

_SCHEMA = """
CREATE TABLE IF NOT EXISTS telemetry (
    id            INTEGER PRIMARY KEY,
    device        TEXT    NOT NULL,
    recv_ts       INTEGER NOT NULL,
    uptime_ms     INTEGER NOT NULL DEFAULT 0,
    co2_ppm       REAL,
    temp_c        REAL,
    rh_pct        REAL,
    lux           REAL,
    soil_pct      REAL,
    leaf_max_c    REAL,
    vpd_kpa       REAL,
    leaf_air_dt_c REAL,
    node_online   INTEGER NOT NULL DEFAULT 0,
    node_age_ms   INTEGER NOT NULL DEFAULT 0,
    node_lost     INTEGER NOT NULL DEFAULT 0,
    cam_online    INTEGER NOT NULL DEFAULT 0,
    rgb_live      INTEGER NOT NULL DEFAULT 0,
    thermal_live  INTEGER NOT NULL DEFAULT 0,
    thermal_fps   REAL    NOT NULL DEFAULT 0,
    wifi_rssi     INTEGER NOT NULL DEFAULT 0,
    actuators     TEXT    NOT NULL DEFAULT '{}',
    auto          INTEGER NOT NULL DEFAULT 1,
    ask_now       INTEGER NOT NULL DEFAULT 0,
    rx_id         TEXT,
    species_sci   TEXT,
    species_text  TEXT,
    species_conf_pct INTEGER,
    actuator_intent TEXT NOT NULL DEFAULT '{}',
    edges         INTEGER,
    allstops      INTEGER,
    reset_reason  TEXT,
    crashes       INTEGER,
    image_pending INTEGER,
    crash_task    TEXT,
    crash_pc      TEXT,
    crash_bt      TEXT
);
CREATE INDEX IF NOT EXISTS ix_telemetry_device_ts
    ON telemetry (device, recv_ts);

CREATE TABLE IF NOT EXISTS prescriptions (
    rx_id     TEXT PRIMARY KEY,
    device    TEXT    NOT NULL,
    issued_ts INTEGER NOT NULL,
    rx_json   TEXT    NOT NULL,
    raw_json  TEXT
);
CREATE INDEX IF NOT EXISTS ix_prescriptions_device_ts
    ON prescriptions (device, issued_ts);

CREATE TABLE IF NOT EXISTS frames (
    id     INTEGER PRIMARY KEY,
    device TEXT    NOT NULL,
    kind   TEXT    NOT NULL,
    ts     INTEGER NOT NULL,
    blob   BLOB    NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_frames_device_kind
    ON frames (device, kind, id);

CREATE TABLE IF NOT EXISTS llm_calls (
    id     INTEGER PRIMARY KEY,
    device TEXT    NOT NULL,
    ts     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_llm_calls_device_ts
    ON llm_calls (device, ts);

-- Standing state the operator writes and the poll handler consumes, one row per device that has
-- ever been sent one of these. Deliberately not a column on `telemetry`: that table is
-- append-only history written by the device, and a consumable flag living on it would have to be
-- cleared by rewriting a row whose whole job is to be what the device said at one instant.
CREATE TABLE IF NOT EXISTS device_flags (
    device      TEXT PRIMARY KEY,
    update_mode INTEGER NOT NULL DEFAULT 0,
    -- When the arming was written. An arming means "I am about to upload", and that claim goes
    -- stale: a panel that was unplugged for a week would otherwise take the update mode on its
    -- first poll back, minutes before anybody was watching it, and sit in a five-minute takeover
    -- for a request nobody remembers making.
    armed_ts    INTEGER NOT NULL DEFAULT 0,
    -- Which kind of update this arming asked for: 0 waits for a push, 1 tells the panel to go
    -- and fetch the image itself. A modifier on update_mode rather than a flag of its own -
    -- written with it, delivered with it, and never read except by the statement that consumes
    -- it - because a pull with nothing stood down is a 2.5MB flash write sharing a board with
    -- the camera, the poll and the ESP-NOW radio. Nothing may act on this column while
    -- update_mode is 0; see take_update_mode for why it is cleared a beat later than its
    -- arming is. Defaulting to 0 is also what makes the migration below correct: an arming
    -- written before this column existed was a push request, and 0 is what a push looks like.
    firmware_pull INTEGER NOT NULL DEFAULT 0,
    -- Arm a firmware update on one of the two nodes, delivered on the panel's next poll and
    -- cleared as it is delivered, exactly like update_mode above. One column per role rather
    -- than one column holding a role name, because the two are independently armable and an
    -- operator updating both boards would otherwise have the second POST overwrite the first.
    --
    -- Unlike update_mode these do NOT stand the panel down - it keeps polling on its normal
    -- cadence - so the take-and-clear in take_node_pull is load-bearing in a way it is not for
    -- the panel's own flags, where main.cpp stops calling the poll at all. A node pull that
    -- failed to clear would re-arm the node's update on every single poll, forever.
    node_pull_cam  INTEGER NOT NULL DEFAULT 0,
    node_pull_node INTEGER NOT NULL DEFAULT 0
);

-- One line a node said, as the panel heard it. The two nodes have no console anybody can reach:
-- the ESP32-CAM's UART is inside a sealed housing on a pole, and the sensor node's is a
-- DevKit's, on a bench nobody is standing at when the thing misbehaves. So a line travels node
-- -> panel over ESP-NOW (NodeRepMsg, NODE_LOG) and the panel POSTs batches of them here. Nodes
-- never talk to this server except to fetch an image.
--
-- recv_ts is not optional and is the only column this table can be ordered by. `ms` is NOT the
-- node's clock - NodeRepMsg has no millisecond field, so the panel stamps its own millis() when
-- the line arrives over ESP-NOW (src/nodelog.cpp:68). It is stored because it is the only thing
-- that orders lines *within* one batch, where recv_ts is whole seconds and stamped once for all
-- of them. It cannot be the sort key: it is one panel's uptime, it wraps at ~49.7 days, and it
-- says nothing at all about the node reboot that ends every successful update.
--
-- `device` is the panel that forwarded the batch, not the node that said it: the node has no
-- identity on this server (it never authenticates, never polls, and its MAC is known only to
-- the panel), so `role` is what says which board a line came off.
CREATE TABLE IF NOT EXISTS node_logs (
    id      INTEGER PRIMARY KEY,
    device  TEXT    NOT NULL,
    role    TEXT    NOT NULL,
    ms      INTEGER NOT NULL,
    recv_ts INTEGER NOT NULL,
    text    TEXT    NOT NULL
);
-- Newest-first, optionally for one role, is the only query this table has. id descending IS
-- newest-first - the rowid is monotonic and recv_ts is not unique across a batch - so the index
-- exists only to keep the role filter off a full scan.
CREATE INDEX IF NOT EXISTS ix_node_logs_role_id
    ON node_logs (role, id);
"""

_TELEMETRY_COLS = (
    "device, recv_ts, uptime_ms,"
    " co2_ppm, temp_c, rh_pct, lux, soil_pct, leaf_max_c,"
    " vpd_kpa, leaf_air_dt_c,"
    " node_online, node_age_ms, node_lost,"
    " cam_online, rgb_live, thermal_live, thermal_fps, wifi_rssi,"
    " actuators, auto, ask_now, rx_id,"
    " species_sci, species_text, species_conf_pct,"
    " actuator_intent, edges, allstops,"
    " reset_reason, crashes, image_pending,"
    " crash_task, crash_pc, crash_bt"
)

# Columns added to `telemetry` after the first deployments, and the reason this
# file has a migration at all.
#
# Three flat columns rather than one JSON blob: every other wire field on this
# table is its own column, _row_to_dict hands rows straight to derive and into
# the model prompt, and "which polls reported a species" stays a column read
# instead of a LIKE scan over serialised text. `actuators` and `actuator_intent`
# are JSON because their keys are the device's to choose - the second one IS the
# panel's declaration of what it owns, so a column per actuator would have to be
# migrated every time a board grows a switch. A species has exactly three fields
# and always will.
#
# SQLite has no ADD COLUMN IF NOT EXISTS, and every dev database predates these,
# so the columns are reconciled against PRAGMA table_info on connect. Putting
# them only in _SCHEMA would work on a fresh file and fail on every existing
# one, which is the failure mode that is invisible until it is production.
_TELEMETRY_ADDED_COLS = (
    ("species_sci", "TEXT"),
    ("species_text", "TEXT"),
    ("species_conf_pct", "INTEGER"),
    # NOT NULL DEFAULT '{}' so the ALTER can add it to a table that already has
    # rows: every poll before this column existed reported no switch positions,
    # and '{}' is how derive already reads "nothing was reported" - it is not the
    # same claim as every switch being off, which is what a zero-filled default
    # would have written into two weeks of history.
    ("actuator_intent", "TEXT NOT NULL DEFAULT '{}'"),
    # Nullable, unlike actuator_intent above and for the opposite reason. '{}'
    # is what an unreported switch snapshot already looks like, so backfilling it
    # states nothing; 0 is what an untouched panel reports, so backfilling that
    # would write "nobody touched this board" across two weeks of polls that
    # predate the counter. NULL is the only value those rows support.
    ("edges", "INTEGER"),
    ("allstops", "INTEGER"),
    # Nullable, like the two above. "power" is what a healthy boot reports, so
    # backfilling it would claim that two weeks of rows came up cleanly - and the
    # whole reason this column exists is that we could not tell. NULL is the only
    # value a row predating the field can carry.
    ("reset_reason", "TEXT"),
    # Nullable for the same reason as reset_reason, and with a second meaning on top: the
    # device sends these only while a crash is unacknowledged, so NULL is the normal case on a
    # board that has not crashed since its last successful post.
    ("crash_task", "TEXT"),
    ("crash_pc", "TEXT"),
    ("crash_bt", "TEXT"),
    ("crashes", "INTEGER"),
    ("image_pending", "INTEGER"),
)

# Same reconciliation, for the table the operator writes. device_flags shipped with two columns,
# gained armed_ts, then firmware_pull, then the two node_pull flags, so a database sitting at any
# of the earlier shapes needs the missing ones added rather than silently going without an expiry,
# without a way to say which kind of update was asked for, or without a way to reach the nodes at
# all. Every default restates what an older row already meant: 0 for a never-armed expiry, 0 for
# "this was a push request", which every arming written before that column existed was, and 0 for
# "no node update was asked for", which is true of every arming written before there were nodes
# to ask about.
_DEVICE_FLAGS_ADDED_COLS = (
    ("armed_ts", "INTEGER NOT NULL DEFAULT 0"),
    ("firmware_pull", "INTEGER NOT NULL DEFAULT 0"),
    ("node_pull_cam", "INTEGER NOT NULL DEFAULT 0"),
    ("node_pull_node", "INTEGER NOT NULL DEFAULT 0"),
)

# Every table with columns added after it was first written, so _migrate walks one list instead of
# growing a second copy of the same loop each time this happens again.
_ADDED_COLS = (
    ("telemetry", _TELEMETRY_ADDED_COLS),
    ("device_flags", _DEVICE_FLAGS_ADDED_COLS),
)

# Stored as JSON text and decoded on read, so a row can be handed to derive and
# to the model prompt as the object the device sent.
_JSON_COLS = ("actuators", "actuator_intent")

# SQLite has no boolean type. These come back out as 0/1 and are restored on
# read so a row can be JSON-dumped into a model prompt as `true` rather than
# something the model has to guess the meaning of.
_BOOL_COLS = (
    "node_online",
    "cam_online",
    "rgb_live",
    "thermal_live",
    "auto",
    "ask_now",
)

# One connection per thread, held in a threading.local.
#
# FastAPI runs sync handlers on anyio's worker threadpool, and scheduler.py runs
# on a thread of its own. A single shared connection would need a global lock
# around every statement, which serialises reads behind the scheduler's writes -
# the poll handler is on the critical path of a 2s cadence and must not queue
# behind a retention scan. WAL plus a connection per thread gives concurrent
# readers alongside one writer instead, and busy_timeout absorbs the
# SQLITE_BUSY a second writer would otherwise raise immediately.
#
# check_same_thread is left at its default on purpose: the invariant is that no
# connection ever crosses a thread, and the default is what enforces it. Worker
# threads that idle out drop the last reference to their connection, so these do
# not accumulate.
_local = threading.local()
_path_lock = threading.Lock()
_prune_lock = threading.Lock()
_db_path: Optional[Path] = None
_writes = 0


def db_path() -> Path:
    """Resolved once per process, on first use rather than at import.

    Reading the env var lazily means importing this module has no side effects -
    it neither creates a directory nor decides where the database lives before
    main.py has had a chance to load its config.
    """
    global _db_path
    with _path_lock:
        if _db_path is None:
            p = Path(os.environ.get("PLANTRX_DB") or "./data/plantrx.db").expanduser()
            p.parent.mkdir(parents=True, exist_ok=True)
            _db_path = p
        return _db_path


def _conn() -> sqlite3.Connection:
    conn: Optional[sqlite3.Connection] = getattr(_local, "conn", None)
    if conn is not None:
        return conn

    # isolation_level=None: autocommit. Every write here is a single statement,
    # so implicit transactions would only hold the write lock across the return
    # to FastAPI for no atomicity that anyone needs.
    conn = sqlite3.connect(str(db_path()), timeout=5.0, isolation_level=None)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    # WAL + NORMAL survives a process crash and risks only the last commits on a
    # power cut. An fsync per telemetry row at a 2s poll buys nothing: the next
    # poll supersedes it.
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA busy_timeout=5000")
    # Idempotent, and cheap enough to run per connection rather than guard with
    # a flag that would need its own race handling.
    conn.executescript(_SCHEMA)
    _migrate(conn)
    _local.conn = conn
    return conn


def _migrate(conn: sqlite3.Connection) -> None:
    """Add the columns _SCHEMA gained after a database was already on disk.

    One PRAGMA per connection, and an ALTER only for what is genuinely missing.
    A second process racing this loses its ALTER to a duplicate-column error,
    which is the one error here that means the column exists - so it is the only
    one swallowed.

    It only ever adds. `fw` was dropped from _SCHEMA once nothing read it, and
    the column stays behind on every database that already had it - unwritten,
    unread, and harmless because _row_to_dict keys off the column name and
    never off its position.
    """
    for table, cols in _ADDED_COLS:
        have = {r["name"] for r in conn.execute(f"PRAGMA table_info({table})")}
        for name, decl in cols:
            if name in have:
                continue
            try:
                conn.execute(f"ALTER TABLE {table} ADD COLUMN {name} {decl}")
            except sqlite3.OperationalError as exc:
                if "duplicate column" not in str(exc).lower():
                    raise
                continue
            log.info("%s: added column %s", table, name)


def init_db() -> None:
    """Open the database, create anything missing, drop anything expired.

    Idempotent and safe to call from a startup hook, from the scheduler, or not
    at all - every function here opens its own connection on demand.
    """
    _prune_old(_conn())


# --------------------------------------------------------------------------
# Telemetry
# --------------------------------------------------------------------------


def save_telemetry(t: Telemetry, recv_ts: int, derived: dict) -> None:
    """One row per poll. `derived` is derive.enrich(t).

    recv_ts is server receipt time and is the only clock this table orders by.
    uptime_ms is stored so that a later poll can be compared against it: a value
    below its predecessor is a restart, which is what separates "the device
    rebooted" from "the device went quiet". rx_id is stored for the same reading
    - which prescription each row was measured under. Neither settles it alone,
    and neither is a timestamp: the device's own clock reads 0 until NTP lands.
    """
    s = t.sensors
    lk = t.links
    # The device's own identification is stored as sent rather than as rendered,
    # so the history says what the wall was showing at that poll even after a
    # later prescription carries a different name forward.
    sp = t.species
    _conn().execute(
        f"INSERT INTO telemetry ({_TELEMETRY_COLS})"
        " VALUES (?,?,?, ?,?,?,?,?,?, ?,?, ?,?,?, ?,?,?,?,?, ?,?,?,?, ?,?,?, ?, ?,?,"
        " ?,?,?, ?,?,?)",
        (
            t.device,
            int(recv_ts),
            int(t.uptime_ms),
            s.co2_ppm,
            s.temp_c,
            s.rh_pct,
            s.lux,
            s.soil_pct,
            s.leaf_max_c,
            derived.get("vpd_kpa"),
            derived.get("leaf_air_dt_c"),
            int(lk.node_online),
            int(lk.node_age_ms),
            int(lk.node_lost),
            int(lk.cam_online),
            int(lk.rgb_live),
            int(lk.thermal_live),
            float(lk.thermal_fps),
            int(lk.wifi_rssi),
            json.dumps(t.actuators, separators=(",", ":")),
            int(t.auto),
            int(t.ask_now),
            t.rx_id,
            sp.sci if sp else None,
            sp.text if sp else None,
            int(sp.conf_pct) if sp else None,
            # Stored even though nothing acts on it yet: it is the only record of
            # what the grower had switched on at that poll, and window_summary
            # scores a prescription out of these rows long after the switch moved.
            json.dumps(t.actuator_intent, separators=(",", ":")),
            # Passed through as sent, None included: NULL here is a poll that
            # carried no count, which derive._count reads as unknown rather than
            # as a panel nobody touched. Coercing with int() would turn the one
            # value that means "cannot say" into the one that means "nothing".
            t.edges,
            t.allstops,
            t.boot.reset if t.boot else None,
            int(t.boot.crashes) if t.boot else None,
            (1 if t.boot.image_pending else 0) if t.boot else None,
            t.boot.crash_task if t.boot else None,
            t.boot.crash_pc if t.boot else None,
            t.boot.crash_bt if t.boot else None,
        ),
    )
    _maybe_prune()


def telemetry_since(
    device: str, since_ts: int, limit: Optional[int] = None
) -> list[dict]:
    """Rows at or after `since_ts`, oldest first - the order window_summary
    integrates in.

    `limit` keeps the *newest* n and then restores chronological order. A plain
    LIMIT would truncate the tail, which is the half that says what the
    prescription ended up achieving.
    """
    conn = _conn()
    if limit is None:
        rows = conn.execute(
            "SELECT * FROM telemetry WHERE device=? AND recv_ts>=?"
            " ORDER BY recv_ts, id",
            (device, int(since_ts)),
        ).fetchall()
    else:
        rows = conn.execute(
            "SELECT * FROM (SELECT * FROM telemetry WHERE device=? AND recv_ts>=?"
            " ORDER BY recv_ts DESC, id DESC LIMIT ?) ORDER BY recv_ts, id",
            (device, int(since_ts), int(limit)),
        ).fetchall()
    return [_row_to_dict(r) for r in rows]


def last_telemetry(device: str) -> Optional[dict]:
    row = _conn().execute(
        "SELECT * FROM telemetry WHERE device=? ORDER BY recv_ts DESC, id DESC LIMIT 1",
        (device,),
    ).fetchone()
    return _row_to_dict(row) if row is not None else None


def _row_to_dict(row: sqlite3.Row) -> dict:
    d = dict(row)
    for c in _BOOL_COLS:
        d[c] = bool(d[c])
    for c in _JSON_COLS:
        try:
            d[c] = json.loads(d[c] or "{}")
        except (TypeError, ValueError):
            d[c] = {}
    # The rowid is an implementation detail of this file. It would otherwise end
    # up in the model prompt as a number begging to be interpreted.
    d.pop("id", None)
    return d


# --------------------------------------------------------------------------
# Prescriptions
# --------------------------------------------------------------------------


def save_prescription(
    rx: Prescription, raw_model: Optional[dict] = None, device: str = ""
) -> None:
    """Store a prescription verbatim, plus the model output it came from.

    `device` is passed alongside because Prescription has no device field - it
    is addressed by the request it answers, not by its contents - and
    latest_prescription is per device.

    Omitting it is not survivable and must not be quiet about it: the row lands
    under an empty device, latest_prescription never matches it again, and the
    server silently forgets every prescription it has ever issued - `prev` stays
    None forever, so the window is never scored and the model is asked to
    diagnose from scratch on every wake. There is no safe fallback to guess from
    here (two greenhouses would read each other's prescriptions), so it warns
    and stores the row rather than raising and costing the device its answer.

    raw_model is the unvalidated JSON the model returned, kept because the
    interesting failures are the ones where a plausible prescription came out of
    a nonsense diagnosis. Written with ensure_ascii off so the Korean is
    readable in a sqlite3 shell.
    """
    if not device:
        log.warning(
            "save_prescription(%s) has no device: it will never be found again",
            rx.rx_id,
        )
    _conn().execute(
        "INSERT OR REPLACE INTO prescriptions (rx_id, device, issued_ts, rx_json, raw_json)"
        " VALUES (?,?,?,?,?)",
        (
            rx.rx_id,
            device,
            int(rx.issued_ts),
            rx.model_dump_json(),
            json.dumps(raw_model, ensure_ascii=False) if raw_model is not None else None,
        ),
    )
    _maybe_prune()


def latest_prescription(device: str) -> Optional[Prescription]:
    row = _conn().execute(
        "SELECT rx_json FROM prescriptions WHERE device=?"
        " ORDER BY issued_ts DESC, rowid DESC LIMIT 1",
        (device,),
    ).fetchone()
    if row is None:
        return None
    try:
        return Prescription.model_validate_json(row["rx_json"])
    except ValidationError:
        # A schema change strands old rows. Treating one as absent costs the
        # server its memory of the last prescription; raising would cost the
        # greenhouse every poll until someone noticed.
        return None


def prescription_by_id(device: str, rx_id: str) -> Optional[Prescription]:
    """The prescription the device says it is running, if this server issued it.

    Scoped to the device even though rx_id is the primary key: a panel echoing
    an id from another greenhouse has to read as an id we never issued to it,
    not as somebody else's bands to score its window against.
    """
    row = _conn().execute(
        "SELECT rx_json FROM prescriptions WHERE rx_id=? AND device=?",
        (rx_id, device),
    ).fetchone()
    if row is None:
        return None
    try:
        return Prescription.model_validate_json(row["rx_json"])
    except ValidationError:
        # Same trade as latest_prescription: a stranded row reads as absent.
        return None


def prescription_wake(rx_id: str) -> Optional[dict]:
    """The wake block the model asked for, as {"after_s": int, "when": [ ... ]}.

    Lives on the raw model output rather than on Prescription: when to think
    again is the server's business and the device never sees it, so putting it
    in the contract would ship the device a field it must ignore.

    Renamed on the way out because brain.py spells them wake_after_s/wake_when
    while scheduler.clamp_wake takes after_s/when. Both spellings are accepted
    reading back so rows written either way keep working - the same class of
    mismatch that quietly emptied every wake condition once already.

    None when the row is gone or was stored without raw output; a prescription
    replayed from a cold start then just gets the heartbeat ceiling.
    """
    row = _conn().execute(
        "SELECT raw_json FROM prescriptions WHERE rx_id=?", (rx_id,)
    ).fetchone()
    if row is None or not row["raw_json"]:
        return None
    try:
        raw = json.loads(row["raw_json"])
    except (TypeError, ValueError):
        return None
    if not isinstance(raw, dict):
        return None
    try:
        after_s = int(raw.get("wake_after_s", raw.get("after_s")) or 0)
    except (TypeError, ValueError):
        after_s = 0
    when = raw.get("wake_when", raw.get("when")) or []
    if not isinstance(when, list):
        when = []
    return {"after_s": after_s, "when": [w for w in when if isinstance(w, dict)]}


# --------------------------------------------------------------------------
# Frames
# --------------------------------------------------------------------------


def save_frame(device: str, kind: str, blob: bytes, ts: int) -> None:
    """Keep the newest FRAME_KEEP frames per device and kind.

    Pruned on every insert rather than on the write counter: these are the rows
    with real bytes in them, and there is at most one pair per poll, so the
    delete is not worth deferring.
    """
    conn = _conn()
    conn.execute(
        "INSERT INTO frames (device, kind, ts, blob) VALUES (?,?,?,?)",
        (device, kind, int(ts), sqlite3.Binary(blob)),
    )
    conn.execute(
        "DELETE FROM frames WHERE device=? AND kind=? AND id NOT IN"
        " (SELECT id FROM frames WHERE device=? AND kind=? ORDER BY id DESC LIMIT ?)",
        (device, kind, device, kind, FRAME_KEEP),
    )


def latest_frame(device: str, kind: str) -> Optional[bytes]:
    row = _conn().execute(
        "SELECT blob FROM frames WHERE device=? AND kind=? ORDER BY id DESC LIMIT 1",
        (device, kind),
    ).fetchone()
    return bytes(row["blob"]) if row is not None else None


def frame_ts(device: str, kind: str) -> Optional[int]:
    """Capture time of what latest_frame would return.

    Newest is by rowid, not by ts, here and in the prune: the device clock reads
    0 until NTP lands, so a frame uploaded during boot claims 1970 and would
    otherwise be the first thing pruned - or, ordered the other way, would
    outrank every frame that followed it.
    """
    row = _conn().execute(
        "SELECT ts FROM frames WHERE device=? AND kind=? ORDER BY id DESC LIMIT 1",
        (device, kind),
    ).fetchone()
    return int(row["ts"]) if row is not None else None


# --------------------------------------------------------------------------
# LLM budget
# --------------------------------------------------------------------------


def record_llm_call(device: str, ts: int) -> None:
    """One row per call *attempted*, not per call that worked.

    main._run_brain writes this before it awaits the model, because the budget
    exists to bound what the model path costs and a request that times out or
    comes back unparseable has already cost it. Recording only successes made
    both gates in scheduler.decide blind in exactly the state they were written
    for: calls_today stayed at 0 for as long as every call failed.
    """
    _conn().execute(
        "INSERT INTO llm_calls (device, ts) VALUES (?,?)", (device, int(ts))
    )


def llm_calls_since(device: str, since_ts: int) -> int:
    row = _conn().execute(
        "SELECT COUNT(*) AS n FROM llm_calls WHERE device=? AND ts>=?",
        (device, int(since_ts)),
    ).fetchone()
    return int(row["n"]) if row is not None else 0


def last_llm_call_ts(device: str) -> Optional[int]:
    """When this device last spent a model call, or None if it never has.

    scheduler.decide measures its floor from here rather than from the last
    prescription's issued_ts. A device that has never been diagnosed has no
    issued_ts, so a floor measured from one is vacuous on the only device that
    polls the model path forever.
    """
    row = _conn().execute(
        "SELECT ts FROM llm_calls WHERE device=? ORDER BY id DESC LIMIT 1",
        (device,),
    ).fetchone()
    return int(row["ts"]) if row is not None else None


# --------------------------------------------------------------------------
# One-shot signals to the device
# --------------------------------------------------------------------------


# How long an arming stays actionable. Fifteen minutes: the device polls about once a minute, so
# a live request is picked up almost immediately, and the rest is room for an operator who armed
# it and then went to find the firmware. Beyond that the arming is somebody's forgotten intent,
# and acting on it takes a working panel away for five minutes for no reason anyone remembers.
UPDATE_MODE_TTL_S = 15 * 60


def set_update_mode(device: str, pull: bool = False) -> None:
    """Arm "enter update mode" for the next poll this device makes.

    `pull` picks which kind. False is the original arrangement: stand the panel down and wait
    for somebody to espota an image at it. True adds "and then go and fetch the image yourself",
    which is the whole point of the pull path - the panel in the greenhouse can update with
    nobody holding a laptop.

    Nothing here reaches out to the device. It is behind NAT wherever it is plugged in and the
    server has never had a socket to it, so the only way to say anything is to leave it where
    the next telemetry response will pick it up.

    On disk rather than in a dict beside main._last_event for the same reason: a redeploy
    between the POST and the next poll would swallow the request silently, and the only symptom
    the operator would ever see is a panel that never took the update.

    Re-arming overwrites `pull` rather than accumulating it, and that is the safe direction:
    the newest request is the one somebody actually means, so a plain arm placed after a pull
    arm cancels the pull instead of quietly keeping it. The opposite rule would have a stray
    POST send a panel off to download 2.5MB that nobody asked for.
    """
    _conn().execute(
        "INSERT INTO device_flags (device, update_mode, armed_ts, firmware_pull) VALUES (?,1,?,?)"
        " ON CONFLICT(device) DO UPDATE SET update_mode=1, armed_ts=excluded.armed_ts,"
        " firmware_pull=excluded.firmware_pull",
        (device, int(time.time()), 1 if pull else 0),
    )


def take_update_mode(device: str) -> tuple[bool, bool]:
    """(armed, pull) once per arming, (False, False) every time after it.

    Clearing is the whole point. Update mode has no exit except a restart, so the device that
    obeys this flag is back and polling within minutes; a flag that survived delivery would take
    the panel over again on that first poll back, and again after the reboot that ends that one.
    A one-shot signal turns into a permanently bricked panel the moment it stops being one-shot.
    That applies twice over to the pull half: a firmware_pull that outlived its delivery would
    have the panel reinstall the same image on every poll, forever.

    Read and cleared in a single UPDATE rather than a SELECT followed by one, because two polls
    arriving together is ordinary - the device retries whenever a response is lost - and the two
    statements would hand the same arming to both. One statement under autocommit cannot be
    interleaved, and the row it returns is the server's answer to which caller won it.

    That one statement clears update_mode and only update_mode, and the pull bit is tidied up
    afterwards, which looks like a loose end and is not. SQLite's RETURNING hands back the row
    as it is AFTER the update, so a statement that cleared firmware_pull in the same SET would
    return the zero it had just written and every pull would arrive at the device as a push -
    measured, not reasoned about. Leaving the column alone is what makes the value readable.
    It is still consumed exactly once, because it is only ever reached through update_mode: no
    caller reads firmware_pull on its own, and the arming that gates it is gone by the time the
    next poll asks. The second write exists so a human running `select * from device_flags`
    against the volume is not shown an arming state that no longer exists, and it is guarded on
    update_mode=0 so that an arming which arrived in the microsecond between the two statements
    is left as its poster wrote it rather than quietly downgraded to a push.

    Consumed at-most-once, and that direction is chosen: a response lost on the way back costs
    one more POST from whoever wanted the update, while a flag that outlived its delivery costs
    a panel that nobody can use.

    Armings expire. "Enter update mode" means "I am about to upload", and a panel that was
    unplugged for a week must not act on it the moment it comes back: the takeover would land
    minutes before anybody was watching, for a request nobody remembers. RETURNING lets the
    consuming statement stay one statement - the row is cleared whatever its age, so a stale
    arming does not sit armed forever, and the age decides only whether it is acted on.
    """
    conn = _conn()
    cur = conn.execute(
        "UPDATE device_flags SET update_mode=0 WHERE device=? AND update_mode=1"
        " RETURNING armed_ts, firmware_pull",
        (device,),
    )
    row = cur.fetchone()
    if row is None:
        return False, False
    conn.execute(
        "UPDATE device_flags SET firmware_pull=0 WHERE device=? AND update_mode=0",
        (device,),
    )

    age = int(time.time()) - int(row["armed_ts"])
    if age > UPDATE_MODE_TTL_S:
        log.info("%s: update_mode arming expired (%ds old), dropped", device, age)
        return False, False
    return True, bool(row["firmware_pull"])


# Which node roles a panel can be told to update. "panel" is deliberately absent: the panel
# updates itself through update_mode/firmware_pull and fwpull.cpp, and a panel arming a node
# update against itself would be a device asked to watch its own reboot.
NODE_PULL_COLS = {"cam": "node_pull_cam", "node": "node_pull_node"}


def set_node_pull(device: str, role: str) -> None:
    """Arm "tell node `role` to update" on this panel's next poll.

    The panel is the only thing on this server's side of the greenhouse that can reach a node -
    the nodes speak ESP-NOW and have no route to here at all - so an operator arming a node
    update is really arming a message the panel will pass on. Same rail as set_update_mode for
    that reason and no other: write a flag, let the next poll carry it, never reach out.

    Re-arming the same role is idempotent rather than cumulative, and arming the other role does
    not disturb this one - the two are separate columns because "update both boards" is two
    POSTs and the second must not cancel the first.

    Deliberately does NOT touch update_mode. A node update leaves the panel running: it is the
    board that relays the command, receives the node's progress reports over ESP-NOW and draws
    them, so standing it down would blind the operator to the update they just asked for. See
    take_node_pull for what main.py does when both are armed at once.
    """
    col = NODE_PULL_COLS[role]
    _conn().execute(
        f"INSERT INTO device_flags (device, {col}) VALUES (?,1)"
        f" ON CONFLICT(device) DO UPDATE SET {col}=1",
        (device,),
    )


def take_node_pull(device: str) -> tuple[bool, bool]:
    """(cam, node) once per arming, (False, False) every time after it.

    One UPDATE per role, each its own one-shot: the WHERE names the value it is about to
    overwrite, so exactly one caller can see rowcount 1 and every caller after it sees 0. Two
    polls arriving together is ordinary - the panel retries whenever a response is lost - and
    this is what stops both of them being handed the same arming.

    Written as rowcount rather than with RETURNING because RETURNING hands back the row as it is
    AFTER the update, so a statement that cleared the column and returned it would return the
    zero it had just written. take_update_mode above works around that by clearing one column and
    returning another; here there is no second column to lean on, and "did this statement change
    a row" is the whole question anyway.

    The clearing is load-bearing in a way the panel's own flags are not, and this is the note to
    read before changing anything here. update_mode takes the panel over: main.cpp stops polling,
    so a flag that failed to clear could only fire once more, after the reboot. A node pull leaves
    the panel polling on its normal cadence, so a flag that failed to clear would arm the node's
    update again on every poll for as long as the greenhouse runs - a camera that reboots itself
    every sixty seconds and no obvious reason why.

    No expiry, unlike take_update_mode, and the difference is the cost of acting late. A stale
    update_mode takes the only screen in the greenhouse into a five-minute takeover with no exit
    but a reboot. A stale node pull tells a node to check for an image; if it is already running
    that image it says so and stops (it compares elf_sha256 before downloading anything), and if
    it is not, it installs the image an operator published and asked for. The worst case is a
    camera off the wall for the length of a download, which is the same thing the operator was
    asking for, just later - not worth a column and a second TTL to prevent.
    """
    conn = _conn()
    cam = conn.execute(
        "UPDATE device_flags SET node_pull_cam=0 WHERE device=? AND node_pull_cam=1",
        (device,),
    ).rowcount == 1
    node = conn.execute(
        "UPDATE device_flags SET node_pull_node=0 WHERE device=? AND node_pull_node=1",
        (device,),
    ).rowcount == 1
    return cam, node


# --------------------------------------------------------------------------
# Node logs
# --------------------------------------------------------------------------


def save_node_logs(device: str, lines: list[NodeLogLine], recv_ts: int) -> int:
    """Store one POSTed batch and say how many rows it became.

    One recv_ts for the whole batch rather than one per row, because that is what is true: the
    panel buffers lines it heard over ESP-NOW and posts them together, so the server learned all
    of them at the same instant and stamping each row with its own `time.time()` would invent a
    resolution the transport does not have. Ordering inside a batch is the insert order, which
    the rowid preserves.

    executemany rather than a loop of execute: the connection is in autocommit, so a loop would
    take and release the write lock once per line, and a verbose node's batch is tens of lines.
    """
    rows = [(device, ln.role, int(ln.ms), int(recv_ts), ln.text) for ln in lines]
    if not rows:
        return 0
    _conn().executemany(
        "INSERT INTO node_logs (device, role, ms, recv_ts, text) VALUES (?,?,?,?,?)", rows
    )
    _maybe_prune()
    return len(rows)


def recent_node_logs(role: Optional[str] = None, limit: int = NODE_LOG_READ_DEFAULT) -> list[dict]:
    """The newest rows, newest first, optionally for one role.

    Newest first rather than chronological, unlike telemetry_since: nobody integrates these, they
    are read by a human who wants to know what a node said most recently, and a reader that has
    to scroll to the bottom to find it is a reader that will paste a limit of 5000 instead.

    `role` is matched literally and is NOT checked against the three known names, which is the
    opposite of what the firmware endpoints do with the same word - deliberately. There, an
    unknown role must be refused because serving the wrong image bricks a board. Here it is a
    filter over rows that already exist, an unknown one matches nothing, and refusing it would
    make the one role that matters unqueryable: nodeproto_role_name() answers "?" for a role byte
    it does not recognise, so "?" is exactly what a version-skewed node's lines are filed under
    and exactly what somebody debugging that skew needs to ask for.

    The limit is clamped here rather than at the route, so no caller can ask for the whole table.
    """
    n = max(1, min(int(limit), NODE_LOG_READ_MAX))
    conn = _conn()
    if role is None:
        rows = conn.execute(
            "SELECT device, role, ms, recv_ts, text FROM node_logs ORDER BY id DESC LIMIT ?",
            (n,),
        ).fetchall()
    else:
        rows = conn.execute(
            "SELECT device, role, ms, recv_ts, text FROM node_logs WHERE role=?"
            " ORDER BY id DESC LIMIT ?",
            (role, n),
        ).fetchall()
    return [dict(r) for r in rows]


# --------------------------------------------------------------------------
# Retention
# --------------------------------------------------------------------------


def _maybe_prune() -> None:
    global _writes
    with _prune_lock:
        _writes += 1
        due = _writes % PRUNE_EVERY == 0
    if due:
        _prune_old(_conn())


def _prune_old(conn: sqlite3.Connection) -> None:
    cutoff = int(time.time()) - TELEMETRY_TTL_S
    conn.execute("DELETE FROM telemetry WHERE recv_ts < ?", (cutoff,))
    # llm_calls are counted over a day at most, so the same cutoff is generous.
    # Deliberately no VACUUM: it takes an exclusive lock and the freed pages get
    # reused by the next fortnight of telemetry anyway.
    conn.execute("DELETE FROM llm_calls WHERE ts < ?", (cutoff,))
    # Node logs on their own, much shorter, cutoff - see NODE_LOG_TTL_S. Pruned here rather than
    # in save_node_logs so that a node which went quiet after filling the table still has its
    # rows expire: this runs off the shared write counter, which every other table's inserts
    # turn as well.
    conn.execute("DELETE FROM node_logs WHERE recv_ts < ?",
                 (int(time.time()) - NODE_LOG_TTL_S,))


def stats() -> dict[str, Any]:
    """Row counts and disk footprint, for a health endpoint or a look around."""
    conn = _conn()
    out: dict[str, Any] = {"path": str(db_path())}
    for table in ("telemetry", "prescriptions", "frames", "llm_calls", "device_flags",
                  "node_logs"):
        out[table] = int(conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
    # The -wal sidecar holds everything written since the last checkpoint, so
    # the main file alone reads as 4KB on a busy database and makes the disk
    # look empty right when it is filling up.
    total = 0
    base = db_path()
    for p in (base, base.with_name(base.name + "-wal"), base.with_name(base.name + "-shm")):
        try:
            total += p.stat().st_size
        except OSError:
            pass
    out["bytes"] = total
    return out
