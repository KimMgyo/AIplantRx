"""Pulling the published image from GitHub, so that publishing is a tag and not an scp.

firmware.py describes publishing as a file copy, and it still is exactly that - this module is a
robot that does the copy. It asks GitHub for this project's newest release, downloads the
firmware.bin attached to it, and leaves those bytes at firmware.image_path(). Nothing downstream
knows GitHub exists: the manifest is still a parse of the file on disk, /v1/firmware/image still
serves that file, and a panel still installs nothing until an operator arms that panel.

WHY THE SERVER PULLS INSTEAD OF CI PUSHING.
The obvious alternative is a deploy step in the workflow that copies the image onto this box, and
it would need a key with write access to the server, held by a public repository's CI, in order
to place executable code that every panel in the greenhouse will run. This direction needs no
credential in either place: release assets of a public repository are public bytes, so the
fetch is unauthenticated and the only way it can go wrong is by fetching nothing. That asymmetry
is the whole reason the workflow publishes a release rather than an artifact - the artifacts
endpoint requires a token even for a public repo.

WHY IT OVERWRITES WHAT AN OPERATOR PUT THERE BY HAND.
Two publishing routes into one path is how "I copied the new image and the panel installed the
old one" happens. There is one path and, once this is running, one writer. An operator who needs
to publish something that is not on a release turns this off (PLANTRX_GH_POLL_S=0) rather than
racing it.

WHAT A FRESH DEPLOY DOES.
The first poll happens at startup, not one interval later, so a new box with an empty data volume
populates itself from the newest release before anybody asks it for a manifest. That is the case
that used to require someone with scp and the right file.

THREE IMAGES, ONE RELEASE, ONE API CALL.
The panel was the only role for a while and this module read like it: one asset name, one
destination. All three firmwares are built and published together now, so a poll fetches the
release once and then settles each role against it. A role that is absent or refused does not stop
the other two - the panel is the board a grower is standing in front of, and it should not wait on
a camera image somebody forgot to tag.

WHY THE DIGEST IS CHECKED BEFORE THE BODY IS FETCHED.
The release JSON already carries each asset's sha256, so "is this already published" is answerable
for nothing. It used to be answered AFTER downloading the whole asset and parsing its descriptor,
which was affordable exactly once - at one image per poll, behind a working ETag. It is not
affordable at three, and the ETag is held in memory, so any restart loop turns into megabytes an
hour of no-op traffic against a public asset URL. The descriptor check still runs, on the bytes
that actually arrive; this only decides whether to ask for them.
"""

import hashlib
import logging
import os
import threading
import time
from pathlib import Path

import httpx

from . import firmware

log = logging.getLogger(__name__)

# The repository is a default rather than required configuration because there is exactly one
# upstream for this firmware and an unset variable would present as "OTA quietly stopped
# working". It stays overridable for a fork.
REPO = os.getenv("PLANTRX_GH_REPO", "KimMgyo/AIplantRx")

# The asset name for a role IS the file name that role is published under, so this reads it off
# firmware.py instead of restating it. Two spellings of "firmware-cam.bin" - one here and one in
# the module that serves it - is a release that uploads an asset nothing ever looks for.
def _asset_name(role: str) -> str:
    return firmware.image_path(role).name


# 0 disables the loop. The floor exists because this runs unauthenticated: 60 requests an hour
# per IP is the whole budget, and a mistyped interval of 5 would spend it in five minutes and
# then fail for the rest of the hour. Conditional requests that come back 304 are not charged
# against that budget, which is why the ETag below is worth keeping.
POLL_S = int(os.getenv("PLANTRX_GH_POLL_S", "900"))
MIN_POLL_S = 60

_API = "https://api.github.com"
# Connect quickly or not at all; read slowly, because the second request is a 3.5MB body and the
# link this server sits on is not the constraint the panels are.
_TIMEOUT = httpx.Timeout(15.0, read=180.0)
_UA = "plantrx-server"

# Set only on a poll that ends in agreement with the release it looked at - either the bytes were
# installed or they were already installed. A failure deliberately leaves it alone, so the next
# poll asks again instead of being told 304 and skipping a release it never actually fetched.
#
# In memory rather than on disk, which costs one wasted 3.5MB download per restart: after a
# restart the first poll re-fetches and then finds the elf_sha256 already published. The
# alternative is a state file next to the image, and a second thing in the volume that can
# disagree with the image beside it is worse than a download a day.
_etag: str | None = None


def _release(client: httpx.Client) -> tuple[int, dict, str | None]:
    """The newest non-draft, non-prerelease release. Status is returned rather than raised.

    /releases/latest and not /releases[0] on purpose: the endpoint's own definition of latest
    excludes drafts and prereleases, so a tag pushed to try something out cannot become the image
    a greenhouse installs.
    """
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": _UA,
    }
    if _etag:
        headers["If-None-Match"] = _etag
    r = client.get(f"{_API}/repos/{REPO}/releases/latest", headers=headers)
    if r.status_code in (304, 404):
        return r.status_code, {}, r.headers.get("ETag")
    r.raise_for_status()
    return r.status_code, r.json(), r.headers.get("ETag")


def _local_digest(path: Path) -> str | None:
    """sha256 of what is already published, in the "sha256:<hex>" spelling GitHub's JSON uses.

    None for a file that is not there, which is a fresh volume and the one case where there is
    nothing to compare and the download has to happen.
    """
    h = hashlib.sha256()
    try:
        with path.open("rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
    except OSError:
        return None
    return "sha256:" + h.hexdigest()


def _pull_role(client: httpx.Client, role: str, rel: dict, tag: str) -> tuple[bool, str]:
    """Settle one role against one release. Returns (agreed, the phrase for the log line).

    `agreed` is what decides whether the ETag may be kept, and it is False for every outcome the
    next poll should look at again - including an asset that is simply not on the release. A
    missing image is not a steady state to be remembered: the workflow re-uploads assets with
    --clobber, so a release that grows the asset it was missing must not be behind a 304.
    """
    name = _asset_name(role)
    asset = next((a for a in rel.get("assets") or [] if a.get("name") == name), None)
    if asset is None:
        return False, f"{role}: no {name}"

    dest = firmware.image_path(role)

    # The whole question, answered for the price of hashing a file this box already has. GitHub
    # publishes the asset's digest in the release JSON, so agreement needs no body at all.
    # Absent digest falls through to the download - the field is recent, and a fetch is the
    # behaviour this module had before it existed.
    want = asset.get("digest")
    if want and want == _local_digest(dest):
        return True, f"{role}: already {want[7:19]}"

    # Downloaded beside the destination and renamed over it, never written to it. os.replace is
    # atomic within a filesystem, so a device that asks for the image during a pull gets either
    # the old one whole or the new one whole. Writing in place would let it read a
    # half-downloaded image, and a half-downloaded image passes the descriptor check - the
    # descriptor is in the first 288 bytes.
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    # No Authorization on this request even if one is ever added above: the download redirects to
    # a signed URL on another host, and a bearer token arriving alongside that signature is
    # rejected by the object store rather than ignored.
    got = 0
    try:
        with client.stream(
            "GET", asset["browser_download_url"], headers={"User-Agent": _UA}
        ) as resp:
            resp.raise_for_status()
            with part.open("wb") as fh:
                for chunk in resp.iter_bytes(1 << 16):
                    fh.write(chunk)
                    got += len(chunk)

        size = int(asset.get("size") or 0)
        if size and got != size:
            return False, f"{role}: got {got} of {size} bytes"

        desc = firmware.describe(part)
        if desc is None:
            return False, f"{role}: {name} is not an ESP32 application image"

        # Reached when the digest was absent or disagreed but the image is the same one anyway -
        # a re-tagged identical build, or a release whose JSON carries no digest.
        cur = firmware.manifest(role)
        if cur is not None and cur["elf_sha256"] == desc["elf_sha256"]:
            return True, f"{role}: already {desc['elf_sha256'][:12]}"

        os.replace(part, dest)
    finally:
        # Anything still at .part failed a check or died mid-stream. Leaving it costs megabytes
        # in the volume and, worse, reads as a publish in progress to whoever looks next.
        try:
            os.unlink(part)
        except FileNotFoundError:
            pass

    was = cur["elf_sha256"][:12] if cur else "nothing"
    return True, f"{role}: {desc['elf_sha256'][:12]} ({got} bytes, was {was})"


def pull_once() -> str:
    """One poll over all three roles. Returns the line that goes in the log.

    The return value is a sentence rather than a bool because every outcome here is a thing an
    operator eventually asks about - why a release did not land, or which one did.
    """
    global _etag

    with httpx.Client(timeout=_TIMEOUT, follow_redirects=True) as client:
        status, rel, etag = _release(client)
        if status == 304:
            return "unchanged"
        if status == 404:
            return f"{REPO} has no published release"

        tag = rel.get("tag_name") or "?"
        agreed = True
        said = []
        for role in firmware.ROLES:
            ok, phrase = _pull_role(client, role, rel, tag)
            agreed = agreed and ok
            said.append(phrase)

        # Only when every role settled. One unfinished image is a reason to ask again, and asking
        # again is exactly what an unset ETag buys.
        if agreed:
            _etag = etag
        return f"{tag}: " + "; ".join(said)


def _loop(interval: int) -> None:
    while True:
        try:
            log.info("ghfw: %s", pull_once())
        except Exception as exc:
            # Every failure here is a network away from a machine that is running fine, and the
            # consequence of all of them is that the currently published image stays published.
            # So this is a warning and a retry, never a raise: a thread that dies takes OTA
            # publishing with it until somebody restarts the server.
            log.warning("ghfw: poll failed: %s: %s", type(exc).__name__, exc)
        time.sleep(interval)


def start() -> None:
    """Start polling. Called once from the app's startup event.

    A daemon thread rather than an asyncio task for two reasons: the work is a multi-megabyte
    write to disk, which blocks whatever runs it, and firmware.py already serialises its md5
    cache with a threading.Lock - so a thread replacing that file is the case that lock was
    written for. It also leaves the startup hook synchronous, which is what it already is.
    """
    if POLL_S <= 0:
        log.info("ghfw: disabled (PLANTRX_GH_POLL_S=%d)", POLL_S)
        return
    interval = max(POLL_S, MIN_POLL_S)
    threading.Thread(target=_loop, args=(interval,), name="ghfw", daemon=True).start()
    log.info("ghfw: watching %s for %s every %ds", REPO, ASSET_NAME, interval)
