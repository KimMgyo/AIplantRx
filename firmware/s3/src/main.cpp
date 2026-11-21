// AIplantRx v2 S3: main screen and settings on the Waveshare ESP32-S3-Touch-LCD-7.
//
// Everything after setup() runs in tasks: LVGL on core 1 (lvgl_v8_port), Wi-Fi, discovery, camera,
// sensor node, LLM and the dev server on core 0. The Arduino loop task is deleted.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "app.h"
#include "devsrv.h"
#include "fwupdate.h"
#include "lvgl_v8_port.h"
#include "net.h"
#include "nodes.h"
#include "perf.h"
#include "settings.h"
#include "ui.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

// Core for the RGB panel interrupt. Its bounce-buffer refill copies the whole framebuffer out of PSRAM
// in the interrupt, 28.6 MB/s at 37 Hz, on the core that called board->begin(). On core 1 that time
// comes out of LVGL's rendering; on core 0 it shares with Wi-Fi and the camera decode.
#ifndef PANEL_CORE
#define PANEL_CORE 1
#endif

static Board *board;

static bool board_begin() {
#if PANEL_CORE == 1
    return board->begin();
#else
    static volatile int res = -1;
    xTaskCreatePinnedToCore([](void *) { res = board->begin() ? 1 : 0; vTaskDelete(nullptr); }, "panel", 6144, nullptr, 5,
                            nullptr, PANEL_CORE);
    while (res < 0) delay(5);
    return res == 1;
#endif
}

void setup() {
    Serial.begin(115200);
    // Boot crash, measured (25 resets in a row once): "Stack canary watchpoint triggered (ipc1)" inside
    // gpio_intr_service. IDF registers the GPIO interrupt service through the 1 KB IPC task, and the
    // touch driver does that from board->begin() right after the GT911 address setup has toggled its INT
    // pin (GPIO4): the pending edge fires the moment the service is enabled, on that tiny stack.
    // Registered here, before any GPIO interrupt is enabled, nothing can be pending; the driver then
    // finds the service installed (ESP_ERR_INVALID_STATE, which it accepts) and skips the IPC.
    gpio_install_isr_service(0);
    Serial.printf("\n=== AIplantRx S3 %s ===\n", FW_VERSION);
    settings_load();

    board = new Board();
    board->init();
    // smartfarm, measured: board->begin() runs a cross-core IPC call on a 1 KB stack, and an interrupt at
    // the wrong moment overflows it. A short settle before begin() took boots from 1/5 to 5/5 clean.
    delay(300);
#if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto bus = lcd->getBus();
    if (bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB)
        static_cast<BusRGB *>(bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
#endif
#endif
    if (!board_begin()) {
        Serial.println("board begin failed");
        return;
    }
    auto *bl = board->getBacklight();
    if (bl) bl->off();                      // dark until the first frame is drawn
    if (!lvgl_port_init(board->getLCD(), board->getTouch())) {
        Serial.println("lvgl init failed");
        return;
    }

    net_init();
    nodes_init();
    lvgl_port_lock(-1);
    ui_init();
    devsrv_attach_indev();
    perf_attach();
    lvgl_port_unlock();
    devsrv_init();
    devsrv_serial_init();
    fwu_init();
    delay(200);
    if (bl) bl->on();
    Serial.printf("[boot] ui ready, heap int=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() { vTaskDelete(nullptr); }
