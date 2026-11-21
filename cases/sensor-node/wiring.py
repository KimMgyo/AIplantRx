#!/usr/bin/env python3
"""Wiring diagram of the sensor node (ARCHITECTURE.md 2.3 + README 배선) -> out/wiring.html, out/wiring.svg,
out/devkit_pins.svg.

Two power domains: the devkit runs from its own USB-C (its 3V3 feeds SCD41, MLX90640, MAX485); 12V feeds two
5 V bucks (ring, ESP32-CAM), the SEN0604 and the LR7843 / pumps.  One wire joins devkit GND and 12V -.
The schematic groups the devkit pins by function; each label also gives the pin's place on the real header
("왼10" = left row, 10th pin from the antenna end, board seen from the component side with USB-C at the bottom).
The second drawing is that header: the DevKitC V4 pin map, which the Higenis HG-ESP32-DEVKIT V4 copies.
Dashed parts are recommended, later or undecided.  Wires get a background-coloured halo, so a wire that crosses
another without a dot visibly breaks it.
Colours use theme variables with fallbacks so the same file works inline in T3 and stand-alone."""
from __future__ import annotations

from pathlib import Path

OUT = Path(__file__).resolve().parent / "out"

FG = "var(--foreground, #111827)"
MUTED = "var(--muted-foreground, #6b7280)"
CARD = "var(--card, #ffffff)"
BORDER = "var(--border, #d1d5db)"
BG = "var(--background, #ffffff)"
C = {  # net colours
    "12V": "#dc2626", "GND": FG, "5V": "#ea580c", "USB": "#7c3aed", "3V3": "#2563eb", "I2C": "#16a34a",
    "UART": "#0891b2", "A": "#ca8a04", "B": "#1d4ed8", "DIN": "#db2777", "LOAD": "#92400e", "CTL": "#64748b",
    "brown": "#92400e", "black": FG, "yellow": "#ca8a04", "blue": "#1d4ed8", "MUTED": MUTED,
}
L: dict[str, list[str]] = {}


def canvas():
    """Start a drawing.  Layers are joined bottom to top, so labels and pins always sit above wires."""
    global L
    L = {k: [] for k in ("box", "wire", "dot", "pin", "text")}


def t(x, y, s, size=13, anchor="start", color=FG, weight=400):
    L["text"].append(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{color}" '
                     f'font-weight="{weight}">{s}</text>')


def box(x, y, w, h, title, sub=None, dashed=False, sub2=None):
    dash = ' stroke-dasharray="6 4"' if dashed else ""
    L["box"].append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{CARD}" stroke="{BORDER}" '
                    f'stroke-width="1.5"{dash}/>')
    if title:
        t(x + w / 2, y - 8, title, 14, "middle", FG, 600)
    if sub:
        t(x + w / 2, y + h + 17, sub, 12, "middle", MUTED)
    if sub2:
        t(x + w / 2, y + h + 33, sub2, 12, "middle", MUTED)


def pin(x, y, label, side):
    """A pin on a box edge; the label sits inside the box."""
    L["pin"].append(f'<circle cx="{x}" cy="{y}" r="3" fill="{FG}"/>')
    if label:
        dx = 8 if side == "l" else -8
        t(x + dx, y + 4, label, 12, "start" if side == "l" else "end", FG)


def wire(pts, net, width=2.2, dash=None, halo=True):
    p = " ".join(f"{a},{b}" for a, b in pts)
    if halo:
        L["wire"].append(f'<polyline points="{p}" fill="none" stroke="{BG}" stroke-width="{width + 5}" '
                         f'stroke-linejoin="round" stroke-linecap="butt"/>')
    d = f' stroke-dasharray="{dash}"' if dash else ""
    L["wire"].append(f'<polyline points="{p}" fill="none" stroke="{C[net]}" stroke-width="{width}" '
                     f'stroke-linejoin="round" stroke-linecap="round"{d}/>')


def dot(x, y, net):
    L["dot"].append(f'<circle cx="{x}" cy="{y}" r="4.5" fill="{C[net]}"/>')


def gnd(x, y, side="r", stub=16):
    """Signal ground symbol: goes to devkit GND (= 12V - through the link wire)."""
    x2 = x + stub if side == "r" else x - stub
    wire([(x, y), (x2, y), (x2, y + 8)], "GND", 1.8, halo=False)
    for k, hw in enumerate((8, 5, 2)):
        L["pin"].append(f'<line x1="{x2 - hw}" y1="{y + 8 + 4 * k}" x2="{x2 + hw}" y2="{y + 8 + 4 * k}" '
                        f'stroke="{FG}" stroke-width="1.8"/>')


def res(x0, x1, y, net="CTL", label="1kΩ"):
    xm = (x0 + x1) / 2
    wire([(x0, y), (x1, y)], net)
    L["pin"].append(f'<rect x="{xm - 18}" y="{y - 6}" width="36" height="12" fill="{CARD}" stroke="{C[net]}" '
                    f'stroke-width="2"/>')
    t(xm, y - 10, label, 11, "middle", MUTED)


def vres(x, y0, y1, net, label):
    """Vertical resistor between y0 (top) and y1."""
    ym = (y0 + y1) / 2
    wire([(x, y0), (x, y1)], net, halo=False)
    L["pin"].append(f'<rect x="{x - 6}" y="{ym - 16}" width="12" height="32" fill="{CARD}" stroke="{C[net]}" '
                    f'stroke-width="2"/>')
    t(x + 12, ym + 4, label, 11, "start", MUTED)


def diode_later(x, y_cath, y_anode):
    """Dashed flyback diode (cathode up) for the pump terminal: not fitted yet."""
    ym = (y_cath + y_anode) / 2
    s = f'stroke="{MUTED}" stroke-width="1.6" stroke-dasharray="3 3"'
    L["pin"].append(f'<line x1="{x}" y1="{y_cath}" x2="{x}" y2="{ym - 6}" {s}/>')
    L["pin"].append(f'<line x1="{x}" y1="{ym + 6}" x2="{x}" y2="{y_anode}" {s}/>')
    L["pin"].append(f'<polygon points="{x - 7},{ym + 6} {x + 7},{ym + 6} {x},{ym - 6}" fill="{CARD}" {s}/>')
    L["pin"].append(f'<line x1="{x - 7}" y1="{ym - 6}" x2="{x + 7}" y2="{ym - 6}" stroke="{MUTED}" '
                    f'stroke-width="1.8"/>')


def svg_doc(w, h):
    body = "".join(L["box"] + L["wire"] + L["dot"] + L["pin"] + L["text"])
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
            f'font-family="var(--font-sans, system-ui, sans-serif)"><rect width="100%" height="100%" fill="{BG}"/>'
            f'{body}</svg>')


# ================================================================== drawing 1: wiring
canvas()
W, H = 1700, 1300
RX = 1460  # right column (outside parts, ring, CAM)
X12, XG = 1200, 1220  # 12V / GND verticals on the right

t(30, 34, "센서 노드 배선도", 20, "start", FG, 700)
t(30, 56, "2026-11-21 · 핀은 초안(ARCHITECTURE.md 2.3) · 아직 실물로 배선·시험하지 않음 · "
          "핀 이름 뒤의 왼/오 n = 실제 헤더 위치(아래 핀 지도)", 12, "start", MUTED)

# ------------------------------------------------------------------ 12V terminal, fuse, rails
box(30, 80, 120, 70, "12V 단자 (KF301 2P)", "← 12V 어댑터", dashed=True, sub2="2A 예산 · 3A 여부 미정")
pin(150, 100, "+", "r")
pin(150, 130, "−", "r")
wire([(150, 100), (X12, 100)], "12V", 3.5)
wire([(150, 130), (XG, 130)], "GND", 3.5)
L["pin"].append(f'<rect x="163" y="93" width="34" height="14" fill="{CARD}" stroke="{C["12V"]}" stroke-width="2" '
                f'stroke-dasharray="4 3"/>')
L["pin"].append(f'<line x1="163" y1="100" x2="197" y2="100" stroke="{C["12V"]}" stroke-width="1.6"/>')
t(180, 88, "퓨즈 3~5A 권장", 11, "middle", MUTED)
t(560, 94, "12V +", 12, "start", C["12V"], 600)
t(560, 148, "12V − (전원 GND)", 12, "start", FG, 600)

# ------------------------------------------------------------------ pumps: P terminals + LR7843
names = ("물", "양액", "pH")
gpio = ("IO26", "IO27", "IO32")
VX12, VXG = 225, 245
P0, PSTEP = 220, 190
wire([(VX12, 100), (VX12, P0 + 2 * PSTEP + 25)], "12V", 3)
wire([(VXG, 130), (VXG, P0 + 2 * PSTEP + 95)], "GND", 3)
dot(VX12, 100, "12V")
dot(VXG, 130, "GND")
for i in range(3):
    y0 = P0 + i * PSTEP
    box(40, y0, 110, 100, f"P{i + 1} 단자", f"→ {names[i]} 펌프 (밖)", dashed=True)
    pin(150, y0 + 25, "+", "r")
    pin(150, y0 + 60, "−", "r")
    box(270, y0, 170, 120, f"LR7843 #{i + 1}")
    pin(270, y0 + 25, "+", "l")
    pin(270, y0 + 60, "LOAD", "l")
    pin(270, y0 + 95, "−", "l")
    pin(440, y0 + 35, "PWM", "r")
    pin(440, y0 + 85, "GND", "r")
    wire([(150, y0 + 25), (270, y0 + 25)], "12V", 2.8)
    dot(VX12, y0 + 25, "12V")
    wire([(270, y0 + 60), (150, y0 + 60)], "LOAD", 2.8)
    wire([(270, y0 + 95), (VXG, y0 + 95)], "GND", 2.8)
    dot(VXG, y0 + 95, "GND")
    diode_later(186, y0 + 25, y0 + 60)
    t(186, y0 + 80, "나중에", 10, "middle", MUTED)
    wire([(440, y0 + 35), (620, y0 + 35)], "CTL")
    gnd(440, y0 + 85)
    t(530, y0 + 29, gpio[i], 11, "middle", MUTED)

# ------------------------------------------------------------------ float switch (undecided)
box(40, 800, 110, 80, "수위 플로트 (미정)", "벽 단자 없음", dashed=True, sub2="NO/NC 미정")
pin(150, 825, "S", "r")
pin(150, 860, "G", "r")
gnd(150, 860)
vres(520, 752, 825, "3V3", "10kΩ")
L["pin"].append(f'<line x1="508" y1="752" x2="532" y2="752" stroke="{C["3V3"]}" stroke-width="2.4"/>')
t(520, 744, "3V3", 11, "middle", C["3V3"], 600)
wire([(150, 825), (620, 825)], "CTL", dash="7 5")
dot(520, 825, "CTL")
t(580, 819, "IO34", 11, "middle", MUTED)

# ------------------------------------------------------------------ devkit
L["box"].append(f'<rect x="620" y="200" width="240" height="860" rx="8" fill="{CARD}" stroke="{BORDER}" '
                f'stroke-width="1.5"/>')
t(740, 190, "ESP32 devkit 38핀", 14, "middle", FG, 600)
t(740, 690, "HG-ESP32-DEVKIT V4", 11, "middle", MUTED)
t(740, 706, "WROOM-32E (각인 확인)", 11, "middle", MUTED)
pin(830, 200, "", "l")
wire([(830, 200), (830, 130)], "GND", 3)
dot(830, 130, "GND")
t(838, 172, "GND 연결선 · 오1", 12, "start", FG, 600)
for y, lab in ((255, "IO26 · 왼10"), (445, "IO27 · 왼11"), (635, "IO32 · 왼7"), (825, "IO34 · 왼5"),
               (960, "5V ✕ 연결 안 함 · 왼19")):
    pin(620, y, lab, "l")
right_pins = [(240, "3V3 · 왼1"), (280, "IO21 SDA · 오6"), (310, "IO22 SCL · 오3"), (345, "GND · 오7"),
              (430, "IO16 RX2 · 오12"), (460, "IO17 TX2 · 오11"), (490, "IO4 DE/RE · 오13"), (600, "IO23 · 오2"),
              (780, "IO18 TX1 · 오9"), (810, "IO19 RX1 · 오8"), (850, "IO25 · 왼9"), (930, "IO33 · 왼8")]
for y, lab in right_pins:
    pin(860, y, lab, "r")
gnd(860, 345)
pin(740, 1060, "", "l")
t(748, 1052, "USB-C", 12)
wire([(740, 1060), (740, 1100)], "USB", 3)
box(650, 1100, 180, 54, "", None, dashed=True)
t(740, 1124, "5V 충전기", 13, "middle", FG, 600)
t(740, 1142, "A-to-C 케이블 (C-to-C ✕)", 11, "middle", MUTED)

# ------------------------------------------------------------------ 12V / GND verticals on the right
wire([(X12, 100), (X12, 990)], "12V", 3)
wire([(XG, 130), (XG, 1025)], "GND", 3)
dot(X12, 100, "12V")
dot(XG, 130, "GND")

# ------------------------------------------------------------------ I2C sensors on the devkit's 3V3
box(960, 170, 190, 100, "SCD41 (0x62)")
t(1142, 262, "뒷면에 직접 납땜", 11, "end", MUTED)
for y, lab in ((190, "VIN"), (230, "SDA"), (250, "SCL")):
    pin(960, y, lab, "l")
pin(1150, 210, "GND", "r")
gnd(1150, 210)
box(960, 295, 190, 100, "MLX90640 55° (0x33)")
for y, lab in ((315, "VIN"), (355, "SDA"), (380, "SCL")):
    pin(960, y, lab, "l")
pin(1150, 340, "GND", "r")
gnd(1150, 340)
wire([(880, 190), (880, 520)], "3V3")
wire([(900, 230), (900, 355)], "I2C")
wire([(920, 250), (920, 380)], "I2C")
wire([(860, 240), (880, 240)], "3V3")
dot(880, 240, "3V3")
for y in (190, 315, 520):
    wire([(880, y), (960, y)], "3V3")
    dot(880, y, "3V3")
wire([(860, 280), (900, 280)], "I2C")
dot(900, 280, "I2C")
for y in (230, 355):
    wire([(900, y), (960, y)], "I2C")
    dot(900, y, "I2C")
wire([(860, 310), (920, 310)], "I2C")
dot(920, 310, "I2C")
for y in (250, 380):
    wire([(920, y), (960, y)], "I2C")
    dot(920, y, "I2C")

# ------------------------------------------------------------------ RS485: MAX485 (3.3V) -> 485 terminal -> SEN0604 (12V)
box(960, 415, 190, 125, "MAX485 (3.3V)", "120Ω 종단 그대로")
for y, lab in ((430, "RO"), (460, "DI"), (490, "DE+RE"), (520, "VCC")):
    pin(960, y, lab, "l")
for y, lab in ((465, "A"), (495, "B"), (525, "GND")):
    pin(1150, y, lab, "r")
gnd(1150, 525)
wire([(860, 430), (960, 430)], "UART")
wire([(860, 460), (960, 460)], "UART")
wire([(860, 490), (960, 490)], "CTL")
box(1250, 405, 150, 140, "485 단자 (KF301 4P)", None, dashed=True)
for y, lab in ((420, "V"), (440, "G"), (465, "A"), (495, "B")):
    pin(1250, y, lab, "l")
    pin(1400, y, lab, "r")
wire([(X12, 420), (1250, 420)], "12V", 2.8)
dot(X12, 420, "12V")
wire([(XG, 440), (1250, 440)], "GND", 2.8)
dot(XG, 440, "GND")
wire([(1150, 465), (1250, 465)], "A")
wire([(1150, 495), (1250, 495)], "B")
box(RX, 405, 200, 140, "SEN0604 토양 센서", None, dashed=True)
for k, s in enumerate(("케이스 밖 · 2 m", "12V (5~30V)", "Modbus 0x01", "9600 8N1")):
    t(RX + 190, 474 + 16 * k, s, 11, "end", MUTED)
for y, lab, net in ((420, "갈색 V", "brown"), (440, "검정 G", "black"), (465, "노랑 A", "yellow"),
                    (495, "파랑 B", "blue")):
    pin(RX, y, lab, "l")
    wire([(1400, y), (RX, y)], net, 2.6)

# ------------------------------------------------------------------ NeoPixel ring + buck 1
box(1250, 635, 150, 80, "강압 1 (5V, 링 전용)")
pin(1250, 655, "IN+", "l")
pin(1250, 690, "IN−", "l")
pin(1400, 655, "OUT+", "r")
pin(1400, 690, "OUT−", "r")
wire([(X12, 655), (1250, 655)], "12V", 2.8)
dot(X12, 655, "12V")
wire([(XG, 690), (1250, 690)], "GND", 2.8)
dot(XG, 690, "GND")
box(RX, 585, 200, 125, "NeoPixel Ring 24 (1586)", "링 5V에 220~470µF 권장")
for k, s in enumerate(("흰색 최대 1.44A", "470Ω 내장", "밝기 제한 없음")):
    t(RX + 190, 625 + 16 * k, s, 11, "end", MUTED)
for y, lab in ((600, "DIN"), (655, "5V"), (690, "GND")):
    pin(RX, y, lab, "l")
wire([(860, 600), (RX, 600)], "DIN")
wire([(1400, 655), (RX, 655)], "5V", 2.8)
wire([(1400, 690), (RX, 690)], "GND", 2.8)
cx = 1430  # recommended electrolytic across the ring's 5V
s = f'stroke="{MUTED}" stroke-width="1.6" stroke-dasharray="3 3"'
L["pin"].append(f'<line x1="{cx}" y1="655" x2="{cx}" y2="668" {s}/>')
L["pin"].append(f'<line x1="{cx}" y1="677" x2="{cx}" y2="690" {s}/>')
L["pin"].append(f'<line x1="{cx - 9}" y1="668" x2="{cx + 9}" y2="668" stroke="{MUTED}" stroke-width="2"/>')
L["pin"].append(f'<path d="M{cx - 9},679 Q{cx},673 {cx + 9},679" fill="none" stroke="{MUTED}" stroke-width="2"/>')

# ------------------------------------------------------------------ ESP32-CAM: buck 2, relay-upload lines
res(860, 960, 780, "UART")
wire([(960, 780), (RX, 780)], "UART")
res(860, 960, 810, "UART")
wire([(960, 810), (RX, 810)], "UART")
res(860, 960, 850)
res(860, 960, 930)
for k, ya in enumerate((850, 930)):
    box(990, ya - 15, 130, 50, f"PC817C #{k + 1}")
    pin(990, ya, "1 A", "l")
    pin(990, ya + 25, "2 K", "l")
    pin(1120, ya, "4 C", "r")
    pin(1120, ya + 25, "3 E", "r")
    wire([(960, ya), (990, ya)], "CTL")
    gnd(990, ya + 25, side="l", stub=14)
    gnd(1120, ya + 25, stub=14)
    wire([(1120, ya), (RX, ya)], "CTL")
box(1250, 970, 150, 80, "강압 2 (5V, CAM)")
pin(1250, 990, "IN+", "l")
pin(1250, 1025, "IN−", "l")
pin(1400, 990, "OUT+", "r")
pin(1400, 1025, "OUT−", "r")
wire([(X12, 990), (1250, 990)], "12V", 2.8)
dot(X12, 990, "12V")
wire([(XG, 1025), (1250, 1025)], "GND", 2.8)
dot(XG, 1025, "GND")
box(RX, 765, 200, 280, "ESP32-CAM", "U0R·U0T·IO0·EN = 중계 업로드용")
for y, lab in ((780, "U0R"), (810, "U0T"), (850, "IO0"), (930, "EN (RST 버튼 패드)"), (990, "5V"),
               (1025, "GND")):
    pin(RX, y, lab, "l")
wire([(1400, 990), (RX, 990)], "5V", 2.8)
wire([(1400, 1025), (RX, 1025)], "GND", 2.8)

# ------------------------------------------------------------------ legend
lx, ly = 30, 935
L["box"].append(f'<rect x="{lx}" y="{ly}" width="570" height="345" rx="8" fill="none" stroke="{BORDER}" '
                f'stroke-width="1.2"/>')
t(lx + 16, ly + 26, "범례", 14, "start", FG, 600)
items = [("12V", "12V + (어댑터)", 3.5), ("GND", "12V − / GND", 3.5), ("5V", "강압 5V", 2.8),
         ("USB", "USB 5V (devkit 전용)", 2.8), ("3V3", "devkit 3.3V", 2.2), ("I2C", "I2C SDA / SCL", 2.2),
         ("UART", "UART (MAX485, CAM)", 2.2), ("A", "RS485 A", 2.2), ("B", "RS485 B", 2.2),
         ("DIN", "링 데이터", 2.2), ("LOAD", "펌프 LOAD (펌프 −)", 2.8), ("CTL", "제어 (PWM, DE, PC817)", 2.2)]
for k, (net, lab, w) in enumerate(items):
    ix = lx + 16 + (k % 2) * 280
    iy = ly + 52 + (k // 2) * 24
    wire([(ix, iy), (ix + 34, iy)], net, w, halo=False)
    t(ix + 44, iy + 4, lab, 12)
ny = ly + 198
dot(lx + 30, ny - 4, "GND")
t(lx + 50, ny, "연결점. 점 없이 끊겨 지나가는 선은 연결 아님", 12)
gnd(lx + 14, ny + 14, stub=16)
t(lx + 50, ny + 25, "devkit GND (= GND 연결선으로 12V −와 같음)", 12)
L["box"].append(f'<rect x="{lx + 16}" y="{ny + 38}" width="28" height="16" rx="3" fill="none" stroke="{BORDER}" '
                f'stroke-width="1.5" stroke-dasharray="6 4"/>')
t(lx + 50, ny + 50, "벽 단자·케이스 밖 부품. 점선 부품은 권장·나중에·미정", 12)
for k, n in enumerate(("devkit 5V 핀은 연결 안 함 → 역류 방지 다이오드 불필요",
                       "플라이백 다이오드 지금 생략 → 펌프는 켜고 끄기만, PWM 금지",
                       "펌프는 한 번에 하나, 링 최대 촬영 중 정지 (12V 예산)",
                       "25·33번은 평소 LOW. HIGH면 CAM IO0 / EN이 당겨짐")):
    t(lx + 16, ny + 74 + k * 20, n, 12)

# ------------------------------------------------------------------ open items
ox, oy = 880, 1095
L["box"].append(f'<rect x="{ox}" y="{oy}" width="790" height="185" rx="8" fill="none" stroke="{BORDER}" '
                f'stroke-width="1.2"/>')
t(ox + 16, oy + 26, "아직 안 정한 것 · 실물로 볼 것", 14, "start", FG, 600)
for k, n in enumerate(("12V 어댑터: 2A(예산, README 주의) / 3A 이상(ARCHITECTURE 2.3 전원 문단) 중 하나로",
                       "펌프 3개: 모델·전류·ml/s 미정. 12V, 정격 0.8A 이하로 고를 것",
                       "플로트: 달지, NO/NC, 벽 단자(지금 없음) 미정. 달면 IO34 + 10kΩ 풀업",
                       "devkit 모듈 각인: WROOM-32E면 16/17 그대로, WROVER면 RS485 핀을 바꿈",
                       "강압 모듈 2개: 배선 전에 무부하 출력이 5V인지 (3.3V판과 겉모양 같음)",
                       "펌프 안전 상수(1회 최대 가동, 하루 최대 물 양, 잠금 시간) 숫자")):
    t(ox + 16, oy + 52 + k * 21, n, 12)

wiring_svg = svg_doc(W, H)

# ================================================================== drawing 2: devkit header map
canvas()
W2, H2 = 1100, 990
PX_L, PX_R, ROW0, PITCH = 420, 680, 190, 30
t(30, 34, "devkit 핀 지도", 20, "start", FG, 700)
t(30, 56, "하이제니스 HG-ESP32-DEVKIT V4 (판매 글: DevKitC V4와 같은 핀맵). 부품 면에서 보고 안테나가 위, "
          "USB-C가 아래. 번호는 안테나 쪽부터 1~19", 12, "start", MUTED)
L["box"].append(f'<rect x="400" y="110" width="300" height="710" rx="10" fill="{CARD}" stroke="{BORDER}" '
                f'stroke-width="1.8"/>')
L["box"].append(f'<rect x="500" y="80" width="100" height="180" rx="4" fill="none" stroke="{BORDER}" '
                f'stroke-width="1.5"/>')
t(550, 102, "안테나", 11, "middle", MUTED)
t(550, 170, "WROOM", 11, "middle", MUTED)
t(550, 185, "-32E", 11, "middle", MUTED)
L["box"].append(f'<rect x="520" y="790" width="60" height="40" rx="5" fill="none" stroke="{BORDER}" '
                f'stroke-width="1.5"/>')
t(550, 815, "USB-C", 11, "middle", MUTED)
for bx, lab in ((440, "EN"), (630, "BOOT")):
    L["box"].append(f'<rect x="{bx}" y="770" width="30" height="22" rx="3" fill="none" stroke="{BORDER}" '
                    f'stroke-width="1.5"/>')
    t(bx + 15, 808, lab, 10, "middle", MUTED)

FLASH = "플래시 · 쓰지 말 것"
LEFT = [("3V3", "3V3", "SCD41·MLX90640·MAX485 VCC, 플로트 10kΩ 풀업"), ("EN", None, "리셋 · 비움"),
        ("VP 36", None, "입력 전용 · 예비"), ("VN 39", None, "입력 전용 · 예비"),
        ("IO34", "CTL", "수위 플로트 (미정) · 10kΩ 풀업"), ("IO35", None, "입력 전용 · 예비"),
        ("IO32", "CTL", "LR7843 #3 PWM → P3 pH 펌프"), ("IO33", "CTL", "1kΩ → PC817C #2 → CAM EN"),
        ("IO25", "CTL", "1kΩ → PC817C #1 → CAM IO0"), ("IO26", "CTL", "LR7843 #1 PWM → P1 물 펌프"),
        ("IO27", "CTL", "LR7843 #2 PWM → P2 양액 펌프"), ("IO14", None, "예비"), ("IO12", None, "부팅 핀 · 비움"),
        ("GND", "GND", "LR7843 ×3 신호 GND, PC817C 2·3번"), ("IO13", None, "예비"), ("SD2", None, FLASH),
        ("SD3", None, FLASH), ("CMD", None, FLASH), ("5V", "X", "✕ 연결 안 함 (USB 5V와 강압 5V를 떼어 둠)")]
RIGHT = [("GND", "GND", "GND 연결선 → 12V − (꼭)"), ("IO23", "DIN", "링 DIN"),
         ("IO22", "I2C", "SCL → SCD41, MLX90640"), ("TX0", None, "USB 시리얼 (CP2102N) · 비움"),
         ("RX0", None, "USB 시리얼 (CP2102N) · 비움"), ("IO21", "I2C", "SDA → SCD41, MLX90640"),
         ("GND", "GND", "SCD41·MLX90640·MAX485 GND"), ("IO19", "UART", "RX1 ← 1kΩ ← CAM U0T"),
         ("IO18", "UART", "TX1 → 1kΩ → CAM U0R"), ("IO5", None, "부팅 핀 · 비움"),
         ("IO17", "UART", "TX2 → MAX485 DI"), ("IO16", "UART", "RX2 ← MAX485 RO"),
         ("IO4", "CTL", "MAX485 DE+RE (HIGH = 송신)"), ("IO0", None, "BOOT 버튼 · 비움"),
         ("IO2", None, "부팅 핀 · 비움"), ("IO15", None, "부팅 핀 · 비움"), ("SD1", None, FLASH),
         ("SD0", None, FLASH), ("CLK", None, FLASH)]


def header_row(side, i, name, net, dest):
    y = ROW0 + i * PITCH
    px = PX_L if side == "l" else PX_R
    sgn = -1 if side == "l" else 1
    used = net not in (None, "X")
    if used:
        wire([(px, y), (px + sgn * 90, y)], net, 2.6, dash="7 5" if name == "IO34" else None, halo=False)
        L["pin"].append(f'<circle cx="{px}" cy="{y}" r="6" fill="{C[net]}"/>')
    elif net == "X":
        L["pin"].append(f'<circle cx="{px}" cy="{y}" r="6" fill="none" stroke="{C["12V"]}" stroke-width="2"/>')
    else:
        L["pin"].append(f'<circle cx="{px}" cy="{y}" r="5" fill="none" stroke="{MUTED}" stroke-width="1.5"/>')
    num_x, name_x = (px + 12, px + 30) if side == "l" else (px - 12, px - 30)
    anchor_in = "start" if side == "l" else "end"
    t(num_x, y + 4, str(i + 1), 10, anchor_in, MUTED)
    t(name_x, y + 4, name, 12, anchor_in, FG if net else MUTED, 600 if used else 400)
    tx = px + sgn * 98
    color = C["12V"] if net == "X" else (FG if used else MUTED)
    t(tx, y + 4, dest, 12, "end" if side == "l" else "start", color, 600 if net == "X" else 400)


for i, (name, net, dest) in enumerate(LEFT):
    header_row("l", i, name, net, dest)
for i, (name, net, dest) in enumerate(RIGHT):
    header_row("r", i, name, net, dest)

for k, n in enumerate(("색 있는 선 = 쓰는 핀(색은 배선도와 같음). 빈 회색 동그라미 = 비움.",
                       "SD0~3·CMD·CLK는 모듈 플래시가 쓰는 핀이라 손대면 안 됩니다. 0·2·5·12·15는 켤 때 상태를 보는 "
                       "부팅 핀이라 비워 둡니다.",
                       "남는 핀: IO13, IO14 (입출력), IO35·VP 36·VN 39 (입력 전용).",
                       "GND는 세 개를 나눠 씁니다: 오1 = 12V − 연결선, 오7 = 센서·MAX485, 왼14 = 펌프 모듈·PC817C.",
                       "납땜 전에 보드 실크 글자와 한 번 맞춰 보세요.")):
    t(30, 870 + k * 22, n, 12, "start", FG if k else MUTED)

pins_svg = svg_doc(W2, H2)

# ================================================================== write
html = ("<!doctype html><html lang=\"ko\"><head><meta charset=\"utf-8\"><title>센서 노드 배선도</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body>"
        "<p style=\"margin:0 0 8px;font-size:13px;color:var(--muted-foreground,#6b7280)\">"
        "센서 노드 배선도와 devkit 핀 지도 · 가로로 스크롤해서 보세요</p>"
        "<div style=\"overflow-x:auto\">" + wiring_svg + "</div>"
        "<div style=\"overflow-x:auto;margin-top:24px\">" + pins_svg + "</div></body></html>")


def standalone(s):
    for var, val in (("var(--foreground, #111827)", "#111827"), ("var(--muted-foreground, #6b7280)", "#6b7280"),
                     ("var(--card, #ffffff)", "#ffffff"), ("var(--border, #d1d5db)", "#d1d5db"),
                     ("var(--background, #ffffff)", "#ffffff"),
                     ("var(--font-sans, system-ui, sans-serif)", "sans-serif")):
        s = s.replace(var, val)
    return s


OUT.mkdir(exist_ok=True)
(OUT / "wiring.html").write_text(html, encoding="utf-8")
(OUT / "_cache").mkdir(exist_ok=True)
(OUT / "_cache" / "wiring_fit.html").write_text(
    html.replace(f'width="{W}" height="{H}"', 'width="100%"').replace(f'width="{W2}" height="{H2}"', 'width="100%"'),
    encoding="utf-8")
(OUT / "wiring.svg").write_text(standalone(wiring_svg), encoding="utf-8")
(OUT / "devkit_pins.svg").write_text(standalone(pins_svg), encoding="utf-8")
print(OUT / "wiring.html")
