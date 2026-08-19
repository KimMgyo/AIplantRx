"""Three boards, one pair of endpoints: which image each of them is handed.

    cd server && python tests/test_firmware_roles.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives. pytest collects this file unchanged.

WHY THIS FILE EXISTS. /v1/firmware/latest and /v1/firmware/image described one file
for as long as one board could install anything. The ESP32-CAM and the sensor node
pull their own images now, and the failure that adds is not "the wrong version" -
it is the wrong hardware. A 7" RGB panel application is a perfectly valid ESP32
image: a CAM handed one writes it, verifies its md5, boots it, finds no PSRAM
camera where it expects one, and goes dark inside a housing with no serial console.
Nothing raises anywhere. The download succeeds, the hash matches, the board is
simply gone, and the only symptom is a stream that stopped.

So the assertions below are about images that must not be served:

  A. No ?role= at all still means the panel, at the same path on disk it has
     always used - fwpull.cpp on every panel already in a greenhouse builds that
     URL with no query, and those are the boards that cannot be told otherwise.
  B. Each role is served its own bytes and never another role's.
  C. An unknown role is refused at the edge instead of falling back to the panel.
  D. A role with nothing published is an ordinary 404 while the others keep
     working: a deployment that has never published a cam image is correctly
     configured, not broken.
  E. A file that is not an ESP32 application is refused for a node exactly as it
     already was for the panel, and left on disk for whoever put it there.
  F. Content-Length matches the manifest's size, because Update.begin() commits to
     an OTA partition write before the first byte arrives.
  G. The md5 cache is keyed per file. One slot for three images would hand the
     cam's manifest the panel's hash, and a board that has already erased its OTA
     partition would abort mid-write on the mismatch.
"""
import hashlib
import os
import struct
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "fwroles.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is one test here, not the fixture

from fastapi.testclient import TestClient  # noqa: E402

from app import firmware, main, store  # noqa: E402

CLIENT = TestClient(main.app)

# A minimal image that firmware._app_desc accepts, built from the offsets that module
# documents rather than from a copy of a real firmware.bin. Same builder as
# tests/test_firmware_pull.py and for the same reason: these two files are the only places in
# the repo that write the descriptor instead of reading it, so an offset that drifts fails here
# as well as there.
_MAGIC = 0xABCD5432


def _image(elf_sha: bytes, idf: bytes = b"v5.3.2", pad: int = 4096) -> bytes:
    assert len(elf_sha) == 32
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0x00, _MAGIC)
    desc[0x70:0x70 + len(idf)] = idf
    desc[0x90:0x90 + 32] = elf_sha
    return b"\x00" * 0x20 + bytes(desc) + b"\xa5" * pad


# One image per role, each a different hash AND a different length. The length is not decoration:
# a Content-Length or a size read off the wrong role's manifest is invisible when all three
# images are the same size, and serving one role's bytes under another's length is precisely
# what this endpoint pair can now get wrong.
SHA = {role: bytes([i + 1]) * 32 for i, role in enumerate(firmware.ROLES)}
IMG = {role: _image(SHA[role], pad=4096 + 512 * i) for i, role in enumerate(firmware.ROLES)}

# Every md5 firmware.py computes, counted. The wrapper delegates to the real hashlib, so this
# changes nothing about what is hashed - it only makes "did the cache serve this, or did we read
# 2.5MB again" an assertion instead of a belief.
_HASHES = {"n": 0}
_real_md5 = hashlib.md5


def _counting_md5(*a, **kw):
    _HASHES["n"] += 1
    return _real_md5(*a, **kw)


firmware.hashlib = types.SimpleNamespace(md5=_counting_md5)


def _stage(**state) -> None:
    """Put all three roles into a known state: their own image, arbitrary bytes, or absent.

    All three every time, and never just the one under test. The images live in one directory
    beside the database, tests/test_firmware_pull.py empties that directory as its own fixture,
    and under pytest both files share a process - stating the whole world here is what makes
    these tests independent of whatever ran before them.

    The cache is cleared because it is keyed on (mtime_ns, size): rewriting a path with
    same-length bytes inside one filesystem tick is something a test does and a greenhouse does
    not, and a stale hit would be this fixture's bug rather than the code's.
    """
    for role in firmware.ROLES:
        path = firmware.image_path(role)
        path.parent.mkdir(parents=True, exist_ok=True)
        want = state.get(role)
        if want is True:
            path.write_bytes(IMG[role])
        elif isinstance(want, (bytes, bytearray)):
            path.write_bytes(bytes(want))
        elif path.exists():
            path.unlink()
    firmware._md5_cache.clear()


def test_no_role_at_all_is_still_the_panel():
    """The compatibility case, and the only one here that cannot be corrected later.

    fwpull.cpp on the panels already installed builds this URL with no query at all and reads
    the answer as their own image. If the default moved, or the panel's file moved off
    firmware.bin, every one of those boards would poll a 404 forever - and the only channel for
    telling them otherwise is the update they can no longer fetch.
    """
    _stage(panel=True, cam=True, node=True)
    frozen = store.db_path().parent / "firmware" / "firmware.bin"
    assert firmware.image_path() == frozen, firmware.image_path()
    assert firmware.image_path("panel") == frozen, firmware.image_path("panel")

    m = CLIENT.get("/v1/firmware/latest")
    assert m.status_code == 200, m.text
    assert m.json()["elf_sha256"] == SHA["panel"].hex(), m.json()
    r = CLIENT.get("/v1/firmware/image")
    assert r.status_code == 200, r.text
    assert r.content == IMG["panel"], "a bare request was served %d bytes" % len(r.content)
    return "no ?role= -> %s, %d bytes" % (frozen.name, len(r.content))


def test_each_role_is_served_its_own_image():
    """The whole point of the query parameter, asserted on bytes and not on status codes."""
    _stage(panel=True, cam=True, node=True)
    for role in firmware.ROLES:
        m = CLIENT.get("/v1/firmware/latest", params={"role": role})
        assert m.status_code == 200, (role, m.text)
        assert m.json()["elf_sha256"] == SHA[role].hex(), (role, m.json())
        assert m.json()["size"] == len(IMG[role]), (role, m.json())
        r = CLIENT.get("/v1/firmware/image", params={"role": role})
        assert r.status_code == 200, (role, r.text)
        assert r.content == IMG[role], (
            "%s was served %d bytes, its own image is %d" % (role, len(r.content), len(IMG[role])))
    return "panel/cam/node -> %s" % "/".join(str(len(IMG[r])) for r in firmware.ROLES)


def test_an_unknown_role_is_refused_and_not_defaulted():
    """A typo must cost a 400, never an image.

    Falling back to the default would mean a request for a role this server has never heard of
    is answered with the panel's application - the exact brick this file's header describes. The
    empty string is in the list because `?role=` is what a firmware building the URL from an
    unset config produces, and `../panel` because the role indexes a filename table and a caller
    is entitled to try.
    """
    _stage(panel=True, cam=True, node=True)
    for bad in ("cam2", "", "PANEL", "sensor", "../panel", "panel "):
        for path in ("/v1/firmware/latest", "/v1/firmware/image"):
            r = CLIENT.get(path, params={"role": bad})
            assert r.status_code == 400, (path, bad, r.status_code, r.text)
            assert IMG["panel"] not in r.content, (path, bad)
    return "6 unrecognised roles refused on both endpoints, none served the panel's image"


def test_a_role_with_nothing_published_is_an_ordinary_404():
    """Nothing published for a role is an answer, not a fault.

    Most deployments will only ever publish a panel image. Both endpoints have to say "nothing
    here" for the other two roles indefinitely, in the same shape the panel has always handled,
    and the roles that do have an image must keep working in the same breath - otherwise the
    first cam poll on a normal server reads as an outage.
    """
    _stage(panel=True)
    for role in ("cam", "node"):
        for path in ("/v1/firmware/latest", "/v1/firmware/image"):
            r = CLIENT.get(path, params={"role": role})
            assert r.status_code == 404, (path, role, r.status_code, r.text)
            assert r.json()["detail"] == "no firmware published", (path, role, r.json())
    ok = CLIENT.get("/v1/firmware/latest", params={"role": "panel"})
    assert ok.status_code == 200, ok.text
    return "cam and node 404 while the panel is still served"


def test_a_file_that_is_not_an_esp32_image_is_refused():
    """The descriptor check applies to every role, and the bad file stays where it is.

    An operator who scp's the wrong thing gets a 404 and a log line naming the path, which is
    the same treatment the panel's image has always had. Deleting it would be worse than
    useless: the file is the evidence of what went wrong, and the endpoint is not the thing that
    put it there.
    """
    junk = b"<!DOCTYPE html>\n<title>404 Not Found</title>\n" * 64
    _stage(panel=True, node=junk)
    for path in ("/v1/firmware/latest", "/v1/firmware/image"):
        r = CLIENT.get(path, params={"role": "node"})
        assert r.status_code == 404, (path, r.status_code, r.text)
        assert b"DOCTYPE" not in r.content, path
    assert firmware.image_path("node").read_bytes() == junk, "the refused file was disturbed"
    return "a non-ESP32 node image is refused on both endpoints and left on disk"


def test_content_length_matches_the_manifest_size():
    """Update.begin() is handed a length before the first byte arrives.

    The ESP32 commits to an OTA partition write up front, so the device reads `size` from the
    manifest and then trusts the transfer to be that long. A response whose Content-Length
    disagrees with the manifest it was chosen from is an update that fails after erasing the
    inactive partition, which is the most expensive moment to fail at.
    """
    _stage(panel=True, cam=True, node=True)
    for role in firmware.ROLES:
        m = CLIENT.get("/v1/firmware/latest", params={"role": role}).json()
        r = CLIENT.get("/v1/firmware/image", params={"role": role})
        assert r.headers["content-length"] == str(m["size"]), (role, r.headers, m)
        assert len(r.content) == m["size"], (role, len(r.content), m["size"])
    return "content-length == manifest size for all three roles"


def test_the_md5_cache_holds_one_entry_per_role():
    """Three images, three cached hashes, and no rehash on the second ask.

    The cache was a single slot when there was a single image, and the comment above it is about
    the property that has to survive: a second caller on a cold cache waits on the lock and
    reads the answer rather than hashing the same megabytes in parallel. Keying it per path is
    what stops three roles from evicting each other on a server polled by three boards a minute
    apart - and, worse, from ever answering with a hash computed over a different file.
    """
    _stage(panel=True, cam=True, node=True)
    base = _HASHES["n"]
    cold = {}
    for role in firmware.ROLES:
        cold[role] = CLIENT.get("/v1/firmware/latest", params={"role": role}).json()["md5"]
    hashed = _HASHES["n"] - base
    assert hashed == 3, "three cold manifests hashed %d files" % hashed

    warm = {}
    for role in firmware.ROLES:
        warm[role] = CLIENT.get("/v1/firmware/latest", params={"role": role}).json()["md5"]
    assert _HASHES["n"] - base == 3, (
        "a warm manifest re-hashed: %d hashes for six requests" % (_HASHES["n"] - base))

    assert warm == cold, (cold, warm)
    assert len(set(cold.values())) == 3, cold
    for role in firmware.ROLES:
        assert cold[role] == _real_md5(IMG[role]).hexdigest(), (role, cold[role])
    assert len(firmware._md5_cache) == 3, firmware._md5_cache
    return "3 files hashed once each, 6 manifests served"


def test_every_firmware_route_needs_the_bearer():
    """Both endpoints, with and without a role, are behind the same shared secret.

    DEVICE_TOKEN is read per request but lives as a module global, and pytest imports every test
    file in this directory into one process - several of which pop PLANTRX_TOKEN precisely so
    they can poll unauthenticated. So it is set for the length of this test and put back in a
    finally: leaving it on would turn their polls into 401s and blame them for it.
    """
    _stage(panel=True, cam=True, node=True)
    was = main.DEVICE_TOKEN
    main.DEVICE_TOKEN = "test-token"
    good = {"Authorization": "Bearer test-token"}
    try:
        for path in ("/v1/firmware/latest", "/v1/firmware/image"):
            assert CLIENT.get(path).status_code == 401, path
            assert CLIENT.get(path, params={"role": "cam"}).status_code == 401, path
            bad = CLIENT.get(path, headers={"Authorization": "Bearer wrong"})
            assert bad.status_code == 401, (path, bad.status_code)
            assert IMG["panel"] not in bad.content, path
            ok = CLIENT.get(path, params={"role": "cam"}, headers=good)
            assert ok.status_code == 200, (path, ok.text)
    finally:
        main.DEVICE_TOKEN = was
    return "both endpoints 401 without the bearer, 200 with it"


if __name__ == "__main__":
    print("default   %s" % test_no_role_at_all_is_still_the_panel())
    print("per-role  %s" % test_each_role_is_served_its_own_image())
    print("unknown   %s" % test_an_unknown_role_is_refused_and_not_defaulted())
    print("empty     %s" % test_a_role_with_nothing_published_is_an_ordinary_404())
    print("notesp32  %s" % test_a_file_that_is_not_an_esp32_image_is_refused())
    print("length    %s" % test_content_length_matches_the_manifest_size())
    print("md5       %s" % test_the_md5_cache_holds_one_entry_per_role())
    print("auth      %s" % test_every_firmware_route_needs_the_bearer())
    print("OK")
