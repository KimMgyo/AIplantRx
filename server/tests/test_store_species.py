"""The telemetry table's species columns, against a database that predates them.

    cd server && python tests/test_store_species.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives.

This file exists because the only database that matters is one that already
exists. `CREATE TABLE IF NOT EXISTS` is a no-op on a table that is already there,
so adding columns to store._SCHEMA alone produces code that works perfectly on
every fresh file and fails on every dev box and on production - and fails at the
INSERT, one poll at a time, in a handler that is designed never to 500. So the
old shape is built here by hand rather than mocked: the table is created from
the column list as it stood before the species columns, and the migration is
made to walk the same upgrade a running install walks.
A column that *left* _SCHEMA is the same problem read backwards, and is checked
here for the same reason.
"""
import os
import sqlite3
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "old-shape.db")

from app import derive, store  # noqa: E402
from app.schema import DeviceSpecies, Links, Sensors, Telemetry  # noqa: E402

# Wall clock, not a fixed timestamp: init_db() prunes on open, so a row dated
# 2025 would be deleted by retention before the round trip could read it back
# and the file would be testing the pruner instead of the migration.
T0 = int(time.time())

# store._SCHEMA's telemetry table as it stood before the species columns. Spelled
# out rather than derived from the current _SCHEMA, because a migration test that
# builds its "old" database out of the new definition tests nothing at all.
_OLD_TABLE = """
CREATE TABLE telemetry (
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
    fw            TEXT
);
"""

_ADDED = tuple(name for name, _decl in store._TELEMETRY_ADDED_COLS)

# Split by declaration, not by hand: a NOT NULL column with a default cannot read
# back as NULL, so asserting "absent means NULL" over the whole list would make
# every future non-null column a false failure. The two halves carry different
# contracts and are checked separately below.
_ADDED_NULLABLE = tuple(
    name for name, decl in store._TELEMETRY_ADDED_COLS if "NOT NULL" not in decl
)
_ADDED_NOT_NULL = tuple(
    name for name, decl in store._TELEMETRY_ADDED_COLS if "NOT NULL" in decl
)


def _columns(path):
    conn = sqlite3.connect(path)
    try:
        return {r[1] for r in conn.execute("PRAGMA table_info(telemetry)")}
    finally:
        conn.close()


def _telemetry(device, species=None):
    return Telemetry(
        device=device,
        uptime_ms=60000,
        sensors=Sensors(temp_c=25.2, rh_pct=58.0, co2_ppm=800.0),
        links=Links(node_online=True, cam_online=True, rgb_live=True),
        actuators={"mist": 0, "fan": 40},
        species=species,
    )


def _build_old_database():
    """A pre-species database with a row already in it.

    The row matters: a migration that quietly recreated the table would pass
    every column check and lose two weeks of history.
    """
    path = str(store.db_path())
    conn = sqlite3.connect(path)
    try:
        conn.executescript(_OLD_TABLE)
        conn.execute(
            "INSERT INTO telemetry (device, recv_ts, temp_c) VALUES (?,?,?)",
            ("OLD:ROW", T0 - 60, 21.5),
        )
        conn.commit()
    finally:
        conn.close()
    return path


def test_migration_adds_the_columns_to_an_existing_table():
    path = _build_old_database()
    before = _columns(path)
    assert "temp_c" in before, before
    for name in _ADDED:
        assert name not in before, "%s: the old shape already had it; nothing is being tested" % name

    store.init_db()  # the upgrade a running install walks on its next connect

    after = _columns(path)
    for name in _ADDED:
        assert name in after, "%s was not added to an existing telemetry table" % name
    assert before <= after, "the migration dropped columns: %s" % sorted(before - after)

    kept = store.telemetry_since("OLD:ROW", 0)
    assert len(kept) == 1, kept
    assert kept[0]["temp_c"] == 21.5, kept[0]
    assert kept[0]["species_sci"] is None, kept[0]
    return sorted(after - before)


def test_reported_species_round_trips():
    """What the wall was showing at that poll, readable back out of the row."""
    t = _telemetry("AA:BB:CC:DD:EE:01",
                   DeviceSpecies(sci="Ajuga genevensis", text="아주가", conf_pct=87))
    store.save_telemetry(t, T0, derive.enrich(t))

    row = store.last_telemetry(t.device)
    assert row is not None, "the insert did not land"
    assert row["species_sci"] == "Ajuga genevensis", row
    assert row["species_text"] == "아주가", row
    assert row["species_conf_pct"] == 87, row
    return row


def test_silent_device_stores_nulls():
    """No species reported is stored as absent, not as an empty name.

    "" and NULL read the same on a card and differently in a query, and the
    question this column exists to answer is which polls the panel had a name
    for. The switch-position column answers a different question and so has a
    different empty: it is NOT NULL, and "the panel declared no switches" is the
    empty object, which is what derive already reads as nothing reported.
    """
    t = _telemetry("AA:BB:CC:DD:EE:02")
    store.save_telemetry(t, T0 + 1, derive.enrich(t))

    row = store.last_telemetry(t.device)
    assert row is not None, "the insert did not land"
    for name in _ADDED_NULLABLE:
        assert row[name] is None, (name, row[name])
    for name in _ADDED_NOT_NULL:
        assert row[name] is not None, (
            "%s is NOT NULL and must never read back as absent" % name
        )
        assert not row[name], (name, row[name])
    return row


def test_a_column_that_left_the_schema_still_takes_an_insert():
    """fw was dropped from store._SCHEMA once nothing read it, and _migrate only
    ever adds - so every database built before the removal still carries the
    column while the INSERT has stopped naming it.

    Had fw been declared NOT NULL, this is where every poll from every device
    would have begun failing: inside the one handler designed never to 500, on
    dev boxes and production only, and never on a fresh file.

    The wire half is the same removal from the other side. A board that has not
    been reflashed still sends fw, and it has to go on being accepted and
    ignored - rejecting it would cost the greenhouse its prescription over a
    field the server no longer wants.
    """
    assert "fw" in _columns(str(store.db_path())), (
        "this database never had fw; the tolerance is not being tested")

    t = Telemetry.model_validate({
        "device": "AA:BB:CC:DD:EE:03",
        "uptime_ms": 60000,
        "sensors": {"temp_c": 24.0, "rh_pct": 51.0},
        "fw": "smartfarm-s3/1.0",
    })
    assert not hasattr(t, "fw"), "fw is back on the contract"
    store.save_telemetry(t, T0 + 2, derive.enrich(t))

    row = store.last_telemetry(t.device)
    assert row is not None, "the insert did not land"
    assert row["fw"] is None, "something is still writing fw: %r" % row["fw"]
    return row


def test_migration_is_idempotent():
    """Every connection runs it, and a second one must not raise."""
    store._migrate(store._conn())
    store._migrate(store._conn())
    row = store.last_telemetry("AA:BB:CC:DD:EE:01")
    assert row["species_text"] == "아주가", row


if __name__ == "__main__":
    added = test_migration_adds_the_columns_to_an_existing_table()
    named = test_reported_species_round_trips()
    silent = test_silent_device_stores_nulls()
    dropped = test_a_column_that_left_the_schema_still_takes_an_insert()
    test_migration_is_idempotent()
    print("db     %s" % store.db_path())
    print("added  %s to a table that already held a row" % added)
    print("named  %s / %s / %s%%" % (named["species_sci"], named["species_text"],
                                     named["species_conf_pct"]))
    print("silent %s" % {k: silent[k] for k in _ADDED})
    print("fw     accepted on the wire, %r in a table that still has it"
          % dropped["fw"])
    print("OK")
