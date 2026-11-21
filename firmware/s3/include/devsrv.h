// Development HTTP server on port 80 and espota.
//
//   GET /info             JSON: version, IP, RSSI, heap, uptime
//   GET /screen.bmp       what the panel shows right now (800x480, 16-bit)
//   GET /tap?x=&y=[&ms=]  press at x,y (default 120 ms)
//   GET /swipe?x=&y=&x2=&y2=[&ms=]
//   GET /set?llm_url=..&llm_key=..&pn_key=..   (any of them; saved to NVS)
//   GET /set?theme=0..2 | /set?screen=main | /set?screen=settings&page=N
//   GET /perf[?reset=1]   frame timing (perf.h)
//   GET /update[?check=1|?install=1]   firmware update state, check or install (fwupdate.h)
//   GET /reboot
//
// The same over the USB serial port (115200, one command per line), for when there is no Wi-Fi:
//   info | tap x y [ms] | swipe x y x2 y2 [ms] | wifi <ssid>\t<pass> | baud <n> | shot | reboot
//   shot answers "SHOT <bytes>\n" and the raw 800x480 RGB565 frame (use baud 2000000 first).
#pragma once

void devsrv_init();              // starts the task; serves once Wi-Fi is up
void devsrv_serial_init();       // serial commands (works without Wi-Fi)
void devsrv_attach_indev();      // under the LVGL lock: the input device /tap drives
int devsrv_ota_progress();       // -1 idle, else 0..100
