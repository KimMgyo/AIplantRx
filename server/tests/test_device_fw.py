"""What three boards are running, as the panel reports it and the server keeps it.

    cd server && python tests/test_device_fw.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py gives: the
image installs requirements.txt and a runner in there would ship to production for nothing.
pytest collects this file unchanged.

WHY THIS TABLE IS WORTH TESTING. It is the left-hand side of the only comparison the operator
page makes - this board is running X, the release published Y, so the button either matters or
does not. Every way of getting that wrong is silent: a row per poll instead of a row per board
looks fine until a fortnight of history is twenty thousand identical hashes; a role dropped
because one poll omitted it reads as a board that vanished; and an operator view that CONSUMES an
arming instead of peeking at it hands the flag to a web page, which cannot reboot into update
mode, cannot relay an ESP-NOW command, and cannot do anything with it but draw it - while the
device that was armed thirty seconds ago polls and finds nothing waiting.

So the assertions below are about the shape of what is kept:

  A. One row per (device, role), overwritten in place. The primary key IS the design.
  B. A role the panel did not mention this poll is retained, not deleted - "all three" and
     "the two I have heard from" are the same panel a minute apart.
  C. `fw` absent is not an error. It is what every build before this field posts, and the
     compatibility path has to be the quiet one.
  D. peek_device_flags does not clear what take_update_mode would.
  E. known_devices lists a board that reported firmware and wrote no telemetry row, because
     save_telemetry is gated on the sensor sanity check and the board an operator is most
     likely hunting for is exactly the one whose sensors stopped.
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "devicefw.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is another file's test, not this fixture

from fastapi.testclient import TestClient  # noqa: E402

from app import main, store  # noqa: E402
from app.schema import FirmwareState  # noqa: E402

CLIENT = TestClient(main.app)

PANEL = "58:E6:C5:71:97:30"
OTHER = "AA:BB:CC:DD:EE:FF"

# Sixteen hex digits, the width a node can actually say: NodeRepMsg carries elf_sha[8]
# (shared/nodeproto.h), so this is what the panel sends for all three boards.
ELF_PANEL = "40b1fe11a645ba73"
ELF_CAM = "aeb7bc805ad48b2e"
ELF_NODE = "2390f2194a89c9f4"

T0 = int(time.time())


def _clear() -> None:
    store._conn().execute("DELETE FROM device_fw")
    store._conn().execute("DELETE FROM device_flags")
    store._conn().execute("DELETE FROM telemetry")


def _fw(elf: str, up_s: int = 100, heap: int = 128000, online: bool = True,
        pending: bool = False, can_ota: bool = True) -> dict:
    return {"elf": elf, "up_s": up_s, "heap": heap, "online": online,
            "pending": pending, "can_ota": can_ota}


def _poll(device: str = PANEL, fw: dict | None = None, sane: bool = True) -> dict:
    """One telemetry POST, with or without the fw block.

    The sensors are real-looking on purpose: save_telemetry is gated on
    scheduler.telemetry_is_sane, and a poll of zeros would take the untested path through the one
    assertion below that is specifically about a device with no telemetry row.
    """
    body: dict = {"device": device, "uptime_ms": 60000}
    if sane:
        body["sensors"] = {"co2_ppm": 900, "temp_c": 24.0, "rh_pct": 55.0}
    if fw is not None:
        body["images"] = fw
    r = CLIENT.post("/v1/telemetry", json=body)
    assert r.status_code == 200, (r.status_code, r.text[:200])
    return r.json()


def _rows(device: str = PANEL) -> dict:
    return {r["role"]: r for r in store.all_device_fw() if r["device"] == device}


def test_a_poll_stores_one_row_per_role():
    """The happy path, read back through the store rather than the response.

    The response is a Prescription and says nothing about this; the row is the whole point, so
    the row is what is asserted.
    """
    _clear()
    _poll(fw={"panel": _fw(ELF_PANEL), "cam": _fw(ELF_CAM), "node": _fw(ELF_NODE)})
    rows = _rows()
    assert sorted(rows) == ["cam", "node", "panel"], sorted(rows)
    assert rows["panel"]["elf"] == ELF_PANEL, rows["panel"]
    assert rows["cam"]["elf"] == ELF_CAM, rows["cam"]
    assert rows["node"]["elf"] == ELF_NODE, rows["node"]
    assert rows["panel"]["can_ota"] is True, rows["panel"]
    return "three roles, three rows, each carrying its own hash"


def test_a_second_poll_overwrites_rather_than_accumulates():
    """A: the primary key is the design, so this is the assertion that defends it.

    A panel polls every minute. Without the upsert, a fortnight is twenty thousand rows per board
    all saying the same sixteen hex digits, and the operator page's "what is it running" becomes a
    question about which row to believe.
    """
    _clear()
    _poll(fw={"panel": _fw(ELF_PANEL, up_s=100)})
    _poll(fw={"panel": _fw(ELF_NODE, up_s=200, pending=True)})
    rows = store.all_device_fw()
    assert len(rows) == 1, rows
    assert rows[0]["elf"] == ELF_NODE, rows[0]
    assert rows[0]["up_s"] == 200, rows[0]
    assert rows[0]["pending"] is True, rows[0]
    return "the second report replaces the first in place"


def test_a_role_left_out_of_one_poll_is_kept():
    """B. "All three" and "the two I have heard from" are one panel a minute apart.

    nodeota's view of a node goes unknown when it has been quiet long enough, and the panel omits
    a board it cannot speak for. Deleting the row on that poll would draw the sensor node as
    having never existed, one tick after it was on the screen.
    """
    _clear()
    _poll(fw={"panel": _fw(ELF_PANEL), "cam": _fw(ELF_CAM), "node": _fw(ELF_NODE)})
    _poll(fw={"panel": _fw(ELF_PANEL, up_s=999)})
    rows = _rows()
    assert sorted(rows) == ["cam", "node", "panel"], sorted(rows)
    assert rows["panel"]["up_s"] == 999, rows["panel"]
    assert rows["node"]["elf"] == ELF_NODE, "the omitted role was dropped"
    return "an omitted role keeps its last known answer"


def test_a_poll_without_fw_stores_nothing_and_is_not_an_error():
    """C: the compatibility path, and it has to be the quiet one.

    Every build before this field posts exactly this body. A 422 here would take a working
    greenhouse off the air on the strength of a field nobody's firmware knew to send.
    """
    _clear()
    _poll()
    assert store.all_device_fw() == [], store.all_device_fw()
    # And a later poll that DOES carry it still lands, so the upgrade needs no reset.
    _poll(fw={"panel": _fw(ELF_PANEL)})
    assert [r["elf"] for r in store.all_device_fw()] == [ELF_PANEL], store.all_device_fw()
    return "fw absent is accepted, stores nothing, and does not poison the next poll"


def test_the_old_fw_string_is_still_accepted_and_ignored():
    """The name this field could NOT have, asserted from the wire side.

    An earlier firmware posted a version string at "fw", it was dropped from the contract once
    nothing read it, and tests/test_store_species.py pins the promise that a board which has not
    been reflashed keeps sending it and keeps being served. The new per-board block was written
    against "fw" first, and the suite caught it here: pydantic 422s a str where a dict is
    declared, so every un-reflashed panel would have lost its prescription over a field this
    server never asked it for - the greenhouse's uplink, traded for an operator page.

    Both keys at once, because the interesting case is the middle of the rollout: a panel with the
    new firmware and a server that still has to tolerate the old one.
    """
    _clear()
    r = CLIENT.post("/v1/telemetry", json={
        "device": PANEL, "uptime_ms": 60000,
        "sensors": {"co2_ppm": 900, "temp_c": 24.0, "rh_pct": 55.0},
        "fw": "smartfarm-s3/1.0",
        "images": {"panel": _fw(ELF_PANEL)},
    })
    assert r.status_code == 200, (r.status_code, r.text[:200])
    assert [x["elf"] for x in store.all_device_fw()] == [ELF_PANEL], store.all_device_fw()

    # And the legacy string alone, which is what a board that was never reflashed posts.
    _clear()
    old = CLIENT.post("/v1/telemetry", json={
        "device": PANEL, "uptime_ms": 60000,
        "sensors": {"co2_ppm": 900, "temp_c": 24.0, "rh_pct": 55.0},
        "fw": "smartfarm-s3/1.0",
    })
    assert old.status_code == 200, (old.status_code, old.text[:200])
    assert store.all_device_fw() == [], store.all_device_fw()
    return "the legacy fw string is accepted and ignored, alongside or without images"


def test_peeking_at_the_flags_does_not_consume_them():
    """D: the bug that would make the operator page eat its own requests.

    Asserted against take_update_mode in the same test, because the property is a difference
    between the two functions and reading either one alone cannot show it.
    """
    _clear()
    store.set_update_mode(PANEL, pull=True)
    store.set_node_pull(PANEL, "cam")

    for _ in range(3):
        flags = store.peek_device_flags(PANEL)
        assert flags["update_mode"] is True, flags
        assert flags["firmware_pull"] is True, flags
        assert flags["node_pull_cam"] is True, flags
        assert flags["node_pull_node"] is False, flags

    # The device's own poll is the thing that consumes it, and after that the page sees it gone.
    armed, pull = store.take_update_mode(PANEL)
    assert (armed, pull) == (True, True), (armed, pull)
    assert store.peek_device_flags(PANEL)["update_mode"] is False, "the take did not clear"
    cam, node = store.take_node_pull(PANEL)
    assert (cam, node) == (True, False), (cam, node)
    assert store.peek_device_flags(PANEL)["node_pull_cam"] is False, "the take did not clear"
    return "three peeks leave the arming standing; one take clears it"


def test_a_device_with_no_telemetry_row_is_still_listed():
    """E: the board an operator is most likely hunting for.

    save_telemetry is gated on scheduler.telemetry_is_sane, so a panel whose sensor node is dead
    answers its poll, reports its firmware, and writes no telemetry row. Listing devices from
    `telemetry` alone would hide exactly that board - and the update button somebody wants to
    press on it.
    """
    _clear()
    _poll(device=OTHER, fw={"panel": _fw(ELF_PANEL)}, sane=False)
    listed = [d["device"] for d in store.known_devices()]
    assert OTHER in listed, listed
    row = [d for d in store.known_devices() if d["device"] == OTHER][0]
    assert row["uptime_ms"] == 0, "uptime came from somewhere other than a telemetry row"
    return "a firmware report alone is enough to be listed, with uptime 0"


def test_the_newest_report_orders_the_list():
    """all_device_fw answers freshest first, which is what the page's ordering is built on."""
    _clear()
    _poll(device=OTHER, fw={"panel": _fw(ELF_CAM)})
    time.sleep(1.1)   # recv_ts is whole seconds; two rows inside one second cannot be ordered
    _poll(device=PANEL, fw={"panel": _fw(ELF_PANEL)})
    devices = [r["device"] for r in store.all_device_fw()]
    assert devices[0] == PANEL, devices
    return "all_device_fw is newest-recv_ts first"


if __name__ == "__main__":
    for fn in (test_a_poll_stores_one_row_per_role,
               test_a_second_poll_overwrites_rather_than_accumulates,
               test_a_role_left_out_of_one_poll_is_kept,
               test_a_poll_without_fw_stores_nothing_and_is_not_an_error,
               test_the_old_fw_string_is_still_accepted_and_ignored,
               test_peeking_at_the_flags_does_not_consume_them,
               test_a_device_with_no_telemetry_row_is_still_listed,
               test_the_newest_report_orders_the_list):
        print("%-56s %s" % (fn.__name__, fn()))
    print("OK")
