"""The log path off the two boards nobody can attach a cable to.

    cd server && python tests/test_nodelog.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives. pytest collects this file unchanged.

WHY THIS FILE EXISTS. The sensor node has no WiFi except for the seconds it spends
downloading an image, and the ESP32-CAM sits in a housing on a pole. Neither has a
console anybody can reach, so when one of them refuses an update the only
explanation is a line it printed to nobody. That line now travels to the panel over
ESP-NOW and the panel POSTs it here, which makes this table the single place a
person can look up what a node actually said.

The parts worth defending are the ones a plain "it stores rows" implementation
gets wrong:

  A. The row is stamped with the server's receive time. `ms` in the body is the
     forwarding PANEL's millis(), not the node's - no node millisecond clock is on
     the wire at all - so it wraps and it restarts when the panel does, and it can
     order lines inside one batch and nothing wider. The two-batch test below is
     that disagreement.
  B. Both caps refuse the whole batch. Trimming a caller that is not the panel down
     to 64 lines would leave a table full of rows nobody can account for.
  C. The read has a ceiling of its own, so nobody turns a debugging aid into a
     query that reads the volume.
  D. Both routes are behind the bearer, because this is the one table an operator
     trusts to tell them what happened.
"""
import asyncio
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "nodelog.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is one test here, not the fixture

from fastapi.testclient import TestClient  # noqa: E402
from starlette.requests import Request  # noqa: E402

from app import main, scheduler, store  # noqa: E402
from app.schema import (NODELOG_LINES_MAX, NODELOG_TEXT_MAX,  # noqa: E402
                        NodeLogLine)

CLIENT = TestClient(main.app)
# The same app, but willing to hand back a 5xx instead of re-raising it, which is the only way to
# observe from here what the panel would observe over the wire.
CRASH_CLIENT = TestClient(main.app, raise_server_exceptions=False)

# The panel's own MAC. `device` on this route names the forwarder and not the board that spoke,
# which is the one thing about it that surprises a reader of the other endpoints.
PANEL = "AA:BB:CC:DD:EE:FF"

# Real time, and not the fixed 1754300000 the other test files use. Those files can date their
# rows in 2025 because retention never runs during them; these rows have a retention of their
# own that is three days rather than a fortnight (store.NODE_LOG_TTL_S), and _maybe_prune fires
# off a write counter shared with every other file in a pytest process. A fixed past timestamp
# would mean a prune triggered by somebody else's telemetry silently deletes this file's table
# mid-test, which reads as "the endpoint stored nothing".
T0 = int(time.time())


def _clear() -> None:
    """Empty the table so a test reads only its own rows.

    GET has no device filter - handing back the newest lines regardless of who forwarded them is
    the whole point of it - so two tests sharing this table would each see the other's.
    """
    store._conn().execute("DELETE FROM node_logs")


def _post(lines, at=T0, device=PANEL):
    """One batch, received at `at` on the server's clock.

    scheduler.now is restored afterwards rather than stubbed for the module. Several other test
    files stub it at import time and pytest shares one process with them, so a module-level stub
    here would mean the last file imported decides what "now" means for every file that runs.
    """
    was = scheduler.now
    scheduler.now = lambda: at
    try:
        return CLIENT.post("/v1/nodelog", json={"device": device, "lines": lines})
    finally:
        scheduler.now = was


def _line(role="cam", ms=1000, text="hello"):
    return {"role": role, "ms": ms, "text": text}


def test_a_batch_posts_and_reads_back():
    """The round trip, asserted through the endpoint rather than through the store.

    A Korean line is in here on purpose. The panel forwards what it heard verbatim and is not
    allowed to be the reason a line changes, so a text column that mangled non-ASCII would be a
    silent corruption of the only record of what a node said.
    """
    _clear()
    r = _post([
        _line("cam", 1000, "ota: begin 1441792 bytes"),
        _line("cam", 1200, "ota: write failed, aborting"),
        _line("node", 90, "카메라 노드 업데이트 실패"),
    ])
    assert r.status_code == 200, r.text
    assert r.json() == {"ok": True, "stored": 3}, r.json()

    got = CLIENT.get("/v1/nodelog")
    assert got.status_code == 200, got.text
    rows = got.json()
    assert len(rows) == 3, rows
    # Newest first: a reader wants the last thing a node said without scrolling.
    assert [x["text"] for x in rows] == [
        "카메라 노드 업데이트 실패",
        "ota: write failed, aborting",
        "ota: begin 1441792 bytes",
    ], rows
    assert [x["role"] for x in rows] == ["node", "cam", "cam"], rows
    assert [x["ms"] for x in rows] == [90, 1200, 1000], rows
    assert all(x["device"] == PANEL for x in rows), rows
    assert all(x["recv_ts"] == T0 for x in rows), rows
    return "3 lines posted, read back newest-first with the panel's MAC and the server's clock"


def test_the_server_clock_orders_what_the_body_clock_cannot():
    """A smaller `ms` on a later line, which is why recv_ts is not an optional nicety.

    `ms` is the forwarding PANEL's millis() (see schema.NodeLogLine), so it disagrees with the
    true order in two reachable ways: the panel reboots - which is exactly what an operator does
    after flashing it, beside the nodes it was mid-update on - and it wraps to 0 after ~49.7 days
    of panel uptime. Either way the line explaining an outcome carries a smaller `ms` than the
    line announcing the attempt, and sorting on it puts the two lines a person came to this table
    for in the wrong order.

    Note what this test does NOT claim. It used to be written as a NODE reboot showing up as `ms`
    going backwards. That cannot happen: no node millisecond clock is on the wire at all -
    NodeRepMsg carries only uptime_s, in whole seconds, and the log path discards even that
    (shared/nodeproto.h:123, src/nodeota.cpp:396). The property under test is narrower and real -
    the read is ordered by when this server heard a line, never by what the body said about it.
    """
    _clear()
    assert _post([_line("cam", 9_000_000, "ota: rebooting into new image")], at=T0).status_code == 200
    assert _post([_line("cam", 40, "boot: running 1.4.0")], at=T0 + 12).status_code == 200

    rows = CLIENT.get("/v1/nodelog", params={"role": "cam"}).json()
    assert [x["text"] for x in rows] == [
        "boot: running 1.4.0", "ota: rebooting into new image"], rows
    assert [x["recv_ts"] for x in rows] == [T0 + 12, T0], rows
    # And the body's clock, kept because it is the only thing that orders two lines inside one
    # batch, is exactly the field that would have got this backwards.
    assert rows[0]["ms"] < rows[1]["ms"], rows
    return "the later line sorts first on recv_ts and last on ms"


def test_the_role_filter_and_the_limit_narrow_the_read():
    """Two knobs, because the interesting question is usually "what did the cam say"."""
    _clear()
    _post([_line("cam", 1, "cam one"), _line("node", 2, "node one"),
           _line("cam", 3, "cam two"), _line("?", 4, "unknown role")])

    cam = CLIENT.get("/v1/nodelog", params={"role": "cam"}).json()
    assert [x["text"] for x in cam] == ["cam two", "cam one"], cam
    # "?" is what nodeproto_role_name() answers for a role byte it does not know, so a
    # version-skewed node's lines are filed under it - and the filter has to be able to ask for
    # exactly those, which is why an unknown role is a legal filter here and a 400 on /firmware.
    skew = CLIENT.get("/v1/nodelog", params={"role": "?"}).json()
    assert [x["text"] for x in skew] == ["unknown role"], skew
    assert CLIENT.get("/v1/nodelog", params={"role": "panel"}).json() == []

    one = CLIENT.get("/v1/nodelog", params={"limit": 1}).json()
    assert [x["text"] for x in one] == ["unknown role"], one
    return "role= selects (including the skew role \"?\"), limit= truncates from the newest end"


def test_an_over_long_batch_is_refused_whole():
    """64 lines is the panel's buffer; 65 is somebody else, and none of it is stored.

    Storing the first 64 of an over-long batch would be worse than refusing it: the rows that
    landed would look exactly like a normal post, and the ones that did not would be a gap
    nobody can see. The boundary is asserted in both directions so the cap cannot drift into
    being off by one.
    """
    _clear()
    ok = _post([_line("cam", i, "line %d" % i) for i in range(NODELOG_LINES_MAX)])
    assert ok.status_code == 200, ok.text
    assert ok.json()["stored"] == NODELOG_LINES_MAX, ok.json()

    _clear()
    too_many = _post([_line("cam", i, "line %d" % i) for i in range(NODELOG_LINES_MAX + 1)])
    assert too_many.status_code == 422, (too_many.status_code, too_many.text)
    assert CLIENT.get("/v1/nodelog").json() == [], "a refused batch wrote rows anyway"
    return "%d lines accepted, %d refused with nothing written" % (
        NODELOG_LINES_MAX, NODELOG_LINES_MAX + 1)


def test_an_over_long_line_is_refused_with_its_batch():
    """One oversize line takes the whole POST down, for the same reason as the batch cap.

    The cap is above what NodeRepMsg can physically carry (160 bytes of text), so a line that
    trips it did not come off the wire - it came from something writing to this endpoint
    directly, and the row it wants to write is the one an operator would later read as a node's
    own words.
    """
    _clear()
    edge = _post([_line("cam", 1, "x" * NODELOG_TEXT_MAX)])
    assert edge.status_code == 200, edge.text

    _clear()
    over = _post([_line("cam", 1, "ok"), _line("cam", 2, "x" * (NODELOG_TEXT_MAX + 1))])
    assert over.status_code == 422, (over.status_code, over.text)
    assert CLIENT.get("/v1/nodelog").json() == [], "the good line of a refused batch was stored"
    return "%d characters accepted, %d refuses the batch" % (
        NODELOG_TEXT_MAX, NODELOG_TEXT_MAX + 1)


def test_both_nodelog_routes_need_the_bearer():
    """Write and read are both behind the shared secret.

    The write, because anyone on the LAN could otherwise fill the one table an operator reads to
    find out what a node did. The read, because those lines are a running commentary on what the
    greenhouse's boards are doing and there is no reason to hand it out.

    DEVICE_TOKEN is put back in a finally for the reason tests/test_firmware_roles.py gives: it
    is a module global, and the other test files in this directory poll unauthenticated on
    purpose.
    """
    _clear()
    was = main.DEVICE_TOKEN
    main.DEVICE_TOKEN = "test-token"
    good = {"Authorization": "Bearer test-token"}
    body = {"device": PANEL, "lines": [_line()]}
    try:
        assert CLIENT.post("/v1/nodelog", json=body).status_code == 401
        assert CLIENT.post("/v1/nodelog", json=body,
                           headers={"Authorization": "Bearer wrong"}).status_code == 401
        assert CLIENT.get("/v1/nodelog").status_code == 401
        assert CLIENT.get("/v1/nodelog", params={"role": "cam"}).status_code == 401
        assert CLIENT.post("/v1/nodelog", json=body, headers=good).status_code == 200
        assert len(CLIENT.get("/v1/nodelog", headers=good).json()) == 1
    finally:
        main.DEVICE_TOKEN = was
    return "POST and GET both 401 without the bearer"


def test_the_read_limit_has_a_ceiling():
    """A caller asking for everything gets NODE_LOG_READ_MAX and not the table.

    Written through the store rather than the endpoint because the point is the ceiling on the
    read, and getting past the endpoint's 64-line write cap would take sixteen POSTs to say
    nothing extra. The clamp lives in recent_node_logs precisely so that it holds for both
    callers.
    """
    _clear()
    n = store.NODE_LOG_READ_MAX + 5
    store.save_node_logs(
        PANEL, [NodeLogLine(role="flood", ms=i, text="line %d" % i) for i in range(n)], T0)

    rows = CLIENT.get("/v1/nodelog", params={"role": "flood", "limit": 100000}).json()
    assert len(rows) == store.NODE_LOG_READ_MAX, len(rows)
    # Clamped from the newest end, so an over-large limit still answers the question the reader
    # asked: what was said most recently.
    assert rows[0]["text"] == "line %d" % (n - 1), rows[0]
    assert len(store.recent_node_logs(limit=0)) == 1, "a limit of 0 read nothing at all"
    _clear()
    return "%d rows stored, %d handed back for limit=100000" % (n, len(rows))


def test_a_crash_under_the_write_is_not_a_success():
    """The panel advances its cursor on any 2xx, so a write that failed must not be one.

    src/nodelog.cpp:347 accepts 200..299 and :432 advances s_sent on it, dropping the batch from
    the panel's ring without touching s_dropped. main._unhandled used to answer EVERY unhandled
    error with a 200 carrying a Prescription, so a save that raised took twenty lines off the
    panel, stored none of them, and left no trace at either end - the exact hole the cursor rule
    at src/nodelog.cpp:19-23 exists to prevent, and the one failure this table cannot report on
    because it is the table.
    """
    _clear()

    def boom(*a, **k):
        raise RuntimeError("disk full")

    was = store.save_node_logs
    store.save_node_logs = boom
    try:
        r = CRASH_CLIENT.post("/v1/nodelog",
                              json={"device": PANEL, "lines": [_line("cam", 1, "x")]})
    finally:
        store.save_node_logs = was

    assert r.status_code == 500, r.status_code
    # Not merely "not 200": the body must not be a Prescription either, or a client that trusts
    # the shape over the status reads a crash as a valid answer.
    assert "rx_id" not in r.text, r.text
    assert store.recent_node_logs() == [], "a refused write still stored something"
    return "a save that raises answers 500 with no prescription, and the cursor stays put"


def test_only_the_poll_gets_an_error_disguised_as_an_answer():
    """The 200-on-crash belongs to the poll alone - and the poll still has it.

    Both halves in one place because they are one decision. A 200 from this server means "here is
    a valid Prescription", so the poll is the only caller for which answering a bug with one is
    honest; it keeps that answer because a panel retrying a server-side bug every two seconds is
    what the handler was written to prevent. Every other route gets a real 500. Exercised against
    the handler itself rather than through a crashing route, because that is the whole of its
    behaviour: it branches on the path and nothing else.
    """
    def answer(path):
        scope = {"type": "http", "method": "POST", "path": path, "headers": []}
        return asyncio.run(main._unhandled(Request(scope), RuntimeError("boom")))

    poll = answer(main._POLL_PATH)
    assert poll.status_code == 200, poll.status_code
    assert b"rx_id" in poll.body, poll.body

    for path in ("/v1/nodelog", "/v1/firmware/latest", "/v1/firmware/image", "/v1/identify",
                 "/v1/frame", "/health"):
        r = answer(path)
        assert r.status_code == 500, (path, r.status_code)
        assert b"rx_id" not in r.body, (path, r.body)
    return "%s keeps its 200; six other paths answer 500" % main._POLL_PATH


if __name__ == "__main__":
    print("roundtrip %s" % test_a_batch_posts_and_reads_back())
    print("ordering  %s" % test_the_server_clock_orders_what_the_body_clock_cannot())
    print("filter    %s" % test_the_role_filter_and_the_limit_narrow_the_read())
    print("batchcap  %s" % test_an_over_long_batch_is_refused_whole())
    print("textcap   %s" % test_an_over_long_line_is_refused_with_its_batch())
    print("auth      %s" % test_both_nodelog_routes_need_the_bearer())
    print("ceiling   %s" % test_the_read_limit_has_a_ceiling())
    print("crash     %s" % test_a_crash_under_the_write_is_not_a_success())
    print("poll-only %s" % test_only_the_poll_gets_an_error_disguised_as_an_answer())
    print("OK")
