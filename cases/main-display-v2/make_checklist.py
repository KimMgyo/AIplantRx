#!/usr/bin/env python3
"""Print / assembly checklist for the v2 case: writes checklist.html (one file, images inside).

Every plate gets two renders: the part as it comes off the bed, and the assembly with that part in orange.
Times and filament come from out/plates_info.json, file names from out/sd_card.  --no-render reuses the images."""
from __future__ import annotations

import base64
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
O = HERE / "out"
A = O / "asm"
IMG = O / "checklist_img"
HL, CTX, BED = "#f97316", "#cbd5e1", "#e5e7eb"
SIZE = (800, 540)

CELLS = [f"v2_06_oled_body_{i}" for i in range(1, 6)] + [f"v2_07_oled_lid_{i}" for i in range(1, 6)]
VIEWS = {"back": (0.55, 0.42, -1.0), "front": (0.5, 0.32, 1.0), "side": (1.0, 0.35, -0.75), "c3": (0.6, 0.3, -1.0)}

# id, step, title, what it is, print orientation, checks after printing, where (highlight, view, variant, caption)
PRINTS = [
    dict(id="v2_00_fit_test", step="1", title="조립 테스트 조각", optional=True,
         what="실제 부품에서 잘라낸 9조각: 스파인 끝 띠 + 벽 이음 2조각, 스파인 홈 + 다리 혀, OLED 칸 몸체 + 뚜껑, C3 카트리지 + 뚜껑. 큰 부품을 뽑기 전에 맞춤을 확인합니다. OLED 칸 구조는 세 버전이 같아서 이 테스트가 맞으면 어느 버전이든 맞습니다.",
         orient="납작하게 한 판",
         checks=["OLED를 핀에 끼우고, 뚜껑을 2.5 mm 낮게 뒤에서 넣어 밀어 올렸을 때 끝까지 들어가는지, 앞에서 눌러도 OLED가 빠지지 않는지",
                 "C3 보드가 카트리지에 눕고 뚜껑이 홈에 들어가는지, USB-C 케이블이 끝까지 꽂히는지",
                 "너트(또는 F-3mm)를 벽 보스 홈에 넣고 M3 × 10으로 조였을 때 머리가 카운터보어 안에 들어가는지, 위쪽 도브테일이 끼워지는지",
                 "다리 혀가 스파인 홈에 밀려 들어가고, 뒤로 당겨도 빠지지 않는지"],
         where=None),
    dict(id="v2_01_pod_left", step="2", title="왼쪽 뒤판",
         what="깊은 뒤판(유리 앞면에서 안쪽 면까지 25.55 mm). 7\" 모듈 황동 스탠드오프 2개, TCA9548A 기둥 2개, 맨 왼쪽 OLED 나사 자리, 7번(UART2) 플러그 공간.",
         orient="뒷면이 베드에",
         checks=["7\" 모듈 뒤에 대 보고 스탠드오프 구멍 2개가 맞는지", "TCA9548A 구멍 2개가 기둥에 맞는지"],
         where=(["v2_01_pod_left"], "back", "cells", "조립 위치 (뒤에서 본 모습이라 오른쪽에 보임)")),
    dict(id="v2_02_pod_right", step="3", title="오른쪽 뒤판",
         what="왼쪽 뒤판과 대칭. 스탠드오프 2개, 맨 오른쪽 OLED 나사 자리, 10번(I2C) 플러그 공간.",
         orient="뒷면이 베드에",
         checks=["7\" 모듈 뒤에 대 보고 스탠드오프 구멍 2개가 맞는지"],
         where=(["v2_02_pod_right"], "back", "cells", "조립 위치 (뒤에서 본 모습이라 왼쪽에 보임)")),
    dict(id="v2_03_spine", step="4", title="스파인 (가운데 등판)",
         what="커넥터가 없는 가운데를 채우는 두꺼운 판. 케이스 가운데 뒷판이자 스탠드 등판. 다리 홈 2개, C3 카트리지 구멍, 가운데 OLED 3개와 벽 위·아래 나사 자리.",
         orient="뒷면이 베드에 · 가장 긴 판이라 밤에 추천",
         checks=["두 뒤판 사이 홈에 들어가는지", "다리 홈 안쪽에 실 찌꺼기가 없는지"],
         where=(["v2_03_spine"], "back", "cells", "조립 위치 (뒤에서)")),
    dict(id="v2_04_wall_left", step="5", title="왼쪽 벽",
         what="화면 둘레 테두리의 왼쪽 반. 위쪽 가운데에 도브테일 홈. 너트 홈 7개.",
         orient="뒤쪽 모서리로 세워서",
         checks=["벽 보스 옆 홈 7개에 너트가 들어가는지", "뒤판 테두리 위에 얹히는지"],
         where=(["v2_04_wall_left"], "front", "cells", "조립 위치 (앞에서)")),
    dict(id="v2_05_wall_right", step="6", title="오른쪽 벽",
         what="오른쪽 반. 위쪽 가운데에 도브테일 혀. 너트 홈 7개.",
         orient="뒤쪽 모서리로 세워서",
         checks=["왼쪽 벽과 도브테일이 맞물리는지", "벽 보스 옆 홈 7개에 너트가 들어가는지"],
         where=(["v2_05_wall_right"], "front", "cells", "조립 위치 (앞에서)")),
    dict(id="v2_06_07_oled_cells_x5", step="7", variant="cells", title="OLED 턱 · 5칸 버전",
         what="칸마다 따로 된 몸체 5개(가운데 3 + 벽까지 넓은 양 끝 2)와 똑같은 뚜껑 5개. 한 칸이 잘못 나오면 그 칸만 다시 뽑으면 됩니다(예비 1칸 파일 있음).",
         orient="몸체는 앞면이 베드에, 뚜껑은 세워서",
         checks=["OLED가 핀에 끼워지는지", "뚜껑이 끝까지 들어가는지"],
         where=(CELLS, "front", "cells", "조립 위치 (앞에서)")),
    dict(id="v2_06_07c_oled_onepiece", step="7", variant="onepiece", title="OLED 턱 · 통짜 버전",
         what="몸체 한 장(벽에서 벽까지 이음매 없는 앞면, 지금 배치) + 뚜껑 한 장(5장을 막대로 연결).",
         split=["v2_06c_oled_chin_onepiece", "v2_07c_oled_lid_onepiece"],
         orient="몸체는 앞면이 베드에, 뚜껑은 세워서",
         checks=["OLED 5개가 핀에 끼워지는지", "뚜껑이 홈 10개에 한꺼번에 들어가는지"],
         where=(["asm_v2_06c_oled_chin_onepiece", "asm_v2_07c_oled_lid_onepiece"], "front", "onepiece", "조립 위치 (앞에서)")),
    dict(id="v2_06_07e_oled_even", step="7", variant="even", title="OLED 턱 · 통짜 등간격 버전",
         what="통짜 버전과 같고, 벽과 창 사이·창과 창 사이 간격이 모두 9.76 mm. 양 끝 화면이 7\" 표시영역보다 약 7 mm 바깥까지 나갑니다.",
         split=["v2_06e_oled_chin_even", "v2_07e_oled_lid_even"],
         orient="몸체는 앞면이 베드에, 뚜껑은 세워서",
         checks=["OLED 5개가 핀에 끼워지는지", "뚜껑이 홈 10개에 한꺼번에 들어가는지"],
         where=(["asm_v2_06e_oled_chin_even", "asm_v2_07e_oled_lid_even"], "front", "even", "조립 위치 (앞에서)")),
    dict(id="v2_08_09_c3_cartridge", step="8", title="C3 카트리지 + 뚜껑",
         what="ESP32-C3 SuperMini를 담는 통과 밀어 닫는 뚜껑. 케이스에서는 세워져 USB-C가 뒤로 곧게 나옵니다. 스파인 뒤에서 끼우고 M3 2개로 고정.",
         orient="카트리지는 바닥이 베드에 닿게 눕혀서, 뚜껑도 납작하게",
         checks=["C3가 들어가고 뚜껑이 닫히는지", "USB-C 케이블이 끝까지 꽂히는지"],
         where=(["v2_08_c3_cart", "v2_09_c3_lid"], "c3", "cells", "조립 위치 (뒤에서, 가까이)")),
    dict(id="v2_10_legs_x2", step="9", title="스탠드 다리 2개",
         what="똑같은 삼각형 다리 2개. 둥근 선 구멍과 케이블타이 구멍이 있습니다. 스파인 뒤 홈에 아래에서 밀어 올려 끼웁니다.",
         orient="납작하게",
         checks=["스파인 홈에 밀려 올라가는지"],
         where=(["v2_10_leg_L", "v2_10_leg_R"], "side", "cells", "조립 위치 (뒤 옆에서)")),
]
SPARES = [
    dict(id="v2_06_07_oled_cell_x1", step="예비", title="OLED 5칸 버전 가운데 칸 1개", spare=True,
         what="5칸 버전에서 한 칸이 잘못 나왔을 때 다시 뽑는 몸체 + 뚜껑 1세트.", orient="몸체는 앞면이 베드에, 뚜껑은 세워서",
         checks=[], where=(["v2_06_oled_body_3", "v2_07_oled_lid_3"], "front", "cells", "조립 위치 (앞에서)")),
    dict(id="v2_10_leg_x1", step="예비", title="다리 1개", spare=True,
         what="다리 하나만 다시 뽑을 때.", orient="납작하게",
         checks=[], where=(["v2_10_leg_R"], "side", "cells", "조립 위치 (뒤 옆에서)")),
]
WARN = {"v2_00_fit_test", "v2_06_07_oled_cells_x5", "v2_06_07_oled_cell_x1", "v2_06_07c_oled_onepiece",
        "v2_06_07e_oled_even"}

HW = [
    dict(id="hw_screw", label="M3 × 10 나사", qty="27개",
         note="모듈 스탠드오프 4 · 벽 14 · OLED 뚜껑 5 · C3 카트리지 2 · TCA 2"),
    dict(id="hw_nut", label="M3 너트 (5.5 × 2.4) 또는 F-3mm 황동 서포트", qty="23개",
         note="벽 보스 14 · OLED 뚜껑 발 5 · 스파인(C3) 2 · 왼쪽 뒤판(TCA) 2. 모듈 스탠드오프 4곳은 너트 없음"),
    dict(id="hw_tie", label="케이블타이", qty="선택", note="다리 구멍에 선을 묶을 때"),
    dict(id="hw_bed", label="IPA, 딱풀", qty="", note="세워서 뽑는 판의 바닥 접착용 (베드 60 °C)"),
]
EL = [
    dict(id="el_s3", label="Waveshare ESP32-S3-Touch-LCD-7", qty="1", note="황동 스탠드오프 4개가 달린 상태"),
    dict(id="el_oled", label="GME12864 0.96\" OLED", qty="5", note="헤더 핀에 선을 바로 납땜"),
    dict(id="el_tca", label="TCA9548A I2C 멀티플렉서", qty="1", note=""),
    dict(id="el_c3", label="TENSTAR ESP32-C3 SuperMini", qty="1", note="USB-C가 유일한 외부 케이블"),
    dict(id="el_plug", label="S3 7번(UART2) · 10번(I2C) 플러그 케이블", qty="각 1", note=""),
    dict(id="el_usb", label="USB-C 케이블", qty="1", note="C3 전원 겸 펌웨어"),
    dict(id="el_r18", label="S3 18번 저항이 5V 쪽인지 확인", qty="", note="10번 단자 VCC로 5V를 넣기 때문"),
]
ASM = [
    dict(id="a1", label="벽 두 쪽을 앞면이 아래로 가게 놓고 위쪽 가운데 도브테일을 맞춥니다. 벽 보스 옆 홈에 너트를 넣고, 뒤판 두 장을 테두리 위에 얹어 조입니다.", qty="M3 10 · 너트 10"),
    dict(id="a2", label="스파인을 두 뒤판 사이 홈에 뒤에서 넣고 위·아래 끝 띠를 조입니다.", qty="M3 4 · 너트 4"),
    dict(id="a3", label="TCA9548A를 앞쪽에서 왼쪽 뒤판 기둥에 고정합니다(부품면이 앞, 너트는 뒤판 홈, 선은 뒤로).", qty="M3 2 · 너트 2"),
    dict(id="a4", label="OLED 5개에 선을 납땜해 몸체 핀에 끼웁니다. 뚜껑 발 홈에 너트를 넣고, 뚜껑을 2.5 mm 낮게 뒤에서 곧게 넣은 뒤 2.5 mm 밀어 올립니다. 앞에서 턱 자리에 넣고 뒤에서 조입니다.", qty="M3 5 · 너트 5"),
    dict(id="a5", label="C3에 선을 납땜해 카트리지에 넣고 뚜껑을 밀어 닫습니다. 스파인 홈에 너트를 넣고, 선을 스파인 구멍으로 먼저 넣은 뒤 카트리지를 뒤에서 끼워 조입니다.", qty="M3 2 · 너트 2"),
    dict(id="a6", label="S3의 7번(UART2)·10번(I2C) 플러그를 꽂고 선을 턱 쪽으로 내립니다. 7\" 모듈을 앞에서 넣고 뒤판 카운터보어로 스탠드오프에 조입니다.", qty="M3 4"),
    dict(id="a7", label="다리 두 개를 스파인 아래에서 홈에 밀어 올립니다.", qty=""),
]


def render():
    import numpy as np
    import pyvista as pv
    from PIL import Image
    IMG.mkdir(exist_ok=True)
    var_files = {"cells": [A / f"{n}.stl" for n in CELLS],
                 "onepiece": [O / "asm_v2_06c_oled_chin_onepiece.stl", O / "asm_v2_07c_oled_lid_onepiece.stl"],
                 "even": [O / "asm_v2_06e_oled_chin_even.stl", O / "asm_v2_07e_oled_lid_even.stl"]}
    base = [f for f in sorted(A.glob("v2_*.stl")) if not f.stem.startswith(("v2_06_oled_body", "v2_07_oled_lid"))]
    cache = {}

    def mesh(p):
        if p not in cache:
            cache[p] = pv.read(str(p))
        return cache[p]

    def snap(pl, vec, name, zoom, up, focus=None, dist=None):
        pl.view_vector(vec, viewup=up)
        pl.reset_camera()
        if focus is None:
            pl.camera.zoom(zoom)
        else:
            v = np.array(vec, float) / np.linalg.norm(vec)
            pl.camera.focal_point = focus
            pl.camera.position = tuple(np.array(focus, float) + v * dist)
            pl.camera.up = up
        pl.enable_anti_aliasing("ssaa")
        img = pl.screenshot(None, return_img=True)
        pl.close()
        Image.fromarray(img).save(IMG / f"{name}.webp", quality=86, method=6)

    for it in PRINTS + SPARES:
        m = pv.read(str(O / "plates" / f"{it['id']}.stl"))
        b = m.bounds
        pl = pv.Plotter(off_screen=True, window_size=SIZE)
        pl.set_background("white")
        pl.add_mesh(pv.Plane(center=((b[0] + b[1]) / 2, (b[2] + b[3]) / 2, -0.2),
                             i_size=b[1] - b[0] + 24, j_size=b[3] - b[2] + 24), color=BED)
        pl.add_mesh(m, color=HL, smooth_shading=False, specular=0.25)
        small = it["id"].startswith(("v2_08", "v2_06_07_oled_cell_x1"))
        snap(pl, (0.55, -1.0, 0.85), it["id"] + "_part", 1.35 if small else 1.2, (0, 0, 1))
        if not it["where"]:
            continue
        hl, view, variant, _ = it["where"]
        pl = pv.Plotter(off_screen=True, window_size=SIZE)
        pl.set_background("white")
        pl.enable_depth_peeling()
        if view == "front":
            pl.add_mesh(mesh(A / "module_7INCH-800X480-TP.stl"), color="#111827")
        files = base + var_files[variant]
        for f in files:
            if f.stem not in hl:
                pl.add_mesh(mesh(f), color=CTX, opacity=0.33, smooth_shading=False)
        for f in files:
            if f.stem in hl:
                pl.add_mesh(mesh(f), color=HL, smooth_shading=False, specular=0.25)
        if view == "c3":
            snap(pl, VIEWS[view], it["id"] + "_where", 1.0, (0, 1, 0), focus=(0.0, -68.0, -28.0), dist=230.0)
        else:
            snap(pl, VIEWS[view], it["id"] + "_where", 1.15 if view == "side" else 1.2, (0, 1, 0))
        print("rendered", it["id"], flush=True)


def secs(t):
    h = re.search(r"(\d+)h", t)
    m = re.search(r"(\d+)m", t)
    s = re.search(r"(\d+)s", t)
    return (int(h.group(1)) * 3600 if h else 0) + (int(m.group(1)) * 60 if m else 0) + (int(s.group(1)) if s else 0)


def sd_name(n):
    for f in (O / "sd_card").glob("*.gcode"):
        if re.fullmatch(re.escape(n) + r"_(\d+h)?\d+m\.gcode", f.name):
            return f.name
    raise SystemExit(f"no SD file for {n}")


def b64(name):
    return "data:image/webp;base64," + base64.b64encode((IMG / f"{name}.webp").read_bytes()).decode()


def short(t):
    return t.replace(" ", "")


def build():
    info = json.loads((O / "plates_info.json").read_text(encoding="utf-8"))
    imgs, out = {}, {}
    for key, lst in (("prints", PRINTS), ("spares", SPARES)):
        rows = []
        for it in lst:
            r = {k: v for k, v in it.items() if k not in ("where", "split")}
            pi = info[it["id"]]
            r.update(file=sd_name(it["id"]), time=short(pi["time"]), sec=secs(pi["time"]), g=float(pi["grams"]),
                     warn=it["id"] in WARN)
            imgs[it["id"] + "_part"] = b64(it["id"] + "_part")
            r["part"] = it["id"] + "_part"
            if it.get("where"):
                imgs[it["id"] + "_where"] = b64(it["id"] + "_where")
                r["where"] = it["id"] + "_where"
                r["wcap"] = it["where"][3]
            if it.get("split"):
                r["split"] = [dict(file=sd_name(n), time=short(info[n]["time"]), g=float(info[n]["grams"])) for n in it["split"]]
            rows.append(r)
        out[key] = rows
    out.update(hw=HW, el=EL, asm=ASM, img=imgs)
    html = TEMPLATE.replace("__DATA__", json.dumps(out, ensure_ascii=False))
    (HERE / "checklist.html").write_text(html, encoding="utf-8")
    print("wrote", HERE / "checklist.html", round(len(html) / 1e6, 2), "MB")


TEMPLATE = (HERE / "checklist_template.html").read_text(encoding="utf-8") if (HERE / "checklist_template.html").exists() else ""

if __name__ == "__main__":
    if "--no-render" not in sys.argv:
        render()
    build()
