"""The operator's page: which board is running what, and the one button that changes it.

Everything an update needed before this existed lived in two places that are no use from a
phone - a curl against /v1/firmware/latest for what is published, and the panel's own firmware
page for what is running, which requires standing in front of the panel. The whole point of
arming a pull remotely (main.request_update_mode) is that the greenhouse is an hour away, and
that endpoint was unusable from the only device an operator has out there. So this file answers
one question - is each board running the published image, and if not, arm it - and answers it in
a browser.

Two things about its shape follow from that and are not accidents:

  - The state route is READ-ONLY and arms nothing. The two arming endpoints already exist in
    main.py with their own reasoning about one-shot delivery, and a page that POSTed to some
    third thing would be a second way to arm the same flag, drifting from the first. The page
    calls those endpoints directly.

  - It reads the flags WITHOUT consuming them (store.peek_device_flags). Every other reader on
    this rail is the poll handler, whose read IS the delivery - see store.take_update_mode for
    why. A page that used take_* would deliver an operator's armed update to a browser and the
    panel would never hear about it, which is the single worst bug this file could have.

The page itself is unauthenticated and the data behind it is not, which is the right split: the
HTML is a constant with no greenhouse in it, and it cannot fetch anything until somebody types
the token into it.
"""

import logging
from pathlib import Path
from typing import Any, Optional

from fastapi import APIRouter
from fastapi.responses import HTMLResponse

from . import firmware, scheduler, store

log = logging.getLogger(__name__)

# Two routers because the two routes differ in exactly one thing that matters - whether the
# bearer is required - and main.py applies that with Depends(auth) at include time. Splitting
# them here means the auth decision stays visible at the mount site in main.py, next to every
# other route's, rather than being buried in a decorator in this file where a reader looking for
# "what is unauthenticated on this server" would not think to check.
state_router = APIRouter(prefix="/admin")   # mounted under /v1, behind auth
page_router = APIRouter()                   # mounted on the app, deliberately open

# The four columns on device_flags, named once. The state JSON always carries all four as
# booleans even for a device that has never been armed: the page renders its armed-and-waiting line off these,
# and a key that is sometimes absent would have it render nothing at all rather than "not armed".
_FLAG_KEYS = ("update_mode", "firmware_pull", "node_pull_cam", "node_pull_node")

# How many log lines ride along on every state response. Sixty rather than the store's default
# 200 because this payload is fetched every five seconds over whatever mobile link the greenhouse
# has, and sixty lines is already more than fits on the phone screen it is drawn on. Somebody who
# needs the deeper history has GET /v1/nodelog, which is what its limit= is for.
_LOG_ROWS = 60

# Stable card order on the page: the panel first, because it is the board that carries the other
# two, then cam, then node. all_device_fw() answers newest-recv_ts-first, which is the right order
# for a table of raw rows and the wrong one for a page - a cam that happened to report a second
# later than the panel would swap the two cards under the operator's thumb between polls, and a
# button that moves while being pressed is how the wrong board gets updated.
_ROLE_ORDER = {role: i for i, role in enumerate(firmware.ROLES)}


def _role_entry(row: dict, published: str) -> dict:
    """One board's firmware line: what it runs, what is published, and whether those agree.

    `current` compares on the length of what the device reported, because the two strings are
    not the same length by design: the board sends a prefix of its elf_sha256 (schema.FirmwareState
    documents why - the panel forwards this for three boards inside one telemetry body) and the
    manifest carries all 64 characters. Comparing them for equality would report every board in
    the greenhouse as out of date, forever.

    An empty `running` is never current. That is the case that matters most here: it means the
    board has not said what it is running - old firmware, or a node that has not answered yet -
    and "" is a prefix of every string, so a bare prefix test would badge an unknown version as
    up-to-date and hide exactly the board somebody needs to look at.
    """
    running = str(row["elf"] or "")
    return {
        "role": str(row["role"]),
        "running": running,
        "published": published,
        "current": bool(running) and running == published[:len(running)],
        "up_s": int(row["up_s"]),
        "heap": int(row["heap"]),
        "online": bool(row["online"]),
        "pending": bool(row["pending"]),
        "can_ota": bool(row["can_ota"]),
        "recv_ts": int(row["recv_ts"]),
    }


def _device_entry(device: str, last_seen: int, uptime_ms: int,
                  rows: list[dict], published: dict[str, str]) -> dict:
    flags = store.peek_device_flags(device)
    rows = sorted(rows, key=lambda r: _ROLE_ORDER.get(str(r["role"]), len(_ROLE_ORDER)))
    return {
        "device": device,
        "last_seen": int(last_seen),
        "uptime_ms": int(uptime_ms),
        "flags": {key: bool(flags.get(key)) for key in _FLAG_KEYS},
        "roles": [_role_entry(r, published.get(str(r["role"]), "")) for r in rows],
    }


@state_router.get("/state")
def admin_state() -> dict[str, Any]:
    """Everything the page draws, in one response, so a poll is one request.

    One route rather than three (published / devices / logs) because the page's job is a
    comparison: a version that is current against a manifest fetched four seconds later is a
    verdict about two different moments, and it would flicker between up-to-date and needs-update during a publish
    for no reason the operator could see. One handler reads one consistent view.

    Sync rather than async, for the reason at the top of store.py: this does blocking sqlite reads
    plus up to three stat-and-hash passes over the published images, and a sync handler does that
    on anyio's worker threadpool instead of stalling the event loop the telemetry polls share.
    """
    published = {role: firmware.manifest(role) for role in firmware.ROLES}
    # The elf_sha256 of each published image, or "" where nothing is published. A role with no
    # manifest is the ordinary state of this server (see firmware.manifest), so this is a normal
    # value and not a missing one - the page turns it into a greyed button with a reason.
    pub_elf = {role: str((m or {}).get("elf_sha256", "")) for role, m in published.items()}

    by_device: dict[str, list[dict]] = {}
    for row in store.all_device_fw():
        by_device.setdefault(str(row["device"]), []).append(row)

    devices: list[dict] = []
    for known in store.known_devices():
        name = str(known["device"])
        devices.append(_device_entry(name, known["last_seen"], known["uptime_ms"],
                                     by_device.pop(name, []), pub_elf))

    # A device with firmware rows and no telemetry left to date it. The two tables expire on
    # completely different terms - telemetry is pruned at TELEMETRY_TTL_S, device_fw is one row
    # per board that is only ever overwritten - so a panel that has been unplugged for longer
    # than a fortnight still has a version on file and nothing to say when it was last seen.
    # Dropping it would take the board off the page at exactly the moment somebody is looking
    # for it; last_seen 0 says "no telemetry retained" and the roles carry their own recv_ts.
    for name in sorted(by_device, key=lambda n: -max(int(r["recv_ts"]) for r in by_device[n])):
        devices.append(_device_entry(name, 0, 0, by_device[name], pub_elf))

    return {
        "ts": scheduler.now(),
        "published": published,
        "devices": devices,
        "logs": store.recent_node_logs(limit=_LOG_ROWS),
    }


# The page, as one string in this module rather than a file under app/static/.
#
# A file would need a StaticFiles mount or a runtime path resolution, and both buy something this
# page does not use: there is exactly one document, it has no images, no fonts and no second
# stylesheet, so a directory would be organising a single file. The string is served by the route
# below, which means the page cannot go missing from a deployment that copied the module - and a
# server whose entire HTTP surface is a dozen audited routes does not grow a general "serve bytes
# from a directory" mount to save an escaped triple quote.
#
# No CDN, no npm, no build step, and this is the hard constraint rather than a preference: the
# greenhouse server is reachable from a phone on the same link the panel polls over, and that link
# is regularly the only thing working. A page that fetched a framework from anywhere else would be
# blank in the exact situation it exists for.
_PAGE_FILE = Path(__file__).with_name("static") / "admin.html"


def _page() -> str:
    """The page's bytes, read from app/static/admin.html on every request.

    A FILE and not a string literal in this module, and the reason is not organisation - it is
    tools/gen_fonts.py. That script derives the panel's font subset from the string literals in
    src/, include/ and server/app/*.py, on the sound premise that Korean prose in this package is
    prose the server may put on the panel's screen. This page's Korean is browser-bound and the
    panel will never draw a character of it, but a scanner cannot tell the two apart - so an inline
    literal made every subset font carry twenty glyphs for a document the panel cannot render, and
    failed tests/test_font_coverage.py to say so. Moving the document out of Python makes the
    exclusion structural: the scanner globs *.py, and this is not one.
    read on every request rather than at import, because it costs one 13KB read on a route nobody
    hits in a loop, and it means editing the page on a running server is a refresh rather than a
    redeploy.

    No StaticFiles mount and no directory serving: there is exactly one document, it has no images,
    no fonts and no second stylesheet, and a server whose whole HTTP surface is a dozen audited
    routes should not grow a general "serve bytes from a directory" mount to hold one file.

    No CDN, no npm, no build step, and that is the hard constraint rather than a preference: this
    server is reached from a phone on the same link the panel polls over, and that link is
    regularly the only thing working. A page that fetched a framework from anywhere else would be
    blank in the exact situation it exists for.
    """
    return _PAGE_FILE.read_text(encoding="utf-8")


@page_router.get("/admin", response_class=HTMLResponse)
def admin_page() -> HTMLResponse:
    """The page, unauthenticated, because there is nothing in it.

    It is a constant: no device, no version, no log line. Everything on screen arrives from
    /v1/admin/state, which is behind the same bearer as the rest of /v1, so an unauthenticated
    visitor gets a token prompt and an empty page.

    Putting it behind auth was tried on paper and does not work: a browser has nowhere to put a
    bearer on a top-level navigation, so the alternatives are a cookie session or basic auth -
    a second credential scheme on a server whose entire threat model is one shared secret over
    TLS. The secret still guards the data; this route guards nothing because it holds nothing.
    """
    return HTMLResponse(_page())
