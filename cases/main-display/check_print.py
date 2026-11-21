#!/usr/bin/env python3
"""Print-readiness summary for every STL in <dir>/stl with its sliced G-code in <dir>/gcode:
downward faces steeper than 45 deg above the first layer (overhang clusters), watertightness,
and PrusaSlicer's time / filament estimate.  Usage: check_print.py [out | out/fit_test]"""
import json
import re
import sys
from pathlib import Path

import numpy as np
import trimesh

base = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("out")


def comps(mm):
    idx = np.arange(len(mm.faces))
    parent = idx.copy()

    def f(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for a, b in mm.face_adjacency:
        ra, rb = f(a), f(b)
        if ra != rb:
            parent[ra] = rb
    roots = np.array([f(i) for i in idx])
    for r in np.unique(roots):
        yield mm.submesh([np.where(roots == r)[0]], append=True)


res = {}
for n in sorted(p.stem for p in (base / "stl").glob("*.stl")):
    m = trimesh.load(base / "stl" / f"{n}.stl")
    fn = m.face_normals
    zmin = m.triangles[:, :, 2].min(axis=1)
    sel = (fn[:, 2] < -0.72) & (zmin > 0.15)
    clusters = []
    if sel.any():
        sub = m.submesh([np.where(sel)[0]], append=True)
        for c in comps(sub):
            b = c.bounds
            clusters.append(dict(z=round(float(b[0][2]), 2), size=[round(float(x), 1) for x in (b[1] - b[0])[:2]],
                                 area=round(float(c.area), 1)))
    clusters.sort(key=lambda c: -c["area"])
    gp = base / "gcode" / f"{n}.gcode"
    g = gp.read_text(errors="ignore") if gp.exists() else ""

    def grab(k):
        mm = re.search(r"^; " + re.escape(k) + r" = (.*)$", g, re.M)
        return mm.group(1).strip() if mm else None
    res[n] = dict(bbox=[round(float(x), 1) for x in m.extents], watertight=bool(m.is_watertight),
                  overhang_clusters=len(clusters), top=clusters[:12],
                  time=grab("estimated printing time (normal mode)"), grams=grab("filament used [g]"),
                  layer=grab("layer_height"), first=grab("first_layer_height"), perimeters=grab("perimeters"),
                  infill=grab("fill_density"), support=grab("support_material"))
print(json.dumps(res, indent=1, ensure_ascii=False))
