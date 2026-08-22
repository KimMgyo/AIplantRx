"""The version in the manifest: where it comes from, and when it must be absent.

    cd server && python tests/test_firmware_version.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives: the image installs requirements.txt and a runner in there would ship to
production for nothing. pytest collects this file unchanged.

WHY THIS FILE EXISTS. A board decides whether to install by comparing elf_sha256, and
a hash has no order - it can only say "different". Both directions of that have cost
this project something real. A rebuild of byte-identical sources relinks and so
changes elf_sha256, which had a sensor node downloading 1,085,584 bytes to reinstall
the code it was already running (v0.4.2 -> v0.4.3, empty `git diff`, 68 bytes
different). A board flashed over USB with unreleased work differs from the published
image in exactly the same undirected way, which had the panel's update button
replacing that work with an older release - three times. The version is what turns
"different" into "later", and the release tag is the only version this system has,
because nothing in the image carries one (esp_app_desc.version reads '76b7a3f' in
every build this repo has ever produced).

So what is asserted here is not that a string travels. It is the four ways this can
be worse than the hash comparison it is layered over:

  A. A TAG THAT IS NOT REPORTED is a board still downloading its own code back.
     Recorded on install AND on "already installed", because a server that restarts
     holding the newest release's bytes replaces nothing, and a version only written
     by a replace would never reach those boards at all.
  B. A TAG REPORTED FOR THE WRONG BYTES is the dangerous one, and it is silent. A
     record left over from an image that has since been replaced would tell a board
     running v0.4.3 that the v0.4.2 image on this server is v0.4.3 - i.e. that it is
     current - and no later release could talk it out of that. Every record therefore
     carries the elf_sha256 of the image it was written for, and one that does not
     match the image being described is worth nothing.
  C. AN INVENTED TAG for an image an operator scp'd in by hand. It belongs to no
     release. "" is the honest answer and it is what puts the device back on
     comparing hashes, which is exactly what it did before this field existed.
  D. A VERSION THAT DISTURBS THE FIELDS THE INSTALL ITSELF DEPENDS ON. size goes to
     Update.begin() and md5 to Update.setMD5(); this is additive ordering, so those
     have to come out of manifest() unchanged whether a tag is recorded or not.

GitHub is never called. The pull tests hand ghfw a fake httpx client, because what is
under test is which tag ends up beside which bytes, not the third party.
"""
import hashlib
import json
import os
import struct
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "fwver.db")

from app import firmware, ghfw  # noqa: E402
from app.schema import FirmwareManifest  # noqa: E402

# The same descriptor builder as tests/test_firmware_pull.py and tests/test_firmware_roles.py,
# written out rather than imported: those two modules set PLANTRX_DB to temp directories of
# their own at import time, and importing one of them here would move this file's firmware
# directory out from under it.
_MAGIC = 0xABCD5432


def _image(elf_sha: bytes, idf: bytes = b"v5.3.2", pad: int = 4096) -> bytes:
    assert len(elf_sha) == 32
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0x00, _MAGIC)
    desc[0x70:0x70 + len(idf)] = idf
    desc[0x90:0x90 + 32] = elf_sha
    return b"\x00" * 0x20 + bytes(desc) + b"\xa5" * pad


NAME = {r: firmware.image_path(r).name for r in firmware.ROLES}
SHA = {
    "panel": bytes(range(0, 32)),
    "cam": bytes(range(32, 64)),
    "node": bytes(range(64, 96)),
}
IMG = {r: _image(SHA[r]) for r in firmware.ROLES}

# A second panel image, for every case where the bytes moved and a record did not.
SHA_OTHER = bytes(range(96, 128))
IMG_OTHER = _image(SHA_OTHER, pad=8192)


def _reset(published: dict | None = None) -> None:
    """Nothing published and nothing recorded, then whatever this test wants published.

    Both directories are emptied, because a record from an earlier test is exactly the
    stale record case B is about and it must be introduced deliberately, never inherited.
    """
    ghfw._etag = None
    firmware._md5_cache.clear()
    for d in (firmware.image_path("panel").parent, firmware.version_path("panel").parent):
        d.mkdir(parents=True, exist_ok=True)
        for stray in d.iterdir():
            stray.unlink()
    for role, body in (published or {}).items():
        firmware.image_path(role).write_bytes(body)


def _recorded(role: str) -> dict | None:
    """What is on disk for this role, parsed, or None when nothing is recorded."""
    p = firmware.version_path(role)
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else None


class _Resp:
    """Enough of httpx.Response for the two calls ghfw makes."""

    def __init__(self, status=200, body_json=None, body=b""):
        self.status_code = status
        self._json = body_json
        self._body = body
        self.headers = {}

    def raise_for_status(self):
        if self.status_code >= 400:
            raise RuntimeError("HTTP %d" % self.status_code)

    def json(self):
        return self._json

    def iter_bytes(self, size):
        for i in range(0, len(self._body), size):
            yield self._body[i:i + size]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _Client:
    """One release, its assets, and a note of which bodies were actually fetched."""

    def __init__(self, release, bodies):
        self.release = release
        self.bodies = bodies
        self.streamed = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def get(self, url, headers=None):
        return _Resp(200, body_json=self.release)

    def stream(self, method, url, headers=None):
        name = url.rsplit("/", 1)[-1]
        self.streamed.append(name)
        return _Resp(200, body=self.bodies[name])


def _scene(tag, images=None, digests=False):
    """A release under `tag` carrying `images`, optionally advertising their digests.

    `digests` is the switch between ghfw's two "already published" paths: with them the
    agreement is reached from the release JSON alone and no body is fetched at all, and
    without them it is reached by comparing the downloaded descriptor. Both must record
    the tag, so both need to be reachable from here.
    """
    if images is None:
        images = {NAME[r]: IMG[r] for r in firmware.ROLES}
    assets = []
    for name, body in images.items():
        a = {
            "name": name,
            "size": len(body),
            "browser_download_url": "https://example.invalid/%s" % name,
        }
        if digests:
            a["digest"] = "sha256:" + hashlib.sha256(body).hexdigest()
        assets.append(a)
    return _Client({"tag_name": tag, "assets": assets}, images)


def _run(client):
    """One pull against a fake client, with ghfw's httpx swapped out around it."""
    keep = ghfw.httpx
    ghfw.httpx = types.SimpleNamespace(Client=lambda **kw: client)
    try:
        return ghfw.pull_once()
    finally:
        ghfw.httpx = keep


def test_a_recorded_tag_reaches_the_manifest():
    """Without this the whole exercise is decoration: the board still gets no ordering.

    A tag that is recorded and not reported leaves every device comparing hashes, which
    is the reinstall-your-own-code bug still fully present.
    """
    _reset({"panel": IMG["panel"]})
    firmware.record_version("panel", "v0.4.3")
    m = firmware.manifest("panel")
    assert m is not None, "a published image did not describe itself"
    assert m["version"] == "v0.4.3", m["version"]
    assert m["elf_sha256"] == SHA["panel"].hex(), m["elf_sha256"]
    # And the record names the bytes it was written for, which is what makes it refusable
    # later rather than believed forever.
    assert _recorded("panel") == {"tag": "v0.4.3", "elf_sha256": SHA["panel"].hex()}, \
        _recorded("panel")
    return "a recorded tag is reported as version=v0.4.3"


def test_an_untagged_image_reports_an_empty_version():
    """C: an image an operator copied in belongs to no release, and gets no version.

    Inventing one here - the newest release's tag, or a mtime dressed up as a version -
    would tell a board that a hand-built image is a particular release, and the board
    would then refuse the real one for being no newer.
    """
    _reset({"panel": IMG["panel"]})
    m = firmware.manifest("panel")
    assert m is not None, "a published image did not describe itself"
    assert "version" in m, "the key must always be there; only its value can be empty"
    assert m["version"] == "", m["version"]
    assert _recorded("panel") is None, "a version was written for an image nobody tagged"
    return "an image with no record reports version=\"\" and nothing is invented"


def test_a_record_that_outlives_its_image_produces_no_manifest():
    """A tag on its own is not a publication, and must not read as one.

    An image deleted (or never restored with the rest of a volume) while its record
    stays behind is the case that would otherwise answer /v1/firmware/latest with a
    version and no bytes to go with it.
    """
    _reset({"panel": IMG["panel"]})
    firmware.record_version("panel", "v0.4.3")
    firmware.image_path("panel").unlink()
    assert firmware.manifest("panel") is None, "a record alone produced a manifest"
    assert _recorded("panel") is not None, "the record was expected to still be there"
    # And recording again with no image writes nothing, so the tag cannot resurrect itself
    # into a manifest on the next poll either.
    firmware.record_version("panel", "v0.4.4")
    assert _recorded("panel")["tag"] == "v0.4.3", _recorded("panel")
    return "a record with no image is not a manifest and cannot become one"


def test_a_record_left_over_from_other_bytes_is_ignored():
    """B: the silent one. A stale tag is worse than no tag at all.

    v0.4.3 recorded over v0.4.2's bytes tells a board running v0.4.3 that this server
    holds v0.4.3, so it installs nothing - permanently, because no later release changes
    what that record says. The record naming its own elf_sha256 is what makes this
    unrepresentable instead of unlikely.
    """
    _reset({"panel": IMG["panel"]})
    firmware.record_version("panel", "v0.4.3")
    # An operator's scp, or a crash between the replace and the record.
    firmware.image_path("panel").write_bytes(IMG_OTHER)
    firmware._md5_cache.clear()
    m = firmware.manifest("panel")
    assert m is not None and m["elf_sha256"] == SHA_OTHER.hex(), m
    assert m["version"] == "", "a stale record was believed: %r" % m["version"]
    return "a record whose elf_sha256 is not the image's is worth nothing"


def test_an_unparseable_record_falls_back_to_hashes():
    """A torn or hand-edited record must not break the manifest a board is waiting on.

    manifest() returning None here would read to every device as "nothing published"
    while a perfectly good image sat on disk, which is a worse outage than the missing
    ordering it is standing in for.
    """
    _reset({"panel": IMG["panel"]})
    firmware.version_path("panel").write_text('{"tag": "v0.4', encoding="utf-8")
    m = firmware.manifest("panel")
    assert m is not None, "a corrupt record unpublished a good image"
    assert m["version"] == "", m["version"]
    # A record of the wrong shape entirely is the same answer, not a TypeError on the way
    # out of a response model.
    firmware.version_path("panel").write_text('["v0.4.3"]', encoding="utf-8")
    assert firmware.manifest("panel")["version"] == "", "a list was read as a version"
    return "an unreadable record is \"\", not a 500 and not a missing image"


def test_the_version_does_not_disturb_the_other_fields():
    """D: this is additive ordering. Everything the install depends on stays put.

    size is handed to Update.begin() before the first byte arrives and md5 to
    Update.setMD5(), so a version that shifted either would turn a working update into a
    failed flash and three boots back to the old slot.
    """
    _reset({"panel": IMG["panel"]})
    without = firmware.manifest("panel")
    firmware.record_version("panel", "v0.4.3")
    with_tag = firmware.manifest("panel")
    assert without is not None and with_tag is not None
    for k in ("elf_sha256", "size", "md5", "idf_ver", "mtime"):
        assert without[k] == with_tag[k], "%s changed when a tag was recorded" % k
    assert without["version"] == "" and with_tag["version"] == "v0.4.3"
    # Against the bytes rather than against itself, so this also fails if recording a
    # version ever rewrote the image.
    assert with_tag["size"] == len(IMG["panel"]), with_tag["size"]
    assert with_tag["md5"] == hashlib.md5(IMG["panel"]).hexdigest(), with_tag["md5"]
    assert with_tag["elf_sha256"] == SHA["panel"].hex(), with_tag["elf_sha256"]
    assert with_tag["idf_ver"] == "v5.3.2", with_tag["idf_ver"]
    return "elf_sha256/size/md5/idf_ver/mtime are identical with and without a version"


def test_the_manifest_survives_its_response_model():
    """The key the three firmwares parse is spelled once, and it is spelled here.

    A field the boards read as "version" and a server that emits "fw_version" is two
    halves of a feature that never meets, and it would present as every board silently
    falling back to hashes - i.e. as this change having done nothing.
    """
    _reset({"panel": IMG["panel"]})
    firmware.record_version("panel", "v0.4.3")
    body = json.loads(FirmwareManifest(**firmware.manifest("panel")).model_dump_json())
    assert body["version"] == "v0.4.3", body
    assert isinstance(body["version"], str), type(body["version"])
    _reset({"cam": IMG["cam"]})
    empty = json.loads(FirmwareManifest(**firmware.manifest("cam")).model_dump_json())
    assert "version" in empty and empty["version"] == "", empty
    return "the response body carries version, \"\" included and never omitted"


def test_a_pull_records_the_release_tag():
    """A: the tag arrives with the bytes, for every role, on a fresh volume.

    This is the path a new deployment takes, and a board that installs a release and is
    then told the server's image has no version would compare hashes forever.
    """
    _reset(None)
    reason = _run(_scene("v1.2.3"))
    assert reason.startswith("v1.2.3: "), reason
    for r in firmware.ROLES:
        m = firmware.manifest(r)
        assert m is not None, r
        assert m["version"] == "v1.2.3", (r, m["version"])
        assert m["elf_sha256"] == SHA[r].hex(), (r, m["elf_sha256"])
    return "one release publishes three images and three versions"


def test_a_retag_of_identical_bytes_still_records():
    """A's second half, and the case a replace-only record would get wrong.

    Both of ghfw's agreement paths are exercised: the digest match, which fetches no
    body at all, and the descriptor match, which fetches one and then declines to write
    it. A server that comes up already holding the newest release's bytes takes exactly
    these paths, so if they recorded nothing those boards would have no ordering until
    somebody happened to cut another release.
    """
    _reset({r: IMG[r] for r in firmware.ROLES})
    before = {r: firmware.image_path(r).stat().st_mtime_ns for r in firmware.ROLES}

    cheap = _scene("v1.2.4", digests=True)
    assert "already " in _run(cheap), "setup: the bytes were supposed to agree"
    assert cheap.streamed == [], "an agreeing digest downloaded %s" % cheap.streamed
    for r in firmware.ROLES:
        assert firmware.manifest(r)["version"] == "v1.2.4", r

    dear = _scene("v1.2.5")
    assert "already " in _run(dear), "setup: the bytes were supposed to agree"
    assert sorted(dear.streamed) == sorted(NAME.values()), dear.streamed
    for r in firmware.ROLES:
        assert firmware.manifest(r)["version"] == "v1.2.5", r
        assert firmware.image_path(r).stat().st_mtime_ns == before[r], \
            "%s was rewritten for a version" % r
    return "a re-tag moves the version without touching the image"


def test_a_refused_asset_records_no_version():
    """A tag is recorded for bytes that landed, and for nothing else.

    An asset that is not an ESP32 application is refused before the replace, so the
    image on disk is still the old one - and recording the new release's tag against it
    would be case B arriving through the front door.
    """
    _reset({"panel": IMG["panel"]})
    firmware.record_version("panel", "v1.0.0")
    junk = b"<!DOCTYPE html><title>Not Found</title>" * 100
    reason = _run(_scene("v1.3.0", images={NAME["panel"]: junk}))
    assert "not an ESP32 application image" in reason, reason
    m = firmware.manifest("panel")
    assert m is not None and m["elf_sha256"] == SHA["panel"].hex(), "junk replaced the image"
    assert m["version"] == "v1.0.0", "a refused release moved the version: %r" % m["version"]
    return "a refused asset leaves both the image and its version alone"


if __name__ == "__main__":
    print("reported   %s" % test_a_recorded_tag_reaches_the_manifest())
    print("untagged   %s" % test_an_untagged_image_reports_an_empty_version())
    print("orphan     %s" % test_a_record_that_outlives_its_image_produces_no_manifest())
    print("stale      %s" % test_a_record_left_over_from_other_bytes_is_ignored())
    print("corrupt    %s" % test_an_unparseable_record_falls_back_to_hashes())
    print("additive   %s" % test_the_version_does_not_disturb_the_other_fields())
    print("wire       %s" % test_the_manifest_survives_its_response_model())
    print("pull       %s" % test_a_pull_records_the_release_tag())
    print("retag      %s" % test_a_retag_of_identical_bytes_still_records())
    print("refused    %s" % test_a_refused_asset_records_no_version())
    print("OK")
