"""Send the S3's AI-note request (PROMPT_TALK_SYSTEM and TALK_GRAMMAR, include/prompts.h) from the PC with
sample triggers, to tune the prompt before it goes into a build.

    python tools/llm_talk_test.py [rounds]

Same server and key handling as llm_profile_test.py."""
import json
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from llm_profile_test import PROMPTS, URL, key, prompts  # noqa: E402

BASE = {"plant": "몬스테라", "time": "14:21", "phase": "day", "air_c": 24.6, "rh": 58, "co2": 820, "leaf_c": 23.1,
        "leaf_minus_air": -1.5, "soil_pct": 46, "ec": 1.2, "ph": 6.4,
        "targets": {"temp_day_c": [20, 28], "soil_moisture_pct": [30, 50], "soil_ec": [0.5, 1.5], "soil_ph": [5.5, 6.5]},
        "recent": ["09:40 물 39ml", "09:10 물약 5ml"], "said": ["흙이 28%까지 말라서 물을 39ml 줬어요."]}
TRIGGERS = [
    {"kind": "hand", "hand_c": 33.8, "hand_minus_leaf": 10.7},
    {"kind": "device", "device": "fertiliser", "ml": 5},
    {"kind": "device", "device": "water", "ml": 39},
    {"kind": "value", "rule": "soil_dry", "value": 27, "target": [30, 50]},
    {"kind": "value", "rule": "water_not_absorbed", "value": 39, "change": 0.4},
    {"kind": "new_plant", "identification_confidence_pct": 83},
    {"kind": "idle", "minutes": 10},
]
HAN = re.compile(r"[぀-ヿ㐀-䶿一-鿿]")


def grammar():
    """TALK_GRAMMAR as the C compiler sees it (the header writes it as an escaped C string)."""
    s = open(PROMPTS, encoding="utf-8").read()
    m = re.search(r'static const char TALK_GRAMMAR\[\] = "((?:[^"\\]|\\.)*)";', s)
    return m.group(1).encode().decode("unicode_escape")


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    p, g, k = prompts(), grammar(), key()
    print("grammar:", g)
    bad = 0
    for _ in range(rounds):
        for t in TRIGGERS:
            body = {"model": "bonsai-2-27b",
                    "messages": [{"role": "system", "content": p["PROMPT_TALK_SYSTEM"]},
                                 {"role": "user", "content": json.dumps(dict(BASE, trigger=t), ensure_ascii=False)}],
                    "temperature": 0.8, "top_p": 0.95, "max_tokens": 120, "grammar": g,
                    "chat_template_kwargs": {"enable_thinking": False}}
            req = urllib.request.Request(URL, json.dumps(body).encode(), {"Content-Type": "application/json", "Authorization": "Bearer " + k})
            t0 = time.time()
            d = json.loads(urllib.request.urlopen(req, timeout=60).read())
            text = d["choices"][0]["message"]["content"]
            han = bool(HAN.search(text))
            bad += han
            print(f"{t['kind']:<10} {time.time() - t0:4.1f}s {'HAN ' if han else ''} {text}")
    print("notes with Chinese/Japanese characters:", bad)


if __name__ == "__main__":
    main()
