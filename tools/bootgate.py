# Resets the panel ten times and reports whether each boot reached a drawn UI.
#
# WHY TEN AND WHY THIS EXISTS. This board has twice shipped a firmware that booted fine on the
# bench and then failed one boot in five in the greenhouse - both times a race in early startup
# (a 1KB ipc1 stack behind board->begin(), and a task that starved the watchdog's idle task).
# A single successful boot proves nothing about either: the failures were 20-60% and the only
# thing that separated a real fix from a lucky one was repetition. Ten resets is the smallest
# number that reliably showed the 20% case, and it is cheap - about three minutes.
#
# Use it as the acceptance test for any change to setup(), a task's priority or stack, or the
# board bringup. "It booted once" is not a result.
#
#   python tools/bootgate.py [COM5] [runs]
#
# Reads the reset reason the firmware itself reports, because that is the fact that matters: a
# board that says reset=panic on boot 4 crashed on boot 3, whatever boot 4 then does.
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 10
WINDOW_S = 14  # UI is up by ~3.1s on a healthy build; the rest is room for a retry

bad = 0
for run in range(RUNS):
    with serial.Serial(PORT, 115200, timeout=1) as s:
        # DTR low / RTS pulsed is the reset the USB bridge exposes; esptool uses the same pair.
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)

        t0 = time.time()
        panics = 0
        reason = "?"
        ready = None
        while time.time() - t0 < WINDOW_S:
            line = s.readline().decode("utf8", "replace")
            if "Guru Meditation" in line:
                panics += 1
            if "[health] reset=" in line:
                reason = line.split("reset=", 1)[1].split()[0]
            if "UI ready" in line and ready is None:
                ready = time.time() - t0

        ok = ready is not None and panics == 0
        if not ok:
            bad += 1
        print(
            # "panics", not "crashes": the [health] line this parses has its own `crashes`
            # field, which is the NVS lifetime counter, and printing a different number under the
            # same word next to it is how a reader concludes the counter was wiped. It happened.
            "boot %2d: %s  last=%-8s panics_here=%d  UI at %s"
            % (
                run + 1,
                "ok  " if ok else "BAD ",
                reason,
                panics,
                ("%.1fs" % ready) if ready else "NEVER",
            ),
            flush=True,
        )
    time.sleep(1)

print("--- %d/%d clean ---" % (RUNS - bad, RUNS))
sys.exit(1 if bad else 0)
