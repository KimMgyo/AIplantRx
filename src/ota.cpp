// Firmware updates over WiFi.
//
// WHY THIS EXISTS, and it is not convenience. The finished device needs the OLED on this
// board - the panel is where a grower stands, and the sensor node will be somewhere down
// the greenhouse. The only pins physically exposed for a second I2C bus are the ones
// behind the UART2 connector, and a switch on the board decides whether those go to the
// USB-serial bridge or to that connector. Handing them to I2C therefore costs the serial
// console AND esptool: verified by flipping the switch, at which point the board answered
// neither logging nor uploads.
//
// That trade is only acceptable if firmware can arrive another way. So this comes first,
// and the pin handover comes after it is proven - the other order ends with a board that
// cannot be reprogrammed.
//
// The escape hatch stays: flipping the switch back to UART1 restores esptool, so a bad OTA
// image is recoverable with a screwdriver rather than a soldering iron. That is the whole
// reason this is safe to do.
//
// The partition table already had room - default_16MB.csv ships otadata plus app0 and app1
// at 6.5MB each, and this firmware is 2.4MB. Nothing about the flash layout changes.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include "ota.h"
#include "health.h"
#include "updatemode.h"
#include "net.h"
#include "hlog.h"

static bool s_started = false;
static volatile int s_progress = -1;   // 0..100 while an update is running, else -1

// Its own task, not loop().
//
// espota opens with a UDP handshake and then a TCP transfer, and both have timeouts of a
// few seconds. This board's loop() calls plantrx_poll(), which blocks on an HTTP round
// trip, and then delay(1000) - so ArduinoOTA.handle() would be reached about once a
// second and the handshake would be a coin flip. A task polling at 50ms costs nothing and
// makes the upload deterministic.
static void ota_task(void *arg) {
    for (;;) {
        if (!s_started) {
            if (WiFi.status() == WL_CONNECTED) {
                // Hostname, so `pio run -t upload --upload-port smartfarm-panel.local`
                // works without hunting for the address on a DHCP lease.
                ArduinoOTA.setHostname("smartfarm-panel");
                // The reboot is ours; see the handle() call below for why.
                ArduinoOTA.setRebootOnSuccess(false);
                ArduinoOTA.onStart([]() {
                    s_progress = 0;
                    // An upload arriving is itself a request for a quiet board, so the mode is
                    // entered here as well as from the button and the server. Nothing has to be
                    // pressed first for a plain `pio run -t upload` to get the whole chip.
                    updatemode_enter("업로드 감지");
                });
                ArduinoOTA.onEnd([]() { s_progress = 100; });
                ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
                    s_progress = total ? (int)((done * 100) / total) : 0;
                });
                ArduinoOTA.onError([](ota_error_t) { s_progress = -1; });
                ArduinoOTA.begin();
                s_started = true;
                hlogf("[ota] listening as smartfarm-panel at %s\n",
                              WiFi.localIP().toString().c_str());
            }
        } else if (WiFi.status() != WL_CONNECTED) {
            // The link went away. ArduinoOTA's listener dies with it, so re-arm on the
            // next association rather than sitting on a socket that cannot answer.
            s_started = false;
            hlogf("[ota] link lost; will re-arm when WiFi returns" "\n");
        } else {
            ArduinoOTA.handle();

            // The reboot is ours, not ArduinoOTA's (setRebootOnSuccess(false) below).
            //
            // Two reasons. It has to go through health_restart() so the next boot knows the
            // restart was deliberate - esp_restart() on this board sometimes arrives as a
            // panic, and the crash counter must not count our own reboots (see health.cpp).
            // And a moment's pause lets the panel finish painting 100%, which is the last
            // thing a person standing in front of it sees before the screen goes dark.
            if (s_progress >= 100) {
                delay(400);
                health_restart("OTA");
            }
        }
        // 50ms while idle, 2ms during a transfer.
        //
        // A flat 50ms polled the socket twenty times a second, and a 2.5MB image does not
        // fit through that: the first attempt stalled at 24% and the sender timed out.
        // handle() moves one chunk per call, so the poll rate IS the throughput - and this
        // board is busy, with camnet decoding JPEG at 20fps on one core and LVGL painting
        // on the other. Idle polling stays lazy because an armed listener with nothing to
        // do should not be spinning.
        // Two ticks, not one: vTaskDelay(1) wakes at the NEXT tick boundary, which averages
        // half a millisecond and can be almost nothing, and handle() holds the CPU for tens of
        // milliseconds per chunk while it erases flash. Two ticks is a guaranteed full tick of
        // slack for the core-0 idle task the watchdog watches. The throughput that costs is
        // repaid many times over by camnet standing down (see camnet.cpp).
        vTaskDelay(pdMS_TO_TICKS(s_progress >= 0 ? 2 : 50));
    }
}

void ota_init(void) {
    // 6KB of stack: ArduinoOTA's update path calls into the flash driver and mbedTLS's
    // MD5, and 4KB was not enough for it on this core.
    // Priority 5, above the camera puller and the LVGL tick. An update that loses the
    // race against a video decode does not fail slowly, it fails at 24% - and while it is
    // running, nothing else on this board matters.
    //
    // Core 0, away from LVGL: the display keeps painting during an update, which is what
    // lets the panel show progress instead of looking crashed.
    xTaskCreatePinnedToCore(ota_task, "ota", 6144, nullptr, 5, nullptr, 0);
}

bool ota_active(void) { return s_progress >= 0; }
int ota_progress(void) { return s_progress; }
