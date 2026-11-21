"""Send the S3's LLM requests (include/prompts.h) from the PC, to tune prompts before they go into a build.

    python tools/llm_profile_test.py                         a few test species, profile + intro
    python tools/llm_profile_test.py "Ocimum basilicum"      one species
    python tools/llm_profile_test.py --genus Echeveria --family Crassulaceae   low-score case: genus/family only

The server and key: AIPX_LLM_URL (default https://2.tailb0610e.ts.net) and AIPX_LLM_KEY; without the key
it reads C:\\llm\\api-keys.txt on the school PC over `ssh 2`. The key is never printed.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROMPTS = os.path.join(HERE, "..", "include", "prompts.h")
URL = os.environ.get("AIPX_LLM_URL", "https://2.tailb0610e.ts.net").rstrip("/") + "/v1/chat/completions"

# What the S3's plant table (include/plants_ko.h) would add for these species.
KNOWN = {"Ocimum basilicum": ("바질", "herb"), "Monstera deliciosa": ("몬스테라", "tropical_foliage"),
         "Echeveria elegans": ("에케베리아 엘레강스", "succulent"), "Solanum lycopersicum": ("토마토", "fruiting_veg"),
         "Epipremnum aureum": ("스킨답서스", "tropical_foliage"), "Lactuca sativa": ("상추", "leafy_veg")}

TESTS = [
    ("Ocimum basilicum", "Ocimum", "Lamiaceae", ["Basil", "Sweet basil"]),
    ("Monstera deliciosa", "Monstera", "Araceae", ["Swiss cheese plant"]),
    ("Echeveria elegans", "Echeveria", "Crassulaceae", ["Mexican snowball"]),
    ("Solanum lycopersicum", "Solanum", "Solanaceae", ["Tomato"]),
    ("Epipremnum aureum", "Epipremnum", "Araceae", ["Golden pothos"]),
    ("Lactuca sativa", "Lactuca", "Asteraceae", ["Lettuce"]),
]


def prompts():
    s = open(PROMPTS, encoding="utf-8").read()
    return {m.group(1): m.group(2) for m in re.finditer(r'static const char (\w+)\[\] = R"P\((.*?)\)P";', s, re.S)}


def key():
    k = os.environ.get("AIPX_LLM_KEY")
    if k:
        return k
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "2", "(Get-Content C:\\llm\\api-keys.txt -Raw).Trim()"],
                       capture_output=True, text=True, timeout=30)
    return r.stdout.strip()


def ask(k, system, user, schema, name, max_tokens):
    body = {
        "model": "bonsai-2-27b",
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        "temperature": 0.3, "top_p": 0.8, "top_k": 20, "max_tokens": max_tokens,
        "chat_template_kwargs": {"enable_thinking": False},
        "response_format": {"type": "json_schema", "json_schema": {"name": name, "strict": True, "schema": json.loads(schema)}},
    }
    req = urllib.request.Request(URL, json.dumps(body).encode(), {"Content-Type": "application/json", "Authorization": "Bearer " + k})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=60) as r:
        d = json.loads(r.read())
    dt = time.time() - t0
    content = d["choices"][0]["message"]["content"]
    return json.loads(content), dt, d.get("usage", {}).get("completion_tokens")


def user_msg(sci, genus, family, common, score=0.8):
    if score < 0.15:
        return f"Pl@ntNet is not sure of the species. Genus: {genus}\nFamily: {family}\nGive typical indoor conditions for this genus."
    m = f"Scientific name: {sci}\nGenus: {genus}\nFamily: {family}\nCommon names: {', '.join(common) or '-'}\nPl@ntNet score: {score:.2f}"
    if sci in KNOWN:
        m += f"\nKorean name: {KNOWN[sci][0]}\nGroup: {KNOWN[sci][1]}"
    return m


# The same message the S3 builds (identify.cpp, intro_request).
def intro_msg(prof, sci):
    return "\n".join([
        f"Name: {prof['korean_name']}",
        f"Scientific name: {sci}",
        f"Group: {prof['group']}",
        f"Profile: day {prof['temp_day_c']['min']}-{prof['temp_day_c']['max']} C, soil moisture "
        f"{prof['soil_moisture_pct']['min']}-{prof['soil_moisture_pct']['max']} %, water every {prof['water_interval_days']} days",
        f"Care tip: {prof['tip_ko']}",
    ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("species", nargs="?")
    ap.add_argument("--genus")
    ap.add_argument("--family")
    ap.add_argument("--no-intro", action="store_true")
    a = ap.parse_args()
    p = prompts()
    k = key()
    if not k:
        sys.exit("no LLM key")
    if a.genus:
        tests = [("", a.genus, a.family or "", [], 0.05)]
    elif a.species:
        tests = [(a.species, a.species.split()[0], a.family or "", [], 0.8)]
    else:
        tests = [t + (0.8,) for t in TESTS]
    for sci, genus, family, common, score in tests:
        prof, dt, n = ask(k, p["PROMPT_PROFILE_SYSTEM"], user_msg(sci, genus, family, common, score), p["PROMPT_PROFILE_SCHEMA"], "plant_profile", 700)
        print(f"\n== {sci or genus}  ({dt:.1f} s, {n} tokens)")
        print(json.dumps(prof, ensure_ascii=False))
        if not a.no_intro:
            u = intro_msg(prof, sci or genus)
            intro, dt2, n2 = ask(k, p["PROMPT_INTRO_SYSTEM"], u, p["PROMPT_INTRO_SCHEMA"], "plant_intro", 300)
            print(f"   intro ({dt2:.1f} s, {len(intro['intro_ko'])} chars): {intro['intro_ko']}")


if __name__ == "__main__":
    main()
