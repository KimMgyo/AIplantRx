"""Why the panel rebooted, as the operator view actually serves it.

    cd server && python tests/test_crash_visible.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py gives.
pytest collects this file unchanged.

WHY THIS FILE EXISTS. Everything in it was already being written to disk and read by nobody.
The device has been posting its reset reason, a cumulative crash count and the panic handler's own
record of the last crash - task, exception PC, backtrace - for releases; store.save_telemetry has
been storing all six columns; no query ever selected them back out. Three counted crashes went by
on three firmwares (a panic, a task watchdog, an interrupt watchdog) and taught nobody anything,
because the one diagnostic that names a crash was invisible unless somebody opened SQLite by hand.
A field that is stored and never read is not a feature, and these asserts are the difference.

The fourth case is the one that cost the most to find by hand. `crashes` deliberately excludes
power-ons - src/health.cpp defends that at length, and it is right, because a counter that ticked
on our own OTA reboots would answer a different question than the one it claims to. The
consequence is that five ESP_RST_POWERON resets in ten minutes moved no number on this page at
all, and the pattern was recovered only by paging 1,916 rows of retained console. So the device
now keeps a restart ring in NVS and this server accumulates it, and the count and the ring are
kept as two records because they answer two questions.

  A. A device that posted a crash record has it in /v1/admin/state, with the receipt time of the
     poll that carried it.
  B. A device that posted none reports the ABSENCE. Not a clean bill of health: the device sends
     its record only until a post carrying it is answered and then erases it, and a watchdog
     reset may leave no dump to send at all, so "no record" means "nothing new reported".
  C. The reset reason and the crash count survive independently of the dump fields, because those
     two are present on every poll and the dump fields are present on one. Reading the panic
     record off the newest row would show it for a single five-second refresh and never again.
  D. Five power resets in ten minutes are readable as a fact - reasons, order, and the spread of
     the times - and `crashes` does not move across any of them.
  E. A re-sent ring does not re-date a boot, because the device re-sends its whole ring every
     poll and first_seen is a claim about the first one.
  F. A board whose sensors are not answering still has its restart history. save_telemetry is
     behind the sanity gate and this is not, which matters because a board that is rebooting
     repeatedly is a strong candidate for a board whose node has stopped reporting too.
  G. Firmware with no ring at all reports absence rather than an empty history.

The clock is fixed rather than wall-clock, the same choice tests/test_device_state.py makes and
safe for the same reason: store._prune_old runs on real time and off a write counter this file
never turns.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "crashvisible.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is tests/test_admin_state.py's subject, not this one

from fastapi.testclient import TestClient  # noqa: E402

from app import main, scheduler, store  # noqa: E402

T0 = 1754300000
CLOCK = {"t": T0}
scheduler.now = lambda: CLOCK["t"]
CLIENT = TestClient(main.app)

PANEL = "58:E6:C5:71:97:30"

# A crash record in the widths src/health.cpp's buffers allow: char[16] task, char[12] of
# "0x%08lx" pc, and up to char[112] of space-separated backtrace.
CRASH = ("camnet", "0x42017a3e",
         "0x42017a3e 0x420180aa 0x4038b1c1 0x4037fd4e 0x40378e2a")


def _reset() -> None:
    for table in ("telemetry", "device_boots", "device_fw", "device_flags"):
        store._conn().execute(f"DELETE FROM {table}")
    CLOCK["t"] = T0


def _boot(reset: str = "power", crashes: int = 205, boots: int = 0,
          restarts: list | None = None, crash: tuple | None = None) -> dict:
    """One `boot` block as the panel builds it in src/plantrx.cpp.

    boots=0 with no `restarts` key is what firmware predating the ring sends, and the crash trio
    is omitted rather than nulled for the same reason: absent is what the device transmits, and a
    body spelling these as null would test a device that does not exist.
    """
    b = {"reset": reset, "crashes": crashes, "image_pending": False}
    if boots:
        b["boots"] = boots
    if restarts is not None:
        b["restarts"] = restarts
    if crash is not None:
        b["crash_task"], b["crash_pc"], b["crash_bt"] = crash
    return b


def _poll(boot: dict, device: str = PANEL, sane: bool = True) -> None:
    """One poll at the current clock.

    `sane` drives links.node_online, which is the whole of scheduler.telemetry_is_sane for a body
    carrying node readings: with it False the row is refused and no telemetry is stored at all,
    which is the state case F is about.
    """
    r = CLIENT.post("/v1/telemetry", json={
        "device": device, "uptime_ms": 61000,
        "sensors": {"co2_ppm": 900.0, "temp_c": 24.0, "rh_pct": 55.0},
        "links": {"node_online": sane},
        "boot": boot,
    })
    assert r.status_code == 200, (r.status_code, r.text[:200])


def _dev(device: str = PANEL) -> dict:
    r = CLIENT.get("/v1/admin/state")
    assert r.status_code == 200, (r.status_code, r.text[:200])
    got = [d for d in r.json()["devices"] if d["device"] == device]
    assert got, [d["device"] for d in r.json()["devices"]]
    return got[0]


def test_the_boot_block_is_always_the_same_shape():
    """A key that comes and goes is a page rendering blanks with no error anywhere - the same
    contract tests/test_admin_state.py asserts for flags, for the same reason.
    """
    _reset()
    _poll(_boot())
    boot = _dev()["boot"]
    assert set(boot) == {"reset", "crashes", "crash", "restarts"}, sorted(boot)
    return "reset, crashes, crash and restarts are present on every device"


def test_a_posted_crash_record_reaches_the_state():
    """A. The panic handler's record, which was stored and never read back out until now."""
    _reset()
    _poll(_boot(reset="panic", crashes=204, crash=CRASH))
    crash = _dev()["boot"]["crash"]
    assert crash is not None, "a posted crash record did not reach the operator view"
    assert (crash["task"], crash["pc"], crash["bt"]) == CRASH, crash
    # The receipt time rides with it because this table retains a fortnight, and a backtrace from
    # twelve days ago beside a reboot from tonight is worse than no backtrace.
    assert crash["ts"] == T0, crash
    return "task, pc, backtrace and the receipt time of the poll that carried them"


def test_no_crash_record_reads_as_absence_and_not_as_health():
    """B. The device erases its dump once a post carrying it is answered, and a watchdog reset may
    leave no dump at all, so this must never read as "this board has never crashed".
    """
    _reset()
    _poll(_boot(reset="int_wdt", crashes=206))
    boot = _dev()["boot"]
    assert boot["crash"] is None, boot["crash"]
    # And the two fields that DO answer "has it been crashing" are still there to answer it.
    assert boot["reset"] == "int_wdt", boot
    assert boot["crashes"] == 206, boot
    # The page says so in schema.Boot's terms rather than badging the board clean.
    page = CLIENT.get("/admin").text
    assert "새로 보고된 크래시 기록이 없습니다." in page, "the page has no sentence for the absence"
    assert "지금까지 크래시가 없었다는 뜻은 아닙니다." in page, "the absence reads as a clean bill"
    return "crash is null, reset and crashes still answer, and the page refuses to imply health"


def test_the_reason_and_the_count_outlive_the_dump_fields():
    """C. The device sends crash_task/pc/bt only until a post carrying them is answered, so in the
    ordinary case ONE row in the table has them and every row after it is NULL. Reading them off
    the newest row would show a crash for one refresh of the page and then lose it for good.
    """
    _reset()
    _poll(_boot(reset="panic", crashes=204, crash=CRASH))
    # Three polls after the acknowledgement, the way the board actually behaves: same boot, same
    # reason, no crash trio, because its own copy is erased.
    for _ in range(3):
        CLOCK["t"] += 60
        _poll(_boot(reset="panic", crashes=204))
    boot = _dev()["boot"]
    assert boot["crash"] is not None, "the crash record was lost to the polls that followed it"
    assert boot["crash"]["ts"] == T0, "the record was re-dated to a poll that did not carry it"
    assert boot["reset"] == "panic" and boot["crashes"] == 204, boot

    # And a later crash replaces it, rather than the first one being kept forever.
    CLOCK["t"] += 60
    later = ("camnet", "0x42019000", "0x42019000 0x4038b1c1")
    _poll(_boot(reset="int_wdt", crashes=205, crash=later))
    boot = _dev()["boot"]
    assert (boot["crash"]["task"], boot["crash"]["pc"], boot["crash"]["bt"]) == later, boot
    assert boot["reset"] == "int_wdt" and boot["crashes"] == 205, boot
    return "the dump survives the polls that stop carrying it, and a newer dump replaces it"


def test_five_power_resets_in_ten_minutes_are_a_fact():
    """D. The case that cost 1,916 rows of console to find by hand.

    Five power-ons, then one interrupt watchdog twenty minutes later. `crashes` must not move
    across the five - a power reset is not a panic and the counter is right to refuse it - and the
    ring must show all five anyway, in order and dated.
    """
    _reset()
    ring = []
    for i in range(5):
        ring.append("power")
        _poll(_boot(reset="power", crashes=205, boots=201 + i, restarts=list(ring)))
        CLOCK["t"] += 120
    boot = _dev()["boot"]
    assert boot["crashes"] == 205, "a power reset moved the crash counter"
    assert [r["reason"] for r in boot["restarts"]] == ["power"] * 5, boot["restarts"]
    # Newest first, and each boot is its own row rather than five identical-looking ones merged:
    # seq is the device's own cumulative boot number and is the only thing that tells them apart.
    assert [r["seq"] for r in boot["restarts"]] == [205, 204, 203, 202, 201], boot["restarts"]
    # Dated by the poll that first named each one, which is what makes "in ten minutes" readable
    # rather than reconstructed.
    seen = [r["first_seen"] for r in boot["restarts"]]
    assert seen == [T0 + 480, T0 + 360, T0 + 240, T0 + 120, T0], seen
    assert seen[0] - seen[-1] == 480, "the spread of the five was lost"

    # Then the watchdog twenty minutes later: a different reason, and one that DOES count.
    CLOCK["t"] += 20 * 60
    ring.append("int_wdt")
    _poll(_boot(reset="int_wdt", crashes=206, boots=206, restarts=ring[-8:]))
    boot = _dev()["boot"]
    assert boot["crashes"] == 206, "the watchdog reset did not count as a crash"
    assert [r["reason"] for r in boot["restarts"]][:2] == ["int_wdt", "power"], boot["restarts"]
    assert boot["restarts"][0]["seq"] == 206, boot["restarts"][0]
    return "five power-ons and one watchdog, in order, dated, with crashes moving once"


def test_a_resent_ring_does_not_redate_a_boot():
    """E. The device re-sends its whole ring on every poll, and first_seen is a claim about the
    FIRST poll that named a boot. A re-date would turn a restart from an hour ago into one from
    just now, which is the number an operator would act on.
    """
    _reset()
    ring = ["power", "power", "panic"]
    _poll(_boot(reset="panic", crashes=204, boots=203, restarts=ring))
    first = [r["first_seen"] for r in _dev()["boot"]["restarts"]]
    assert first == [T0, T0, T0], first
    for _ in range(4):
        CLOCK["t"] += 300
        _poll(_boot(reset="panic", crashes=204, boots=203, restarts=ring))
    again = _dev()["boot"]["restarts"]
    assert [r["first_seen"] for r in again] == first, again
    assert len(again) == 3, "the re-sent ring was stored again as new boots"
    return "four re-sends of the same ring change neither the dates nor the row count"


def test_the_ring_survives_a_board_whose_sensors_are_not_answering():
    """F. save_telemetry is behind scheduler.telemetry_is_sane and this is not, deliberately: a
    board that is rebooting repeatedly is a strong candidate for a board whose sensor node has
    stopped reporting, and gating the restart history would drop exactly the history of the board
    it exists to explain.
    """
    _reset()
    _poll(_boot(reset="power", crashes=205, boots=205, restarts=["power", "power"]), sane=False)
    boot = _dev()["boot"]
    assert [r["reason"] for r in boot["restarts"]] == ["power", "power"], boot["restarts"]
    # And the two telemetry-backed fields honestly say nothing, because no row was stored.
    assert boot["reset"] is None and boot["crashes"] is None, boot
    assert boot["crash"] is None, boot
    return "an insane payload still leaves its restart history, and the rest reads as unreported"


def test_firmware_with_no_ring_reports_absence():
    """G. A board whose firmware predates the ring sends none, and an empty list must not read as
    a board that has never rebooted - the same distinction B draws for the crash record.
    """
    _reset()
    _poll(_boot(reset="power", crashes=205))
    boot = _dev()["boot"]
    assert boot["restarts"] == [], boot["restarts"]
    assert boot["reset"] == "power" and boot["crashes"] == 205, boot
    page = CLIENT.get("/admin").text
    assert "이 펌웨어는 재시작 목록을 보고하지 않습니다." in page, "no sentence for a missing ring"
    return "no ring reads as 'this firmware does not report it', not as 'it never rebooted'"


if __name__ == "__main__":
    for fn in (test_the_boot_block_is_always_the_same_shape,
               test_a_posted_crash_record_reaches_the_state,
               test_no_crash_record_reads_as_absence_and_not_as_health,
               test_the_reason_and_the_count_outlive_the_dump_fields,
               test_five_power_resets_in_ten_minutes_are_a_fact,
               test_a_resent_ring_does_not_redate_a_boot,
               test_the_ring_survives_a_board_whose_sensors_are_not_answering,
               test_firmware_with_no_ring_reports_absence):
        print("%-58s %s" % (fn.__name__, fn()))
    print("OK")
