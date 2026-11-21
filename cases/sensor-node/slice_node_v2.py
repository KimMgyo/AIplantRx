#!/usr/bin/env python3
"""Slice build_node_v2.py's plate and frame (MK2S, 0.35mm FAST, Generic PLA) with a slicer brim, and copy the
G-code to out/v2/sd_card.  Everything is printed in white PLA: no filament change.  Run build_node_v2.py first.

Brim (PrusaSlicer, outer only, first layer only; research 2026-11-18):
  - width: Prusa advises at least 3 mm; 5..8 mm is the usual PLA range, wider for warping or tall narrow parts.
    plate 8 mm (v1's one-piece body lifted at the corners), frame 10 mm (user asked for more than 5; tall thin
    walls, 140 + 2x10 = 160 mm still fits the 250 x 210 bed; 3 mm already clears
    PrusaSlicer's low-bed-adhesion warning, 0 mm does not).
  - separation gap 0.15 mm: sources put 0.1..0.2 between hold and easy removal (Orca default 0.1, raise to
    0.15..0.2 if it will not come off).  Measured in this profile's G-code (elephant foot 0.2, first layer 0.42
    wide), the gap between the brim's inner line and the outer perimeter is setting - 0.02: 0.15 -> 0.13 mm.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess

import slice_node as s1

OUT2 = s1.OUT / "v2"
JOBS = {"sn2_01_plate": "node2_plate", "sn2_02_frame": "node2_frame"}
M600_AT = {}                      # all white (user): no M600
BRIM = {"sn2_01_plate": (8.0, 0.15), "sn2_02_frame": (10.0, 0.15)}     # (width, separation gap) mm


def slice_one(stl, gcode, brim):
    cmd = [s1.SLICER, "--printer-profile", "Original Prusa i3 MK2S", "--print-profile", "0.35mm FAST",
           "--material-profile", "Generic PLA", "--brim-type", "outer_only", "--brim-width", str(brim[0]),
           "--brim-separation", str(brim[1]), "--loglevel", "2", "--export-gcode", "--output", str(gcode), str(stl)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    g = gcode.read_text(errors="ignore") if gcode.exists() else ""

    def grab(k):
        m = re.search(r"^; " + re.escape(k) + r" = (.*)$", g, re.M)
        return m.group(1).strip() if m else None
    out = r.stdout + r.stderr
    return dict(ok=r.returncode == 0, time=grab("estimated printing time (normal mode)"),
                grams=grab("total filament used [g]"), brim=f"{grab('brim_width')} mm, gap {grab('brim_separation')} mm",
                stability_warning="stability" in out)


def main():
    (OUT2 / "gcode").mkdir(parents=True, exist_ok=True)
    sd = OUT2 / "sd_card"
    sd.mkdir(parents=True, exist_ok=True)
    for f in sd.glob("sn2_*.gcode"):
        f.unlink()
    res = {}
    for job, stl in JOBS.items():
        g = OUT2 / "gcode" / f"{job}.gcode"
        r = slice_one(OUT2 / "stl" / f"{stl}.stl", g, BRIM[job])
        if r["ok"] and job in M600_AT:
            r["m600_at_z"] = M600_AT[job] if s1.add_m600(g, M600_AT[job]) else "NOT INSERTED"
        if r["ok"]:
            shutil.copy(g, sd / f"{job}_{s1.short(r['time'])}.gcode")
        res[job] = r
        print(job, json.dumps(r, ensure_ascii=False), flush=True)
    (OUT2 / "slice_info.json").write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
