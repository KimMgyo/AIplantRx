#!/usr/bin/env python3
"""
Sensor node case v2 (build123d 0.13): bottom plate + side frame, closed by the v1 lid (already printed).

Why v2: v1 was one box with boards resting on seats.  Printed, nearly nothing held: turned over, boards fell
out; locating pins were 0.4 under the holes; the RS485 pocket was too narrow, the buck seats too loose, and the
SCD41 chamber had no room for its header.  Dimensions are re-checked in research/parts_research.md section I.

Parts
  plate  sensor face + every board mount (printed face down, all white PLA: the 0.9 skin under the ring is the
         diffuser; no filament change)
  frame  the four walls, corner pillars, KF301 terminal pockets, USB-C / terminal windows, vents, snap grooves
  lid    v1 make_lid(), unchanged (already printed)

Fasteners: 8 x M3 nut + 8 x M3 x 8 bolt.
  plate -> frame: bolts from the sensor face (counterbored, heads flush) through the plate's corner pads into
                  nuts slid into slots low in the frame pillars.  The pads sit in pockets under the pillars and
                  register the frame on the plate.
  lid -> frame:   as v1 (nuts high in the pillars).  Installing under the PC case top: M3 x 12 there instead.

Board retention (no small screws on hand): every board is held so the box can be turned over with the lid off.
  - boards with holes (MLX90640, SCD41, LR7843): pins 0.15 under the hole (v1: 0.4) on the diagonal pair
  - every board: snap hooks over the PCB edge at component-free spots, arms 2 lines thick that flex outward
    while the board is pressed in; 45 deg lead-in on top, flat catch 0.15 above the PCB
  - the ring: on the diffuser skin in its pocket; 3 dabs of hot glue (no room for a hook between the walls)
"""
from __future__ import annotations

import json
import math

from build123d import (Compound, Plane, Polygon, Pos, RectangleRounded, Rot, export_step, export_stl, extrude)

import build_node as b
from build_node import (CLR, FLOOR, H, HOLE_PLUS, IN, LINE2, LINE4, LID_PAD, L, M3_CB, M3_HOLE, NUT_Z, PILLAR,
                        R_CORNER, SIZE, box, cyl, ring_solid)

OUT2 = b.OUT / "v2"

# ---------------------------------------------------------------- v1 placements changed for v2
# (build_node's globals are patched, so b.dummies() / b.lr_boxes() / b.cam_box() follow)
b.RS_X0 = 50.4                    # MAX485 moved 2.4 in from the +X wall: room for its edge hooks
b.BUCK_C = {"ring": (40.5, 11.0), "cam": (40.5, -13.0)}   # 2 toward -X, clear of the MAX485 hooks
b.BUCK_SEAT = L(26)               # 9.30: long enough hook arms (v1 6.85)
b.LR_SEAT_Z = L(28)               # 10.00 (v1 7.20): hook arms, and 7.3 under the PCB for the 12V wires
b.LR_Y1 = IN - 0.6                # signal end 0.6 off the wall (the frame is a separate part now)
b.LR_Y0 = b.LR_Y1 - b.LR[1]
b.PERF_SEAT = L(28)               # 10.00
b.PERF = (25.4, 40.0)             # user's 4 x 6 cm blue double-sided board (AliExpress order 2026-06-27; FR4 1.6,
                                  # 2.54 pitch, 14 x 20 holes): keep the 40 mm factory edge, one cut across at ~25.4
b.PERF_C = (-62.4 + 12.7, 14.8)   # factory long edge on the -X side, the cut edge toward the ring

MLX_PIN = 2.5 - 0.15              # Adafruit 4407: holes d2.5, 2.54 from the edges (fab print + STEP)
SCD_PIN = 3.0 - 0.15              # Adafruit 5190: holes d3.0, 2.54 from the edges
LR_PIN = 2.0                      # HW-532 holes ~d2.4 (user measured, approximate); user chose a 2.0 pin
LR_HOLE_DX = 6.1                  # 12.2 apart (user measured 12.2)
CAM_CLR = 0.4                     # cradle clearance per side (v1 0.25 was too tight)
# Camera recess and lens hole from bkgoodman's ESP32-CAM case (Printables 75024, ~14.5k downloads; measured in
# its STEP 2026-11-18): holder recess 10.0 x 10.3 (the 8.5 holder floats +-0.75 / +-0.9), lens hole d9.5.
# v1/v2 had a 9.2 collar and a d7.9 hole.  The recess keeps +-0.5 extra along the board for the hand-taped
# camera's position (10.6 .. 11.5 from the SD end).
CAM_RECESS = (10.0, 10.3)
b.CAM_LENS_SLACK = 0.5
b.CAM_LENS_D = 9.5                # lens hole: cut at 9.5 + HOLE_PLUS (0.3), prints ~9.5
RS_W, RS_L = 14.3, 44.0           # C25B module: width measured 14.3 (seller image 14.2), length 44
RS_TOL = 0.3                      # per side
BUCK_W, BUCK_L, BUCK_T = 12.0, 17.5, 1.6   # CN3903-type 5 V buck, 17.5 x 12 (listing + Alibaba), PCB 1.6 (measured)
BUCK_CLR = 0.2

# ---------------------------------------------------------------- plate <-> frame joint
PAD = 8.5                         # corner pad on the plate, centred on the fan-hole bolt axis
PAD_H = 2.75
PAD_TOP = FLOOR + PAD_H           # 5.40
POCKET_CLR = 0.25
CB_D, CB_DEPTH = M3_CB            # counterbore from the sensor face: d7.1 x 3.35 (head flush)
LOW_NUT_Z0 = PAD_TOP + POCKET_CLR + 2.0    # 7.65: 2.0 of pillar under the nut takes the bolt's pull


def hook(ex, ey, n, z_top, w=4.0, catch=0.5, gap=0.25, base=FLOOR - 0.01):
    """Snap hook on a PCB edge.  (ex, ey): a point on the edge line; n: outward normal '+x' '-x' '+y' '-y';
    z_top: PCB top.  The arm stands `gap` outside the edge, the catch reaches `catch` over the PCB top."""
    t = LINE2
    zc = z_top + 0.15
    lead = catch + gap
    top = zc + 0.35 + lead
    arm = box(-w / 2, w / 2, gap, gap + t, base, top)
    prof = Polygon((gap + 0.01, zc), (-catch, zc), (-catch, zc + 0.35), (gap + 0.01, zc + 0.35 + lead), align=None)
    lip = extrude(Plane.YZ * prof, amount=w / 2, both=True)      # centred on the arm (+-w/2 along the edge)
    rot = {"+y": 0.0, "-y": 180.0, "+x": -90.0, "-x": 90.0}[n]
    return Pos(ex, ey, 0) * Rot(0, 0, rot) * (arm + lip)


def bolt_axes():
    return [(sx, sy, sx * b.FAN_HOLE, sy * b.FAN_HOLE) for sx in (-1, 1) for sy in (-1, 1)]


# ---------------------------------------------------------------- frame
def kf_pockets(add, cut, labels):
    """v1 KF301 pockets behind the -Y wall (they fit: unchanged), windows, wire exits, labels."""
    for name, n, pins, x0, x1 in b.kf_row():
        xc = (x0 + x1) / 2
        yb = -IN + b.KF_D + 0.3
        add.append(box(x0 - LINE4, x1 + LINE4, -IN - 0.01, yb + LINE4, FLOOR, b.KF_SEAT + 7.0)
                   - box(x0, x1, -IN - 0.1, yb, FLOOR - 0.1, b.KF_SEAT + 7.1))
        for xa, xb in ((x0 - 0.01, x0 + 1.3), (x1 - 1.3, x1 + 0.01)):
            add.append(box(xa, xb, -IN - 0.01, yb + 0.01, FLOOR, b.KF_SEAT))
        for k in range(max(1, n // 2)):
            ex = xc + (k - (max(1, n // 2) - 1) / 2) * 10.0
            cut.append(box(ex - 3.5, ex + 3.5, yb - 0.1, yb + LINE4 + 0.1, FLOOR - 0.1, b.KF_SEAT - 1.0))
        for k in range(n):
            px = xc - (n - 1) * b.KF_P / 2 + k * b.KF_P
            hw = (b.KF_P - 1.85) / 2
            wz0, wz1 = b.KF_SEAT + b.KF_MOUTH - 2.6, b.KF_SEAT + b.KF_MOUTH + 2.0
            cut.append(box(px - hw, px + hw, -SIZE / 2 - 0.1, -IN + 0.1, wz0, wz1)
                       + Pos(px, -IN - b.WALL / 2, wz1) * Rot(0, 45, 0) * b.Box(hw * 1.4142, b.WALL + 0.4, hw * 1.4142))
            labels.append((pins.split()[k], px, b.KF_SEAT - 2.6))
        labels.append((name, xc, b.KF_SEAT + b.KF_MOUTH + 6.2))


def make_frame():
    outer = Pos(0, 0, FLOOR) * extrude(RectangleRounded(SIZE, SIZE, R_CORNER), amount=H - FLOOR)
    inner = Pos(0, 0, FLOOR - 1) * extrude(RectangleRounded(2 * IN, 2 * IN, R_CORNER - b.WALL), amount=H + 2)
    frame = outer - inner
    add, cut, labels = [], [], []
    for sx, sy, hx, hy in bolt_axes():
        add.append(box(sx * IN, sx * (IN - PILLAR), sy * IN, sy * (IN - PILLAR), FLOOR, H - LID_PAD))
        p = PAD / 2 + POCKET_CLR
        cut.append(box(hx - p, hx + p, hy - p, hy + p, FLOOR - 0.1, PAD_TOP + POCKET_CLR))    # pad pocket
        cut.append(cyl(hx, hy, M3_HOLE / 2, FLOOR - 0.1, LOW_NUT_Z0 + 2.8 + 6.0))            # low bolt
        # low nut slots open along Y toward the inside: along X the (+X,-Y) one faced the 485 terminal pocket
        # (2.6 left in front of it, a nut needs 5.5+)
        cut.append(Pos(hx, hy, LOW_NUT_Z0) * Rot(0, 0, 90.0 if sy < 0 else -90.0) * b.nut_slot())
        cut.append(cyl(hx, hy, M3_HOLE / 2, NUT_Z[0] - 4.0, H + 0.1))                         # lid bolt
        cut.append(Pos(hx, hy, NUT_Z[0]) * Rot(0, 0, 180.0 if sx > 0 else 0.0) * b.nut_slot())
    for k in range(4):
        cut.append(Rot(0, 0, 90 * k) * b.snap_bar(IN, b.SNAP_GROOVE, b.SNAP_LEN + 2.0))
    # USB-C opening, centred on the devkit receptacle (devkit stays where v1 put it: it fitted)
    dcx = b.DEV_X0 + b.DEV[0] / 2
    usb_z = b.DEV_SEAT_Z + 1.6 + b.DEV_USB_H / 2
    cut.append(b.rounded_hole_y(dcx, usb_z, *b.USB_CUT, b.USB_R, -SIZE / 2 - 0.1, -IN + 0.1))
    labels.append(("USB", dcx, usb_z + b.USB_CUT[1] / 2 + 3.2))
    kf_pockets(add, cut, labels)
    for i in range(-5, 6):
        cut.append(box(i * 9.0 - 1.5, i * 9.0 + 1.5, IN - 0.1, SIZE / 2 + 0.1, H - 14.0, H - 4.0))
    for a in add:
        frame = frame + a
    for c in cut:
        frame = frame - c
    frame = frame - b.engrave(labels)
    return frame & outer


def frame_keepout():
    """Volume the frame occupies above the floor, grown by 0.3: plate features must stay out of it."""
    z0, z1 = FLOOR, H + 1
    k = box(-80, 80, -80, 80, z0, z1) - box(-(IN - 0.3), IN - 0.3, -(IN - 0.3), IN - 0.3, z0 - 1, z1 + 1)
    for sx, sy, _, _ in bolt_axes():
        k = k + box(sx * (IN + 1), sx * (IN - PILLAR - 0.3), sy * (IN + 1), sy * (IN - PILLAR - 0.3), z0, z1)
    for _, _, _, x0, x1 in b.kf_row():
        yb = -IN + b.KF_D + 0.3
        k = k + box(x0 - LINE4 - 0.3, x1 + LINE4 + 0.3, -IN - 1, yb + LINE4 + 0.3, z0, z1)
    return k


# ---------------------------------------------------------------- plate
def plate_features():
    add, cut = [], []
    C = b.C
    # ring pocket (v1): walls, open toward the inside down to the diffuser skin
    p0, p1 = b.RING_POCKET
    add.append(ring_solid(*C, p1 + LINE4, p1, FLOOR - 0.01, b.RING_SUPPORT_Z))
    add.append(ring_solid(*C, p0, p0 - LINE4, FLOOR - 0.01, b.RING_SUPPORT_Z))        # v1: +1.2, now room for
                                                                                     # the MLX +X hook to flex
    cut.append(ring_solid(*C, p1, p0, b.DIFF_T, b.RING_SUPPORT_Z + 0.1))

    # MLX90640: can hole, 4 standoffs, 2 pins (d2.35), hooks on both long edges (component-free middle)
    mx, my = b.MLX_C
    cut.append(cyl(mx, my, (b.MLX_CAN_D + 0.5 + HOLE_PLUS) / 2, -0.1, FLOOR + 0.2))
    add += b.standoffs(mx, my, *b.MLX_HOLES, b.MLX_PCB_Z, 4.6)
    add += b.locate_pins(mx, my, *b.MLX_HOLES, b.MLX_PCB_Z, MLX_PIN)
    zt = b.MLX_PCB_Z + 1.6
    add.append(hook(mx - b.MLX_BOARD[0] / 2, my, "-x", zt))
    add.append(hook(mx + b.MLX_BOARD[0] / 2, my, "+x", zt))

    # ESP32-CAM: lens slot, holder pad + collar (v1), cradle 0.4, hooks at the SD end and on the +X edge
    # (its -X edge lies over the ring: no hook can stand there)
    cx, cy = b.CAM_C
    sl = b.CAM_LENS_SLACK
    r = (b.CAM_LENS_D + HOLE_PLUS) / 2
    cut.append(cyl(cx, cy - sl, r, -0.1, b.CAM_HOLDER_Z + 0.1) + cyl(cx, cy + sl, r, -0.1, b.CAM_HOLDER_Z + 0.1)
               + box(cx - r, cx + r, cy - sl, cy + sl, -0.1, b.CAM_HOLDER_Z + 0.1))
    hbx, hby = CAM_RECESS[0] / 2, CAM_RECESS[1] / 2
    add.append(box(cx - hbx - LINE4, cx + hbx + LINE4, cy - hby - sl - LINE4, cy + hby + sl + LINE4, FLOOR - 0.01,
                   b.CAM_HOLDER_Z + L(5)))
    cut.append(box(cx - hbx, cx + hbx, cy - hby - sl, cy + hby + sl, b.CAM_HOLDER_Z, b.CAM_HOLDER_Z + L(5) + 0.1))
    x0, x1, y0, y1 = b.cam_box()
    add += b.cradle(x0, x1, y0, y1, b.CAM_SEAT_Z, clr=CAM_CLR, skip=(0, 2, 3))
    add.append(box(x0 + 0.5, x0 + 3.0, y1 - 3.0, y1 - 0.5, FLOOR - 0.01, b.CAM_SEAT_Z))   # seat, antenna-end -X corner
    zt = b.CAM_SEAT_Z + b.CAM_PCB_T
    add.append(hook(cx, y0, "-y", zt, gap=CAM_CLR))
    add.append(hook(cx - 6.0, y0, "-y", zt, gap=CAM_CLR))       # 2nd SD-end hook, inside the ring's inner wall
    add.append(hook(x1, y0 + 30.5, "+x", zt, gap=CAM_CLR))
    add.append(hook(x0, y1 - 3.0, "-x", zt, gap=CAM_CLR))       # -X edge just past the ring's outer wall

    # SCD41: the PCB itself closes the chamber (its header points up, away from the sensor): a 2-line wall
    # under the PCB edge, notched for the two STEMMA QT sockets, grille in the floor, standoffs + 2 pins (d2.85),
    # hooks in the middle of both long edges
    sx_, sy_ = b.SCD_C
    hx, hy = b.SCD_BOARD[0] / 2, b.SCD_BOARD[1] / 2
    e = 0.2                                     # wall 0.2 inside the PCB edge: 0.45 to the hook arms
    add.append(box(sx_ - hx + e, sx_ + hx - e, sy_ - hy + e, sy_ + hy - e, FLOOR - 0.01, b.SCD_PCB_Z)
               - box(sx_ - hx + e + LINE2, sx_ + hx - e - LINE2, sy_ - hy + e + LINE2, sy_ + hy - e - LINE2,
                     FLOOR - 0.1, b.SCD_PCB_Z + 0.1))
    for xa, xb in ((sx_ - hx - 0.1, sx_ - hx + 1.4), (sx_ + hx - 1.4, sx_ + hx + 0.1)):
        cut.append(box(xa, xb, sy_ - 3.3, sy_ + 3.3, b.SCD_PCB_Z - 3.2, b.SCD_PCB_Z + 0.1))     # STEMMA QT sockets
    for i in range(-3, 4):
        cut.append(box(sx_ + i * 3.0 - 0.8, sx_ + i * 3.0 + 0.8, sy_ - 7.0, sy_ + 7.0, -0.1, FLOOR + 0.1))
    add += b.standoffs(sx_, sy_, *b.SCD_HOLES, b.SCD_PCB_Z, 5.0)
    add += b.locate_pins(sx_, sy_, *b.SCD_HOLES, b.SCD_PCB_Z, SCD_PIN)
    zt = b.SCD_PCB_Z + 1.6
    add.append(hook(sx_, sy_ - hy, "-y", zt))
    add.append(hook(sx_, sy_ + hy, "+y", zt))

    # devkit (fitted in v1: same seats), hooks in the middle of both long edges (catch 0.4: the header
    # solder joints start 0.4 in) and one on the module's antenna end (module PCB 0.8 on top of the board)
    dx0, dx1, dy0, dy1 = b.DEV_X0, b.DEV_X0 + b.DEV[0], b.DEV_Y0, b.DEV_Y0 + b.DEV[1]
    dcx = (dx0 + dx1) / 2
    add += b.cradle(dx0, dx1, dy0, dy1, b.DEV_SEAT_Z, step=0, skip=(3,))
    add.append(box(dx1 + 0.25, dx1 + 3.0, dy0 + b.DEV_PCB_L - 24.0, dy0 + b.DEV_PCB_L - 18.0, FLOOR - 0.01,
                   b.DEV_SEAT_Z + L(8) - 0.2))
    add.append(box(dcx - 6.0, dcx + 6.0, dy0 + 7.0, dy0 + 12.0, FLOOR - 0.01, b.DEV_SEAT_Z))
    add.append(box(dx0 + 0.3, dx0 + 10.0, dy0 + b.DEV_PCB_L + 0.5, dy1 - 0.5, FLOOR - 0.01, b.DEV_SEAT_Z + 1.6))
    zt = b.DEV_SEAT_Z + 1.6
    add.append(hook(dx0, dy0 + 36.0, "-x", zt, catch=0.4))     # past the side guide (dy0 + 24 .. 30)
    add.append(hook(dx1, dy0 + 36.0, "+x", zt, catch=0.4))
    # only 0.4 of grip each (header joints), so more of them: a second pair, and one near the USB end on +X
    # (the -X side there is the corner pillar)
    add.append(hook(dx0, dy0 + 18.0, "-x", zt, catch=0.4))
    add.append(hook(dx1, dy0 + 18.0, "+x", zt, catch=0.4))
    add.append(hook(dx1, dy0 + 6.0, "+x", zt, catch=0.4))
    add.append(hook(dcx, dy1, "+y", zt + 0.8))

    # MAX485 (C25B): corner guides 0.4, side ledges (v1), hooks in the middle of both long edges that take
    # a board up to 15.0 wide (seller 14.2)
    rx0, rx1, ry0, ry1 = b.RS_X0, b.RS_X0 + RS_W, b.RS_Y0, b.RS_Y0 + RS_L
    add += b.cradle(rx0, rx1, ry0, ry1, b.RS_SEAT_Z, step=0, clr=RS_TOL)
    for xa, xb in ((rx0 + 0.3, rx0 + 0.3 + LINE4), (rx1 - 0.3 - LINE4, rx1 - 0.3)):   # inside the edge: the
        add.append(box(xa, xb, ry0 + 10.0, ry1 - 10.0, FLOOR - 0.01, b.RS_SEAT_Z))      # hook arms stay free
    zt = b.RS_SEAT_Z + 1.6
    add.append(hook(rx0, (ry0 + ry1) / 2, "-x", zt, w=6.0, gap=RS_TOL, catch=0.7))
    add.append(hook(rx1, (ry0 + ry1) / 2, "+x", zt, w=6.0, gap=RS_TOL, catch=0.7))

    # 3 x LR7843: corner cradles 0.4, pads under the terminal end, pads + pins (d1.7) under the two holes,
    # hooks in the middle of both long edges
    r_keep = b.RING_POCKET[1] + LINE4 + 0.5
    for x0, x1, y0, y1 in b.lr_boxes():
        corners = ((x0, y0), (x1, y0), (x0, y1), (x1, y1))
        skip = tuple(k for k, (xs, ys) in enumerate(corners) if math.hypot(xs, ys) - 4.3 < r_keep)
        add += b.cradle(x0, x1, y0, y1, b.LR_SEAT_Z, step=0.4, clr=0.4, skip=skip)
        cx_ = (x0 + x1) / 2
        for px in (cx_ - 2.5, cx_ + 2.5):
            add.append(box(px - 1.0, px + 1.0, y0 + 0.5, y0 + 5.0, FLOOR - 0.01, b.LR_SEAT_Z))
        for sx in (-1, 1):
            hx_, hy_ = cx_ + sx * LR_HOLE_DX, y1 - b.LR_HOLE[1]
            add.append(cyl(hx_, hy_, 2.3, FLOOR - 0.01, b.LR_SEAT_Z))
            add.append(cyl(hx_, hy_, LR_PIN / 2, b.LR_SEAT_Z - 0.01, b.LR_SEAT_Z + 1.6 + b.PIN_TIP))
        zt = b.LR_SEAT_Z + 1.6
        add.append(hook(x0, (y0 + y1) / 2, "-x", zt))
        add.append(hook(x1, (y0 + y1) / 2, "+x", zt))

    # perfboard 25.4 x 40 cut from the 4 x 6 cm board: the factory edges (-X long edge, both 25.4 ends) sit in
    # corner cradles (0.4); the hand-cut +X edge has no lips, only seat pads and a hook with a long catch, so
    # a cut anywhere from 24.6 to 26.4 wide is held
    px0, py0 = b.PERF_C[0] - b.PERF[0] / 2, b.PERF_C[1] - b.PERF[1] / 2
    px1, py1 = px0 + b.PERF[0], py0 + b.PERF[1]
    add += b.cradle(px0, px1, py0, py1, b.PERF_SEAT, clr=0.4, skip=(1, 3))
    for ya, yb in ((py0 + 0.5, py0 + 4.0), (py1 - 4.0, py1 - 0.5)):
        add.append(box(px1 - 0.9 - LINE2, px1 - 0.9, ya, yb, FLOOR - 0.01, b.PERF_SEAT))   # 2 lines, outer hole row
    zt = b.PERF_SEAT + 1.6
    add.append(hook(px0, b.PERF_C[1], "-x", zt, gap=0.4, catch=0.6))
    add.append(hook(px1, b.PERF_C[1], "+x", zt, gap=0.4, catch=1.2))

    # 2 x buck: pads under the middle of the long edges, end stops at the middle of the short edges,
    # hooks on the long edges (corners stay free for the soldered wires)
    for cx_, cy_ in b.BUCK_C.values():
        bx0, bx1 = cx_ - BUCK_W / 2, cx_ + BUCK_W / 2
        by0, by1 = cy_ - BUCK_L / 2, cy_ + BUCK_L / 2
        for xa, xb in ((bx0 + 0.4, bx0 + 0.4 + LINE2), (bx1 - 0.4 - LINE2, bx1 - 0.4)):   # 2 lines, 0.6 off the hooks
            add.append(box(xa, xb, cy_ - 4.5, cy_ + 4.5, FLOOR - 0.01, b.BUCK_SEAT))
        for ya, yb in ((by0 - BUCK_CLR - LINE4, by0 - BUCK_CLR), (by1 + BUCK_CLR, by1 + BUCK_CLR + LINE4)):
            add.append(box(cx_ - 2.0, cx_ + 2.0, ya, yb, FLOOR - 0.01, b.BUCK_SEAT + BUCK_T + 1.5))
        zt = b.BUCK_SEAT + BUCK_T
        hy_ = cy_ + (4.5 if cy_ > 0 else -4.5)          # the end away from the ring pocket
        add.append(hook(bx0, hy_, "-x", zt, gap=BUCK_CLR))
        add.append(hook(bx1, hy_, "+x", zt, gap=BUCK_CLR))
        add.append(hook(bx1, 2 * cy_ - hy_, "+x", zt, gap=BUCK_CLR))   # ring-side end: +X only (-X is at the ring)
    return add, cut


def make_plate():
    floor = extrude(RectangleRounded(SIZE, SIZE, R_CORNER), amount=FLOOR)
    add, cut = plate_features()
    plate = floor
    for a in add:
        plate = plate + a
    for c in cut:
        plate = plate - c
    plate = plate - frame_keepout()
    for sx, sy, hx, hy in bolt_axes():
        plate = plate + box(hx - PAD / 2, hx + PAD / 2, hy - PAD / 2, hy + PAD / 2, FLOOR - 0.01, PAD_TOP)
        plate = plate - cyl(hx, hy, M3_HOLE / 2, -0.1, PAD_TOP + 0.1) - cyl(hx, hy, CB_D / 2, -0.1, CB_DEPTH)
    return plate


def to_bed(name, s):
    """Print orientation.  No modelled ears any more: the brim comes from the slicer (slice_node_v2.py)."""
    if name == "frame":
        return Pos(0, 0, -FLOOR) * s
    if name == "lid":
        f = Rot(180, 0, 0) * s
        return Pos(0, 0, -f.bounding_box().min.Z) * f
    return s


def dummies2():
    d = b.dummies()
    d.pop("cam", None)
    # ESP32-CAM: v1 stand-in, the PCB at 40.0 (user: ~40)
    cx0, cx1, cy0, cy1 = b.cam_box()
    cz = b.CAM_SEAT_Z
    d["cam"] = (box(cx0, cx1, cy0, cy1, cz, cz + b.CAM_PCB_T)
                + box(b.CAM_C[0] - 7.5, b.CAM_C[0] + 7.5, cy0, cy0 + 15.0, cz - 2.2, cz)
                + box(cx0 + 0.8, cx0 + 3.3, cy0 + 3.3, cy0 + 23.6, cz + b.CAM_PCB_T, cz + b.CAM_PCB_T + b.DUPONT)
                + box(cx1 - 3.3, cx1 - 0.8, cy0 + 3.3, cy0 + 23.6, cz + b.CAM_PCB_T, cz + b.CAM_PCB_T + b.DUPONT),
                "#111827")
    # MLX / SCD41 headers on the back (user): housing + duponts going up
    mx, my = b.MLX_C
    zt = b.MLX_PCB_Z + 1.6
    d["mlx_hdr"] = (box(mx + 8.89 - 2.54 - 1.27, mx + 8.89 - 2.54 + 1.27, my - 6.35, my + 6.35, zt, zt + 16.5), "#111827")
    sx_, sy_ = b.SCD_C
    zt = b.SCD_PCB_Z + 1.6
    hy0 = sy_ - b.SCD_BOARD[1] / 2 + 2.54
    d["scd_hdr"] = (box(sx_ + 0.5 - 6.35, sx_ + 0.5 + 6.35, hy0 - 1.27, hy0 + 1.27, zt, zt + 16.5), "#111827")
    d.pop("scd", None)
    d["scd"] = (box(sx_ - 12.7, sx_ + 12.7, sy_ - 11.43, sy_ + 11.43, b.SCD_PCB_Z, b.SCD_PCB_Z + 1.6)
                + box(sx_ - 5.05, sx_ + 5.05, sy_ - 5.05, sy_ + 5.05, b.SCD_PCB_Z - 7.8, b.SCD_PCB_Z)
                + box(sx_ - 12.64, sx_ - 7.7, sy_ - 3.0, sy_ + 3.0, b.SCD_PCB_Z - 2.96, b.SCD_PCB_Z)
                + box(sx_ + 7.7, sx_ + 12.64, sy_ - 3.0, sy_ + 3.0, b.SCD_PCB_Z - 2.96, b.SCD_PCB_Z), "#1e293b")
    rx0, ry0 = b.RS_X0, b.RS_Y0
    rz = b.RS_SEAT_Z
    d["rs"] = (box(rx0, rx0 + RS_W, ry0, ry0 + RS_L, rz, rz + 1.6)
               + box(rx0 + 2, rx0 + RS_W - 2, ry0 + RS_L - 12, ry0 + RS_L - 4, rz + 1.6, rz + 11.6)
               + box(rx0 + 1.0, rx0 + RS_W - 1.0, ry0 + 0.3, ry0 + 2.84, rz - b.DUPONT, rz)
               + box(rx0 + 1.0, rx0 + RS_W - 1.0, ry0 + RS_L - 2.84, ry0 + RS_L - 0.3, rz - b.DUPONT, rz), "#2563eb")
    return d


def main():
    import pyvista as pv
    (OUT2 / "stl").mkdir(parents=True, exist_ok=True)
    parts = {"plate": make_plate(), "frame": make_frame(), "lid": b.make_lid()}
    info = {}
    for n, s in parts.items():
        export_stl(to_bed(n, s), str(OUT2 / "stl" / f"node2_{n}.stl"), tolerance=0.01, angular_tolerance=0.1)
        bb = s.bounding_box()
        info[n] = {"size": [round(bb.size.X, 2), round(bb.size.Y, 2), round(bb.size.Z, 2)],
                   "volume_cm3": round(s.volume / 1000, 2), "valid": s.is_valid}
    export_step(Compound(list(parts.values())), str(OUT2 / "node_v2.step"))
    dm = dummies2()
    info["interference_mm3"] = b.interference(parts, dm)
    for a, c in (("plate", "frame"), ("lid", "frame"), ("lid", "plate")):
        info[f"{a}_x_{c}_mm3"] = round((parts[a] & parts[c]).volume, 3)
    (OUT2 / "build_info.json").write_text(json.dumps(info, indent=2))
    print(json.dumps(info, indent=1))

    prev = OUT2 / "preview"
    cache = OUT2 / "_cache"
    prev.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    meshes = {}
    for n, s in list(parts.items()) + [(k, v[0]) for k, v in dm.items()]:
        p = cache / f"{n}.stl"
        export_stl(s, str(p), tolerance=0.05, angular_tolerance=0.2)
        meshes[n] = pv.read(str(p))
    col = {"plate": "#5b6068", "frame": "#3f4550", "lid": "#2f343c"}
    col.update({k: v[1] for k, v in dm.items()})

    def shot(fname, items, pos, focal):
        pl = pv.Plotter(off_screen=True, window_size=(1400, 1050))
        pl.set_background("#f4f5f7")
        for n, dz in items:
            pl.add_mesh(meshes[n].translate((0, 0, dz), inplace=False), color=col[n], specular=0.25, specular_power=20)
        pl.camera.position, pl.camera.focal_point, pl.camera.up = pos, focal, (0, 0, 1)
        pl.enable_anti_aliasing("ssaa")
        pl.screenshot(str(prev / fname))
        pl.close()

    boards = [k for k in dm if k not in ("m3_bolts", "m3_nuts")]
    shot("v2_plate_boards.png", [("plate", 0)] + [(k, 0) for k in boards], (-170, -210, 230), (0, 0, 8))
    shot("v2_plate_empty.png", [("plate", 0)], (-170, -210, 230), (0, 0, 8))
    shot("v2_exploded.png", [("plate", 0), ("frame", 45), ("lid", 95)] + [(k, 0) for k in boards],
         (-300, -330, 260), (0, 0, 45))
    shot("v2_frame.png", [("frame", 0)], (-200, -240, 200), (0, 0, 20))


if __name__ == "__main__":
    main()
