#pragma once

#include <stdint.h>

// Restores the panel's user state - theme, the seven device switches, fan speed,
// the AI-RX mode - from NVS. MUST run before ui_init(): ui_build() reads those globals
// to lay out the first frame, so a restore landing afterwards paints the factory
// defaults and then visibly jumps to what the user actually left behind.
void ui_prefs_load(void);

void ui_init(void);

// The AI-RX mode switch, for the uplink's telemetry. The switch's state is a UI
// global, and this exists so plantrx.cpp does not have to include a header that
// exports every widget pointer in the project to read one bool.
bool ui_auto_control(void);

// The panel's actuator inventory: the seven switches on the 제어 page plus the
// fan's percentage, which is what the *user* has asked for - not a reading off
// a relay, because this board has none. The uplink sends it as
// `actuator_intent` so the server's prompt can name the devices that actually
// exist here instead of the model inventing its own, and so the window summary
// can reason about how long each one has been asked to run. Indexed rather
// than seven getters, so plantrx.cpp carries no device names of its own and an
// eighth device is one table row. Same reasoning as ui_auto_control() above:
// plantrx.cpp must not include a header that exports every widget pointer.
int         ui_actuator_count(void);   // 7
const char *ui_actuator_name(int i);   // "fan" .. "led"; "" when i is out of range
int         ui_actuator_level(int i);  // 0..100; 0 when i is out of range

// What actuator_intent alone cannot say. It is a snapshot, taken once per poll,
// so a switch that went on and off between two polls reads as a switch nobody
// touched - and the server then scores that window as though its prescription had
// been left alone. These two are monotonic counts since boot: the server diffs
// consecutive polls to get the movement in between.
//
// 전체 정지 gets its own count because it is the one control that means the
// grower disagreed with the prescription, and it was the most invisible of all -
// taken back inside its undo window it left the switches exactly as the server
// last saw them. Reversing it does not decrement: the press happened.
//
// Both restart at zero across a reboot, which uptime_ms on the same telemetry
// already accounts for.
uint32_t ui_switch_edges(void);    // switch transitions since boot
uint32_t ui_allstop_count(void);   // 전체 정지 presses since boot
