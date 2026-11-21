"""AIplantRx v2 UI mockups.

    python make_mockups.py

1. Draws sample camera and thermal frames into html/assets.
2. Bakes the static layers of each S3 screen (html/bake_*.html) into one 800x480 image, dithered to RGB565
   as the S3 stores it, plus the small pre-drawn pieces (sidebar fade strip, AI glyph, thermal legend).
3. Renders the S3 screens (html/*.html, 800x480) with headless Chrome into PNGs next to this file.
   style.css only uses what LVGL 8 draws cheaply on the S3 (UI.md 1.6).
4. Draws the five 128x64 white OLED panels at 1 bit into oled_panels.png.

Fonts: Pretendard (installed, or put the OTFs in ../fonts), Framework7 Icons (../fonts; the PUA copy
for the OLEDs comes from ../fonts/make_f7_pua.py), Galmuri11 for the OLEDs' small text.
Values are sample data (basil).
"""
import base64
import glob
import json
import os
import re
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.normpath(os.path.join(HERE, "..", "fonts"))
HTML = os.path.join(HERE, "html")
ASSETS = os.path.join(HTML, "assets")
FONT_DIRS = [FONTS, os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Windows\Fonts"), r"C:\Windows\Fonts"]
CHROME = [r"C:\Program Files\Google\Chrome\Application\chrome.exe",
          os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe")]
PRET = {400: "Pretendard-Regular.otf", 500: "Pretendard-Medium.otf", 600: "Pretendard-SemiBold.otf", 700: "Pretendard-Bold.otf"}


def find_font(name):
    for d in FONT_DIRS:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    raise FileNotFoundError(name)


# ---------------------------------------------------------------- sample frames

def thermal_data():
    h, w = 24, 32
    y, x = np.mgrid[0:h, 0:w].astype(float)
    t = 24.4 + 0.5 * (y / h)
    t -= 2.0 * np.exp(-(((x - 15) / 6.0) ** 2 + ((y - 9) / 4.6) ** 2))
    t -= 1.1 * np.exp(-(((x - 21) / 3.8) ** 2 + ((y - 12) / 3.2) ** 2))
    t += 2.6 * np.exp(-(((x - 16) / 5.0) ** 2 + ((y - 21) / 2.2) ** 2))
    t += 1.6 * np.exp(-(((x - 3) / 2.6) ** 2 + ((y - 2) / 2.2) ** 2))
    t += np.random.default_rng(3).normal(0, 0.08, t.shape)
    return t


INFERNO = [(0, 0, 4), (40, 11, 84), (101, 21, 110), (159, 42, 99), (212, 72, 66), (245, 125, 21), (250, 193, 39), (252, 255, 164)]


def colormap(v):
    v = np.clip(v, 0, 1) * (len(INFERNO) - 1)
    i = np.minimum(v.astype(int), len(INFERNO) - 2)
    fr = (v - i)[..., None]
    a = np.array(INFERNO, float)
    return (a[i] * (1 - fr) + a[i + 1] * fr).astype(np.uint8)


def thermal_frame(w=320, h=240):
    t = thermal_data()
    big = np.array(Image.fromarray(t.astype(np.float32)).resize((w, h), Image.BICUBIC))
    lo, hi = float(t.min()), float(t.max())
    my, mx = np.unravel_index(np.argmax(t), t.shape)
    return Image.fromarray(colormap((big - lo) / (hi - lo))), {"lo": lo, "hi": hi, "max": (mx + 0.5) * w / 32, "may": (my + 0.5) * h / 24}


def leaf(d, cx, cy, length, width, ang, color):
    import math
    pts = []
    for k in range(25):
        u = k / 24
        pts.append(((u - 0.5) * length, width * 0.5 * math.sin(math.pi * u) ** 0.9))
    pts += [(px, -py) for px, py in reversed(pts)]
    c, s = math.cos(ang), math.sin(ang)
    d.polygon([(cx + px * c - py * s, cy + px * s + py * c) for px, py in pts], fill=color)


def rgb_frame(w=320, h=240):
    yy = np.linspace(0, 1, h)[:, None, None]
    wall = (np.array([200, 190, 174]) * (1 - yy) + np.array([168, 158, 144]) * yy) * np.ones((1, w, 1))
    img = Image.fromarray(wall.astype(np.uint8))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 188, w, h], fill=(122, 98, 76))
    d.ellipse([96, 196, 224, 214], fill=(96, 78, 62))
    d.polygon([(116, 150), (204, 150), (192, 208), (128, 208)], fill=(176, 96, 62))
    d.rectangle([110, 140, 210, 156], fill=(192, 110, 72))
    d.ellipse([118, 136, 202, 150], fill=(66, 50, 38))
    for x1, y1 in [(150, 70), (176, 64), (132, 96), (190, 98)]:
        d.line([(160, 142), (x1, y1)], fill=(84, 120, 56), width=3)
    for lf in [(150, 70, 46, 26, -1.9, (76, 146, 62)), (176, 64, 50, 28, -1.2, (88, 162, 70)), (132, 96, 50, 28, -2.6, (70, 138, 58)),
               (190, 98, 48, 27, -0.5, (82, 156, 66)), (146, 112, 40, 22, 2.8, (64, 128, 54)), (176, 112, 42, 23, 0.3, (74, 142, 60)),
               (160, 52, 34, 18, -1.6, (104, 176, 82)), (118, 84, 34, 19, -2.9, (92, 160, 72)), (204, 80, 36, 19, -0.2, (96, 166, 76))]:
        leaf(d, *lf)
    img = img.filter(ImageFilter.GaussianBlur(0.7))
    arr = np.array(img).astype(float) + np.random.default_rng(5).normal(0, 3.0, (h, w, 3))
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))


# ---------------------------------------------------------------- S3 screens (HTML -> Chrome)

SIDEBAR = [
    [("wifi", "Wi-Fi", "wifi", "#007AFF", "AIPLANT-DEMO", 0), ("ai", "AI 서버", "sparkles", "#5856D6", "연결됨", 0),
     ("plantnet", "PlantNet", "camera_viewfinder", "#34C759", "482회 남음", 0)],
    [("general", "일반", "gear_alt_fill", "#8E8E93", "", 0), ("update", "소프트웨어 업데이트", "arrow_down_circle_fill", "#8E8E93", "", 2),
     ("devices", "기기", "square_stack_3d_up_fill", "#007AFF", "4대", 0)],
    [("plant", "식물 프로필", "leaf_arrow_circlepath", "#34C759", "바질", 0), ("control", "자동 제어", "thermometer", "#FF9500", "", 0),
     ("light", "조명", "lightbulb_fill", "#FFCC00", "", 0), ("water", "물·물약", "drop_fill", "#32ADE6", "", 0),
     ("camera", "카메라", "camera_fill", "#8E8E93", "", 0)],
    [("display", "화면 및 밝기", "sun_max_fill", "#007AFF", "", 0), ("aitalk", "AI 한마디", "bubble_left_fill", "#AF52DE", "", 0),
     ("demo", "시연 모드", "play_rectangle_fill", "#FF2D55", "", 0), ("about", "정보", "info_circle_fill", "#8E8E93", "", 0)],
]


def write_fonts_css():
    f7 = base64.b64encode(open(os.path.join(FONTS, "Framework7Icons-Regular.woff2"), "rb").read()).decode()
    css = [f"@font-face{{font-family:'Framework7 Icons';src:url(data:font/woff2;base64,{f7}) format('woff2');}}"]
    for w, name in PRET.items():
        path = find_font(name).replace("\\", "/")
        local = name.replace(".otf", "")
        css.append(f"@font-face{{font-family:'Pretendard';font-weight:{w};src:local('{local.replace('-', ' ')}'),local('{local}'),url('file:///{path}');}}")
    open(os.path.join(HTML, "fonts.css"), "w", encoding="utf-8").write("\n".join(css))


GRAY_ICONS = {"general", "update", "camera", "about"}


def lighten(hex_color, k=0.38):
    c = [int(hex_color[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02X%02X%02X" % tuple(int(v + (255 - v) * k) for v in c)


THEMES = ["glass", "dark", "light"]  # one layout, three palettes (style.css .t-*)
CAM_TILES = [(464, 16), (464, 244)]  # main camera tiles on screen, 320x220 with radius 20
CAM_W, CAM_H, CAM_R = 320, 220, 20


def vic(icon, color):
    """Round icon with a 2-stop vertical gradient (LVGL draws this natively)."""
    return f'<div class="vic" style="--c1:{lighten(color)};--c2:{color}"><i class="i">{icon}</i></div>'


def vsidebar(selected, theme):
    """visionOS Settings sidebar: account header, then groups split by space only, round gradient icons."""
    # Rows are 46 px, groups 21 px apart, the account header 81 px with its gap; scroll the list (as a person
    # would have) so the selected row ends 16 px above the screen bottom.
    index = [g[0] for grp in SIDEBAR for g in grp].index(selected)
    group = next(i for i, grp in enumerate(SIDEBAR) if any(g[0] == selected for g in grp))
    bottom = 18 + 81 + (index + 1) * 46 + group * 21
    scroll = max(0, bottom - 480 + 16)
    # A scrolled list fades out under the screen's top edge, as visionOS lists do. LVGL draws that as a
    # 242x30 strip cut from the baked background with an alpha ramp, laid over the list (no mask).
    fade = f'<img class="vfade" src="assets/fade_side_{theme}.png">' if scroll else ""
    out = [f'<div class="vside"><div class="vscroll"><div style="margin-top:-{scroll}px"><div class="vacct"><div class="av"><img src="assets/plant.png"></div>'
           '<div><b>바질</b><small>AIplantRx · 재배 3일째<br>사진과 재배 기록을 봅니다.</small></div></div>']
    for grp in SIDEBAR:
        out.append('<div class="vgrp">')
        for key, title, icon, color, value, badge in grp:
            sel = " sel" if key == selected else ""
            if key in GRAY_ICONS:
                ic_html = f'<div class="vic gray"><i class="i">{icon}</i></div>'
            else:
                ic_html = vic(icon, color)
            right = f'<span class="vbadge" style="margin-left:auto">{badge}</span>' if badge else (f'<span class="v">{value}</span>' if value else "")
            out.append(f'<div class="vrow{sel}">{ic_html}<span>{title}</span>{right}</div>')
        out.append("</div>")
    out.append(f"</div></div>{fade}</div>")
    return "".join(out)


def cam_caps(theme):
    """20x20 pieces of the background over the camera tiles' corners, in place of corner clipping."""
    out = []
    for k, (x, y) in enumerate(CAM_TILES):
        for j, (dx, dy) in enumerate([(0, 0), (CAM_W - CAM_R, 0), (0, CAM_H - CAM_R), (CAM_W - CAM_R, CAM_H - CAM_R)]):
            out.append(f'<span class="cap" style="left:{x + dx}px;top:{y + dy}px;'
                       f'background:url(assets/caps_main_{theme}.png) -{(k * 4 + j) * CAM_R}px 0"></span>')
    return "".join(out)


def env_images():
    """A warm, out-of-focus room behind the visionOS-style glass (the S3 draws it as one static image)."""
    W, H = 800, 480
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for yy in range(H):
        t = yy / H
        if t < 0.16:
            c = (54, 45, 36)
        elif t < 0.74:
            u = (t - 0.16) / 0.58
            c = (int(122 + 14 * u), int(106 + 10 * u), int(88 + 6 * u))
        else:
            c = (78, 58, 40)
        d.line([(0, yy), (W, yy)], fill=c)
    d.rectangle([0, 60, 170, 340], fill=(196, 184, 164))
    d.rectangle([600, 40, 800, 330], fill=(186, 172, 150))
    d.rectangle([250, 90, 330, 330], fill=(206, 200, 190))
    d.rectangle([380, 70, 470, 250], fill=(48, 42, 36))
    d.rectangle([0, 355, 800, 480], fill=(84, 62, 42))
    for x in range(0, 800, 60):
        d.line([(x, 360), (x - 40, 480)], fill=(70, 50, 34), width=6)
    img.filter(ImageFilter.GaussianBlur(40)).save(os.path.join(ASSETS, "env_blur.png"))


BAYER4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16 + 1 / 32


def rgb565(path):
    """Ordered-dither an image to RGB565 and expand it back, which is what the 16-bit panel shows."""
    a = np.array(Image.open(path).convert("RGB")).astype(float)
    h, w, _ = a.shape
    t = np.tile(BAYER4, (h // 4 + 1, w // 4 + 1))[:h, :w] - 0.5
    out = np.empty_like(a)
    for ch, bits in enumerate((5, 6, 5)):
        top = (1 << bits) - 1
        q = np.clip(np.floor(a[..., ch] / 255 * top + 0.5 + t), 0, top)
        out[..., ch] = np.round(q * 255 / top)
    Image.fromarray(out.astype(np.uint8)).save(path)


def corner_cover(r):
    """How much of each pixel a round top-left corner of radius r covers (0..1), 4x supersampled."""
    yy, xx = np.mgrid[0:r * 4, 0:r * 4] + 0.5
    inside = ((xx / 4 - r) ** 2 + (yy / 4 - r) ** 2 <= r * r) * 255
    return np.array(Image.fromarray(inside.astype(np.uint8)).resize((r, r), Image.BOX)) / 255


def cut(bg, x, y, w, h, alpha):
    """A piece of a background image with the given opacity (0..1), as the S3 builds it at boot."""
    return Image.fromarray(np.dstack([bg[y:y + h, x:x + w], np.round(alpha * 255).astype(np.uint8)]), "RGBA")


def bake():
    """Per theme: the static layers of each screen as one image, and the small pieces cut from them.
    Glass ships its two full-screen backgrounds (RGB565, dithered). Dark and light fill the screen with one
    color (plus the sidebar divider) on the S3; their images here only serve to cut the pieces and the mockups."""
    tl20 = corner_cover(CAM_R)
    for t in THEMES:
        for page, name in [("bake_main.html", f"bg_main_{t}.png"), ("bake_settings.html", f"bg_settings_{t}.png")]:
            render(page, os.path.join(ASSETS, name), {}, t)
            if t == "glass":
                rgb565(os.path.join(ASSETS, name))
        bs = np.array(Image.open(os.path.join(ASSETS, f"bg_settings_{t}.png")).convert("RGB"))
        # Sidebar fade: the list viewport starts 18 px below the screen top.
        ramp = np.linspace(1, 0, 30)[:, None] * np.ones((1, 242))
        cut(bs, 0, 18, 242, 30, ramp).save(os.path.join(ASSETS, f"fade_side_{t}.png"))
        # Camera tile corners on the main screen, 8 pieces in one 160x20 strip.
        bm = np.array(Image.open(os.path.join(ASSETS, f"bg_main_{t}.png")).convert("RGB"))
        strip = Image.new("RGBA", (CAM_R * 8, CAM_R))
        corners = [(0, 0, tl20), (CAM_W - CAM_R, 0, np.fliplr(tl20)), (0, CAM_H - CAM_R, np.flipud(tl20)),
                   (CAM_W - CAM_R, CAM_H - CAM_R, tl20[::-1, ::-1])]
        for k, (x, y) in enumerate(CAM_TILES):
            for j, (dx, dy, cover) in enumerate(corners):
                strip.paste(cut(bm, x + dx, y + dy, CAM_R, CAM_R, 1 - cover), ((k * 4 + j) * CAM_R, 0))
        strip.save(os.path.join(ASSETS, f"caps_main_{t}.png"))
    # AI glyph: the 22 px conic-gradient circle (LVGL has no conic gradient), 4x supersampled.
    n = 88
    y, x = np.mgrid[0:n, 0:n] + 0.5
    c = n / 2
    u = ((np.degrees(np.arctan2(x - c, c - y)) - 210) % 360) / 360  # CSS conic: 0 deg is up, clockwise
    stops = np.array([[int(s[i:i + 2], 16) for i in (1, 3, 5)] for s in
                      ["#FF6B9A", "#B66CFF", "#5AA9FF", "#42DFC0", "#FFC05A", "#FF6B9A"]], float)
    v = u * (len(stops) - 1)
    i = np.minimum(v.astype(int), len(stops) - 2)
    fr = (v - i)[..., None]
    rgb = stops[i] * (1 - fr) + stops[i + 1] * fr
    inside = (((x - c) ** 2 + (y - c) ** 2) <= c * c) * 255.0
    glyph = Image.fromarray(np.dstack([rgb, inside]).astype(np.uint8), "RGBA")
    glyph.resize((22, 22), Image.BOX).save(os.path.join(ASSETS, "ai_glyph.png"))
    # Thermal legend: a 6x130 capsule from the same color table the S3 uses for the thermal image.
    yy, xx = np.mgrid[0:520, 0:24] + 0.5
    capsule = (((xx - 12) ** 2 + (yy - np.clip(yy, 12, 508)) ** 2) <= 144) * 255.0
    a = np.array(Image.fromarray(capsule.astype(np.uint8)).resize((6, 130), Image.BOX))
    lg = np.dstack([colormap(np.linspace(1, 0, 130)[:, None] * np.ones((1, 6))), a])
    Image.fromarray(lg, "RGBA").save(os.path.join(ASSETS, "legend.png"))


def thermal_tokens(info):
    ox, oy = 0, -10  # the 320x220 tile shows the 320x240 frame 1:1, cropped 10 px top and bottom
    bx0, by0, bx1, by1 = 92 + ox, 42 + oy, 228 + ox, 150 + oy
    mx, my = info["max"] + ox, info["may"] + oy
    return {
        "{{TH_BOX}}": f"left:{bx0}px;top:{by0}px;width:{bx1 - bx0}px;height:{by1 - by0}px",
        "{{TH_LABEL}}": f"left:{bx0}px;top:{by1 + 6}px",
        "{{TH_XH}}": f"left:{mx:.0f}px;top:{my:.0f}px",
        "{{TH_XHV}}": f"left:{mx + 10:.0f}px;top:{my - 18:.0f}px",
        "{{TH_MAX}}": f"{info['hi']:.1f}", "{{TH_HI}}": f"{info['hi']:.0f}", "{{TH_LO}}": f"{info['lo']:.0f}",
    }


VIC_TOKEN = re.compile(r"\{\{VIC:([a-z0-9_]+):(#[0-9A-Fa-f]{6})\}\}")


def render(page, out, tokens, theme="glass"):
    src = open(os.path.join(HTML, page), encoding="utf-8").read()
    for k, v in tokens.items():
        src = src.replace(k, v)
    src = src.replace("{{THEME}}", "t-" + theme).replace("{{T}}", theme).replace("{{CAMCAPS}}", cam_caps(theme))
    src = VIC_TOKEN.sub(lambda m: vic(m[1], m[2]), src)
    for key in [g[0] for grp in SIDEBAR for g in grp]:
        src = src.replace("{{VSIDEBAR:%s}}" % key, vsidebar(key, theme))
    tmp = os.path.join(HTML, "_" + page)
    open(tmp, "w", encoding="utf-8").write(src)
    exe = next(p for p in CHROME if os.path.exists(p))
    prof = os.path.join(tempfile.gettempdir(), "aiplantrx-mockups-chrome")
    url = "file:///" + tmp.replace("\\", "/")
    subprocess.run([exe, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--window-size=800,480", "--allow-file-access-from-files", "--user-data-dir=" + prof,
                    "--virtual-time-budget=2000", "--screenshot=" + out, url], check=True, capture_output=True, timeout=90)
    os.remove(tmp)
    im = Image.open(out)
    assert im.size == (800, 480), (out, im.size)


# AI card states (UI.md 6.3). Numbers the S3 recolors by their unit: <t> temperature, <w> water and soil, <e> EC and pH.
AI_STATES = {
    "idle": dict(icon="clock_fill", color="#8E8E93", why="10분마다", when="방금", meta="Bonsai 2 · 2.3초",
                 text="잎이 <t>23.1°</t>로 공기 <t>24.6°</t>보다 <t>1.5°</t> 시원해요. 물이 잎에서 증발하며 열을 식히고 있다는 뜻이라, "
                      "흙 <w>46%</w>면 오늘은 물을 더 주지 않아도 돼요."),
    "hand": dict(icon="hand_raised_fill", color="#FF9F0A", why="열화상에 손", when="방금", stream=True,
                 text="손이 보여요! 손은 <t>33.8°</t>, 바질 잎은 <t>23.1°</t>라서 <t>10.7°</t>나 차이 나요. 사람은 몸에서 열을 만들고, 잎은 뿌리에서 올린"),
    "water": dict(icon="drop_fill", color="#32ADE6", why="물 120ml 줌", when="방금", meta="Bonsai 2 · 2.2초",
                  text="흙이 <w>38%</w>까지 말라서 물을 <w>120ml</w> 줬어요. 10분 만에 <w>47%</w>로 올라왔으니 뿌리까지 잘 스며들었어요. "
                       "내일까지는 안 줘도 돼요."),
    "offline": dict(icon="drop_fill", color="#32ADE6", why="물 120ml 줌", when="12분 전", meta="Bonsai 2 · 2.2초",
                    text="흙이 <w>38%</w>까지 말라서 물을 <w>120ml</w> 줬어요. 10분 만에 <w>47%</w>로 올라왔으니 뿌리까지 잘 스며들었어요. "
                         "내일까지는 안 줘도 돼요."),
}


def ai_card(s):
    text = s["text"]
    for tag in "twe":
        text = text.replace(f"<{tag}>", f'<span class="n {tag}">').replace(f"</{tag}>", "</span>")
    why = f'<span class="why"><i class="i" style="color:{s["color"]}">{s["icon"]}</i>{s["why"]} · {s["when"]}</span>'
    meta = ('<span class="meta"><span class="vspin"></span>쓰는 중</span>' if s.get("stream")
            else f'<span class="meta">{s["meta"]}</span>')
    cur = '<span class="cur"></span>' if s.get("stream") else ""
    return (f'<div class="ai"><div class="h"><div class="aig"><i class="i">sparkles</i></div>AI 한마디{meta}{why}</div>'
            f'<p>{text}{cur}</p></div>')


def ai_sheet(tok):
    """The AI card in four states, cut from glass main screens, 2x2 with captions (s3_ai_states.png)."""
    states = [("idle", "조용할 때: 10분마다 그때 값을 보고 새로 씀"),
              ("hand", "열화상 앞에 손을 대면: 바로 쓰기 시작해 글자가 이어서 나옴"),
              ("water", "장치가 움직이면: 무엇을 했고 값이 어떻게 바뀌었는지"),
              ("offline", "AI 서버가 안 될 때: 마지막 말을 그대로 두고 시각만 흐름")]
    cw, ch, pad, cap = 448, 128, 16, 30
    sheet = Image.new("RGB", (2 * cw + 3 * pad, 2 * (cap + ch) + 3 * pad), "white")
    d = ImageDraw.Draw(sheet)
    for k, (key, caption) in enumerate(states):
        p = os.path.join(tempfile.gettempdir(), f"aiplantrx_ai_{key}.png")
        render("main.html", p, dict(tok, **{"{{AICARD}}": ai_card(AI_STATES[key])}), "glass")
        x, y = pad + (k % 2) * (cw + pad), pad + (k // 2) * (cap + ch + pad)
        d.text((x, y + 4), caption, font=pret(16, 600), fill=(40, 40, 44))
        sheet.paste(Image.open(p).convert("RGB").crop((8, 292, 8 + cw, 292 + ch)), (x, y + cap))
        os.remove(p)
    sheet.save(os.path.join(HERE, "s3_ai_states.png"))


def s3_screens():
    os.makedirs(ASSETS, exist_ok=True)
    rgb = rgb_frame()
    rgb.save(os.path.join(ASSETS, "rgb.png"))
    rgb.crop((100, 40, 220, 160)).resize((120, 120), Image.LANCZOS).save(os.path.join(ASSETS, "plant.png"))
    th, info = thermal_frame()
    th.save(os.path.join(ASSETS, "thermal.png"))
    env_images()
    write_fonts_css()
    bake()
    tok = thermal_tokens(info)
    tok["{{AICARD}}"] = ai_card(AI_STATES["idle"])
    out = lambda n, t: os.path.join(HERE, n + ("" if t == "glass" else "_" + t) + ".png")
    # Each theme: main, identify result (over its own main) and Wi-Fi settings. The rest only in glass.
    for t in THEMES:
        render("main.html", out("s3_main", t), tok, t)
        shutil.copy(out("s3_main", t), os.path.join(ASSETS, f"shot_main_{t}.png"))
        render("result.html", out("s3_identify_result", t), tok, t)
        render("settings_wifi.html", out("s3_settings_wifi", t), tok, t)
        shutil.copy(out("s3_settings_wifi", t), os.path.join(ASSETS, f"shot_wifi_{t}.png"))
    render("settings_wifi_join.html", out("s3_settings_wifi_join", "glass"), tok)
    render("settings_update.html", out("s3_settings_update", "glass"), tok)
    render("settings_plant.html", out("s3_settings_plant", "glass"), tok)
    render("settings_aitalk.html", out("s3_settings_aitalk", "glass"), tok)
    ai_sheet(tok)


# ---------------------------------------------------------------- OLEDs (1-bit, 128x64)
# OLED 3 is a yellow/blue module (rows 0-15 yellow, 16-63 blue).  Every panel keeps the title line in
# rows 0-15 and the rest from row 18 down, so it reads as title/data there and the same on the white ones.
# The C3 draws these with the same fonts (firmware/c3_hwtest/src/oled_ui.cpp).

_fc = {}


def pret(size, w=600):
    k = ("p", size, w)
    if k not in _fc:
        _fc[k] = ImageFont.truetype(find_font(PRET[w]), size)
    return _fc[k]


def galm():
    # Pretendard breaks up below 13 px at 1 bit, so small text uses the Galmuri11 pixel font.
    if "g" not in _fc:
        _fc["g"] = ImageFont.truetype(os.path.join(FONTS, "Galmuri11.ttf"), 12)
    return _fc["g"]


F7MAP = None


def ic(name):
    global F7MAP
    if F7MAP is None:
        F7MAP = json.load(open(os.path.join(FONTS, "framework7-icons-pua.json")))
    return chr(int(F7MAP[name], 16))


def f7(size=16):
    k = ("i", size)
    if k not in _fc:
        _fc[k] = ImageFont.truetype(os.path.join(FONTS, "Framework7Icons-PUA.ttf"), size)
    return _fc[k]


def canvas():
    img = Image.new("1", (128, 64))
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    return img, d


def header(d, icon, title, right="", invert=False):
    c = 0 if invert else 1
    if invert:
        d.rounded_rectangle([0, 0, 127, 15], 3, fill=1)
    d.text((2 if invert else 0, 8), ic(icon), font=f7(16), fill=c, anchor="lm")
    d.text((20, 8), title, font=pret(13, 600), fill=c, anchor="lm")
    if right:
        d.text((125 if invert else 127, 9), right, font=galm(), fill=c, anchor="rm")


def big(d, x, base, value, unit, size=30):
    d.text((x, base), value, font=pret(size, 600), fill=1, anchor="ls")
    w = d.textlength(value, font=pret(size, 600))
    d.text((x + w + 2, base - size * 0.52), unit, font=galm(), fill=1, anchor="ls")


def bar(d, lo, hi, tlo, thi, val, x0=1, x1=126, y=58):
    sx = lambda v: x0 + (x1 - x0) * (min(max(v, lo), hi) - lo) / (hi - lo)
    d.line([(x0, y), (x1, y)], fill=1)
    a, b = sx(tlo), sx(thi)
    d.rounded_rectangle([a, y - 2, b, y + 2], 2, fill=1)
    m = sx(val)
    d.ellipse([m - 5, y - 5, m + 5, y + 5], fill=0)
    d.ellipse([m - 4, y - 4, m + 4, y + 4], fill=1)
    d.ellipse([m - 2, y - 2, m + 2, y + 2], fill=0)


def footer(d, text):
    d.rounded_rectangle([0, 51, 127, 63], 3, fill=1)
    d.text((64, 57), text, font=galm(), fill=0, anchor="mm")


def thermal_1bit(w, h):
    t = thermal_data()
    g = np.array(Image.fromarray(t.astype(np.float32)).resize((w, h), Image.BILINEAR))
    g = (g - g.min()) / (g.max() - g.min())
    yy, xx = np.mgrid[0:h, 0:w]
    on = np.where(g < 0.33, False, np.where(g > 0.62, True, (xx + yy) % 2 == 0))
    return Image.fromarray((on * 255).astype(np.uint8)).convert("1", dither=Image.Dither.NONE)


def oled_air(state="normal"):
    img, d = canvas()
    header(d, "thermometer", "공기 온도", "낮 구간")
    big(d, 0, 46, "24.6", "°C")
    d.text((127, 31), "목표", font=galm(), fill=1, anchor="rs")
    d.text((127, 45), "20–28", font=galm(), fill=1, anchor="rs")
    if state == "stale":
        footer(d, "연결 대기 · 12초 전 값")
    else:
        bar(d, 10, 35, 20, 28, 24.6)
    return img


def oled_humid():
    img, d = canvas()
    header(d, "drop", "습도", "CO2")
    big(d, 0, 46, "58", "%")
    d.text((127, 40), "820", font=pret(20, 600), fill=1, anchor="rs")
    d.text((127, 50), "ppm", font=galm(), fill=1, anchor="rs")
    bar(d, 0, 100, 40, 60, 58, x1=82)
    return img


def oled_leaf():
    img, d = canvas()
    header(d, "scope", "잎 온도", "최고 26.8")
    img.paste(thermal_1bit(44, 33), (0, 21))
    big(d, 50, 42, "23.1", "°C", size=24)
    d.text((50, 57), "공기 −1.5°C", font=galm(), fill=1, anchor="ls")
    return img


def oled_soil(state="normal"):
    img, d = canvas()
    if state == "low":
        header(d, "drop_fill", "흙 수분 낮음", invert=True)
        big(d, 0, 46, "31", "%")
        d.text((127, 31), "곧 물 줌", font=galm(), fill=1, anchor="rs")
        d.text((127, 45), "목표 40–60", font=galm(), fill=1, anchor="rs")
        bar(d, 0, 100, 40, 60, 31)
        return img
    header(d, "drop_fill", "흙 수분", "물 09:40")
    big(d, 0, 46, "46" if state == "normal" else "38", "%")
    d.text((127, 31), "흙 22.8°C", font=galm(), fill=1, anchor="rs")
    d.text((127, 45), "목표 40–60", font=galm(), fill=1, anchor="rs")
    if state == "pump":
        footer(d, "물 주는 중 · 2/3초")
    else:
        bar(d, 0, 100, 40, 60, 46)
    return img


def oled_nutri():
    img, d = canvas()
    header(d, "eyedropper_halffull", "양분", "물약 3일 뒤")
    d.text((0, 30), "EC", font=galm(), fill=1, anchor="ls")
    d.text((0, 48), "1.2", font=pret(22, 600), fill=1, anchor="ls")
    d.text((68, 30), "pH", font=galm(), fill=1, anchor="ls")
    d.text((68, 48), "6.4", font=pret(22, 600), fill=1, anchor="ls")
    d.line([(63, 21), (63, 47)], fill=1)
    bar(d, 0, 3, 1.0, 1.6, 1.2, x0=1, x1=58)
    bar(d, 4, 8, 6.0, 7.0, 6.4, x0=69, x1=126)
    return img


def oled_sheet():
    S, PAD = 3, 26
    on, off = (238, 238, 236), (6, 6, 7)
    yellow, blue = (255, 214, 10), (70, 160, 255)

    def show(img, two_colour=False):
        a = np.array(img.convert("L")) > 0
        out = np.zeros((64 * S, 128 * S, 3), np.uint8)
        out[:] = off
        for yy in range(64):
            c = (yellow if yy < 16 else blue) if two_colour else on
            for xx in range(128):
                if a[yy, xx]:
                    out[yy * S:yy * S + S - 1, xx * S:xx * S + S - 1] = c
        return Image.fromarray(out)

    row1 = [("1 공기 온도", oled_air()), ("2 습도 · CO2", oled_humid()), ("3 잎 온도 (열화상, 노랑/파랑)", oled_leaf()),
            ("4 흙 수분 · 물 펌프", oled_soil()), ("5 양분 · pH · 물약", oled_nutri())]
    row2 = [("범위 밖: 제목 반전, 2초마다 깜빡임", oled_soil("low")), ("장치 동작 중: 아래 줄 반전", oled_soil("pump")),
            ("연결 끊김: 마지막 값 유지", oled_air("stale"))]
    pw, ph = 128 * S, 64 * S
    W, H = PAD + 5 * (pw + PAD), 70 + ph + 60 + 50 + ph + 60
    sheet = Image.new("RGB", (W, H), (242, 242, 247))
    d = ImageDraw.Draw(sheet)
    d.text((PAD, 16), "OLED 5개 · 128×64 1비트, 3번만 노랑/파랑 2색 (실제 해상도, 3배 확대)", font=pret(26, 700),
           fill=(0, 0, 0))
    for i, (cap, img) in enumerate(row1):
        x = PAD + i * (pw + PAD)
        d.rounded_rectangle([x - 8, 62, x + pw + 8, 70 + ph + 8], 10, fill=(0, 0, 0))
        sheet.paste(show(img, two_colour=i == 2), (x, 70))
        d.text((x, 70 + ph + 18), cap, font=pret(20, 600), fill=(0, 0, 0))
    y2 = 70 + ph + 60
    d.text((PAD, y2 + 4), "상태 표시", font=pret(22, 700), fill=(0, 0, 0))
    for i, (cap, img) in enumerate(row2):
        x = PAD + i * (pw + PAD)
        d.rounded_rectangle([x - 8, y2 + 42, x + pw + 8, y2 + 50 + ph + 8], 10, fill=(0, 0, 0))
        sheet.paste(show(img), (x, y2 + 50))
        d.text((x, y2 + 50 + ph + 14), cap, font=pret(18, 500), fill=(60, 60, 67))
    sheet.save(os.path.join(HERE, "oled_panels.png"))


if __name__ == "__main__":
    for old in ["s3_settings_devices.png"] + [os.path.join("html", "assets", n) for n in
                ["bg_wifi.png", "bg_main.png", "bg_settings.png", "fade_side.png", "corner_side.png", "shot_main.png", "shot_wifi.png",
                 "env.png", "corner_side_glass.png", "corner_side_dark.png", "corner_side_light.png"]]:
        p = os.path.join(HERE, old)
        if os.path.exists(p):
            os.remove(p)
    s3_screens()
    oled_sheet()
    print("ok")
