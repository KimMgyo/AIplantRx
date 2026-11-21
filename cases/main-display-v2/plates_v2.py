#!/usr/bin/env python3
"""Print plates for build_v2.py: one G-code per part group plus a short fit-test plate whose coupons are cut
straight from the real parts (same geometry, same bed orientation), then PrusaSlicer for all of them.
No brim and no mouse ears on any part (user's choice): clean the PEI with IPA, 60 C bed."""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from build123d import Compound, Pos, Rot, export_stl  # noqa: E402
import build_v2 as b  # noqa: E402

OUT = HERE / "out"
SLICER = r"C:\Program Files\Prusa3D\PrusaSlicer\prusa-slicer-console.exe"
EAR_R, EAR_T = 8.0, b.L(1)          # 0.55: first layer + one layer


def ears(s, pts):
    return s.fuse(*[b.cyl(x, y, EAR_R, 0.0, EAR_T) for x, y in pts])


def corners(s, inset=2.0):
    bb = s.bounding_box()
    return [(bb.min.X + inset, bb.min.Y + inset), (bb.max.X - inset, bb.min.Y + inset),
            (bb.min.X + inset, bb.max.Y - inset), (bb.max.X - inset, bb.max.Y - inset)]


def row(items, gap=4.0):
    """Lay bed-oriented solids left to right, centred on y = 0."""
    out, x = [], 0.0
    for s in items:
        bb = s.bounding_box()
        out.append(Pos(x - bb.min.X, -(bb.min.Y + bb.max.Y) / 2, 0) * s)
        x += bb.size.X + gap
    return out


def stack(rows, gap=4.0):
    out, y = [], 0.0
    for r in rows:
        c = Compound(r)
        bb = c.bounding_box()
        out.append(Pos(-(bb.min.X + bb.max.X) / 2, y - bb.min.Y, 0) * c)
        y += bb.size.Y + gap
    c = Compound(out)
    bb = c.bounding_box()
    return Pos(-(bb.min.X + bb.max.X) / 2, -(bb.min.Y + bb.max.Y) / 2, 0) * c


def coupons(p):
    """Fit-test pieces cut from the real parts, returned in bed orientation."""
    c = {}
    # top seam: spine end band (counterbore) + left wall (boss with nut slot, dovetail socket) + right wall tongue
    y0, y1 = 47.0, b.OUT_TOP + 0.5
    c["t1_spine_band_screw"] = b.to_bed("v2_03_spine", p["v2_03_spine"] & b.box(-34.0, -22.0, y0, y1, b.Z_BACK - 1, 5))
    c["t2_wall_left_seam"] = b.to_bed("v2_04_wall_left", p["v2_04_wall_left"] & b.box(-34.0, b.SEAM_X, y0, y1, -40, 5))
    c["t3_wall_right_seam"] = b.to_bed("v2_05_wall_right", p["v2_05_wall_right"] & b.box(b.SEAM_X - 6.0, 8.0, y0, y1, -40, 5))
    xs0, xs1, ys1 = b.LEG_X - 6.5, b.LEG_X + 6.5, -68.0
    c["t4_spine_slot"] = b.to_bed("v2_03_spine", p["v2_03_spine"] & b.box(xs0, xs1, b.OUT_BOT - 1, ys1, b.Z_BACK - 1, b.RIM_TOP + 0.01))
    c["t5_leg_tongue"] = b.to_bed("v2_10_leg", p["v2_10_leg"] & b.box(0, 50, b.OUT_BOT - 1, ys1 - 3.0, b.Z_BACK - 14.0, b.Z_BACK + 6.0))
    c["t6_oled_body"] = b.to_bed("v2_06_oled_body", p["v2_06_oled_body"])
    c["t7_oled_lid"] = b.to_bed("v2_07_oled_lid", p["v2_07_oled_lid"])
    c["t8_c3_cart"] = b.to_bed("v2_08_c3_cart", p["v2_08_c3_cart"])
    c["t9_c3_lid"] = b.to_bed("v2_09_c3_lid", p["v2_09_c3_lid"])
    return c


def plates(p):
    bed = {n: b.to_bed(n, s) for n, s in p.items()}
    pl = {}
    for n in ("v2_01_pod_left", "v2_02_pod_right", "v2_03_spine"):
        pl[n] = bed[n]
    pl["v2_04_wall_left"] = bed["v2_04_wall_left"]
    pl["v2_05_wall_right"] = bed["v2_05_wall_right"]
    pl["v2_06_07_oled_cells_x5"] = stack([row([bed["v2_06_oled_body_end_L"]] + [bed["v2_06_oled_body"]] * 3
                                              + [bed["v2_06_oled_body_end_R"]], gap=3.0),
                                          row([bed["v2_07_oled_lid"]] * 5)])
    pl["v2_06_07_oled_cell_x1"] = Compound(row([bed["v2_06_oled_body"], bed["v2_07_oled_lid"]], gap=6.0))
    pl["v2_08_09_c3_cartridge"] = Compound(row([bed["v2_08_c3_cart"], bed["v2_09_c3_lid"]], gap=6.0))
    leg = bed["v2_10_leg"]
    lb = leg.bounding_box()
    legs = row([leg, Rot(0, 0, 180) * leg], gap=6.0)
    pl["v2_10_legs_x2"] = Compound(legs)
    pl["v2_10_leg_x1"] = bed["v2_10_leg"]
    one = {n: b.to_bed(n, s) for n, s in b.onepiece_parts().items()}
    pl["v2_06_07c_oled_onepiece"] = stack([[one["v2_06c_oled_chin_onepiece"]], [one["v2_07c_oled_lid_onepiece"]]],
                                          gap=6.0)
    pl["v2_06c_oled_chin_onepiece"] = one["v2_06c_oled_chin_onepiece"]
    pl["v2_07c_oled_lid_onepiece"] = one["v2_07c_oled_lid_onepiece"]
    ev = {n: b.to_bed(n, s) for n, s in b.even_parts().items()}
    pl["v2_06_07e_oled_even"] = stack([[ev["v2_06e_oled_chin_even"]], [ev["v2_07e_oled_lid_even"]]], gap=6.0)
    pl["v2_06e_oled_chin_even"] = ev["v2_06e_oled_chin_even"]
    pl["v2_07e_oled_lid_even"] = ev["v2_07e_oled_lid_even"]
    c = coupons(p)
    pl["v2_00_fit_test"] = stack([row([c["t1_spine_band_screw"], c["t2_wall_left_seam"], c["t3_wall_right_seam"],
                                       c["t4_spine_slot"], c["t5_leg_tongue"]], gap=6.0),
                                  row([c["t6_oled_body"], c["t7_oled_lid"], c["t8_c3_cart"], c["t9_c3_lid"]], gap=6.0)],
                                 gap=8.0)
    return pl, c


def slice_one(stl, gcode, brim=0.0):
    cmd = [SLICER, "--printer-profile", "Original Prusa i3 MK2S", "--print-profile", "0.35mm FAST",
           "--material-profile", "Generic PLA", "--loglevel", "2", "--export-gcode", "--output", str(gcode)]
    if brim:
        cmd += ["--brim-width", str(brim)]
    cmd.append(str(stl))
    r = subprocess.run(cmd, capture_output=True, text=True)
    g = gcode.read_text(errors="ignore") if gcode.exists() else ""

    def grab(k):
        m = re.search(r"^; " + re.escape(k) + r" = (.*)$", g, re.M)
        return m.group(1).strip() if m else None
    warn = [ln for ln in (r.stdout + r.stderr).splitlines() if "stability" in ln.lower() or "warning" in ln.lower()]
    return dict(time=grab("estimated printing time (normal mode)"), grams=grab("total filament used [g]"),
                ok=r.returncode == 0, warnings=warn[:6])


def main():
    p = b.parts()
    pl, c = plates(p)
    (OUT / "plates").mkdir(parents=True, exist_ok=True)
    (OUT / "gcode").mkdir(parents=True, exist_ok=True)
    (OUT / "fit_test").mkdir(parents=True, exist_ok=True)
    for n, s in c.items():
        export_stl(s, str(OUT / "fit_test" / f"{n}.stl"), tolerance=0.01, angular_tolerance=0.1)
    only = set(sys.argv[1:])
    info_path = OUT / "plates_info.json"
    res = json.loads(info_path.read_text(encoding="utf-8")) if (only and info_path.exists()) else {}
    for n, s in pl.items():
        if only and n not in only:
            continue
        stl = OUT / "plates" / f"{n}.stl"
        export_stl(s, str(stl), tolerance=0.01, angular_tolerance=0.1)
        bb = s.bounding_box()
        brim = 0.0
        r = slice_one(stl, OUT / "gcode" / f"{n}.gcode", brim)
        r["bed_xy"] = [round(bb.size.X, 1), round(bb.size.Y, 1)]
        r["height"] = round(bb.size.Z, 2)
        r["brim"] = brim
        res[n] = r
        print(n, json.dumps(r, ensure_ascii=False), flush=True)
    info_path.write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()

