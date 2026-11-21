#!/usr/bin/env python3
"""Wiring diagram of the main display (ARCHITECTURE.md 2.2 + README 배선 메모) -> out/wiring.html, out/wiring.svg.

One input: the C3 SuperMini's USB-C.  C3 5V/GND power the Waveshare S3 board through its I2C header (10, VCC
set to 5V with resistor 18); C3 UART (20/21) talks to the S3's UART2 header (7, switch 15 on UART2); C3 I2C
(5/6, 3.3V) goes to the TCA9548A, whose channels 2..6 drive the five SSD1306 OLEDs (all 0x3C; OLED 3 on channel 4 is a yellow/blue one).
Circled numbers are the labels on Waveshare's onboard-resources picture (research/waveshare).
Colours use theme variables with fallbacks so the same file works inline in T3 and stand-alone."""
from __future__ import annotations

from pathlib import Path

OUT = Path(__file__).resolve().parent / "out"

FG = "var(--foreground, #111827)"
MUTED = "var(--muted-foreground, #6b7280)"
CARD = "var(--card, #ffffff)"
BORDER = "var(--border, #d1d5db)"
C = {"5V": "#ea580c", "GND": FG, "USB": "#7c3aed", "3V3": "#2563eb", "SDA": "#16a34a", "SCL": "#16a34a",
     "UART": "#0891b2"}
DASH = {"SCL": "7 4"}
W, H, TOP = 1480, 1060, 90           # TOP: empty band above the charger box
svg: list[str] = []


def t(x, y, s, size=13, anchor="start", color=FG, weight=400):
    svg.append(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{color}" '
               f'font-weight="{weight}">{s}</text>')


def box(x, y, w, h, title, sub=None, dashed=False):
    dash = ' stroke-dasharray="6 4"' if dashed else ""
    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{CARD}" stroke="{BORDER}" '
               f'stroke-width="1.5"{dash}/>')
    if title:
        t(x + w / 2, y - 8, title, 14, "middle", FG, 600)
    if sub:
        t(x + w / 2, y + h + 17, sub, 12, "middle", MUTED)


def pin(x, y, label, side, color=FG, used=True):
    """A pin on a box edge; the label sits inside the box.  Unused pins are hollow and muted."""
    fill = FG if used else CARD
    svg.append(f'<circle cx="{x}" cy="{y}" r="3" fill="{fill}" stroke="{FG if used else MUTED}" stroke-width="1.2"/>')
    dx = 8 if side == "l" else -8
    t(x + dx, y + 4, label, 12, "start" if side == "l" else "end", color if used else MUTED)


def wire(pts, net, width=2.2):
    p = " ".join(f"{a},{b}" for a, b in pts)
    dash = f' stroke-dasharray="{DASH[net]}"' if net in DASH else ""
    svg.append(f'<polyline points="{p}" fill="none" stroke="{C[net]}" stroke-width="{width}" '
               f'stroke-linejoin="round" stroke-linecap="round"{dash}/>')


def dot(x, y, net):
    svg.append(f'<circle cx="{x}" cy="{y}" r="4.5" fill="{C[net]}"/>')


def gnd(x, y, side="l", stub=16):
    """Ground symbol: C3 GND (= the GND wire to S3 header 10)."""
    x2 = x + stub if side == "r" else x - stub
    wire([(x, y), (x2, y), (x2, y + 8)], "GND", 1.8)
    for k, hw in enumerate((8, 5, 2)):
        svg.append(f'<line x1="{x2 - hw}" y1="{y + 8 + 4 * k}" x2="{x2 + hw}" y2="{y + 8 + 4 * k}" '
                   f'stroke="{FG}" stroke-width="1.8"/>')


def v33(x, y, side="l", stub=26, label="3V3", source=False):
    """3V3 tag: C3 3V3 pin.  The arrow points away from the C3 (source) and into a part that takes 3V3."""
    s = 1 if side == "r" else -1
    x2 = x + s * stub
    wire([(x, y), (x2, y)], "3V3")
    tip, back = (x2 + s * 10, x2) if source else (x2 - s * 4, x2 + s * 6)
    svg.append(f'<polygon points="{back},{y - 6} {back},{y + 6} {tip},{y}" fill="{C["3V3"]}"/>')
    t(x2 + s * 14, y + 4, label, 11, "end" if side == "l" else "start", C["3V3"], 600)


def nc(x, y, side="r"):
    """Not-connected cross just outside a pin."""
    cx = x + (12 if side == "r" else -12)
    svg.append(f'<path d="M{cx - 4},{y - 4} L{cx + 4},{y + 4} M{cx - 4},{y + 4} L{cx + 4},{y - 4}" '
               f'stroke="{MUTED}" stroke-width="1.6"/>')


# ------------------------------------------------------------------ USB charger -> C3
box(585, 110, 150, 58, "", None, dashed=True)
t(660, 135, "USB-C 충전기", 13, "middle", FG, 600)
t(660, 154, "5V 2A 이상", 11, "middle", MUTED)
wire([(660, 168), (660, 260)], "USB", 3.2)
t(670, 222, "케이스 밖으로 나가는 유일한 선", 11, "start", MUTED)

# ------------------------------------------------------------------ C3 SuperMini (component side, USB-C up)
C3X0, C3X1 = 560, 760
box(C3X0, 260, C3X1 - C3X0, 380, "", "부품면에서 본 핀 순서 (USB-C 위)")
t(650, 252, "ESP32-C3 SuperMini", 14, "end", FG, 600)
pin(660, 260, "", "l")
t(660, 280, "USB-C", 12, "middle")
left = [(310, "5V", True), (350, "GND", True), (390, "3V3", True), (430, "IO4", False), (470, "IO3", False),
        (510, "IO2", False), (550, "IO1", False), (590, "IO0", False)]
right = [(310, "IO5 SDA", True), (350, "IO6 SCL", True), (390, "IO7", False), (430, "IO8 (LED)", False),
         (470, "IO9 (BOOT)", False), (510, "IO10", False), (550, "IO20 RX", True), (590, "IO21 TX", True)]
for y, lab, used in left:
    pin(C3X0, y, lab, "l", used=used)
for y, lab, used in right:
    pin(C3X1, y, lab, "r", used=used)

# ------------------------------------------------------------------ Waveshare S3 (headers 10 and 7)
S3X0, S3X1 = 60, 330
box(S3X0, 240, S3X1 - S3X0, 440, "Waveshare ESP32-S3-Touch-LCD-7", "번호 = Waveshare 보드 그림의 번호")
t(S3X1 - 10, 292, "⑩ I2C 단자", 12, "end", MUTED, 600)
for y, lab in ((310, "VCC"), (350, "GND")):
    pin(S3X1, y, lab, "r")
for y, lab in ((380, "SDA"), (410, "SCL")):
    pin(S3X1, y, lab, "r", used=False)
    nc(S3X1, y)
t(S3X1 - 10, 502, "⑦ UART2 단자", 12, "end", MUTED, 600)
pin(S3X1, 520, "3V3", "r", used=False)
nc(S3X1, 520)
pin(S3X1, 550, "GND", "r")
gnd(S3X1, 550, side="r", stub=18)
pin(S3X1, 590, "RXD (GPIO44)", "r")
pin(S3X1, 620, "TXD (GPIO43)", "r")
for k, s in enumerate(("⑮ 스위치: UART2", "⑱ I2C 전압 저항:", "   5V 쪽으로 옮김", "⑤⑥ USB-C, ⑯⑰ 버튼:",
                       "   안 씀 (케이스로 막힘)")):
    t(S3X0 + 14, 300 + k * 20 + (0 if k < 3 else 10), s, 12, "start", FG if k in (0, 1, 2) else MUTED,
      600 if k in (0, 1) else 400)

# 5V / GND: C3 -> S3 header 10
wire([(C3X0, 310), (S3X1, 310)], "5V", 3.2)
wire([(C3X0, 350), (S3X1, 350)], "GND", 3.2)
t(445, 302, "5V (S3 전원)", 12, "middle", C["5V"], 600)
t(445, 342, "GND", 12, "middle", FG, 600)
v33(C3X0, 390, "l", 30, "3V3 → TCA·OLED", source=True)

# UART: C3 TX -> S3 RXD, C3 RX <- S3 TXD (crossed)
wire([(C3X1, 590), (790, 590), (790, 720), (400, 720), (400, 590), (S3X1, 590)], "UART")
wire([(C3X1, 550), (810, 550), (810, 740), (380, 740), (380, 620), (S3X1, 620)], "UART")
t(595, 714, "C3 TX(21) → S3 RXD", 12, "middle", C["UART"], 600)
t(595, 756, "C3 RX(20) ← S3 TXD", 12, "middle", C["UART"], 600)

# ------------------------------------------------------------------ TCA9548A
TX0, TX1 = 900, 1090
box(TX0, 240, TX1 - TX0, 520, "TCA9548A (0x70)")
for y, lab in ((290, "VIN"), (330, "GND"), (390, "SDA"), (430, "SCL")):
    pin(TX0, y, lab, "l")
v33(TX0, 290, "l", 24)
gnd(TX0, 330, "l", 16)
for y, lab in ((500, "RST"), (540, "A0"), (570, "A1"), (600, "A2")):
    pin(TX0, y, lab, "l", used=False)
    nc(TX0, y, "l")
t(TX0 + 12, 640, "RST·A0~A2 비움", 11, "start", MUTED)
t(TX0 + 12, 656, "(보드 저항으로 0x70)", 11, "start", MUTED)
wire([(C3X1, 310), (840, 310), (840, 390), (TX0, 390)], "SDA")
wire([(C3X1, 350), (820, 350), (820, 430), (TX0, 430)], "SCL")
t(795, 300, "I2C 3.3V", 11, "middle", C["SDA"], 600)

# ------------------------------------------------------------------ OLED x5 on channels 2..6 (as wired 2026-11-21)
OX0, OX1 = 1240, 1440
cells = ("공기 온도", "습도·CO2", "잎 온도·열화상", "흙 수분·물", "EC·pH·물약")
for k in range(5):
    y0 = 250 + 105 * k
    sd, sc = y0 + 15, y0 + 35
    pin(TX1, sd, f"SD{k + 2}", "r")
    pin(TX1, sc, f"SC{k + 2}", "r")
    box(OX0, y0, OX1 - OX0, 90, "")
    t(OX1 - 10, y0 + 20, f"OLED {k + 1}", 13, "end", FG, 600)
    t(OX1 - 10, y0 + 38, cells[k], 11, "end", MUTED)
    t(OX1 - 10, y0 + 54, "SSD1306 0x3C" + (", 노랑/파랑 2색" if k == 2 else ""), 11, "end", MUTED)
    for y, lab in ((sd, "SDA"), (sc, "SCL"), (y0 + 55, "VCC"), (y0 + 75, "GND")):
        pin(OX0, y, lab, "l")
    wire([(TX1, sd), (OX0, sd)], "SDA")
    wire([(TX1, sc), (OX0, sc)], "SCL")
    v33(OX0, y0 + 55, "l", 24)
    gnd(OX0, y0 + 75, "l", 16)
t(TX1 - 10, 745, "SD0·SD1·SD7(SC 같음) 비움", 11, "end", MUTED)
t(OX0 + (OX1 - OX0) / 2, 790, "OLED 1~5 = 앞에서 볼 때 왼쪽부터", 12, "middle", MUTED)

# ------------------------------------------------------------------ legend and notes
lx, ly = 40, 790
svg.append(f'<rect x="{lx}" y="{ly}" width="1150" height="250" rx="8" fill="none" stroke="{BORDER}" '
           f'stroke-width="1.2"/>')
t(lx + 16, ly + 26, "범례", 14, "start", FG, 600)
items = [("USB", "USB 5V (충전기 → C3)", 3.2), ("5V", "5V (C3 → S3)", 3.2), ("GND", "GND", 3.2),
         ("3V3", "C3 3.3V", 2.2), ("SDA", "I2C SDA", 2.2), ("SCL", "I2C SCL (점선)", 2.2),
         ("UART", "UART (C3 ↔ S3)", 2.2)]
for k, (net, lab, w) in enumerate(items):
    cx = lx + 16
    cy = ly + 50 + k * 24
    wire([(cx, cy), (cx + 34, cy)], net, w)
    t(cx + 44, cy + 4, lab, 12)
notes = ["● = 연결점, ○ = 안 쓰는 핀, ✕ = 연결하지 않음. 점 없이 지나가는 선은 연결 아님",
         "⏚ = C3 GND (S3 ⑩ GND와 같은 선). ▶ 3V3 = C3 3V3 핀",
         "TX와 RX는 엇갈려 연결: C3 TX(21) → S3 RXD, C3 RX(20) ← S3 TXD",
         "S3 ⑩ SDA·SCL은 비움: S3 쪽 버스의 CH422G가 0x30~0x3F에 응답해 OLED 0x3C와 부딪침",
         "OLED·TCA는 모두 3.3V(C3 3V3). 5V를 넣거나 5V로 풀업하지 않기",
         "⑱ 저항을 옮길 때 세 패드를 한꺼번에 잇지 않기(3V3-5V 합선). 전원 전에 ⑩ VCC를 멀티미터로 확인",
         "OLED 핀 순서는 모듈 인쇄 확인(GND·VCC 순서가 바뀐 모듈이 있음). 이 그림은 GND VCC SCL SDA 모듈 기준",
         "전류: 화면부 평균 0.6~0.7A, 순간 1A 안팎 → 충전기 5V 2A 이상, 5V·GND 선은 짧게",
         "C3 USB는 C3 업로드와 S3 업로드·로그 중계에도 씀(S3 ⑮가 UART2라 S3 USB-C로는 못 올림)"]
for k, n in enumerate(notes):
    t(lx + 260, ly + 50 + k * 22, n, 12, "start", FG)

body = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 {TOP} {W} {H - TOP}" width="{W}" height="{H - TOP}" '
        f'font-family="var(--font-sans, system-ui, sans-serif)">' + "".join(svg) + "</svg>")
html = ("<!doctype html><html lang=\"ko\"><head><meta charset=\"utf-8\"><title>화면부 배선도</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body>"
        "<p style=\"margin:0 0 8px;font-size:13px;color:var(--muted-foreground,#6b7280)\">"
        "화면부(S3 + C3 + TCA9548A + OLED 5개) 배선도 · 가로로 스크롤해서 보세요</p>"
        "<div style=\"overflow-x:auto\">" + body + "</div></body></html>")
OUT.mkdir(exist_ok=True)
(OUT / "wiring.html").write_text(html, encoding="utf-8")
(OUT / "_cache").mkdir(exist_ok=True)
(OUT / "_cache" / "wiring_fit.html").write_text(html.replace(f'width="{W}" height="{H - TOP}"', 'width="100%"'),
                                                encoding="utf-8")
(OUT / "wiring.svg").write_text(body.replace("var(--foreground, #111827)", "#111827")
                                .replace("var(--muted-foreground, #6b7280)", "#6b7280")
                                .replace("var(--card, #ffffff)", "#ffffff")
                                .replace("var(--border, #d1d5db)", "#d1d5db")
                                .replace("var(--font-sans, system-ui, sans-serif)", "sans-serif"),
                                encoding="utf-8")
print(OUT / "wiring.html")
