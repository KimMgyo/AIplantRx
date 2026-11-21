#!/usr/bin/env python3
"""Slice build_node.py's STLs with PrusaSlicer (Original Prusa i3 MK2S, 0.35mm FAST, Generic PLA) and copy
the G-code to out/sd_card with the print time in the name.  Run build_node.py first.
The body's first 3 layers (0.9 mm: the diffuser skin under the ring) want white / natural PLA, so its G-code
gets a filament change (M600, the profile's own color_change_gcode) at the start of the 4th layer."""
from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
SLICER = r"C:\Program Files\Prusa3D\PrusaSlicer\prusa-slicer-console.exe"
JOBS = {"sn_01_body_M600": "node_body", "sn_03_scd_cap": "node_scd_cap", "sn_04_lid": "node_lid"}
M600_AT = {"sn_01_body_M600": "1.25"}          # layer that starts right after DIFF_T = 0.90
M600 = ";COLOR_CHANGE,T0,#3f4550\nM117 Load body colour\nM600\nG1 E0.3 F1500 ; prime after color change\n"


def add_m600(gcode: Path, z: str) -> bool:
    """Insert the filament change after that layer's ;AFTER_LAYER_CHANGE block, before its first move."""
    g = gcode.read_text(errors="ignore")
    mark = f";AFTER_LAYER_CHANGE\n;{z}\n"
    i = g.find(f";LAYER_CHANGE\n;Z:{z}\n")
    j = g.find(mark, i)
    if i < 0 or j < 0:
        return False
    j += len(mark)
    gcode.write_text(g[:j] + M600 + g[j:])
    return True


def slice_one(stl: Path, gcode: Path):
    cmd = [SLICER, "--printer-profile", "Original Prusa i3 MK2S", "--print-profile", "0.35mm FAST",
           "--material-profile", "Generic PLA", "--loglevel", "2", "--export-gcode", "--output", str(gcode),
           str(stl)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    g = gcode.read_text(errors="ignore") if gcode.exists() else ""

    def grab(k):
        m = re.search(r"^; " + re.escape(k) + r" = (.*)$", g, re.M)
        return m.group(1).strip() if m else None
    warn = [ln for ln in (r.stdout + r.stderr).splitlines() if "warning" in ln.lower() or "error" in ln.lower()]
    return dict(ok=r.returncode == 0, time=grab("estimated printing time (normal mode)"),
                grams=grab("total filament used [g]"), support=grab("support_material"), warnings=warn[:6])


def short(t):
    h = re.search(r"(\d+)h", t or "")
    m = re.search(r"(\d+)m", t or "")
    return (f"{h.group(1)}h" if h else "") + (f"{m.group(1)}m" if m else "")


def main():
    (OUT / "gcode").mkdir(parents=True, exist_ok=True)
    sd = OUT / "sd_card"
    sd.mkdir(parents=True, exist_ok=True)
    for f in sd.glob("sn_*.gcode"):
        f.unlink()
    res = {}
    for job, stl in JOBS.items():
        g = OUT / "gcode" / f"{job}.gcode"
        r = slice_one(OUT / "stl" / f"{stl}.stl", g)
        if r["ok"] and job in M600_AT:
            r["m600_at_z"] = M600_AT[job] if add_m600(g, M600_AT[job]) else "NOT INSERTED"
        if r["ok"]:
            shutil.copy(g, sd / f"{job}_{short(r['time'])}.gcode")
        res[job] = r
        print(job, json.dumps(r, ensure_ascii=False), flush=True)
    (OUT / "slice_info.json").write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
