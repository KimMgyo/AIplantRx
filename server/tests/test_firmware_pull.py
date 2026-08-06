"""ghfw: which release becomes the published image, and which never does.

    cd server && python tests/test_firmware_pull.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives: the image installs requirements.txt and a runner in there would ship to
production for nothing. pytest collects this file unchanged.

GitHub is never called. Every test hands ghfw a fake httpx client, because what is
under test is the decision table - which bytes are allowed to replace the published
image - and not the third party.

WHY THIS FILE EXISTS AT ALL. The published image is the one thing this server hands a
panel that the panel then executes. firmware.py already refuses to *serve* something
that is not an ESP32 application; this module is the other end of the same duty, and
it is the end that writes. A puller that installs a 404 page, half a download, or last
week's bytes over this week's produces a panel that boots three times, rolls back, and
tells somebody standing in a greenhouse that the update failed. None of these cases
raises anywhere: they are all a file that exists and has the wrong contents in it.

So the assertions below are about writes that must not happen:

  A. A release with no firmware.bin, and a 404, leave the published image alone.
  B. Bytes that are not an ESP32 application are refused before the replace.
  C. A body that does not match the asset's declared size is refused.
  D. A pull that ends in a refusal leaves no .part file behind, because a stray
     .part reads as a publish in progress to whoever looks next.
  E. The same image arriving again does not rewrite the file - the mtime is what
     firmware.py's md5 cache is keyed on, and re-publishing identical bytes would
     invalidate it for nothing.
  F. A failed download does not remember the ETag, so the next poll asks again
     instead of being told 304 about a release it never actually fetched.
"""
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


SHA_A = bytes(range(32))
SHA_B = bytes(range(32, 64))
IMG_A = _image(SHA_A)
IMG_B = _image(SHA_B)


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
    """Records what was asked for, answers from a script."""

    def __init__(self, release, asset_body, etag=None, stream_error=None):
        self.release = release
        self.asset_body = asset_body
        self.etag = etag
        self.stream_error = stream_error
        self.get_headers = None
        self.streamed = 0

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def get(self, url, headers=None):
        self.get_headers = dict(headers or {})
        if self.release is None:
            return _Resp(404, etag=self.etag)
        if self.release == "304":
            return _Resp(304, etag=self.etag)
        return _Resp(200, body_json=self.release, etag=self.etag)

    def stream(self, method, url, headers=None):
        self.streamed += 1
        if self.stream_error is not None:
            raise self.stream_error
        return _Resp(200, body=self.asset_body)


def _release(tag="v1.0.0", asset="firmware.bin", size=None, body=IMG_A):
    """A releases/latest payload with one asset on it."""
    assets = []
    if asset is not None:
        assets.append({
            "name": asset,
            "size": len(body) if size is None else size,
            "browser_download_url": "https://example.invalid/%s" % asset,
        })
    return {"tag_name": tag, "assets": assets}


def _run(client):
    """One pull against a fake client, with ghfw's module state reset around it."""
    keep = ghfw.httpx
    ghfw.httpx = types.SimpleNamespace(Client=lambda **kw: client)
    try:
        return ghfw.pull_once()
    finally:
        ghfw.httpx = keep


def _reset(published: bytes | None = None):
    """Start from a known published image, or from nothing published at all."""
    ghfw._etag = None
    firmware._md5_cache = None
    dest = firmware.image_path()
    dest.parent.mkdir(parents=True, exist_ok=True)
    for stray in dest.parent.iterdir():
        stray.unlink()
    if published is not None:
        dest.write_bytes(published)
    return dest


def _dir_state(dest):
    return sorted(p.name for p in dest.parent.iterdir())


def test_no_release_leaves_the_image_alone():
    """A repo with nothing published is not a reason to unpublish what we have."""
    dest = _reset(IMG_A)
    before = dest.stat().st_mtime_ns
    reason = _run(_Client(None, b""))
    assert "no published release" in reason, reason
    assert dest.read_bytes() == IMG_A, "the published image was disturbed"
    assert dest.stat().st_mtime_ns == before, "the file was rewritten"
    assert _dir_state(dest) == ["firmware.bin"], _dir_state(dest)
    return "404 leaves the published image untouched"


def test_release_without_the_asset_is_not_a_publish():
    """A release that carries source archives and no firmware.bin is not for us."""
    dest = _reset(IMG_A)
    reason = _run(_Client(_release(asset=None), b""))
    assert "carries no firmware.bin" in reason, reason
    assert dest.read_bytes() == IMG_A
    assert _dir_state(dest) == ["firmware.bin"], _dir_state(dest)
    return "a release without the asset publishes nothing"


def test_bytes_that_are_not_firmware_are_refused():
    """The check firmware.py makes when serving, made again before writing.

    This is the case that matters most: an asset named firmware.bin that is a 404
    page, an ELF, or a zip. It has a name and a size and nothing else wrong with it
    until a panel boots it.
    """
    dest = _reset(IMG_A)
    junk = b"<!DOCTYPE html><title>Not Found</title>" * 100
    reason = _run(_Client(_release(body=junk), junk))
    assert "not an ESP32 application image" in reason, reason
    assert dest.read_bytes() == IMG_A, "junk replaced the published image"
    assert _dir_state(dest) == ["firmware.bin"], _dir_state(dest)
    return "a non-image asset is refused before the replace"


def test_a_short_body_is_refused():
    """A truncated download has a valid descriptor in its first 288 bytes."""
    dest = _reset(IMG_A)
    # The release says the asset is larger than what the stream actually delivers.
    reason = _run(_Client(_release(body=IMG_B, size=len(IMG_B) + 4096), IMG_B))
    assert "got %d of %d bytes" % (len(IMG_B), len(IMG_B) + 4096) in reason, reason
    assert dest.read_bytes() == IMG_A, "a short body replaced the published image"
    assert _dir_state(dest) == ["firmware.bin"], _dir_state(dest)
    return "a body short of the declared size is refused"


def test_a_new_image_is_published_and_described():
    """The happy path, checked through firmware.manifest() rather than the return value.

    manifest() is what a panel actually reads, so it is what proves the pull landed:
    the elf_sha256 it reports has to come from the bytes that were just downloaded.
    """
    dest = _reset(IMG_A)
    reason = _run(_Client(_release(tag="v2.3.4", body=IMG_B), IMG_B))
    assert reason.startswith("published v2.3.4 "), reason
    assert dest.read_bytes() == IMG_B
    firmware._md5_cache = None
    m = firmware.manifest()
    assert m is not None, "the pulled image does not describe itself"
    assert m["elf_sha256"] == SHA_B.hex(), m["elf_sha256"]
    assert m["size"] == len(IMG_B), m["size"]
    assert m["idf_ver"] == "v5.3.2", m["idf_ver"]
    assert _dir_state(dest) == ["firmware.bin"], _dir_state(dest)
    return "a new image lands and the manifest describes the pulled bytes"


def test_a_first_publish_needs_no_existing_image():
    """A fresh deploy with an empty volume populates itself."""
    dest = _reset(None)
    assert not dest.exists()
    reason = _run(_Client(_release(tag="v9.9.9", body=IMG_A), IMG_A))
    assert reason.startswith("published v9.9.9 "), reason
    assert "was nothing" in reason, reason
    assert dest.read_bytes() == IMG_A
    return "an empty data volume publishes the newest release"


def test_the_same_image_is_not_rewritten():
    """Identical bytes are a no-op, down to the mtime.

    firmware.py keys its md5 cache on (mtime_ns, size). Replacing a file with a copy
    of itself would invalidate that cache and re-hash 3.5MB on the next manifest, for
    a file whose contents nobody changed.
    """
    dest = _reset(IMG_A)
    before = dest.stat().st_mtime_ns
    reason = _run(_Client(_release(tag="v1.0.0", body=IMG_A), IMG_A))
    assert reason.startswith("already published "), reason
    assert SHA_A.hex()[:12] in reason, reason
    assert dest.stat().st_mtime_ns == before, "identical bytes rewrote the file"
    assert _dir_state(dest) == ["firmware.bin"], _dir_state(dest)
    return "re-pulling the published image touches nothing"


def test_the_etag_short_circuits_a_second_poll():
    """Having agreed with a release once, the next poll is conditional."""
    _reset(None)
    first = _Client(_release(body=IMG_A), IMG_A, etag='W/"abc123"')
    assert _run(first).startswith("published "), "setup failed"
    assert "If-None-Match" not in first.get_headers, first.get_headers

    second = _Client("304", b"", etag='W/"abc123"')
    reason = _run(second)
    assert reason == "unchanged", reason
    assert second.get_headers.get("If-None-Match") == 'W/"abc123"', second.get_headers
    assert second.streamed == 0, "a 304 still downloaded the asset"
    return "a successful poll makes the next one conditional"


def test_a_failed_download_does_not_remember_the_etag():
    """The bug this ordering exists to prevent.

    Storing the ETag from the metadata call before the download succeeds would mean a
    transport failure is answered with 304 forever: the release would be skipped as
    "unchanged" while its bytes were never fetched. The panel would sit on the old
    image and every log line would say the server was up to date.
    """
    dest = _reset(IMG_A)
    boom = _Client(_release(body=IMG_B), IMG_B, etag='W/"xyz789"',
                   stream_error=RuntimeError("connection reset"))
    try:
        _run(boom)
        raise AssertionError("a transport failure should propagate to the loop")
    except RuntimeError as exc:
        assert "connection reset" in str(exc), exc
    assert ghfw._etag is None, "a failed pull remembered the ETag"
    assert dest.read_bytes() == IMG_A
    assert _dir_state(dest) == ["firmware.bin"], _dir_state(dest)

    # And the retry is unconditional, which is the whole point.
    retry = _Client(_release(body=IMG_B), IMG_B, etag='W/"xyz789"')
    reason = _run(retry)
    assert "If-None-Match" not in retry.get_headers, retry.get_headers
    assert reason.startswith("published "), reason
    assert dest.read_bytes() == IMG_B
    return "a failed download is retried, not skipped as unchanged"


def test_the_loop_survives_a_failing_poll():
    """_loop logs and sleeps rather than dying, so one bad night is not permanent."""
    _reset(IMG_A)
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


if __name__ == "__main__":
    for fn in (test_no_release_leaves_the_image_alone,
               test_release_without_the_asset_is_not_a_publish,
               test_bytes_that_are_not_firmware_are_refused,
               test_a_short_body_is_refused,
               test_a_new_image_is_published_and_described,
               test_a_first_publish_needs_no_existing_image,
               test_the_same_image_is_not_rewritten,
               test_the_etag_short_circuits_a_second_poll,
               test_a_failed_download_does_not_remember_the_etag,
               test_the_loop_survives_a_failing_poll,
               test_the_poll_interval_has_a_floor):
        print("%-52s %s" % (fn.__name__, fn()))
    print("OK")
