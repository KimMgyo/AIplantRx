"""Flash the CAM through the CAM-MB's CH340, or watch its console.

    python tools/ch340_flash.py COM27              build (pio run -e cam) and write everything at 115200
    python tools/ch340_flash.py COM27 --monitor    reset the CAM and print its console (Ctrl+C to stop)

This CH340 (clone chip, WCH driver 3.9.2024.9) fails SetCommState with error 31 whenever the baud equals
the one already set, and pyserial re-sends the whole port setup on every timeout change, so esptool and
pio upload die while opening the port. The chip itself works (reset, receive, upload). The patch below
treats "same baud again" as done, and on the first setup after open detours through another baud.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
PIO = os.path.join(os.path.expanduser("~"), ".platformio", "packages")
sys.path.insert(0, os.path.join(PIO, "tool-esptoolpy"))

import serial  # noqa: E402
import serial.win32 as w  # noqa: E402

_orig = w.SetCommState
_last = {}


def _set_comm_state(h, pdcb):
    dcb = pdcb._obj
    if _orig(h, pdcb):
        _last[getattr(h, "value", h)] = dcb.BaudRate
        return 1
    key = getattr(h, "value", h)
    want = dcb.BaudRate
    if _last.get(key) == want:
        return 1   # only timeouts changed; the chip already runs this
    if key not in _last:
        dcb.BaudRate = 9600 if want != 9600 else 19200
        ok = _orig(h, pdcb)
        dcb.BaudRate = want
        if ok and _orig(h, pdcb):
            _last[key] = want
            return 1
        _last[key] = want
        return 1
    return 0


if sys.platform == "win32":
    w.SetCommState = _set_comm_state


def monitor(port):
    sp = serial.Serial()
    sp.port, sp.baudrate, sp.timeout = port, 115200, 0.2
    sp.dtr = sp.rts = False
    sp.open()
    sp.rts = True   # the CAM-MB wires RTS to EN
    time.sleep(0.1)
    sp.rts = False
    try:
        while True:
            b = sp.read(4000)
            if b:
                print(b.decode("utf-8", "replace"), end="", flush=True)
    except KeyboardInterrupt:
        pass
    sp.close()


def flash(port):
    subprocess.run(["pio", "run", "-e", "cam"], cwd=FW, check=True)
    b = os.path.join(FW, ".pio", "build", "cam")
    boot_app0 = os.path.join(PIO, "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
    import esptool
    esptool.main(["--chip", "esp32", "--port", port, "--baud", "115200",
                  "--before", "default_reset", "--after", "hard_reset",
                  "write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
                  "0x1000", os.path.join(b, "bootloader.bin"),
                  "0x8000", os.path.join(b, "partitions.bin"),
                  "0xe000", boot_app0,
                  "0x10000", os.path.join(b, "firmware.bin")])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if "--monitor" in sys.argv[2:]:
        monitor(sys.argv[1])
    else:
        flash(sys.argv[1])
