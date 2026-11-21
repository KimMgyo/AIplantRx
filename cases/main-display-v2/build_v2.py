#!/usr/bin/env python3
"""
Main display case v2 - modular, bezel-less, every part a short support-free print (build123d 0.13)

Hardware: Waveshare ESP32-S3-Touch-LCD-7 (official STEP), 5 x GME12864 0.96" OLED, TCA9548A, and a TENSTAR
ESP32-C3 SuperMini whose USB-C is now the only cable: C3 5V/GND -> S3 I2C header (10) VCC/GND, C3 UART <->
S3 UART2 header (7), C3 I2C -> TCA9548A -> 5 OLEDs (wires soldered straight to the OLED header pins).

Frame (same as v1): X right (seen from the front), Y up, Z toward the viewer.  Z = 0 is the 7" glass front
face, the glass centre is the XY origin.  The module's PCB / USB side is up, its USB-C ports on the left.

Parts
  pod_left, pod_right    deep back plates.  Inner face 25.55 behind the glass: the UART2 (7) and I2C (10)
                         plugs with their wires need 25 mm.  They carry the module's 4 brass standoffs.
  spine                  the connector-free middle is shallow (16.1); a thick spine fills it.  It is the
                         stand's back plate and the case's centre back at once, so the stand adds no depth.
  wall_left, wall_right  the outer walls only (no bezel: the glass is the front), 0.55 proud of the glass.
                         Printed standing on their back edge; dovetailed at the seams; screwed from the back
                         into the rims of the pods and spine.
  oled_body, oled_lid    x5, one cell per OLED: body = window + d1.7 locating pins; lid = 4 posts, goes in from
                         behind 2.5 low, slides 2.5 up into the body's grooves and has the foot that one M3 x 10
                         pulls onto the back.
  oled chin variants     onepiece_parts(): the 5 cells as one body + one lid.  even_parts(): the same, with the
                         5 windows evenly spaced (wall | window | ... | window | wall gaps all 9.76).
  c3_cart, c3_lid        ESP32-C3 SuperMini cartridge (cavity and USB-C opening copied from the user's
                         23mm-case), standing behind the middle OLED with its USB-C straight out the back.
  leg x2                 flat triangles; their dovetail tongues slide up into slots in the spine's back.

Fasteners: M3 x 10 into M3 nuts (5.5 AF x 2.4) or F-3mm brass hex supports (5.5 AF x 3.0) everywhere.
Printer: Prusa i3 MK2S, stock 0.4 nozzle, PrusaSlicer 0.35mm FAST (first layer 0.20): heights that matter
sit on L(n) = 0.20 + 0.35 n measured from each part's bed.
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
V1 = HERE.parent / "main-display"
sys.path.insert(0, str(V1))

from build123d import (Compound, Cone, Cylinder, Kind, Plane, Polygon, Pos, Rot, export_step,  # noqa: E402
                       export_stl, extrude, offset)
import build_case as v1  # noqa: E402

OUT = HERE / "out"
L, rr, rr_loft, cyl, box, hex_prism = v1.L, v1.rr, v1.rr_loft, v1.cyl, v1.box, v1.hex_prism
CCM = v1.CCM

# ------------------------------------------------------------------ module and outline
GLASS_W, GLASS_H, GLASS_R = v1.GLASS_W, v1.GLASS_H, v1.GLASS_R
STANDOFFS = v1.STANDOFFS
GAP, WALL_T = 0.30, 2.40
IN_X, IN_TOP, IN_R = GLASS_W / 2 + GAP, GLASS_H / 2 + GAP, GLASS_R + GAP          # 96.78, 55.68, 8.18
OUT_X, OUT_TOP, OUT_R = IN_X + WALL_T, IN_TOP + WALL_T, IN_R + WALL_T             # 99.18, 58.08, 10.58

# OLED chin (GME12864: PCB 27.5 x 27.8, holes 23.2 x 23.5, header edge up)
OLED_W, OLED_H, OLED_PCB_T = v1.OLED_W, v1.OLED_H, v1.OLED_PCB_T
HOLE_DX, HOLE_DY = v1.OLED_HOLE_DX, v1.OLED_HOLE_DY
OLED_X = v1.OLED_X
PITCH = v1.OLED_PITCH                       # 33.284: outer lit areas line up with the 7" visible area
CHIN_TOP = -IN_TOP                          # -55.68 top edge of the cells (0.3 under the glass)
OLED_TOP = -58.0
OLED_CY = OLED_TOP - OLED_H / 2             # -71.90
IN_BOT = OLED_TOP - OLED_H - 0.5            # -86.30
OUT_BOT = IN_BOT - WALL_T                   # -88.70

# ------------------------------------------------------------------ depth
Z_BACK = -27.85                             # back face of pods and spine
Z_PLATE = Z_BACK + L(6)                     # -25.55 pod inner face: 25.55 mm for the plugs and wires
RIM_TOP = Z_BACK + L(14)                    # -22.75 rims (pods) / end bands (spine); the walls stand here
Z_FRONT = RIM_TOP + L(66)                   # +0.55 wall front edge: 0.55 proud of the glass
Z_SPINE = Z_BACK + L(33)                    # -16.10 spine front face = step wall top (PCBA parts end -13.7)
BOSS_TOP = Z_BACK + L(32)                   # -16.45 standoff bosses (standoff ends -16.50: 0.05 preload)
STANDOFF_CB = Z_BACK + L(13)                # -23.10 head seat: M3 x 10 bites 3.4 mm into the standoff
SEAT_Z = Z_BACK + L(9)                      # -24.50 head seat under every rim joint (1.75 mm of plastic)
NUT_Z = (RIM_TOP + L(3), RIM_TOP + L(12))   # (-21.50, -18.35) nut slots in wall bosses and cell feet
WB_TOP, WB_R = RIM_TOP + L(24), 3.75        # wall bosses
CB_R, CLEAR_R, AF = 3.2, 1.7, 5.75          # d6.4 counterbore, d3.4 hole, nut / F-3mm support slot
CHAN_X, STEP_T = 40.0, 2.40                 # channel (spine) half width, step wall
SPINE_HW = CHAN_X - 0.30
CH = 0.8                                    # outer edge chamfers
SPINE_FRONT_Y = (-78.3, 47.9)               # the spine's front part (rest of it: back band only)

# ------------------------------------------------------------------ OLED cells
CELL_GAP = 0.30
CELL_HW = PITCH / 2 - CELL_GAP / 2          # 16.49
END_GAP = 0.15                              # outer OLED cells reach the side walls (0.15 short of them)
FACE_T = L(5)                               # 1.95; the OLED glass rests on its back face
STANDOFF_OLED = 2.10                        # glass + tape between face and PCB (fit-tested in v1)
PCB_FRONT = -(FACE_T + STANDOFF_OLED)       # -4.05
PCB_BACK = PCB_FRONT - OLED_PCB_T           # -5.25
PIN_END = -L(19)                            # -6.85: d1.7 pins run 1.6 mm through the PCB (the length that held
                                            # the OLEDs best in the v1 tests)
POCKET_HW = OLED_W / 2 + 0.15               # 13.90
GROOVE_Z = (-L(20), -L(27))                 # (-7.20, -9.65) lid groove in the rails (body print heights)
GROOVE_D = 1.2
RAIL_TOP = -L(32)                           # -11.40
GROOVE_STOP_Y = -57.5                       # grooves end here
LID_Z = (-7.35, -9.50)                      # lid plate front / back
# The lid goes in straight from behind, 2.5 mm low, with its 3 pairs of edge tabs through notches in the groove
# lips, then slides 2.5 mm up so the tabs sit under the lips.  The posts travel only 2.5 mm over the board, well
# clear of the parts row across the OLED's back (C6 ... C3 sit right in the post columns); the long pins enter the
# U-shaped posts from the top while it slides.
LID_SLIDE = 2.5
LID_HW = POCKET_HW - 0.10                   # 13.80 plate: passes between the rails
TAB_HW = POCKET_HW + GROOVE_D - 0.10        # 15.00 tabs reach into the grooves
TAB_LEN = 6.0
LID_TABS = [(GROOVE_STOP_Y - 0.1 - TAB_LEN, GROOVE_STOP_Y - 0.1), (OLED_CY - 3.0, OLED_CY + 3.0),
            (OLED_CY - 11.0, OLED_CY - 5.0)]
LIP_NOTCHES = [(t0 - LID_SLIDE - 0.1, t1 - LID_SLIDE + 0.1) for t0, t1 in LID_TABS]
LID_TOP_Y = GROOVE_STOP_Y - 0.10
POST_GAP = 0.20                             # posts stop 0.2 behind the PCB (as in v1)
POST_TIP = PCB_BACK - POST_GAP              # -5.45
POST_U = (1.2, 2.3, -1.4, 1.2, -1.1)        # slot half width, outer half width, U bottom, U top, slot bottom
                                            # (from the hole centre; pin r 0.85: 0.35 side / 0.25 bottom clearance)
FOOT_HW, FOOT_TOP, FOOT_Y = 5.0, -78.6, -82.4

# ------------------------------------------------------------------ ESP32-C3 SuperMini cartridge
C3_CAV_W, C3_CAV_L = 18.15, 23.35           # user's 23mm-case cavity
C3_FLOOR, C3_WALL, C3_END = 1.0, 1.6, 1.5
C3_CAV_H = 5.6                              # floor to lid: PCB 1.0 + USB-C 3.16 + wires
C3_LID_T, C3_GROOVE, C3_LIP = 1.0, 0.8, 0.8
C3_USB_OPEN = (4.5, 2.1, 5.8)               # half width, y0, y1 above the box bottom (user's case)
C3_HW = C3_CAV_W / 2 + C3_WALL              # 10.675
C3_Y0 = -75.9
C3_Y1 = C3_Y0 + C3_FLOOR + C3_CAV_H + C3_LID_T + 0.2 + C3_LIP   # -67.30
C3_YM = (C3_Y0 + C3_Y1) / 2
C3_LEN = 2 * C3_END + C3_CAV_L              # 26.35
C3_FRONT_Z = LID_Z[1] - 0.45                # -9.95 (the middle cell's lid 0.45 in front)
C3_USB_Z = C3_FRONT_Z - C3_LEN              # -36.30: USB-C face 8.45 behind the spine, between the legs
C3_EAR_X, C3_EAR_SCREW_X = 17.5, 14.1
C3_EAR_Z = Z_BACK - L(6)                    # -30.15 ear back face = screw head seat
C3_NUT_Z = Z_BACK + 2.95                    # -24.90 nut seat in the spine

# ------------------------------------------------------------------ TCA9548A (left pod, behind OLED 1-2)
TCA_C = (-59.0, -67.0)
TCA_PITCH, TCA_SIZE, TCA_T = v1.TCA_PITCH, v1.TCA_SIZE, 1.6
TCA_POST_TOP = Z_BACK + L(25)               # -18.90 TCA underside (parts face the OLEDs, wires behind)
TCA_NUT_Z = (Z_BACK + L(14), Z_BACK + L(23))

# ------------------------------------------------------------------ stand legs
TILT = v1.TILT                              # 20 deg, screen leans back
LEG_X, LEG_T = 24.0, 8.0
LEG_TOP_Y = 20.0
BASE_L = 85.0
LEG_BAR = 7.5
# half dovetail: +x side straight (it lies on the bed when the leg prints flat), -x side flared at 45 deg
SLOT_PTS = [(-1.7, -1.0), (-1.7, 1.0), (-4.2, 3.5), (-4.2, 4.1), (4.2, 4.1), (4.2, -1.0)]      # (dx, dz from Z_BACK)
TONGUE_PTS = [(-1.5, -1.0), (-1.5, 1.2), (-4.0, 3.7), (-4.0, 3.8), (4.0, 3.8), (4.0, -1.0)]
TIE = (5.5, 2.8)

# the two wall halves meet on the centre line, top and bottom, so every visible parting line is symmetric
SEAM_X = 0.0

# where the walls are screwed down (x, y, into): the boss stands 3.5 from the wall's inner face
WB = [(-(IN_X - 3.5), 30.0, "pod"), (-(IN_X - 3.5), -25.0, "pod"),
      ((IN_X - 3.5), 30.0, "pod"), ((IN_X - 3.5), -25.0, "pod"),
      (-70.0, IN_TOP - 3.5, "pod"), (70.0, IN_TOP - 3.5, "pod"),
      (-28.0, IN_TOP - 3.5, "spine"), (28.0, IN_TOP - 3.5, "spine"),
      (-90.0, IN_BOT + 3.5, "pod"), (90.0, IN_BOT + 3.5, "pod"),
      (-57.5, IN_BOT + 3.5, "pod"), (57.5, IN_BOT + 3.5, "pod"),
      (-12.0, IN_BOT + 3.5, "spine"), (12.0, IN_BOT + 3.5, "spine")]
CELL_SCREWS = [(x, FOOT_Y) for x in OLED_X]
SPINE_SCREWS = [(x, y) for x, y, k in WB if k == "spine"] + [(x, FOOT_Y) for x in OLED_X if abs(x) < CHAN_X]
POD_CELL_SCREWS = [(x, FOOT_Y) for x in OLED_X if abs(x) > CHAN_X]
VENT_X = [s * x for s in (-1, 1) for x in (50.0, 55.0, 72.0, 78.0, 84.0)]   # mirror-symmetric, clear of both
                                                                            # standoff columns (-61.61 / +64.59)
VENT_Y = [(18.0, 46.0), (-50.0, -40.0)]


# ------------------------------------------------------------------ helpers
def prism_xz(pts, y0, y1):
    """Polygon in (x, z) extruded along y from y0 to y1."""
    pl = Plane(origin=(0, y1, 0), x_dir=(1, 0, 0), z_dir=(0, -1, 0))
    return extrude(pl * Polygon(*pts, align=None), amount=y1 - y0, dir=(0, -1, 0))


def prism_yz(pts, x0, x1):
    """Polygon in (y, z) extruded along x from x0 to x1."""
    pl = Plane(origin=(x0, 0, 0), x_dir=(0, 1, 0), z_dir=(1, 0, 0))
    return extrude(pl * Polygon(*pts, align=None), amount=x1 - x0, dir=(1, 0, 0))


def box_back_ch(x0, x1, y0, y1, z0, z1, ch, edges="xy"):
    """Box whose back face (z0) has its edges chamfered by ch ('x': the x0/x1 edges, 'y': the y0/y1 edges)."""
    b = box(x0, x1, y0, y1, z0, z1)
    cut = []
    if "x" in edges:
        for xe, s in ((x0, 1), (x1, -1)):
            cut.append(prism_xz([(xe - s * 0.01, z0 - 0.01), (xe + s * ch, z0 - 0.01), (xe - s * 0.01, z0 + ch)],
                                y0 - 1, y1 + 1))
    if "y" in edges:
        for ye, s in ((y0, 1), (y1, -1)):
            cut.append(prism_yz([(ye - s * 0.01, z0 - 0.01), (ye + s * ch, z0 - 0.01), (ye - s * 0.01, z0 + ch)],
                                x0 - 1, x1 + 1))
    return b.cut(*cut)


def outline(z0, z1, inner=False, ch_back=0.0, ch_front=0.0):
    """Case outline (outer or inner) prism with optional chamfers on the back / front edges."""
    if inner:
        x, b, t, r = IN_X, IN_BOT, IN_TOP, IN_R
    else:
        x, b, t, r = OUT_X, OUT_BOT, OUT_TOP, OUT_R
    s = rr(-x, x, b, t, r, z0 + ch_back, z1 - ch_front)
    if ch_back:
        s = s + rr_loft(-x, x, b, t, r, z0 + ch_back, -ch_back, z0)
    if ch_front:
        s = s + rr_loft(-x, x, b, t, r, z1 - ch_front, -ch_front, z1)
    return s


def nut_slot(x, y, z0, z1, d, length=9.0, af=AF):
    """Hex pocket at (x, y) for a Z screw, opened sideways toward d = (+-1, 0) or (0, +-1)."""
    dx, dy = d
    if dy:
        return hex_prism(x, y, af, z0, z1, 90.0) + box(x - af / 2, x + af / 2, y, y + dy * length, z0, z1)
    return hex_prism(x, y, af, z0, z1, 0.0) + box(x, x + dx * length, y - af / 2, y + af / 2, z0, z1)


def wall_dir(x, y):
    """Unit direction from a wall boss centre to its wall."""
    if abs(abs(x) - (IN_X - 3.5)) < 0.01:
        return (1 if x > 0 else -1, 0)
    return (0, 1) if y > 0 else (0, -1)


def seat_cut(x, y, seat=SEAT_Z, top=None):
    """Counterbore from the back face to the head seat + clearance hole on up."""
    top = RIM_TOP + 1.0 if top is None else top
    return cyl(x, y, CB_R, Z_BACK - 1.0, seat) + cyl(x, y, CLEAR_R, Z_BACK - 1.0, top)


# ------------------------------------------------------------------ pods
def make_pod(s):
    """s = -1 left, +1 right: deep back plate from the channel edge outward."""
    clip = box(s * CHAN_X, s * (OUT_X + 5.0), OUT_BOT - 5.0, OUT_TOP + 5.0, Z_BACK - 5.0, 5.0)
    tray = outline(Z_BACK, RIM_TOP, ch_back=CH) - outline(Z_PLATE, RIM_TOP + 1.0, inner=True)
    pod = tray & clip
    add = [box(s * CHAN_X, s * (CHAN_X + STEP_T), IN_BOT, IN_TOP, Z_BACK, Z_SPINE),         # step wall (channel side)
           box(s * CHAN_X, s * (CHAN_X + STEP_T), OUT_BOT, OUT_TOP, Z_BACK, RIM_TOP) & outline(Z_BACK - 1, 0)]
    for x, y in STANDOFFS:
        if x * s > 0:
            add.append(cyl(x, y, 4.5, Z_PLATE - 0.1, BOSS_TOP))
    for x, y, k in WB:
        if k == "pod" and x * s > 0:
            dx, dy = wall_dir(x, y)
            add.append(cyl(x, y, 4.25, Z_BACK + 0.5, RIM_TOP))
            if dx:
                add.append(box(x, x + dx * 6.0, y - 4.25, y + 4.25, Z_BACK + 0.5, RIM_TOP))
            else:
                add.append(box(x - 4.25, x + 4.25, y, y + dy * 6.0, Z_BACK + 0.5, RIM_TOP))
            add.append(box(min(x, x + dx * 6.0), max(x, x + dx * 6.0) + (8.5 if dy else 0) * 0, y - 4.25, y + 4.25,
                           Z_BACK + 0.5, RIM_TOP) if dx else
                       box(x - 4.25, x + 4.25, min(y, y + dy * 6.0), max(y, y + dy * 6.0), Z_BACK + 0.5, RIM_TOP))
    for x, y in POD_CELL_SCREWS:
        if x * s > 0:
            add.append(cyl(x, y, 4.5, Z_PLATE - 0.1, RIM_TOP))
            add.append(box(x - 4.5, x + 4.5, IN_BOT - 0.5, y, Z_PLATE - 0.1, RIM_TOP))
    if s < 0:
        for k in (-1, 1):
            add.append(cyl(TCA_C[0] + k * TCA_PITCH / 2, TCA_C[1], 3.5, Z_PLATE - 0.1, TCA_POST_TOP))
    pod = pod.fuse(*add) & outline(Z_BACK - 1.0, 0.0)
    # chamfer the back edge along the channel (V groove with the spine's chamfer)
    cut = [prism_xz([(s * CHAN_X - s * 0.01, Z_BACK - 0.01), (s * (CHAN_X + CH), Z_BACK - 0.01),
                     (s * CHAN_X - s * 0.01, Z_BACK + CH)], OUT_BOT - 1, OUT_TOP + 1)]
    for x, y in STANDOFFS:
        if x * s > 0:
            cut.append(cyl(x, y, CB_R, Z_BACK - 1.0, STANDOFF_CB))
            cut.append(cyl(x, y, CLEAR_R, Z_BACK - 1.0, BOSS_TOP + 1.0))
    for x, y, k in WB:
        if k == "pod" and x * s > 0:
            cut.append(seat_cut(x, y))
    for x, y in POD_CELL_SCREWS:
        if x * s > 0:
            cut.append(seat_cut(x, y))
    if s < 0:
        for k in (-1, 1):
            x = TCA_C[0] + k * TCA_PITCH / 2
            cut.append(cyl(x, TCA_C[1], CLEAR_R, Z_BACK + 0.4, TCA_POST_TOP + 1.0))       # blind: 0.4 skin
            cut.append(nut_slot(x, TCA_C[1], TCA_NUT_Z[0], TCA_NUT_Z[1], (0, 1)))
    for x in VENT_X:
        if x * s > 0:
            for y0, y1 in VENT_Y:
                if all(math.hypot(x - sx, min(abs(y0 - sy), abs(y1 - sy), 0 if y0 < sy < y1 else 99)) > 6.2
                       for sx, sy in STANDOFFS):
                    cut.append(rr(x - 1.25, x + 1.25, y0, y1, 1.2, Z_BACK - 1.0, Z_PLATE + 1.0))
    return pod.cut(*cut)


# ------------------------------------------------------------------ spine
# six big pockets in the spine's inside face, down to the 5.1 mm back band (which carries the leg slots):
# a ribbed 11.75 mm deep section.  Print time is set by the solid back skin (the footprint), not the pockets.
SPINE_POCKETS = ([(x0, x1, y0, y1, RIM_TOP) for x0, x1 in ((-37.3, -1.2), (1.2, 37.3))
                  for y0, y1 in ((-60.0, -21.2), (-18.8, 20.6), (23.0, 45.5))]
                 + [(x0, x1, -76.0, -62.4, RIM_TOP) for x0, x1 in ((-37.3, -19.6), (19.6, 37.3))])


def make_spine():
    hw = SPINE_HW
    sp = box_back_ch(-hw, hw, OUT_BOT, OUT_TOP, Z_BACK, RIM_TOP, CH)
    sp = sp + box(-hw, hw, SPINE_FRONT_Y[0], SPINE_FRONT_Y[1], RIM_TOP - 0.1, Z_SPINE)
    cut = [box(x0, x1, y0, y1, z0, Z_SPINE + 1.0) for x0, x1, y0, y1, z0 in SPINE_POCKETS]
    for x, y in SPINE_SCREWS:
        cut.append(seat_cut(x, y, top=Z_SPINE + 1.0))
    cut.append(box(-C3_HW - 0.2, C3_HW + 0.2, C3_Y0 - 0.2, C3_Y1 + 0.2, Z_BACK - 1.0, Z_SPINE + 1.0))
    for s in (-1, 1):
        x = s * C3_EAR_SCREW_X
        cut.append(cyl(x, C3_YM, CLEAR_R, Z_BACK - 1.0, Z_SPINE + 1.0))
        cut.append(hex_prism(x, C3_YM, AF, C3_NUT_Z, Z_SPINE + 1.0, 90.0))
        xl = s * LEG_X
        cut.append(prism_xz([(xl + dx, Z_BACK + dz) for dx, dz in SLOT_PTS], OUT_BOT - 1.0, LEG_TOP_Y - 2.0))
    return sp.cut(*cut)


# ------------------------------------------------------------------ walls
def wall_ring():
    ring = outline(RIM_TOP, Z_FRONT, ch_front=CH) - outline(RIM_TOP - 1.0, Z_FRONT + 1.0, inner=True)
    ring = ring - rr_loft(-IN_X, IN_X, IN_BOT, IN_TOP, IN_R, Z_FRONT - 0.3, 0.6, Z_FRONT + 0.3)
    add = []
    for x, y, k in WB:
        dx, dy = wall_dir(x, y)
        b = cyl(x, y, WB_R, RIM_TOP, WB_TOP)
        if dx:
            b = b + box(min(x, x + dx * 4.0), max(x, x + dx * 4.0), y - WB_R, y + WB_R, RIM_TOP, WB_TOP)
        else:
            b = b + box(x - WB_R, x + WB_R, min(y, y + dy * 4.0), max(y, y + dy * 4.0), RIM_TOP, WB_TOP)
        add.append(b)
    # dovetail block at the top seam; at the bottom seam the middle OLED's foot sits there, so each half is
    # screwed down 12 mm either side of the centre line instead
    add.append(box(SEAM_X - 7.0, SEAM_X + 7.0, IN_TOP - 4.5, IN_TOP + 0.5, RIM_TOP, -12.0))
    ring = ring.fuse(*add) & outline(RIM_TOP - 1.0, Z_FRONT + 1.0)
    cut = []
    for x, y, k in WB:
        dx, dy = wall_dir(x, y)
        cut.append(cyl(x, y, CLEAR_R, RIM_TOP - 1.0, WB_TOP + 1.0))
        cut.append(nut_slot(x, y, NUT_Z[0], NUT_Z[1], (-dx, -dy)))
    return ring.cut(*cut)


def seam_tongue(xs, yc, clr=0.0):
    """Dovetail at a seam (plan view), root 1 mm right of xs, tip 4 mm left of it, through the seam block height.
    clr > 0 gives the socket (cut from the left half)."""
    pts = [(xs + 1.0, yc - 1.3 - clr), (xs - 4.0 - clr, yc - 2.1 - clr), (xs - 4.0 - clr, yc + 2.1 + clr),
           (xs + 1.0, yc + 1.3 + clr)]
    z0 = RIM_TOP - (1.0 if clr else 0.0)
    z1 = -12.0 + (0.3 if clr else 0.0)
    return extrude(Plane.XY.offset(z0) * Polygon(*pts, align=None), amount=z1 - z0, dir=(0, 0, 1))


SEAM_YC = (IN_TOP - 4.5 + OUT_TOP) / 2


def make_walls():
    ring = wall_ring()
    left = (ring & box(-200.0, SEAM_X, -200.0, 200.0, -100.0, 100.0)).cut(seam_tongue(SEAM_X, SEAM_YC, 0.2))
    right = (ring & box(SEAM_X, 200.0, -200.0, 200.0, -100.0, 100.0)).fuse(ring & seam_tongue(SEAM_X, SEAM_YC))
    return left, right


# ------------------------------------------------------------------ OLED cell
def make_cell_body(fx0=-CELL_HW, fx1=CELL_HW):
    """One cell, centred on x = 0 (OLED centre (0, OLED_CY)); fx0 / fx1 = face edges (wider for the end cells)."""
    cy = OLED_CY
    body = rr(fx0, fx1, IN_BOT, CHIN_TOP, 0.6, -FACE_T, -0.3)
    body = body + rr_loft(fx0, fx1, IN_BOT, CHIN_TOP, 0.6, -0.3, -0.3, 0.0)
    add = []
    for sx in (-1, 1):
        x = sx * HOLE_DX
        add.append(cyl(x, cy - HOLE_DY, 2.0, PCB_FRONT, -FACE_T + 0.01))
        add.append(cyl(x, cy + HOLE_DY, 2.0, PCB_FRONT, -FACE_T + 0.01)
                   & box(x - 3.0, x + 3.0, cy + v1.OLED_STRIP_DY[0], cy + HOLE_DY + 3.0, PCB_FRONT - 0.1, 0.0))
        for y in (cy - HOLE_DY, cy + HOLE_DY):
            add.append(cyl(x, y, v1.OLED_PIN_D / 2, PIN_END + 0.35, PCB_FRONT + 0.01))
            add.append(Pos(x, y, PIN_END) * Cone(v1.OLED_PIN_D / 2 - 0.3, v1.OLED_PIN_D / 2, 0.35, align=CCM))
        x0, x1 = sorted((sx * POCKET_HW, sx * CELL_HW))
        add.append(box(x0, x1, IN_BOT, CHIN_TOP, RAIL_TOP, -FACE_T + 0.01))
    add.append(box(-HOLE_DX, HOLE_DX, cy + v1.OLED_STRIP_DY[0], cy + v1.OLED_STRIP_DY[1], PCB_FRONT, -FACE_T + 0.01))
    body = body.fuse(*add)
    cut = []
    wy = cy + v1.OLED_WIN_DY
    hw, hh = v1.OLED_WIN_W / 2, v1.OLED_WIN_H / 2
    cut.append(rr(-hw, hw, wy - hh, wy + hh, 0.5, -FACE_T - 1.0, 1.0))
    cut.append(rr_loft(-hw, hw, wy - hh, wy + hh, 0.5, -v1.OLED_WIN_CH, v1.OLED_WIN_CH + 0.3, 0.3))
    cut.append(box(-5.3, 5.3, cy + v1.OLED_STRIP_DY[1] + 0.05, OLED_TOP + 0.15, -FACE_T - 0.5, -FACE_T + L(2)))
    for sx in (-1, 1):                                      # lid grooves, 45 deg ceiling (prints face down)
        a, b = sx * (POCKET_HW - 0.01), sx * (POCKET_HW + GROOVE_D)
        cut.append(prism_xz([(a, GROOVE_Z[0]), (b, GROOVE_Z[0]), (b, GROOVE_Z[1]), (a, GROOVE_Z[1] - GROOVE_D)],
                            IN_BOT - 1.0, GROOVE_STOP_Y))
        for n0, n1 in LIP_NOTCHES:                          # entry notches in the lips behind the grooves
            n0 = IN_BOT - 1.0 if n0 < IN_BOT + 2.0 else n0
            cut.append(box(a, b + 0.01, n0, n1, RAIL_TOP - 1.0, GROOVE_Z[0] - 0.01))
    return body.cut(*cut)


def make_cell_lid(foot_dx=0.0):
    """Same lid for all five cells.  foot_dx moves the screw foot sideways (the evenly spaced chin keeps its
    screws where the pods / spine have their bosses)."""
    cy = OLED_CY
    lid = box(-LID_HW, LID_HW, IN_BOT, LID_TOP_Y, LID_Z[1], LID_Z[0])
    add = []
    for sx in (-1, 1):
        for t0, t1 in LID_TABS:
            add.append(box(sx * (LID_HW - 0.01), sx * TAB_HW, t0, t1, LID_Z[1], LID_Z[0]))
        hx = sx * HOLE_DX
        sl, ow, ub, ut, sb = POST_U
        xo = sx * min(HOLE_DX + ow, POCKET_HW - 0.1)
        for sy in (-1, 1):                                  # U-shaped posts, open on top for the long pins
            hy = cy + sy * HOLE_DY
            z0, z1 = POST_TIP, LID_Z[0] - 0.01
            add.append(box(hx - sx * ow, hx - sx * sl, hy + ub, hy + ut, z0, z1))
            add.append(box(hx + sx * sl, xo, hy + ub, hy + ut, z0, z1))
            add.append(box(hx - sx * ow, xo, hy + ub, hy + sb, z0, z1))
    add.append(box(foot_dx - FOOT_HW, foot_dx + FOOT_HW, IN_BOT, FOOT_TOP, RIM_TOP, LID_Z[0]))
    lid = lid.fuse(*add)
    cut = [box(-6.0, 6.0, OLED_TOP - 4.5, LID_TOP_Y + 1.0, LID_Z[1] - 1.0, LID_Z[0] + 1.0),       # header + wires
           cyl(foot_dx, FOOT_Y, CLEAR_R, RIM_TOP - 1.0, NUT_Z[1] + 4.0),
           nut_slot(foot_dx, FOOT_Y, NUT_Z[0], NUT_Z[1], (0, 1), length=6.0)]
    return lid.cut(*cut)


def make_end_body(s, cx=None):
    """Outer OLED cell (s = -1 left, +1 right): same OLED bay, its face runs on to the side wall so the five
    cells fill the chin from wall to wall, and it fills the corner left by the glass' rounded bottom corner
    (front face only).  Returned centred on its OLED like the middle cells."""
    if cx is None:
        cx = OLED_X[-1] if s > 0 else OLED_X[0]
    xe = s * (IN_X - END_GAP) - cx                          # outer face edge, local
    fx0, fx1 = (-CELL_HW, xe) if s > 0 else (xe, CELL_HW)
    body = make_cell_body(fx0, fx1)
    rib = box(xe - s * 2.0, xe, IN_BOT, CHIN_TOP, RAIL_TOP, -FACE_T + 0.01)            # stiffens the wide face
    xc0, xc1 = sorted((s * (IN_X - IN_R - 1.0) - cx, xe))
    corner = (box(xc0, xc1, CHIN_TOP - 0.5, CHIN_TOP + IN_R + 0.3, -FACE_T, 0.0)
              - Pos(-cx, 0, 0) * rr(-IN_X, IN_X, CHIN_TOP, IN_TOP + 10.0, IN_R, -5.0, 5.0))  # glass corner + 0.3 gap
    body = body.fuse(rib, corner)
    # follow the wall's rounded inner corner at the bottom
    return body & (Pos(-cx, 0, 0) * rr(-IN_X + END_GAP, IN_X - END_GAP, IN_BOT, IN_TOP + 20.0, IN_R - END_GAP,
                                       -50.0, 50.0))


# ------------------------------------------------------------------ one-piece OLED chin (alternative to the 5 cells)
CELL_BOUNDS = [(OLED_X[i] + OLED_X[i + 1]) / 2 for i in range(4)]       # +-16.64, +-49.93
CHIN_BEAM_Z = (-11.70, -13.90)              # lid beam: behind the body rails (-11.40), in front of the wall
                                            # bosses (-14.15) and the step walls (-16.10)


def make_chin_body(xs=OLED_X):
    """All five OLED bays in one piece (case coordinates): one continuous face from wall to wall, the same
    bays, pins, windows and lid grooves as the cells, so the five separate lids fit it too.  xs = OLED centres;
    between two bays the rails are joined into one solid web."""
    cells = []
    for i, cx in enumerate(xs):
        c = make_end_body(-1, cx) if i == 0 else make_end_body(1, cx) if i == 4 else make_cell_body()
        cells.append(Pos(cx, 0, 0) * c)
    fx = IN_X - END_GAP
    face = rr(-fx, fx, IN_BOT, CHIN_TOP, 0.6, -FACE_T, -0.3) + rr_loft(-fx, fx, IN_BOT, CHIN_TOP, 0.6, -0.3, -0.3, 0.0)
    ribs = []
    for i in range(4):
        xb = (xs[i] + xs[i + 1]) / 2
        ribs.append(box(min(xb - 0.2, xs[i] + CELL_HW - 0.01), max(xb + 0.2, xs[i + 1] - CELL_HW + 0.01),
                        IN_BOT, CHIN_TOP, RAIL_TOP, -FACE_T + 0.01))
    body = face.fuse(*cells, *ribs)
    cut = []
    hw, hh = v1.OLED_WIN_W / 2, v1.OLED_WIN_H / 2
    wy = OLED_CY + v1.OLED_WIN_DY
    for cx in xs:                                           # the continuous face filled them in again
        cut.append(rr(cx - hw, cx + hw, wy - hh, wy + hh, 0.5, -FACE_T - 1.0, 1.0))
        cut.append(rr_loft(cx - hw, cx + hw, wy - hh, wy + hh, 0.5, -v1.OLED_WIN_CH, v1.OLED_WIN_CH + 0.3, 0.3))
        cut.append(box(cx - 5.3, cx + 5.3, OLED_CY + v1.OLED_STRIP_DY[1] + 0.05, OLED_TOP + 0.15,
                       -FACE_T - 0.5, -FACE_T + L(2)))
    body = body.cut(*cut)
    return body & rr(-IN_X + END_GAP, IN_X - END_GAP, IN_BOT, IN_TOP + 20.0, IN_R - END_GAP, -50.0, 50.0)


def make_chin_lid(xs=OLED_X, feet=OLED_X):
    """The five lids joined by a beam along their feet (case coordinates).  It slides up into all ten grooves at
    once and is held by the same five M3 as the separate lids.  xs = OLED centres, feet = screw positions."""
    lids = [Pos(cx, 0, 0) * make_cell_lid(f - cx) for cx, f in zip(xs, feet)]
    beam = box(min(feet) - FOOT_HW, max(feet) + FOOT_HW, IN_BOT, FOOT_TOP, CHIN_BEAM_Z[1], CHIN_BEAM_Z[0])
    return lids[0].fuse(*lids[1:], beam)


def onepiece_parts():
    return {"v2_06c_oled_chin_onepiece": make_chin_body(), "v2_07c_oled_lid_onepiece": make_chin_lid()}


# ------------------------------------------------------------------ one-piece chin, evenly spaced (variant)
# The five windows (27 x 15 on the front face) and the six gaps wall | window | ... | window | wall are all the
# same: 9.76 mm.  The pods, spine, walls and C3 parts stay as they are: the lid's five screw feet stay at the old
# screw positions (OLED_X) and only the bays move.  The middle OLED (and the C3 behind it) does not move.
WIN_FRONT_W = v1.OLED_WIN_W + 2 * v1.OLED_WIN_CH          # 27.0
EVEN_GAP = (2 * IN_X - 5 * WIN_FRONT_W) / 6               # 9.76
EVEN_PITCH = WIN_FRONT_W + EVEN_GAP                        # 36.76
OLED_X_EVEN = [EVEN_PITCH * i for i in (-2, -1, 0, 1, 2)]


def even_parts():
    return {"v2_06e_oled_chin_even": make_chin_body(OLED_X_EVEN),
            "v2_07e_oled_lid_even": make_chin_lid(OLED_X_EVEN, OLED_X)}


# ------------------------------------------------------------------ C3 cartridge
def make_c3_cart():
    hw, y0, y1 = C3_HW, C3_Y0, C3_Y1
    z0, z1 = C3_USB_Z, C3_FRONT_Z
    floor_top = y0 + C3_FLOOR
    cart = box(-hw, hw, y0, y1, z0, z1)
    cut = [box(-C3_CAV_W / 2, C3_CAV_W / 2, floor_top, y1 + 1.0, z0 + C3_END, z1 - C3_END),
           box(-C3_CAV_W / 2, C3_CAV_W / 2, floor_top + 1.0 + 0.6, y1 + 1.0, z1 - C3_END - 0.1, z1 + 1.0),
           box(-C3_CAV_W / 2 - C3_GROOVE, C3_CAV_W / 2 + C3_GROOVE, floor_top + C3_CAV_H,
               floor_top + C3_CAV_H + C3_LID_T + 0.2, z0 + C3_END, z1 + 1.0),
           box(-C3_CAV_W / 2, C3_CAV_W / 2, floor_top + C3_CAV_H, y1 + 1.0, z0 + C3_END, z1 + 1.0)]
    uh, u0, u1 = C3_USB_OPEN
    cut.append(box(-uh, uh, y0 + u0, y0 + u1, z0 - 1.0, z0 + C3_END + 0.5))
    ears = []
    for s in (-1, 1):                                       # ears with 45 deg gussets (prints USB end down)
        a = s * (hw - 0.01)
        g = s * (C3_EAR_X - (C3_EAR_Z - z0))
        ears.append(prism_xz([(a, z0), (g, z0), (s * C3_EAR_X, C3_EAR_Z), (s * C3_EAR_X, Z_BACK), (a, Z_BACK)],
                             y0, y1))
    cart = cart.fuse(*ears)
    for s in (-1, 1):
        x = s * C3_EAR_SCREW_X
        cut.append(cyl(x, C3_YM, CB_R, z0 - 1.0, C3_EAR_Z))
        cut.append(cyl(x, C3_YM, CLEAR_R, z0 - 1.0, Z_BACK + 1.0))
    return cart.cut(*cut)


def make_c3_lid():
    """Covers the cartridge from the USB end to the spine's front face; the last 6 mm stay open on top so
    the soldered wires leave upward behind the middle OLED (its lid is only 0.45 mm in front)."""
    w = C3_CAV_W / 2 + C3_GROOVE - 0.15
    y0 = C3_Y0 + C3_FLOOR + C3_CAV_H + 0.1
    return box(-w, w, y0, y0 + C3_LID_T, C3_USB_Z + C3_END + 0.2, Z_SPINE)


# ------------------------------------------------------------------ legs
_t = math.radians(TILT)


def c2w(yc, zc):
    return (yc * math.cos(_t) + zc * math.sin(_t), -yc * math.sin(_t) + zc * math.cos(_t))


def w2c(wy, wz):
    return (wy * math.cos(_t) - wz * math.sin(_t), wy * math.sin(_t) + wz * math.cos(_t))


DESK_WY = min(c2w(OUT_BOT, Z_BACK + CH)[0], c2w(OUT_BOT + CH, Z_BACK)[0])


def leg_points():
    ya = (DESK_WY - Z_BACK * math.sin(_t)) / math.cos(_t)
    A = (ya, Z_BACK)
    C = (LEG_TOP_Y, Z_BACK)
    wza = c2w(*A)[1]
    B = w2c(DESK_WY, wza - BASE_L)
    return A, B, C


def make_leg(s=1):
    A, B, C = leg_points()
    x0, x1 = s * LEG_X - LEG_T / 2, s * LEG_X + LEG_T / 2
    pl = Plane(origin=(x0, 0, 0), x_dir=(0, 1, 0), z_dir=(1, 0, 0))
    tri = pl * Polygon(A, B, C, align=None)
    try:
        tri = offset(offset(tri, -4.0, kind=Kind.ARC), 4.0, kind=Kind.ARC)
    except Exception:
        pass
    leg = extrude(tri, amount=LEG_T, dir=(1, 0, 0))
    P = [A, B, C]
    a, b, c = (math.dist(P[1], P[2]), math.dist(P[0], P[2]), math.dist(P[0], P[1]))
    ix = (a * P[0][0] + b * P[1][0] + c * P[2][0]) / (a + b + c)
    iz = (a * P[0][1] + b * P[1][1] + c * P[2][1]) / (a + b + c)
    area = abs((P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) - (P[2][0] - P[0][0]) * (P[1][1] - P[0][1])) / 2
    r_in = area / ((a + b + c) / 2)
    hole = Pos(x0 - 1.0, ix, iz) * Rot(0, 90, 0) * Cylinder(r_in - LEG_BAR, LEG_T + 2.0, align=CCM)
    xl = s * LEG_X
    tongue = prism_xz([(xl + dx, Z_BACK + dz) for dx, dz in TONGUE_PTS], A[0] + 0.6, LEG_TOP_Y - 2.3)
    leg = (leg - hole) + tongue
    # cable-tie slots straight down through the bottom bar (world vertical), at 35 % and 75 % of A..B
    wa, wb = c2w(*A), c2w(*B)
    ties = []
    for f in (0.35, 0.75):
        wz = wa[1] + f * (wb[1] - wa[1])
        w = box(xl - TIE[1] / 2, xl + TIE[1] / 2, DESK_WY - 2.0, DESK_WY + LEG_BAR + 4.0, wz - TIE[0] / 2, wz + TIE[0] / 2)
        ties.append(Rot(TILT, 0, 0) * w)
    return leg.cut(*ties)


# ------------------------------------------------------------------ assembly / export
def parts():
    left, right = make_walls()
    body, lid = make_cell_body(), make_cell_lid()
    p = {
        "v2_01_pod_left": make_pod(-1),
        "v2_02_pod_right": make_pod(1),
        "v2_03_spine": make_spine(),
        "v2_04_wall_left": left,
        "v2_05_wall_right": right,
        "v2_06_oled_body": body,
        "v2_06_oled_body_end_L": make_end_body(-1),
        "v2_06_oled_body_end_R": make_end_body(1),
        "v2_07_oled_lid": lid,
        "v2_08_c3_cart": make_c3_cart(),
        "v2_09_c3_lid": make_c3_lid(),
        "v2_10_leg": make_leg(1),
    }
    return p


def assembly(p):
    """Every part in case coordinates (5 cells, 2 legs)."""
    a = {k: v for k, v in p.items() if not k.startswith(("v2_06_oled_body", "v2_07_oled_lid", "v2_10_leg"))}
    for i, x in enumerate(OLED_X):
        key = "v2_06_oled_body_end_L" if i == 0 else "v2_06_oled_body_end_R" if i == 4 else "v2_06_oled_body"
        a[f"v2_06_oled_body_{i + 1}"] = Pos(x, 0, 0) * p[key]
        a[f"v2_07_oled_lid_{i + 1}"] = Pos(x, 0, 0) * p["v2_07_oled_lid"]
    a["v2_10_leg_R"] = p["v2_10_leg"]
    a["v2_10_leg_L"] = Pos(-2 * LEG_X, 0, 0) * p["v2_10_leg"]
    return a


def to_bed(name, s):
    """Print orientation, bed at z = 0."""
    if name in ("v2_01_pod_left", "v2_02_pod_right", "v2_03_spine"):
        o = s                                                    # back face down
    elif name in ("v2_04_wall_left", "v2_05_wall_right"):
        o = s                                                    # back edge down
    elif name.startswith(("v2_06_oled_body", "v2_06c", "v2_06e")):
        o = Rot(0, 180, 0) * s                                   # face down
    elif name in ("v2_07_oled_lid", "v2_07c_oled_lid_onepiece", "v2_07e_oled_lid_even"):
        o = Rot(90, 0, 0) * s                                    # standing on its bottom edge
    elif name == "v2_08_c3_cart":
        o = Rot(90, 0, 0) * s                                    # lying on its floor, cavity up (user 2026-11-20;
        # was USB end down).  8.6 tall = L(24); the slicer rounds floor top 1.0 -> 0.9, lid groove 6.6..7.8 ->
        # 6.5..7.9 and USB window bottom 2.1 -> 1.95 (all a little roomier).  Only overhangs: the 0.8 lip over the
        # lid groove and the 9.0 bridge over the USB window; the M3 holes run horizontally.
    elif name == "v2_09_c3_lid":
        o = Rot(90, 0, 0) * s                                    # flat
    elif name == "v2_10_leg":
        o = Rot(0, 90, 0) * s                                    # flat
    else:
        o = s
    bb = o.bounding_box()
    return Pos(-(bb.min.X + bb.max.X) / 2, -(bb.min.Y + bb.max.Y) / 2, -bb.min.Z) * o


def main():
    (OUT / "stl").mkdir(parents=True, exist_ok=True)
    p = parts()
    info = {}
    for n, s in list(p.items()) + list(onepiece_parts().items()) + list(even_parts().items()):
        b = to_bed(n, s)
        export_stl(b, str(OUT / "stl" / f"{n}.stl"), tolerance=0.01, angular_tolerance=0.1)
        bb = b.bounding_box()
        info[n] = dict(size=[round(bb.size.X, 1), round(bb.size.Y, 1), round(bb.size.Z, 2)],
                       solids=len(b.solids()), volume_cm3=round(b.volume / 1000, 1))
    a = assembly(p)
    export_step(Compound(list(a.values())), str(OUT / "v2_assembly.step"))
    A, B, C = leg_points()
    info["legs"] = dict(A=[round(v, 2) for v in A], B=[round(v, 2) for v in B], C=[round(v, 2) for v in C],
                        desk_wy=round(DESK_WY, 2))
    (OUT / "build_info.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()

