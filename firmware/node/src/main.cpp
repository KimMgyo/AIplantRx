// AIplantRx sensor node: ESP32 devkit (HG-ESP32-DEVKIT V4, CP2102N) in the grow box (ARCHITECTURE.md 2.3).
//
// This version runs without its sensors (SCD41, MLX90640, SEN0604 come later):
//   - the common module (firmware/common): Wi-Fi, UDP announce, /info, /wifi, /ota from GitHub, espota,
//     rollback;
//   - INTERFACES.md 2 with every sensor reported missing: /state, /thermal (503), PUT /profile (clamped,
//     kept in NVS), POST /pump and POST /light within the safety limits (control.cpp);
//   - the ESP32-CAM's wired lifeline: an esptool bridge on TCP 4000 and the CAM's console (cambridge.cpp).
//
// USB console (115200): wifi <ssid> <pass> | info | reboot | state | camlog | camreset | camwifi |
//                       camswap on|off (UART pins the other way round) | cam <line> (to the CAM's console)
#include <Arduino.h>
#include <aipnode.h>
#include "cambridge.h"
#include "control.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

static bool before_ota() {
    control_safe();
    return !cam_bridge_active();   // not while the CAM is being written
}

static void ota_failed() { control_resume(); }

static void info_extra(JsonObject o) { cam_info(o); }

static void console_line(char *line) {
    if (!strcmp(line, "state")) control_print(Serial);
    else if (!strcmp(line, "camlog")) cam_print_log(Serial);
    else if (!strcmp(line, "camreset")) cam_restart();
    else if (!strcmp(line, "camswap on")) cam_set_swap(true);
    else if (!strcmp(line, "camswap off")) cam_set_swap(false);
    else if (!strcmp(line, "camwifi")) Serial.println(cam_send_wifi() ? "sent" : "no saved network (or bridge running)");
    else if (!strncmp(line, "cam ", 4)) Serial.println(cam_send_line(line + 4) ? "sent" : "bridge running");
    else if (!aipnode_command(line, Serial))
        Serial.println("[devkit] commands: wifi <ssid> <pass> | info | reboot | state | camlog | camreset | camwifi | camswap on|off | cam <line>");
}

void setup() {
    Serial.begin(115200);
    cam_begin();   // first: the CAM's IO0/EN lines go to their released state
    Serial.printf("\n[devkit] AIplantRx sensor node %s\n", FW_VERSION);

    AipNodeConfig cfg;
    cfg.role = "node";
    cfg.name = "node";
    cfg.version = FW_VERSION;
    cfg.before_ota = before_ota;
    cfg.ota_failed = ota_failed;
    cfg.info_extra = info_extra;
    aipnode_begin(cfg);
    control_begin();
    control_register(aipnode_http());
    cam_register(aipnode_http());
}

void loop() {
    static char line[200];
    static size_t n = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[n] = 0;
            n = 0;
            if (line[0]) console_line(line);
        } else if (n < sizeof line - 1) {
            line[n++] = c;
        }
    }
    cam_poll();
    control_poll();
    delay(5);
}
