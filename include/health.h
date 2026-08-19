// Why the board last restarted, how often it has crashed, and whether this image is still on
// probation. See src/health.cpp for why this exists: with the panel's display on UART0's pins
// there is often no console, and "quiet" and "panicking every two seconds" look the same from
// the outside.
#pragma once
#include <stdint.h>

// Reads the reset reason, bumps the persistent crash counter if the last restart was a crash,
// and notes whether the bootloader is waiting for this image to prove itself. Call first in
// setup(), before anything that might itself crash.
void health_init(void);

// Confirms a probationary image once it has shown it works - WiFi associated, or ten minutes
// without a crash. Until then the bootloader will revert to the previous slot on the next
// restart, which is what saves a board whose only update path is the network. Call from the
// main loop; it does nothing after the first success.
void health_tick(void);

// Called from the main loop. Prints internal-DRAM and PSRAM headroom on the same cadence as the
// other modules' debug ticks, and at boot reports an allocation that failed before the last
// reset. See health.cpp: the crash this exists for names the WiFi driver, not the heap.
void health_debug_tick(void);

// Called once the display is drawing. Half of what ends probation - see health.cpp for why
// a dead screen and a dead router are not the same failure.
void health_ui_ready(void);

// Restarts the board on purpose, and says so on the way out. Use this instead of esp_restart()
// anywhere the firmware reboots itself: it records the intent in NVS first, so the next boot
// knows the restart was ours and does not report it as a crash. See health.cpp for why an
// intentional restart on this platform sometimes arrives looking exactly like one.
void health_restart(const char *why);

// The panic handler's own record of the last crash, read out of the coredump partition at boot:
// the task that died, the exception PC, and a backtrace, as strings. Empty when there is no
// dump. See health.cpp - these are what "reset=panic" could never say.
bool health_have_crash(void);
const char *health_crash_task(void);
const char *health_crash_pc(void);
const char *health_crash_bt(void);

// Call after telemetry carrying the crash has actually reached the server. Erases the dump.
void health_crash_reported(void);

// "power", "panic", "task_wdt", "brownout", ... Short names rather than the enum's numbers,
// because this goes on the wire for a human to read.
const char *health_reset_name(void);

// Crashes since the device was first flashed, across power cycles. A number that climbs while
// telemetry keeps arriving is a board in a crash loop that still manages to report - which is
// exactly the state that was invisible before this existed.
uint32_t health_crashes(void);

// Whether this firmware is still unconfirmed. Worth showing on the panel: a reboot in this
// state does not keep the image.
bool health_image_pending(void);

// Where this image is running from, and where a failed probation would send the board back to.
//
// Both were already computed at init and thrown away - the boot log printed them and nothing
// else could ask. The firmware page needs them for the question a grower has before pressing
// 업데이트: an image that has not proved itself yet is one reboot from being replaced, and a
// board whose revert address is 0 has nothing to fall back to at all. That last case is not
// hypothetical - it is every first boot and every board whose NVS was erased.
//
// health_revert_addr() is only meaningful while health_image_pending() is true; a confirmed
// image has no revert target this module tracks, because "good" then names the running image
// itself. Callers must gate on the pending flag rather than on a non-zero address.
uint32_t health_slot_addr(void);
uint32_t health_revert_addr(void);
