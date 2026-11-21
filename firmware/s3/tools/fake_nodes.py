"""Fake ESP32-CAM and sensor node on the PC, so the S3 panel and the C3 OLED can be developed without them.

Implements the device contract in INTERFACES.md (api 1, sections 1-3).

    python tools/fake_nodes.py [lan_ip] [--http10] [--fast] [--photos DIR] [--capture-fail N]

  lan_ip            this PC's address on the boards' network (default 192.168.10.20). It only picks the UDP
                    broadcast address (x.x.x.255); with 127.x the announcements go to 127.0.0.1:4210 instead.
                    The HTTP servers always bind to 0.0.0.0.
  --http10          close every connection (firmware before 2.0.0-dev of 11-12 crashed on keep-alive: it kept the
                    HTTPClient connected past the end of the WiFiClient it pointed to). Default: HTTP/1.1
                    keep-alive with Content-Length on every reply, like esp_http_server.
  --fast            demo time (ARCHITECTURE.md 4.2 "a day in a minute"): one simulated day lasts 60 s for the
                    day-based schedules (water/nutrient intervals, next_water, next_nutr), and soil drying and
                    EC/pH drift run 20x faster. The clock itself stays real: t, day/night, at_min, today's events
                    and the daily caps; so do pump runs, soaking, the 60 s pulse gap and the pH lock.
  --photos DIR      /capture cycles through the .jpg/.png files in DIR (cropped and scaled to the size asked).
                    Default: the mockup plant (ui/mockups/html/assets/rgb.png) scaled up. See tools/photos/.
  --capture-fail N  every Nth /capture answers 503 (to test the S3's retries).

Boards. Each announces {"role","name","ver","port"} on UDP 4210 every 5 s (not while it restarts, is offline or
runs an OTA, like firmware/common/aipnode.cpp):
  fake CAM          role "cam",  name "fake-cam",  ver "fake-1.0.0", HTTP :8091
  fake sensor node  role "node", name "fake-node", ver "fake-1.0.0", HTTP :8092

Both ports (INTERFACES.md 1):
  GET  /info        role, name, ver, host, ip, ssid "fake", rssi, ap, uptime_s, reset, heap, heap_min, pending,
                    ota {state, pct, err}
  POST /wifi        {"ssid","pass"} -> {"ok":true,"trying":ssid}. Only logged; the fake stays where it is.
  POST /ota         {"url":"https://..","sha256":"<64 hex>","size":N,"version":".."} -> 202, 400 bad body,
                    409 busy. Downloads nothing: starting, downloading (pct 0..99 over 10 s), verifying,
                    restarting, then 4 s without HTTP or announcements, then back with "version" as its ver,
                    ota.state "idle", pending true for 20 s (the self-check), reset 3 (software).
  GET  /reboot      "rebooting", then the same 4 s restart.
CAM (INTERFACES.md 3):
  GET  /frame       QVGA JPEG, 24 frames of the mockup plant swaying, with sensor noise.
  GET  /capture     ?size=uxga (1600x1200, default) or sxga (1280x1024): a JPEG after about 1.5 s. The camera
                    does one thing at a time, so /frame waits while a capture runs.
Sensor node (INTERFACES.md 2):
  GET  /state       all the fields of 2, simulated: air and soil drift, soil dries, EC/pH drift, leaf_c and
                    th_max/th_min come from the thermal grid, day and light from the profile's light window
                    (KST), today's pump runs in events.
  GET  /thermal     32x24 int16 little-endian centi-degrees, row-major, 1536 bytes, new frame every 125 ms.
  PUT  /profile     checks the types (400 otherwise), clamps to the hardware ranges, stores it and answers
                    {"ok":true,"profile":{clamped}}. Starts with grade "default", 관엽식물.
  POST /pump        {"pump":"water|nutr|ph","ml":N} or {"pump":..,"s":N} -> {"ok":true,"ms":..,"ml":..}.
                    409 while another pump runs, during capture or OTA, or when today's cap is used up.
  POST /light       {"mode":"capture"|"schedule"|"on"|"off"} -> {"ok":true,"mode":..,"light":..}.
  POST /sim         test control, not part of the contract (any subset):
                      tank_empty bool   the water pump runs but the soil does not get wetter
                      leaf_hot   bool   leaf 3 °C above the air
                      co2        number force the CO2 reading (null: back to the simulation)
                      soil_lost  bool   soil_c/soil_pct/ec/ph null, sensors.soil false (water falls back to the
                                        schedule with an llm profile)
                      mlx_lost   bool   /thermal answers 503, leaf_c/th_max/th_min null, sensors.mlx false
                      offline    s      the node answers nothing and does not announce for that long (0 ends it)
                      cam_offline s     the same for the CAM
                      soil       %      set the soil moisture now (to try the water loop without waiting)
  GET  /sim         the flags, plus a "debug" block (true values, light mode, pump, locks).

Control loop (what the real node will do, INTERFACES.md 2 and ARCHITECTURE.md 4.2):
  water   llm: while soil_pct < soil[0], 3 s pulses (13 ml/s, 39 ml); each wets the soil +4 % over 20 s of
          soaking; at least 60 s between pulses. default: the same, only below 20 %. Soil probe lost with an
          llm profile: water_ml every water_interval_days (runs of at most 10 s, 60 s apart), next_water set.
  nutr    llm only: nutr_ml every nutr_interval_days (2 ml/s). The first feed is due one day after the first
          llm profile arrives.
  ph      llm only: pH above ph[1] -> 2 ml of pH-down (one direction only), then locked for 10 minutes.
  safety  one pump at a time, none during capture or OTA, at most 10 s per run, per day at most 500 ml of water
          and 30 ml each of nutrient and pH-down.
One line is logged per state change (pump on/off, profile, light mode, OTA phases, sim flags, captures) and a
traffic summary every 30 s, never one per poll.
"""
import argparse
import collections
import io
import json
import math
import os
import random
import re
import select
import socket
import struct
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import numpy as np
from PIL import Image, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
RGB = os.path.normpath(os.path.join(HERE, "..", "..", "..", "ui", "mockups", "html", "assets", "rgb.png"))

KST = 9 * 3600
FLOW = {"water": 13.0, "nutr": 2.0, "ph": 2.0}      # ml/s: 12 V water pump, peristaltic dosing pumps
DAY_CAP_ML = {"water": 500, "nutr": 30, "ph": 30}
MAX_RUN_S = 10
PULSE_S, SOAK_S, PULSE_GAP_S = 3, 20, 60
PUMP_GAP_S = 2                                     # between two different pumps (the 12 V rail settles)
SOIL_PER_ML = 4.0 / (PULSE_S * FLOW["water"])      # +4 % per 3 s pulse
DEFAULT_SOIL_MIN = 20                              # grade "default": water only when the soil is this dry
PH_DOSE_ML, PH_LOCK_S = 2.0, 600
CAPTURE_S = 30
REBOOT_S = 4
CONFIRM_S = 20                                     # aipnode.cpp CONFIRM_AFTER_MS
OTA_DOWNLOAD_S = 10
ANNOUNCE_S = 5
MAX_EVENTS = 32                                    # keeps /state well inside the S3's 8 KB buffer
SIZES = {"uxga": (1600, 1200), "sxga": (1280, 1024)}

DEFAULT_PROFILE = {
    "grade": "default", "name_ko": "관엽식물", "sci": "", "family": "", "score": None,
    "temp_day": [20, 28], "temp_night": [18, 22], "rh": [60, 80],
    "soil": [40, 60], "ec": [0.8, 1.4], "ph": [5.5, 6.5],
    "light_on": "07:00", "light_off": "19:00",
    "water_interval_days": 3, "water_ml": 100,
    "nutr_interval_days": 14, "nutr_ml": 10,
}
# Hardware safety ranges: (lo, hi, decimals). The LLM cannot widen these.
RANGES = {"temp_day": (10, 32, 1), "temp_night": (10, 32, 1), "rh": (20, 95, 0),
          "soil": (10, 80, 0), "ec": (0.3, 3.0, 2), "ph": (5.0, 7.5, 2)}
SCALARS = {"water_interval_days": (1, 21), "water_ml": (20, 300), "nutr_interval_days": (3, 30), "nutr_ml": (0, 20)}
LIGHT_MIN_H, LIGHT_MAX_H = 8, 16


class Cfg:
    lan = "192.168.10.20"
    proto = "HTTP/1.1"
    fast = False
    day_s = 86400          # one "day" of the schedules
    drift = 1              # speed of soil drying and EC/pH drift
    photos = None
    capture_fail = 0


DEVICES = {}
_log_lock = threading.Lock()


def log(who, msg):
    with _log_lock:
        print("%s [%s] %s" % (time.strftime("%H:%M:%S"), who, msg), flush=True)


def kst_min(t):
    return int((t + KST) % 86400 // 60)


def kst_day(t):
    return int((t + KST) // 86400)


def kst_midnight(t):
    return kst_day(t) * 86400 - KST


def kst_hm(t):
    return "%02d:%02d" % divmod(kst_min(t), 60)


def hm_min(s):
    h, m = s.split(":")
    return int(h) * 60 + int(m)


def isnum(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def tidy(v, nd):
    v = round(float(v), nd)
    return int(v) if v == int(v) else v


def r1(v):
    return None if v is None else round(float(v), 1)


# ---------------------------------------------------------------- profile

def clean_profile(body):
    """PUT /profile body -> (profile clamped to the hardware ranges, notes). ValueError on wrong types."""
    if not isinstance(body, dict):
        raise ValueError("need a JSON object")
    errs, notes = [], []
    out = {"grade": body.get("grade")}
    if out["grade"] not in ("llm", "default"):
        errs.append('grade must be "llm" or "default"')
    for k in ("name_ko", "sci", "family"):
        v = body.get(k)
        if v is None:
            v = ""
        if not isinstance(v, str):
            errs.append(k + " must be a string")
            continue
        out[k] = v[:48]
    sc = body.get("score")
    if sc is not None and not isnum(sc):
        errs.append("score must be a number or null")
    out["score"] = tidy(sc, 1) if isnum(sc) else None
    if "t" in body and not isnum(body["t"]):
        errs.append("t must be epoch seconds")
    for k, (lo, hi, nd) in RANGES.items():
        v = body.get(k)
        if not (isinstance(v, list) and len(v) == 2 and all(isnum(x) for x in v)):
            errs.append(k + " must be [lo, hi] numbers")
            continue
        a, b = v
        if a > b:
            a, b = b, a
            notes.append(k + " swapped")
        ca, cb = min(max(a, lo), hi), min(max(b, lo), hi)
        if (ca, cb) != (a, b):
            notes.append("%s %s -> [%s, %s]" % (k, v, tidy(ca, nd), tidy(cb, nd)))
        out[k] = [tidy(ca, nd), tidy(cb, nd)]
    mins = {}
    for k in ("light_on", "light_off"):
        v = body.get(k)
        m = re.fullmatch(r"(\d{1,2}):(\d{2})", v) if isinstance(v, str) else None
        if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
            errs.append(k + ' must be "HH:MM"')
            continue
        mins[k] = int(m.group(1)) * 60 + int(m.group(2))
    if len(mins) == 2:
        on, off = mins["light_on"], mins["light_off"]
        dur = (off - on) % 1440
        if dur < LIGHT_MIN_H * 60 or dur > LIGHT_MAX_H * 60:
            off = (on + (LIGHT_MIN_H if dur < LIGHT_MIN_H * 60 else LIGHT_MAX_H) * 60) % 1440
            notes.append("light window %s-%s -> %s-%02d:%02d" % (body["light_on"], body["light_off"],
                                                                 body["light_on"], off // 60, off % 60))
        out["light_on"] = "%02d:%02d" % divmod(on, 60)
        out["light_off"] = "%02d:%02d" % divmod(off, 60)
    for k, (lo, hi) in SCALARS.items():
        v = body.get(k)
        if not isnum(v):
            errs.append(k + " must be a number")
            continue
        c = int(round(min(max(v, lo), hi)))
        if c != v:
            notes.append("%s %s -> %d" % (k, v, c))
        out[k] = c
    if errs:
        raise ValueError("; ".join(errs))
    order = ["grade", "name_ko", "sci", "family", "score", "temp_day", "temp_night", "rh", "soil", "ec", "ph",
             "light_on", "light_off", "water_interval_days", "water_ml", "nutr_interval_days", "nutr_ml"]
    return {k: out[k] for k in order}, notes


# ---------------------------------------------------------------- one board (INTERFACES.md 1)

class Device:
    def __init__(self, role, port, heap):
        self.role, self.port, self.heap = role, port, heap
        self.name = "fake-" + role
        self.ver = "fake-1.0.0"
        self.lock = threading.RLock()
        self.boot_t = time.time()
        self.reset = 1                       # ESP_RST_POWERON
        self.boots = 0
        self.ota = {"state": "idle", "pct": -1, "err": ""}
        self.ota_busy = False
        self.down_until = 0.0                # no HTTP, no announcements (restart or /sim offline)
        self.off_until = 0.0                 # restarting: the board itself is off
        self.gen = 0                         # bumped when the board goes away: older connections are dead
        self.back_msg = ""
        self.pending_until = 0.0
        self.announce_now = True
        self.extra = None                    # -> dict of board-specific /info fields
        self.on_reboot = None                # outputs off when the board restarts
        self.on_ota_start = None             # outputs off before an OTA
        self.served = collections.Counter()

    def down(self, now=None):
        return (time.time() if now is None else now) < self.down_until

    def info(self):
        now = time.time()
        with self.lock:
            d = {"role": self.role, "name": self.name, "ver": self.ver, "host": "aiplantrx-" + self.name,
                 "ip": Cfg.lan, "ssid": "fake", "rssi": -55 + random.randint(-3, 3), "ap": False,
                 "uptime_s": int(max(0, now - self.boot_t)), "reset": self.reset,
                 "heap": self.heap + random.randint(-1500, 1500), "heap_min": self.heap - 23000,
                 "pending": now < self.pending_until, "ota": dict(self.ota)}
        if self.extra:
            d.update(self.extra())
        return d

    def go_offline(self, seconds):
        now = time.time()
        with self.lock:
            if seconds <= 0:
                if now >= self.off_until and self.down_until > now:
                    self.down_until = now
                return
            self.down_until = max(self.down_until, now + seconds)
            self.gen += 1
            self.back_msg = "back online after %g s" % seconds
        log(self.role, "offline for %g s: no HTTP, no announcements" % seconds)

    def start_ota(self, d):
        with self.lock:
            if self.ota_busy:
                return 409, "busy"
        if not isinstance(d, dict):
            return 400, 'need {"url","sha256","size"}'
        url, sha, size, ver = d.get("url"), d.get("sha256"), d.get("size"), d.get("version")
        if not (isinstance(url, str) and url.startswith("https://") and len(url) < 512
                and isinstance(sha, str) and re.fullmatch(r"[0-9a-fA-F]{64}", sha)
                and isinstance(size, int) and not isinstance(size, bool) and 0 < size < 2 ** 32):
            return 400, "bad url, sha256 or size"
        ver = ver[:23] if isinstance(ver, str) else ""
        with self.lock:
            if self.ota_busy:
                return 409, "busy"
            self.ota_busy = True
            self.ota = {"state": "starting", "pct": 0, "err": ""}
            boot = self.boots
        threading.Thread(target=self._ota, args=(url, size, ver, boot), daemon=True).start()
        return 202, {"ok": True}

    def _ota(self, url, size, ver, boot):
        log(self.role, "OTA %s (%d bytes) from %s: starting" % (ver or "(no version)", size, url))
        if self.on_ota_start:
            self.on_ota_start()

        def phase(state, pct, msg=None):
            with self.lock:
                if self.boots != boot:       # restarted meanwhile: that OTA is gone with the RAM
                    return False
                self.ota.update(state=state, pct=pct)
            if msg:
                log(self.role, msg)
            return True

        time.sleep(0.5)
        if not phase("downloading", 0, "OTA: downloading (%d s)" % OTA_DOWNLOAD_S):
            return
        t0 = time.time()
        while time.time() - t0 < OTA_DOWNLOAD_S:
            time.sleep(0.2)
            if not phase("downloading", min(99, int((time.time() - t0) * 100 / OTA_DOWNLOAD_S))):
                return
        if not phase("verifying", 99, "OTA: verifying size and sha256"):
            return
        time.sleep(1.0)
        if not phase("restarting", 100, "OTA: done, restarting"):
            return
        self._restart(ver or None, 0.8, "OTA")

    def restart(self, why):
        threading.Thread(target=self._restart, args=(None, 0.3, why), daemon=True).start()

    def _restart(self, new_ver, delay, why):
        time.sleep(delay)                    # the reply goes out first
        with self.lock:
            now = time.time()
            self.boots += 1
            self.off_until = now + REBOOT_S
            self.down_until = max(self.down_until, self.off_until)
            self.gen += 1
            old = self.ver
            if new_ver:
                self.ver = new_ver
            self.boot_t = self.off_until
            self.reset = 3                   # ESP_RST_SW
            self.ota_busy = False
            self.ota = {"state": "idle", "pct": -1, "err": ""}
            self.pending_until = self.off_until + CONFIRM_S if new_ver else 0.0
            self.back_msg = ("back up as %s (was %s), pending its self-check for %d s" % (self.ver, old, CONFIRM_S)
                             if new_ver else "back up as %s" % self.ver)
            self.announce_now = True
        if self.on_reboot:
            self.on_reboot()
        log(self.role, "restarting (%s): no HTTP, no announcements for %d s" % (why, REBOOT_S))


# ---------------------------------------------------------------- CAM (INTERFACES.md 3)

class Cam:
    def __init__(self, dev):
        self.dev = dev
        base = np.array(Image.open(RGB).convert("RGB").resize((320, 240))).astype(np.int16)
        rng = np.random.default_rng(1)
        self.frames = []
        for k in range(24):
            dx = int(round(3 * math.sin(k / 24 * 2 * math.pi)))
            img = np.roll(base, dx, axis=1) + rng.normal(0, 4, base.shape).astype(np.int16)
            b = io.BytesIO()
            Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(b, "JPEG", quality=80)
            self.frames.append(b.getvalue())
        print("frames: %d, %.1f KB each" % (len(self.frames), sum(map(len, self.frames)) / len(self.frames) / 1024))
        self.n = 0
        self.captures = 0
        self.lock = threading.Lock()         # the camera does one thing at a time
        self.cache = collections.OrderedDict()
        self.warned = False
        dev.extra = lambda: {"frames": self.n, "captures": self.captures}

    def frame(self):
        with self.lock:                      # waits while a capture holds the sensor
            self.n += 1
            return self.frames[self.n % len(self.frames)]

    def source(self, n):
        if Cfg.photos:
            try:
                files = sorted(f for f in os.listdir(Cfg.photos) if f.lower().endswith((".jpg", ".jpeg", ".png")))
            except OSError:
                files = []
            if files:
                return os.path.join(Cfg.photos, files[(n - 1) % len(files)])
            if not self.warned:
                self.warned = True
                log("cam", "no .jpg/.png in %s: using rgb.png" % Cfg.photos)
        return RGB

    def jpeg(self, path, w, h, q=12):
        # q is the OV2640's jpeg_quality (4..63, lower = better); 12 ~ Pillow quality 70.
        quality = max(20, min(95, int(100 - q * 2.5)))
        key = (path, os.path.getmtime(path), w, h, quality)
        if key not in self.cache:
            im = Image.open(path)
            im.draft("RGB", (max(w, h), max(w, h)))
            im = ImageOps.exif_transpose(im).convert("RGB")
            im = ImageOps.fit(im, (w, h), Image.Resampling.LANCZOS)
            b = io.BytesIO()
            im.save(b, "JPEG", quality=quality)
            self.cache[key] = b.getvalue()
            while len(self.cache) > 12:
                self.cache.popitem(last=False)
        return self.cache[key]

    def capture(self, size, q=12):
        w, h = SIZES[size]
        with self.lock:
            t0 = time.time()
            self.captures += 1
            n = self.captures
            src = self.source(n)
            fail = Cfg.capture_fail > 0 and n % Cfg.capture_fail == 0
            body = None if fail else self.jpeg(src, w, h, q)
            time.sleep(max(0.0, 1.5 + random.uniform(-0.1, 0.3) - (time.time() - t0)))
            took = time.time() - t0
        if fail:
            log("cam", "capture #%d %s: 503 after %.1f s (--capture-fail %d)" % (n, size, took, Cfg.capture_fail))
        else:
            log("cam", "capture #%d %s %dx%d: %d KB in %.1f s (%s)" % (n, size, w, h, len(body) // 1024, took,
                                                                     os.path.basename(src)))
        return body


# ---------------------------------------------------------------- sensor node (INTERFACES.md 2)

GY, GX = np.mgrid[0:24, 0:32].astype(float)
LEAF_MASK = 1.0 / (1.0 + (((GX - 15.5) / 8.5) ** 2 + ((GY - 11.0) / 6.5) ** 2) ** 4)


class Node:
    def __init__(self, dev):
        self.dev = dev
        self.lock = threading.RLock()
        self.rng = np.random.default_rng(2)
        now = time.time()
        self.profile, _ = clean_profile(DEFAULT_PROFILE)
        self.llm_since = 0.0
        # The room and the plant, as the sensors would read them without noise.
        self.air, self.rh, self.co2 = 24.6, 58.0, 820.0
        self.soil, self.soil_c, self.ec, self.ph = 47.0, 22.8, 1.20, 6.40
        self.effects = []                    # [t0, t1, attribute, total]: a change spread over time
        self.sim = {"tank_empty": False, "leaf_hot": False, "co2": None, "soil_lost": False, "mlx_lost": False}
        self.pump = None
        self.last_end = dict.fromkeys(FLOW, 0.0)
        self.last_any_end = 0.0
        self.soak_until = 0.0
        self.ph_lock_until = 0.0
        self.mode, self.mode_until = "schedule", 0.0
        self.today = kst_day(now)
        self.events = []
        self.ml_today = dict.fromkeys(FLOW, 0.0)
        self.last_water = self.last_nutr = self.next_nutr = self.next_water = 0.0
        self.water_left = 0.0                # ml still to give in a schedule-based watering
        self.once = set()
        self.read = {}
        self.thermal = b""
        self.th = (None, None, None)
        self.t_last = now
        self.t_read = 0.0
        # One water pulse two hours ago, so the day's timeline is not empty at first look.
        t = now - 7200
        if kst_day(t) == self.today:
            ml = PULSE_S * FLOW["water"]
            self.events.append({"kind": "water", "at_min": kst_min(t), "len_s": PULSE_S, "ml": int(round(ml))})
            self.ml_today["water"] = ml
            self.last_water = t
            self.last_end["water"] = t + PULSE_S
        self._sense(now)
        self._thermal(now)
        dev.on_reboot = lambda: self.outputs_off("restart")
        dev.on_ota_start = lambda: self.outputs_off("OTA")

    # -- time and light
    def is_day(self, now):
        on, off = hm_min(self.profile["light_on"]), hm_min(self.profile["light_off"])
        m = ((now + KST) % 86400) / 60.0
        return on <= m < off if on < off else (m >= on or m < off)

    def next_boundary(self, now):
        mid = kst_midnight(now)
        c = [mid + d * 86400 + hm_min(self.profile[k]) * 60 for d in (0, 1) for k in ("light_on", "light_off")]
        return min(x for x in c if x > now)

    def light(self, now):
        if self.mode in ("capture", "on"):
            return True
        if self.mode == "off":
            return False
        return self.is_day(now)

    def left(self, kind):
        return DAY_CAP_ML[kind] - self.ml_today[kind]

    def _once(self, key, msg):
        if key not in self.once:
            self.once.add(key)
            log("node", msg)

    # -- simulation
    def run(self):
        while True:
            try:
                self.tick()
            except Exception as e:          # keep the board alive; a bug here should not stop the servers
                log("node", "sim error: %r" % e)
            time.sleep(0.125)

    def tick(self):
        now = time.time()
        with self.lock:
            dt = min(2.0, max(0.0, now - self.t_last))
            self.t_last = now
            if kst_day(now) != self.today:
                self.today = kst_day(now)
                self.events, self.ml_today = [], dict.fromkeys(FLOW, 0.0)
                self.once.clear()
                log("node", "new day (KST): today's events and totals cleared")
            self._physics(now, dt)
            if now - self.t_read >= 1.0:
                self.t_read = now
                self._sense(now)
            self._thermal(now)
            if self.mode != "schedule" and now >= self.mode_until:
                log("node", "light: %s ended, back to schedule (%s)" % (self.mode, "on" if self.is_day(now) else "off"))
                self.mode = "schedule"
            if self.pump and now >= self.pump["end"]:
                self._stop(now, "done")
            self._control(now)

    def _physics(self, now, dt):
        lit = self.light(now)
        self.air += ((25.0 if lit else 21.0) + (0.8 if self.mode == "capture" else 0.0) - self.air) * min(1.0, dt / 1200)
        self.rh += (64.0 - 2.5 * (self.air - 22.0) - self.rh) * min(1.0, dt / 900)
        self.co2 += ((740.0 if lit else 900.0) - self.co2) * min(1.0, dt / 1800)
        self.soil_c += (self.air - 1.8 - self.soil_c) * min(1.0, dt / 2400)
        dry = 1.5 * (1.3 if lit else 0.6) * Cfg.drift * min(1.0, self.soil / 30.0)    # %/h
        self.soil = max(0.0, self.soil - dry * dt / 3600)
        self.ec = max(0.2, self.ec - 0.02 * Cfg.drift * dt / 3600)
        self.ph = min(8.0, self.ph + 0.03 * Cfg.drift * dt / 3600)
        prev, keep = now - dt, []
        for e in self.effects:
            t0, t1, attr, total = e
            a, b = max(prev, t0), min(now, t1)
            if b > a:
                setattr(self, attr, getattr(self, attr) + total * (b - a) / (t1 - t0))
            if t1 > now:
                keep.append(e)
        self.effects = keep
        self.soil = min(self.soil, 92.0)

    def _sense(self, now):
        g = self.rng.normal
        r = {"air_c": round(self.air + 0.15 * math.sin(now / 47) + g(0, 0.03), 1),
             "rh": int(round(min(99.0, max(5.0, self.rh + 2 * math.sin(now / 130) + g(0, 0.3))))),
             "co2": int(round(max(400.0, self.co2 + 20 * math.sin(now / 70) + g(0, 4))))}
        if self.sim["co2"] is not None:
            r["co2"] = int(round(self.sim["co2"]))
        if self.sim["soil_lost"]:
            r.update(soil_c=None, soil_pct=None, ec=None, ph=None)
        else:
            r.update(soil_c=round(self.soil_c + g(0, 0.03), 1), soil_pct=round(max(0.0, self.soil + g(0, 0.08)), 1),
                     ec=round(max(0.0, self.ec + g(0, 0.004)), 2), ph=round(self.ph + g(0, 0.008), 1))
        self.read = r

    def _thermal(self, now):
        lit = self.light(now)
        if self.sim["leaf_hot"]:
            d = 3.0
        else:   # transpiring leaves sit below the air; a drying plant closes its stomata and warms up
            d = (-1.3 if lit else -0.4) + max(0.0, (20.0 - self.soil) * 0.1)
        bg = self.air - 0.3 + 0.6 * (GY / 23.0)
        leaf = self.air + d + 0.25 * np.sin(GX / 1.7) * np.cos(GY / 2.3)
        f = bg * (1 - LEAF_MASK) + leaf * LEAF_MASK
        hx, hy = 16 + 13 * math.sin(now / 6), 21 + 1.5 * math.cos(now / 4)     # a warm spot (pot rim, lamp)
        f += 5.0 * np.exp(-(((GX - hx) / 3.0) ** 2 + ((GY - hy) / 2.0) ** 2))
        f += self.rng.normal(0, 0.08, f.shape)
        raw = np.round(f * 100).astype("<i2")
        box = np.sort(raw[4:15, 9:23], axis=None)          # columns 9..22, rows 4..14 (INTERFACES.md 2)
        self.thermal = raw.tobytes()
        self.th = (int(box[box.size // 2]) / 100.0, int(raw.max()) / 100.0, int(raw.min()) / 100.0)

    # -- pumps
    def _start(self, kind, ml, why):
        now = time.time()
        secs = ml / FLOW[kind]
        self.pump = {"kind": kind, "t0": now, "end": now + secs, "ml": ml, "why": why}
        log("node", "%s pump on: %.1f s, %d ml (%s)" % (kind, secs, round(ml), why))
        return secs

    def _stop(self, now, why):
        p, self.pump = self.pump, None
        k = p["kind"]
        run = max(0.0, min(now, p["end"]) - p["t0"])
        ml = p["ml"] if now >= p["end"] else FLOW[k] * run
        self.events.append({"kind": k, "at_min": kst_min(p["t0"]), "len_s": max(1, int(round(run))),
                            "ml": int(round(ml))})
        del self.events[:-MAX_EVENTS]
        self.ml_today[k] += ml
        self.last_end[k] = self.last_any_end = now
        note = "" if why == "done" else " (stopped: %s)" % why
        if k == "water":
            self.last_water = p["t0"]
            self.soak_until = now + SOAK_S
            if self.sim["tank_empty"]:
                note += " (tank empty: nothing came out)"
            else:
                self.effects.append([now, now + SOAK_S, "soil", ml * SOIL_PER_ML])
                self.effects.append([now, now + SOAK_S, "ec", -0.0005 * ml])
        elif k == "nutr":
            self.effects.append([now, now + 30, "ec", 0.02 * ml])
            self.effects.append([now, now + 30, "ph", -0.005 * ml])
        else:
            self.effects.append([now, now + 60, "ph", -0.08 * ml])
        log("node", "%s pump off: %.1f s, %d ml, today %d ml%s" % (k, run, round(ml), round(self.ml_today[k]), note))

    def outputs_off(self, why):
        with self.lock:
            if self.pump:
                self._stop(time.time(), why)
            if why == "restart" and self.mode != "schedule":
                self.mode, self.mode_until = "schedule", 0.0

    def _water_schedule(self, now):
        """next_water exists only while watering runs on the schedule: llm profile and the soil probe lost."""
        p = self.profile
        if p["grade"] != "llm" or self.read.get("soil_pct") is not None:
            if self.next_water and self.read.get("soil_pct") is not None:
                log("node", "soil probe back: watering follows the soil again")
            self.next_water, self.water_left = 0.0, 0.0
        elif not self.next_water:
            self.next_water = self.last_water + p["water_interval_days"] * Cfg.day_s if self.last_water else now
            log("node", "soil probe lost: watering on the schedule, %d ml every %d days, next %s"
                % (p["water_ml"], p["water_interval_days"], kst_hm(max(now, self.next_water))))

    def _control(self, now):
        p, r = self.profile, self.read
        llm = p["grade"] == "llm"
        soil_ok = r.get("soil_pct") is not None
        sched = llm and not soil_ok
        self._water_schedule(now)
        if self.pump or self.mode == "capture" or self.dev.ota_busy or now < self.dev.off_until:
            return
        if now - self.last_any_end < PUMP_GAP_S:
            return
        # nutrient: on its schedule
        if llm and self.next_nutr and now >= self.next_nutr:
            ml = p["nutr_ml"]
            self.last_nutr = now
            self.next_nutr = now + p["nutr_interval_days"] * Cfg.day_s
            if ml <= 0:
                log("node", "nutrient due but nutr_ml is 0: skipped, next %s" % kst_hm(self.next_nutr))
            elif self.left("nutr") < ml:
                log("node", "nutrient due but today's %d ml cap is used: skipped, next %s"
                    % (DAY_CAP_ML["nutr"], kst_hm(self.next_nutr)))
            else:
                self._start("nutr", ml, "schedule, every %d days, next %s" % (p["nutr_interval_days"], kst_hm(self.next_nutr)))
                return
        # pH: one direction only (pH-down), a small dose, then a lock
        if llm and soil_ok and now >= self.ph_lock_until:
            ph = r["ph"]
            if ph > p["ph"][1]:
                ml = min(PH_DOSE_ML, self.left("ph"))
                if ml >= 0.5:
                    self.ph_lock_until = now + PH_LOCK_S
                    self._start("ph", ml, "pH %.1f > %g, then locked %d min" % (ph, p["ph"][1], PH_LOCK_S // 60))
                    return
                self._once("ph_cap", "pH high but today's %d ml pH cap is used" % DAY_CAP_ML["ph"])
            elif ph < p["ph"][0]:
                self._once("ph_low", "pH %.1f below %g: the pH pump only lowers pH, nothing to do" % (ph, p["ph"][0]))
            else:
                self.once.discard("ph_low")
        # water
        if soil_ok:
            thr = p["soil"][0] if llm else DEFAULT_SOIL_MIN
            if r["soil_pct"] < thr and now >= self.soak_until and now - self.last_end["water"] >= PULSE_GAP_S:
                ml = min(PULSE_S * FLOW["water"], self.left("water"))
                if ml >= 1:
                    self._start("water", ml, "soil %.1f%% < %g%%%s" % (r["soil_pct"], thr, "" if llm else " (default profile)"))
                    return
                self._once("water_cap", "soil dry but today's %d ml water cap is used" % DAY_CAP_ML["water"])
        elif sched:
            if self.water_left <= 0 and now >= self.next_water:
                self.water_left = float(p["water_ml"])
                self.next_water = now + p["water_interval_days"] * Cfg.day_s
                log("node", "scheduled watering (soil probe lost): %d ml, next %s" % (p["water_ml"], kst_hm(self.next_water)))
            if self.water_left > 0 and now - self.last_end["water"] >= PULSE_GAP_S:
                ml = min(self.water_left, MAX_RUN_S * FLOW["water"], self.left("water"))
                if ml >= 1:
                    self.water_left -= ml
                    self._start("water", ml, "schedule, %d ml left after this" % round(self.water_left))
                    return
                self.water_left = 0.0
                self._once("water_cap", "scheduled watering cut: today's %d ml water cap is used" % DAY_CAP_ML["water"])

    # -- requests
    def state_json(self):
        now = time.time()
        with self.lock:
            r = self.read
            mlx, soil = not self.sim["mlx_lost"], not self.sim["soil_lost"]
            leaf, tmax, tmin = self.th if mlx else (None, None, None)
            k = self.pump["kind"] if self.pump else None
            d = {"api": 1, "t": int(now),
                 "air_c": r["air_c"], "rh": r["rh"], "co2": r["co2"],
                 "soil_c": r["soil_c"], "soil_pct": r["soil_pct"], "ec": r["ec"], "ph": r["ph"],
                 "leaf_c": r1(leaf), "th_max": r1(tmax), "th_min": r1(tmin),
                 "day": self.is_day(now),
                 "light": self.light(now), "water": k == "water", "nutr": k == "nutr", "ph_pump": k == "ph",
                 "capture": self.mode == "capture",
                 "water_ml_today": int(round(self.ml_today["water"])), "nutr_ml_today": int(round(self.ml_today["nutr"])),
                 "ph_ml_today": int(round(self.ml_today["ph"])),
                 "last_water": int(self.last_water), "next_water": int(self.next_water), "next_nutr": int(self.next_nutr),
                 "events": self.events,
                 "sensors": {"scd41": True, "mlx": mlx, "soil": soil},
                 "profile": self.profile}
            return json.dumps(d, ensure_ascii=False, separators=(",", ":")).encode("utf-8")

    def thermal_bytes(self):
        with self.lock:
            return None if self.sim["mlx_lost"] else self.thermal

    def set_profile(self, body):
        prof, notes = clean_profile(body)      # ValueError -> 400
        now = time.time()
        with self.lock:
            self.profile = prof
            if prof["grade"] == "llm":
                if not self.llm_since:
                    self.llm_since = now
                self.next_nutr = (self.last_nutr + prof["nutr_interval_days"] * Cfg.day_s if self.last_nutr
                                  else self.llm_since + Cfg.day_s)
            else:
                self.llm_since, self.next_nutr = 0.0, 0.0
            self.next_water = 0.0              # from the new interval, if it waters on the schedule
            self._water_schedule(now)
            self.once.clear()
            nn = kst_hm(self.next_nutr) if self.next_nutr else "-"
        t = body.get("t")
        log("node", "profile set: %s %s %s, soil %s, ec %s, ph %s, light %s-%s, water %d ml/%d d, nutr %d ml/%d d, "
            "next nutr %s%s" % (prof["grade"], prof["name_ko"], prof["sci"], prof["soil"], prof["ec"], prof["ph"],
                                prof["light_on"], prof["light_off"], prof["water_ml"], prof["water_interval_days"],
                                prof["nutr_ml"], prof["nutr_interval_days"], nn,
                                "; clamped: " + ", ".join(notes) if notes else ""))
        if isnum(t) and abs(t - now) > 30:
            log("node", "profile t is %+d s from the PC clock (the fake keeps the PC clock)" % (t - now))
        return {"ok": True, "profile": prof}

    def manual_pump(self, kind, ml, s):
        with self.lock:
            if self.dev.ota_busy:
                return 409, "OTA in progress: outputs off"
            if self.mode == "capture":
                return 409, "capture light on: pumps blocked"
            if self.pump:
                return 409, "%s pump running" % self.pump["kind"]
            secs = min(s if ml is None else ml / FLOW[kind], MAX_RUN_S)
            ml = secs * FLOW[kind]
            left = self.left(kind)
            if left < 0.5:
                return 409, "daily limit: %d ml of %s already today" % (DAY_CAP_ML[kind], kind)
            ml = min(ml, left)
            secs = self._start(kind, ml, "POST /pump")
            return 200, {"ok": True, "ms": int(round(secs * 1000)), "ml": int(round(ml))}

    def set_light(self, mode):
        now = time.time()
        with self.lock:
            if mode == "capture":
                if self.pump:
                    self._stop(now, "capture")
                self.mode_until = now + CAPTURE_S
                msg = "capture (white, pumps stopped and blocked) for %d s" % CAPTURE_S
            elif mode in ("on", "off"):
                self.mode_until = self.next_boundary(now)
                msg = "manual %s until the schedule boundary at %s" % (mode, kst_hm(self.mode_until))
            else:
                self.mode_until = 0.0
                msg = "schedule (%s-%s, now %s)" % (self.profile["light_on"], self.profile["light_off"],
                                                    "on" if self.is_day(now) else "off")
            self.mode = mode
            light = self.light(now)
        log("node", "light: " + msg)
        return {"ok": True, "mode": mode, "light": light}

    SIM_FIELDS = {"tank_empty": "bool", "leaf_hot": "bool", "soil_lost": "bool", "mlx_lost": "bool",
                  "co2": "co2", "offline": "secs", "cam_offline": "secs", "soil": "pct"}

    def sim_set(self, d):
        """Checks and applies POST /sim. Returns (error, offline seconds per role)."""
        if not isinstance(d, dict):
            return "need a JSON object", {}
        for k, v in d.items():
            kind = self.SIM_FIELDS.get(k)
            if kind is None:
                return "unknown field %s (known: %s)" % (k, ", ".join(self.SIM_FIELDS)), {}
            if kind == "bool" and not isinstance(v, bool):
                return k + " must be true or false", {}
            if kind == "co2" and v is not None and not (isnum(v) and 0 <= v <= 40000):
                return "co2 must be ppm (0..40000) or null", {}
            if kind == "secs" and not (isnum(v) and 0 <= v <= 3600):
                return k + " must be seconds (0..3600)", {}
            if kind == "pct" and not (isnum(v) and 0 <= v <= 100):
                return "soil must be 0..100", {}
        now = time.time()
        with self.lock:
            for k, v in d.items():
                if k in self.sim and self.sim[k] != v:
                    self.sim[k] = v
                    log("node", "sim: %s = %s" % (k, json.dumps(v)))
                elif k == "soil":
                    self.soil = float(v)
                    self.effects = [e for e in self.effects if e[2] != "soil"]
                    log("node", "sim: soil set to %g %%" % v)
            self._sense(now)
            self._thermal(now)
            self._water_schedule(now)
        off = {}
        if "offline" in d:
            off["node"] = d["offline"]
        if "cam_offline" in d:
            off["cam"] = d["cam_offline"]
        return None, off

    def sim_get(self):
        now = time.time()
        with self.lock:
            out = dict(self.sim)
            out["offline"] = max(0, int(math.ceil(self.dev.down_until - now)))
            cam = DEVICES.get("cam")
            out["cam_offline"] = max(0, int(math.ceil(cam.down_until - now))) if cam else 0
            p = self.pump
            out["debug"] = {
                "fast": Cfg.fast, "day_s": Cfg.day_s,
                "soil_true": round(self.soil, 2), "ec_true": round(self.ec, 3), "ph_true": round(self.ph, 3),
                "air_true": round(self.air, 2), "light_mode": self.mode,
                "mode_left_s": max(0, int(self.mode_until - now)) if self.mode != "schedule" else 0,
                "pump": None if not p else {"kind": p["kind"], "left_s": round(p["end"] - now, 1), "ml": round(p["ml"])},
                "soaking_s": max(0, int(self.soak_until - now)),
                "next_pulse_s": max(0, int(self.last_end["water"] + PULSE_GAP_S - now)),
                "ph_lock_s": max(0, int(self.ph_lock_until - now)),
                "water_left_ml": round(self.water_left),
                "caps_left_ml": {k: round(self.left(k)) for k in FLOW},
            }
        return out


# ---------------------------------------------------------------- HTTP

class Server(ThreadingHTTPServer):
    allow_reuse_address = os.name != "nt"    # on Windows SO_REUSEADDR would let a second copy bind the same port
    daemon_threads = True
    block_on_close = False
    request_queue_size = 32


LINGER0 = struct.pack("HH" if os.name == "nt" else "ii", 1, 0)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"            # keeps the connection, like esp_http_server on the real boards
    dev = None
    ROUTES = {"/info": {"GET": "h_info"}, "/wifi": {"POST": "h_wifi"}, "/ota": {"POST": "h_ota"},
              "/reboot": {"GET": "h_reboot"}}

    def setup(self):
        super().setup()
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.gen = -1 if self.dev.down() else self.dev.gen   # made while the board was away: it never existed

    def log_message(self, *a):
        pass

    def do_GET(self):
        self.dispatch("GET")

    def do_POST(self):
        self.dispatch("POST")

    def do_PUT(self):
        self.dispatch("PUT")

    def dispatch(self, method):
        if self.dev.down() or self.gen != self.dev.gen:
            return self.swallow()
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = -1
        if n < 0 or n > 16384:
            self.close_connection = True
            return self.send(413 if n > 0 else 400, "bad Content-Length")
        self.body = self.rfile.read(n) if n else b""
        u = urlsplit(self.path)
        self.query = parse_qs(u.query)
        route = self.ROUTES.get(u.path)
        if route is None:
            return self.send(404, "not found")
        fn = route.get(method)
        if fn is None:
            return self.send(405, "method not allowed")
        self.dev.served[u.path] += 1
        getattr(self, fn)()

    def swallow(self):
        """The board is away (restarting or offline): no answer at all, the way a board that is not on the
        network behaves. When it is back, connections from before are reset, as after a real restart."""
        self.close_connection = True
        conn = self.connection
        try:
            while self.dev.down():
                rd, _, _ = select.select([conn], [], [], 0.25)
                if rd and not conn.recv(65536):
                    return                   # the client gave up
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, LINGER0)
        except OSError:
            pass

    def body_json(self):
        try:
            return json.loads(self.body.decode("utf-8")) if self.body else None
        except (ValueError, UnicodeDecodeError):
            return None

    def send(self, status, payload, ct=None):
        # Headers and body in one send with Nagle off, the way one httpd_resp_send() goes out on the boards.
        if isinstance(payload, (dict, list)):
            body, ct = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"), "application/json"
        elif isinstance(payload, str):
            body, ct = payload.encode("utf-8"), "text/plain; charset=utf-8"
        else:
            body = payload
        head = "%s %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n" % (
            self.protocol_version, status, HTTPStatus(status).phrase, ct, len(body))
        self.wfile.write(head.encode() + body)

    # -- common (INTERFACES.md 1)
    def h_info(self):
        self.send(200, self.dev.info())

    def h_wifi(self):
        d = self.body_json()
        if not isinstance(d, dict):
            return self.send(400, 'need {"ssid","pass"}')
        ssid = d.get("ssid")
        if not isinstance(ssid, str) or not ssid or len(ssid.encode()) > 32:
            return self.send(400, "bad ssid")
        log(self.dev.role, "wifi: asked to join %r (the fake stays on this PC's network)" % ssid)
        self.send(200, {"ok": True, "trying": ssid})

    def h_ota(self):
        self.send(*self.dev.start_ota(self.body_json()))

    def h_reboot(self):
        self.send(200, "rebooting")
        self.dev.restart("GET /reboot")


class CamHandler(Handler):
    cam = None
    ROUTES = dict(Handler.ROUTES, **{"/frame": {"GET": "h_frame"}, "/capture": {"GET": "h_capture"}})

    def h_frame(self):
        self.send(200, self.cam.frame(), "image/jpeg")

    def h_capture(self):
        size = (self.query.get("size") or ["uxga"])[0].lower() or "uxga"
        if size not in SIZES:
            return self.send(400, "size must be uxga or sxga")
        try:
            q = int((self.query.get("q") or ["12"])[0])
        except ValueError:
            return self.send(400, "q must be 4..63")
        if not 4 <= q <= 63:
            return self.send(400, "q must be 4..63")
        body = self.cam.capture(size, q)
        if body is None:
            return self.send(503, "capture failed")
        self.send(200, body, "image/jpeg")


class NodeHandler(Handler):
    node = None
    ROUTES = dict(Handler.ROUTES, **{"/state": {"GET": "h_state"}, "/thermal": {"GET": "h_thermal"},
                                     "/profile": {"PUT": "h_profile"}, "/pump": {"POST": "h_pump"},
                                     "/light": {"POST": "h_light"}, "/sim": {"GET": "h_sim_get", "POST": "h_sim"}})

    def h_state(self):
        self.send(200, self.node.state_json(), "application/json")

    def h_thermal(self):
        b = self.node.thermal_bytes()
        if b is None:
            return self.send(503, "MLX90640 not answering")
        self.send(200, b, "application/octet-stream")

    def h_profile(self):
        d = self.body_json()
        try:
            self.send(200, self.node.set_profile(d))
        except ValueError as e:
            self.send(400, "bad profile: %s" % e)

    def h_pump(self):
        d = self.body_json()
        usage = 'need {"pump":"water|nutr|ph","ml":N} or {"pump":"water|nutr|ph","s":N}'
        if not isinstance(d, dict) or d.get("pump") not in FLOW:
            return self.send(400, usage)
        ml, s = d.get("ml"), d.get("s")
        if ml is not None and not (isnum(ml) and ml > 0):
            return self.send(400, "ml must be a number > 0")
        if s is not None and not (isnum(s) and s > 0):
            return self.send(400, "s must be a number > 0")
        if ml is None and s is None:
            return self.send(400, usage)
        self.send(*self.node.manual_pump(d["pump"], ml, s))

    def h_light(self):
        d = self.body_json()
        if not isinstance(d, dict) or d.get("mode") not in ("capture", "schedule", "on", "off"):
            return self.send(400, 'need {"mode":"capture"|"schedule"|"on"|"off"}')
        self.send(200, self.node.set_light(d["mode"]))

    def h_sim(self):
        err, off = self.node.sim_set(self.body_json())
        if err:
            return self.send(400, err)
        self.send(200, dict(self.node.sim_get(), ok=True))
        for role, secs in off.items():       # after the reply, so the caller sees it
            DEVICES[role].go_offline(secs)

    def h_sim_get(self):
        self.send(200, self.node.sim_get())


# ---------------------------------------------------------------- UDP announcements and the main loop

def announce_loop(devs):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        u.bind((Cfg.lan, 0))
    except OSError as e:
        log("udp", "cannot bind %s (%s): sending from any address" % (Cfg.lan, e))
    dest = ("127.0.0.1", 4210) if Cfg.lan.startswith("127.") else (Cfg.lan.rsplit(".", 1)[0] + ".255", 4210)
    log("udp", "announcing to %s:%d every %d s" % (dest[0], dest[1], ANNOUNCE_S))
    last = {d.role: 0.0 for d in devs}
    away = {d.role: False for d in devs}
    t_stats, err_logged = time.time(), False
    while True:
        now = time.time()
        for d in devs:
            with d.lock:
                down = d.down(now)
                if away[d.role] and not down:
                    log(d.role, d.back_msg or "back")
                    d.announce_now = True
                away[d.role] = down
                if d.pending_until and now >= d.pending_until:
                    d.pending_until = 0.0
                    log(d.role, "%s confirmed (self-check passed)" % d.ver)
                due = d.announce_now or now - last[d.role] >= ANNOUNCE_S
                pkt = None
                if due and not down and not d.ota_busy:
                    pkt = json.dumps({"role": d.role, "name": d.name, "ver": d.ver, "port": d.port}).encode()
                    d.announce_now = False
                    last[d.role] = now
            if pkt:
                try:
                    u.sendto(pkt, dest)
                    err_logged = False
                except OSError as e:
                    if not err_logged:
                        err_logged = True
                        log("udp", "announcement failed: %s" % e)
        if now - t_stats >= 30:
            parts = []
            for d in devs:
                with d.lock:
                    c, d.served = d.served, collections.Counter()
                if c:
                    parts.append("%s %s" % (d.role, " ".join("%s %d (%.1f/s)" % (p, n, n / (now - t_stats))
                                                             for p, n in sorted(c.items()))))
            if parts:
                log("stats", "last %d s: %s" % (now - t_stats, " | ".join(parts)))
            t_stats = now
        time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser(description="Fake CAM (:8091) and sensor node (:8092), INTERFACES.md api 1.")
    ap.add_argument("lan_ip", nargs="?", default=Cfg.lan, help="this PC's LAN address (picks the broadcast address)")
    ap.add_argument("--http10", action="store_true", help="close every connection")
    ap.add_argument("--fast", action="store_true", help="one simulated day = 60 s for the schedules")
    ap.add_argument("--photos", metavar="DIR", help="cycle /capture through the .jpg/.png files in DIR")
    ap.add_argument("--capture-fail", type=int, default=0, metavar="N", help="every Nth /capture answers 503")
    a = ap.parse_args()
    Cfg.lan = a.lan_ip
    Cfg.proto = "HTTP/1.0" if a.http10 else "HTTP/1.1"
    Cfg.fast = a.fast
    Cfg.day_s = 60 if a.fast else 86400
    Cfg.drift = 20 if a.fast else 1
    Cfg.photos = a.photos
    Cfg.capture_fail = max(0, a.capture_fail)
    if a.photos and not os.path.isdir(a.photos):
        ap.error("--photos: %s is not a folder" % a.photos)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # Korean names, also when redirected
    except (AttributeError, ValueError):
        pass

    cam_dev, node_dev = Device("cam", 8091, 154000), Device("node", 8092, 186000)
    DEVICES.update(cam=cam_dev, node=node_dev)
    Handler.protocol_version = Cfg.proto
    CamHandler.dev, CamHandler.cam = cam_dev, Cam(cam_dev)
    NodeHandler.dev, NodeHandler.node = node_dev, Node(node_dev)
    for port, h in ((8091, CamHandler), (8092, NodeHandler)):
        threading.Thread(target=Server(("0.0.0.0", port), h).serve_forever, daemon=True).start()
    threading.Thread(target=NodeHandler.node.run, daemon=True).start()
    log("main", "fake CAM :8091, fake sensor node :8092 (%s%s)%s%s%s" % (
        Cfg.proto, ", keep-alive" if not a.http10 else ", closes every connection",
        ", fast: 1 day = 60 s" if a.fast else "", ", photos from " + a.photos if a.photos else "",
        ", captures #%d, #%d, ... fail" % (Cfg.capture_fail, 2 * Cfg.capture_fail) if Cfg.capture_fail else ""))
    try:
        announce_loop([cam_dev, node_dev])
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
