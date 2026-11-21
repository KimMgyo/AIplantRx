#!/usr/bin/env python3
"""Reads the five OLED frame buffers from the running C3 into one PNG, drawn as the real modules look:
OLED 3 (TCA channel 4) yellow in rows 0-15 and blue below, the rest white.

    python tools/oled_dump.py 192.168.10.88 out.png     over Wi-Fi (GET /oled?ch=N)
    python tools/oled_dump.py COM32 out.png             over the C3's USB ("dump <ch>")

The USB way opens the port without touching DTR/RTS (pyserial's defaults reset the C3 when the port closes).
"""
import urllib.request
import sys
import time

import serial
from PIL import Image

CHANNELS = [2, 3, 4, 5, 6]  # OLED 1..5
TWO_COLOUR = {4}
S = 4


def read_frames(port):
    sp = serial.Serial()
    sp.port, sp.baudrate, sp.timeout = port, 115200, 0.2
    sp.dtr = sp.rts = False
    sp.open()
    frames, buf = {}, b""
    try:
        sp.reset_input_buffer()
        for ch in CHANNELS:
            sp.write(f"dump {ch}\n".encode())
        t0 = time.time()
        while len(frames) < len(CHANNELS) and time.time() - t0 < 5:
            buf += sp.read(4096)
            for line in buf.split(b"\n"):
                if line.startswith(b"[dump] ch"):
                    head, _, hexs = line.strip().partition(b" ")[2].partition(b" ")
                    if len(hexs) == 2048:
                        frames[int(head[2:])] = bytes.fromhex(hexs.decode())
    finally:
        sp.close()
    return frames


def read_frames_http(host):
    frames = {}
    for ch in CHANNELS:
        try:
            with urllib.request.urlopen(f"http://{host}/oled?ch={ch}", timeout=5) as r:
                b = r.read()
            if len(b) == 1024:
                frames[ch] = b
        except OSError as e:
            print(f"ch{ch}: {e}")
    return frames


def panel(fb, two_colour):
    img = Image.new("RGB", (128 * S + 2 * S, 64 * S + 2 * S), (8, 8, 10))
    px = img.load()
    for y in range(64):
        on = (255, 214, 10) if two_colour and y < 16 else (70, 160, 255) if two_colour else (238, 238, 236)
        for x in range(128):
            if fb[x + (y // 8) * 128] >> (y & 7) & 1:
                for dy in range(S - 1):
                    for dx in range(S - 1):
                        px[S + x * S + dx, S + y * S + dy] = on
    return img


def main():
    port, out = sys.argv[1], sys.argv[2]
    frames = read_frames(port) if port.upper().startswith("COM") or port.startswith("/dev/") else read_frames_http(port)
    missing = [c for c in CHANNELS if c not in frames]
    if missing:
        print("no frame from channel", missing)
    w = 128 * S + 2 * S
    sheet = Image.new("RGB", (len(CHANNELS) * (w + 12) - 12, 64 * S + 2 * S), (40, 40, 44))
    for i, ch in enumerate(CHANNELS):
        if ch in frames:
            sheet.paste(panel(frames[ch], ch in TWO_COLOUR), (i * (w + 12), 0))
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
