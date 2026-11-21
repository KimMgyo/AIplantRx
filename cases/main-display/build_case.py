#!/usr/bin/env python3
"""
Main display desk-stand case (build123d 0.13)

Hardware
  * Waveshare ESP32-S3-Touch-LCD-7 Rev1.2 (7" 800x480 touch), official STEP/DXF/PDF
  * 5 x OLED 0.96" SSD1306 I2C GME12864-11/12/13 (PCB 27.5 x 27.8, 4 x d2.0 holes 23.2 x 23.5)
  * 1 x TCA9548A I2C multiplexer (CJMCU clone, ~31 x 21, 2 holes ~25.6 apart)

Printer: Original Prusa i3 MK2S, stock 0.4 nozzle, PrusaSlicer "0.35mm FAST"
(first layer 0.20, then 0.35).  Heights that matter are put on layer boundaries L(n).

Fasteners: every joint is an M3 x 10 screw into a captive M3 hex nut (5.5 AF x 2.4, DIN 934) or
an M3 F-F brass hex support 3 mm long (5.5 AF x 3.0).  Each pocket takes either, and each pocket
sits so the screw pulls the nut INTO plastic.  The 7" module's own brass standoffs are used as-is.

Case frame used for all design numbers
  X -> right (as seen by the viewer), Y -> up, Z -> toward the viewer
  Z = 0 is the 7" glass front face = back face of the front plate.
  Glass centre is the XY origin.  The PCB / 11.00 mm bezel side of the module is UP
  (same orientation as Waveshare's drawing), so the two USB-C ports are on the LEFT,
  41.4 mm in from the left glass edge, opening toward -X.
"""
from __future__ import annotations

import json
import math
from pathlib import Path

from build123d import (Align, Box, Cone, Cylinder, Ellipse, Kind, Plane, Polygon, Pos, Rectangle,
                       RectangleRounded, Rot, Text, export_step, export_stl, extrude, loft,
                       offset, Compound)

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
MIN3 = (Align.MIN, Align.MIN, Align.MIN)
CCM = (Align.CENTER, Align.CENTER, Align.MIN)

# --------------------------------------------------------------------------- printer
FIRST_LAYER, LAYER = 0.20, 0.35


def L(n: int) -> float:
    """Top of layer n+1 (n = 0 -> first layer)."""
    return round(FIRST_LAYER + LAYER * n, 3)


# ------------------------------------------- Waveshare ESP32-S3-Touch-LCD-7 (official STEP)
GLASS_W, GLASS_H, GLASS_R = 192.96, 110.76, 7.88
VA_W, VA_H = 154.88, 86.72
VA_TOP_BEZEL, VA_BOTTOM_BEZEL = 11.00, 13.04
VA_CY = (VA_BOTTOM_BEZEL - VA_TOP_BEZEL) / 2          # +1.02 (VA sits higher)
GLASS_BACK_Z = -2.10                                    # back of cover glass + touch film
STANDOFFS = [(-61.61, 33.80), (64.59, 33.80), (-61.61, -31.85), (64.59, -31.85)]
STANDOFF_LEN = 8.00                                     # factory M3 brass standoffs
FRAME_BACK_Z = -8.50                                    # back face of the metal frame tabs
STANDOFF_END_Z = FRAME_BACK_Z - STANDOFF_LEN            # -16.50
USB_EDGE_X = -55.09                                     # receptacle mouths (PCB edge)
USB_PORTS = {"USB": 11.55, "UART": -3.55}               # Y of the two receptacle centres
USB_Z = -11.73
I2C_HDR = (45.40, 50.44, -2.42, 7.58)                   # x0, x1, y0, y1 of the HY2.0 4P I2C
HDR_TOP_Z = -18.40                                      # tallest part of the module
BUTTONS = {"BOOT": 4.12, "RESET": -6.98}                # side-actuated, at PCB top edge
BTN_Z = -11.90
# optional side exit for the two USB-C cables, straight out of the left wall in line with the ports.
# 32.1 x 9 stadium: both plug bodies (12.5 x 6.5, also chunkier overmolds) pass with room at the
# rounded ends.  Knock-out: cut from the inside, a thin skin stays on the outer face until you cut it
# out with a knife (SIDE_USB_SKIN = 0 makes it an open hole).
SIDE_USB_SLOT = (min(USB_PORTS.values()) - 8.5, max(USB_PORTS.values()) + 8.5, USB_Z - 4.5, USB_Z + 4.5)
SIDE_USB_SKIN = 0.6

# ------------------------------------------------------------------------ case layout
TILT = 20.0                     # degrees, screen leans back
_T = math.radians(TILT)
GAP = 0.30                      # glass / PCB clearance per side
WALL = 2.60
TOP_WALL = 8.60                 # thick top wall: 4 screws + side-loaded nut slots outside the glass
FRONT_T = L(5)                  # 1.95 front plate
TUB_H = L(81)                   # 28.55 front frame height (print Z)
FRONT_Z = FRONT_T
RIM_Z = FRONT_T - TUB_H         # -26.60
COVER_T = L(6)                  # 2.30
COVER_OUT_Z = RIM_Z - COVER_T   # -28.90
COVER_CH = 0.8                  # outer edge chamfer of the back cover

IN_X = GLASS_W / 2 + GAP        # 96.78
IN_TOP = GLASS_H / 2 + GAP      # 55.68
IN_R = GLASS_R + GAP            # 8.18
RIB_T = 2.0
RIB_H = L(12) - FRONT_T         # 2.45 glass locating rib below the glass
RIB_Y1 = -(GLASS_H / 2 + GAP)   # -55.68
RIB_Y0 = RIB_Y1 - RIB_T         # -57.68

# 0.96" OLED GME12864-11/12/13 (SSD1306, 4-pin I2C), header edge up.  Mount measured from MakerWorld
# 2703510 (research/gme12864) and fit-tested: the glass rests on the front plate, the PCB on 4 bosses
# 2.1 mm behind it.  d1.7 pins on the bosses locate the board in its d2.0 holes; posts on the back
# cover stop 0.2 mm behind the hole rings (where that design's M2 screw heads sat) and keep it there.
OLED_W, OLED_H, OLED_PCB_T = 27.5, 27.8, 1.2
OLED_TOP = RIB_Y0 - 0.32        # -58.00 PCB top edge
OLED_CY = OLED_TOP - OLED_H / 2  # -71.90 board centre
OLED_HOLE_DX, OLED_HOLE_DY = 11.6, 11.75     # hole centres from the board centre: 23.2 x 23.5
OLED_AA_W = 21.744                           # lit pixel area
OLED_WIN_W, OLED_WIN_H, OLED_WIN_DY = 26.0, 14.0, 2.8   # window, its centre 2.8 above the board centre
OLED_WIN_CH = 0.5                            # front-face chamfer (27 x 15 on the front face)
OLED_PITCH = (VA_W - OLED_AA_W) / 4          # 33.284: outer lit areas line up with the 7" VA
OLED_X = [OLED_PITCH * i for i in (-2, -1, 0, 1, 2)]
OLED_STANDOFF = round(L(11) - FRONT_T, 3)    # 2.10: glass + tape between the front plate and the PCB
OLED_BOSS_D = 4.0
OLED_STRIP_DY = (10.4, 11.8)                 # strip joining the header-side bosses; they stop at 10.4
                                             # (glass edge) - the header joints sit above 11.8
OLED_HDR_DY = 12.7                           # header pin row above the board centre
OLED_PIN_D = 1.7
OLED_PIN_Z = FRONT_Z - L(14)                 # -3.15: pins end inside the PCB hole (1.05 of its 1.2 mm),
                                             # 0.35 short of the cover posts - they never touch them
OLED_POST_R, OLED_POST_GAP = 2.0, 0.2
OLED_POST_Z = -OLED_STANDOFF - OLED_PCB_T - OLED_POST_GAP   # -3.50 post tip (cover print Z = L(72))
OLED_POST_RECESS = (1.25, 1.05)              # relief in each post tip: radius, depth (floor on the grid)

IN_BOT = OLED_TOP - OLED_H - 0.5             # -86.30: 0.5 below the PCB for the glass FPC folded round its edge
OUT_X = IN_X + WALL                          # 99.38
OUT_TOP = IN_TOP + TOP_WALL                  # 63.28
OUT_BOT = IN_BOT - WALL                      # -87.90
OUT_R = IN_R + WALL                          # 10.78
# Rectangular case leaning back: its lowest point is the back cover's bottom edge (just above the
# chamfer).  That edge rests on the desk; the rear leg holds the tilt.
CONTACT = (OUT_BOT, COVER_OUT_Z + COVER_CH)                # case-frame (Y, Z) of the desk contact
Y_DESK = CONTACT[0] * math.cos(_T) + CONTACT[1] * math.sin(_T)

# ------------------------------------------------------------------------ fasteners
SCREW_LEN = 10.0                             # M3 x 10 everywhere
SCREW_R = 1.45                               # thread body used for checks
CLEAR_R = 1.7                                # d3.4 clearance hole
NUT_AF = 5.5                                 # M3 nut / brass hex support, across flats
NUT_T_MAX = 3.0                              # F-3mm support (a 2.4 mm nut also fits)
POCKET_AF = NUT_AF + 0.25                    # 5.75
POCKET_T = 3.15                              # nine 0.35 layers

# screws front-frame <- back cover.  The 7" module is pushed in from the back, so nothing of the
# front frame may sit inside the glass outline: the top screws live in the thick top wall, the
# others in the OLED chin.  Nuts slide in sideways from the cavity into slots 2.45 mm above the
# rim, so tightening pulls them against plastic.
TOP_SCREW_Y = IN_TOP + 3.0                   # 58.68 (nut slot leaves 2.2 mm top skin)
CHIN_SCREW_Y = -80.0
# screw heads sit in d6.4 counterbores so the back cover stays flat: a 3.0 mm socket head ends
# 0.35 mm below the outer face, a pan head (<= d6.0, <= 2.4 high) about 1 mm below.  Each counterbore
# is backed by a cover boss that sinks 2.45 mm into a 2.8 mm pocket in the frame rim, so the cover sits on
# the rim and never on the bosses.
HEAD_CB_R = 3.2
HEAD_SEAT_Z = COVER_OUT_Z + L(9)             # -25.55 counterbore floor (head seat)
RIM_BOSS_R, RIM_POCKET_R = 4.2, 4.4
RIM_POCKET_Z = FRONT_Z - L(73)               # -23.80 pocket floor in the frame
RIM_BOSS_TOP = COVER_OUT_Z + L(13)           # -24.15 (cover print Z 4.75): 0.35 below the pocket floor.
                                             # An off-grid top would be rounded up a layer by the slicer.
TUB_NUT_Z = (FRONT_Z - L(67), FRONT_Z - L(58))   # (-21.70, -18.55) slot, rim side first
CHIN_BLOCK_W = 7.8                           # (no chin screws between the OLEDs any more: 5.8 mm gaps)
CHIN_BLOCK_TOP = -72.5                       # top of the side-loading nut slot in the corner blocks
TUB_SCREWS = [(-85.0, TOP_SCREW_Y, "top"), (-35.0, TOP_SCREW_Y, "top"), (35.0, TOP_SCREW_Y, "top"),
              (85.0, TOP_SCREW_Y, "top"), (-93.0, CHIN_SCREW_Y, "corner"), (93.0, CHIN_SCREW_Y, "corner")]

# back cover
LIP_C, LIP_T = 0.25, 1.6
LIP_H = round(L(14) - L(6), 3)               # 2.80
COVER_BOSS_TOP = STANDOFF_END_Z + 0.05       # 0.05 mm preload on the glass (print Z = L(35))
STANDOFF_CB_FLOOR = COVER_OUT_Z + L(16)      # -23.10: deep counterbore, M3x10 bites 3.4 mm
TCA_C = (0.0, -38.0)
TCA_PITCH = 25.6                             # measure your board! (sellers: 25.4 - 25.6)
TCA_SIZE = (31.5, 21.5)
TCA_PCB_T = 1.6
TCA_SEAT_R, TCA_SEAT_HW = 4.0, 3.6           # the TCA touches the bracket only round its two holes and on a
                                             # strip from each hole to the board end (no parts there)
# TCA9548A + ESP32-C3 SuperMini carrier (04_tca_bracket).  It lies on the module's two lower brass standoffs
# - their ends drop into cups - and the two lower cover screws pass through it, so both boards belong to the
# front assembly: every I2C wire (OLEDs, ESP32 HY2.0, TCA) stays on the front when the back cover comes off.
# Both boards sit on its back face (cover side) with their parts toward the module, so with the cover off you
# see their undersides side by side and can solder from the back.
# Print Z of the bracket = z - BRK_Z0 (back face on the bed).
BRK_Z0 = COVER_OUT_Z + L(20)                 # -21.70 back face = top of the two lower cover bosses
BRK_CUP_FLOOR = BRK_Z0 + L(14)               # -16.60 the standoff ends sit here.  Both parts are on their
                                             # layer grids, so the stack cannot hit -16.50 exactly: the lower
                                             # screws pull the module's lower edge 0.1 mm back (clamped, no play)
                                             # rather than pushing the cover 0.25 mm off its rim.
BRK_TOP = BRK_Z0 + L(18)                     # -15.20: 1.4 mm cups around the standoff ends
BRK_CUP_R, BRK_TOWER_R = 3.4, 4.5
BRK_T = L(6)                                 # 2.30 arm / pad plate
BRK_ARM_W, BRK_RIB_W = 8.0, 2.0
BRK_PAD = (19.5, -52.5, -27.0)               # ring round the TCA window: half width, y0, y1
TCA_NUT_FLOOR = BRK_Z0 + L(3)                # -20.45 nut pocket floor; the pocket opens on the module side
TCA_BOSS_TOP = BRK_Z0 + L(12)                # -17.30 boss top = nut pocket mouth
TCA_HEAD_Z = BRK_Z0 - TCA_PCB_T              # -23.30 M3 x 10 heads on the TCA underside
TCA_RELIEF = COVER_OUT_Z + L(3)              # -27.65 cover inner face behind the TCA (1.05 deeper): heads, wires
# ESP32-C3 SuperMini (TENSTAR) on the same bracket, lying crosswise straight above the TCA9548A.  It has no
# mounting holes, so it drops upside down into a pocket in the bracket back face: the USB-C shell rests on
# the pocket floor and the two bare antenna-end corners on ledges, the bare underside faces the back cover
# and two cover posts keep it in (like the OLEDs).  The pocket starts just above the TCA board (its floor
# sits at the TCA board's height), behind the ESP32-S3 board, whose parts stop at z -12.6.  USB-C points
# sideways, so a cable plugs in from the side with the cover off.
# Sizes: STEP models (github mrtnvgr/KiCad_ESP32-C3-SuperMini, Printables 1360390), nologo drawing.
C3_W, C3_L, C3_L_MAX = 18.0, 22.52, 22.8     # PCB width, length (some batches are 22.8 long)
C3_USB_W, C3_USB_H = 9.0, 3.16               # USB-C shell width, height above the PCB top (3.12 - 3.16)
C3_USB_OVER = 1.4                            # shortest shell overhang past the PCB end (1.4 - 2.0)
C3_PLUG_HW = 7.0                             # slot for the USB-C plug body (up to 14 wide); stops outside it
C3_X = TCA_C[0]                              # pad centred on the TCA
C3_WALL_Y = BRK_PAD[2]                       # -27.0: pad starts here, 0.25 above the TCA board
C3_USB_DIR = -1                              # USB-C toward -X: same side as the 7" module's USB-C ports
                                             # (left seen from the front); 1 turns it the other way
C3_CL = 0.15                                 # pocket clearance per side
C3_HW = C3_W / 2 + C3_CL                     # 9.15 pocket half width
C3_SIDE = 2.65                               # pad beyond the pocket: 1.05 solder relief + 1.6 wall
C3_Y = C3_WALL_Y + C3_HW + C3_SIDE           # -15.2 board centre line
C3_POCKET_L = C3_L_MAX + 2 * C3_CL           # 23.1 along the board (fits the 22.8 boards)
C3_PAD_L = (-1.6, C3_POCKET_L + 1.15)        # along the board from the antenna-end wall face: wall, stops
C3_LEDGE_L = C3_CL + 1.92                    # ledges under the antenna-end corners (pins start 2.2 in)
C3_BAR_W = 5.8                               # bar across the pocket under the USB-C shell (rest of the floor
                                             # is open, like the TCA window); always 5.2+ under the shell
C3_ANT_HW = 3.9                              # ledges stay this far from the centre line (antenna 7.1 wide)
C3_LEDGE_Z = BRK_Z0 + L(3)                   # -20.45 PCB top (component side) at the antenna-end corners
C3_FLOOR_Z = BRK_Z0 + L(12)                  # -17.30 the USB-C shell rests here (3.15 above the ledges)
C3_PAD_TOP = BRK_Z0 + L(15)                  # -16.25 module-side face of the thicker pad round the pocket
C3_POST_R = 1.5
C3_POST_TIP = COVER_OUT_Z + L(20)            # -21.70 = bracket back face: 0.25 off a 1.0 mm PCB, 0.35 off 0.9.
                                             # Thicker boards: one layer lower per 0.35 (L(19), L(18) ...)


def c3_xy(u, v=0.0):
    """Case X, Y of a point on the C3 pocket: u along the board from the antenna-end wall face toward the
    USB end, v across it."""
    um = (C3_PAD_L[0] + C3_PAD_L[1]) / 2
    return (C3_X + C3_USB_DIR * (u - um), C3_Y - C3_USB_DIR * v)


def c3_frame():
    """Location taking the pocket's local frame (X across, Y = u along the board) to the case."""
    um = (C3_PAD_L[0] + C3_PAD_L[1]) / 2
    return Pos(C3_X, C3_Y, 0) * Rot(0, 0, -90 * C3_USB_DIR) * Pos(0, -um, 0)


C3_POSTS = [c3_xy(C3_L - 10.0), c3_xy(C3_L - 17.0)]   # on the bare underside, centre line
STAND_SCREWS = [(-35.0, -60.0), (35.0, -60.0), (-35.0, -14.0), (35.0, -14.0)]
VENT_X = [float(x) for x in range(-48, 49, 6)]
VENT_Y = (27.0, 45.0)
LOW_VENT_X = [s * x for s in (-1, 1) for x in (56.0, 61.5, 67.0, 72.5, 78.0)]   # intake, beside the stand
LOW_VENT_Y = (-72.0, -63.0)
# every cable (both USB-C incl. power, I2C ...) leaves through one big hole low in the middle of the
# cover, straight into the stand.  The USB-C cables run inside the case from the plugs to this hole.
# It sits between the rows of OLED posts (y -62.2 and -81.7).
CABLE_HOLE = (-28.0, 28.0, -79.5, -62.8)      # x0, x1, y0, y1 in the cover
# opening in the stand plate = the cover hole with 45 deg corners: printed on its side, the hole's
# ends are short 3.7 mm bridges.
PLATE_HOLE = [(-21.5, -79.5), (21.5, -79.5), (28.0, -73.0), (28.0, -69.3), (21.5, -62.8),
              (-21.5, -62.8), (-28.0, -69.3), (-28.0, -73.0)]

# stand: a plain triangular frame (back plate, base, brace) screwed to the cover.
STAND_W = 86.0
PLATE_T, BASE_T, BRACE_T = 6.0, 5.0, 6.0
PLATE_TOP_Y = 5.0                            # case-frame Y where the back plate ends
STAND_Z_BACK = -90.0                         # world Z of the rear edge
PLATE_NUT_Z = COVER_OUT_Z - PLATE_T + POCKET_T   # -31.75: nut seat, pocket opens into the tunnel
BASE_HOLE = (31.0, -62.0, -18.0, 10.0)       # half length in x, z0, z1 (world), end flat: cables out the
                                             # bottom; 45 deg ends so it prints on its side without long bridges
BRACE_HOLE = (26.0, 46.0)                    # round rear window: radius, centre distance up the brace
BRACE_HOLE_AXIS = 48.0                       # bore axis elevation (deg): no bore face overhangs > 42 deg
TIE_SLOT = (5.5, 2.8)                        # cable-tie slot, x by z
TIE_PAIR = 7.0
TIE_SLOTS = [(sx * 35.5, z) for z in (-16.0, -70.0) for sx in (-1, 1)]   # pair centres on the base

# ------------------------------------------------------------------------- helpers
def rr(x0, x1, y0, y1, r, z0, z1):
    """Rounded-rectangle prism spanning x0..x1, y0..y1, z0..z1."""
    w, h = x1 - x0, y1 - y0
    zlo, zhi = min(z0, z1), max(z0, z1)
    sk = RectangleRounded(w, h, r) if r > 0 else Rectangle(w, h)
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, zlo) * extrude(sk, zhi - zlo)


def rr_loft(x0, x1, y0, y1, r, z0, grow, z1):
    """Rounded rectangle at z0 lofted to z1 where every side has moved by 'grow'."""
    cx, cy, w, h = (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0
    s0 = Pos(cx, cy, z0) * RectangleRounded(w, h, r)
    s1 = Pos(cx, cy, z1) * RectangleRounded(w + 2 * grow, h + 2 * grow, r + grow)
    return loft([s0, s1], ruled=True)


def cyl(x, y, r, z0, z1):
    zlo, zhi = min(z0, z1), max(z0, z1)
    return Pos(x, y, zlo) * Cylinder(r, zhi - zlo, align=CCM)


def box(x0, x1, y0, y1, z0, z1):
    return Pos(min(x0, x1), min(y0, y1), min(z0, z1)) * Box(abs(x1 - x0), abs(y1 - y0), abs(z1 - z0), align=MIN3)


def c2w(yc, zc):
    """Case frame (Y, Z) -> world (y up, z toward viewer) for the tilted case."""
    t = math.radians(TILT)
    return (yc * math.cos(t) + zc * math.sin(t), -yc * math.sin(t) + zc * math.cos(t))


def to_world(shape):
    return Rot(-TILT, 0, 0) * shape


def hex_prism(cx, cy, af, z0, z1, corner_deg=90.0):
    """Hexagonal prism along Z.  corner_deg=90 puts corners on +-Y (flats face +-X)."""
    r = af / math.sqrt(3.0)
    pts = [(cx + r * math.cos(math.radians(corner_deg + 60 * k)),
            cy + r * math.sin(math.radians(corner_deg + 60 * k))) for k in range(6)]
    zlo, zhi = min(z0, z1), max(z0, z1)
    return Pos(0, 0, zlo) * extrude(Polygon(*pts, align=None), zhi - zlo)


def hex_slot(cx, cy, af, z0, z1, dy):
    """Hex at (cx, cy) swept along Y by dy (corners on +-Y): a side-loading nut slot."""
    return (hex_prism(cx, cy, af, z0, z1) + box(cx - af / 2, cx + af / 2, cy, cy + dy, z0, z1)
            + hex_prism(cx, cy + dy, af, z0, z1))


def w_up(local, y0):
    """Local XY = world (x, -z), local Z = world y; then lift to y0."""
    return Pos(0, y0, 0) * (Rot(-90, 0, 0) * local)


# ------------------------------------------------------------------------ front frame
def make_front_frame():
    ch = 1.0
    shell = rr(-OUT_X, OUT_X, OUT_BOT, OUT_TOP, OUT_R, RIM_Z, FRONT_Z - ch)
    shell = shell + rr_loft(-OUT_X, OUT_X, OUT_BOT, OUT_TOP, OUT_R, FRONT_Z - ch, -ch, FRONT_Z)
    body = shell - rr(-IN_X, IN_X, IN_BOT, IN_TOP, IN_R, RIM_Z - 1.0, 0.0)

    add = [box(-IN_X - 0.1, IN_X + 0.1, RIB_Y0, RIB_Y1, -RIB_H, 0.3)]      # glass locating rib
    for cx in OLED_X:                                                       # OLED bosses, strip, pins
        cy = OLED_CY
        for sx in (-1, 1):
            x = cx + sx * OLED_HOLE_DX
            add.append(cyl(x, cy - OLED_HOLE_DY, OLED_BOSS_D / 2, -OLED_STANDOFF, 0.3))
            add.append(cyl(x, cy + OLED_HOLE_DY, OLED_BOSS_D / 2, -OLED_STANDOFF, 0.3)       # header side:
                       & box(x - 3.0, x + 3.0, cy + OLED_STRIP_DY[0], cy + OLED_HOLE_DY + 3.0,   # flat toward
                             -OLED_STANDOFF - 0.1, 0.4))                                         # the glass
            for y in (cy - OLED_HOLE_DY, cy + OLED_HOLE_DY):
                add.append(cyl(x, y, OLED_PIN_D / 2, OLED_PIN_Z + 0.35, -OLED_STANDOFF + 0.01))
                add.append(Pos(x, y, OLED_PIN_Z) * Cone(OLED_PIN_D / 2 - 0.3, OLED_PIN_D / 2, 0.35, align=CCM))
        add.append(box(cx - OLED_HOLE_DX, cx + OLED_HOLE_DX, cy + OLED_STRIP_DY[0], cy + OLED_STRIP_DY[1],
                       -OLED_STANDOFF, 0.3))
    for x, y, kind in TUB_SCREWS:                                           # chin screw bosses
        if kind == "top":
            continue                                                        # solid top wall
        if kind == "chin":                                                  # narrow block between OLEDs
            add.append(box(x - CHIN_BLOCK_W / 2, x + CHIN_BLOCK_W / 2, IN_BOT - 0.5, CHIN_BLOCK_TOP, RIM_Z, 0.3))
        else:
            add.append(cyl(x, y, 5.0, RIM_Z, 0.3))
            add.append(box(x - 5.0, x + 5.0, y, IN_BOT - 0.5, RIM_Z, 0.3))  # web to the bottom wall
            s = 1 if x > 0 else -1
            add.append(box(x, s * (IN_X + 0.5), y - 4.0, y + 4.0, RIM_Z, 0.3))
    body = body.fuse(*add) & shell                                          # nothing past the outer skin

    cut = []
    wx0, wx1 = -VA_W / 2 - 0.75, VA_W / 2 + 0.75
    wy0, wy1 = VA_CY - VA_H / 2 - 0.75, VA_CY + VA_H / 2 + 0.75
    cut.append(rr(wx0, wx1, wy0, wy1, 1.0, -1.0, FRONT_Z + 1.0))
    cut.append(rr_loft(wx0, wx1, wy0, wy1, 1.0, FRONT_Z - 1.0, 1.3, FRONT_Z + 0.3))
    for cx in OLED_X:
        wy = OLED_CY + OLED_WIN_DY
        ox0, ox1 = cx - OLED_WIN_W / 2, cx + OLED_WIN_W / 2
        oy0, oy1 = wy - OLED_WIN_H / 2, wy + OLED_WIN_H / 2
        cut.append(rr(ox0, ox1, oy0, oy1, 0.5, -1.0, FRONT_Z + 1.0))
        cut.append(rr_loft(ox0, ox1, oy0, oy1, 0.5, FRONT_Z - OLED_WIN_CH, OLED_WIN_CH + 0.3, FRONT_Z + 0.3))
        # header solder joints poke out of the OLED front above the strip: a little extra room
        cut.append(box(cx - 5.3, cx + 5.3, OLED_CY + OLED_STRIP_DY[1] + 0.05, OLED_TOP + 0.15, -0.5, FRONT_T - L(2)))
    for x, y, kind in TUB_SCREWS:
        cut.append(cyl(x, y, CLEAR_R, RIM_Z - 1.0, HEAD_SEAT_Z + SCREW_LEN + 1.5))
        cut.append(cyl(x, y, RIM_POCKET_R, RIM_Z - 1.0, RIM_POCKET_Z))          # takes the cover boss
        dy = -(y - IN_TOP + 3.5) if kind == "top" else (CHIN_BLOCK_TOP - y + 1.0)
        cut.append(hex_slot(x, y, POCKET_AF, TUB_NUT_Z[0], TUB_NUT_Z[1], dy))
    for x in BUTTONS.values():                                              # paper-clip holes, top wall
        cut.append(Pos(x, (IN_TOP + OUT_TOP) / 2, BTN_Z) * Rot(90, 0, 0) * Cylinder(1.1, TOP_WALL + 4.0))
    cut.append(side_usb_slot(opened=False))
    return body.cut(*cut)


def side_usb_slot(opened=True):
    """Rounded slot through the left wall (opened=True: right through, for checks / an open hole)."""
    y0, y1, z0, z1 = SIDE_USB_SLOT
    xs = -OUT_X - 1.0 if (opened or SIDE_USB_SKIN <= 0) else -OUT_X + SIDE_USB_SKIN
    x1 = -IN_X + 1.0
    pl = Plane(origin=(xs, (y0 + y1) / 2, (z0 + z1) / 2), x_dir=(0, 1, 0), z_dir=(1, 0, 0))
    return extrude(pl * RectangleRounded(y1 - y0, z1 - z0, (z1 - z0) / 2 - 0.01), amount=x1 - xs)


# ------------------------------------------------------------------------- back cover
def oled_post_xy():
    return [(cx + sx * OLED_HOLE_DX, OLED_CY + sy * OLED_HOLE_DY) for cx in OLED_X for sx in (-1, 1)
            for sy in (-1, 1)]


def make_back_cover():
    ch = COVER_CH
    plate = rr(-OUT_X, OUT_X, OUT_BOT, OUT_TOP, OUT_R, COVER_OUT_Z + ch, RIM_Z)
    plate = plate + rr_loft(-OUT_X, OUT_X, OUT_BOT, OUT_TOP, OUT_R, COVER_OUT_Z + ch, -ch, COVER_OUT_Z)

    lo = IN_X - LIP_C
    lip = rr(-lo, lo, IN_BOT + LIP_C, IN_TOP - LIP_C, IN_R - LIP_C, RIM_Z - 0.2, RIM_Z + LIP_H)
    li = lo - LIP_T
    lip = lip - rr(-li, li, IN_BOT + LIP_C + LIP_T, IN_TOP - LIP_C - LIP_T, IN_R - LIP_C - LIP_T,
                   RIM_Z - 1.0, RIM_Z + LIP_H + 1.0)
    for x, y, kind in TUB_SCREWS:                                           # clear the chin bosses
        if kind == "top":
            continue
        zz = (RIM_Z - 1.0, RIM_Z + LIP_H + 1.0)
        if kind == "chin":
            lip = lip - box(x - CHIN_BLOCK_W / 2 - 0.4, x + CHIN_BLOCK_W / 2 + 0.4, CHIN_BLOCK_TOP + 0.4,
                            OUT_BOT - 1.0, *zz)
        else:
            s = 1 if x > 0 else -1
            lip = lip - cyl(x, y, 5.5, *zz)
            lip = lip - box(x - 5.5, x + 5.5, y, OUT_BOT - 1.0, *zz)
            lip = lip - box(x, s * (OUT_X + 1.0), y - 4.5, y + 4.5, *zz)

    add = [lip]
    for x, y in STANDOFFS:
        top = COVER_BOSS_TOP if y > 0 else BRK_Z0                           # lower two carry the TCA bracket
        add.append(cyl(x, y, 4.5, RIM_Z - 0.2, top))
    for x, y, _ in TUB_SCREWS:                                              # backs the head counterbores
        add.append(cyl(x, y, RIM_BOSS_R, RIM_Z - 0.2, RIM_BOSS_TOP))
    for x, y in STAND_SCREWS:
        add.append(cyl(x, y, 5.0, RIM_Z - 0.2, RIM_Z + 1.4))
    for x, y in oled_post_xy():                                             # posts keep the OLEDs on their pins
        add.append(cyl(x, y, OLED_POST_R, RIM_Z - 0.2, OLED_POST_Z))
    for x, y in C3_POSTS:                                                   # keep the C3 SuperMini in its pocket
        add.append(cyl(x, y, C3_POST_R, RIM_Z - 0.2, C3_POST_TIP))
    cover = plate.fuse(*add)

    cut = []
    for x, y in oled_post_xy():                                             # pocket for the pin tip
        cut.append(cyl(x, y, OLED_POST_RECESS[0], OLED_POST_Z - OLED_POST_RECESS[1], OLED_POST_Z + 1.0))
    for x, y in STANDOFFS:
        cut.append(cyl(x, y, CLEAR_R, COVER_OUT_Z - 1.0, COVER_BOSS_TOP + 1.0))
        cut.append(cyl(x, y, HEAD_CB_R, COVER_OUT_Z - 1.0, STANDOFF_CB_FLOOR))
    for x, y, _ in TUB_SCREWS:
        cut.append(cyl(x, y, CLEAR_R, COVER_OUT_Z - 1.0, RIM_BOSS_TOP + 1.0))
        cut.append(cyl(x, y, HEAD_CB_R, COVER_OUT_Z - 1.0, HEAD_SEAT_Z))
    for x, y in STAND_SCREWS:
        cut.append(cyl(x, y, CLEAR_R, COVER_OUT_Z - 1.0, RIM_Z + 2.0))
    cx0, cx1, cy0, cy1 = CABLE_HOLE                                         # cable pass-through to the stand
    cut.append(rr(cx0, cx1, cy0, cy1, 5.0, COVER_OUT_Z - 1.0, RIM_Z + 1.0))
    tw, th = TCA_SIZE[0] / 2 + 3.0, TCA_SIZE[1] / 2 + 3.0     # room for the TCA screw heads and wires
    cut.append(box(TCA_C[0] - tw, TCA_C[0] + tw, TCA_C[1] - th, TCA_C[1] + th, TCA_RELIEF, RIM_Z + 0.5))
    for x in VENT_X:
        cut.append(rr(x - 1.25, x + 1.25, VENT_Y[0], VENT_Y[1], 1.2, COVER_OUT_Z - 1.0, RIM_Z + 1.0))
    for x in LOW_VENT_X:
        cut.append(rr(x - 1.25, x + 1.25, LOW_VENT_Y[0], LOW_VENT_Y[1], 1.2, COVER_OUT_Z - 1.0, RIM_Z + 1.0))
    x0, x1, y0, y1 = I2C_HDR                                                # room for the HY2.0 plug wires
    cut.append(box(x0 - 3.4, x1 + 3.6, y0 - 3.6, y1 + 3.4, COVER_OUT_Z + L(3), RIM_Z + 0.5))
    return cover.cut(*cut)


# ------------------------------------------------------------------------- TCA bracket
def lower_standoffs():
    return sorted((x, y) for x, y in STANDOFFS if y < 0)


def make_tca_bracket():
    """Bar between the two lower module standoffs with a ring for the TCA9548A and a pocket for the
    ESP32-C3 SuperMini, both on the back face.  Prints back face down: towers with cups for the standoff
    ends, a rib along each arm, TCA nut pockets open on the top (module) face, the TCA window and the C3
    pocket open on the bed face."""
    (xl, ys), (xr, _) = lower_standoffs()
    z1 = BRK_Z0 + BRK_T
    hw, py0, py1 = BRK_PAD
    cx, cy = TCA_C
    add = [box(cx - hw, cx + hw, py0, py1, BRK_Z0, z1),                                   # ring round the TCA
           box(xl, xr, ys - BRK_ARM_W / 2, ys + BRK_ARM_W / 2, BRK_Z0, z1),                  # arm
           box(cx - hw, cx + hw, C3_WALL_Y - 0.1, C3_WALL_Y + 4.6, BRK_Z0, z1)]              # ring -> C3 pad,
    #                                                                       below the C3's USB-C plug
    for xa, xb in ((xl, cx - hw), (cx + hw, xr)):                                          # ribs
        add.append(box(xa, xb, ys - BRK_RIB_W / 2, ys + BRK_RIB_W / 2, BRK_Z0, BRK_CUP_FLOOR))
    for x in (xl, xr):
        add.append(cyl(x, ys, BRK_TOWER_R, BRK_Z0, BRK_TOP))
    for dx in (-TCA_PITCH / 2, TCA_PITCH / 2):
        add.append(cyl(cx + dx, cy, TCA_SEAT_R, BRK_Z0, TCA_BOSS_TOP))
    c3 = c3_pocket()
    add.append(c3["pad"])
    body = add[0].fuse(*add[1:])
    cut = []
    for x in (xl, xr):
        cut.append(cyl(x, ys, BRK_CUP_R, BRK_CUP_FLOOR, BRK_TOP + 1.0))                     # cup
        cut.append(cyl(x, ys, CLEAR_R, BRK_Z0 - 1.0, BRK_TOP + 1.0))                        # cover screw
    # window for the TCA's parts and header joints: everything under the board except the two seats
    w2, h2 = TCA_SIZE[0] / 2 + 0.5, TCA_SIZE[1] / 2 + 0.5
    win = box(cx - w2, cx + w2, cy - h2, min(cy + h2, C3_WALL_Y), BRK_Z0 - 1.0, TCA_BOSS_TOP + 1.0)
    seats = []
    for s in (-1, 1):
        hx = cx + s * TCA_PITCH / 2
        seats += [cyl(hx, cy, TCA_SEAT_R, BRK_Z0 - 2.0, TCA_BOSS_TOP + 2.0),
                  box(hx, cx + s * (w2 + 1.0), cy - TCA_SEAT_HW, cy + TCA_SEAT_HW, BRK_Z0 - 2.0, TCA_BOSS_TOP + 2.0)]
    cut.append(win.cut(*seats))
    for dx in (-TCA_PITCH / 2, TCA_PITCH / 2):
        cut.append(cyl(cx + dx, cy, CLEAR_R, BRK_Z0 - 1.0, TCA_BOSS_TOP + 1.0))
        cut.append(hex_prism(cx + dx, cy, POCKET_AF, TCA_NUT_FLOOR, TCA_BOSS_TOP + 1.0))   # nut, module side
    cut += c3["cut"]
    return body.cut(*cut).fuse(*c3["ledges"])


def c3_pocket():
    """ESP32-C3 SuperMini pocket in the bracket back face (open on the bed side when printing), built in
    the pocket frame (X across the board, Y along it from the antenna-end wall face) and placed by
    c3_frame().  pad: thicker plate round it.  cut: pocket (PCB + clearance), side relief along the pin
    rows, slot for the USB-C plug body (through the pad, so a plug always seats wherever the board sits in
    its pocket; the PCB end rests on the stops either side of it).  ledges: under the antenna-end corners."""
    hw, ls = C3_HW, C3_POCKET_L
    r0, r1 = C3_LEDGE_L, ls - 0.5                            # pin rows: room for castellation solder
    z0 = BRK_Z0 - 1.0
    pad = box(-hw - C3_SIDE, hw + C3_SIDE, C3_PAD_L[0], C3_PAD_L[1], BRK_Z0, C3_PAD_TOP)
    cut = [box(-hw, hw, 0.0, ls, z0, C3_FLOOR_Z),
           box(-hw - 1.05, hw + 1.05, r0, r1, z0, BRK_Z0 + L(5)),                  # 0.7 past the PCB top
           box(-C3_PLUG_HW, C3_PLUG_HW, ls, C3_PAD_L[1] + 1.0, z0, C3_PAD_TOP + 1.0),
           box(-hw, hw, r0, ls - C3_BAR_W, z0, C3_PAD_TOP + 1.0)]                 # open floor
    ledges = [box(s * C3_ANT_HW, s * (hw + 0.5), -0.5, r0, C3_LEDGE_Z, C3_FLOOR_Z + 0.5)
              for s in (-1, 1)]
    f = c3_frame()
    return dict(pad=f * pad, cut=[f * c for c in cut], ledges=[f * b for b in ledges], relief=(r0, r1))


# ------------------------------------------------------------------------------ stand
def _isect(p, d, q, e):
    den = d[0] * e[1] - d[1] * e[0]
    t = ((q[0] - p[0]) * e[1] - (q[1] - p[1]) * e[0]) / den
    return (p[0] + t * d[0], p[1] + t * d[1])


def stand_profile():
    """Side profile of the rear leg in world (y, z).  Nothing of it is in front of the case."""
    y_desk = Y_DESK
    F = c2w(OUT_BOT, FRONT_Z)                           # front-bottom edge (floats above the desk)
    A = c2w(*CONTACT)                                   # back-bottom edge, on the desk
    yk = (y_desk - COVER_OUT_Z * math.sin(_T)) / math.cos(_T)
    K = (y_desk, c2w(yk, COVER_OUT_Z)[1])               # where the back face meets the desk
    S3 = c2w(PLATE_TOP_Y, COVER_OUT_Z)
    S4 = c2w(PLATE_TOP_Y, COVER_OUT_Z - PLATE_T)
    base_top = y_desk + BASE_T
    B2, C = (y_desk, STAND_Z_BACK), (base_top, STAND_Z_BACK)
    outer = [K, S3, S4, C, B2]

    dP = (K[0] - S3[0], K[1] - S3[1])                   # plate back face, parallel to the cover
    dB = (C[0] - S4[0], C[1] - S4[1])                   # brace outer face
    n = (-dB[1], dB[0]); nl = math.hypot(*n); n = (n[0] / nl, n[1] / nl)
    if (K[0] - S4[0]) * n[0] + (K[1] - S4[1]) * n[1] < 0:
        n = (-n[0], -n[1])
    Bi = (S4[0] + n[0] * BRACE_T, S4[1] + n[1] * BRACE_T)
    V1 = _isect(S4, dP, (base_top, 0.0), (0.0, 1.0))
    V2 = _isect(S4, dP, Bi, dB)
    V3 = _isect((base_top, 0.0), (0.0, 1.0), Bi, dB)
    return dict(outer=outer, window=[V1, V2, V3], y_desk=y_desk, F=F, A=A, K=K, S3=S3, S4=S4, C=C,
                n_brace=n, z_rear=STAND_Z_BACK)


def make_stand():
    p = stand_profile()
    yd = p["y_desk"]
    face = Plane.YZ * Polygon(*p["outer"], align=None)
    win = Plane.YZ * Polygon(*p["window"], align=None)
    win = offset(offset(win, -4.0, kind=Kind.ARC), 4.0, kind=Kind.ARC)
    v1, v2, v3 = p["window"]                                                # keep the apex sharp: no
    tip = [v2, ((v1[0] + v2[0]) / 2, (v1[1] + v2[1]) / 2), ((v3[0] + v2[0]) / 2, (v3[1] + v2[1]) / 2)]
    win = win + Plane.YZ * Polygon(*tip, align=None)                        # flat ceiling when printed
    stand = extrude(face, amount=STAND_W / 2, both=True) - extrude(win, amount=STAND_W, both=True)

    cut = []
    for x, y in STAND_SCREWS:                                               # cover -> stand, nut in the tunnel
        cut.append(to_world(cyl(x, y, CLEAR_R, COVER_OUT_Z + 0.5, COVER_OUT_Z - PLATE_T - 1.0)))
        cut.append(to_world(hex_prism(x, y, POCKET_AF, PLATE_NUT_Z, COVER_OUT_Z - PLATE_T - 3.0)))  # flat on top
    cut.append(to_world(Pos(0, 0, COVER_OUT_Z - PLATE_T - 2.0)               # big cable opening in the plate
                        * extrude(Polygon(*PLATE_HOLE, align=None), PLATE_T + 2.5)))
    bx, bz0, bz1, bf = BASE_HOLE                                            # cables out through the bottom
    lc, hw = -(bz0 + bz1) / 2, (bz1 - bz0) / 2                              # local y = world -z
    x1 = bx - (hw - bf / 2)
    base_hole = [(-x1, lc - hw), (x1, lc - hw), (bx, lc - bf / 2), (bx, lc + bf / 2), (x1, lc + hw),
                 (-x1, lc + hw), (-bx, lc + bf / 2), (-bx, lc - bf / 2)]
    cut.append(w_up(Pos(0, 0, -1.0) * extrude(Polygon(*base_hole, align=None), BASE_T + 2.0), yd))
    for x, z in TIE_SLOTS:                                                  # cable-tie slot pairs
        for k in (-1, 1):
            zc = z + k * TIE_PAIR / 2
            cut.append(w_up(box(x - TIE_SLOT[0] / 2, x + TIE_SLOT[0] / 2, -zc - TIE_SLOT[1] / 2,
                                -zc + TIE_SLOT[1] / 2, -1.0, BASE_T + 1.0), yd))
    # round window in the brace.  The brace is 55 deg from the desk, so a bore square to it would leave
    # a 55 deg ceiling; the bore is tipped up to BRACE_HOLE_AXIS instead (no bore face beyond 42 deg
    # when printed base-down) and its section is shortened along the slope by cos(tip), so the opening
    # on the outer face is a true circle.
    c, s4, n = p["C"], p["S4"], p["n_brace"]
    lb = math.hypot(s4[0] - c[0], s4[1] - c[1])
    du = ((s4[0] - c[0]) / lb, (s4[1] - c[1]) / lb)                         # up the brace (world y, z)
    r, vc = BRACE_HOLE
    ctr = (c[0] + du[0] * vc, c[1] + du[1] * vc)                            # centre on the outer face
    out_n = (-n[0], -n[1])                                                  # outward: up and back
    el = math.radians(BRACE_HOLE_AXIS)
    ax = (math.sin(el), math.copysign(math.cos(el), out_n[1]))              # bore axis, outward
    tip = math.acos(ax[0] * out_n[0] + ax[1] * out_n[1])
    over = r * math.tan(tip) + 3.0                                          # tipped bore: run it long ...
    pl = Plane(origin=(0.0, ctr[0] + ax[0] * over, ctr[1] + ax[1] * over), x_dir=(1, 0, 0),
               z_dir=(0, -ax[0], -ax[1]))
    bore = extrude(pl * Ellipse(r, r * math.cos(tip)), amount=BRACE_T / math.cos(tip) + 2.0 * over)
    slab = Plane(origin=(0.0, c[0] - n[0], c[1] - n[1]), x_dir=(1, 0, 0), z_dir=(0, n[0], n[1]))
    slab = extrude(slab * Rectangle(STAND_W + 2.0, 2.0 * lb + 20.0), amount=BRACE_T + 2.0)
    cut.append(bore & slab)                                                 # ... and keep it to the brace
    p.update(brace_hole_centre=ctr, brace_hole_axis=ax, brace_hole_tip_deg=math.degrees(tip))
    return stand.cut(*cut), p

# ------------------------------------------------------------------------- fasteners
def fasteners(nut_t=NUT_T_MAX):
    """Every joint: seated nut, its way in or out of the pocket, and the screw body.
    'frame' says whether the solids are in the case frame or the world frame."""
    out = []
    for x, y, kind in TUB_SCREWS:
        z0 = TUB_NUT_Z[0]
        dy = -12.0 if kind == "top" else 12.0
        out.append(dict(group="front frame <- back cover", part="front", frame="case",
                        nut=hex_prism(x, y, NUT_AF, z0, z0 + nut_t),
                        path=hex_slot(x, y, NUT_AF, z0, z0 + nut_t, dy),
                        screw=cyl(x, y, SCREW_R, HEAD_SEAT_Z, HEAD_SEAT_Z + SCREW_LEN)))
    for dx in (-TCA_PITCH / 2, TCA_PITCH / 2):
        x, y = TCA_C[0] + dx, TCA_C[1]
        out.append(dict(group="TCA9548A -> TCA bracket", part="bracket", frame="case",
                        nut=hex_prism(x, y, NUT_AF, TCA_NUT_FLOOR, TCA_NUT_FLOOR + nut_t),
                        path=hex_prism(x, y, NUT_AF, TCA_NUT_FLOOR, TCA_NUT_FLOOR + nut_t + 10.0),
                        screw=cyl(x, y, SCREW_R, TCA_HEAD_Z, TCA_HEAD_Z + SCREW_LEN)))
    for x, y in STANDOFFS:
        out.append(dict(group="back cover -> 7in module standoffs", part="cover", frame="case",
                        nut=None, path=None,
                        screw=cyl(x, y, SCREW_R, STANDOFF_CB_FLOOR, STANDOFF_CB_FLOOR + SCREW_LEN)))
    for x, y in STAND_SCREWS:
        head = RIM_Z + 1.4
        out.append(dict(group="back cover -> stand", part="stand", frame="world",
                        nut=to_world(hex_prism(x, y, NUT_AF, PLATE_NUT_Z, PLATE_NUT_Z - nut_t)),
                        path=to_world(hex_prism(x, y, NUT_AF, PLATE_NUT_Z, PLATE_NUT_Z - nut_t - 12.0)),
                        screw=to_world(cyl(x, y, SCREW_R, head, head - SCREW_LEN))))
    return out


# ------------------------------------------------------------------------- build all
def build():
    front = make_front_frame()
    cover = make_back_cover()
    stand_w, prof = make_stand()
    return dict(front=front, cover=cover, bracket=make_tca_bracket(), stand_world=stand_w, profile=prof)


def print_oriented(parts):
    """Rotate each part into its print orientation with the bed at Z = 0."""
    def drop(s):
        bb = s.bounding_box()
        return Pos(-(bb.min.X + bb.max.X) / 2, -(bb.min.Y + bb.max.Y) / 2, -bb.min.Z) * s
    return {
        "01_front_frame": drop(Rot(0, 180, 0) * parts["front"]),          # front face on the bed
        "02_back_cover": drop(parts["cover"]),                            # outer face on the bed
        "03_stand": drop(Rot(0, 90, 0) * parts["stand_world"]),           # on its side (triangle on the bed)
        "04_tca_bracket": drop(parts["bracket"]),                         # back face on the bed, cups up
    }


def main():
    OUT.mkdir(exist_ok=True)
    (OUT / "stl").mkdir(exist_ok=True)
    (OUT / "step").mkdir(exist_ok=True)
    parts = build()
    pr = print_oriented(parts)
    info = {}
    for name, s in pr.items():
        bb = s.bounding_box()
        export_stl(s, str(OUT / "stl" / f"{name}.stl"), tolerance=0.01, angular_tolerance=0.1)
        export_step(s, str(OUT / "step" / f"{name}.step"))
        info[name] = dict(size=[round(bb.size.X, 2), round(bb.size.Y, 2), round(bb.size.Z, 2)],
                          volume_cm3=round(s.volume / 1000, 1))
    y_desk = parts["profile"]["y_desk"]
    lift = Pos(0, -y_desk, 0)
    asm = Compound(children=[lift * to_world(parts["front"]), lift * to_world(parts["cover"]),
                             lift * to_world(parts["bracket"]), lift * parts["stand_world"]])
    export_step(asm, str(OUT / "step" / "00_assembly_on_desk.step"))
    bb = asm.bounding_box()
    info["assembly_on_desk"] = dict(width=round(bb.size.X, 1), height=round(bb.max.Y, 1),
                                    depth=round(bb.size.Z, 1), tilt_deg=TILT)
    info["case_outer"] = dict(w=round(2 * OUT_X, 2), h=round(OUT_TOP - OUT_BOT, 2),
                              d=round(FRONT_Z - COVER_OUT_Z, 2))
    info["oled_centres_x"] = [round(x, 3) for x in OLED_X]
    (OUT / "build_info.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()
