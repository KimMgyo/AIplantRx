#!/usr/bin/env python3
"""
Sensor node case v1 (build123d 0.13)

Hangs under the PC case's top panel in a 140 mm fan / radiator slot and looks straight down at the plant.
A vented lid closes the top.  Four M3 bolts at the 140 mm fan-hole pattern go down through the lid
(counterbored: heads end flush, so the lid lies flat under the case panel) into M3 nuts slid into slots in
the corner pillars.  M3 x 8 holds the lid alone; M3 x 12 goes through the case's top panel as well.  Without
bolts the lid still holds: a V-bump on a short skirt in the middle of each side clicks into a V-groove in
the wall (the tall walls are the spring).

Print frame = model frame: Z = 0 is the sensor face (it faces the plant once installed; it is the print
bed side, so the visible face gets the bed finish).  Z grows toward the case top panel.  X, Y are the plan.
Installed, gravity pulls every board onto its seat (the seats are on the sensor-face side), so nothing inside
needs a screw: pins locate the MLX90640 and the SCD41 (melt the tips or a dot of hot glue for handling), the
other boards sit in cradles.  The ring rests on the diffuser skin the same way.

Parts
  body      the box: optics in the middle, boards on seats, openings in the walls.  The diffuser is part of
            it: under the ring pocket the face is only DIFF_T thick, so the whole sensor face is one flat
            plane on the bed.  The G-code changes filament (M600) after that skin: white first, then any colour.
  lid       vented plate, corner pads on the lowered pillars, 4 snap skirts; printed top face down

Hardware (cases/sensor-node/research/parts_research.md)
  NeoPixel Ring 24 (Adafruit 1586)   OD 65.5, ID 52.3, 3.2 thick, LEDs face the plant, on the diffuser skin
  MLX90640 55 deg (Adafruit 4407)    25.40 x 17.78, d2.5 holes 20.32 x 12.70 (2 pins), can d9.3 x 11.25 above the PCB;
                                     can tip 0.15 inside the sensor face (IR needs an open hole, no window)
  ESP32-CAM (AI-Thinker)             PCB 27.0 x 40.0, camera side down; the camera holder rests on a pad so
                                     the lens position does not depend on the board stack
  SCD41 (Adafruit 5190)              25.40 x 22.86, d3.0 holes 20.32 x 17.78 (2 pins), sensor faces a floor grille
                                     inside a baffle so it breathes outside air, not the box's warm air
  ESP32 38-pin devkit (wide, USB-C)  28.2 x 54.35 (user); USB-C (power + upload) through the -Y wall; pins
                                     down with dupont housings (14 mm under the PCB)
  MAX485 module                      44 x 14.2, no holes; pins down with duponts, along the +X wall
  3 x LR7843 MOSFET module (HW-532)  16 x 33, inside along the +Y wall: water / nutrient / pH pumps (12 V);
                                     blue load terminals face the middle of the box for wiring
  KF301 / DG301 5.0 terminals        PCB screw terminals (not panel parts) held in pockets behind the -Y
                                     wall, wire mouths through windows: 12V in (2P), pump 1..3 (2P each),
                                     RS485 sensor (4P: V G A B).  Pins point down; wires are soldered to them.

Every outside connection is on the -Y wall: USB-C, 12V, 3 pumps, RS485.

Spread between sources (absorbed by a lens slot and foam tape): CAM_LENS_FROM_END 10.6..11.5, CAM_STACK.
Power, two domains: the devkit runs from its own USB-C only (its 3V3 feeds SCD41, MLX90640, MAX485); 12V in
feeds two 5 V bucks (ring alone at full white; ESP32-CAM), the SEN0604 and the LR7843 / pumps.  The devkit 5V
pin is left unconnected so the two 5 V sources never meet; one wire joins devkit GND and 12V -.
Print rules (PrusaSlicer 2.5.16 vendor ini, MK2S 0.4, 0.35mm FAST): walls 1.12 (2 lines) or >= 1.83
(4 lines), never in between; vertical holes +0.3; bridges <= 15 mm; elephant-foot compensation 0.2 is on, so
bed-side recesses get 0.3 clearance; heights that matter on L(n) = 0.20 + 0.35 n.
"""
from __future__ import annotations

import json
import math
from pathlib import Path

from build123d import (Align, Box, Compound, Cylinder, Pos, RectangleRounded, RegularPolygon, Rot, export_step,
                       export_stl, extrude)

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
CCM = (Align.CENTER, Align.CENTER, Align.MIN)


def L(n: int) -> float:
    return round(0.20 + 0.35 * n, 3)


def box(x0, x1, y0, y1, z0, z1):
    x0, x1 = sorted((x0, x1))
    y0, y1 = sorted((y0, y1))
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, z0) * Box(x1 - x0, y1 - y0, z1 - z0, align=CCM)


def cyl(x, y, r, z0, z1):
    return Pos(x, y, z0) * Cylinder(r, z1 - z0, align=CCM)


def rounded_hole_y(cx, cz, w, h, r, y0, y1):
    """Rounded-rectangle opening through a Y wall: w (X) x h (Z), corner radius r, from y0 to y1."""
    s = box(cx - w / 2, cx + w / 2, y0, y1, cz - h / 2 + r, cz + h / 2 - r) \
        + box(cx - w / 2 + r, cx + w / 2 - r, y0, y1, cz - h / 2, cz + h / 2)
    for sx in (-1, 1):
        for sz in (-1, 1):
            s += Pos(cx + sx * (w / 2 - r), (y0 + y1) / 2, cz + sz * (h / 2 - r)) * Rot(90, 0, 0) * Cylinder(r, y1 - y0)
    return s


def ring_solid(x, y, r_out, r_in, z0, z1):
    return cyl(x, y, r_out, z0, z1) - cyl(x, y, r_in, z0 - 1, z1 + 1)


def polar(r, deg, c=(0.0, 0.0)):
    a = math.radians(deg)
    return c[0] + r * math.cos(a), c[1] + r * math.sin(a)


# ---------------------------------------------------------------- outline
SIZE = 140.0                      # one 140 mm fan slot
R_CORNER = 5.0
WALL = 2.4
H = 38.0                          # L(108): sensor face to top rim
FLOOR = L(7)                      # 2.65
IN = SIZE / 2 - WALL              # 67.6 inner half size
FAN_HOLE = 124.75 / 2             # 124.5 (Noctua) .. 125.0 (Arctic) -> middle: the M3 bolts sit on it
PILLAR = 13.0                     # corner pillar size
LINE2, LINE4 = 1.12, 1.83         # 0.35mm FAST (ext 0.60 + int 0.43): 2 / 4 perimeters, avoid in between
HOLE_PLUS = 0.3                   # vertical holes print ~0.3 small
CLR = 0.3                         # bed-side recess clearance (elephant foot)

# ---------------------------------------------------------------- lid, M3 corners, snaps
LID_T = L(5)                      # 1.95 lid plate
LID_PAD = 2.8                     # corner pads under the plate; the pillars stop at H - 2.8 = L(100)
M3_HOLE = 3.2 + HOLE_PLUS         # clearance, prints ~3.2
M3_CB = (6.5 + HOLE_PLUS + CLR, L(9))   # counterbore d7.1 x 3.35 from the lid top: socket / pan / button heads flush
NUT_AF = 5.5 + 0.3                # M3 nut 5.5 AF x 2.4
NUT_Z = (L(84), L(92))            # 29.60 .. 32.40 nut slot; 2.8 of pillar above the nut
NUT_SLOT = 10.0                   # slot from the hole along X toward the box (the nut slides in from inside)
SNAP_Z = H - 2.2                  # 35.8: groove / bump centre
SNAP_LEN = 24.0                   # skirt length in the middle of each side
SNAP_GAP = 0.2                    # skirt to wall
SNAP_BUMP = 0.55                  # bump past the skirt: 0.35 interference, sits in a 0.4 groove
SNAP_GROOVE = 0.4
SNAP_SKIRT = 4.0                  # skirt depth below the rim
# Mouse ears: the solid 140 x 140 face plus 38 mm walls shrink and lift the body's corners off the bed.
# A disc on each corner vertex (2 layers, fused to the corner, cut off after) holds them down; a full brim
# would leave a trimmed edge all around the sensor face.
EAR_R = 15.0
EAR_T = L(1)                      # 0.55: first layer + 1

# ---------------------------------------------------------------- ring + diffuser
C = (0.0, 0.0)                    # ring centre = box centre = fan slot centre
RING_OD, RING_ID, RING_T = 65.5, 52.3, 3.2
RING_PAD_R = 31.6
RING_ROT = 90.0                   # ring LED1 points +Y (deg from +X, seen from the plant)
RING_PADS = {"DIN": 7.5, "GND": 157.5, "5V": 187.5}   # clockwise from LED1, seen from the LED side
DIFF_T = L(2)                     # 0.90 diffuser skin under the ring: the first 3 layers (all solid), printed
                                  # in white; M600 after it switches to the body colour
PIN_TIP = 1.0                     # board pins: past the 1.6 PCB, melt or hot-glue
RING_SUPPORT_Z = L(17)            # 6.15: top of the pocket walls
RING_POCKET = (RING_ID / 2 - CLR, RING_OD / 2 + CLR)  # (25.85, 33.05)
# The pocket is open toward the inside (the ring drops in from there onto the skin, LEDs down): no ribs, no
# bridges.  Installed, gravity holds it; 3 dabs of hot glue (or tape) on the wall tops keep it for handling.

# ---------------------------------------------------------------- optics
OPT_DX = 12.0                     # lens / can offset from the ring centre: 24 mm between the two
MLX_C = (C[0] + OPT_DX, C[1])
CAM_C = (C[0] - OPT_DX, C[1])
MLX_CAN_D, MLX_CAN_H = 9.3, 11.25
MLX_PCB_Z = L(32)                 # 11.40: can tip 0.15 inside the sensor face
MLX_BOARD = (17.78, 25.40)        # (x, y): long axis along Y, clear of the CAM board
MLX_HOLES = (12.70 / 2, 20.32 / 2)
CAM_BOARD = (27.0, 40.0)          # PCB (DFRobot DFR0602 drawing); SD/camera end toward -Y
CAM_PCB_T = 1.0
CAM_LENS_FROM_END = 10.8          # lens centre from the SD end: sources 10.6 .. 11.5 (hand-taped camera)
CAM_LENS_SLACK = 1.5              # lens hole is a slot +-1.5 along the board for that spread
CAM_HOLDER_Z = L(9)               # 3.35: the camera holder's face rests on this pad (registers the lens)
CAM_STACK = 5.35                  # PCB face -> holder face: SD 2.2 + tape 0.5 + FPC..holder top 2.65
CAM_SEAT_Z = L(24)                # 8.60 = 3.35 + 5.35 - 0.1 preload; foam tape takes up the spread
CAM_LENS_D = 7.6                  # lens barrel d7.2 (lower step) / d5.4 (tip), 2.9 beyond the holder
CAM_BODY = 9.2                    # square collar for the 8.5 +-0.2 holder

# ---------------------------------------------------------------- electronics
DUPONT = 14.0                     # header spacer 2.5 + dupont housing: room under / over a PCB
DEV = (28.2, 54.35)               # devkit (x, y): PCB 48.26 + module antenna 6.04 beyond it
DEV_PCB_L = 48.26
DEV_X0, DEV_Y0 = -54.4, -IN + 1.6   # HiGenis board: USB-C flush with the PCB edge, so the receptacle sits
                                    # 1.6 inside the wall and a plug overmold reaches 1.1 past it (clear)
DEV_SEAT_Z = L(58)                # 20.50: duponts end 6.5 up, clear of the 6.15 ring support (0.35)
DEV_USB_H = 3.2                   # USB-C body 3.16 .. 3.26 above the PCB
SCD_C = (-39.0, 52.0)              # -X/+Y: 12 from the ring edge (grille 21), ~85 from the bucks
SCD_BOARD = (25.40, 22.86)
SCD_HOLES = (20.32 / 2, 17.78 / 2)
SCD_PCB_Z = L(32)                 # 11.40: sensor cap (7.8) 0.95 above the floor
# Sealed chamber: the baffle walls rise 2.25 past the PCB top and a separate cap closes them, so the sensor
# only breathes outside air through the floor grille, never the box's warm air.  Wires leave through a
# 12 x 3 notch in the wall on the header side (-Y); a dab of hot glue / foam closes what is left of it.
SCD_WALL_Z = L(43)                # 15.25 (PCB top 13.00: room for soldered wires and the pin heads)
SCD_CAP_T = L(3)                  # 1.25
SCD_NOTCH = (12.0, 3.0)
RS = (14.2, 44.0)                 # MAX485 (x, y) along the +X wall, terminal block toward +Y
RS_X0, RS_Y0 = IN - 0.6 - RS[0], -20.0
RS_SEAT_Z = L(48)                 # 17.00: duponts end 3.0 up (floor 2.65)
TIE = (3.6, 1.8)                  # zip-tie tunnel (2.5 x 1 ties)
# 2 x fixed 5 V buck (user's AliExpress 1005008257960729, CN3903-type, 17.5 x 12 x 4.3, no mounting holes:
# the 4 corner holes are IN+ IN- OUT+ OUT- pads).  Rated 1.6 .. 1.8 A continuous: one feeds only the
# ring (1.44 A full white), one the ESP32-CAM (~0.3 A).  They run hot
# (70 .. 110 C est.): held 4.2 above the floor on tiny pads under the middle of the long edges only, lips
# at the edge middles (the corners carry the soldered wires), in the +X bay far from the SCD41; the open
# top lets the heat out.
BUCK = (12.3, 17.8)               # (x, y) footprint with clearance; long axis along Y
BUCK_SEAT = L(19)                 # 6.85
BUCK_C = {"ring": (42.5, 11.0), "cam": (42.5, -13.0)}
# Perfboard piece (cut to 25 x 35, corners left empty) for the CAM relay parts: 2 x PC817C (IO0, EN) and
# 4 x 1k (2 in the UART lines, 2 LED resistors).  -X bay between the devkit's antenna end and the SCD41
# chamber, next to the devkit pins and the CAM header.
PERF = (25.0, 35.0)
PERF_C = (-49.5, 11.5)
PERF_SEAT = L(20)                 # 7.20: 4.55 under the board for solder tails and wires
# 3 x LR7843 MOSFET module (HW-532): water / nutrient / pH pumps.  PCB 16 x 33, the blue 3-way 5.0 terminal
# (+, LOAD, -) overhangs the +Y end by ~2 and its wire mouths face the +Y wall, straight through a wall slot;
# screws from above (open top).  The signal end (PWM, GND, two d2 holes 12 apart) faces the ring.
LR = (16.0, 33.0)
LR_TERM_OVER = 1.8                # blue terminal past the PCB end (1.3 .. 1.8)
LR_X = (-1.0, 26.0, 46.0)         # module centres (water, nutrient, pH): the space in front of each terminal stays free
LR_Y1 = IN - 0.2                  # signal end (PWM, GND, holes) against the +Y wall
LR_Y0 = LR_Y1 - LR[1]             # 34.4: load-terminal end, its wire mouths face -Y (the middle of the box)
LR_SEAT_Z = L(20)                 # 7.20: 4.55 under the PCB for the 12V wires soldered under + and -
LR_HOLE = (6.0, 2.0)              # holes d2, +-6 (12 apart, 12.1..12.3 measured), 2.0 from the signal end

# KF301 / DG301 5.0 PCB screw terminals behind the -Y wall (Degson / KEFA drawings): body 5n (+0.6 dovetail)
# x 7.6 deep x 10 high, pins d1.0 down, wire mouth centre 3.0 above the pin face, screws on top.
KF_P, KF_D, KF_H, KF_DOVE = 5.0, 7.6, 10.0, 0.6
KF_SEAT = L(26)                   # 9.30: pin face; 6.65 under it for the pins (3.6 .. 4.3) and soldered wires
KF_MOUTH = 3.0
KF_ROW = (("12V", 2, "+ -"), ("P1", 2, "+ -"), ("P2", 2, "+ -"), ("P3", 2, "+ -"), ("485", 4, "V G A B"))
KF_X0 = -23.5                     # left wall of the row (right of the devkit's USB-C)
KF_SCREW_Y = -IN + 3.8            # screw centre: about mid-depth (Degson plan view), 0.6 x 3.5 flat blade, M2.5
KF_LID_SLOT = 5.3                 # lid slot per terminal block (prints ~5.0): one hole per screw would merge
                                  # (5.0 pitch); its wall edge stays 0.12 clear of the -Y snap skirt
USB_CUT = (13.5, 8.0)             # through the 2.4 wall: the plug overmould (<= 12.35 x 6.5, real 13 x 7.4)
                                  # must pass the wall to seat fully
USB_R = 2.0                       # overmould-shaped corners: a square-cornered 12.35 x 6.5 still passes (1.9 < 2.0),
                                  # so does a 13 x 7.4 with R2 corners; the top is a 9.5 bridge plus two R2 arcs


def cam_box():
    y0 = CAM_C[1] - CAM_LENS_FROM_END
    return (CAM_C[0] - CAM_BOARD[0] / 2, CAM_C[0] + CAM_BOARD[0] / 2, y0, y0 + CAM_BOARD[1])


def cradle(x0, x1, y0, y1, z_seat, lip=L(8) - 0.2, step=1.25, clr=0.25, skip=()):
    """Corner posts with 2.8 (8 layer) lips, 1.12 (2 line) thick.  step > 0: the board rests on a step that
    reaches `step` under each corner; step = 0: the posts stay outside the board (pins under the corners),
    the board then needs its own pads.  skip: corners (0..3 = x0y0, x1y0, x0y1, x1y1) left out."""
    out = []
    for k, (xs, ys) in enumerate(((x0, y0), (x1, y0), (x0, y1), (x1, y1))):
        if k in skip:
            continue
        sx = -1 if xs == x0 else 1
        sy = -1 if ys == y0 else 1
        inner = -sx * step if step else sx * clr
        px = (xs + sx * 3.0, xs + inner)
        py = (ys + sy * 3.0, ys + (-sy * step if step else sy * clr))
        top = z_seat if step else z_seat + lip
        out.append(box(*px, *py, FLOOR - 0.01, top))
        lx = (xs + sx * clr, xs + sx * (clr + LINE2))
        ly = (ys + sy * clr, ys + sy * (clr + LINE2))
        out.append(box(*lx, *py, z_seat - 0.01, z_seat + lip))
        out.append(box(*px, *ly, z_seat - 0.01, z_seat + lip))
    return out


def standoffs(cx, cy, hx, hy, z_top, d_out):
    return [cyl(cx + sx * hx, cy + sy * hy, d_out / 2, FLOOR - 0.01, z_top) for sx in (-1, 1) for sy in (-1, 1)]


def locate_pins(cx, cy, hx, hy, z_top, d_pin):
    """Locating pins on the diagonal pair of standoffs: through the 1.6 PCB and PIN_TIP past it."""
    return [cyl(cx + sx * hx, cy + sy * hy, d_pin / 2, z_top - 0.01, z_top + 1.6 + PIN_TIP)
            for sx, sy in ((-1, -1), (1, 1))]


def kf_row():
    """(name, poles, pin labels, x0, x1) of each KF301 pocket along the -Y wall, left to right."""
    out, x = [], KF_X0 + LINE4
    for name, n, pins in KF_ROW:
        w = n * KF_P + KF_DOVE + 0.3
        out.append((name, n, pins, x, x + w))
        x += w + LINE4
    return out


def engrave(labels, size=3.4, depth=0.6):
    """Text cut 0.6 into the outside of the -Y wall, reading left to right from outside."""
    from build123d import Text
    cuts = []
    for txt, x, z in labels:
        t = extrude(Text(txt, font_size=size, align=(Align.CENTER, Align.CENTER)), amount=depth + 0.4)
        cuts.append(Pos(x, -SIZE / 2 + depth, z) * Rot(90, 0, 0) * t)
    return Compound(cuts)


def scd_inner():
    """Inner faces (x0, x1, y0, y1) of the SCD41 chamber walls: the board outline + 1.0."""
    return (SCD_C[0] - SCD_BOARD[0] / 2 - 1.0, SCD_C[0] + SCD_BOARD[0] / 2 + 1.0,
            SCD_C[1] - SCD_BOARD[1] / 2 - 1.0, SCD_C[1] + SCD_BOARD[1] / 2 + 1.0)


def make_scd_cap():
    """Lid of the SCD41 chamber, in place (installed frame): a plate on the wall tops with a 1.4 skirt that
    locates inside the walls (left open in front of the wire notch).  Printed upside down (plate on the bed)."""
    x0, x1, y0, y1 = scd_inner()
    plate = box(x0 - LINE4, x1 + LINE4, y0 - LINE4, y1 + LINE4, SCD_WALL_Z, SCD_WALL_Z + SCD_CAP_T)
    c = 0.25
    skirt = (box(x0 + c, x1 - c, y0 + c, y1 - c, SCD_WALL_Z - 1.4, SCD_WALL_Z + 0.01)
             - box(x0 + c + LINE2, x1 - c - LINE2, y0 + c + LINE2, y1 - c - LINE2, SCD_WALL_Z - 1.5, SCD_WALL_Z + 0.1)
             - box(SCD_C[0] - SCD_NOTCH[0] / 2, SCD_C[0] + SCD_NOTCH[0] / 2, y0 - 0.1, y0 + c + LINE2 + 0.1,
                   SCD_WALL_Z - 1.5, SCD_WALL_Z + 0.1))
    return plate + skirt


def to_bed(name, s):
    """Print orientation: the SCD cap and the lid go plate-down; everything else is printed as modelled.
    The body gets mouse ears under its 4 corners (print STL only, not the model)."""
    if name == "body":
        for sx in (-1, 1):
            for sy in (-1, 1):
                s = s + cyl(sx * SIZE / 2, sy * SIZE / 2, EAR_R, 0.0, EAR_T)
        return s
    if name not in ("scd_cap", "lid"):
        return s
    f = Rot(180, 0, 0) * s
    return Pos(0, 0, -f.bounding_box().min.Z) * f


def lr_boxes():
    """PCB footprints (x0, x1, y0, y1) of the three LR7843 modules (terminal overhang not included)."""
    return [(cx - LR[0] / 2, cx + LR[0] / 2, LR_Y0, LR_Y1) for cx in LR_X]


def tie_anchor(x, y, along_x):
    """Floor block with a zip-tie tunnel across it: the cable lies over it along `along_x` (or Y)."""
    w, d, h = 10.0, 6.0, L(17)
    blk = box(x - w / 2, x + w / 2, y - d / 2, y + d / 2, FLOOR - 0.01, h) if along_x else \
        box(x - d / 2, x + d / 2, y - w / 2, y + w / 2, FLOOR - 0.01, h)
    tz = FLOOR + 1.6
    tun = box(x - TIE[0] / 2, x + TIE[0] / 2, y - d, y + d, tz, tz + TIE[1]) if along_x else \
        box(x - d, x + d, y - TIE[0] / 2, y + TIE[0] / 2, tz, tz + TIE[1])
    return blk, tun


def nut_slot():
    """M3 nut pocket at the origin with its slot along +X (flats parallel to the slot)."""
    h = NUT_Z[1] - NUT_Z[0]
    return extrude(RegularPolygon(NUT_AF / math.sqrt(3), 6), amount=h) + box(0, NUT_SLOT, -NUT_AF / 2, NUT_AF / 2, 0, h)


def snap_bar(y, half_diag, length):
    """Diamond bar along X (45 deg flanks, prints both ways) centred at (0, y, SNAP_Z)."""
    a = half_diag * math.sqrt(2)
    return Pos(0, y, SNAP_Z) * Rot(45, 0, 0) * Box(length, a, a)


def make_lid():
    """Lid in place (installed frame): plate on the rim, corner pads on the lowered pillars, M3 counterbores on
    the fan-hole pattern, a snap skirt in the middle of each side, vent slots, screwdriver slots over the
    terminal screws.  Printed top face down."""
    lid = Pos(0, 0, H) * extrude(RectangleRounded(SIZE, SIZE, R_CORNER), amount=LID_T)
    c = 0.25
    cav = Pos(0, 0, H - LID_PAD) * extrude(RectangleRounded(2 * (IN - c), 2 * (IN - c), R_CORNER - WALL - c),
                                           amount=LID_PAD + 0.02)
    for sx in (-1, 1):
        for sy in (-1, 1):
            lid = lid + (box(sx * (IN - PILLAR + c), sx * IN, sy * (IN - PILLAR + c), sy * IN, H - LID_PAD, H + 0.01)
                         & cav)
    for k in range(4):
        y1 = IN - SNAP_GAP
        skirt = box(-SNAP_LEN / 2, SNAP_LEN / 2, y1 - LINE4, y1, H - SNAP_SKIRT, H + 0.01)
        lid = lid + Rot(0, 0, 90 * k) * (skirt + snap_bar(y1, SNAP_BUMP, SNAP_LEN - 4.0))
    for sx in (-1, 1):
        for sy in (-1, 1):
            hx, hy = sx * FAN_HOLE, sy * FAN_HOLE
            lid = lid - cyl(hx, hy, M3_HOLE / 2, H - LID_PAD - 0.1, H + LID_T + 0.1)
            lid = lid - cyl(hx, hy, M3_CB[0] / 2, H + LID_T - M3_CB[1], H + LID_T + 0.1)
    for i in range(-8, 9):
        lid = lid - box(-44.0, 44.0, i * 6.0 - 1.5, i * 6.0 + 1.5, H - 0.1, H + LID_T + 0.1)
    # screwdriver slots over the KF301 screws (tighten wires with the lid on): one rounded slot per block
    y0 = -(IN - SNAP_GAP - LINE4) + 0.12
    yc, r = y0 + KF_LID_SLOT / 2, KF_LID_SLOT / 2
    for _, n, _, x0, x1 in kf_row():
        xa = (x0 + x1) / 2 - (n - 1) * KF_P / 2
        xb = xa + (n - 1) * KF_P
        lid = lid - (box(xa, xb, y0, y0 + KF_LID_SLOT, H - 0.1, H + LID_T + 0.1)
                     + cyl(xa, yc, r, H - 0.1, H + LID_T + 0.1) + cyl(xb, yc, r, H - 0.1, H + LID_T + 0.1))
    return lid


def make_body():
    outer = extrude(RectangleRounded(SIZE, SIZE, R_CORNER), amount=H)
    inner = Pos(0, 0, FLOOR) * extrude(RectangleRounded(2 * IN, 2 * IN, R_CORNER - WALL), amount=H)
    body = outer - inner

    add, cut, labels = [], [], []
    # corner pillars (top lowered for the lid's pads): M3 hole, nut slot opening toward the box centre
    for sx in (-1, 1):
        for sy in (-1, 1):
            add.append(box(sx * IN, sx * (IN - PILLAR), sy * IN, sy * (IN - PILLAR), 0.0, H - LID_PAD))
            hx, hy = sx * FAN_HOLE, sy * FAN_HOLE
            cut.append(cyl(hx, hy, M3_HOLE / 2, NUT_Z[0] - 4.0, H + 0.1))
            cut.append(Pos(hx, hy, NUT_Z[0]) * Rot(0, 0, 180.0 if sx > 0 else 0.0) * nut_slot())
    # snap grooves: V in the inner face of each wall, in the middle
    for k in range(4):
        cut.append(Rot(0, 0, 90 * k) * snap_bar(IN, SNAP_GROOVE, SNAP_LEN + 2.0))

    # ring: pocket walls, pocket open toward the inside down to the diffuser skin (the face stays flat)
    p0, p1 = RING_POCKET
    add.append(ring_solid(*C, p1 + LINE4, p1, FLOOR - 0.01, RING_SUPPORT_Z))
    add.append(ring_solid(*C, p0, p0 - LINE4 - 1.2, FLOOR - 0.01, RING_SUPPORT_Z))
    cut.append(ring_solid(*C, p1, p0, DIFF_T, RING_SUPPORT_Z + 0.1))

    # MLX90640: open hole for the can, 4 standoffs, pins on the diagonal pair
    cut.append(cyl(*MLX_C, (MLX_CAN_D + 0.5 + HOLE_PLUS) / 2, -0.1, FLOOR + 0.2))
    add += standoffs(*MLX_C, *MLX_HOLES, MLX_PCB_Z, 4.6)
    add += locate_pins(*MLX_C, *MLX_HOLES, MLX_PCB_Z, 2.1)

    # ESP32-CAM: lens slot (+-CAM_LENS_SLACK along the board), pad + collar for the camera holder, cradle.
    # The holder face sits on the pad at CAM_HOLDER_Z, so the lens tip ends 0.45 inside the sensor face.
    sl = CAM_LENS_SLACK
    r = (CAM_LENS_D + HOLE_PLUS) / 2
    cut.append(cyl(CAM_C[0], CAM_C[1] - sl, r, -0.1, CAM_HOLDER_Z + 0.1)
               + cyl(CAM_C[0], CAM_C[1] + sl, r, -0.1, CAM_HOLDER_Z + 0.1)
               + box(CAM_C[0] - r, CAM_C[0] + r, CAM_C[1] - sl, CAM_C[1] + sl, -0.1, CAM_HOLDER_Z + 0.1))
    hb = CAM_BODY / 2
    add.append(box(CAM_C[0] - hb - LINE4, CAM_C[0] + hb + LINE4, CAM_C[1] - hb - sl - LINE4,
                   CAM_C[1] + hb + sl + LINE4, FLOOR - 0.01, CAM_HOLDER_Z + L(5)))
    cut.append(box(CAM_C[0] - hb, CAM_C[0] + hb, CAM_C[1] - hb - sl, CAM_C[1] + hb + sl,
                   CAM_HOLDER_Z, CAM_HOLDER_Z + L(5) + 0.1))
    # camera side: corners are free of parts.  Two corners would stand over the open ring pocket (nothing
    # under them to print on); the diagonal pair left locates the board in X and Y, and with the camera
    # holder on its pad the board rests on three points.
    add += cradle(*cam_box(), CAM_SEAT_Z, skip=(0, 3))

    # SCD41: grille, sealed chamber (walls past the PCB, closed by the cap part), wire notch, standoffs + 2 pins
    bx0, bx1, by0, by1 = scd_inner()
    add.append(box(bx0 - LINE4, bx1 + LINE4, by0 - LINE4, by1 + LINE4, FLOOR - 0.01, SCD_WALL_Z)
               - box(bx0, bx1, by0, by1, FLOOR - 0.1, SCD_WALL_Z + 0.1))
    cut.append(box(SCD_C[0] - SCD_NOTCH[0] / 2, SCD_C[0] + SCD_NOTCH[0] / 2, by0 - LINE4 - 0.1, by0 + 0.1,
                   SCD_WALL_Z - SCD_NOTCH[1], SCD_WALL_Z + 0.1))
    for i in range(-3, 4):
        cut.append(box(SCD_C[0] + i * 3.0 - 0.8, SCD_C[0] + i * 3.0 + 0.8,
                       SCD_C[1] - 7.0, SCD_C[1] + 7.0, -0.1, FLOOR + 0.1))
    add += standoffs(*SCD_C, *SCD_HOLES, SCD_PCB_Z, 5.0)
    add += locate_pins(*SCD_C, *SCD_HOLES, SCD_PCB_Z, 2.6)

    # devkit: posts outside the board (pin rows run along both long edges), a pad under the USB end
    # between the rows and a pad under the module's antenna overhang (module bottom = PCB top)
    dx0, dx1, dy0, dy1 = DEV_X0, DEV_X0 + DEV[0], DEV_Y0, DEV_Y0 + DEV[1]
    dcx = (dx0 + dx1) / 2
    # The antenna-end corner toward the CAM board (1.65 away) gets a side guide instead of a corner post;
    # the guide and the antenna pad stay outside the open ring pocket (r > 34.9) so they print from the floor.
    add += cradle(dx0, dx1, dy0, dy1, DEV_SEAT_Z, step=0, skip=(3,))
    add.append(box(dx1 + 0.25, dx1 + 3.0, dy0 + DEV_PCB_L - 24.0, dy0 + DEV_PCB_L - 18.0, FLOOR - 0.01,
                   DEV_SEAT_Z + L(8) - 0.2))
    add.append(box(dcx - 6.0, dcx + 6.0, dy0 + 7.0, dy0 + 12.0, FLOOR - 0.01, DEV_SEAT_Z))   # USB legs: keep off
    add.append(box(dx0 + 0.3, dx0 + 10.0, dy0 + DEV_PCB_L + 0.5, dy1 - 0.5, FLOOR - 0.01, DEV_SEAT_Z + 1.6))
    # USB-C opening in the -Y wall, centred on the receptacle (PCB top + 1.58)
    usb_z = DEV_SEAT_Z + 1.6 + DEV_USB_H / 2
    cut.append(rounded_hole_y(dcx, usb_z, *USB_CUT, USB_R, -SIZE / 2 - 0.1, -IN + 0.1))
    labels.append(("USB", dcx, usb_z + USB_CUT[1] / 2 + 3.2))

    # MAX485: posts outside, side ledges under the middle of both long edges (headers at both short ends)
    rx0, rx1, ry0, ry1 = RS_X0, RS_X0 + RS[0], RS_Y0, RS_Y0 + RS[1]
    add += cradle(rx0, rx1, ry0, ry1, RS_SEAT_Z, step=0)
    for xa, xb in ((rx0 - 2.0, rx0 + 1.0), (rx1 - 1.0, rx1 + 2.0)):
        add.append(box(xa, xb, ry0 + 10.0, ry1 - 10.0, FLOOR - 0.01, RS_SEAT_Z))
    # KF301 terminal row behind the -Y wall: pockets (front = the wall, 1.83 side / back walls, end ledges
    # under the body ends, open below for the soldered wires, open above for the screws), one 4.0 x 5.4
    # window per wire mouth, labels engraved on the outside
    for name, n, pins, x0, x1 in kf_row():
        xc = (x0 + x1) / 2
        yb = -IN + KF_D + 0.3
        add.append(box(x0 - LINE4, x1 + LINE4, -IN - 0.01, yb + LINE4, FLOOR - 0.01, KF_SEAT + 7.0)
                   - box(x0, x1, -IN - 0.1, yb, FLOOR - 0.1, KF_SEAT + 7.1))
        for xa, xb in ((x0 - 0.01, x0 + 1.3), (x1 - 1.3, x1 + 0.01)):
            add.append(box(xa, xb, -IN - 0.01, yb + 0.01, FLOOR - 0.01, KF_SEAT))
        # wire exits for the soldered pins through the back wall, under the seat (7 wide: short bridges)
        for k in range(max(1, n // 2)):
            ex = xc + (k - (max(1, n // 2) - 1) / 2) * 10.0
            cut.append(box(ex - 3.5, ex + 3.5, yb - 0.1, yb + LINE4 + 0.1, FLOOR, KF_SEAT - 1.0))
        for k in range(n):
            px = xc - (n - 1) * KF_P / 2 + k * KF_P
            # 3.15 windows (>= 4-line ribs) with a 45 deg pointed top: nothing to bridge in the wall
            hw = (KF_P - 1.85) / 2
            wz0, wz1 = KF_SEAT + KF_MOUTH - 2.6, KF_SEAT + KF_MOUTH + 2.0
            cut.append(box(px - hw, px + hw, -SIZE / 2 - 0.1, -IN + 0.1, wz0, wz1)
                       + Pos(px, -IN - WALL / 2, wz1) * Rot(0, 45, 0) * Box(hw * 1.4142, WALL + 0.4, hw * 1.4142))
            labels.append((pins.split()[k], px, KF_SEAT - 2.6))
        labels.append((name, xc, KF_SEAT + KF_MOUTH + 6.2))

    # 3 x LR7843 inside: corner cradles (corners over the open ring pocket left out), pads under the
    # signal-end holes, 2 pads under the load-terminal end between its 3 solder tails
    r_keep = RING_POCKET[1] + LINE4 + 0.5
    for x0, x1, y0, y1 in lr_boxes():
        corners = ((x0, y0), (x1, y0), (x0, y1), (x1, y1))
        skip = tuple(k for k, (xs, ys) in enumerate(corners) if math.hypot(xs, ys) - 4.3 < r_keep)
        # step 0.4: the terminal block is only 0.5 narrower than the PCB on each side
        add += cradle(x0, x1, y0, y1, LR_SEAT_Z, step=0.4, skip=skip)
        cx = (x0 + x1) / 2
        for px in (cx - 2.5, cx + 2.5):
            add.append(box(px - 1.0, px + 1.0, y0 + 0.5, y0 + 5.0, FLOOR - 0.01, LR_SEAT_Z))
        for sx in (-1, 1):
            hx, hy = cx + sx * LR_HOLE[0], y1 - LR_HOLE[1]
            add.append(cyl(hx, hy, 2.3, FLOOR - 0.01, LR_SEAT_Z))       # solid pad (hole spacing unsure: no pins)

    # perfboard for the CAM relay parts: corner cradle (the corners of the cut piece stay empty)
    px0, py0 = PERF_C[0] - PERF[0] / 2, PERF_C[1] - PERF[1] / 2
    add += cradle(px0, px0 + PERF[0], py0, py0 + PERF[1], PERF_SEAT)

    # 2 x 5 V buck: pads under the middle of the long edges, lips at the middle of all four edges
    lip_top = BUCK_SEAT + L(8) - 0.2
    for cx, cy in BUCK_C.values():
        bx0, bx1, by0, by1 = cx - BUCK[0] / 2, cx + BUCK[0] / 2, cy - BUCK[1] / 2, cy + BUCK[1] / 2
        for xa, xb, la, lb in ((bx0 - 0.25 - LINE4, bx0 + 1.2, bx0 - 0.25 - LINE4, bx0 - 0.25),
                               (bx1 - 1.2, bx1 + 0.25 + LINE4, bx1 + 0.25, bx1 + 0.25 + LINE4)):
            add.append(box(xa, xb, cy - 2.5, cy + 2.5, FLOOR - 0.01, BUCK_SEAT))
            add.append(box(la, lb, cy - 2.5, cy + 2.5, FLOOR - 0.01, lip_top))
        for ya, yb in ((by0 - 0.25 - LINE4, by0 - 0.25), (by1 + 0.25, by1 + 0.25 + LINE4)):
            add.append(box(cx - 2.0, cx + 2.0, ya, yb, FLOOR - 0.01, lip_top))

    # vents high on the +Y wall
    for i in range(-5, 6):
        cut.append(box(i * 9.0 - 1.5, i * 9.0 + 1.5, IN - 0.1, SIZE / 2 + 0.1, H - 14.0, H - 4.0))

    for a in add:
        body = body + a
    for c in cut:
        body = body - c
    body = body - engrave(labels)
    body = body & outer                              # posts that reach into a wall never poke out of it
    return body


def board_holes(cx, cy, hx, hy, d, z):
    return Compound([cyl(cx + sx * hx, cy + sy * hy, d / 2, z - 0.1, z + 1.7) for sx in (-1, 1) for sy in (-1, 1)])


def dummies():
    """Stand-in parts: previews and the interference check (not printed)."""
    d = {}
    z_led = DIFF_T                                       # LEDs face the plant, resting on the diffuser
    d["ring"] = (ring_solid(*C, RING_OD / 2, RING_ID / 2, z_led + 1.6, z_led + RING_T), "#e5e7eb")
    leds = [Pos(*polar(29.21, i * 15, C), z_led) * Rot(0, 0, i * 15) * Box(5, 5, 1.6, align=CCM)
            for i in range(24)]
    d["leds"] = (Compound(leds), "#fde68a")
    mx, my = MLX_BOARD
    d["mlx"] = (box(MLX_C[0] - mx / 2, MLX_C[0] + mx / 2, MLX_C[1] - my / 2, MLX_C[1] + my / 2,
                    MLX_PCB_Z, MLX_PCB_Z + 1.6) - board_holes(*MLX_C, *MLX_HOLES, 2.5, MLX_PCB_Z), "#1e293b")
    d["mlx_can"] = (cyl(*MLX_C, MLX_CAN_D / 2, MLX_PCB_Z - MLX_CAN_H, MLX_PCB_Z), "#9ca3af")
    cx0, cx1, cy0, cy1 = cam_box()
    cz = CAM_SEAT_Z
    d["cam"] = (box(cx0, cx1, cy0, cy1, cz, cz + CAM_PCB_T)
                + box(CAM_C[0] - 7.5, CAM_C[0] + 7.5, cy0, cy0 + 15.0, cz - 2.2, cz)          # SD slot
                + box(cx0 + 4.0, cx1 - 4.0, cy0 + 16.0, cy1, cz + CAM_PCB_T, cz + CAM_PCB_T + 3.4)  # shield
                + box(cx0 + 0.8, cx0 + 3.3, cy0 + 3.3, cy0 + 23.6, cz + CAM_PCB_T, cz + CAM_PCB_T + DUPONT)
                + box(cx1 - 3.3, cx1 - 0.8, cy0 + 3.3, cy0 + 23.6, cz + CAM_PCB_T, cz + CAM_PCB_T + DUPONT),
                "#111827")
    d["cam_lens"] = (box(CAM_C[0] - 4.25, CAM_C[0] + 4.25, CAM_C[1] - 4.25, CAM_C[1] + 4.25, CAM_HOLDER_Z,
                         CAM_SEAT_Z - 2.2) + cyl(*CAM_C, 3.6, CAM_HOLDER_Z - 2.0, CAM_HOLDER_Z)
                     + cyl(*CAM_C, 2.7, CAM_HOLDER_Z - 2.9, CAM_HOLDER_Z - 1.9), "#374151")
    sx, sy = SCD_BOARD
    d["scd"] = (box(SCD_C[0] - sx / 2, SCD_C[0] + sx / 2, SCD_C[1] - sy / 2, SCD_C[1] + sy / 2,
                    SCD_PCB_Z, SCD_PCB_Z + 1.6) - board_holes(*SCD_C, *SCD_HOLES, 3.0, SCD_PCB_Z)
                + box(SCD_C[0] - 5.05, SCD_C[0] + 5.05, SCD_C[1] - 4.3, SCD_C[1] + 5.8, SCD_PCB_Z - 7.8,
                      SCD_PCB_Z), "#1e293b")
    dx0, dx1, dy0, dy1 = DEV_X0, DEV_X0 + DEV[0], DEV_Y0, DEV_Y0 + DEV[1]
    dcx, dz = (dx0 + dx1) / 2, DEV_SEAT_Z
    pin_y = (dy0 + 0.02, dy0 + DEV_PCB_L - 0.02)
    d["dev"] = (box(dx0, dx1, dy0, dy0 + DEV_PCB_L, dz, dz + 1.6)
                + box(dcx - 9, dcx + 9, dy1 - 25.5, dy1, dz + 1.6, dz + 4.7)                 # module
                + box(dcx - 4.47, dcx + 4.47, dy0 - 0.03, dy0 + 7.32, dz + 1.7, dz + 1.6 + 3.26)
                + box(dx0 + 1.27 - 1.27, dx0 + 1.27 + 1.27, *pin_y, dz - DUPONT, dz)          # duponts
                + box(dx1 - 1.27 - 1.27, dx1 - 1.27 + 1.27, *pin_y, dz - DUPONT, dz), "#0f766e")
    rx0, rx1, ry0, ry1 = RS_X0, RS_X0 + RS[0], RS_Y0, RS_Y0 + RS[1]
    rz = RS_SEAT_Z
    d["rs"] = (box(rx0, rx1, ry0, ry1, rz, rz + 1.6)
               + box(rx0 + 2, rx1 - 2, ry1 - 12, ry1 - 4, rz + 1.6, rz + 11.6)              # A/B terminal
               + box(rx0 + 1.0, rx1 - 1.0, ry0 + 0.3, ry0 + 2.84, rz - DUPONT, rz)          # duponts
               + box(rx0 + 1.0, rx1 - 1.0, ry1 - 2.84, ry1 - 0.3, rz - DUPONT, rz), "#2563eb")
    lz = LR_SEAT_Z
    for k, (x0, x1, y0, y1) in enumerate(lr_boxes()):
        cx = (x0 + x1) / 2
        d[f"lr{k + 1}"] = (box(x0, x1, y0, y1, lz, lz + 1.6)
                           + box(cx - 7.5, cx + 7.5, y0 - LR_TERM_OVER, y0 + 5.8, lz + 1.6, lz + 11.6)  # load
                           + box(cx - 4.0, cx + 4.0, y1 - 7.0, y1 - 0.5, lz + 1.6, lz + 9.6)           # PWM
                           + box(cx - 3.3, cx + 3.3, y1 - 19.6, y1 - 13.0, lz + 1.6, lz + 4.0)         # DPAK
                           + Compound([cyl(cx + t, y0 + 1.75, 1.2, lz - 3.5, lz) for t in (-5, 0, 5)])  # tails
                           + box(cx - 2.5, cx + 2.5, y1 - 4.5, y1 - 1.5, lz - 2.0, lz), "#7c3aed")
    for name, n, pins, x0, x1 in kf_row():
        xc = (x0 + x1) / 2
        w = n * KF_P
        d[f"kf_{name}"] = (box(xc - w / 2, xc + w / 2, -IN, -IN + KF_D, KF_SEAT, KF_SEAT + KF_H)
                           + Compound([cyl(xc - (n - 1) * KF_P / 2 + k * KF_P, -IN + 3.35, 0.5, KF_SEAT - 4.3,
                                           KF_SEAT) for k in range(n)]), "#1d4ed8" if n == 4 else "#16a34a")
    px0, py0 = PERF_C[0] - PERF[0] / 2, PERF_C[1] - PERF[1] / 2
    pz = PERF_SEAT
    d["perf"] = (box(px0, px0 + PERF[0], py0, py0 + PERF[1], pz, pz + 1.6)
                 + box(px0 + 2.0, px0 + PERF[0] - 2.0, py0 + 2.0, py0 + PERF[1] - 2.0, pz - 2.5, pz)     # tails
                 + Compound([box(PERF_C[0] - 3.3, PERF_C[0] + 3.3, PERF_C[1] + dy - 2.3, PERF_C[1] + dy + 2.3,
                                 pz + 1.6, pz + 5.1) for dy in (-7.0, 7.0)]),                        # 2 x DIP-4
                 "#0e7490")
    bz = BUCK_SEAT
    for name, (cx, cy) in BUCK_C.items():
        w, ln = 12.0, 17.5
        pads = [(cx + sx * 4.2, cy + sy * 7.2) for sx in (-1, 1) for sy in (-1, 1)]
        d[f"buck_{name}"] = (box(cx - w / 2, cx + w / 2, cy - ln / 2, cy + ln / 2, bz, bz + 1.2)
                             + box(cx - 3.5, cx + 3.5, cy + 0.5, cy + 7.1, bz + 1.2, bz + 4.3)       # inductor
                             + box(cx - 2.5, cx + 2.5, cy - 6.5, cy - 1.5, bz + 1.2, bz + 2.9)       # IC
                             + Compound([cyl(px, py, 1.0, bz - 1.0, bz + 2.2) for px, py in pads]),  # joints
                             "#b45309")
    bolts, nuts = [], []
    z_head = H + LID_T - M3_CB[1]                       # counterbore floor
    for sx in (-1, 1):
        for sy in (-1, 1):
            hx, hy = sx * FAN_HOLE, sy * FAN_HOLE
            bolts += [cyl(hx, hy, 1.5, z_head - 8.0, z_head), cyl(hx, hy, 2.75, z_head, z_head + 3.0)]
            nuts.append(Pos(hx, hy, NUT_Z[1] - 2.4) * Rot(0, 0, 180.0 if sx > 0 else 0.0)
                        * (extrude(RegularPolygon(5.5 / math.sqrt(3), 6), amount=2.4) - cyl(0, 0, 1.6, -0.1, 2.5)))
    d["m3_bolts"] = (Compound(bolts), "#a1a1aa")
    d["m3_nuts"] = (Compound(nuts), "#d4d4d8")
    return d


def interference(parts: dict, dm: dict):
    """Volume (mm^3) each stand-in shares with each printed part: everything should be ~0."""
    res = {}
    for pn, ps in parts.items():
        for n, (s, _) in dm.items():
            try:
                v = (ps & s).volume
            except Exception:  # noqa: BLE001
                v = -1.0
            if abs(v) > 0.05:
                res[f"{pn} x {n}"] = round(v, 2)
    return res


def render(parts, dm):
    import pyvista as pv
    pv.OFF_SCREEN = True
    prev = OUT / "preview"
    cache = OUT / "_cache"
    prev.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)

    def mesh(shape, name):
        p = cache / f"{name}.stl"
        export_stl(shape, str(p), tolerance=0.05, angular_tolerance=0.2)
        return pv.read(str(p))

    meshes = {"body": (mesh(parts["body"], "body"), "#5b6068"),
              "skin": (mesh(diffuser_skin(), "skin"), "#f8fafc"),
              "scd_cap": (mesh(parts["scd_cap"], "scd_cap"), "#94a3b8"),
              "lid": (mesh(parts["lid"], "lid"), "#3f4550")}
    for n, (s, col) in dm.items():
        meshes[n] = (mesh(s, n), col)

    def shot(fname, names, pos, focal, up=(0, 0, 1), clip=None, parallel=None, opacity=None):
        pl = pv.Plotter(off_screen=True, window_size=(1600, 1200))
        pl.set_background("#f4f5f7")
        for n in names:
            m, col = meshes[n]
            if clip is not None:
                m = m.clip(normal=clip[0], origin=clip[1], invert=False)
            if m.n_points:
                op = (opacity or {}).get(n, 1.0)
                pl.add_mesh(m, color=col, smooth_shading=False, specular=0.25, specular_power=20, opacity=op)
        pl.camera.position, pl.camera.focal_point, pl.camera.up = pos, focal, up
        if parallel:
            pl.enable_parallel_projection()
            pl.camera.parallel_scale = parallel
        pl.enable_anti_aliasing("ssaa")
        pl.screenshot(str(prev / fname))
        pl.close()

    allm = [n for n in meshes if n not in ("lid", "m3_bolts", "skin")]
    see = {"skin": 0.9, "scd_cap": 0.45}
    shot("node_sensor_face.png", allm + ["skin"], (0, 0, -400), (0, 0, 0), up=(0, 1, 0), parallel=82, opacity=see)
    shot("node_sensor_face_bare.png", allm, (0, 0, -400), (0, 0, 0), up=(0, 1, 0), parallel=82)
    shot("node_sensor_face_3q.png", allm, (-220, -260, -230), (0, 0, 15), opacity=see)
    shot("node_inside_top.png", allm, (0, 0, 400), (0, 0, 0), up=(0, 1, 0), parallel=82)
    shot("node_inside_3q.png", allm, (-200, -240, 260), (0, 0, 10), opacity=see)
    shot("node_scd_chamber.png", [n for n in allm if n != "scd_cap"], (-39, -20, 120), (-39, 52, 8))
    shot("node_inside_3q_empty.png", ["body"], (-200, -240, 260), (0, 0, 10))
    shot("node_section_optics.png", allm, (0, -400, 18), (0, 0, 18), clip=((0, 1, 0), (0, C[1], 0)),
         parallel=48)
    shot("node_section_optics_zoom.png", allm, (0, -400, 8), (0, 0, 8), clip=((0, 1, 0), (0, C[1], 0)),
         parallel=24)
    shot("node_connector_face.png", ["body"] + [n for n in allm if n.startswith("kf_") or n == "dev"],
         (0, -400, 19), (0, 0, 19), parallel=54)
    shot("node_connector_face_zoom.png", ["body"] + [n for n in allm if n.startswith("kf_") or n == "dev"],
         (5, -400, 16), (5, 0, 16), parallel=22)
    shot("node_connector_inside.png", allm, (10, 60, 90), (10, -60, 10))
    shot("node_lid_3q.png", ["body", "lid", "m3_bolts"], (-200, -240, 260), (0, 0, 10))
    shot("node_lid_off.png", ["body", "m3_nuts"], (-150, -200, 230), (0, 0, 15))
    shot("node_lid_underside.png", ["lid"], (-160, -200, -120), (0, 0, 39))
    shot("node_corner_section.png", ["body", "lid", "m3_bolts", "m3_nuts"], (400, FAN_HOLE, 32),
         (FAN_HOLE, FAN_HOLE, 32), clip=((-1, 0, 0), (FAN_HOLE, 0, 0)), parallel=10)
    shot("node_snap_section.png", ["body", "lid"], (400, IN, 35), (0, IN, 35), clip=((-1, 0, 0), (0, 0, 0)),
         parallel=5)


def diffuser_skin():
    """Preview only: the white skin under the ring pocket (part of the body, printed before the M600)."""
    return ring_solid(*C, RING_POCKET[1], RING_POCKET[0], -0.05, DIFF_T - 0.05)   # 0.05 proud: no z-fighting


def parts():
    return {"body": make_body(), "scd_cap": make_scd_cap(), "lid": make_lid()}


def main():
    (OUT / "stl").mkdir(parents=True, exist_ok=True)
    p = parts()
    info = {}
    for n, s in p.items():
        export_stl(to_bed(n, s), str(OUT / "stl" / f"node_{n}.stl"), tolerance=0.01, angular_tolerance=0.1)
        bb = s.bounding_box()
        info[n] = {"size": [round(bb.size.X, 2), round(bb.size.Y, 2), round(bb.size.Z, 2)],
                   "volume_cm3": round(s.volume / 1000, 2), "valid": s.is_valid}
    export_step(Compound(list(p.values())), str(OUT / "node_v1.step"))
    dm = dummies()
    info["interference_mm3"] = interference(p, dm)
    for a, b_ in (("lid", "body"), ("scd_cap", "body")):
        info[f"{a}_x_{b_}_mm3"] = round((p[a] & p[b_]).volume, 3)
    (OUT / "build_info.json").write_text(json.dumps(info, indent=2))
    print(json.dumps(info, indent=1))
    render(p, dm)


if __name__ == "__main__":
    main()
