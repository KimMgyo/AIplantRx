"""The published firmware image: what it is, and handing it out.

Why this exists. `pio run -t upload` pushes, and pushing needs a laptop with the toolchain on
the same network as the panel; the person standing in front of a greenhouse has neither. So the
panel pulls instead - it asks what the newest image is, decides for itself whether it is already
running that image, and downloads it if not. This module is the server half of that, and it is
deliberately two things: one manifest and one file.

Publishing is a file copy. An operator puts `firmware.bin` in the firmware directory beside the
database, with scp or a shared folder, and that is the whole procedure. There is no upload
endpoint and adding one would be a mistake worth naming: an authenticated write that lands
executable code on every panel in the greenhouse is a far larger thing to get wrong than a `cp`,
and nothing here needs it. The auth surface this file adds is two reads.

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


def image_path() -> Path:
    """Where an operator publishes, and the only place either endpoint looks.

    Beside the database rather than under an environment variable of its own, because the two
    have exactly the same deployment requirement - a mounted volume that survives a redeploy -
    and PLANTRX_DB already says where that volume is. A second variable is a second thing to
    forget in Dokploy, and forgetting it would present as "no firmware published" rather than as
    a configuration error anybody would recognise.
    """
    return store.db_path().parent / "firmware" / "firmware.bin"


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
# One entry, because there is one image. Guarded by a lock held across the hashing itself: a
# second caller arriving on a cold cache waits and then reads the answer, rather than doing the
# same 2.5MB read in parallel and racing to store it.
_md5_lock = threading.Lock()
_md5_cache: Optional[tuple[tuple[int, int], str]] = None


def _md5(fh: BinaryIO, key: tuple[int, int]) -> str:
    global _md5_cache
    with _md5_lock:
        if _md5_cache is not None and _md5_cache[0] == key:
            return _md5_cache[1]
        h = hashlib.md5()
        fh.seek(0)
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
        _md5_cache = (key, h.hexdigest())
        return _md5_cache[1]


def manifest() -> Optional[dict[str, Any]]:
    """Everything the panel needs to decide whether to download, or None if there is nothing.

    None covers three situations and deliberately does not tell them apart: nothing has been
    published yet, something has been published that is not an ESP32 application image, and the
    file could not be read. All three mean the same thing to a panel - there is no update here,
    carry on running what you have - and none of them is a server defect. An operator who copied
    the wrong file has made an operator's mistake, so it belongs in the log where they are
    already looking, not in a 500 that tells a device the server is broken.

    The stat and the hash come off one open handle rather than off the path twice, so a manifest
    always describes a single consistent view of the file even if a publish lands mid-request.
    """
    path = image_path()
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
            md5 = _md5(fh, (st.st_mtime_ns, st.st_size))
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
    }
