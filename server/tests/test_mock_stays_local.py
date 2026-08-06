"""Mock display data must never reach the wire.

Two sensors are on order - a BH1750 and a soil probe - and until they arrive their
monitor tiles carry made-up numbers so the strip can be laid out at the width it will
really have. That is a reasonable thing to do to a screen and an unforgivable thing to
do to a database: a fabricated reading in telemetry becomes a row the server averages,
a window the model scores, and a band it prescribes against. Wrong on purpose is worse
than absent, because absent is a state every layer of this project already handles.

The separation is structural, not careful: the substitution happens inside
page_monitor.cpp's own refresh, and sensornode_lux()/_soil() keep returning the absent
sentinel, so plantrx.cpp sends null and the server has no sample. Substituting inside
the getters instead would have been one line shorter and would have poisoned the wire,
the database, the window and the model at once.

This test pins that shape, because "we were careful" is not a mechanism:

  A. plantrx.cpp reads the sensor getters directly, with no mock in sight.
  B. The mock generator exists in exactly one file, and it is a UI file.
  C. The tile says 임시 while it is mocked, so a plausible number on the wall can
     never be mistaken for a measurement.

When the hardware lands, sensornode_has_*() latches on the first genuine reading and
the tiles switch themselves over. Deleting MOCK_LUX_SOIL then makes this test moot
rather than failing - it checks that IF a mock exists, it is contained.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src")


def _read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as fh:
        return fh.read()


def _files():
    out = []
    for dirpath, _dirs, names in os.walk(SRC):
        for n in names:
            if n.endswith((".cpp", ".h")):
                out.append(os.path.join(dirpath, n))
    return out


def test_the_wire_reads_the_sensor_not_the_mock():
    """plantrx.cpp's telemetry takes lux and soil straight from the node getters."""
    src = _read("src", "plantrx.cpp")
    for field, getter in (("lux", "sensornode_lux()"),
                          ("soil_pct", "sensornode_soil()")):
        m = re.search(r'badd_sensor\(&b,\s*"%s"\s*,\s*([^,]+),' % field, src)
        assert m, "plantrx.cpp no longer sends %s the way this test reads it" % field
        arg = m.group(1).strip()
        assert arg == getter, (
            "%s reaches the server as %r instead of %s - a display mock has leaked "
            "onto the wire" % (field, arg, getter))

    # And the getters themselves must not be where a mock was planted.
    node = _read("src", "sensornode.cpp")
    assert "mock" not in node.lower(), (
        "src/sensornode.cpp mentions a mock; the getters feed telemetry, so anything "
        "fabricated there reaches the database and the model")
    return "lux and soil reach the wire straight from the node"


def test_the_mock_lives_in_exactly_one_ui_file():
    """One generator, in the file that draws it, so its blast radius is the screen."""
    holders = []
    for path in _files():
        with open(path, encoding="utf-8") as fh:
            body = fh.read()
        if "mock_wave" in body or "MOCK_LUX_SOIL" in body:
            holders.append(os.path.relpath(path, ROOT).replace("\\", "/"))
    if not holders:
        return "no mock in the tree (hardware arrived, or it was removed)"
    assert holders == ["src/ui/page_monitor.cpp"], (
        "the mock is no longer confined to the page that draws it: %s" % holders)
    return "mock confined to %s" % holders[0]


def test_a_mocked_tile_says_so():
    """The number is labelled where it is read, or it is indistinguishable from real."""
    ui = _read("src", "ui", "page_monitor.cpp")
    if "MOCK_LUX_SOIL" not in ui:
        return "no mock to label"
    assert "임시" in ui, (
        "page_monitor.cpp draws mock readings with nothing on screen saying so")
    # The marker goes on the title, not the value: the value has to stay a bare number
    # for the layout to be worth judging, and the title is the one free channel (the
    # name's COLOUR already answers whose band decided the verdict).
    assert re.search(r'"%s 임시"', ui) or re.search(r'%s\s+임시', ui), (
        "the 임시 marker is not being composed onto a tile title")
    # And real data must be able to win: the latch is what switches it back.
    for fn in ("sensornode_has_lux()", "sensornode_has_soil()"):
        assert fn in ui, (
            "%s is not consulted, so real hardware could not take over from the mock"
            % fn)
    return "mock tiles labelled 임시, real readings take precedence"


if __name__ == "__main__":
    for fn in (test_the_wire_reads_the_sensor_not_the_mock,
               test_the_mock_lives_in_exactly_one_ui_file,
               test_a_mocked_tile_says_so):
        print("%-52s %s" % (fn.__name__, fn()))
    print("OK")
