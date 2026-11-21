// The five OLED panels of UI.md 5 (title line, big number, small info, target bar) with demo values.
#pragma once
#include <Adafruit_SSD1306.h>

// Draws panel `idx` (0..4 = OLED 1..5) into d's buffer; the caller sends it with d->display().
void ui_panel(Adafruit_SSD1306 *d, int idx, uint32_t now);

// The screen shown on every OLED while the C3 relays an S3 upload (nothing refreshes meanwhile).
void ui_upload(Adafruit_SSD1306 *d);
