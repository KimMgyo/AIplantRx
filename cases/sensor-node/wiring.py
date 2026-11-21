#!/usr/bin/env python3
"""Wiring diagram of the sensor node (ARCHITECTURE.md 2.3 + README wiring) -> out/wiring.html.

Two power domains: the devkit runs from its own USB-C (its 3V3 feeds SCD41, MLX90640, MAX485); 12V feeds two
5 V bucks (ring, ESP32-CAM), the SEN0604 and the LR7843 / pumps.  One wire joins devkit GND and 12V -.
Colours use theme variables with fallbacks so the same file works inline in T3 and stand-alone."""
from __future__ import annotations

from pathlib import Path

OUT = Path(__file__).resolve().parent / "out"

FG = "var(--foreground, #111827)"
MUTED = "var(--muted-foreground, #6b7280)"
CARD = "var(--card, #ffffff)"
BORDER = "var(--border, #d1d5db)"
C = {  # net colours
    "12V": "#dc2626", "GND": FG, "5V": "#ea580c", "USB": "#7c3aed", "3V3": "#2563eb", "I2C": "#16a34a",
    "UART": "#0891b2", "A": "#ca8a04", "B": "#1d4ed8", "DIN": "#db2777", "LOAD": "#92400e", "CTL": "#64748b",
    "brown": "#92400e", "black": FG, "yellow": "#ca8a04", "blue": "#1d4ed8",
}
W, H = 1690, 1090
RX = 1460  # right column (outside parts, ring, CAM)
svg: list[str] = []


def t(x, y, s, size=13, anchor="start", color=FG, weight=400):
    svg.append(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{color}" '
               f'font-weight="{weight}">{s}</text>')


def box(x, y, w, h, title, sub=None, dashed=False):
    dash = ' stroke-dasharray="6 4"' if dashed else ""
    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{CARD}" stroke="{BORDER}" '
               f'stroke-width="1.5"{dash}/>')
    t(x + w / 2, y - 8, title, 14, "middle", FG, 600)
    if sub:
        t(x + w / 2, y + h + 17, sub, 12, "middle", MUTED)


def pin(x, y, label, side):
    """A pin on a box edge; the label sits inside the box."""
    svg.append(f'<circle cx="{x}" cy="{y}" r="3" fill="{FG}"/>')
    dx = 8 if side == "l" else -8
    t(x + dx, y + 4, label, 12, "start" if side == "l" else "end", FG)


def wire(pts, net, width=2.2):
    p = " ".join(f"{a},{b}" for a, b in pts)
    svg.append(f'<polyline points="{p}" fill="none" stroke="{C[net]}" stroke-width="{width}" '
               f'stroke-linejoin="round" stroke-linecap="round"/>')


def dot(x, y, net):
    svg.append(f'<circle cx="{x}" cy="{y}" r="4.5" fill="{C[net]}"/>')


def gnd(x, y, side="r", stub=16):
    """Signal ground symbol: goes to devkit GND (= 12V - through the link wire)."""
    x2 = x + stub if side == "r" else x - stub
    wire([(x, y), (x2, y), (x2, y + 8)], "GND", 1.8)
    for k, hw in enumerate((8, 5, 2)):
        svg.append(f'<line x1="{x2 - hw}" y1="{y + 8 + 4 * k}" x2="{x2 + hw}" y2="{y + 8 + 4 * k}" '
                   f'stroke="{FG}" stroke-width="1.8"/>')


def res(x0, x1, y, net="CTL", label="1kΩ"):
    xm = (x0 + x1) / 2
    wire([(x0, y), (xm - 18, y)], net)
    wire([(xm + 18, y), (x1, y)], net)
    svg.append(f'<rect x="{xm - 18}" y="{y - 6}" width="36" height="12" fill="{CARD}" stroke="{C[net]}" '
               f'stroke-width="2"/>')
    t(xm, y - 10, label, 11, "middle", MUTED)


# ------------------------------------------------------------------ rails and the 12V terminal
box(30, 30, 145, 70, "12V 단자 (KF301 2P)", "← 12V 2A 어댑터", dashed=True)
pin(175, 50, "+", "r")
pin(175, 80, "−", "r")
wire([(175, 50), (1200, 50)], "12V", 3.5)
wire([(175, 80), (1220, 80)], "GND", 3.5)
t(560, 44, "12V +", 12, "start", C["12V"], 600)
t(560, 98, "12V − (전원 GND)", 12, "start", FG, 600)

# ------------------------------------------------------------------ pumps: P terminals + LR7843
names = ("물", "양액", "pH")
gpio = ("IO26", "IO27", "IO32")
wire([(215, 50), (215, 170 + 2 * 190 + 25)], "12V", 3)
wire([(235, 80), (235, 170 + 2 * 190 + 95)], "GND", 3)
dot(215, 50, "12V")
dot(235, 80, "GND")
for i in range(3):
    y0 = 170 + i * 190
    box(40, y0, 130, 100, f"P{i + 1} 단자", f"→ {names[i]} 펌프 (케이스 밖)", dashed=True)
    pin(170, y0 + 25, "+  (펌프 +)", "r")
    pin(170, y0 + 60, "−  (펌프 −)", "r")
    box(260, y0, 180, 120, f"LR7843 #{i + 1}")
    pin(260, y0 + 25, "+", "l")
    pin(260, y0 + 60, "LOAD", "l")
    pin(260, y0 + 95, "−", "l")
    pin(440, y0 + 35, "PWM", "r")
    pin(440, y0 + 85, "GND", "r")
    wire([(170, y0 + 25), (260, y0 + 25)], "12V", 2.8)
    dot(215, y0 + 25, "12V")
    wire([(260, y0 + 60), (170, y0 + 60)], "LOAD", 2.8)
    wire([(260, y0 + 95), (235, y0 + 95)], "GND", 2.8)
    dot(235, y0 + 95, "GND")
    wire([(440, y0 + 35), (600, y0 + 35)], "CTL")
    gnd(440, y0 + 85)
    t(520, y0 + 29, gpio[i], 11, "middle", MUTED)

# ------------------------------------------------------------------ devkit
svg.append(f'<rect x="600" y="150" width="240" height="810" rx="8" fill="{CARD}" stroke="{BORDER}" stroke-width="1.5"/>')
t(690, 140, "ESP32 devkit 38핀 (USB-C)", 14, "middle", FG, 600)
pin(800, 150, "", "l")
t(792, 170, "GND", 12, "end")
wire([(800, 150), (800, 80)], "GND", 3)
dot(800, 80, "GND")
t(808, 122, "GND 연결선 (꼭)", 12, "start", FG, 600)
for y, lab in ((205, "IO26"), (395, "IO27"), (585, "IO32")):
    pin(600, y, lab, "l")
pin(600, 880, "5V  ✕ 연결 안 함", "l")
pin(720, 960, "", "l")
t(728, 952, "USB-C", 12)
wire([(720, 960), (720, 1000)], "USB", 3)
box(630, 1000, 180, 54, "", None, dashed=True)
t(720, 1024, "5V 충전기", 13, "middle", FG, 600)
t(720, 1042, "A-to-C 케이블 (C-to-C ✕)", 11, "middle", MUTED)
right_pins = [(190, "3V3"), (230, "IO21 SDA"), (260, "IO22 SCL"), (295, "GND"), (380, "IO16 RX2"),
              (410, "IO17 TX2"), (440, "IO4 DE/RE"), (520, "IO23"), (680, "IO18 TX1"), (710, "IO19 RX1"),
              (750, "IO25"), (830, "IO33")]
for y, lab in right_pins:
    pin(840, y, lab, "r")
gnd(840, 295)

# ------------------------------------------------------------------ I2C sensors on the devkit's 3V3
box(960, 120, 190, 100, "SCD41")
t(1142, 212, "뒷면에 직접 납땜", 11, "end", MUTED)
for y, lab in ((140, "VIN"), (180, "SDA"), (200, "SCL")):
    pin(960, y, lab, "l")
pin(1150, 160, "GND", "r")
gnd(1150, 160)
box(960, 245, 190, 100, "MLX90640 55°")
for y, lab in ((265, "VIN"), (305, "SDA"), (330, "SCL")):
    pin(960, y, lab, "l")
pin(1150, 290, "GND", "r")
gnd(1150, 290)
wire([(840, 190), (880, 190)], "3V3")
wire([(880, 140), (880, 465)], "3V3")
dot(880, 190, "3V3")
for y in (140, 265, 465):
    wire([(880, y), (960, y)], "3V3")
    dot(880, y, "3V3")
wire([(840, 230), (900, 230)], "I2C")
wire([(900, 180), (900, 305)], "I2C")
dot(900, 230, "I2C")
for y in (180, 305):
    wire([(900, y), (960, y)], "I2C")
    dot(900, y, "I2C")
wire([(840, 260), (920, 260)], "I2C")
wire([(920, 200), (920, 330)], "I2C")
dot(920, 260, "I2C")
for y in (200, 330):
    wire([(920, y), (960, y)], "I2C")
    dot(920, y, "I2C")

# ------------------------------------------------------------------ RS485: MAX485 (3.3V) -> 485 terminal -> SEN0604 (12V)
box(960, 365, 190, 120, "MAX485 (3.3V)", "120Ω 종단 그대로")
for y, lab in ((380, "RO"), (410, "DI"), (440, "DE+RE"), (465, "VCC")):
    pin(960, y, lab, "l")
for y, lab in ((400, "A"), (430, "B"), (465, "GND")):
    pin(1150, y, lab, "r")
gnd(1150, 465)
wire([(840, 380), (960, 380)], "UART")
wire([(840, 410), (960, 410)], "UART")
wire([(840, 440), (960, 440)], "CTL")
box(1250, 360, 150, 130, "485 단자 (KF301 4P)", None, dashed=True)
for y, lab in ((380, "V"), (400, "A"), (430, "B"), (465, "G")):
    pin(1250, y, lab, "l")
    pin(1400, y, lab, "r")
wire([(1150, 400), (1250, 400)], "A")
wire([(1150, 430), (1250, 430)], "B")
box(RX, 355, 200, 140, "SEN0604 토양 센서", None, dashed=True)
t(RX + 190, 400, "케이스 밖", 11, "end", MUTED)
t(RX + 190, 416, "2 m 케이블", 11, "end", MUTED)
t(RX + 190, 432, "5~30V", 11, "end", MUTED)
for y, lab, net in ((380, "갈색 V", "brown"), (400, "노랑 A", "yellow"), (430, "파랑 B", "blue"),
                    (465, "검정 G", "black")):
    pin(RX, y, lab, "l")
    wire([(1400, y), (RX, y)], net, 2.6)

# ------------------------------------------------------------------ 12V bus on the right: 485 V, buck 1, buck 2
wire([(1200, 50), (1200, 890)], "12V", 3)
wire([(1220, 80), (1220, 925)], "GND", 3)
dot(1200, 50, "12V")
dot(1220, 80, "GND")
wire([(1200, 380), (1250, 380)], "12V", 2.8)
dot(1200, 380, "12V")
wire([(1220, 465), (1250, 465)], "GND", 2.8)
dot(1220, 465, "GND")

# ------------------------------------------------------------------ NeoPixel ring + buck 1
box(1250, 555, 150, 80, "강압 1 (5V, 링 전용)")
pin(1250, 575, "IN+", "l")
pin(1250, 610, "IN−", "l")
pin(1400, 575, "OUT+", "r")
pin(1400, 610, "OUT−", "r")
wire([(1200, 575), (1250, 575)], "12V", 2.8)
dot(1200, 575, "12V")
wire([(1220, 610), (1250, 610)], "GND", 2.8)
dot(1220, 610, "GND")
box(RX, 505, 200, 125, "NeoPixel Ring 24")
t(RX + 190, 545, "최대 1.44A", 11, "end", MUTED)
t(RX + 190, 561, "470Ω 내장", 11, "end", MUTED)
for y, lab in ((520, "DIN"), (575, "5V"), (610, "GND")):
    pin(RX, y, lab, "l")
wire([(840, 520), (RX, 520)], "DIN")
wire([(1400, 575), (RX, 575)], "5V", 2.8)
wire([(1400, 610), (RX, 610)], "GND", 2.8)

# ------------------------------------------------------------------ ESP32-CAM: buck 2, relay-upload lines
res(840, 960, 680, "UART")
wire([(960, 680), (RX, 680)], "UART")
res(840, 960, 710, "UART")
wire([(960, 710), (RX, 710)], "UART")
res(840, 960, 750)
res(840, 960, 830)
for k, (ya, lab) in enumerate(((750, "IO0"), (830, "EN"))):
    box(990, ya - 15, 130, 50, f"PC817C #{k + 1}")
    pin(990, ya, "1 A", "l")
    pin(990, ya + 25, "2 K", "l")
    pin(1120, ya, "4 C", "r")
    pin(1120, ya + 25, "3 E", "r")
    wire([(960, ya), (990, ya)], "CTL")
    gnd(990, ya + 25, side="l", stub=14)
    gnd(1120, ya + 25, stub=14)
    wire([(1120, ya), (RX, ya)], "CTL")
box(1250, 870, 150, 80, "강압 2 (5V, CAM)")
pin(1250, 890, "IN+", "l")
pin(1250, 925, "IN−", "l")
pin(1400, 890, "OUT+", "r")
pin(1400, 925, "OUT−", "r")
wire([(1200, 890), (1250, 890)], "12V", 2.8)
dot(1200, 890, "12V")
wire([(1220, 925), (1250, 925)], "GND", 2.8)
dot(1220, 925, "GND")
box(RX, 665, 200, 280, "ESP32-CAM", "U0R·U0T·IO0·EN = 중계 업로드용")
for y, lab in ((680, "U0R"), (710, "U0T"), (750, "IO0"), (830, "EN (RST 버튼 패드)"), (890, "5V"),
               (925, "GND")):
    pin(RX, y, lab, "l")
wire([(1400, 890), (RX, 890)], "5V", 2.8)
wire([(1400, 925), (RX, 925)], "GND", 2.8)

# ------------------------------------------------------------------ legend
lx, ly = 40, 720
svg.append(f'<rect x="{lx}" y="{ly}" width="520" height="320" rx="8" fill="none" stroke="{BORDER}" '
           f'stroke-width="1.2"/>')
t(lx + 16, ly + 26, "범례", 14, "start", FG, 600)
items = [("12V", "12V + (어댑터)", 3.5), ("GND", "12V − / GND", 3.5), ("5V", "강압 5V", 2.8),
         ("USB", "USB 5V (devkit 전용)", 2.8), ("3V3", "devkit 3.3V", 2.2), ("I2C", "I2C SDA / SCL", 2.2),
         ("UART", "UART (MAX485, CAM)", 2.2), ("A", "RS485 A", 2.2), ("B", "RS485 B", 2.2),
         ("DIN", "링 데이터", 2.2), ("LOAD", "펌프 LOAD", 2.8), ("CTL", "제어 (PWM, DE, PC817)", 2.2)]
for k, (net, lab, w) in enumerate(items):
    cx = lx + 16 + (k % 2) * 250
    cy = ly + 50 + (k // 2) * 24
    wire([(cx, cy), (cx + 34, cy)], net, w)
    t(cx + 44, cy + 4, lab, 12)
notes = ["● = 연결점. 점 없이 지나가는 선은 연결 아님",
         "⏚ = devkit GND (= 맨 위 GND 연결선으로 12V −와 같음)",
         "devkit 5V 핀은 연결 안 함 → 역류 방지 다이오드 불필요",
         "펌프 플라이백 다이오드 생략 → 펌프는 켜고 끄기만, PWM 금지",
         "펌프는 한 번에 하나, 링 최대 촬영 중 정지 (12V 2A)"]
for k, n in enumerate(notes):
    t(lx + 16, ly + 210 + k * 21, n, 12, "start", FG)

body = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
        f'font-family="var(--font-sans, system-ui, sans-serif)">' + "".join(svg) + "</svg>")
html = ("<!doctype html><html lang=\"ko\"><head><meta charset=\"utf-8\"><title>센서 노드 배선도</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body>"
        "<p style=\"margin:0 0 8px;font-size:13px;color:var(--muted-foreground,#6b7280)\">"
        "센서 노드 배선도 · 가로로 스크롤해서 보세요</p>"
        "<div style=\"overflow-x:auto\">" + body + "</div></body></html>")
OUT.mkdir(exist_ok=True)
(OUT / "wiring.html").write_text(html, encoding="utf-8")
(OUT / "_cache").mkdir(exist_ok=True)
(OUT / "_cache" / "wiring_fit.html").write_text(html.replace(f'width="{W}" height="{H}"', 'width="100%"'), encoding="utf-8")
(OUT / "wiring.svg").write_text(body.replace("var(--foreground, #111827)", "#111827")
                                .replace("var(--muted-foreground, #6b7280)", "#6b7280")
                                .replace("var(--card, #ffffff)", "#ffffff")
                                .replace("var(--border, #d1d5db)", "#d1d5db")
                                .replace("var(--font-sans, system-ui, sans-serif)", "sans-serif"),
                                encoding="utf-8")
print(OUT / "wiring.html")
