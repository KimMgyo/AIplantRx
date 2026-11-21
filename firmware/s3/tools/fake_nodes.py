"""Fake camera and sensor node on the PC, for load-testing the S3 UI before their firmware exists.

    python tools/fake_nodes.py [lan_ip] [--http10]

--http10 closes every connection (firmware before 2.0.0-dev of 11-12 crashed on keep-alive: it kept the
HTTPClient connected past the end of the WiFiClient it pointed to).

Announces {"role":"cam","port":8091} and {"role":"node","port":8092} on UDP 4210 every 2 s and serves
  :8091 GET /frame     QVGA JPEG (24 frames of the mockup plant, swaying, with sensor noise)
  :8092 GET /state     JSON with drifting values and today's device events
  :8092 GET /thermal   32x24 int16 centi-degrees, a warm spot moving across the frame
"""
import io
import json
import math
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
from PIL import Image

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
LAN = ARGS[0] if ARGS else "192.168.10.20"
PROTO = "HTTP/1.0" if "--http10" in sys.argv else "HTTP/1.1"
BCAST = LAN.rsplit(".", 1)[0] + ".255"
HERE = os.path.dirname(os.path.abspath(__file__))
RGB = os.path.join(HERE, "..", "..", "..", "ui", "mockups", "html", "assets", "rgb.png")

base = np.array(Image.open(RGB).convert("RGB").resize((320, 240))).astype(np.int16)
rng = np.random.default_rng(1)
FRAMES = []
for k in range(24):
    dx = int(round(3 * math.sin(k / 24 * 2 * math.pi)))
    img = np.roll(base, dx, axis=1) + rng.normal(0, 4, base.shape).astype(np.int16)
    b = io.BytesIO()
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(b, "JPEG", quality=80)
    FRAMES.append(b.getvalue())
print("frames: %d, %.1f KB each" % (len(FRAMES), sum(map(len, FRAMES)) / len(FRAMES) / 1024))
t_start = time.time()


def thermal():
    t = time.time() - t_start
    y, x = np.mgrid[0:24, 0:32].astype(float)
    f = 24.4 + 0.5 * (y / 24)
    f -= 2.0 * np.exp(-(((x - 15) / 6.0) ** 2 + ((y - 9) / 4.6) ** 2))
    hx, hy = 16 + 12 * math.sin(t / 6), 18 + 3 * math.cos(t / 4)
    f += 6.0 * np.exp(-(((x - hx) / 3.0) ** 2 + ((y - hy) / 2.5) ** 2))
    f += rng.normal(0, 0.08, f.shape)
    return (f * 100).astype("<i2").tobytes()


def state():
    t = time.time() - t_start
    lt = time.localtime()
    return json.dumps({
        "air_c": round(24.6 + 0.4 * math.sin(t / 30), 1), "rh": 58, "co2": 820 + int(30 * math.sin(t / 20)),
        "soil_pct": round(46 + 2 * math.sin(t / 50), 1), "ec": 1.2, "ph": 6.4,
        "heater": False, "light": 6 <= lt.tm_hour < 18, "water": False, "nutr": False,
        "heater_min_today": 30, "water_ml_today": 120,
        "next_nutr": int(time.time()) + 2 * 86400,
        "events": [{"kind": "heater", "at_min": 290, "len_min": 30}, {"kind": "water", "at_min": 580, "len_min": 1}],
    }).encode()


class Cam(BaseHTTPRequestHandler):
    protocol_version = PROTO     # HTTP/1.1 keeps the connection, like esp_http_server on the real nodes
    n = 0

    def do_GET(self):
        if self.path != "/frame":
            return self.send_error(404)
        Cam.n += 1
        body = FRAMES[Cam.n % len(FRAMES)]
        reply(self, body, "image/jpeg")

    def log_message(self, *a):
        pass


class Node(BaseHTTPRequestHandler):
    protocol_version = PROTO

    def do_GET(self):
        if self.path == "/state":
            body, ct = state(), "application/json"
        elif self.path == "/thermal":
            body, ct = thermal(), "application/octet-stream"
        else:
            return self.send_error(404)
        reply(self, body, ct)

    def log_message(self, *a):
        pass


def serve(port, h):
    ThreadingHTTPServer(("0.0.0.0", port), h).serve_forever()


def reply(handler, body, ct):
    # Headers and body in one send with Nagle off, the way one httpd_resp_send() goes out on the nodes.
    handler.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    head = ("%s 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n" % (handler.protocol_version, ct, len(body))).encode()
    handler.wfile.write(head + body)


threading.Thread(target=serve, args=(8091, Cam), daemon=True).start()
threading.Thread(target=serve, args=(8092, Node), daemon=True).start()
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
u.bind((LAN, 0))
print("announcing to %s:4210 from %s" % (BCAST, LAN), flush=True)
last = 0
while True:
    for role, port in (("cam", 8091), ("node", 8092)):
        u.sendto(json.dumps({"role": role, "name": "fake-" + role, "ver": "fake", "port": port}).encode(), (BCAST, 4210))
    time.sleep(2)
    if time.time() - last > 10:
        last = time.time()
        print("frames served: %d" % Cam.n, flush=True)

