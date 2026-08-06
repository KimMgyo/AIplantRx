"""PlantNet species identification, moved off the device.

The firmware did this itself (src/plantid.cpp): the API keys were compiled into
include/plantnet_config.h and the TLS session ran with `setInsecure()`, because
the ESP32 carries no CA bundle. A flash dump therefore leaks every key, and the
"secure" upload was unauthenticated. Here the keys live in the environment and
the certificate is actually verified.

The request and response handling below mirror plantid.cpp field for field so
the two can be diffed; deviations are commented.

Identification is a nice-to-have signal, never a critical path: every ordinary
failure logs and returns None rather than raising. identify_ex() returns that
same None paired with the Korean line the panel prints, because the caller that
is a button somebody pressed has to be able to say why the card came back empty.
"""

from __future__ import annotations

import asyncio
import logging
import os
from datetime import date, datetime, timezone
from typing import Any, Optional

import httpx
from pydantic import BaseModel

log = logging.getLogger(__name__)

_PLANTNET_URL = "https://my-api.plantnet.org/v2/identify/all"

# Was PLANTNET_LANG on the device. Only affects which common names come back;
# the scientific name is returned regardless, and _resolve_korean() does the
# Korean separately from the scientific name either way.
_LANG = "en"

# Free plan, per key, per UTC day. Only used to report a total before a key has
# ever answered - PlantNet tells us the real figure in
# `remainingIdentificationRequests` on the first reply, which is why
# quota_is_measured() exists and why the panel prints 최대 until it is true.
_KEY_DAILY_QUOTA = 500

# PlantNet runs inference on the upload; the firmware allowed 25 s overall.
_TIMEOUT = httpx.Timeout(30.0, connect=10.0)
_LOOKUP_TIMEOUT = httpx.Timeout(8.0, connect=5.0)

_UA = "SmartFarm-Server/1.0"

# One identification at a time, as on the device ("one at a time (protects the
# quota)"). Two concurrent polls must not spend two requests on one plant.
_lock = asyncio.Lock()

# Rotation state, keyed by the key string so it survives an env reload.
_key_idx = 0
_remaining: dict[str, int] = {}
_spent: set[str] = set()
_day: Optional[date] = None


class SpeciesResult(BaseModel):
    sci: str
    common: str
    korean: str
    score: float
    source: str = "plantnet"


def _keys() -> list[str]:
    # Read every call: Dokploy restarts inject env, and tests set it late.
    return [k.strip() for k in os.environ.get("PLANTNET_API_KEYS", "").split(",") if k.strip()]


def is_configured() -> bool:
    return bool(_keys())


def _roll_day() -> None:
    global _day
    today = datetime.now(timezone.utc).date()
    if _day != today:
        _day = today
        _spent.clear()
        _remaining.clear()


def _spend(key: str) -> None:
    _spent.add(key)
    _remaining[key] = 0


def quota_remaining() -> Optional[int]:
    """Requests left today across all keys, or None when unconfigured.

    Mirrors plantid_total_remaining(): a key that has not answered yet counts as
    a full quota, so this is an upper bound until every key has been used once.
    """
    keys = _keys()
    if not keys:
        return None
    _roll_day()
    return sum(_remaining.get(k, _KEY_DAILY_QUOTA) for k in keys)


def quota_total() -> Optional[int]:
    """Requests per day across all keys, or None when unconfigured.

    The companion figure to quota_remaining(), and it carries the same caveat
    from the other side: this is the nominal free-plan allowance per key, not a
    number PlantNet ever confirms, so it is what "x of y left" is measured
    against rather than something to reconcile against.
    """
    keys = _keys()
    if not keys:
        return None
    return len(keys) * _KEY_DAILY_QUOTA


def quota_is_measured() -> bool:
    """Whether quota_remaining() is a count or still the upper bound.

    True only once every configured key has answered at least once, which is
    exactly when no key is being counted at a full quota it may not have. The
    panel prints the figure either way; this is what lets it mark the estimate
    as an estimate instead of walking a number back later.
    """
    keys = _keys()
    if not keys:
        return False
    _roll_day()
    return all(k in _remaining for k in keys)


def _rotation(keys: list[str]) -> list[str]:
    ordered = keys[_key_idx % len(keys):] + keys[: _key_idx % len(keys)]
    return [k for k in ordered if k not in _spent]


def _first_result(payload: dict[str, Any]) -> Optional[dict[str, Any]]:
    results = payload.get("results")
    if isinstance(results, list) and results and isinstance(results[0], dict):
        return results[0]
    return None


def _parse(payload: dict[str, Any]) -> Optional[tuple[str, str, float]]:
    """(scientific name, common name, score) from an identify reply.

    Same precedence as the firmware: results[0].species.scientificNameWithoutAuthor
    (no authorship suffix), falling back to the top-level bestMatch string.
    """
    top = _first_result(payload) or {}
    species = top.get("species") if isinstance(top.get("species"), dict) else {}

    sci = species.get("scientificNameWithoutAuthor") or payload.get("bestMatch") or ""
    if not isinstance(sci, str) or not sci:
        return None  # "결과 없음" on the device

    score = top.get("score")
    score = float(score) if isinstance(score, (int, float)) else 0.0

    # commonNames is frequently an empty array; that is not an error.
    common = ""
    names = species.get("commonNames")
    if isinstance(names, list) and names and isinstance(names[0], str):
        common = names[0]

    return sci, common, score


def _note_remaining(key: str, payload: dict[str, Any]) -> None:
    left = payload.get("remainingIdentificationRequests")
    if not isinstance(left, (int, float)):
        return
    left = int(left)
    _remaining[key] = max(left, 0)
    if left <= 0:
        _spent.add(key)  # exhausted without a 429; do not waste a round trip on it


_HANGUL_SYLLABLES = (0xAC00, 0xD7A3)


def _has_hangul(s: str) -> bool:
    # The firmware sniffs UTF-8 lead bytes 0xEA-0xED, i.e. the syllable block.
    # A romanised or English title must not be shown as the Korean name.
    return any(_HANGUL_SYLLABLES[0] <= ord(c) <= _HANGUL_SYLLABLES[1] for c in s)


async def _wikipedia_ko(client: httpx.AsyncClient, sci: str) -> str:
    r = await client.get(
        "https://en.wikipedia.org/w/api.php",
        params={
            "action": "query",
            "format": "json",
            "utf8": 1,
            "redirects": 1,
            "prop": "langlinks",
            "lllang": "ko",
            "titles": sci,
        },
        timeout=_LOOKUP_TIMEOUT,
    )
    r.raise_for_status()
    pages = r.json().get("query", {}).get("pages", {})
    for page in pages.values():
        for link in page.get("langlinks") or []:
            title = link.get("*") or link.get("title") or ""
            if isinstance(title, str) and _has_hangul(title):
                return title
    return ""


async def _translate_ko(client: httpx.AsyncClient, text: str) -> str:
    r = await client.get(
        "https://translate.googleapis.com/translate_a/single",
        params={"client": "gtx", "sl": "en", "tl": "ko", "dt": "t", "q": text},
        timeout=_LOOKUP_TIMEOUT,
    )
    r.raise_for_status()
    # [[["번역","source",...],...],...] - the device takes the first quoted string,
    # which is this same element.
    body = r.json()
    try:
        out = body[0][0][0]
    except (TypeError, IndexError, KeyError):
        return ""
    return out if isinstance(out, str) and _has_hangul(out) else ""


async def _resolve_korean(client: httpx.AsyncClient, sci: str, common: str) -> str:
    """English Wikipedia -> Korean langlink on the scientific name; if there is
    no Korean article, machine-translate the common name. Same two steps as
    resolve_korean() in plantid.cpp.

    Optional and non-fatal: a plant with no Korean name still identifies.
    """
    if sci:
        try:
            ko = await _wikipedia_ko(client, sci)
            if ko:
                return ko
        except (httpx.HTTPError, ValueError) as e:
            log.info("korean name: wikipedia lookup failed for %r: %s", sci, e)

    src = common or sci
    if not src:
        return ""
    try:
        return await _translate_ko(client, src)
    except (httpx.HTTPError, ValueError) as e:
        log.info("korean name: translation failed for %r: %s", src, e)
        return ""


async def identify_ex(jpeg: bytes) -> tuple[Optional[SpeciesResult], str]:
    """Identify, and say why not when it did not.

    The reason is "" on success and otherwise one of the five Korean strings the
    panel prints verbatim, picked at the same points that already log the
    failure. It exists because /v1/identify is a button somebody pressed and
    stood in front of: a card that just goes blank tells them nothing about
    whether to retake the photo, wait for tomorrow's quota, or call somebody.
    The automatic path on the poll has no one watching and uses identify()
    below, which throws the reason away.
    """
    keys = _keys()
    if not keys:
        log.debug("plantnet: PLANTNET_API_KEYS unset, skipping identification")
        return None, "키 미설정"
    if jpeg[:2] != b"\xff\xd8":
        log.warning("plantnet: payload is not a JPEG (%d bytes)", len(jpeg))
        return None, "사진 형식 오류"

    global _key_idx
    async with _lock:
        _roll_day()
        order = _rotation(keys)
        if not order:
            log.warning("plantnet: every key is spent for today")
            return None, "일일 사용량 초과"

        async with httpx.AsyncClient(timeout=_TIMEOUT, headers={"User-Agent": _UA}) as client:
            resp: Optional[httpx.Response] = None
            used = ""
            for key in order:
                try:
                    resp = await client.post(
                        _PLANTNET_URL,
                        params={
                            "api-key": key,
                            "nb-results": 3,
                            "lang": _LANG,
                            # Keep the low-confidence top hit instead of an empty
                            # result set - the card shows the score alongside.
                            "no-reject": "true",
                        },
                        # The device omits `organs`, so PlantNet applies its
                        # "auto" default; sent explicitly here so the shape is
                        # visible in a request log.
                        data={"organs": "auto"},
                        files={"images": ("plant.jpg", jpeg, "image/jpeg")},
                    )
                except httpx.HTTPError as e:
                    log.warning("plantnet: request failed: %s", e)
                    # transport failure is not a quota problem; do not rotate
                    return None, "식별 서버 오류"
                used = key
                if resp.status_code == 429:
                    _spend(key)
                    _key_idx = (keys.index(key) + 1) % len(keys)
                    continue
                break

            if resp is None or resp.status_code == 429:
                log.warning("plantnet: daily quota exhausted on every key")
                # A 429 on the last key left is the same fact the empty rotation
                # above reports, one round trip later, so it reads the same way.
                return None, "일일 사용량 초과"

            try:
                payload = resp.json()
            except ValueError:
                log.warning("plantnet: HTTP %d with non-JSON body", resp.status_code)
                return None, "식별 서버 오류"
            if not isinstance(payload, dict):
                log.warning("plantnet: unexpected reply of type %s", type(payload).__name__)
                return None, "식별 서버 오류"

            if resp.status_code != 200:
                log.warning("plantnet: API %d %s", resp.status_code, payload.get("message", ""))
                return None, "식별 서버 오류"

            _note_remaining(used, payload)

            parsed = _parse(payload)
            if parsed is None:
                log.info("plantnet: no match")
                return None, "결과 없음"
            sci, common, score = parsed

            korean = await _resolve_korean(client, sci, common)

    return SpeciesResult(sci=sci, common=common, korean=korean, score=score), ""


async def identify(jpeg: bytes) -> Optional[SpeciesResult]:
    """identify_ex() for the automatic path, which has nobody to explain itself to."""
    result, _reason = await identify_ex(jpeg)
    return result
