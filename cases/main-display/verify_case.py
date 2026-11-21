#!/usr/bin/env python3
"""Interference checks + preview renders for build_case.py.

* Loads Waveshare's official STEP (research/waveshare/ESP32-S3-touch-lcd-7.stp) and moves it into
  the case frame: (Xs, Ys, Zs) -> (Xs, -Zs + 1.02, Ys - 0.8).
* Adds keep-out bodies the STEP does not contain: brass standoffs, USB-C plug overmolds and the
  internal cable run to the cable hole, HY2.0 plug + wires, OLED modules with straight header +
  Dupont housings, TCA9548A.
* Reports every overlap volume, the tipping force, and writes PNG previews to out/preview.
"""
from __future__ import annotations

import json
import math
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from build123d import (Compound, Pos, Rot, import_step, import_brep, export_brep, export_stl,  # noqa: E402
                       CenterOf, ShapeList)
import build_case as bc  # noqa: E402

STEP = HERE / "research" / "waveshare" / "ESP32-S3-touch-lcd-7.stp"
CACHE = HERE / "out" / "_cache"
PREV = HERE / "out" / "preview"


def disp_tf(s):
    return Pos(0, 1.02, -0.8) * (Rot(90, 0, 0) * s)


def load_display():
    names = ["7INCH-800X480-TP", "7INCH-800X480-LCD", "7INCH-DSI-TOUCHSCREEN-BRACKET", "EVA"]
    cached = [CACHE / f"disp_{n}.brep" for n in names] + [CACHE / "disp_PCBA.brep"]
    if all(p.exists() for p in cached):
        parts = {n: import_brep(str(CACHE / f"disp_{n}.brep")) for n in names}
        return parts, import_brep(str(CACHE / "disp_PCBA.brep"))
    t = time.time()
    c = import_step(str(STEP))
    asm = [x for x in c.children if "TOUCHSCREEN_ASM" in x.label][0]
    parts = {ch.label: disp_tf(Compound(ch.solids())) for ch in asm.children}
    pcba = [x for x in c.children if "PCBA" in x.label][0]
    pcba_c = disp_tf(Compound(pcba.solids()))
    print(f"STEP loaded {time.time() - t:.1f}s")
    for n, s in parts.items():
        export_brep(s, str(CACHE / f"disp_{n}.brep"))
    export_brep(pcba_c, str(CACHE / "disp_PCBA.brep"))
    return parts, pcba_c


def keepouts():
    k = {}
    k["standoffs"] = [bc.cyl(x, y, 3.2, bc.FRAME_BACK_Z, bc.STANDOFF_END_Z) for x, y in bc.STANDOFFS]
    plugs, cables = [], []
    x0 = bc.USB_EDGE_X - 26.0                                # end of the plug overmold
    y_run = -66.0                                            # across the chin, behind the OLED headers
    for (name, y), r, x_hole in zip(bc.USB_PORTS.items(), (10.0, 5.0), (-18.0, -8.0)):
        plugs.append(bc.box(x0, bc.USB_EDGE_X, y - 6.25, y + 6.25, bc.USB_Z - 3.25, bc.USB_Z + 3.25))
        # 4.5 mm cable: bends toward the back, runs down the left side behind the module, across the
        # chin, then out through the cable hole into the stand (both USB-C, one of them is the power)
        zc, xd = bc.USB_Z - r, x0 - r
        cab = [bc.box(x0 - 0.5, x0 + 0.5, y - 2.25, y + 2.25, bc.USB_Z - 2.25, bc.USB_Z + 2.25)]
        for i in range(0, 91, 10):
            a = math.radians(i)
            px, pz = x0 - r * math.sin(a), zc + r * math.cos(a)
            cab.append(bc.box(px - 1.6, px + 1.6, y - 2.25, y + 2.25, pz - 1.6, pz + 1.6))
        cab.append(bc.box(xd - 2.25, xd + 2.25, y_run - 2.25, y + 2.25, zc - 2.25, zc + 2.25))
        cab.append(bc.box(xd - 2.25, x_hole + 2.25, y_run - 2.25, y_run + 2.25, zc - 2.25, zc + 2.25))
        cab.append(bc.box(x_hole - 2.25, x_hole + 2.25, y_run - 2.25, y_run + 2.25,
                          bc.COVER_OUT_Z - bc.PLATE_T - 3.0, zc + 2.25))
        cables.append(Compound(cab))
    k["usb_plugs"], k["usb_cables"] = plugs, cables
    x0, x1, y0, y1 = bc.I2C_HDR
    k["hy20_plug_wires"] = [bc.box(x0, x1, y0, y1, bc.HDR_TOP_Z - 3.0, bc.HDR_TOP_Z),
                            bc.box(x0 + 0.5, x1 - 0.5, y0 + 0.5, y1 - 0.5, bc.HDR_TOP_Z - 7.0, bc.HDR_TOP_Z - 3.0)]
    # GME12864: PCB with 4 d2.0 holes, 26.7 x 19.26 glass (+ tape) up to the front plate, header on the
    # back with Dupont plug and wires, header joints on the front, glass FPC folded round the bottom edge
    pcb, glass, hdr, dup, stubs, wires, va, fpc = [], [], [], [], [], [], [], []
    zf, t, cy = -bc.OLED_STANDOFF, bc.OLED_PCB_T, bc.OLED_CY
    zb = zf - t
    for cx in bc.OLED_X:
        p = bc.box(cx - bc.OLED_W / 2, cx + bc.OLED_W / 2, cy - bc.OLED_H / 2, cy + bc.OLED_H / 2, zb, zf)
        p = p.cut(*[bc.cyl(cx + sx * bc.OLED_HOLE_DX, cy + sy * bc.OLED_HOLE_DY, 1.0, zb - 1, zf + 1)
                    for sx in (-1, 1) for sy in (-1, 1)])
        pcb.append(p)
        glass.append(bc.box(cx - 13.35, cx + 13.35, cy - 8.96, cy + 10.30, zf, -0.1))
        wy = cy + bc.OLED_WIN_DY
        va.append(bc.box(cx - bc.OLED_AA_W / 2, cx + bc.OLED_AA_W / 2, wy - 5.43, wy + 5.43, -0.1, -0.05))
        hy = cy + bc.OLED_HDR_DY
        hdr.append(bc.box(cx - 5.08, cx + 5.08, hy - 1.27, hy + 1.27, zb - 2.5, zb))
        dup.append(bc.box(cx - 5.08, cx + 5.08, hy - 1.27, hy + 1.27, zb - 2.5 - 14.0, zb - 2.5))
        wires.append(bc.box(cx - 4.0, cx + 4.0, hy - 1.0, hy + 1.0, zb - 2.5 - 18.5, zb - 2.5 - 14.0))
        stubs.append(bc.box(cx - 4.6, cx + 4.6, hy - 0.8, hy + 0.8, zf, zf + 1.8))
        fpc.append(bc.box(cx - 9.3, cx + 9.3, cy - 13.9, cy - 8.96, zf, zf + 0.3))          # front run
        fpc.append(bc.box(cx - 9.3, cx + 9.3, cy - 14.2, cy - 13.9, zb - 0.5, zf + 0.3))     # fold
        fpc.append(bc.box(cx - 9.3, cx + 9.3, cy - 13.9, cy - 4.0, zb - 0.5, zb))           # back + joints
    k.update(oled_pcb=pcb, oled_glass=glass, oled_header=hdr, oled_dupont=dup, oled_wires=wires,
             oled_pin_stubs=stubs, oled_fpc=fpc)
    # TCA9548A on the bracket back face, parts toward the module (into the bracket window), underside
    # toward the cover
    cx, cy, zb = bc.TCA_C[0], bc.TCA_C[1], bc.BRK_Z0
    holes = [cx + dx for dx in (-bc.TCA_PITCH / 2, bc.TCA_PITCH / 2)]
    tca = bc.box(cx - 15.75, cx + 15.75, cy - 10.75, cy + 10.75, zb - bc.TCA_PCB_T, zb)
    tca = tca.cut(*[bc.cyl(hx, cy, 1.65, zb - 3, zb + 1) for hx in holes])
    k["tca_board"] = [tca]
    # parts and header joints on the component side (1.6 mm, none on the two seats), and wires soldered on
    # the underside or right-angle headers with Dupont plugs lying flat (2.6 mm)
    parts = bc.box(cx - 15.0, cx + 15.0, cy - 10.0, cy + 10.0, zb, zb + 1.6)
    free = []
    for hx in holes:
        s = 1 if hx > cx else -1
        free += [bc.cyl(hx, cy, bc.TCA_SEAT_R + 0.2, zb - 1, zb + 3),
                 bc.box(hx, cx + s * 17.0, cy - bc.TCA_SEAT_HW - 0.2, cy + bc.TCA_SEAT_HW + 0.2, zb - 1, zb + 3)]
    under = bc.box(cx - 15.0, cx + 15.0, cy - 10.0, cy + 10.0, bc.TCA_HEAD_Z - 2.6, bc.TCA_HEAD_Z)
    k["tca_parts_wires"] = [parts.cut(*free), under]
    k["tca_screw_heads"] = [bc.cyl(hx, cy, 2.75, bc.TCA_HEAD_Z - 3.0, bc.TCA_HEAD_Z) for hx in holes]
    k["_oled_va"] = va
    k["c3_supermini"] = [c3_board()]
    return k


C3_STEP = HERE / "research" / "c3_supermini" / "ESP32-C3-SuperMini_mrtnvgr.stp"
C3_PCB_TOP, C3_PCB_T, C3_HALF_L = 0.45, 0.9, 11.26     # in the STEP: PCB z -0.45..0.45, ends at y +-11.26


def c3_board():
    """ESP32-C3 SuperMini STEP (one merged solid: PCB, USB-C, buttons, chip, antenna) placed upside down in
    its pocket: component side toward the module, PCB top on the antenna-end ledges, antenna end 0.05 off
    its wall.  The STEP has USB-C toward +Y, which is the pocket frame's along-the-board direction."""
    cache = CACHE / "c3_supermini.brep"
    if cache.exists():
        s = import_brep(str(cache))
    else:
        s = import_step(str(C3_STEP)).solids()[0]
        export_brep(s, str(cache))
    return bc.c3_frame() * (Pos(0, 0.05 + C3_HALF_L, bc.C3_LEDGE_Z - C3_PCB_TOP) * s)


def c3_plug():
    """USB-C plug overmold (12.5 x 6.5 x 26) in the C3 receptacle, cover off, board sitting on its antenna
    end wall (the lowest mouth position); its 4.5 mm cable runs on 4 mm and turns out of the open back."""
    m = 0.05 + bc.C3_L + bc.C3_USB_OVER
    zc = bc.C3_LEDGE_Z + bc.C3_USB_H / 2
    e = m + 26.0
    f = bc.c3_frame()
    return Compound([f * bc.box(-6.25, 6.25, m, e, zc - 3.25, zc + 3.25),
                     f * bc.box(-2.25, 2.25, e, e + 4.0, zc - 2.25, zc + 2.25),
                     f * bc.box(-2.25, 2.25, e + 0.5, e + 4.0, bc.RIM_Z - 20.0, zc + 2.25)])


def oled_insertion_sweep(depth=60.0):
    """Volume swept by the 5 OLED modules (PCB with its holes, glass, header, FPC) pushed in along +Z
    onto their pins."""
    zf, t, cy = -bc.OLED_STANDOFF, bc.OLED_PCB_T, bc.OLED_CY
    zb = zf - t
    hy = cy + bc.OLED_HDR_DY
    rects = [(-bc.OLED_W / 2, bc.OLED_W / 2, cy - bc.OLED_H / 2, cy + bc.OLED_H / 2, zb, zf),   # PCB
             (-13.35, 13.35, cy - 8.96, cy + 10.30, zf, -0.1),                                  # glass
             (-5.08, 5.08, hy - 1.27, hy + 1.27, zb - 2.5, zb),                                 # header
             (-4.6, 4.6, hy - 0.8, hy + 0.8, zf, zf + 1.8),                                     # joints
             (-9.3, 9.3, cy - 14.2, cy - 8.96, zb - 0.5, zf + 0.3)]                             # FPC
    out = []
    for cx in bc.OLED_X:
        holes = [bc.cyl(cx + sx * bc.OLED_HOLE_DX, cy + sy * bc.OLED_HOLE_DY, 1.0, -80, 5)
                 for sx in (-1, 1) for sy in (-1, 1)]
        for x0, x1, y0, y1, z0, z1 in rects:
            out.append(bc.box(cx + x0, cx + x1, y0, y1, z0 - depth, z1).cut(*holes))
    return Compound(out)


def check_fasteners(front, cover, stand_w, bodies, bracket):
    """Nut fits its pocket, the way in/out is free, the screw clears every printed part, and how
    many mm of thread the M3 x 10 has inside the nut (2.4 mm nut and 3.0 mm brass support)."""
    W = bc.to_world
    parts = {"case": {"front": front, "cover": cover, "bracket": bracket},
             "world": {"front": W(front), "cover": W(cover), "bracket": W(bracket), "stand": stand_w}}
    others = {"case": [bodies[n] for n in ("usb_plugs", "usb_cables", "pcba_component_boxes", "display_lcd",
                                           "display_bracket", "display_tp", "oled_pcb", "oled_dupont")],
              "world": [W(bodies["usb_cables"]), W(bodies["usb_plugs"])]}
    area = math.pi * bc.SCREW_R ** 2
    groups = {}
    for t in (3.0, 2.4):
        for f in bc.fasteners(t):
            g = groups.setdefault(f["group"], dict(count=0, nut_vs_parts_mm3=0.0, nut_way_blocked_mm3=0.0,
                                                   screw_vs_parts_mm3=0.0, screw_vs_module_cables_mm3=0.0,
                                                   min_thread_engagement_mm={}))
            ps = parts[f["frame"]]
            if t == 3.0:
                g["count"] += 1
                if f["nut"] is not None:
                    g["nut_vs_parts_mm3"] += sum(overlap(f["nut"], p) for p in ps.values())
                    g["nut_way_blocked_mm3"] += overlap(f["path"], ps[f["part"]])
                g["screw_vs_parts_mm3"] += sum(overlap(f["screw"], p) for p in ps.values())
                if f["nut"] is not None:
                    g["screw_vs_module_cables_mm3"] += sum(overlap(f["screw"], o) for o in others[f["frame"]])
            target = f["nut"] if f["nut"] is not None else bodies["standoffs"]
            e = overlap(f["screw"], target) / area
            key = "brass_support_3.0" if t == 3.0 else "nut_2.4"
            if f["nut"] is None:
                key = "module_brass_standoff"
            g["min_thread_engagement_mm"][key] = round(min(g["min_thread_engagement_mm"].get(key, 99.0), e), 2)
    for g in groups.values():
        for k in ("nut_vs_parts_mm3", "nut_way_blocked_mm3", "screw_vs_parts_mm3", "screw_vs_module_cables_mm3"):
            g[k] = round(g[k], 3)
    return groups


def side_slot_margins(plug_w=12.5, plug_h=6.5):
    """Y and Z room left round a plug_w x plug_h plug body in the side stadium, measured at its corners."""
    y0, y1, z0, z1 = bc.SIDE_USB_SLOT
    r = (z1 - z0) / 2
    inset = r - math.sqrt(r * r - (plug_h / 2) ** 2)
    ys = list(bc.USB_PORTS.values())
    my = min((y1 - inset) - (max(ys) + plug_w / 2), (min(ys) - plug_w / 2) - (y0 + inset))
    return [round(my, 2), round(r - plug_h / 2, 2)]


def overlap(a, b):
    ba, bb_ = a.bounding_box(), b.bounding_box()
    if (ba.max.X < bb_.min.X or bb_.max.X < ba.min.X or ba.max.Y < bb_.min.Y or bb_.max.Y < ba.min.Y
            or ba.max.Z < bb_.min.Z or bb_.max.Z < ba.min.Z):
        return 0.0
    try:
        r = a & b
    except Exception:
        return float("nan")
    if r is None:
        return 0.0
    if isinstance(r, (list, ShapeList)):
        return sum(x.volume for x in r if x is not None)
    return r.volume


def main():
    CACHE.mkdir(parents=True, exist_ok=True)
    PREV.mkdir(parents=True, exist_ok=True)
    parts = bc.build()
    front, cover, stand_w = parts["front"], parts["cover"], parts["stand_world"]
    disp, pcba = load_display()
    tp = disp["7INCH-800X480-TP"]
    print("glass bbox (expect X+-96.48 Y+-55.38 Z<=0):", tp.bounding_box())
    print("pcba bbox:", pcba.bounding_box())
    k = keepouts()

    report = {}
    bracket = parts["bracket"]
    printed = {"front_frame": front, "back_cover": cover, "tca_bracket": bracket}
    bodies = {"display_" + n.split("-")[-1].lower(): s for n, s in disp.items()}
    pcba_boxes = []
    for s in pcba.solids():
        b = s.bounding_box()
        if b.size.X * b.size.Y * b.size.Z > 0.2:
            pcba_boxes.append(bc.box(b.min.X, b.max.X, b.min.Y, b.max.Y, b.min.Z, b.max.Z))
    bodies["pcba_component_boxes"] = Compound(pcba_boxes)
    for name, lst in k.items():
        if not name.startswith("_"):
            bodies[name] = Compound(lst)
    for pn, ps in printed.items():
        for bn, bs in bodies.items():
            v = overlap(ps, bs)
            if v > 1e-3 or v != v:
                report[f"{pn} x {bn}"] = round(v, 3)
    # printed parts vs each other and vs the stand (world frame)
    W = bc.to_world
    pairs = {"front_frame x back_cover": (front, cover), "tca_bracket x front_frame": (bracket, front),
             "tca_bracket x back_cover": (bracket, cover)}
    for n, (a, b) in pairs.items():
        report[n] = round(overlap(a, b), 3)
    for pn, ps in printed.items():
        report[f"stand x {pn}"] = round(overlap(stand_w, W(ps)), 3)
    for bn in ("usb_cables", "usb_plugs"):
        report[f"stand x {bn}"] = round(overlap(stand_w, W(bodies[bn])), 3)
    # frame screw heads (3.0 mm socket head, d5.5 / pan head d6.0 x 2.4) sit in the cover counterbores
    heads = [bc.cyl(x, y, 2.75, bc.HEAD_SEAT_Z - 3.0, bc.HEAD_SEAT_Z) for x, y, _ in bc.TUB_SCREWS]
    pans = [bc.cyl(x, y, 3.0, bc.HEAD_SEAT_Z - 2.4, bc.HEAD_SEAT_Z) for x, y, _ in bc.TUB_SCREWS]
    report["frame screw heads (socket 3.0) x back_cover"] = round(overlap(cover, Compound(heads)), 3)
    report["frame screw heads (pan d6.0 x 2.4) x back_cover"] = round(overlap(cover, Compound(pans)), 3)
    report["stand x frame screw heads"] = round(overlap(stand_w, W(Compound(heads + pans))), 3)
    # keep-outs against the module itself (plug bodies must clear the metal frame)
    for kn in ("usb_plugs", "usb_cables", "hy20_plug_wires", "oled_dupont", "oled_wires", "tca_parts_wires"):
        for dn in ("display_tp", "display_lcd", "display_bracket", "standoffs"):
            v = overlap(bodies[kn], bodies[dn])
            if v > 1e-3:
                report[f"{kn} x {dn}"] = round(v, 3)
    # assembly paths: the 7" module and the OLEDs are pushed in from the back (+Z) into the frame
    gl = bc.rr(-bc.GLASS_W / 2, bc.GLASS_W / 2, -bc.GLASS_H / 2, bc.GLASS_H / 2, bc.GLASS_R, -60.0, -0.001)
    report["insertion sweep: 7in glass outline x front_frame"] = round(overlap(front, gl), 3)
    # cable route: a USB-A plug (16 x 8 mm) must pass cover -> stand plate -> tunnel -> out the
    # bottom or out the back window; the cover hole takes a 50 x 18 bundle
    bundle = bc.box(-25.0, 25.0, -78.5, -63.8, bc.RIM_Z + 2.0, bc.COVER_OUT_Z - bc.PLATE_T - 1.5)
    report["cable route: 50 x 14.7 bundle through cover hole x back_cover"] = round(overlap(cover, bundle), 3)
    bundle_p = bc.box(-22.0, 22.0, -78.5, -63.8, bc.RIM_Z + 2.0, bc.COVER_OUT_Z - bc.PLATE_T - 1.5)
    report["cable route: 44 x 14.7 bundle through plate hole x stand"] = round(overlap(stand_w, W(bundle_p)), 3)
    report["TCA parts + headers x ESP32 PCBA parts"] = round(overlap(bodies["tca_parts_wires"],
                                                                     bodies["pcba_component_boxes"]), 3)
    # the bracket (with the TCA on it) is lowered straight onto the standoff ends from the open back
    tca_set = Compound([bracket] + k["tca_board"] + k["tca_parts_wires"])
    obst = [front, bodies["oled_dupont"], bodies["oled_wires"], bodies["usb_cables"], bodies["usb_plugs"]]
    report["insertion sweep: TCA bracket + board lowered from the back"] = round(
        sum(overlap(Pos(0, 0, -dz) * tca_set, o) for dz in (0.0, 2.0, 4.0, 6.0, 8.0, 10.0) for o in obst), 3)
    yd = parts["profile"]["y_desk"]
    down = bc.w_up(bc.box(-20.0, 20.0, 25.0, 55.0, -2.0, bc.BASE_T + 3.0), yd)
    report["cable route: 40 x 30 bundle down through base hole x stand"] = round(overlap(stand_w, down), 3)
    from build123d import Circle, Plane, Rectangle, extrude
    pr = parts["profile"]
    c, n = pr["C"], pr["n_brace"]
    d = (pr["S4"][0] - c[0], pr["S4"][1] - c[1])
    dl = math.hypot(*d)
    v = bc.BRACE_HOLE[1]
    o = (0, c[0] + d[0] / dl * v, c[1] + d[1] / dl * v)
    back = extrude(Plane(origin=o, x_dir=(1, 0, 0), z_dir=(0, n[0], n[1])) * Rectangle(16.0, 8.0), amount=12.0, both=True)
    report["cable route: USB-A plug out the back window x stand"] = round(overlap(stand_w, back), 3)
    ax, hc = pr["brace_hole_axis"], pr["brace_hole_centre"]
    bun = extrude(Plane(origin=(0, hc[0] + ax[0] * 10.0, hc[1] + ax[1] * 10.0), x_dir=(1, 0, 0),
                        z_dir=(0, -ax[0], -ax[1])) * Circle(22.0), amount=20.0)
    report["cable route: d44 bundle out the round rear window x stand"] = round(overlap(stand_w, bun), 3)
    # OLEDs are pushed straight in from the open back onto their pins
    straight = oled_insertion_sweep()
    report["insertion sweep: OLED straight from the back x front_frame"] = round(overlap(front, straight), 3)
    # optional side exit: with the knock-out cut open, a USB-C plug body (12.5 x 6.5) slides from outside
    # the left wall straight into each port without touching the frame or the module
    front_open = front.cut(bc.side_usb_slot(opened=True))
    side = 0.0
    for y in bc.USB_PORTS.values():
        plug = bc.box(-bc.OUT_X - 15.0, bc.USB_EDGE_X - 0.05, y - 6.25, y + 6.25, bc.USB_Z - 3.25, bc.USB_Z + 3.25)
        side += overlap(front_open, plug) + sum(overlap(bodies[n], plug) for n in ("display_tp", "display_lcd",
                                                                                     "display_bracket", "standoffs"))
    report["side exit (knock-out opened): USB-C plugs slid in through the left wall x frame/module"] = round(side, 3)
    # the plugs and cables running out of the side must not hit the stand either
    side_world = []
    for y in bc.USB_PORTS.values():
        side_world.append(bc.box(-bc.OUT_X - 15.0, bc.USB_EDGE_X - 0.05, y - 6.25, y + 6.25,
                                 bc.USB_Z - 3.25, bc.USB_Z + 3.25))
        side_world.append(bc.box(-bc.OUT_X - 60.0, -bc.OUT_X - 15.0, y - 2.25, y + 2.25,
                                 bc.USB_Z - 2.25, bc.USB_Z + 2.25))
    report["side exit: plugs + cables out of the left wall x stand"] = round(overlap(stand_w, W(Compound(side_world))), 3)
    # ESP32-C3 SuperMini in its bracket pocket above the TCA: nothing printed touches it, the cover posts stop
    # it 0.25 - 0.35 off its underside, it clears the module, TCA, OLED and cable keep-outs, and with the
    # cover off a USB-C plug goes in from the side, behind the ESP32-S3 board
    c3 = bodies["c3_supermini"]
    report["TCA board x bracket (sits on the two seats)"] = round(overlap(bracket, bodies["tca_board"]), 3)
    report["TCA parts/joints + underside wires x bracket/cover"] = round(
        overlap(bracket, bodies["tca_parts_wires"]) + overlap(cover, bodies["tca_parts_wires"]), 3)
    report["TCA screw heads (socket 3.0) x back cover"] = round(overlap(cover, bodies["tca_screw_heads"]), 3)
    report["C3 SuperMini x bracket (pocket, ledges, floor)"] = round(overlap(bracket, c3), 3)
    report["C3 SuperMini x back cover"] = round(overlap(cover, c3), 3)
    report["C3 SuperMini x front frame"] = round(overlap(front, c3), 3)
    report["C3 SuperMini x module, TCA, HY2.0, OLEDs, USB cables"] = round(sum(
        overlap(c3, bodies[n]) for n in ("display_tp", "display_lcd", "display_bracket", "pcba_component_boxes",
                                         "standoffs", "tca_board", "tca_parts_wires", "hy20_plug_wires",
                                         "oled_dupont", "oled_wires", "usb_plugs", "usb_cables")), 3)
    report["C3 pushed 0.2 mm toward the cover x cover posts"] = round(overlap(cover, Pos(0, 0, -0.2) * c3), 3)
    report["C3 pushed 0.5 mm toward the cover x cover posts (posts stop it)"] = round(overlap(cover, Pos(0, 0, -0.5) * c3), 3)
    plug = c3_plug()
    report["C3 USB-C plug, cover off x frame/bracket/module/OLEDs/standoffs"] = round(
        overlap(front, plug) + overlap(bracket, plug) + sum(overlap(bodies[n], plug) for n in (
            "display_tp", "display_lcd", "display_bracket", "pcba_component_boxes", "standoffs", "oled_pcb",
            "oled_header", "oled_dupont", "oled_wires", "oled_fpc", "tca_parts_wires", "usb_cables")), 3)
    # the cover posts stop just behind the PCB: they must not touch the boards, the pins or the FPC
    pins = Compound([bc.cyl(x, y, bc.OLED_PIN_D / 2, bc.OLED_PIN_Z, -bc.OLED_STANDOFF) for x, y in bc.oled_post_xy()])
    report["cover posts x OLED pins (pin tips in the post pockets)"] = round(overlap(cover, pins), 3)
    report["cover x OLED boards, pressed 0.2 mm toward the posts"] = round(
        overlap(cover, Pos(0, 0, -bc.OLED_POST_GAP + 0.01) * Compound(k["oled_pcb"])), 3)
    report["cover x OLED boards, pressed 0.3 mm (posts stop them)"] = round(
        overlap(cover, Pos(0, 0, -bc.OLED_POST_GAP - 0.1) * Compound(k["oled_pcb"])), 3)

    # clearances that matter (mm)
    clear = {
        "glass_edge_to_wall": bc.GAP,
        "hy20_wires_to_cover_inner(after relief)": round((bc.HDR_TOP_Z - 7.0) - (bc.COVER_OUT_Z + 1.25), 2),
        "oled_wires_to_cover_inner": round((-bc.OLED_STANDOFF - bc.OLED_PCB_T - 2.5 - 18.5) - bc.RIM_Z, 2),
        "oled_post_tip_to_pcb_back": bc.OLED_POST_GAP,
        "oled_pin_depth_in_pcb_hole": round(-bc.OLED_STANDOFF - bc.OLED_PIN_Z, 2),
        "oled_board_travel_to_leave_pins_vs_post_gap": [round(-bc.OLED_STANDOFF - bc.OLED_PIN_Z, 2), bc.OLED_POST_GAP],
        "oled_pin_in_hole_radial_play": round(1.0 - bc.OLED_PIN_D / 2, 2),
        "oled_pin_tip_to_post_face": round(bc.OLED_PIN_Z - bc.OLED_POST_Z, 2),
        "rim_boss_top_to_frame_pocket_floor": round(bc.RIM_POCKET_Z - bc.RIM_BOSS_TOP, 2),
        "oled_post_to_bottom_wall": round((bc.OLED_CY - bc.OLED_HOLE_DY - bc.OLED_POST_R) - bc.IN_BOT, 2),
        "cover_boss_preload_on_standoff": round(bc.COVER_BOSS_TOP - bc.STANDOFF_END_Z, 2),
        "usb_plug_front_to_bracket_back(-6.0)": round(-6.0 - (bc.USB_Z + 3.25), 2),
        "socket_head_below_cover_outer_face": round((bc.HEAD_SEAT_Z - 3.0) - bc.COVER_OUT_Z, 2),
        "pan_head_below_cover_outer_face": round((bc.HEAD_SEAT_Z - 2.4) - bc.COVER_OUT_Z, 2),
        "plastic_between_rim_pocket_and_nut": round(bc.TUB_NUT_Z[0] - bc.RIM_POCKET_Z, 2),
        "frame_top_wall_skin_at_pocket": round(bc.OUT_TOP - (bc.TOP_SCREW_Y + bc.RIM_POCKET_R), 2),
        "lower_standoff_ends_pulled_back_onto_bracket_cups": round(bc.STANDOFF_END_Z - bc.BRK_CUP_FLOOR, 2),
        "tca_screw_tip_to_module_back(-7.87)": round(-7.87 - (bc.TCA_HEAD_Z + bc.SCREW_LEN), 2),
        "tca_screw_head_to_cover_relief(socket_3.0)": round((bc.TCA_HEAD_Z - 3.0) - bc.TCA_RELIEF, 2),
        "tca_underside_wires_to_cover_relief": round((bc.TCA_HEAD_Z - 2.6) - bc.TCA_RELIEF, 2),
        "side_usb_knockout_skin": bc.SIDE_USB_SKIN,
        "side_usb_slot_around_plug_y_z(at the plug corners)": side_slot_margins(),
        "c3_underside_to_cover_posts(pcb_1.0,_0.9)": [round((bc.C3_LEDGE_Z - 1.0) - bc.C3_POST_TIP, 2),
                                                        round((bc.C3_LEDGE_Z - 0.9) - bc.C3_POST_TIP, 2)],
        "c3_usb_shell_to_pocket_floor(3.16,_3.12)": [round(bc.C3_FLOOR_Z - (bc.C3_LEDGE_Z + bc.C3_USB_H), 2),
                                                     round(bc.C3_FLOOR_Z - (bc.C3_LEDGE_Z + 3.12), 2)],
        "c3_pocket_fits_board_lengths": [bc.C3_L, bc.C3_L_MAX],
        "c3_usb_shell_length_on_the_bar(min)": round(bc.C3_L - (bc.C3_POCKET_L - bc.C3_BAR_W), 2),
        "c3_usb_mouth_past_stop_face_at_least": round(bc.C3_L + bc.C3_USB_OVER - bc.C3_POCKET_L, 2),
        "c3_pocket_to_tca_board_top_edge": round(bc.C3_WALL_Y - (bc.TCA_C[1] + bc.TCA_SIZE[1] / 2), 2),
        "c3_plug_slot_width": 2 * bc.C3_PLUG_HW,
    }
    # mating heights must sit on the 0.2 + 0.35 n layer boundaries of their part, or the slicer moves them
    # (a top that is off the grid prints one layer taller).  Cover print Z = z - COVER_OUT_Z, frame print
    # Z = FRONT_Z - z.
    on_grid = lambda v: abs(((v - bc.FIRST_LAYER) / bc.LAYER) - round((v - bc.FIRST_LAYER) / bc.LAYER)) < 1e-3  # noqa: E731
    cover_z = {"rim face": bc.RIM_Z, "lip top": bc.RIM_Z + bc.LIP_H, "rim boss top": bc.RIM_BOSS_TOP,
               "head seat": bc.HEAD_SEAT_Z, "standoff boss top": bc.COVER_BOSS_TOP,
               "standoff counterbore floor": bc.STANDOFF_CB_FLOOR, "lower standoff boss top (bracket seat)": bc.BRK_Z0,
               "OLED post tip": bc.OLED_POST_Z,
               "OLED post relief floor": bc.OLED_POST_Z - bc.OLED_POST_RECESS[1], "C3 post tip": bc.C3_POST_TIP,
               "TCA relief floor": bc.TCA_RELIEF}
    frame_z = {"rim": bc.RIM_Z, "rim pocket floor": bc.RIM_POCKET_Z, "nut slot floor": bc.TUB_NUT_Z[0],
               "nut slot roof": bc.TUB_NUT_Z[1], "OLED boss top": -bc.OLED_STANDOFF, "OLED pin tip": bc.OLED_PIN_Z}
    off = [f"cover {k} ({round(v - bc.COVER_OUT_Z, 3)})" for k, v in cover_z.items() if not on_grid(v - bc.COVER_OUT_Z)]
    off += [f"frame {k} ({round(bc.FRONT_Z - v, 3)})" for k, v in frame_z.items() if not on_grid(bc.FRONT_Z - v)]
    brk_z = {"plate top": bc.BRK_Z0 + bc.BRK_T, "cup floor": bc.BRK_CUP_FLOOR, "tower top": bc.BRK_TOP,
             "TCA boss top": bc.TCA_BOSS_TOP, "TCA nut pocket floor": bc.TCA_NUT_FLOOR,
             "C3 ledges": bc.C3_LEDGE_Z, "C3 pocket floor": bc.C3_FLOOR_Z, "C3 pad top": bc.C3_PAD_TOP}
    off += [f"bracket {k} ({round(v - bc.BRK_Z0, 3)})" for k, v in brk_z.items() if not on_grid(v - bc.BRK_Z0)]
    clear["mating_heights_off_the_layer_grid"] = off

    # tipping: printed masses from the sliced G-code when available, display ~250 g (assumed)
    import re
    grams = {}
    for key, fn in (("front", "01_front_frame"), ("cover", "02_back_cover"), ("stand", "03_stand"),
                    ("bracket", "04_tca_bracket")):
        g = HERE / "out" / "gcode" / f"{fn}.gcode"
        if g.exists():
            mm = re.search(r"^; filament used \[g\] = ([0-9.]+)", g.read_text(errors="ignore"), re.M)
            if mm:
                grams[key] = float(mm.group(1))
    rho = 1.24e-3 * 0.55
    masses = []
    for key, s in (("front", W(front)), ("cover", W(cover)), ("stand", stand_w), ("bracket", W(bracket))):
        masses.append((grams.get(key, s.volume * rho), s.center(CenterOf.MASS)))
    dc = W(Pos(0, 0, -6.0) * bc.cyl(0, 0, 1, 0, 1)).center(CenterOf.MASS)
    masses.append((250.0, dc))
    m = sum(x[0] for x in masses)
    com_z = sum(x[0] * x[1].Z for x in masses) / m
    prof = parts["profile"]
    y_desk = prof["y_desk"]
    t = math.radians(bc.TILT)
    ty, tz = bc.c2w(bc.VA_CY + bc.VA_H / 2, bc.FRONT_Z)      # top edge of the visible area
    h = ty - y_desk
    arm_w = com_z - prof["z_rear"]
    arm_v = tz - prof["z_rear"]
    f_tip = (m * 9.81e-3) * arm_w / (math.cos(t) * h - math.sin(t) * arm_v)
    tip = dict(printed_g=grams or "estimated", display_g_assumed=250, total_mass_g=round(m),
               com_z_world=round(com_z, 1), touch_height_mm=round(h, 1),
               tipping_force_N_at_top_of_screen=round(f_tip, 2))
    # desk contact: every part must sit on or above the desk; case and leg both touch it
    desk = {n: round(W(s).bounding_box().min.Y - y_desk, 3)
            for n, s in (("front_frame", front), ("back_cover", cover))}
    desk["stand"] = round(stand_w.bounding_box().min.Y - y_desk, 3)
    desk["front_edge_height_above_desk_mm"] = round(prof["F"][0] - y_desk, 1)
    desk["com_behind_contact_edge_mm"] = round(prof["A"][1] - com_z, 1)
    desk["com_ahead_of_stand_rear_edge_mm"] = round(com_z - prof["z_rear"], 1)
    fast = check_fasteners(front, cover, stand_w, bodies, bracket)
    out = dict(overlaps_mm3=report, clearances_mm=clear, tipping=tip, desk=desk, fasteners=fast)
    (HERE / "out" / "verify_report.json").write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(json.dumps(out, indent=2))

    if "--no-render" in sys.argv:
        return
    render(parts, disp, pcba, k)


def _stl(shape, name, tol=0.05):
    p = CACHE / f"{name}.stl"
    export_stl(shape, str(p), tolerance=tol, angular_tolerance=0.2)
    return str(p)


def render(parts, disp, pcba, k):
    import pyvista as pv
    pv.OFF_SCREEN = True
    pv.global_theme.allow_empty_mesh = True
    y_desk = parts["profile"]["y_desk"]
    lift = Pos(0, -y_desk, 0)
    W = lambda s: lift * bc.to_world(s)  # noqa: E731
    va = bc.box(-bc.VA_W / 2, bc.VA_W / 2, bc.VA_CY - bc.VA_H / 2, bc.VA_CY + bc.VA_H / 2, 0.0, 0.05)
    case_items = {
        "front": (parts["front"], "#34373d"), "cover": (parts["cover"], "#5b6068"),
        "tca_bracket": (parts["bracket"], "#c2410c"),
        "glass": (disp["7INCH-800X480-TP"], "#0d0f12"), "lcd": (disp["7INCH-800X480-LCD"], "#9aa0a6"),
        "bracket": (disp["7INCH-DSI-TOUCHSCREEN-BRACKET"], "#b8bcc2"), "pcba": (pcba, "#1f6f43"),
        "va": (va, "#1b3a6b"), "oled_pcb": (Compound(k["oled_pcb"]), "#1d4ed8"),
        "oled_glass": (Compound(k["oled_glass"]), "#111111"), "oled_va": (Compound(k["_oled_va"]), "#7dd3fc"),
        "oled_dupont": (Compound(k["oled_dupont"] + k["oled_header"]), "#222222"),
        "tca": (Compound(k["tca_board"]), "#7c3aed"), "standoffs": (Compound(k["standoffs"]), "#c9a227"),
        "plugs": (Compound(k["usb_plugs"]), "#2a2a2a"), "cables": (Compound(k["usb_cables"]), "#111111"),
        "c3": (Compound(k["c3_supermini"]), "#0f766e"),
    }
    meshes = {}
    for n, (s, col) in case_items.items():
        tol = 0.15 if n in ("pcba",) else 0.05
        meshes[n] = (pv.read(_stl(W(s), "w_" + n, tol)), col)
    meshes["stand"] = (pv.read(_stl(lift * parts["stand_world"], "w_stand")), "#8a9099")
    desk = pv.Plane(center=(0, -0.2, -30), direction=(0, 1, 0), i_size=520, j_size=360)

    def shot(fname, names, pos, focal, parallel=False, scale=None, desk_on=True, clip=None, size=(1600, 1150),
             src=None, up=(0, 1, 0)):
        src = src or meshes
        pl = pv.Plotter(off_screen=True, window_size=size)
        pl.set_background("#f4f5f7")
        for n in names:
            mesh, col = src[n]
            if clip is not None:
                mesh = mesh.clip(normal=clip[0], origin=clip[1], invert=True)
            if mesh.n_points == 0:
                continue
            pl.add_mesh(mesh, color=col, smooth_shading=False, specular=0.25, specular_power=20)
        if desk_on:
            pl.add_mesh(desk, color="#d9dce1")
        pl.camera.position, pl.camera.focal_point, pl.camera.up = pos, focal, up
        if parallel:
            pl.enable_parallel_projection()
            if scale:
                pl.camera.parallel_scale = scale
        pl.enable_anti_aliasing("ssaa")
        pl.screenshot(str(PREV / fname))
        pl.close()

    allc = list(meshes.keys())
    shot("01_front_3q.png", allc, (-260, 230, 420), (0, 75, -25))
    shot("10_front_eye_level.png", allc, (0, 95, 700), (0, 72, 0))
    zc = -45.0
    shot("11_stand_3q.png", ["stand"], (-190, 210, -330), (0, 30, zc), desk_on=False)
    shot("12_stand_back.png", ["stand"], (150, 160, -300), (0, 35, zc), desk_on=False)
    shot("13_back_low.png", allc, (-120, 60, -420), (0, 45, -40))
    shot("02_back_3q.png", allc, (-330, 210, -420), (0, 70, -30))
    shot("03_side.png", allc, (-600, 80, -25), (0, 80, -25), parallel=True, scale=95)
    inside = [n for n in allc if n not in ("cover", "stand")]
    shot("04_inside_from_back.png", inside, (-120, 160, -520), (0, 75, -10), desk_on=False)
    # sections in the (untilted) case frame
    flat = {n: (pv.read(_stl(s, "c_" + n, 0.15 if n == "pcba" else 0.05)), col) for n, (s, col) in case_items.items()}
    secy = bc.USB_PORTS["USB"]
    shot("05_usb_section_top.png", list(flat.keys()), (-60, 400, -14), (-60, secy, -14), parallel=True, scale=30,
         desk_on=False, clip=((0, 1, 0), (0, secy, 0)), src=flat, up=(0, 0, 1))
    shot("07_inside_case_frame.png", [n for n in flat if n != "cover"], (0, 0, -400), (0, -15, -10),
         parallel=True, scale=82, desk_on=False, src=flat)
    shot("08_section_x0_side.png", list(flat.keys()), (-400, -15, -14), (0, -15, -14), parallel=True, scale=80,
         desk_on=False, clip=((-1, 0, 0), (0.0, 0, 0)), src=flat)

    # exploded view in the case frame (no tilt), parts pulled apart along Z
    ex = {
        "front": (parts["front"], 0), "glass": (disp["7INCH-800X480-TP"], -45), "lcd": (disp["7INCH-800X480-LCD"], -45),
        "bracket": (disp["7INCH-DSI-TOUCHSCREEN-BRACKET"], -45), "pcba": (pcba, -45),
        "oled_pcb": (Compound(k["oled_pcb"]), -25), "oled_glass": (Compound(k["oled_glass"]), -25),
        "tca": (Compound(k["tca_board"]), -95), "tca_bracket": (parts["bracket"], -95),
        "c3": (Compound(k["c3_supermini"]), -112),
        "cover": (parts["cover"], -135),
    }
    pl = pv.Plotter(off_screen=True, window_size=(1600, 1150))
    pl.set_background("#f4f5f7")
    for n, (s, dz) in ex.items():
        col = case_items[n][1] if n in case_items else "#888"
        pl.add_mesh(pv.read(_stl(Pos(0, 0, dz) * s, "ex_" + n, 0.15 if n == "pcba" else 0.05)), color=col, specular=0.2)
    st = Pos(0, 0, -190) * (Rot(bc.TILT, 0, 0) * parts["stand_world"])
    pl.add_mesh(pv.read(_stl(st, "ex_stand")), color="#8a9099")
    pl.camera.position, pl.camera.focal_point, pl.camera.up = (420, 260, -330), (0, -10, -80), (0, 1, 0)
    pl.enable_anti_aliasing("ssaa")
    pl.screenshot(str(PREV / "06_exploded.png"))
    pl.close()
    print("renders written to", PREV)


if __name__ == "__main__":
    main()
