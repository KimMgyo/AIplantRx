"""Arming a node update, and the one poll it is allowed to ride out on.

    cd server && python tests/test_node_update_flags.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives. pytest collects this file unchanged.

WHY THIS FILE EXISTS. node_pull_cam and node_pull_node are the third and fourth
things on the rail update_mode started: an operator POSTs, a flag lands in
device_flags, and the next telemetry response carries it out past the NAT the
server can never reach through. The panel then relays it to the node over ESP-NOW.

The clearing is where this pair differs from the two before it, and it is the
reason for most of the assertions below. update_mode takes the panel over - main.cpp
stops polling - so a flag that failed to clear could only fire once more, after the
reboot. A node pull leaves the panel polling on its normal sixty-second cadence, so
a flag that outlived its delivery arms the node's update again on every poll, for as
long as the greenhouse runs: a camera that reboots itself once a minute with nothing
on any screen explaining why.

The other case worth its own test is the interaction main.telemetry documents: a node
arming is deliberately NOT consumed by a response that carries the panel's own
update_mode, because the panel is about to stand down and reboot and would never
relay it. It has to still be there on the first poll after that reboot.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "nodeflags.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is one test here, not the fixture

from fastapi.testclient import TestClient  # noqa: E402

from app import main, store  # noqa: E402

CLIENT = TestClient(main.app)

# One device per test. device_flags is keyed on the MAC and every test here is about a flag's
# lifetime, so sharing a panel between two of them would make the second one's first poll
# depend on whether the first one's arming was consumed.
_DEV = iter("AA:BB:CC:00:00:%02X" % i for i in range(1, 32))

# No frames are ever uploaded here, which is what keeps this file offline: plantnet is only
# consulted when there is an RGB frame to consult it about, and the model gate is
# brain.is_configured(), false without GEMINI_API_KEY. Every poll below therefore takes the
# no-diagnosis path and comes back with a prescription built entirely on this server.
_BODY = {
    "sensors": {"temp_c": 26.0, "rh_pct": 55.0, "co2_ppm": 900.0, "leaf_max_c": 31.9},
    "links": {"node_online": True, "cam_online": True, "rgb_live": True,
              "thermal_live": True, "thermal_fps": 4.0, "wifi_rssi": -55},
    "actuator_intent": {"fan": 0, "mist": 0},
    "uptime_ms": 86_400_000,
}


def _poll(device):
    """One telemetry poll, returning the prescription the panel would parse."""
    r = CLIENT.post("/v1/telemetry", json=dict(_BODY, device=device))
    assert r.status_code == 200, r.text
    return r.json()


def _arm(device, role):
    r = CLIENT.post("/v1/device/%s/node_update" % device, params={"role": role})
    assert r.status_code == 200, r.text
    assert r.json() == {"ok": True, "device": device, "node_pull": role}, r.json()
    return r.json()


def _pulls(rx):
    return rx["node_pull_cam"], rx["node_pull_node"]


def test_a_panel_nobody_armed_gets_both_flags_false():
    """The default, which is the whole compatibility story for these two keys.

    A panel running firmware built before they existed parses the response with strstr and never
    looks for them; a panel built after it reads false and dispatches nothing. Either way the
    ordinary poll - which is every poll - has to say "no node was asked to do anything".
    """
    rx = _poll(next(_DEV))
    assert _pulls(rx) == (False, False), rx
    return "an unarmed panel polls and is told to relay nothing"


def test_arming_one_role_rides_out_once_and_is_then_gone():
    """The round trip, per role, and the second poll is the half that matters.

    A flag still set on the poll after delivery is not a stale UI state - it is the panel
    dispatching nodeota_request() again sixty seconds later, and again after that.
    """
    for role, expect in (("cam", (True, False)), ("node", (False, True))):
        device = next(_DEV)
        assert _pulls(_poll(device)) == (False, False), role
        _arm(device, role)
        first = _poll(device)
        assert _pulls(first) == expect, (role, first)
        second = _poll(device)
        assert _pulls(second) == (False, False), (
            "%s stayed armed after delivery: %r" % (role, _pulls(second)))
    return "cam and node each ride out on exactly one poll"


def test_arming_both_roles_delivers_both_on_one_poll():
    """Two POSTs, one response.

    The columns are separate so that "update both boards" is two requests and the second does
    not cancel the first; the delivery is one poll because the panel can relay two ESP-NOW
    commands without waiting a minute between them.
    """
    device = next(_DEV)
    _arm(device, "cam")
    _arm(device, "node")
    first = _poll(device)
    assert _pulls(first) == (True, True), first
    assert _pulls(_poll(device)) == (False, False), "one of the two survived its delivery"
    return "both flags armed, both delivered on one poll, both cleared"


def test_re_arming_the_same_role_is_not_cumulative():
    """Three POSTs for one node is one update, not three.

    Idempotent because an operator who clicks twice is asking for the update they already asked
    for; cumulative arming would mean the camera reboots once per click, several minutes apart,
    with the last one arriving long after anybody is still watching.
    """
    device = next(_DEV)
    for _ in range(3):
        _arm(device, "cam")
    assert _pulls(_poll(device)) == (True, False)
    assert _pulls(_poll(device)) == (False, False), "a second click armed a second update"
    return "three POSTs for one role deliver one update"


def test_a_node_arming_survives_the_panels_own_update_mode():
    """The interaction main.telemetry documents, and the one that silently loses a request.

    src/plantrx.cpp refuses to dispatch a node update when update_mode or firmware_pull is set:
    the panel is the board that relays the command and then watches the node's progress reports,
    and it is about to stand down and reboot into its own update. So the response that carries
    the panel's arming must not spend the node's - if it did, the operator's request would
    disappear leaving nothing behind but a log line, on the one response guaranteed to ignore it.
    """
    device = next(_DEV)
    store.set_update_mode(device, pull=True)
    _arm(device, "cam")

    taken = _poll(device)
    assert taken["update_mode"] is True, taken
    assert taken["firmware_pull"] is True, taken
    assert _pulls(taken) == (False, False), (
        "the node arming was spent on the response that stands the panel down: %r" % (
            _pulls(taken),))

    # The panel reboots into its own image and polls again. This is where the node update was
    # always going to happen, and it is also the first moment somebody can watch it.
    after = _poll(device)
    assert after["update_mode"] is False, after
    assert _pulls(after) == (True, False), after
    assert _pulls(_poll(device)) == (False, False), after
    return "the cam arming waits out the panel's own update and lands on the poll after it"


def test_only_the_two_node_roles_can_be_armed():
    """panel is refused, and so is anything that is not a role at all.

    The panel updates itself through update_mode and fwpull.cpp. Arming it here would be a
    device asked to watch its own reboot, and the flag has no column to land in - so this is a
    400 at the edge rather than a KeyError three frames down.
    """
    device = next(_DEV)
    for bad in ("panel", "cam2", "", "CAM"):
        r = CLIENT.post("/v1/device/%s/node_update" % device, params={"role": bad})
        assert r.status_code == 400, (bad, r.status_code, r.text)
    missing = CLIENT.post("/v1/device/%s/node_update" % device)
    assert missing.status_code == 422, (missing.status_code, missing.text)
    assert _pulls(_poll(device)) == (False, False), "a refused arming armed something"
    return "panel and three non-roles refused, a missing role is a 422"


def test_the_arming_route_needs_the_bearer():
    """Same shared secret as everything else on /v1.

    This one writes a flag that ends in a board rewriting its own flash, so an unauthenticated
    caller reaching it is not a privacy problem but a way to knock a camera off the wall.

    DEVICE_TOKEN is put back in a finally for the reason tests/test_firmware_roles.py gives: it
    is a module global and the other test files here poll unauthenticated on purpose.
    """
    device = next(_DEV)
    was = main.DEVICE_TOKEN
    main.DEVICE_TOKEN = "test-token"
    try:
        url = "/v1/device/%s/node_update" % device
        assert CLIENT.post(url, params={"role": "cam"}).status_code == 401
        assert CLIENT.post(url, params={"role": "cam"},
                           headers={"Authorization": "Bearer wrong"}).status_code == 401
        ok = CLIENT.post(url, params={"role": "cam"},
                         headers={"Authorization": "Bearer test-token"})
        assert ok.status_code == 200, ok.text
    finally:
        main.DEVICE_TOKEN = was
    assert _pulls(_poll(device)) == (True, False), "the authenticated arming did not land"
    return "the arming route 401s without the bearer and arms with it"


if __name__ == "__main__":
    print("default   %s" % test_a_panel_nobody_armed_gets_both_flags_false())
    print("oneshot   %s" % test_arming_one_role_rides_out_once_and_is_then_gone())
    print("both      %s" % test_arming_both_roles_delivers_both_on_one_poll())
    print("rearm     %s" % test_re_arming_the_same_role_is_not_cumulative())
    print("takeover  %s" % test_a_node_arming_survives_the_panels_own_update_mode())
    print("roles     %s" % test_only_the_two_node_roles_can_be_armed())
    print("auth      %s" % test_the_arming_route_needs_the_bearer())
    print("OK")
