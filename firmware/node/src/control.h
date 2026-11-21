// Profile, pumps and the ring light (INTERFACES.md 2).
//
// This version has no sensors yet: /state reports every sensor as missing, /thermal answers 503, and
// nothing runs by itself (no soil-driven watering, no schedules for water or feed). The pumps and the
// ring run on request (POST /pump, POST /light) within the firmware's safety limits; the light follows
// the profile's schedule once the clock is known.
#pragma once
#include <Arduino.h>
#include <esp_http_server.h>

void control_begin();
void control_register(httpd_handle_t h);
void control_poll();          // from loop(): ends pump runs, capture light, the day's counters
void control_safe();          // everything off and held off (before an OTA download)
void control_resume();        // the download failed: back to normal
void control_print(Print &out);
