// The ESP32-CAM's wired lifeline through the devkit (ARCHITECTURE.md 2.3).
//
// Wiring: devkit UART1 TX 18 -> 1k -> CAM U0R, CAM U0T -> 1k -> devkit RX 19; devkit 25 and 33 each light
// a PC817C whose transistor pulls CAM IO0 / CAM EN to GND (25 or 33 HIGH = that CAM pin LOW).
//
//   TCP 4000   esptool bridge. A connection puts the CAM in its ROM loader (IO0 low through an EN reset)
//              and passes bytes both ways at 115200; when the connection closes (or 30 s of silence) IO0
//              is released and the CAM restarts into its firmware. Use it as esptool's port:
//                  --port socket://<devkit>:4000 --before no_reset --after no_reset --baud 115200
//              (pio run -e cam-wired -t upload in firmware/cam). 115200 only: the bridge does not follow
//              esptool's baud change.
//   GET  /cam/log     the CAM's recent console output (text, last 4 KB)
//   POST /cam/reset   restart the CAM into its firmware
//   POST /cam/send    body = one console line for the CAM ("info", "reboot", ...)
//   POST /cam/wifi    gives the CAM this devkit's saved network ("wifi <ssid> <pass>" on its console)
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>

void cam_begin();
void cam_register(httpd_handle_t h);
void cam_poll();                 // from loop(): moves the CAM's console output into the log
bool cam_bridge_active();
// RX/TX the other way round, for a board wired 18 <- U0T, 19 -> U0R (kept in NVS).
void cam_set_swap(bool on);
void cam_restart();              // normal boot (no-op while the bridge runs)
bool cam_send_line(const char *line);
bool cam_send_wifi();            // false when this devkit has no saved network or the bridge runs
void cam_print_log(Print &out);
void cam_info(JsonObject o);
