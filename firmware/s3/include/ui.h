// Screens: main and settings (UI.md 2, 4).
#pragma once
#include <stdint.h>

void ui_init();                 // under the LVGL lock
void ui_set_theme(uint8_t t);   // rebuilds everything
void ui_go_main();              // under the LVGL lock
void ui_go_settings(int page);  // under the LVGL lock

// Firmware update progress over whatever screen is up (fwupdate.cpp). Under the LVGL lock.
void ui_update_overlay(const char *ver, int pct, const char *msg);
void ui_update_overlay_close();
// The main screen is up, no sheet is open and nobody has touched the panel for ms. Under the LVGL lock.
bool ui_main_idle(uint32_t ms);
