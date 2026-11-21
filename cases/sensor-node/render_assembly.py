#!/usr/bin/env python3
"""Assembly pictures for the README: one image per step (new parts in colour, parts already in place in light
grey), an exploded view, the finished box and the installed view under a PC case top panel.
Uses the meshes build_node.py leaves in out/_cache, so run build_node.py first."""
from __future__ import annotations

import pyvista as pv

import build_node as b

pv.OFF_SCREEN = True
CACHE = b.OUT / "_cache"
PREV = b.OUT / "preview" / "assembly"
DONE = "#d6d9de"                       # parts already in place
COL = {"body": "#5b6068", "lid": "#3f4550", "scd_cap": "#94a3b8"}
COL.update({n: c for n, (_, c) in b.dummies().items()})
COL["ring"] = "#e11d48"                # the real ring is a black PCB: drawn red so it shows on the dark body

STEPS = [  # (file, new parts)
    ("a01_nuts", ["m3_nuts"]),
    ("a02_ring", ["ring", "leds"]),
    ("a03_mlx_cam", ["mlx", "mlx_can", "cam", "cam_lens"]),
    ("a04_scd", ["scd", "scd_cap"]),
    ("a05_terminals", ["kf_12V", "kf_P1", "kf_P2", "kf_P3", "kf_485"]),
    ("a06_devkit_485", ["dev", "rs"]),
    ("a07_perf", ["perf"]),
    ("a08_mosfets", ["lr1", "lr2", "lr3"]),
    ("a09_bucks", ["buck_ring", "buck_cam"]),
    ("a10_lid", ["lid", "m3_bolts"]),
]
VIEW_IN = ((-200, -240, 260), (0, 0, 10))
VIEWS = {"a05_terminals": ((10, 60, 90), (10, -60, 10))}      # from +Y: the terminal pockets behind the -Y wall


def load(name):
    return pv.read(str(CACHE / f"{name}.stl"))


def body_two_tone():
    """Body as printed: the first DIFF_T (white, before the M600) and the rest in the body colour."""
    m = load("body")
    return [(m.clip(normal="z", origin=(0, 0, b.DIFF_T), invert=True), "#f1f2f4"),
            (m.clip(normal="z", origin=(0, 0, b.DIFF_T), invert=False), COL["body"])]


def shot(fname, items, pos, focal, up=(0, 0, 1), size=(1400, 1050), headlight=False):
    pl = pv.Plotter(off_screen=True, window_size=size)
    pl.set_background("#f4f5f7")
    if any(op < 1.0 for _, _, op in items):
        pl.enable_depth_peeling()
    for m, col, op in items:
        if m.n_points:
            pl.add_mesh(m, color=col, opacity=op, smooth_shading=False, specular=0.25, specular_power=20)
    pl.camera.position, pl.camera.focal_point, pl.camera.up = pos, focal, up
    if headlight:                                    # views from below: the default key light is above
        pl.add_light(pv.Light(light_type="headlight", intensity=0.35))
        pl.add_light(pv.Light(position=(0, 0, -400), focal_point=(0, 0, 0), light_type="scene light",
                              intensity=0.9))      # from the plant: lights the sensor face
    pl.enable_anti_aliasing("ssaa")
    pl.screenshot(str(PREV / f"{fname}.png"))
    pl.close()


def main():
    PREV.mkdir(parents=True, exist_ok=True)
    mesh = {}
    for _, names in STEPS:
        for n in names:
            mesh[n] = load(n)
    base = [(m, c, 1.0) for m, c in body_two_tone()]
    placed = []
    for fname, names in STEPS:
        see = 0.28 if fname == "a01_nuts" else 1.0          # nuts sit inside the pillars: see-through body
        items = [(m, c, see) for m, c, _ in base] + [(mesh[n], DONE, 1.0) for n in placed]             + [(mesh[n], "#f59e0b" if n == "m3_nuts" else COL[n], 1.0) for n in names]
        shot(fname, items, *VIEWS.get(fname, VIEW_IN))
        placed += names

    inside = [n for n in placed if n not in ("lid", "m3_bolts")]
    # finished, lid off (everything in its own colour)
    shot("b01_inside_all", list(base) + [(mesh[n], COL[n], 1.0) for n in inside], *VIEW_IN)
    # exploded: lid + bolts 70 up, SCD cap 30 up
    up = {"lid": 70.0, "m3_bolts": 70.0, "scd_cap": 30.0}
    items = list(base) + [(mesh[n].translate((0, 0, up.get(n, 0.0)), inplace=False), COL[n], 1.0) for n in placed]
    shot("b02_exploded", items, (-260, -300, 250), (0, 0, 40))
    # finished box from below: the sensor face (white skin layer) with the lenses
    done = list(base) + [(mesh[n], COL[n], 1.0) for n in placed]
    shot("b03_sensor_face", done, (-170, -220, -190), (0, 0, 12), headlight=True)
    # installed: hanging under a case top panel (a plate with a 140 fan grille), looking up from the plant
    top = b.H + b.LID_T
    panel = pv.Box(bounds=(-110, 110, -110, 110, top, top + 1.0))
    grille = [pv.Cylinder(center=(0, 0, top + 0.5), direction=(0, 0, 1), radius=r, height=1.2).triangulate()
              for r in (66.0,)]
    panel = panel.triangulate().boolean_difference(grille[0])
    pot = pv.Cylinder(center=(0, 0, -150), direction=(0, 0, 1), radius=55, height=60)
    plant = pv.Sphere(radius=38, center=(0, 0, -95))
    shot("b04_installed", done + [(panel, "#9aa0a8", 1.0), (pot, "#b45309", 1.0), (plant, "#4d7c0f", 1.0)],
         (-330, -420, -170), (0, 0, -40), headlight=True)
    print("written to", PREV)


if __name__ == "__main__":
    main()
