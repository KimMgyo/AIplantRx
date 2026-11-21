#!/usr/bin/env python3
"""Fit-test prints: check every part that has to fit, before printing the 11 h case.

Everything except the two gauges is cut straight out of the real parts from build_case.py, so a
coupon that fits means the full part fits.  Frame coupons are cut at a height that keeps their
layer boundaries identical to the full print (cut at a multiple of 0.35 mm above the frame's bed).

  plate A (one print): 7" bezel gauge with the coupons nested inside its window
    fit_01_bezel_gauge   glass pocket outline (0.3 gap, R8.18) + visible-area window, 0.9 mm
    fit_02_oled_cell     one GME12864 bay: window, 2.1 bosses, d1.7 locating pins, header-joint recess
    fit_03_nut_chin      frame corner block (bottom right) with its side-loaded nut slot (rim side up)
    fit_04_cover_chin    matching piece of the back cover (plate + lip) -> M3 x 10 into the nut
    fit_05_nut_top       frame top wall with its nut slot
    fit_06_cover_top     matching piece of the back cover
    fit_08_standoff      cover piece with one deep counterbore boss -> M3 x 10 into a module standoff
  plate B (one print):
    fit_09_screw_gauge   sits on the module's 4 brass standoffs, tabs touch the glass' left and bottom
                         edges: all 4 holes must line up with the standoffs
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from build123d import Compound, Plane, Pos, Rot, Text, extrude, export_stl, import_brep, ShapeList  # noqa: E402
import build_case as bc  # noqa: E402

OUT = HERE / "out" / "fit_test"
CACHE = HERE / "out" / "_cache"
CUT_Z = bc.FRONT_Z - 0.35 * 48          # -14.85: frame coupons keep the full print's layer grid


def front_down(s):
    return Rot(0, 180, 0) * s


def drop(s):
    bb = s.bounding_box()
    return Pos(-(bb.min.X + bb.max.X) / 2, -(bb.min.Y + bb.max.Y) / 2, -bb.min.Z) * s


def bezel_gauge():
    """Outline wall (where the glass edge must drop in), a 3 mm ledge the glass face rests on, the
    visible-area window with a 2.5 mm frame (its edge = the real bezel edge), and 8 spokes."""
    t, wall, ledge, wf = bc.L(2), 1.6, 3.0, 2.5
    ox, oy, ro = bc.IN_X + wall, bc.IN_TOP + wall, bc.IN_R + wall
    wx0, wx1 = -bc.VA_W / 2 - 0.75, bc.VA_W / 2 + 0.75
    wy0, wy1 = bc.VA_CY - bc.VA_H / 2 - 0.75, bc.VA_CY + bc.VA_H / 2 + 0.75
    band = bc.rr(-ox, ox, -oy, oy, ro, 0.0, t) - bc.rr(-bc.IN_X + ledge, bc.IN_X - ledge, -bc.IN_TOP + ledge,
                                                        bc.IN_TOP - ledge, bc.IN_R - ledge, -1.0, t + 1.0)
    frame = bc.rr(wx0 - wf, wx1 + wf, wy0 - wf, wy1 + wf, 1.0 + wf, 0.0, t) - bc.rr(wx0, wx1, wy0, wy1, 1.0, -1.0, t + 1.0)
    sp = 2.0
    spokes = [bc.box(-ox + 1.0, wx0, y - sp, y + sp, 0.0, t) for y in (wy0 + 6.0, wy1 - 6.0)]
    spokes += [bc.box(wx1, ox - 1.0, y - sp, y + sp, 0.0, t) for y in (wy0 + 6.0, wy1 - 6.0)]
    spokes += [bc.box(x - sp, x + sp, wy1, oy - 1.0, 0.0, t) for x in (-50.0, 50.0)]
    spokes += [bc.box(x - sp, x + sp, -oy + 1.0, wy0, 0.0, t) for x in (-50.0, 50.0)]
    rim = bc.rr(-ox, ox, -oy, oy, ro, -1.75, 0.0) - bc.rr(-bc.IN_X, bc.IN_X, -bc.IN_TOP, bc.IN_TOP, bc.IN_R,
                                                          -3.0, 0.1)
    g = band.fuse(frame, rim, *spokes)
    # "TOP" (PCB / USB side up) engraved on the back face of the top band, readable from the back
    tag = bc.box(-22.0, 22.0, wy1 + wf - 0.5, wy1 + wf + 6.0, 0.0, t)       # label plate on the top spoke line
    back = Plane(origin=(0, wy1 + wf + 2.8, 0.0), x_dir=(-1, 0, 0), z_dir=(0, 0, -1))
    txt = extrude(back * Text("TOP / USB-C LEFT", font_size=3.6, font="Arial"), amount=-0.35)
    return (g + tag) - txt


def screw_gauge():
    z1, z0 = bc.STANDOFF_END_Z, bc.STANDOFF_END_Z - 2.0
    (xl, yt), (xr, _), (_, yb) = bc.STANDOFFS[0], bc.STANDOFFS[1], bc.STANDOFFS[2]
    w = 4.0
    g = (bc.box(xl - w, xl + w, yb - w, yt + w, z0, z1) + bc.box(xr - w, xr + w, yb - w, yt + w, z0, z1)
         + bc.box(xl, xr, yb - w, yb + w, z0, z1))
    ex, ey = -(bc.GLASS_W / 2 + 0.1), -(bc.GLASS_H / 2 + 0.1)      # tab faces touch the glass edges
    g = g + bc.box(ex - 1.6, xl, 20.0 - w, 20.0 + w, z0, z1) + bc.box(ex - 1.6, ex, 20.0 - 6.0, 20.0 + 6.0, z0, -0.8)
    g = g + bc.box(-30.0 - w, -30.0 + w, ey - 1.6, yb, z0, z1) + bc.box(-36.0, -24.0, ey - 1.6, ey, z0, -0.8)
    holes = [bc.cyl(x, y, bc.CLEAR_R, z0 - 1.0, z1 + 1.0) for x, y in bc.STANDOFFS]
    return g.cut(*holes)


def tca_outline():
    tz = bc.RIM_Z
    w, h = bc.TCA_SIZE[0] + 0.6, bc.TCA_SIZE[1] + 0.6
    cx, cy = bc.TCA_C
    outer = bc.box(cx - w / 2 - 0.8, cx + w / 2 + 0.8, cy - h / 2 - 0.8, cy + h / 2 + 0.8, tz - 0.1, tz + bc.L(1))
    inner = bc.box(cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2, tz - 1.0, tz + 2.0)
    return outer - inner


def pieces(front, cover, bracket):
    chin_x = 93.0                                    # bottom corner screw (no screws between the OLEDs)
    top_x = 35.0
    sec = lambda part, x0, x1, y0, y1, z0, z1: part & bc.box(x0, x1, y0, y1, z0, z1)  # noqa: E731
    cz = (bc.COVER_OUT_Z - 0.1, bc.RIM_Z + bc.LIP_H + 0.1)
    p = {
        "fit_01_bezel_gauge": front_down(bezel_gauge()),
        "fit_02_oled_cell": front_down(sec(front, -17.0, 17.0, bc.OUT_BOT - 0.1, bc.OLED_TOP + 1.0,
                                           bc.OLED_PIN_Z - 0.1, bc.FRONT_Z + 1.0)),
        "fit_03_nut_chin": front_down(sec(front, chin_x - 7.5, chin_x + 7.5, -88.5, -68.0, bc.RIM_Z - 0.1, CUT_Z)),
        "fit_04_cover_chin": sec(cover, chin_x - 7.5, chin_x + 7.5, -88.5, -68.0, *cz),
        "fit_05_nut_top": front_down(sec(front, top_x - 8.0, top_x + 8.0, 53.0, 65.0, bc.RIM_Z - 0.1, CUT_Z)),
        "fit_06_cover_top": sec(cover, top_x - 8.0, top_x + 8.0, 53.0, 65.0, *cz),
        "fit_08_standoff": sec(cover, bc.STANDOFFS[0][0] - 7.0, bc.STANDOFFS[0][0] + 7.0, bc.STANDOFFS[0][1] - 7.0,
                               bc.STANDOFFS[0][1] + 7.0, bc.COVER_OUT_Z - 0.1, bc.COVER_BOSS_TOP + 0.1),
        "fit_09_screw_gauge": screw_gauge(),
    }
    return {k: drop(v) for k, v in p.items()}


def oled_assembly_slices(front, cover):
    """Full-depth slice of the case around OLED 5 (right) and the bottom-right corner screw: the real
    front frame (OLED bay, pins, bottom and right walls, corner nut block) and the real back cover
    (plate, lip, corner boss, 4 OLED posts).  Test-only walls close the two cut sides so the cover slice
    rests at the real depth; one M3 x 10 at the real corner joint pulls the two together."""
    cx = bc.OLED_X[4]
    x0, x1 = cx - 17.0, bc.OUT_X + 0.5
    y0, y1 = bc.OUT_BOT - 0.5, bc.OLED_TOP + 6.0
    wt = 2.6
    lip_in = bc.IN_BOT + bc.LIP_C + bc.LIP_T + 0.4             # test walls stop short of the cover lip
    lip_x = bc.IN_X - bc.LIP_C - bc.LIP_T - 0.4
    fr = front & bc.box(x0, x1, y0, y1, bc.RIM_Z - 0.1, bc.FRONT_Z + 1.0)
    walls = (bc.box(x0, x0 + wt, lip_in, y1, bc.RIM_Z, 0.5)
             + bc.box(x0, lip_x, y1 - wt, y1, bc.RIM_Z, 0.5))
    fr = fr + (walls & bc.box(x0, x1, y0, y1, bc.RIM_Z, bc.FRONT_Z))
    cv = cover & bc.box(x0, x1, y0, y1, bc.COVER_OUT_Z - 0.1, bc.OLED_POST_Z + 0.1)
    return fr, cv


LAYOUT_A = {"fit_02_oled_cell": (-52.0, 20.0), "fit_08_standoff": (50.0, 24.0),
            "fit_03_nut_chin": (-55.0, -22.0), "fit_04_cover_chin": (-35.0, -22.0),
            "fit_05_nut_top": (-6.0, -22.0), "fit_06_cover_top": (16.0, -22.0)}


def overlap(a, b):
    r = a & b
    if r is None:
        return 0.0
    if isinstance(r, (list, ShapeList)):
        return sum(x.volume for x in r if x is not None)
    return r.volume


def main():
    (OUT / "stl").mkdir(parents=True, exist_ok=True)
    (OUT / "gcode").mkdir(parents=True, exist_ok=True)
    front, cover, bracket = bc.make_front_frame(), bc.make_back_cover(), bc.make_tca_bracket()
    p = pieces(front, cover, bracket)
    info = {}
    for n, s in p.items():
        export_stl(s, str(OUT / "stl" / f"{n}.stl"), tolerance=0.01, angular_tolerance=0.1)
        bb = s.bounding_box()
        info[n] = dict(size=[round(bb.size.X, 1), round(bb.size.Y, 1), round(bb.size.Z, 2)], solids=len(s.solids()))
    # plate A: coupons inside the gauge window (window spans x +-78.2, y -43.1..45.1 when front-down)
    placed = [p["fit_01_bezel_gauge"]]
    for n, (x, y) in LAYOUT_A.items():
        placed.append(Pos(x, y, 0) * p[n])
    for i in range(len(placed)):
        for j in range(i + 1, len(placed)):
            a, b = placed[i].bounding_box(), placed[j].bounding_box()
            if i > 0 and not (a.max.X + 2 < b.min.X or b.max.X + 2 < a.min.X or a.max.Y + 2 < b.min.Y
                              or b.max.Y + 2 < a.min.Y):
                raise SystemExit(f"layout overlap {i} {j}")
    ring = placed[0]
    for s in placed[1:]:
        if overlap(ring, s) > 0:
            raise SystemExit("coupon overlaps the gauge ring")
    export_stl(Compound(placed), str(OUT / "stl" / "plate_A_fit_test.stl"), tolerance=0.01, angular_tolerance=0.1)
    export_stl(p["fit_09_screw_gauge"], str(OUT / "stl" / "plate_B_screw_gauge.stl"), tolerance=0.01,
               angular_tolerance=0.1)
    # plate C: OLED bay + back cover posts at full case depth
    fr, cv = oled_assembly_slices(front, cover)
    ob = bc.box(bc.OLED_X[4] - bc.OLED_W / 2, bc.OLED_X[4] + bc.OLED_W / 2, bc.OLED_CY - bc.OLED_H / 2,
                bc.OLED_CY + bc.OLED_H / 2, -bc.OLED_STANDOFF - bc.OLED_PCB_T, -bc.OLED_STANDOFF)
    ob = ob.cut(*[bc.cyl(bc.OLED_X[4] + sx * bc.OLED_HOLE_DX, bc.OLED_CY + sy * bc.OLED_HOLE_DY, 1.0, -10, 2)
                  for sx in (-1, 1) for sy in (-1, 1)])
    slice_checks = {
        "frame slice x cover slice (mm3)": round(overlap(fr, cv), 3),
        "OLED board x frame slice (mm3)": round(overlap(ob, fr), 3),
        "OLED board x cover slice (mm3)": round(overlap(ob, cv), 3),
        "OLED board pushed 0.3 mm back x cover slice (mm3, posts stop it)": round(
            overlap(Pos(0, 0, -0.3) * ob, cv), 3),
    }
    fr_p, cv_p = drop(front_down(fr)), drop(cv)
    export_stl(fr_p, str(OUT / "stl" / "fit_13_oled_frame_slice.stl"), tolerance=0.01, angular_tolerance=0.1)
    export_stl(cv_p, str(OUT / "stl" / "fit_14_oled_cover_slice.stl"), tolerance=0.01, angular_tolerance=0.1)
    wf, wc = fr_p.bounding_box().size.X, cv_p.bounding_box().size.X
    plate_c = Compound([Pos(-(wf / 2 + 3.0), 0, 0) * fr_p, Pos(wc / 2 + 3.0, 0, 0) * cv_p])
    export_stl(plate_c, str(OUT / "stl" / "plate_C_oled_assembly.stl"), tolerance=0.01, angular_tolerance=0.1)
    for n, s in (("fit_13_oled_frame_slice", fr_p), ("fit_14_oled_cover_slice", cv_p)):
        bb = s.bounding_box()
        info[n] = dict(size=[round(bb.size.X, 1), round(bb.size.Y, 1), round(bb.size.Z, 2)], solids=len(s.solids()))
    info["plate_C_checks"] = slice_checks
    # plate E: the real TCA bracket (with the C3 SuperMini pocket) + the back cover round one lower standoff
    # (boss + counterbore), to try the stack cover -> bracket -> module standoff with one M3 x 10, + the back
    # cover round the two C3 posts with two test-only rails that stand on the bracket's C3 pad at the real
    # cover distance, to try the C3 in its pocket on the bench, before the 4 h cover
    (xs, ys), _ = bc.lower_standoffs()
    cv_lo = drop(cover & bc.box(xs - 7.0, xs + 7.0, ys - 7.0, ys + 7.0, bc.COVER_OUT_Z - 0.1, bc.BRK_Z0 + 0.1))
    c3 = bc.c3_pocket()
    (r0, r1), f = c3["relief"], bc.c3_frame()
    hw, ow = bc.C3_HW, bc.C3_HW + bc.C3_SIDE
    rails = [f * bc.box(s * (hw + 1.25), s * (ow - 0.2), r0 + 1.0, r1 - 1.0, bc.COVER_OUT_Z + 0.5, bc.BRK_Z0)
             for s in (-1, 1)]
    c3g = cover & (f * bc.box(-ow, ow, bc.C3_PAD_L[0], bc.C3_PAD_L[1], bc.COVER_OUT_Z - 0.1, bc.BRK_Z0 + 0.1))
    c3g = drop(c3g.fuse(*rails))
    export_stl(c3g, str(OUT / "stl" / "fit_16_cover_c3_posts.stl"), tolerance=0.01, angular_tolerance=0.1)
    br = drop(bracket)
    export_stl(cv_lo, str(OUT / "stl" / "fit_15_cover_lower_standoff.stl"), tolerance=0.01, angular_tolerance=0.1)
    by = br.bounding_box().size.Y / 2
    plate_e = Compound([br, Pos(-35.0, -by - 12.0, 0) * cv_lo])     # fit_16 stays a separate, optional STL
    export_stl(plate_e, str(OUT / "stl" / "plate_E_tca_bracket.stl"), tolerance=0.01, angular_tolerance=0.1)
    for n, s in (("fit_15_cover_lower_standoff", cv_lo), ("fit_16_cover_c3_posts", c3g), ("04_tca_bracket", br)):
        bb = s.bounding_box()
        info[n] = dict(size=[round(bb.size.X, 1), round(bb.size.Y, 1), round(bb.size.Z, 2)], solids=len(s.solids()))

    # checks against the real module (STEP cache written by verify_case.py)
    names = ["7INCH-800X480-TP", "7INCH-800X480-LCD", "7INCH-DSI-TOUCHSCREEN-BRACKET", "EVA"]
    disp = {n: import_brep(str(CACHE / f"disp_{n}.brep")) for n in names}
    pcba = import_brep(str(CACHE / "disp_PCBA.brep"))
    boxes = []
    for s in pcba.solids():
        b = s.bounding_box()
        if b.size.X * b.size.Y * b.size.Z > 0.2:
            boxes.append(bc.box(b.min.X, b.max.X, b.min.Y, b.max.Y, b.min.Z, b.max.Z))
    gauge = screw_gauge()
    checks = {
        "bezel gauge x glass (glass drops in)": round(overlap(bezel_gauge(), disp["7INCH-800X480-TP"]), 3),
        "screw gauge x module (glass, LCD, bracket, PCB parts)": round(
            sum(overlap(gauge, s) for s in list(disp.values()) + [Compound(boxes)]), 3),
        "screw gauge hole centres = cover holes": all(
            any(abs(x - hx) < 1e-6 and abs(y - hy) < 1e-6 for hx, hy in bc.STANDOFFS) for x, y in bc.STANDOFFS),
    }
    info["checks"] = checks
    (OUT / "fit_info.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()
