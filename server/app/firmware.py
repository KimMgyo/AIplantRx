"""The published firmware image: what it is, and handing it out.

Why this exists. `pio run -t upload` pushes, and pushing needs a laptop with the toolchain on
the same network as the panel; the person standing in front of a greenhouse has neither. So the
panel pulls instead - it asks what the newest image is, decides for itself whether it is already
running that image, and downloads it if not. This module is the server half of that, and it is
deliberately small: one manifest and one file, per board.

Publishing is a file copy. An operator puts `firmware.bin` in the firmware directory beside the
database, with scp or a shared folder, and that is the whole procedure. There is no upload
endpoint and adding one would be a mistake worth naming: an authenticated write that lands
executable code on every panel in the greenhouse is a far larger thing to get wrong than a `cp`,
and nothing here needs it. The auth surface this file adds is two reads.

Three boards, three files, one procedure. The panel was the only device that could install
anything when this was written; the ESP32-CAM and the sensor node now pull their own images the
same way, so `role` picks which file the same two endpoints describe and serve. The panel's file
keeps the name it has always had, because every panel already deployed polls without a `role=`
at all and a rename would present to all of them as "nothing published" - permanently, and
silently, on the one device with no console to say so.

The identifier is app_elf_sha256, and that choice is forced rather than preferred. PlatformIO
stamps no version of ours into the image: the esp_app_desc_t that arduino-esp32 ships carries
version '76b7a3f', project_name 'arduino-lib-builder', and a date/time from whenever the
framework itself was built - all four byte-identical across every build this repo has ever
produced. A panel comparing on any of them would decide it was up to date forever and no amount
of publishing would ever move it. app_elf_sha256 is the one field in the descriptor that changes
when our code changes, because it is the hash of the ELF the image was linked from, and the
device reads its own copy out of esp_app_get_description() to compare against.
"""

import hashlib
import json
import logging
import os
import struct
import threading
from pathlib import Path
from typing import Any, BinaryIO, Optional

from . import store

log = logging.getLogger(__name__)

# esp_app_desc_t, as declared in esp_app_desc.h and confirmed byte for byte against a real
# firmware.bin from this project's build. The descriptor sits at a fixed offset - immediately
# after the 32-byte esp_image_header_t + first segment header - which is what lets this be a
# 288-byte read instead of an ELF walk or a call out to esptool.
_DESC_AT = 0x20
_DESC_LEN = 256
_DESC_MAGIC = 0xABCD5432  # ESP_APP_DESC_MAGIC_WORD

# Offsets inside the descriptor. Only the two fields that go on the wire are named; the rest
# (secure_version, version, project_name, date, time) are either unused or, as the module
# docstring explains, constant across our builds and therefore worthless as identifiers.
_OFF_MAGIC = 0x00
_OFF_IDF_VER = 0x70  # char[32], NUL-padded
_OFF_ELF_SHA = 0x90  # uint8[32], raw bytes


# The three boards, spelled as shared/nodeproto.h's nodeproto_role_name() spells them. That
# header is the wire contract all three firmwares build against, so these strings are not a
# choice this file gets to make: a fourth spelling here would be a device asking for an image
# under a name the server has never heard of, answered with a 404 that reads like "nothing
# published" and sends nobody looking for a typo.
ROLES = ("panel", "cam", "node")

# What a request that names no role means. It has to stay "panel" forever: a panel flashed before
# fwpull.cpp learned to send ?role= builds its URL without the query at all, and it is the one
# board that cannot be reached to change that. Current panel builds ARE explicit
# (src/fwpull.cpp), so this default is a compatibility path and not the live one.
DEFAULT_ROLE = "panel"

# One directory, one file per role. The panel's name is load-bearing and the other two are not:
# an operator (or ghfw.py) has been dropping bytes at firmware.bin since this module existed, so
# that path is frozen, while cam and node are new and get a name that says which board they are
# for. Flat rather than a subdirectory per role because publishing is `scp firmware-cam.bin`
# and a directory that has to exist first is a step to forget.
_IMAGE_NAME = {
    "panel": "firmware.bin",
    "cam": "firmware-cam.bin",
    "node": "firmware-node.bin",
}


def image_path(role: str = DEFAULT_ROLE) -> Path:
    """Where an operator publishes this role's image, and the only place either endpoint looks.

    Beside the database rather than under an environment variable of its own, because the two
    have exactly the same deployment requirement - a mounted volume that survives a redeploy -
    and PLANTRX_DB already says where that volume is. A second variable is a second thing to
    forget in Dokploy, and forgetting it would present as "no firmware published" rather than as
    a configuration error anybody would recognise.

    An unknown role raises rather than falling back to the panel's file, and the KeyError is the
    point: serving a 7" RGB panel image to an ESP32-CAM writes a bootloader-valid, hardware-wrong
    application into its OTA slot on a board nobody can attach a serial cable to. Callers reach
    this through main._role_q, which rejects the request at the edge; a KeyError here is a caller
    that skipped that gate, and it must be loud.
    """
    return store.db_path().parent / "firmware" / _IMAGE_NAME[role]


def _cstr(raw: bytes) -> str:
    """A fixed-width NUL-padded char[] as the string it actually holds."""
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def _app_desc(head: bytes) -> Optional[dict]:
    """The descriptor's two useful fields, or None if this file is not an ESP32 application.

    The magic check is the gate this whole module hangs on, and it is why this function can
    return None at all. Without it, whatever an operator dropped into the firmware directory
    would be described as firmware and served as firmware: a half-finished scp, last week's ELF,
    a zip, the bootloader by mistake. The panel would take those bytes, write them into its
    inactive OTA partition and boot into them. It does survive that - health.cpp rolls back to
    the previous slot after three failed boots - but "survives" is three reboots and several
    minutes in front of somebody who pressed a button expecting an update, and the four bytes
    checked here avoid all of it before a single byte leaves the server.
    """
    if len(head) < _DESC_AT + _DESC_LEN:
        return None
    desc = head[_DESC_AT:_DESC_AT + _DESC_LEN]
    (magic,) = struct.unpack_from("<I", desc, _OFF_MAGIC)
    if magic != _DESC_MAGIC:
        return None
    return {
        "idf_ver": _cstr(desc[_OFF_IDF_VER:_OFF_IDF_VER + 32]),
        # Raw 32 bytes in the image; lowercase hex on the wire, which is the form the device
        # formats its own app_elf_sha256 into before comparing.
        "elf_sha256": desc[_OFF_ELF_SHA:_OFF_ELF_SHA + 32].hex(),
    }


def describe(path: Path) -> Optional[dict[str, str]]:
    """The descriptor fields of an arbitrary file, or None if it is not one of our images.

    manifest() answers this question about the image that is already published and adds size,
    md5 and mtime to the answer. This is the half a *candidate* needs. ghfw.py has just
    downloaded some bytes from a release and has two things to establish before it replaces
    anything: that they are an ESP32 application at all, and that they are not byte-for-byte the
    application already published. Both come off the same 288-byte read, and both use the same
    parser as the manifest rather than a second copy of these offsets.
    """
    try:
        with path.open("rb") as fh:
            return _app_desc(fh.read(_DESC_AT + _DESC_LEN))
    except OSError:
        return None


# THE VERSION OF WHAT IS PUBLISHED, AND WHY THE IMAGE CANNOT SUPPLY IT.
#
# elf_sha256 identifies an image; it does not order two of them. Every question a board actually
# has is an ordering question - is what the server is offering newer than what I am running - and
# a hash can only answer "different". That answer is wrong in both directions and both have
# happened here. A rebuild of identical sources changes the linked ELF's hash, so a node already
# running the newest code downloads a megabyte to install what it is already running; measured on
# this project's own releases, sensor_node's image is the same 1,085,584 bytes in v0.4.2 and
# v0.4.3 with an empty `git diff` between them, and 68 of those bytes differ. In the other
# direction, a board flashed over USB with unreleased work has a hash that merely differs from
# the published one, so pressing update replaces newer code with older - three times, before this
# existed. The descriptor cannot break the tie: as the module docstring explains, its version
# field reads '76b7a3f' in every image this repository has ever produced.
#
# The one place a real version exists is the release the bytes came from. ghfw.py asks GitHub for
# the newest release and that release's TAG is the version, because the workflow publishes a
# release only for a v* tag - so a tag is the only thing that ever becomes a published image, and
# an untagged build cannot be offered to anybody. The tag is recorded here when the image lands
# and handed to the board in the manifest, next to the hash it already compares.
#
# THE RECORD CARRIES THE elf_sha256 OF THE IMAGE IT WAS WRITTEN FOR.
# A version kept in a second file is a version that can disagree with the bytes it claims to
# describe, which is the objection ghfw.py already raises against holding its ETag on disk. So a
# record names the image it belongs to, and _recorded_tag() ignores one whose hash is not the hash
# of the image being described. That makes the drift unrepresentable instead of unlikely. If the
# image is written and the record is not - a crash between the two, an operator's scp, a volume
# restored one directory at a time - the record describes bytes that are no longer here, the
# manifest reports "" and every board falls back to comparing hashes, which is exactly what they
# all do today. A bare tag file fails the other way and far worse: v0.4.3 left sitting over
# v0.4.2's bytes tells a board running v0.4.3 that it is current when it is a release behind, and
# no later release can talk it out of that until the tag itself changes.
#
# Beside the database rather than in the firmware directory, because that directory's contents are
# a contract: everything in it is an image an operator published, and anything else in it reads as
# a publish in progress to whoever looks next (which is the property tests/test_firmware_pull.py
# pins down when it refuses to leave a .part behind). One file per role rather than one file for
# all three, so a write for the node cannot lose the panel's record, and so an operator can delete
# one board's version without editing anything.
def version_path(role: str = DEFAULT_ROLE) -> Path:
    """Where this role's published version is remembered. Named after the image it describes."""
    return store.db_path().parent / "fwver" / (_IMAGE_NAME[role] + ".json")


def record_version(role: str, tag: str) -> None:
    """Remember that the image now on disk for this role is the one released under `tag`.

    Called by ghfw.py for every role that agreed with a release - both the role whose bytes it
    just installed and the role that already had them. The second case is not an optimisation to
    skip: a server that comes up with images already matching the newest release replaces nothing,
    and if only a replace recorded a tag then those boards would have no ordering available until
    somebody happened to cut another release.

    Reads the descriptor itself rather than taking an elf_sha256 from the caller, so the record
    can only ever describe the bytes that are actually published. An image that is missing or
    unreadable records nothing at all: there is no image for the tag to be about, and a tag on its
    own is the thing this must never write.
    """
    desc = describe(image_path(role))
    if desc is None:
        log.warning("firmware: not recording %s for %s: no readable image", tag, role)
        return
    path = version_path(role)
    tmp = path.with_name(path.name + ".part")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Written aside and renamed over, for the same reason ghfw.py downloads to .part: a reader
        # arriving mid-write must see the old record or the new one, never half of either. Half a
        # record is unparseable, which is answered with "" and a fallback to hashes, so even the
        # torn write is safe - this only keeps it from happening at all.
        tmp.write_text(json.dumps({"tag": tag, "elf_sha256": desc["elf_sha256"]}) + "\n",
                       encoding="utf-8")
        os.replace(tmp, path)
    except OSError as exc:
        log.warning("firmware: cannot record %s for %s: %s", tag, role, exc)
        try:
            os.unlink(tmp)
        except OSError:
            pass


def _recorded_tag(role: str, elf_sha256: str) -> str:
    """The tag published with this exact image, or "" when there is not one.

    "" is not a degraded answer and is never logged as one. It is what an image an operator copied
    in by hand honestly has - no release, no tag, nothing to order it against - and it is also
    what a record left over from an image that has since been replaced deserves to be worth. Both
    reach the device as an absent ordering and put it back on comparing hashes.

    A record that cannot be read or parsed is a "" as well, and it says so in the log, because
    that one is not an ordinary state: it is a truncated write or something else editing this
    directory, and the manifest it feeds still has to be answerable.
    """
    path = version_path(role)
    try:
        rec = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return ""
    except (OSError, ValueError) as exc:
        log.warning("firmware: cannot read %s: %s", path, exc)
        return ""
    if not isinstance(rec, dict) or rec.get("elf_sha256") != elf_sha256:
        return ""
    tag = rec.get("tag")
    return tag if isinstance(tag, str) else ""


# md5 of the whole image, remembered against the file identity that produced it.
#
# The manifest is fetched on every pull attempt, and re-reading 2.5MB off disk to arrive at the
# same 32 characters is a cost with nothing on the other side of it. The cache key is
# (st_mtime_ns, st_size) and it is invalidated by exactly one thing: a stat that no longer
# matches. Copying a new image in changes both halves; a write that leaves the file
# byte-identical changes neither, and does not need a new hash either. Nanosecond mtime rather
# than whole seconds because two publishes inside one second is not hypothetical - it is what a
# script doing a rollback test does, and a second-resolution key would serve the first image's
# hash for the second image's bytes.
#
# One entry per published file, keyed on the path, because there are now three of them and a
# single slot would have the panel's manifest evict the cam's and then be evicted by it - two
# devices polling a minute apart would each re-hash on every request and the cache would exist
# only to be missed.
#
# Still ONE lock, and it is deliberately held across the hashing itself rather than only across
# the dict access. That is the whole property this cache has: a second caller arriving on a cold
# entry waits and then reads the answer, instead of doing the same 2.5MB read in parallel and
# racing to store it. A lock per role would let three cold hashes run at once - which is not the
# case worth optimising, because the collision that actually happens is a panel and its own
# retry on the SAME file, and that one is covered either way - and it would need a registry that
# needs a lock of its own to grow safely. Three images is the ceiling here; serialising a cold
# cam hash behind a cold panel hash costs one of them a few hundred milliseconds, once.
_md5_lock = threading.Lock()
_md5_cache: dict[str, tuple[tuple[int, int], str]] = {}


def _md5(fh: BinaryIO, path: Path, key: tuple[int, int]) -> str:
    name = str(path)
    with _md5_lock:
        hit = _md5_cache.get(name)
        if hit is not None and hit[0] == key:
            return hit[1]
        h = hashlib.md5()
        fh.seek(0)
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
        digest = h.hexdigest()
        _md5_cache[name] = (key, digest)
        return digest


def manifest(role: str = DEFAULT_ROLE) -> Optional[dict[str, Any]]:
    """Everything a board needs to decide whether to download, or None if there is nothing.

    None covers three situations and deliberately does not tell them apart: nothing has been
    published yet, something has been published that is not an ESP32 application image, and the
    file could not be read. All three mean the same thing to the device asking - there is no
    update here, carry on running what you have - and none of them is a server defect. An
    operator who copied the wrong file has made an operator's mistake, so it belongs in the log
    where they are already looking, not in a 500 that tells a device the server is broken.

    A role with nothing published is the ordinary state of this server, not a degraded one: two
    of the three files do not exist on any deployment that has not started updating its nodes,
    and the cam asking about its image every time an operator presses the button must read as
    "no update" rather than as anything worth a log line.

    The stat and the hash come off one open handle rather than off the path twice, so a manifest
    always describes a single consistent view of the file even if a publish lands mid-request.

    The version is read after the image and against the image's own elf_sha256, so a manifest can
    never carry a tag belonging to bytes it is not describing. There is no manifest at all without
    an image, which is why a record that outlives the image it names is answered by the None above
    rather than by a version with nothing under it.
    """
    path = image_path(role)
    try:
        with path.open("rb") as fh:
            st = os.fstat(fh.fileno())
            head = fh.read(_DESC_AT + _DESC_LEN)
            desc = _app_desc(head)
            if desc is None:
                log.warning(
                    "firmware: %s is not an ESP32 application image (no esp_app_desc magic);"
                    " refusing to publish it", path)
                return None
            md5 = _md5(fh, path, (st.st_mtime_ns, st.st_size))
    except FileNotFoundError:
        return None
    except OSError as exc:
        log.warning("firmware: cannot read %s: %s", path, exc)
        return None

    return {
        "elf_sha256": desc["elf_sha256"],
        "size": st.st_size,
        "md5": md5,
        "idf_ver": desc["idf_ver"],
        "mtime": int(st.st_mtime),
        # "" whenever there is no tag to be had, which is what a device reads as "no ordering
        # available, compare hashes" - the behaviour every board had before this key existed.
        "version": _recorded_tag(role, desc["elf_sha256"]),
    }
