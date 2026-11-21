"""Talk to the S3 over USB serial: screenshots, taps, info.

    python tools/s3ctl.py shot out.png
    python tools/s3ctl.py tap 410 48 [ms]
    python tools/s3ctl.py info
    python tools/s3ctl.py cmd "swipe 120 400 120 100 400"
"""
import sys
import time

import serial

PORT = "COM26"


def open_port(baud):
    s = serial.Serial()
    s.port = PORT
    s.baudrate = baud
    s.timeout = 0.3
    s.dtr = False      # opening must not reset the board (EN is on RTS)
    s.rts = False
    s.open()
    return s


def drain(s, t=0.3):
    end = time.time() + t
    out = b""
    while time.time() < end:
        out += s.read(4096)
    return out


def cmd(line, wait=0.6):
    s = open_port(115200)
    drain(s, 0.1)
    s.write((line + "\n").encode())
    out = drain(s, wait)
    s.close()
    return out.decode("utf-8", "replace")


def shot(path):
    from PIL import Image
    import numpy as np
    s = open_port(115200)
    drain(s, 0.1)
    s.write(b"baud 2000000\n")
    time.sleep(0.3)
    s.baudrate = 2000000
    drain(s, 0.2)
    s.write(b"shot\n")
    head = b""
    t0 = time.time()
    while b"SHOT " not in head or not head.split(b"SHOT ", 1)[1].count(b"\n"):
        head += s.read(64)
        if time.time() - t0 > 5:
            raise SystemExit("no SHOT header: %r" % head[-200:])
    pre, rest = head.split(b"SHOT ", 1)
    n_txt, data = rest.split(b"\n", 1)
    n = int(n_txt)
    s.timeout = 2
    while len(data) < n:
        chunk = s.read(min(65536, n - len(data)))
        if not chunk:
            break
        data += chunk
    s.write(b"baud 115200\n")
    time.sleep(0.2)
    s.close()
    if len(data) < n:
        raise SystemExit("short frame: %d of %d" % (len(data), n))
    px = np.frombuffer(data[:n], dtype="<u2").reshape(480, 800).astype(np.uint32)
    r = ((px >> 11) & 31) * 255 // 31
    g = ((px >> 5) & 63) * 255 // 63
    b = (px & 31) * 255 // 31
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(path)
    print("saved", path, "in %.1fs" % (time.time() - t0))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "shot":
        shot(a[1] if len(a) > 1 else "shot.png")
    elif a[0] == "tap":
        print(cmd("tap " + " ".join(a[1:])))
    elif a[0] == "info":
        print(cmd("info"))
    elif a[0] == "cmd":
        print(cmd(a[1], float(a[2]) if len(a) > 2 else 0.8))

