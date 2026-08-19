#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "ui.h"
#include "net.h"
#include "health.h"
#include "fwpull.h"
#include "updatemode.h"
#include "ota.h"
#include "paneloled.h"
#include "camnet.h"
#include "camprov.h"
#include "plantid.h"
#include "sensornode.h"
#include "thermal.h"
#include "reading.h"
#include "aijudge.h"
#include "plantrx.h"
#include "sitecfg.h"
#include "hlog.h"
#include "nodelog.h"
#include "nodeota.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

Board *board = nullptr;

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Before the first line worth reading. The ring it allocates is what makes the boot
    // sequence recoverable from elsewhere, and a client that connects an hour later still sees
    // how this board came up - so anything printed before this call is lost to the network.
    hlog_init();

    hlogf("=== ESP32-S3-Touch-LCD-7 Smart Farm UI ===\n");

    // First, before anything that can itself crash: it reads why the LAST run ended, and it
    // is also what keeps the bootloader from accepting this image until it has proved itself.
    health_init();
    // Right after health, before anything that could want a server address. Pure
    // NVS, no hardware, and both the uplink and the firmware pull read it - see
    // sitecfg.h for why the address and the secret are no longer only in the
    // header they used to be compiled in from.
    sitecfg_init();

    board = new Board();
    board->init();

    // Settle before begin(), and this one is measured. See the note in platformio.ini for
    // the mechanism: board->begin() drives a cross-core IPC call, the ipc1 task it runs on
    // has a 1024-byte stack, and an interrupt taken at the deepest point of that callback
    // overflows it. Boot is when interrupts are densest - the timer, the cache and the
    // board init above all fire here - so opening that window immediately is what loses the
    // race.
    //
    // Five resets with this line: 5/5 reached the UI, 0 panics, 3.1s each.
    // Five resets without it:    1/5 reached the UI cleanly, up to 6 panics, one never
    //                            got there at all.
    //
    // The negative control is the load-bearing half of that, and it took two attempts: the
    // first was run with a sed that silently failed on CRLF line endings, so it measured
    // the same binary twice and "proved" the delay did nothing. Anything touching this must
    // be checked the same way - the fault appears 2-4 times in 5, so one clean boot is not
    // evidence of anything.
    delay(300);
#if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif

    if (!board->begin()) {
        hlogf("Board begin failed!" "\n");
        return;
    }
    hlogf("Board initialized" "\n");

    // Keep the panel dark until the UI is actually painted. On boot the RGB
    // framebuffer / LVGL's default screen would otherwise flash white for a
    // frame before ui_init() draws the dark UI. Backlight comes back on below.
    auto *backlight = board->getBacklight();
    if (backlight) backlight->off();

    if (!lvgl_port_init(board->getLCD(), board->getTouch())) {
        hlogf("LVGL port init failed!" "\n");
        return;
    }
    hlogf("LVGL initialized" "\n");

    net_init();      // WiFi autoconnect + NTP; UI polls its state
    // Right after net_init and before anything slow: the task it starts only arms once
    // WiFi associates, so getting it queued early means an update can land during the
    // rest of bring-up rather than only after the UI is painted.
    ota_init();
    // Beside ota_init for the same reason: both are how new firmware arrives, and neither does
    // anything until asked. The pull is the half that does not need a laptop on the network -
    // see fwpull.cpp - so it has to be running before anybody presses the button that uses it.
    fwpull_init();
    // Before camprov_init(), not after: its recv callback is what calls nodelog_add(), and a
    // node that is already broadcasting when this board comes up gets its first line in before
    // the rest of bring-up finishes. The ring is static so this only has to happen before the
    // first write, but "before the thing that writes to it" is the ordering that stays true when
    // the allocation inside it grows.
    nodelog_init();
    thermal_init();  // MLX90640 frame sink; before camprov_init, which feeds it
    camprov_init();  // ESP-NOW responder: hands WiFi creds to the CAM on request
    // After camprov_init(), which owns the recv callback that feeds it. Ordering here is
    // tidiness rather than a constraint - nodeota_init() deliberately does not clear its state
    // table, so a node already broadcasting when this board comes up keeps the report that
    // arrived in between - but "after the thing that delivers to it" is the order that stays
    // true if it ever grows something that does need clearing.
    nodeota_init();   // remote firmware updates for the CAM and the sensor node
    camnet_init();   // the camera feed: ESP32-CAM MJPEG over WiFi
    // Species ID, button-triggered. The keys are the server's now, so its worker POSTs the
    // CAM still to the address plantrx_init() parses two lines down - which is not an ordering
    // bug, because that worker is parked on a semaphore until the button fires.
    plantid_init();
    aijudge_init();     // judgment log: needs thermal/camnet up so it can peek frames
    plantrx_init();     // the uplink: after the ring it appends to and the tap it uploads from

    // The rear I2C header, on the same port 0 the touch controller and the expander
    // are already on. After board->begin(), because that is what installs the driver
    // this shares - probing before it returns ESP_ERR_INVALID_STATE for every address
    // and reads as "nothing is plugged in".
    // NO OLED HERE, and it is not an omission. Both routes off this board are shut:
    //
    //  - The rear I2C header is port 0, shared with the GT911 and the CH422G expander,
    //    and the CH422G ACKs 8-bit 0x40-0x4F and 0x60-0x7F. An SSD1306's 0x78 sits
    //    inside the second group, so a write to it is received by the expander as
    //    well - measured with the module physically unplugged, so it is the board and
    //    not a phantom. The expander drives touch reset and the USB mux; writing
    //    display frames into it cost this session a dead serial port.
    //  - A private bus needs two free GPIOs. Only 6, 11, 12, 13, 15, 16 and (probably)
    //    19, 20 are free here; the UART2 connector's pins are none of them, checked by
    //    sweeping all 56 ordered pairs with the display wired to it. And the switch that
    //    routes the USB bridge to UART2 takes the console AND esptool with it, so that
    //    connector cannot be developed against anyway.
    //
    // The display lives on the sensor node instead: 0x3C is free on its bus, its
    // SDA/SCL are on devkit headers, and it measures the temperature itself rather
    // than receiving it over ESP-NOW. See sensor_node/src/oled.cpp.

    // Before ui_init(), not after: ui_build() reads the restored globals to lay
    // out the first frame, and a restore landing later would paint the factory
    // defaults and then jump. NVS is already usable here - net_init() at line 57
    // opened its own namespace back at net.cpp:127 and connected with the SSID it
    // read out of it. Outside the LVGL lock below because this is a flash read
    // and no widget exists yet to race with it.
    ui_prefs_load();

    lvgl_port_lock(-1);
    ui_init();
    lvgl_port_unlock();
    delay(200);                       // let the LVGL task flush the finished UI
    if (backlight) backlight->on();   // reveal — no white flash

#if PANEL_OLED
    // After the UI, so the panel is painted before the console is risked, and after
    // net_init/ota_init so a board with no display still has a way in.
    if (paneloled_init()) {
        Serial.end();   // found: these pads are I2C now
    }
#endif

    health_ui_ready();
    hlogf("UI ready" "\n");

}

void loop() {
    sensornode_debug_tick();
    thermal_debug_tick();
    camnet_debug_tick();
    camprov_debug_tick();
    plantrx_debug_tick();
    health_debug_tick();
    nodeota_debug_tick();
    // The append writes records the LVGL thread reads (and thumbnail buffers its
    // canvases point straight at), so it runs under the same lock ui_init() uses.
    // Skipped in update mode: it walks the judgment log and repaints canvases, and an update
    // gets the board rather than sharing it. See updatemode.cpp for who else stands down.
    if (!updatemode_active()) {
        lvgl_port_lock(-1);
        aijudge_tick();
        lvgl_port_unlock();
    }
    // Blocks for an HTTP round trip, which is why it runs here rather than on an
    // LVGL timer: the UI task keeps drawing while this waits on the socket.
    health_tick();
    updatemode_tick();
    // Ahead of nodelog_tick() and outside the update-mode guard, like it. No I/O and no
    // allocation: it ages the online flags, resends a command that was never acknowledged, and
    // turns a node that stopped reporting mid-download into a readable failure instead of a
    // frozen bar. The 700ms retry window is only sampled once per loop(), so the resends land
    // about a second apart in practice - three attempts is the point, not their spacing.
    nodeota_tick();
    // Beside the other ticks and outside the update-mode guard below, because it carries its own:
    // it forwards node log lines over one short POST and stands down in update mode for the same
    // reason plantrx_poll() does. On a panel with no server configured it returns immediately.
    nodelog_tick();
    // The poll blocks for a whole HTTP round trip. That is exactly the kind of company an
    // update does not need, so in update mode it does not happen at all.
    if (!updatemode_active()) plantrx_poll();
#if PANEL_OLED
    paneloled_tick();
#endif
    delay(1000);
}
