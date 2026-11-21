#!/usr/bin/env python3
"""Checks for build_v2.py: interference with the real module (official STEP, cached by ../main-display/verify_case.py)
and with every board / plug / wire keep-out, part-to-part overlaps, screw stacks, the 25 mm wire room, the
stand's footprint and stability, and preview renders."""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "main-display"))

from build123d import Compound, Pos, Rot, ShapeList, export_stl, import_brep  # noqa: E402
import build_v2 as b  # noqa: E402

CACHE = HERE.parent / "main-display" / "out" / "_cache"
OUT = HERE / "out"


def vol(r):
    if r is None:
        return 0.0
    if isinstance(r, (list, ShapeList)):
        return sum(x.volume for x in r if x is not None)
    return r.volume


def ov(a, c):
    try:
        return vol(a & c)
    except Exception:
        return -1.0


def module():
    names = ["7INCH-800X480-TP", "7INCH-800X480-LCD", "7INCH-DSI-TOUCHSCREEN-BRACKET", "EVA"]
    d = {n: import_brep(str(CACHE / f"disp_{n}.brep")) for n in names}
    pcba = import_brep(str(CACHE / "disp_PCBA.brep"))
    boxes = []
    for s in pcba.solids():
        bb = s.bounding_box()
        if bb.size.X * bb.size.Y * bb.size.Z > 0.2:
            boxes.append(b.box(bb.min.X, bb.max.X, bb.min.Y, bb.max.Y, bb.min.Z, bb.max.Z))
    d["PCBA (part boxes)"] = Compound(boxes)
    d["brass standoffs"] = Compound([b.cyl(x, y, 3.2, -8.5, -16.5) for x, y in b.STANDOFFS])
    return d


def keepouts():
    k = {}
    k["UART2 (7) plug + wires to -25.0"] = b.box(-55.5, -48.5, -22.6, -10.6, -25.0, -18.4)
    k["I2C (10) plug + wires to -25.0"] = b.box(44.4, 51.4, -3.4, 8.6, -25.0, -18.4)
    oled = []
    for cx in b.OLED_X:
        cy = b.OLED_CY
        pcb = b.box(cx - b.OLED_W / 2, cx + b.OLED_W / 2, cy - b.OLED_H / 2, cy + b.OLED_H / 2, b.PCB_BACK, b.PCB_FRONT)
        pcb = pcb.cut(*[b.cyl(cx + sx * b.HOLE_DX, cy + sy * b.HOLE_DY, 1.0, -20, 5) for sx in (-1, 1) for sy in (-1, 1)])
        gtop = cy + b.v1.OLED_STRIP_DY[0]                      # glass edge on the header side (v1, fit-tested)
        glass = b.box(cx - 13.35, cx + 13.35, gtop - 19.26, gtop, b.PCB_FRONT, -b.FACE_T)
        hdr = b.box(cx - 5.5, cx + 5.5, -61.0, -57.6, b.PCB_BACK - 5.0, b.PCB_BACK)
        oled += [pcb, glass, hdr]
    k["OLED boards, glass, header + soldered wires"] = Compound(oled)
    parts = []
    for cx in b.OLED_X:                                    # parts on the OLED's back (estimated from photos)
        cy, z0, z1 = b.OLED_CY, b.PCB_BACK - 0.7, b.PCB_BACK
        parts += [b.box(cx - 12.3, cx + 12.6, cy - 0.3, cy + 1.4, z0, z1),        # C6 ... C3 row
                  b.box(cx - 10.0, cx - 5.9, cy + 6.0, cy + 7.1, z0, z1),         # address resistors
                  b.box(cx + 2.8, cx + 7.2, cy + 7.3, cy + 10.2, z0, z1),         # U2, C8
                  b.box(cx - 9.8, cx + 9.8, cy - 13.7, cy - 4.4, b.PCB_BACK - 0.15, z1)]     # glass flex
    k["OLED back-side parts (photo estimate)"] = Compound(parts)
    tx, ty = b.TCA_C
    w, h = b.TCA_SIZE[0] / 2, b.TCA_SIZE[1] / 2
    z0 = b.TCA_POST_TOP
    tca = [b.box(tx - w, tx + w, ty - h, ty + h, z0, z0 + b.TCA_T).cut(
        *[b.cyl(tx + s * b.TCA_PITCH / 2, ty, 1.65, -40, 5) for s in (-1, 1)]),
        b.box(tx - 9.0, tx + 9.0, ty - h, ty + h, z0 + b.TCA_T, z0 + b.TCA_T + 2.0),            # parts
        b.box(tx - w, tx + w, ty - h, ty + h, z0 - 4.0, z0).cut(                                # wires behind
            *[b.cyl(tx + s * b.TCA_PITCH / 2, ty, 3.6, -40, 5) for s in (-1, 1)])]
    for s in (-1, 1):
        x = tx + s * b.TCA_PITCH / 2
        tca.append(b.cyl(x, ty, 2.85, z0 + b.TCA_T, z0 + b.TCA_T + 3.0))                         # screw heads
    k["TCA9548A + parts + wires + heads"] = Compound(tca)
    ft = b.C3_Y0 + b.C3_FLOOR
    z0 = b.C3_USB_Z + b.C3_END
    c3 = [b.box(-9.0, 9.0, ft, ft + 1.0, z0, z0 + 22.52),
          b.box(-4.47, 4.47, ft + 1.0, ft + 4.16, z0 - 0.0, z0 + 7.3),
          b.box(-8.0, 8.0, ft + 1.0, ft + 3.2, z0 + 7.5, z0 + 22.3)]
    k["ESP32-C3 SuperMini (PCB, USB-C, parts + wires)"] = Compound(c3)
    return k


def main():
    p = b.parts()
    a = b.assembly(p)
    names = list(a.keys())
    res = {"module": {}, "keepouts": {}, "parts": {}}
    mod = module()
    for n in names:
        for mn, ms in mod.items():
            v = ov(a[n], ms)
            if v != 0:
                res["module"][f"{n} x {mn}"] = round(v, 3)
    ko = keepouts()
    for n in names:
        for kn, ks in ko.items():
            v = ov(a[n], ks)
            if v != 0:
                res["keepouts"][f"{n} x {kn}"] = round(v, 3)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            bi, bj = a[names[i]].bounding_box(), a[names[j]].bounding_box()
            if (bi.max.X < bj.min.X or bj.max.X < bi.min.X or bi.max.Y < bj.min.Y or bj.max.Y < bi.min.Y
                    or bi.max.Z < bj.min.Z or bj.max.Z < bi.min.Z):
                continue
            v = ov(a[names[i]], a[names[j]])
            if v != 0:
                res["parts"][f"{names[i]} x {names[j]}"] = round(v, 3)
    # OLED pressed 0.3 back must hit the lid posts (they hold it on its pins)
    cx, cy = 0.0, b.OLED_CY
    pcb = b.box(cx - b.OLED_W / 2, cx + b.OLED_W / 2, cy - b.OLED_H / 2, cy + b.OLED_H / 2, b.PCB_BACK, b.PCB_FRONT)
    res["OLED pushed 0.35 back x lid posts (> 0: posts hold it)"] = round(ov(Pos(0, 0, -0.35) * pcb, p["v2_07_oled_lid"]), 3)
    # lid fitting path: straight in from behind 2.5 mm low, then 2.5 mm up.  Nothing on the way may touch the
    # body (rails, long pins), the OLED board or the parts on its back.
    obst = [p["v2_06_oled_body"], pcb.cut(*[b.cyl(sx * b.HOLE_DX, b.OLED_CY + sy * b.HOLE_DY, 1.0, -20, 5)
                                              for sx in (-1, 1) for sy in (-1, 1)])]
    obst.append(Pos(-b.OLED_X[2], 0, 0) * ko["OLED back-side parts (photo estimate)"])
    path = [(0.0, -b.LID_SLIDE, dz) for dz in (-8.0, -4.0, -2.0, -1.0, -0.5)]
    path += [(0.0, dy, 0.0) for dy in (-b.LID_SLIDE, -1.8, -1.2, -0.6, 0.0)]
    res["lid on its fitting path x body / board / back parts (all 0)"] = [
        round(sum(ov(Pos(*d) * p["v2_07_oled_lid"], o) for o in obst), 3) for d in path]
    res["lid slid straight up from the bottom (old way) x back parts (> 0: posts would hit C6 / C3)"] = round(
        ov(Pos(0, -12.0, 0) * p["v2_07_oled_lid"], obst[2]), 3)
    # numbers
    res["depth"] = {
        "glass front -> pod inner face (wire room, need >= 25)": round(-b.Z_PLATE, 2),
        "glass front -> back face": round(-b.Z_BACK, 2),
        "wall front edge proud of glass": round(b.Z_FRONT, 2),
        "case depth incl. wall lip": round(b.Z_FRONT - b.Z_BACK, 2),
        "spine front face (PCBA parts end -13.7)": round(b.Z_SPINE, 2),
        "C3 USB-C face behind the back face": round(b.Z_BACK - b.C3_USB_Z, 2),
    }
    res["screws (M3 x 10: seat -> tip, nut zone)"] = {
        "module standoffs (4)": [b.STANDOFF_CB, b.STANDOFF_CB + 10, "into brass standoff from -16.5, bite %.2f" % (b.STANDOFF_CB + 10 + 16.5)],
        "walls -> pods / spine": [b.SEAT_Z, b.SEAT_Z + 10, list(b.NUT_Z)],
        "OLED cells (5)": [b.SEAT_Z, b.SEAT_Z + 10, list(b.NUT_Z)],
        "C3 cartridge (2)": [b.C3_EAR_Z, b.C3_EAR_Z + 10, [b.C3_NUT_Z, b.C3_NUT_Z + 2.4]],
        "TCA (2, from the board side)": [b.TCA_POST_TOP + b.TCA_T, b.TCA_POST_TOP + b.TCA_T - 10, list(b.TCA_NUT_Z)],
        "count": len(b.STANDOFFS) + len(b.WB) + len(b.OLED_X) + 2 + 2,
    }
    # stand: lowest points, footprint, centre of mass (case parts 1.24 g/cm3 x 0.45 fill, module 330 g)
    A, B, C = b.leg_points()
    wa, wb = b.c2w(*A), b.c2w(*B)
    m_mod, g_mod = 330.0, (0.0, 0.0, -6.0)
    tot, mz = m_mod, m_mod * b.c2w(g_mod[1], g_mod[2])[1]
    for n, s in a.items():
        c = s.center()
        m = s.volume / 1000 * 1.24 * 0.45
        tot += m
        mz += m * b.c2w(c.Y, c.Z)[1]
    gz = mz / tot
    res["stand"] = {"desk contact front (case bottom-back edge) wz": round(wa[1], 1),
                    "leg rear end wz": round(wb[1], 1),
                    "centre of mass wz (est.)": round(gz, 1),
                    "margin to front / rear tipping edge (mm)": [round(wa[1] - gz, 1), round(gz - wb[1], 1)],
                    "total mass est. g": round(tot)}
    (OUT / "verify_v2.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
    print(json.dumps(res, indent=1))
    # assembly STLs for renders (case frame)
    asm = OUT / "asm"
    asm.mkdir(exist_ok=True)
    for n, s in a.items():
        export_stl(s, str(asm / f"{n}.stl"), tolerance=0.05, angular_tolerance=0.3)
    for mn, ms in mod.items():
        if mn != "PCBA (part boxes)":
            export_stl(ms, str(asm / f"module_{mn.split()[0]}.stl"), tolerance=0.05, angular_tolerance=0.3)
    export_stl(mod["PCBA (part boxes)"], str(asm / "module_PCBA.stl"), tolerance=0.05, angular_tolerance=0.3)
    for kn, ks in ko.items():
        export_stl(ks, str(asm / f"ko_{kn.split()[0]}.stl"), tolerance=0.05, angular_tolerance=0.3)


if __name__ == "__main__":
    main()

