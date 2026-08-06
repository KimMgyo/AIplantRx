#
# PlatformIO pre-build hook: enable the RGB bounce-buffer cache-invalidate.
#
# WHY: ESP32_Display_Panel hardcodes `.bb_invalidate_cache = 0` in the RGB panel
# config (esp_panel_bus_rgb.cpp, inside BusRGB::Config). There is no app-level API
# to override it (configRGB_BounceBufferSize only sets the bounce-buffer SIZE, not
# this flag). With it off, the RGB bounce engine streams ~30MB/s of framebuffer
# through the 32KB D-cache and continuously evicts the WiFi/lwIP working set - the
# single biggest CONTINUOUS contributor to this board's WiFi instability.
# esp_lcd_panel_rgb.h documents bb_invalidate_cache=1 as the fix for exactly this
# streaming-read-from-PSRAM case.
#
# This lives as a pre: extra_script (rather than a committed patch to a vendored
# copy) because the library sits under .pio/libdeps and is re-fetched on every
# `pio pkg` reinstall; running before each build keeps the fix alive across those.
#
# The edit is idempotent: it only rewrites the `= 0` form, so re-runs and
# already-patched trees are no-ops.
#
# A missing or unrecognisable file WARNS by default and FAILS when
# PATCH_RGB_STRICT is set in the environment. Both halves are deliberate. Locally
# the first `pio run` of a fresh clone can reach this before the library has been
# fetched, and failing there would mean a checkout that cannot build until someone
# knows to run `pio pkg install` first - a warning is the right answer for a build
# a human is watching. In CI nobody is watching, the deps are installed in their
# own step before the build, and a skipped patch would publish a WiFi-unstable
# image that looks exactly like a good one. So the workflow sets the variable, and
# there the same condition is fatal.
#
Import("env")  # noqa: F821  (injected by PlatformIO's SCons runner)

import os

OLD = ".bb_invalidate_cache = 0"
NEW = ".bb_invalidate_cache = 1"
REL = os.path.join(
    "ESP32_Display_Panel", "src", "drivers", "bus", "esp_panel_bus_rgb.cpp"
)


def _target_path():
    # This env's libdeps dir and nothing else. There used to be a glob fallback
    # across `libdeps/*/` here, "in case the keys ever change shape"; it was
    # removed after it did real damage in a test. This tree still holds a
    # libdeps/esp32-s3-touch-lcd-7-crashtest/ from an env that no longer exists,
    # and with the copy for the REAL env moved aside the glob happily patched that
    # stale directory and printed PATCHED - a green line about a tree nothing was
    # compiling from, in precisely the situation the strict check below exists to
    # catch. An exact path that can be absent is worth more than an approximate
    # one that is always present.
    #
    # subst() forces expansion in case PlatformIO hands these back as $-tokens.
    libdeps = env.subst("$PROJECT_LIBDEPS_DIR")  # noqa: F821
    pioenv = env.subst("$PIOENV")  # noqa: F821
    if not libdeps or not pioenv:
        return None
    return os.path.abspath(os.path.join(libdeps, pioenv, REL))


def _refuse(msg):
    # SystemExit rather than a return code: this runs at config time, before SCons
    # has a build graph to fail, and it is the one thing PlatformIO propagates from
    # a pre: script as a nonzero exit.
    if os.environ.get("PATCH_RGB_STRICT"):
        raise SystemExit("[patch_rgb_bounce] FATAL: " + msg)
    print("[patch_rgb_bounce] WARN: " + msg)


def patch_rgb_bounce(*_args, **_kwargs):
    target = _target_path()
    if target is not None and not os.path.isfile(target):
        target = None

    if target is None:
        _refuse(
            "esp_panel_bus_rgb.cpp not found under libdeps yet; skipping "
            "(library may not be fetched). bb_invalidate_cache NOT patched."
        )
        return

    with open(target, "r", encoding="utf-8") as f:
        src = f.read()

    if OLD in src:
        src = src.replace(OLD, NEW)
        with open(target, "w", encoding="utf-8") as f:
            f.write(src)
        print("[patch_rgb_bounce] PATCHED bb_invalidate_cache 0 -> 1 in %s" % target)
    elif NEW in src:
        print("[patch_rgb_bounce] already patched (bb_invalidate_cache = 1): %s" % target)
    else:
        _refuse(
            "neither `%s` nor `%s` found in %s; library layout may have changed - "
            "review manually." % (OLD, NEW, target)
        )


# Run at script-load (pre: scripts execute during config, before the build graph),
# so the source is already patched by the time the library is compiled.
patch_rgb_bounce()
