"""/v1/identify: the shape the device's JSON scanner can actually read.

    cd server && python tests/test_identify_endpoint.py

Plain asserts and no test framework, for the reason tests/test_display_contract.py
gives: the image installs requirements.txt and a runner in there would ship to
production for nothing. pytest collects this file unchanged.

This endpoint exists because the panel cannot speak TLS - the two mbedTLS record
buffers do not fit in internal DRAM beside the camera's PSRAM allocations - so it
POSTs the JPEG here over plain HTTP and this server holds the PlantNet keys. What
comes back is parsed on the device by the strstr scanners in src/plantid.cpp
(`json_str`, `json_num`), which cannot walk nesting and cannot read a null. So the
assertions below are not style checks: a nested object, an array, a null, or a
`reason` left on a successful reply is a card that draws wrong on the wall, and
none of it raises anywhere between here and there.

PlantNet itself is never called. Every test stubs plantnet.identify_ex or leaves
the server unconfigured, because what is under test is the wire shape and the
quota arithmetic, not the third party.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["PLANTRX_DB"] = os.path.join(tempfile.mkdtemp(), "identify.db")
os.environ.pop("PLANTRX_TOKEN", None)  # auth is not what this file is about

from fastapi.testclient import TestClient  # noqa: E402

from app import main, plantnet  # noqa: E402

CLIENT = TestClient(main.app)

# Two keys, so quota_total() is a product and not just _KEY_DAILY_QUOTA read back.
KEYS = "test-key-a,test-key-b"
QUOTA = 2 * plantnet._KEY_DAILY_QUOTA

# A valid-enough JPEG: the FF D8 lead is the only byte the server looks at before
# handing the blob to PlantNet, and the stubs never look at all.
JPEG = b"\xff\xd8\xff\xd9" * 64


def _identify(answer, keys=KEYS, remaining=None):
    """POST one frame with identify_ex stubbed, and give back the parsed body.

    `answer` is the (result, reason) pair identify_ex would have returned, or an
    exception for it to raise instead.
    `keys` None leaves PLANTNET_API_KEYS unset, which is the unconfigured server.
    `remaining` pre-loads plantnet's per-key counters, standing in for keys that
    have already answered once - the only thing that makes the quota measured.
    """
    async def stub(jpeg):
        assert jpeg == JPEG, "the route handed PlantNet a different body"
        if isinstance(answer, BaseException):
            raise answer
        return answer

    keep = (plantnet.identify_ex, os.environ.get("PLANTNET_API_KEYS"))
    plantnet.identify_ex = stub
    if keys is None:
        os.environ.pop("PLANTNET_API_KEYS", None)
    else:
        os.environ["PLANTNET_API_KEYS"] = keys
    # Rotation state is module-global and survives between tests. _roll_day()
    # first, on purpose: it is what stamps plantnet._day, and every later reader
    # calls it and wipes the counters on a day it has not seen - which on the
    # first call of the process is every day, so a dict pre-loaded before that
    # stamp would be cleared out from under the assertions.
    plantnet._roll_day()
    plantnet._remaining.clear()
    plantnet._spent.clear()
    plantnet._remaining.update(remaining or {})
    try:
        r = CLIENT.post("/v1/identify", content=JPEG,
                        headers={"Content-Type": "image/jpeg"})
        assert r.status_code == 200, r.text
        return r.json()
    finally:
        plantnet.identify_ex = keep[0]
        plantnet._remaining.clear()
        plantnet._spent.clear()
        if keep[1] is None:
            os.environ.pop("PLANTNET_API_KEYS", None)
        else:
            os.environ["PLANTNET_API_KEYS"] = keep[1]


def _check_flat(body):
    """No nesting, no arrays, no nulls. All three are unreadable on the device."""
    for key, value in body.items():
        assert not isinstance(value, (dict, list)), \
            "%s is nested; json_str cannot walk into it" % key
        assert value is not None, \
            "%s came back null; the scanner finds the key and then reads nothing" % key


ALWAYS = {"ok", "remaining", "quota", "measured"}
ON_OK = {"sci", "common", "korean", "score"}
ON_FAIL = {"reason"}


def test_success_is_the_flat_card():
    """A hit: the four name fields, the score, the quota, and nothing else.

    `reason` has to be absent rather than empty. The device prints whatever it
    finds under that key when ok is false, and an empty string surviving onto a
    successful reply is one refactor away from being printed anyway.
    """
    found = plantnet.SpeciesResult(sci="Geum urbanum", common="Wood avens",
                                   korean="뱀무", score=0.8123)
    body = _identify((found, ""))

    _check_flat(body)
    assert set(body) == ALWAYS | ON_OK, sorted(body)
    assert body["ok"] is True
    assert body["sci"] == "Geum urbanum"
    assert body["common"] == "Wood avens"
    assert body["korean"] == "뱀무"
    assert abs(body["score"] - 0.8123) < 1e-6
    assert 0.0 <= body["score"] <= 1.0, "score is a fraction, not a percent"
    # Nothing has answered yet, so the count is the ceiling and says so.
    assert body["remaining"] == QUOTA
    assert body["quota"] == QUOTA
    assert body["measured"] is False
    return body


def test_failure_carries_the_korean_reason():
    """A miss: ok false, the Korean line, and not one of the name fields.

    The panel prints `reason` verbatim, so it travels as the string plantnet
    chose - the server does not map it to a code the device would have to own a
    second table for.
    """
    body = _identify((None, "결과 없음"))

    _check_flat(body)
    assert set(body) == ALWAYS | ON_FAIL, sorted(body)
    assert body["ok"] is False
    assert body["reason"] == "결과 없음"
    for key in ON_OK:
        assert key not in body, "%s survived a failed identification" % key
    return body


def test_a_crash_below_the_endpoint_still_answers():
    """An unexpected exception has to arrive as the contract, not as the poll's.

    main._unhandled now answers a non-poll route with a 500 rather than the poll's
    200 (see tests/test_nodelog.py), so an escape from this route would be a 500
    with no `ok` and no `reason` - a blank card and no way to say why. Either way
    the answer has to be this route's contract, so the route absorbs the exception
    itself. The negative control is built in: remove the guard and this test sees
    something that is not the identify shape and fails on the key set.
    """
    body = _identify(RuntimeError("plantnet blew up"))

    _check_flat(body)
    assert set(body) == ALWAYS | ON_FAIL, sorted(body)
    assert body["ok"] is False
    assert body["reason"] == "식별 서버 오류"
    # The quota still travels: the panel redraws the whole card from this reply,
    # and a crash is no reason to tell it the server has no keys.
    assert body["remaining"] == QUOTA
    assert body["quota"] == QUOTA
    return body


def test_unconfigured_server_reports_minus_one():
    """No keys at all: the real identify_ex, no network, and -1 for both figures.

    -1 rather than 0 because the panel has to be able to tell "the server has no
    keys" from "today's requests are gone" - the first is somebody's .env, the
    second is tomorrow. The reason distinguishes them too, but the card draws the
    numbers whether or not anyone reads the line.
    """
    keep = os.environ.get("PLANTNET_API_KEYS")
    os.environ.pop("PLANTNET_API_KEYS", None)
    try:
        r = CLIENT.post("/v1/identify", content=JPEG,
                        headers={"Content-Type": "image/jpeg"})
    finally:
        if keep is not None:
            os.environ["PLANTNET_API_KEYS"] = keep
    assert r.status_code == 200, r.text
    body = r.json()

    _check_flat(body)
    assert set(body) == ALWAYS | ON_FAIL, sorted(body)
    assert body["ok"] is False
    assert body["reason"] == "키 미설정"
    assert body["remaining"] == -1
    assert body["quota"] == -1
    assert body["measured"] is False
    return body


def test_measured_once_every_key_has_answered():
    """`measured` is the difference between a count and an upper bound.

    quota_remaining() counts a key PlantNet has never answered for as holding a
    full day, so the total is an estimate until every key has been used once.
    Both keys pre-loaded here, so the figure is real and the flag says so.
    """
    found = plantnet.SpeciesResult(sci="Bellis perennis", common="Daisy",
                                   korean="데이지", score=0.42)
    left = {"test-key-a": 120, "test-key-b": 300}
    body = _identify((found, ""), remaining=left)

    assert body["measured"] is True
    assert body["remaining"] == 420
    assert body["quota"] == QUOTA

    # The negative control: drop one key's answer and the same total goes back to
    # being a ceiling, or the flag is not measuring anything.
    partial = _identify((found, ""), remaining={"test-key-a": 120})
    assert partial["measured"] is False
    assert partial["remaining"] == 120 + plantnet._KEY_DAILY_QUOTA
    return body


def test_oversized_body_is_refused():
    """413 on a frame that will not fit, and on no frame at all.

    These are the framework's own refusals and are the one thing here that is not
    a 200: the device reports a non-200 itself rather than parsing it, so the
    body does not matter and the status has to.
    """
    for label, blob in (("oversized", b"\xff\xd8" + b"\x00" * main.MAX_FRAME_BYTES),
                        ("empty", b"")):
        r = CLIENT.post("/v1/identify", content=blob,
                        headers={"Content-Type": "image/jpeg"})
        assert r.status_code == 413, "%s body: HTTP %d" % (label, r.status_code)


if __name__ == "__main__":
    ok = test_success_is_the_flat_card()
    bad = test_failure_carries_the_korean_reason()
    none = test_unconfigured_server_reports_minus_one()
    crash = test_a_crash_below_the_endpoint_still_answers()
    test_measured_once_every_key_has_answered()
    test_oversized_body_is_refused()
    print("hit     %s" % ok)
    print("miss    %s" % bad)
    print("no keys %s" % none)
    print("crash   %s" % crash)
    print("flat, no nulls, no key outside the contract; 413 on oversized and empty")
    print("OK")
