"""Drive the S3 over HTTP through fixed scenarios and print /perf for each.

    python tools/perf_run.py [ip] [label]
Needs tools/fake_nodes.py running for the camera scenarios. Uses /set?theme= to switch themes.
"""
import json
import sys
import time
import urllib.request

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.70"
LABEL = sys.argv[2] if len(sys.argv) > 2 else ""


def get(q, t=8):
    return urllib.request.urlopen("http://%s%s" % (IP, q), timeout=t).read().decode()


def perf_reset():
    get("/perf?reset=1")


def perf():
    return json.loads(get("/perf?reset=1"))


def tap(x, y, ms=120):
    get("/tap?x=%d&y=%d&ms=%d" % (x, y, ms))


def swipe(x, y, x2, y2, ms=400):
    get("/swipe?x=%d&y=%d&x2=%d&y2=%d&ms=%d" % (x, y, x2, y2, ms))


def show(name, p):
    print("%-22s fps %5.1f  render %5.2f ms (max %6.2f, slow %3d)  wait %5.2f  px %6d  lv_busy %4.1f%%  cam %4.1f fps fetch %5.1f dec %4.1f ms  th %4.1f fps"
          % (name, p["fps"], p["render_ms"], p["render_max_ms"], p["slow"], p["wait_ms"], p["px"], p["lv_busy_pct"],
             p["cam_fps"], p["cam_fetch_ms"], p["cam_decode_ms"], p["th_fps"]), flush=True)
    return p


def to_main():
    get("/set?screen=main")
    time.sleep(1.5)


def run_theme(theme):
    out = {}
    get("/set?theme=%d" % theme)
    time.sleep(2)
    to_main()
    perf_reset(); time.sleep(8)
    out["main_live"] = show("t%d main_live" % theme, perf())
    perf_reset()
    tap(430, 64); time.sleep(1.5)
    out["open_settings"] = show("t%d open_settings" % theme, perf())
    perf_reset()
    for _ in range(4):
        swipe(120, 440, 120, 140, 450); time.sleep(0.9)
        swipe(120, 140, 120, 440, 450); time.sleep(0.9)
    out["side_scroll"] = show("t%d side_scroll" % theme, perf())
    swipe(120, 140, 120, 440, 300); time.sleep(1)
    tap(120, 429); time.sleep(1.2)
    perf_reset()
    for _ in range(4):
        swipe(520, 440, 520, 140, 450); time.sleep(0.9)
        swipe(520, 140, 520, 440, 450); time.sleep(0.9)
    out["detail_scroll"] = show("t%d detail_scroll" % theme, perf())
    perf_reset()
    tap(767, 30); time.sleep(2)
    out["close_settings"] = show("t%d close_settings" % theme, perf())
    return out


res = {"label": LABEL, "info": json.loads(get("/info"))}
for t in (1, 0):
    res["theme%d" % t] = run_theme(t)
open("D:/voxl-build/perf_%s.json" % (LABEL or "run"), "w").write(json.dumps(res, indent=1))

