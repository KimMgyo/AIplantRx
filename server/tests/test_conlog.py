"""The panel's own console, on the one path a phone can reach.

    cd server && python tests/test_conlog.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives. pytest collects this file unchanged.

WHY THIS FILE EXISTS. The panel's display sits on UART0's pins, so running the
screen means running with no serial console; hlog.cpp answers that with a ring in
PSRAM served on TCP/23. Port 23 only exists on the LAN, and the greenhouse is an
hour from the laptop with the toolchain - so the same ring is pushed here and served
beside the cards. This table is therefore the only way to read what the panel said
while nobody was standing next to it.

What it stores is a BYTE STREAM, not lines, and every test below defends some
consequence of that:

  A. Chunks concatenate byte-for-byte. A chunk is a slice of a ring and may begin
     and end mid-line; the split-line test is the one that fails if anyone ever
     "helpfully" strips or splits on arrival.
  B. The cursor never rewinds. A quiet poll answers with the `since` it was given
     and not 0, because a page that stores the answer unconditionally would replay
     six hours of console the first time the board had nothing to say.
  C. An unknown device is an empty answer and not a 404. The page asks per device
     from a list this same database gave it, so a miss means "has not posted yet",
     which is a state to render.
  D. A chunk with no device is refused. Filed under "" it would be unreachable by
     every reader, which is storage that cannot be read.
  E. Text clipped mid-UTF-8 is accepted. The board truncates at LOG_LINE_MAX with
     no idea where a sequence began, so refusing the chunk would lose the 4KB
     around it to punish one byte.
  F. Both routes are behind the bearer. Anyone who can write here can put words in
     the board's mouth, on the table an operator trusts to tell them what happened.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "conlog.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is one test here, not the fixture

from fastapi.testclient import TestClient  # noqa: E402

from app import main, store  # noqa: E402

store.init_db()
c = TestClient(main.app)
DEV = "58:E6:C5:71:97:30"

# The three chunks a ring really produces: the interesting line is split across two
# of them, which is what a fixed-size read out of a byte ring does to prose.
PARTS = [
    "[health] reset=power crashes=153\n[net] connect ch2\n[plantrx] OK http=2",
    "00 rtt=1128ms why=ok rows=2/4\n[camnet] 19.9fps | LI",
    "VE | cam=online ip=192.168.10.72\n",
]


def _post(text, device=DEV, dropped=0):
    return c.post("/v1/conlog", content=text.encode("utf-8"),
                  headers={"X-Device": device, "X-Dropped": str(dropped),
                           "Content-Type": "text/plain"})


def _get(device=DEV, since=0, **kw):
    return c.get("/v1/conlog", params={"device": device, "since": since, **kw})


def test_chunks_concatenate_byte_for_byte():
    """A: the stream is reassembled by concatenation and nothing else.

    Asserted on the joined text rather than per row on purpose: per-row equality
    would still pass for an implementation that stripped a trailing newline off
    each chunk, and that implementation loses a line break every 4KB.
    """
    for i, p in enumerate(PARTS):
        r = _post(p, dropped=i)
        assert r.status_code == 200, r.text
        assert r.json()["stored"] == len(p), r.text

    d = _get().json()
    joined = "".join(row["text"] for row in d["rows"])
    assert joined == "".join(PARTS), repr(joined[:120])
    assert "[plantrx] OK http=200 rtt=1128ms" in joined, "the split line did not survive"
    assert [row["dropped"] for row in d["rows"]] == [0, 1, 2], d["rows"]
    return "three chunks reassemble exactly, including a line split across two of them"


def test_the_cursor_never_rewinds():
    """B: a poll with nothing new answers with the cursor it was given."""
    first = _get().json()
    assert first["rows"], "nothing stored to tail"
    cur = first["next"]
    assert cur == max(r["id"] for r in first["rows"])

    quiet = _get(since=cur).json()
    assert quiet["rows"] == []
    assert quiet["next"] == cur, "a quiet poll moved the cursor: %r" % quiet["next"]

    _post("[hlog] one more line\n")
    after = _get(since=cur).json()
    assert len(after["rows"]) == 1, after
    assert after["rows"][0]["text"] == "[hlog] one more line\n"
    assert after["next"] > cur
    return "quiet polls hold the cursor; the next chunk arrives alone"


def test_ordering_is_chronological_and_not_newest_first():
    """A, again, from the other side: recent_node_logs is newest-first and this is not.

    Worth its own assertion because the two read helpers sit next to each other in
    store.py and share a shape. Reversing this one produces a console that is
    subtly, unfixably wrong rather than one that fails.
    """
    rows = _get().json()["rows"]
    assert rows == sorted(rows, key=lambda r: r["id"]), "not ascending"
    assert rows[0]["text"].startswith("[health] reset=power"), rows[0]["text"][:40]
    return "oldest first, so concatenation is the stream and not a reversal of it"


def test_an_unknown_device_is_empty_and_not_an_error():
    """C."""
    d = _get(device="AA:BB:CC:DD:EE:FF")
    assert d.status_code == 200, d.text
    body = d.json()
    assert body["rows"] == []
    assert body["next"] == 0
    return "an unposted device renders as empty rather than raising"


def test_a_chunk_with_no_device_is_refused():
    """D. Whitespace too: a header of spaces is as unreadable as an absent one."""
    for bad in ("", "   "):
        r = _post("orphaned bytes", device=bad)
        assert r.status_code == 400, "%r accepted: %s" % (bad, r.text)
    return "a chunk nobody could ever read back is refused at the edge"


def test_text_clipped_mid_utf8_is_accepted():
    """E. What the board does to Korean prose at LOG_LINE_MAX, byte for byte."""
    clipped = b"[plantrx] \xed\x8c\x90\xeb\x8f"      # "판" then a truncated "도"
    r = c.post("/v1/conlog", content=clipped,
               headers={"X-Device": DEV, "Content-Type": "text/plain"})
    assert r.status_code == 200, r.text
    stored = store.read_console(DEV, since=0, limit=store.CONSOLE_READ_MAX)[0][-1]["text"]
    assert stored.startswith("[plantrx] 판"), repr(stored)
    assert "\ufffd" in stored, "the clipped bytes vanished instead of being marked"
    return "a line cut mid-sequence is kept with the break marked, not refused"


def test_an_empty_body_stores_nothing_and_is_not_an_error():
    """E's corner: a retried POST whose body was already forwarded."""
    before = len(_get(limit=store.CONSOLE_READ_MAX).json()["rows"])
    r = _post("")
    assert r.status_code == 200, r.text
    assert r.json()["stored"] == 0
    after = len(_get(limit=store.CONSOLE_READ_MAX).json()["rows"])
    assert after == before, "an empty chunk became a row the reader has to skip"
    return "an empty body is accepted and stored as nothing"


def test_the_read_limit_is_clamped_in_the_store():
    """The ceiling recent_node_logs has, for the same reason: these are 4KB chunks.

    Asserted against the store rather than the route so it holds for the next caller
    too - a clamp that lives in a route is a clamp the second route forgets.
    """
    rows, _ = store.read_console(DEV, since=0, limit=10 ** 6)
    assert len(rows) <= store.CONSOLE_READ_MAX, len(rows)
    rows, _ = store.read_console(DEV, since=0, limit=0)
    assert len(rows) >= 1, "a limit of zero returned nothing instead of being floored"
    return "limit is clamped to CONSOLE_READ_MAX and floored at one, in the store"


def test_tail_returns_the_newest_chunks_still_ascending():
    """The cold-open question, which reading forward from 0 answers with the wrong end.

    Both halves are asserted because getting one right is easy: `tail` has to hand back the
    NEWEST chunks, and it has to hand them back in stream order anyway - newest-first would be a
    console that reads bottom to top inside the page's very first paint.
    """
    for i in range(12):
        assert _post("chunk-%02d\n" % i).status_code == 200

    d = _get(tail=1, limit=4).json()
    texts = [r["text"] for r in d["rows"]]
    assert len(texts) == 4, texts
    assert texts == sorted(texts), "tail came back newest-first: %r" % texts
    assert texts[-1] == "chunk-11\n", texts[-1]
    assert texts[0] == "chunk-08\n", texts[0]
    assert d["next"] == max(r["id"] for r in d["rows"])

    # And the handover: the cursor from a tail read is a cursor, so the next poll drops tail and
    # follows forward without a gap and without a repeat.
    _post("after-the-tail\n")
    nxt = _get(since=d["next"]).json()
    assert [r["text"] for r in nxt["rows"]] == ["after-the-tail\n"], nxt["rows"]
    return "tail hands back the newest chunks in stream order, and its cursor follows forward"


def test_tail_ignores_since_rather_than_combining_with_it():
    """Two different questions. A caller that passed both does not know which it asked."""
    everything = _get(limit=store.CONSOLE_READ_MAX).json()["rows"]
    newest_id = max(r["id"] for r in everything)
    d = _get(since=newest_id, tail=1, limit=3).json()
    assert len(d["rows"]) == 3, "since suppressed the tail: %r" % d["rows"]
    assert d["rows"][-1]["id"] == newest_id
    return "tail=1 answers the newest N even when since is already at the end"


def test_tail_on_an_empty_device_is_still_empty():
    """C, through the other branch: the tail query has its own SQL and its own way to be wrong."""
    d = _get(device="AA:BB:CC:DD:EE:FF", tail=1)
    assert d.status_code == 200, d.text
    assert d.json() == {"rows": [], "next": 0}, d.text
    return "an unposted device tails to nothing rather than raising"


def test_both_routes_need_the_bearer():
    """F. DEVICE_TOKEN is a module global and is put back in a finally, for the reason
    test_nodelog.py gives about its own auth test: the other files in this directory
    poll unauthenticated on purpose, and leaking a token into main breaks them."""
    was = main.DEVICE_TOKEN
    main.DEVICE_TOKEN = "conlog-test-token"
    try:
        assert c.get("/v1/conlog", params={"device": DEV}).status_code == 401
        assert c.post("/v1/conlog", content=b"x",
                      headers={"X-Device": DEV}).status_code == 401
        good = {"Authorization": "Bearer conlog-test-token", "X-Device": DEV}
        assert c.get("/v1/conlog", params={"device": DEV},
                     headers=good).status_code == 200
        assert c.post("/v1/conlog", content=b"authorised\n", headers=good).status_code == 200
    finally:
        main.DEVICE_TOKEN = was
    return "unauthenticated reads and writes are both 401"


def test_the_prune_bounds_the_table():
    """The one table that grows by 15KB a minute, so the TTL is the only thing
    between a debugging aid and a full volume. Reaches into _prune_old directly
    because the shared write counter would need thousands of inserts to turn."""
    old = int(__import__("time").time()) - store.CONSOLE_TTL_S - 60
    store._conn().execute(
        "INSERT INTO device_console (device, recv_ts, dropped, text) VALUES (?,?,?,?)",
        (DEV, old, 0, "[hlog] this is older than the ttl\n"))
    assert any("older than the ttl" in r["text"]
               for r in store.read_console(DEV, 0, store.CONSOLE_READ_MAX)[0])
    store._prune_old(store._conn())
    assert not any("older than the ttl" in r["text"]
                   for r in store.read_console(DEV, 0, store.CONSOLE_READ_MAX)[0])
    assert store.read_console(DEV, 0, store.CONSOLE_READ_MAX)[0], "the prune took everything"
    return "rows past CONSOLE_TTL_S go and newer ones stay"


if __name__ == "__main__":
    for fn in (test_chunks_concatenate_byte_for_byte,
               test_the_cursor_never_rewinds,
               test_ordering_is_chronological_and_not_newest_first,
               test_an_unknown_device_is_empty_and_not_an_error,
               test_a_chunk_with_no_device_is_refused,
               test_text_clipped_mid_utf8_is_accepted,
               test_an_empty_body_stores_nothing_and_is_not_an_error,
               test_the_read_limit_is_clamped_in_the_store,
               test_tail_returns_the_newest_chunks_still_ascending,
               test_tail_ignores_since_rather_than_combining_with_it,
               test_tail_on_an_empty_device_is_still_empty,
               test_both_routes_need_the_bearer,
               test_the_prune_bounds_the_table):
        print("%-52s %s" % (fn.__name__, fn()))
    print("OK")
