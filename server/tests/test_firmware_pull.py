"""ghfw: which release becomes a published image, and which never does.

    cd server && python tests/test_firmware_pull.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives: the image installs requirements.txt and a runner in there would ship to
production for nothing. pytest collects this file unchanged.

GitHub is never called. Every test hands ghfw a fake httpx client, because what is
under test is the decision table - which bytes are allowed to replace a published
image - and not the third party.

WHY THIS FILE EXISTS AT ALL. A published image is the one thing this server hands a
board that the board then executes. firmware.py already refuses to *serve* something
that is not an ESP32 application; this module is the other end of the same duty, and
it is the end that writes. A puller that installs a 404 page, half a download, or last
week's bytes over this week's produces a board that boots three times, rolls back, and
tells somebody standing in a greenhouse that the update failed. None of these cases
raises anywhere: they are all a file that exists and has the wrong contents in it.

So the assertions below are about writes that must not happen:

  A. A release with no image for a role, and a 404, leave that role's published
     image alone.
  B. Bytes that are not an ESP32 application are refused before the replace.
  C. A body that does not match the asset's declared size is refused.
  D. A pull that ends in a refusal leaves no .part file behind, because a stray
     .part reads as a publish in progress to whoever looks next.
  E. The same image arriving again does not rewrite the file - the mtime is what
     firmware.py's md5 cache is keyed on, and re-publishing identical bytes would
     invalidate it for nothing.
  F. A failed download does not remember the ETag, so the next poll asks again
     instead of being told 304 about a release it never actually fetched.

And two properties that came with the third role:

  G. THREE ROLES, ONE RELEASE, ONE METADATA CALL - and one role's refusal does not
     stop the other two. The panel is the board somebody is standing in front of and
     it must not wait on a camera image nobody tagged.
  H. AN AGREEING DIGEST SPENDS NO BODY. The release JSON carries each asset's
     sha256, so "already published" is answerable without a download. This was
     affordable to get wrong at one image per poll behind a working ETag; at three
     it is megabytes an hour of no-op traffic.
"""
import hashlib
import os
import struct
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "fwpull.db")

from app import firmware, ghfw  # noqa: E402

# A minimal image that firmware._app_desc accepts, built from the offsets that module
# documents rather than from a copy of a real firmware.bin. Building it here means these
# tests also fail if those offsets ever drift: this is the only place in the repo that
# writes the descriptor rather than reading it.
_MAGIC = 0xABCD5432


def _image(elf_sha: bytes, idf: bytes = b"v5.3.2", pad: int = 4096) -> bytes:
    assert len(elf_sha) == 32
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0x00, _MAGIC)
    desc[0x70:0x70 + len(idf)] = idf
    desc[0x90:0x90 + 32] = elf_sha
    return b"\x00" * 0x20 + bytes(desc) + b"\xa5" * pad


# Read off firmware.py rather than spelled out, for the same reason ghfw reads them off
# it: the asset name and the published file name are one fact.
NAME = {r: firmware.image_path(r).name for r in firmware.ROLES}

SHA = {
    "panel": bytes(range(0, 32)),
    "cam": bytes(range(32, 64)),
    "node": bytes(range(64, 96)),
}
IMG = {r: _image(SHA[r]) for r in firmware.ROLES}
_ALL = {NAME[r]: IMG[r] for r in firmware.ROLES}

# A second panel image, for the "something new landed" cases.
SHA_NEW = bytes(range(96, 128))
IMG_NEW = _image(SHA_NEW)


def _digest(body: bytes) -> str:
    """The spelling GitHub's release JSON uses, which is what ghfw compares against."""
    return "sha256:" + hashlib.sha256(body).hexdigest()


class _Resp:
    """Enough of httpx.Response for the two calls ghfw makes."""

    def __init__(self, status=200, body_json=None, body=b"", etag=None):
        self.status_code = status
        self._json = body_json
        self._body = body
        self.headers = {"ETag": etag} if etag else {}

    def raise_for_status(self):
        if self.status_code >= 400:
            raise RuntimeError("HTTP %d" % self.status_code)

    def json(self):
        return self._json

    def iter_bytes(self, size):
        # Chunked deliberately: ghfw accumulates a length as it writes, and a
        # single-chunk fake would not exercise that arithmetic.
        for i in range(0, len(self._body), size):
            yield self._body[i:i + size]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _Client:
    """Records what was asked for, answers from a script.

    `streamed` is a list of asset names and not a count, because with three roles the
    interesting question stopped being "did it download" and became "which ones".
    """

    def __init__(self, release, bodies, etag=None, stream_error=None):
        self.release = release
        self.bodies = bodies
        self.etag = etag
        self.stream_error = stream_error
        self.get_headers = None
        self.gets = 0
        self.streamed = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def get(self, url, headers=None):
        self.gets += 1
        self.get_headers = dict(headers or {})
        if self.release is None:
            return _Resp(404, etag=self.etag)
        if self.release == "304":
            return _Resp(304, etag=self.etag)
        return _Resp(200, body_json=self.release, etag=self.etag)

    def stream(self, method, url, headers=None):
        name = url.rsplit("/", 1)[-1]
        self.streamed.append(name)
        if self.stream_error is not None:
            raise self.stream_error
        return _Resp(200, body=self.bodies[name])


def _scene(tag="v1.0.0", images=None, sizes=None, digests=None, etag=None,
           stream_error=None):
    """One release and a client that serves exactly its assets.

    The release JSON and the bytes come from the same dict on purpose: a fake that can
    advertise an asset it cannot serve tests the fake and not the puller.
    """
    if images is None:
        images = dict(_ALL)
    assets = []
    for name, body in images.items():
        a = {
            "name": name,
            "size": (sizes or {}).get(name, len(body)),
            "browser_download_url": "https://example.invalid/%s" % name,
        }
        d = (digests or {}).get(name)
        if d is not None:
            a["digest"] = d
        assets.append(a)
    return _Client({"tag_name": tag, "assets": assets}, images, etag=etag,
                   stream_error=stream_error)


def _run(client):
    """One pull against a fake client, with ghfw's module state reset around it."""
    keep = ghfw.httpx
    ghfw.httpx = types.SimpleNamespace(Client=lambda **kw: client)
    try:
        return ghfw.pull_once()
    finally:
        ghfw.httpx = keep


def _reset(published: dict | None = None):
    """Start from a known set of published images, or from nothing published at all.

    `published` maps role -> bytes. The directory is emptied either way, because a
    stray file from an earlier test reads as a publish this one did not make.
    """
    ghfw._etag = None
    firmware._md5_cache.clear()
    d = firmware.image_path("panel").parent
    d.mkdir(parents=True, exist_ok=True)
    for stray in d.iterdir():
        stray.unlink()
    for role, body in (published or {}).items():
        firmware.image_path(role).write_bytes(body)
    return d


def _dir_state(d):
    return sorted(p.name for p in d.iterdir())


def _all_published():
    return {r: IMG[r] for r in firmware.ROLES}


def test_no_release_leaves_the_images_alone():
    """A repo with nothing published is not a reason to unpublish what we have."""
    d = _reset(_all_published())
    before = {r: firmware.image_path(r).stat().st_mtime_ns for r in firmware.ROLES}
    reason = _run(_Client(None, {}))
    assert "no published release" in reason, reason
    for r in firmware.ROLES:
        p = firmware.image_path(r)
        assert p.read_bytes() == IMG[r], "%s was disturbed" % r
        assert p.stat().st_mtime_ns == before[r], "%s was rewritten" % r
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)
    return "404 leaves all three published images untouched"


def test_release_without_the_asset_is_not_a_publish():
    """A release that carries source archives and no images is not for us."""
    d = _reset(_all_published())
    reason = _run(_scene(images={}, etag='W/"none"'))
    for r in firmware.ROLES:
        assert "%s: no %s" % (r, NAME[r]) in reason, reason
        assert firmware.image_path(r).read_bytes() == IMG[r]
    # And it is not remembered: the workflow re-uploads assets with --clobber, so a
    # release that grows the image it was missing must not be behind a 304.
    assert ghfw._etag is None, "a release with no assets remembered the ETag"
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)
    return "a release without the assets publishes nothing and is re-asked"


def test_bytes_that_are_not_firmware_are_refused():
    """The check firmware.py makes when serving, made again before writing.

    This is the case that matters most: an asset named firmware.bin that is a 404
    page, an ELF, or a zip. It has a name and a size and nothing else wrong with it
    until a board boots it.
    """
    d = _reset(_all_published())
    junk = b"<!DOCTYPE html><title>Not Found</title>" * 100
    images = dict(_ALL)
    images[NAME["panel"]] = junk
    reason = _run(_scene(images=images))
    assert "panel: %s is not an ESP32 application image" % NAME["panel"] in reason, reason
    assert firmware.image_path("panel").read_bytes() == IMG["panel"], "junk replaced it"
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)
    return "a non-image asset is refused before the replace"


def test_a_short_body_is_refused():
    """A truncated download has a valid descriptor in its first 288 bytes."""
    d = _reset(_all_published())
    images = dict(_ALL)
    images[NAME["panel"]] = IMG_NEW
    short = {NAME["panel"]: len(IMG_NEW) + 4096}
    reason = _run(_scene(images=images, sizes=short))
    assert "panel: got %d of %d bytes" % (len(IMG_NEW), len(IMG_NEW) + 4096) in reason, reason
    assert firmware.image_path("panel").read_bytes() == IMG["panel"], "a short body landed"
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)
    return "a body short of the declared size is refused"


def test_a_new_image_is_published_and_described():
    """The happy path, checked through firmware.manifest() rather than the return value.

    manifest() is what a board actually reads, so it is what proves the pull landed:
    the elf_sha256 it reports has to come from the bytes that were just downloaded.
    """
    d = _reset(_all_published())
    images = dict(_ALL)
    images[NAME["panel"]] = IMG_NEW
    reason = _run(_scene(tag="v2.3.4", images=images))
    assert reason.startswith("v2.3.4: "), reason
    assert "panel: %s " % SHA_NEW.hex()[:12] in reason, reason
    assert firmware.image_path("panel").read_bytes() == IMG_NEW
    firmware._md5_cache.clear()
    m = firmware.manifest("panel")
    assert m is not None, "the pulled image does not describe itself"
    assert m["elf_sha256"] == SHA_NEW.hex(), m["elf_sha256"]
    assert m["size"] == len(IMG_NEW), m["size"]
    assert m["idf_ver"] == "v5.3.2", m["idf_ver"]
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)
    return "a new image lands and the manifest describes the pulled bytes"


def test_one_release_settles_all_three_roles():
    """G: one metadata call, three images, three distinct manifests.

    The distinctness is the assertion that matters. Serving one role's bytes under
    another's name writes a bootloader-valid, hardware-wrong application into the OTA
    slot of a board nobody can attach a cable to - which is the same failure
    main._role_q refuses at the edge, arriving from the publishing side instead.
    """
    d = _reset(None)
    c = _scene(tag="v3.0.0")
    reason = _run(c)
    assert c.gets == 1, "one release should cost one metadata call, not %d" % c.gets
    assert sorted(c.streamed) == sorted(NAME.values()), c.streamed
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)
    firmware._md5_cache.clear()
    for r in firmware.ROLES:
        assert "%s: %s " % (r, SHA[r].hex()[:12]) in reason, (r, reason)
        m = firmware.manifest(r)
        assert m is not None and m["elf_sha256"] == SHA[r].hex(), (r, m)
    return "one release publishes all three roles, each its own bytes"


def test_a_matching_digest_skips_the_download():
    """H: the release says what it holds, so agreement costs no body at all."""
    _reset(_all_published())
    digests = {NAME[r]: _digest(IMG[r]) for r in firmware.ROLES}
    c = _scene(digests=digests, etag='W/"same"')
    reason = _run(c)
    assert c.streamed == [], "an agreeing digest still downloaded %s" % c.streamed
    for r in firmware.ROLES:
        assert "%s: already " % r in reason, reason
    # Agreement is agreement: the next poll may be conditional.
    assert ghfw._etag == 'W/"same"', ghfw._etag
    return "a digest that matches the published bytes downloads nothing"


def test_a_disagreeing_digest_still_downloads():
    """The negative control for H: the short circuit must not swallow a real change."""
    _reset(_all_published())
    images = dict(_ALL)
    images[NAME["panel"]] = IMG_NEW
    digests = {NAME[r]: _digest(images[NAME[r]]) for r in firmware.ROLES}
    c = _scene(images=images, digests=digests)
    reason = _run(c)
    assert c.streamed == [NAME["panel"]], c.streamed
    assert firmware.image_path("panel").read_bytes() == IMG_NEW
    assert "cam: already " in reason and "node: already " in reason, reason
    return "only the role whose digest moved is fetched"


def test_one_role_refused_does_not_block_the_others():
    """G's second half. A camera image nobody tagged must not hold the panel back."""
    d = _reset(None)
    images = {NAME["panel"]: IMG["panel"], NAME["node"]: IMG["node"]}
    reason = _run(_scene(images=images, etag='W/"partial"'))
    assert "cam: no %s" % NAME["cam"] in reason, reason
    assert firmware.image_path("panel").read_bytes() == IMG["panel"]
    assert firmware.image_path("node").read_bytes() == IMG["node"]
    assert not firmware.image_path("cam").exists()
    assert ghfw._etag is None, "a partial release remembered the ETag"
    assert _dir_state(d) == sorted([NAME["panel"], NAME["node"]]), _dir_state(d)
    return "two roles land while the third is reported missing"


def test_a_first_publish_needs_no_existing_image():
    """A fresh deploy with an empty volume populates itself."""
    _reset(None)
    for r in firmware.ROLES:
        assert not firmware.image_path(r).exists()
    reason = _run(_scene(tag="v9.9.9"))
    assert reason.startswith("v9.9.9: "), reason
    assert reason.count("was nothing") == len(firmware.ROLES), reason
    for r in firmware.ROLES:
        assert firmware.image_path(r).read_bytes() == IMG[r]
    return "an empty data volume publishes every role from the newest release"


def test_the_same_image_is_not_rewritten():
    """E: identical bytes are a no-op, down to the mtime.

    firmware.py keys its md5 cache on (mtime_ns, size). Replacing a file with a copy
    of itself would invalidate that cache and re-hash megabytes on the next manifest,
    for a file whose contents nobody changed. Asserted with no digest on the release,
    so this is the descriptor comparison and not the short circuit above it.
    """
    d = _reset(_all_published())
    before = {r: firmware.image_path(r).stat().st_mtime_ns for r in firmware.ROLES}
    c = _scene(tag="v1.0.0")
    reason = _run(c)
    assert sorted(c.streamed) == sorted(NAME.values()), "the bytes were needed to decide"
    for r in firmware.ROLES:
        assert "%s: already %s" % (r, SHA[r].hex()[:12]) in reason, reason
        assert firmware.image_path(r).stat().st_mtime_ns == before[r], \
            "identical bytes rewrote %s" % r
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)
    return "re-pulling published images touches nothing"


def test_the_etag_short_circuits_a_second_poll():
    """Having agreed with a release once, the next poll is conditional."""
    _reset(None)
    first = _scene(etag='W/"abc123"')
    assert "was nothing" in _run(first), "setup failed"
    assert "If-None-Match" not in first.get_headers, first.get_headers

    second = _Client("304", {}, etag='W/"abc123"')
    reason = _run(second)
    assert reason == "unchanged", reason
    assert second.get_headers.get("If-None-Match") == 'W/"abc123"', second.get_headers
    assert second.streamed == [], "a 304 still downloaded an asset"
    return "a successful poll makes the next one conditional"


def test_a_failed_download_does_not_remember_the_etag():
    """F: the bug this ordering exists to prevent.

    Storing the ETag from the metadata call before the download succeeds would mean a
    transport failure is answered with 304 forever: the release would be skipped as
    "unchanged" while its bytes were never fetched. The board would sit on the old
    image and every log line would say the server was up to date.
    """
    d = _reset(_all_published())
    images = dict(_ALL)
    images[NAME["panel"]] = IMG_NEW
    boom = _scene(images=images, etag='W/"xyz789"',
                  stream_error=RuntimeError("connection reset"))
    try:
        _run(boom)
        raise AssertionError("a transport failure should propagate to the loop")
    except RuntimeError as exc:
        assert "connection reset" in str(exc), exc
    assert ghfw._etag is None, "a failed pull remembered the ETag"
    assert firmware.image_path("panel").read_bytes() == IMG["panel"]
    assert _dir_state(d) == sorted(NAME.values()), _dir_state(d)

    # And the retry is unconditional, which is the whole point.
    retry = _scene(images=images, etag='W/"xyz789"')
    reason = _run(retry)
    assert "If-None-Match" not in retry.get_headers, retry.get_headers
    assert "panel: %s " % SHA_NEW.hex()[:12] in reason, reason
    assert firmware.image_path("panel").read_bytes() == IMG_NEW
    return "a failed download is retried, not skipped as unchanged"


def test_the_loop_survives_a_failing_poll():
    """_loop logs and sleeps rather than dying, so one bad night is not permanent."""
    _reset(_all_published())
    calls = {"n": 0}

    def explode():
        calls["n"] += 1
        raise RuntimeError("nope")

    keep = (ghfw.pull_once, ghfw.time.sleep)
    ghfw.pull_once = explode
    ghfw.time.sleep = lambda _s: (_ for _ in ()).throw(KeyboardInterrupt)
    try:
        try:
            ghfw._loop(1)
        except KeyboardInterrupt:
            pass
    finally:
        ghfw.pull_once, ghfw.time.sleep = keep
    assert calls["n"] == 1, calls
    return "a raising pull_once is logged and retried, not fatal"


def test_the_poll_interval_has_a_floor():
    """Unauthenticated GitHub allows 60 requests an hour; a mistyped 5 would burn it."""
    assert ghfw.MIN_POLL_S >= 60, ghfw.MIN_POLL_S
    assert max(5, ghfw.MIN_POLL_S) == ghfw.MIN_POLL_S
    return "the poll interval cannot be set below %ds" % ghfw.MIN_POLL_S


def test_the_asset_names_are_firmware_pys_own():
    """The naming contract with the workflow, asserted where it can actually fail.

    ghfw reads asset names off firmware.image_path(), and .github/workflows/firmware.yml
    renames its build outputs to match. This pins the values so a change to _IMAGE_NAME
    fails a test here rather than presenting as a release whose assets nothing looks for.
    """
    assert [ghfw._asset_name(r) for r in firmware.ROLES] == \
        ["firmware.bin", "firmware-cam.bin", "firmware-node.bin"], \
        [ghfw._asset_name(r) for r in firmware.ROLES]
    return "asset names are firmware.py's file names, not a second spelling"


def _armed_start(poll_s: int):
    """Run start() with the thread stubbed, and hand back what it would have launched.

    Stubbed rather than real because start() is being called for its own sake here: a live
    poller would reach GitHub from a unit test, and the thing under test is the handful of lines
    that run BEFORE the thread does any work.
    """
    launched = {}

    class _Thread:
        def __init__(self, target=None, args=(), name=None, daemon=None):
            launched.update(target=target, args=args, name=name, daemon=daemon)

        def start(self):
            launched["started"] = True

    keep = (ghfw.threading.Thread, ghfw.POLL_S)
    ghfw.threading.Thread = _Thread
    ghfw.POLL_S = poll_s
    try:
        ghfw.start()
    finally:
        ghfw.threading.Thread, ghfw.POLL_S = keep
    return launched


def test_start_arms_the_poller_without_raising():
    """The line that took the whole server down, and why it needed a test of its own.

    start() was left holding a reference to a module constant that had been replaced by
    _asset_name(role) when the puller grew from one image to three. Nothing here executed it:
    every other test in this file calls pull_once() or _loop() directly, and the one that
    mentions POLL_S only reads MIN_POLL_S. So the suite was green, the deployment crash-looped
    on startup, and the symptom was a 502 with no server-side log anybody was watching.

    A NameError on a path no test walks is what a linter is for, and this project deliberately
    ships no dev dependencies in the image that serves the greenhouse. So the path gets walked.
    """
    launched = _armed_start(600)
    assert launched.get("started") is True, launched
    assert launched["target"] is ghfw._loop, launched["target"]
    assert launched["daemon"] is True, launched
    # The floor, applied where it is actually applied rather than only asserted about.
    assert _armed_start(5)["args"] == (ghfw.MIN_POLL_S,), "the interval floor was not applied"
    assert _armed_start(600)["args"] == (600,), "a legal interval was not passed through"
    return "start() launches _loop with a floored interval and no NameError"


def test_the_apps_startup_hook_runs_with_the_poller_enabled():
    """The same failure from the outside: whether uvicorn would have come up at all.

    main._startup() is what Dokploy exercises and what was broken. An exception in it is not a
    degraded feature, it is "Application startup failed. Exiting." and a container that never
    serves a byte - which is exactly what happened, with the only evidence a 502 nobody was
    watching a server-side log for.

    Called directly rather than through TestClient's lifespan. The context-manager form runs the
    same hook but keeps a portal thread and a scheduler alive for the length of the block, which
    turns a unit test into something that has to be torn down carefully; this is the one function
    that matters and it takes no arguments. POLL_S is above zero because the disabled branch
    returns before reaching anything interesting.
    """
    launched = {}

    class _Thread:
        def __init__(self, **kw):
            launched.update(kw)

        def start(self):
            launched["started"] = True

    from app import main

    keep = (ghfw.threading.Thread, ghfw.POLL_S)
    ghfw.threading.Thread = _Thread
    ghfw.POLL_S = 900
    try:
        main._startup()
    finally:
        ghfw.threading.Thread, ghfw.POLL_S = keep
    assert launched.get("started") is True, "startup completed without arming the poller"
    return "the app's startup hook runs with the poller enabled"


if __name__ == "__main__":
    for fn in (test_no_release_leaves_the_images_alone,
               test_release_without_the_asset_is_not_a_publish,
               test_bytes_that_are_not_firmware_are_refused,
               test_a_short_body_is_refused,
               test_a_new_image_is_published_and_described,
               test_one_release_settles_all_three_roles,
               test_a_matching_digest_skips_the_download,
               test_a_disagreeing_digest_still_downloads,
               test_one_role_refused_does_not_block_the_others,
               test_a_first_publish_needs_no_existing_image,
               test_the_same_image_is_not_rewritten,
               test_the_etag_short_circuits_a_second_poll,
               test_a_failed_download_does_not_remember_the_etag,
               test_the_loop_survives_a_failing_poll,
               test_the_poll_interval_has_a_floor,
               test_the_asset_names_are_firmware_pys_own,
               test_start_arms_the_poller_without_raising,
               test_the_apps_startup_hook_runs_with_the_poller_enabled):
        print("%-52s %s" % (fn.__name__, fn()))
    print("OK")
