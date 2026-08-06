// Firmware updates over WiFi. See src/ota.cpp for why this is load-bearing rather than a
// convenience: the display this product needs takes the pins the serial console uses.
//
// Upload with:
//   pio run -e esp32-s3-touch-lcd-7 -t upload --upload-port smartfarm-panel.local
// or with the IP, if mDNS is not reachable from the build machine.
#pragma once

// Starts a task that arms ArduinoOTA once WiFi associates, and re-arms it if the link
// drops. Call once in setup(), after net_init(); it does not wait for a connection.
void ota_init(void);

// Whether an update is in flight, and how far along. -1 when idle or after a failure.
// For a progress indicator on the panel: an update takes long enough that a frozen UI
// with no explanation looks like a crash.
bool ota_active(void);
int ota_progress(void);
