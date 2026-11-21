// Screens: main and settings (UI.md 2, 4).
#pragma once
#include <stdint.h>

void ui_init();                 // under the LVGL lock
void ui_set_theme(uint8_t t);   // rebuilds everything
void ui_go_main();              // under the LVGL lock
void ui_go_settings(int page);  // under the LVGL lock
