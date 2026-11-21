#!/usr/bin/env python3
"""Quick fit coupon for the GME12864-11/12/13 0.96" OLED (the module the user actually has).

Mount geometry measured from MakerWorld 2703510 "Stylish snap-together case for SSD1306 display"
(research/gme12864/Display+Case.3mf, front shell): front plate, window, board pocket, 4 bosses and the
strip between the header-side bosses.  Heights are moved onto this case's 0.2 + 0.35 n layer grid.

No screws, no heat staking: two identical snap clips hold the board from behind.  Each clip lies on one
side edge of the board (clear of the header at the top centre and the FPC at the bottom centre), presses
the two corners on that side through small pads, and its two arms snap into slots in the walls beside the
board (in the case: the posts between neighbouring OLEDs).  The clip prints flat, so its arms flex along
the printed lines; the slot roofs are the only part of the frame that takes the load.

Print orientation = as printed: front face on the bed, you look at the back of the front plate.
-Y is the header side (the window is shifted 2.8 mm toward the header).
"""
from __future__ import annotations

import json
from pathlib import Path

from build123d import (Align, Box, Compound, Cylinder, Polygon, Plane, Pos, Rectangle, RectangleRounded, Rot,
                       export_stl, extrude)

HERE = Path(__file__).resolve().parent
OUT = HERE / "out" / "fit_test"


def L(n):
    return round(0.20 + 0.35 * n, 3)


# ---- board mount, measured on the MakerWorld front shell (mm)
BOARD_W, BOARD_H, BOARD_T = 27.5, 27.8, 1.2   # GME12864 (PCB 27.3-27.5 x 27.8)
POCKET_W, POCKET_H = 28.0, 28.0
HOLE_DX, HOLE_DY = 11.6, 11.75           # board hole centres: 23.2 x 23.5
BOSS_D = 4.0
WIN_W, WIN_H, WIN_DY = 26.0, 14.0, -2.8  # window, centre offset toward the header
WIN_CH = 0.5                             # front-face chamfer (27 x 15 on the front face)
STRIP_Y = (-11.8, -10.4)                 # strip joining the header-side bosses; those bosses are cut
                                         # flat at its window-side edge (clears the glass edge)
FRONT_T = L(5)                           # 1.95 = this case's front plate
BOSS_TOP = L(11)                         # 4.05: bosses 2.10 above the plate (glass + tape stack)
WALL_TOP = L(14)                         # 5.10: top and bottom walls of the pocket
BOARD_BACK = BOSS_TOP + BOARD_T          # 5.25

# ---- snap clip (one per side, the same part for both sides)
CLIP_X0, CLIP_BODY_X1 = 10.4, 12.0       # body over the board edge strip, 1.75 .. 3.35 in from the edge
ARM_X = (13.05, 13.85)                   # flexible arm, 0.8 wide; slit 12.0 .. 13.05 lets it bend 1.05 in
TAB_X1 = 14.9                            # tab reaches 0.9 into the slot (wall face at 14.0)
CLIP_Y = 13.4                            # body runs -13.4 .. 13.4
ROOT_Y = 1.75                            # arms leave the body at |y| < 1.75 ...
TAB_Y = (9.25, 11.75)                    # ... and end in a tab at the corners
PAD_Y = (10.1, 13.4)                     # pads press the corner (around the mounting hole)
PAD_H = L(3) - 0.0                       # 1.25 above the board back: clears 0603 parts under the body
CLIP_T = 2.30                            # body / arm thickness (out-of-plane stiffness for the load)
TAB_BEVEL = 0.95                         # lead-in on the tab's lower outer edge

# ---- walls beside the board (posts between OLEDs in the case)
SIDE_X = (14.0, 17.0)                    # 3 mm here; in the case the post between two OLEDs is 5.0 wide
SLOT_Y = (9.0, 12.0)
SLOT_DEPTH = 1.1                         # 14.0 .. 15.1
SLOT_Z0 = L(16)                          # 5.80 (below the tab, any value works)
SLOT_ROOF = L(25)                        # 8.95: clip top = 5.25 + 1.25 + 2.30 = 8.80 -> 0.15 play
SIDE_TOP = L(29)                         # 10.35: 1.4 mm roof over the slot


def zbox(x0, x1, y0, y1, z0, z1):
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, z0) * extrude(Rectangle(x1 - x0, y1 - y0), z1 - z0)


def zcyl(x, y, d, z0, z1):
    return Pos(x, y, z0) * Cylinder(d / 2, z1 - z0, align=(Align.CENTER, Align.CENTER, Align.MIN))


def make_frame():
    ow = 2 * SIDE_X[1]
    oh = POCKET_H + 4.0
    body = extrude(RectangleRounded(ow, oh, 2.0), WALL_TOP)
    for sx in (-1, 1):
        body += zbox(min(sx * SIDE_X[0], sx * SIDE_X[1]), max(sx * SIDE_X[0], sx * SIDE_X[1]),
                     -oh / 2 + 1.0, oh / 2 - 1.0, 0.0, SIDE_TOP)
    body -= zbox(-POCKET_W / 2, POCKET_W / 2, -POCKET_H / 2, POCKET_H / 2, FRONT_T, SIDE_TOP + 1)
    add = [zcyl(sx * HOLE_DX, HOLE_DY, BOSS_D, FRONT_T - 0.01, BOSS_TOP) for sx in (-1, 1)]
    for sx in (-1, 1):
        add.append(zcyl(sx * HOLE_DX, -HOLE_DY, BOSS_D, FRONT_T - 0.01, BOSS_TOP)
                   & zbox(-20, 20, -20, STRIP_Y[1], FRONT_T - 0.02, BOSS_TOP + 0.01))
    add.append(zbox(-HOLE_DX, HOLE_DX, STRIP_Y[0], STRIP_Y[1], FRONT_T - 0.01, BOSS_TOP))
    for a in add:
        body += a
    body -= zbox(-WIN_W / 2, WIN_W / 2, WIN_DY - WIN_H / 2, WIN_DY + WIN_H / 2, -1.0, SIDE_TOP + 1)
    body -= Pos(0, WIN_DY, -0.01) * extrude(Rectangle(WIN_W + 2 * WIN_CH, WIN_H + 2 * WIN_CH), WIN_CH + 0.01,
                                             taper=45)
    for sx in (-1, 1):
        for sy in (-1, 1):
            x0, x1 = sorted((sx * SIDE_X[0], sx * (SIDE_X[0] + SLOT_DEPTH)))
            y0, y1 = sorted((sy * SLOT_Y[0], sy * SLOT_Y[1]))
            body -= zbox(x0, x1, y0, y1, SLOT_Z0, SLOT_ROOF)
    return body


def make_clip_installed():
    """Right-hand clip where it sits on the board (z = 0 at the board back).  The left clip is the same
    part turned 180 degrees."""
    z0, z1 = PAD_H, PAD_H + CLIP_T
    c = zbox(CLIP_X0, CLIP_BODY_X1, -CLIP_Y, CLIP_Y, z0, z1)
    c += zbox(CLIP_BODY_X1 - 0.01, ARM_X[1], -ROOT_Y, ROOT_Y, z0, z1)
    for sy in (-1, 1):
        y0, y1 = sorted((sy * ROOT_Y, sy * TAB_Y[1]))
        c += zbox(ARM_X[0], ARM_X[1], y0, y1, z0, z1)
        ty0, ty1 = sorted((sy * TAB_Y[0], sy * TAB_Y[1]))
        prof = Polygon((ARM_X[1] - 0.01, z0), (TAB_X1 - TAB_BEVEL, z0), (TAB_X1, z0 + TAB_BEVEL), (TAB_X1, z1),
                       (ARM_X[1] - 0.01, z1), align=None)
        c += Pos(0, ty1, 0) * extrude(Plane.XZ * prof, ty1 - ty0)
        py0, py1 = sorted((sy * PAD_Y[0], sy * PAD_Y[1]))
        c += zbox(CLIP_X0, CLIP_BODY_X1, py0, py1, 0.0, z0 + 0.01)
    return c


def board_model():
    return zbox(-BOARD_W / 2, BOARD_W / 2, -BOARD_H / 2, BOARD_H / 2, BOSS_TOP, BOARD_BACK)


def main():
    (OUT / "stl").mkdir(parents=True, exist_ok=True)
    (OUT / "gcode").mkdir(parents=True, exist_ok=True)
    frame = make_frame()
    clip = make_clip_installed()
    right = Pos(0, 0, BOARD_BACK) * clip
    left = Rot(0, 0, 180) * right
    board = board_model()
    vol = lambda s: 0.0 if s is None else sum(v.volume for v in (s.solids() if hasattr(s, "solids") else [s]))  # noqa
    checks = {
        "frame x board (mm3)": round(vol(frame & board), 4),
        "frame x clips installed (mm3)": round(vol(frame & right) + vol(frame & left), 4),
        "board x clips (mm3, pads only touch)": round(vol(board & right) + vol(board & left), 4),
        "tab engagement under slot roof (mm)": round(TAB_X1 - SIDE_X[0], 2),
        "clip play to slot roof (mm)": round(SLOT_ROOF - (BOARD_BACK + PAD_H + CLIP_T), 2),
        "arm bend to pass the roof (mm) / slit (mm)": [round(TAB_X1 - SIDE_X[0] + 0.05, 2),
                                                       round(ARM_X[0] - CLIP_BODY_X1, 2)],
        "arm strain at full bend (%)": round(100 * 3 * (ARM_X[1] - ARM_X[0]) * (TAB_X1 - SIDE_X[0] + 0.05)
                                             / (2 * ((TAB_Y[0] + TAB_Y[1]) / 2 - ROOT_Y) ** 2), 2),
    }
    # clip print orientation: catch face (top in use) on the bed, pads up
    clip_print = Pos(0, 0, PAD_H + CLIP_T) * (Rot(180, 0, 0) * clip)
    info = {}
    parts = {"fit_10_oled_gme12864": frame, "fit_12_oled_clip": clip_print}
    for name, s in parts.items():
        bb = s.bounding_box()
        export_stl(s, str(OUT / "stl" / f"{name}.stl"), tolerance=0.01, angular_tolerance=0.1)
        info[name] = dict(size=[round(bb.size.X, 2), round(bb.size.Y, 2), round(bb.size.Z, 2)],
                          volume_cm3=round(s.volume / 1000, 3), solids=len(s.solids()))
    plate = [frame] + [Pos(dx, 0, 0) * clip_print for dx in (9.5, 15.5)]
    export_stl(Compound(plate), str(OUT / "stl" / "plate_D_oled_gme12864_clip.stl"), tolerance=0.01,
               angular_tolerance=0.1)
    asm = Compound([frame, board, right, left])
    export_stl(asm, str(OUT / "stl" / "_assembly_check_oled_clip.stl"), tolerance=0.02, angular_tolerance=0.2)
    print(json.dumps(dict(parts=info, checks=checks), indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
