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
# already-patched trees are no-ops. A missing file WARNS but never fails the build.
#
Import("env")  # noqa: F821  (injected by PlatformIO's SCons runner)

import glob
import os

OLD = ".bb_invalidate_cache = 0"
NEW = ".bb_invalidate_cache = 1"
REL = os.path.join(
    "ESP32_Display_Panel", "src", "drivers", "bus", "esp_panel_bus_rgb.cpp"
)


def _candidate_paths():
    # Preferred: the libdeps dir for THIS env, as PlatformIO reports it.
    # subst() forces expansion in case PlatformIO hands these back as $-tokens.
    libdeps = env.subst("$PROJECT_LIBDEPS_DIR")  # noqa: F821
    pioenv = env.subst("$PIOENV")  # noqa: F821
    if libdeps and pioenv:
        yield os.path.join(libdeps, pioenv, REL)
    # Fallback: glob every env's libdeps in case the keys ever change shape.
    if libdeps:
        for hit in glob.glob(os.path.join(libdeps, "*", REL)):
            yield hit


def patch_rgb_bounce(*_args, **_kwargs):
    seen = set()
    target = None
    for path in _candidate_paths():
        path = os.path.abspath(path)
        if path in seen:
            continue
        seen.add(path)
        if os.path.isfile(path):
            target = path
            break

    if target is None:
        print(
            "[patch_rgb_bounce] WARN: esp_panel_bus_rgb.cpp not found under libdeps "
            "yet; skipping (library may not be fetched). bb_invalidate_cache NOT patched."
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
        print(
            "[patch_rgb_bounce] WARN: neither `%s` nor `%s` found in %s; library layout "
            "may have changed - review manually." % (OLD, NEW, target)
        )


# Run at script-load (pre: scripts execute during config, before the build graph),
# so the source is already patched by the time the library is compiled.
patch_rgb_bounce()
