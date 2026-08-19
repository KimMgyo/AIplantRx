"""The operator page's data, and the one comparison it exists to make.

    cd server && python tests/test_admin_state.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py gives.
pytest collects this file unchanged.

WHY THE COMPARISON IS THE THING UNDER TEST. Everything else on that page is a number copied from
one place to another. `current` is a verdict, it is drawn as a badge somebody acts on, and both
ways of getting it wrong are worse than showing nothing: a false 최신 hides an update that a
board needs, and a false 업데이트 필요 sends an operator to press a button that will spend five
minutes standing a greenhouse down to install the image it is already running.

The device reports a PREFIX - sixteen hex digits is all NodeRepMsg can carry (shared/nodeproto.h)
- and the manifest carries all sixty-four. So the comparison is prefix-against-manifest, and the
empty string, which is what a board that has never reported an image looks like, must never
compare equal to anything.

  A. The route is behind the same bearer as the rest of /v1. Those versions and log lines are a
     running commentary on what a greenhouse is doing.
  B. The page itself is open, because it is a constant with no device, no version and no log line
     in it - a browser has nowhere to put a bearer on a top-level navigation.
  C. The shape is the contract admin.py's JS reads key by key. A renamed key is a page that
     renders blanks with no error anywhere.
  D. current is a prefix match, false for "", and false across roles.
  E. A role with nothing published does not break the response - "no firmware published" is the
     ordinary answer on a fresh volume, not an error path.
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "adminstate.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is one test here, not the fixture

from fastapi.testclient import TestClient  # noqa: E402

from app import firmware, main, store  # noqa: E402

CLIENT = TestClient(main.app)

PANEL = "58:E6:C5:71:97:30"

# A whole 64-hex manifest hash and the sixteen-digit prefix a board reports of it.
FULL = "40b1fe11a645ba73df248079b2b455d3e93999c120a0d551c8d737ac33f76bcc"
PREFIX = FULL[:16]
OTHER_FULL = "aeb7bc805ad48b2ebe05fc0a4cd141cd2905a371c389629e3b1e2bd416368a4d"

# The offsets firmware._app_desc documents, written here rather than read, so a published image
# can be conjured without a real 3.5MB binary. tests/test_firmware_pull.py owns the same trick.
_MAGIC = 0xABCD5432


def _image(elf_hex: str, idf: bytes = b"v5.3.2") -> bytes:
    import struct
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0x00, _MAGIC)
    desc[0x70:0x70 + len(idf)] = idf
    desc[0x90:0x90 + 32] = bytes.fromhex(elf_hex)
    return b"\x00" * 0x20 + bytes(desc) + b"\xa5" * 2048


def _publish(role: str, elf_hex: str | None) -> None:
    """Put an image where firmware.manifest(role) will find it, or take it away."""
    p = firmware.image_path(role)
    p.parent.mkdir(parents=True, exist_ok=True)
    if elf_hex is None:
        if p.exists():
            p.unlink()
        return
    p.write_bytes(_image(elf_hex))
    firmware._md5_cache.clear()


def _reset(published: dict | None = None) -> None:
    store._conn().execute("DELETE FROM device_fw")
    store._conn().execute("DELETE FROM device_flags")
    store._conn().execute("DELETE FROM node_logs")
    for role in firmware.ROLES:
        _publish(role, (published or {}).get(role))


def _report(fw: dict, device: str = PANEL) -> None:
    r = CLIENT.post("/v1/telemetry", json={
        "device": device, "uptime_ms": 61000,
        "sensors": {"co2_ppm": 900, "temp_c": 24.0, "rh_pct": 55.0},
        "images": fw,
    })
    assert r.status_code == 200, (r.status_code, r.text[:200])


def _fw(elf: str, **kw) -> dict:
    out = {"elf": elf, "up_s": 120, "heap": 128000, "online": True,
           "pending": False, "can_ota": True}
    out.update(kw)
    return out


def _state() -> dict:
    r = CLIENT.get("/v1/admin/state")
    assert r.status_code == 200, (r.status_code, r.text[:200])
    return r.json()


def _role(state: dict, role: str, device: str = PANEL) -> dict:
    dev = [d for d in state["devices"] if d["device"] == device]
    assert dev, [d["device"] for d in state["devices"]]
    got = [x for x in dev[0]["roles"] if x["role"] == role]
    assert got, [x["role"] for x in dev[0]["roles"]]
    return got[0]


def test_the_state_route_needs_the_bearer():
    """A. DEVICE_TOKEN is put back in a finally: it is a module global and the other test files
    in this directory poll unauthenticated on purpose.
    """
    _reset()
    was = main.DEVICE_TOKEN
    main.DEVICE_TOKEN = "test-token"
    try:
        assert CLIENT.get("/v1/admin/state").status_code == 401
        assert CLIENT.get("/v1/admin/state",
                          headers={"Authorization": "Bearer wrong"}).status_code == 401
        ok = CLIENT.get("/v1/admin/state", headers={"Authorization": "Bearer test-token"})
        assert ok.status_code == 200, ok.status_code
    finally:
        main.DEVICE_TOKEN = was
    return "401 without the secret, 401 with the wrong one, 200 with it"


def test_the_page_itself_needs_no_token():
    """B. There is nothing in it: no device, no version, no log line."""
    _reset()
    was = main.DEVICE_TOKEN
    main.DEVICE_TOKEN = "test-token"
    try:
        r = CLIENT.get("/admin")
    finally:
        main.DEVICE_TOKEN = was
    assert r.status_code == 200, r.status_code
    assert "text/html" in r.headers.get("content-type", ""), r.headers
    # It has to be able to ASK for the token, which is the only reason it may be open.
    assert "localStorage" in r.text, "the page cannot hold a token"
    assert PANEL not in r.text, "the page shipped a device in its own body"
    return "GET /admin is 200 text/html with no secret and no data"


def test_the_shape_is_the_contract_the_page_reads():
    """C. A renamed key is a page that renders blanks with no error anywhere."""
    _reset({"panel": FULL})
    _report({"panel": _fw(PREFIX)})
    st = _state()
    assert set(st) == {"ts", "published", "devices", "logs"}, sorted(st)
    assert set(st["published"]) == set(firmware.ROLES), sorted(st["published"])
    dev = st["devices"][0]
    assert set(dev) == {"device", "last_seen", "uptime_ms", "flags", "roles"}, sorted(dev)
    assert set(dev["flags"]) == {"update_mode", "firmware_pull",
                                 "node_pull_cam", "node_pull_node"}, sorted(dev["flags"])
    assert set(_role(st, "panel")) == {
        "role", "running", "published", "current", "up_s", "heap",
        "online", "pending", "can_ota", "recv_ts"}, sorted(_role(st, "panel"))
    return "top level, device, flags and role keys are all exactly as spelled"


def test_current_is_a_prefix_match_and_nothing_looser():
    """D. The verdict, and every way of getting it wrong."""
    _reset({"panel": FULL})
    _report({"panel": _fw(PREFIX)})
    assert _role(_state(), "panel")["current"] is True, "a real prefix match was not current"

    # The empty string is what a board that has never reported an image looks like. It is a
    # prefix of everything and must be current for nothing.
    _reset({"panel": FULL})
    _report({"panel": _fw("")})
    r = _role(_state(), "panel")
    assert r["running"] == "", r
    assert r["current"] is False, "an unknown running version claimed to be current"

    # A different image, which is the whole reason the button exists.
    _reset({"panel": FULL})
    _report({"panel": _fw(OTHER_FULL[:16])})
    assert _role(_state(), "panel")["current"] is False, "a stale board claimed to be current"
    return "prefix match is current; empty and mismatched are not"


def test_nothing_published_does_not_break_the_response():
    """E. A fresh volume is the ordinary case, not an error path."""
    _reset()   # publishes nothing at all
    _report({"panel": _fw(PREFIX), "cam": _fw(OTHER_FULL[:16])})
    st = _state()
    assert all(v is None for v in st["published"].values()), st["published"]
    for role in ("panel", "cam"):
        r = _role(st, role)
        assert r["published"] == "", r
        assert r["current"] is False, "current with nothing published to be current with"
    return "no published image reads as published='' and current=false"


def test_the_flags_and_logs_ride_along():
    """The two things the page draws besides the versions, in one response with them.

    One route rather than three because the page's job is a comparison: a version judged against a
    manifest fetched four seconds later is a verdict about two different moments.
    """
    _reset({"panel": FULL})
    _report({"panel": _fw(PREFIX)})
    CLIENT.post("/v1/nodelog", json={"device": PANEL, "lines": [
        {"role": "cam", "ms": 10, "text": "[cam] streaming restored"},
        {"role": "node", "ms": 20, "text": "[node] rebooting into new image"},
    ]})
    store.set_node_pull(PANEL, "cam")
    st = _state()
    assert [x["text"] for x in st["logs"]][:2] == [
        "[node] rebooting into new image", "[cam] streaming restored"], st["logs"][:2]
    dev = [d for d in st["devices"] if d["device"] == PANEL][0]
    assert dev["flags"]["node_pull_cam"] is True, dev["flags"]
    # Read twice, because a page that consumed the arming would show it once and never again.
    assert _state()["devices"][0]["flags"]["node_pull_cam"] is True, "the read consumed the flag"
    return "logs are newest-first and an arming survives being drawn twice"


if __name__ == "__main__":
    for fn in (test_the_state_route_needs_the_bearer,
               test_the_page_itself_needs_no_token,
               test_the_shape_is_the_contract_the_page_reads,
               test_current_is_a_prefix_match_and_nothing_looser,
               test_nothing_published_does_not_break_the_response,
               test_the_flags_and_logs_ride_along):
        print("%-52s %s" % (fn.__name__, fn()))
    print("OK")
