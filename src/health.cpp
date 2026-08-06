// Why the board last restarted, how often it has crashed, and whether this firmware has
// earned the right to keep running.
//
// WHAT THIS IS FOR. The panel's serial console is not always reachable: the display it needs
// sits on GPIO 43/44, which are UART0's pins, and a switch on the board hands those either to
// the USB bridge or to the header. Running with the display means running blind. Today that
// cost hours - a firmware went out over OTA, boot-looped, and from the outside the only
// symptom was telemetry stopping. "The board is quiet" and "the board is panicking every two
// seconds" looked identical.
//
// So two things live here:
//
//   1. The reset reason and a crash count, reported in telemetry. A panic loop announces
//      itself on the server with no console and no wires.
//   2. Probation: a newly flashed image must prove itself, and one that boot-loops instead
//      is reverted to the slot that last worked.
//
// WHY PROBATION IS DONE HERE AND NOT BY THE BOOTLOADER. IDF has this feature already, and
// the app-side config for it is on (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1), which is why an
// earlier version of this file was a thin arming shim over esp_ota_mark_app_valid_cancel_-
// rollback(). It was measured and it does not work on this platform: a deliberately broken
// image was delivered over OTA, and it reported image=confirmed on arrival and then panicked
// 58 times in a row without ever being reverted. The bootloader is a prebuilt binary shipped
// in the framework package; the flag being set in the APP's sdkconfig says nothing about what
// that binary was compiled with, and it never marks an image PENDING_VERIFY. Building a
// bootloader from source needs the Arduino-as-ESP-IDF-component path, which this project
// already tried and abandoned (see platformio.ini).
//
// So the state lives in NVS instead, and the decision is ours. It needs nothing from the
// bootloader: `good` is the flash address of the last image that proved itself, and any image
// running from a different address is on probation and counting its attempts.
//
// WHAT THIS COVERS AND WHAT IT DOES NOT. An image that boots and dies - the case that cost
// the afternoon - is covered, because each attempt increments a counter that outlives the
// crash. An image so corrupt it cannot start is covered by the bootloader's own image
// verification, which falls back on its own. An image that HANGS before reaching the loop is
// NOT covered: it never crashes, so it boots once, and nothing here runs again to notice.
// Converting hangs into reboots needs the task watchdog holding the loop task, which would
// have to survive plantrx's blocking HTTP and LVGL's rendering, and that is not attempted.
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <Preferences.h>
#include "health.h"
#include "net.h"
#include "hlog.h"

// Override of the core's weak hook (esp32-hal-misc.c:236), which otherwise validates the
// running image from initArduino() before setup() even starts. Kept because it costs one
// symbol and it is the correct answer to the question the core is asking - a decision about
// whether this firmware works cannot be made 1.6s into the boot. It is NOT what makes
// probation work here; that is the NVS counter below.
bool verifyRollbackLater(void) { return true; }

// Boots an unproven image may take before it is given up on. Three because one is a brownout
// or a bad power-up, and the whole run costs about six seconds of panic loop before the
// revert - fast enough that a person watching the wall sees a blink, not an outage.
static const uint8_t MAX_TRIES = 3;

// How long a probationary image gets to put something on the screen. A display that never
// comes up is broken for a wall panel no matter what else works, and unlike a missing router
// it cannot be blamed on the greenhouse. Rebooting lets the attempt counter escalate to a
// revert; without this a bricked-display image would sit there forever on one attempt.
static const uint32_t UI_DEADLINE_MS = 3UL * 60UL * 1000UL;

// How long an image that HAS drawn gets to reach the network before it is accepted anyway. A
// greenhouse with a dead router is a normal Tuesday and downgrading the firmware would not
// fix the router; ten minutes of drawing without a crash is the image demonstrably working.
static const uint32_t NET_DEADLINE_MS = 10UL * 60UL * 1000UL;

static Preferences s_prefs;
static uint32_t s_crashes = 0;       // panics counted across power cycles, from NVS
static esp_reset_reason_t s_reason = ESP_RST_UNKNOWN;
static bool s_pending = false;       // this image has never proved itself
static bool s_validated = false;
static bool s_ui_ready = false;
static uint32_t s_up_start = 0;
static uint32_t s_addr = 0;          // flash address this image is running from
static bool s_intentional = false;   // the last restart was one we asked for

// The last crash, as the panic handler recorded it. Kept as strings because that is how they
// leave the board and how a person reads them; the numbers mean nothing without the ELF, and
// the server has the ELF.
static char s_c_task[16] = "";
static char s_c_pc[12] = "";
static char s_c_bt[112] = "";
static bool s_c_have = false;

// Internal DRAM is the resource this board runs out of first, and it fails somewhere else.
// The WiFi driver's coex layer creates an esp_timer the first time a scan starts; esp_timer_-
// create() turns a failed calloc into ESP_ERR_NO_MEM, ets_timer_setfn wraps that call in an
// ESP_ERROR_CHECK (ets_timer_legacy.c:78), and the board aborts inside the WiFi task. So the
// panic names ets_timer_setfn and coex, and a heap that ran out of internal RAM is reported as
// a radio bug - six crashes here read that way before this existed. The failing allocation is
// recorded in RTC memory, which outlives the reset it is about to cause, so the next boot can
// say what actually ran out.
RTC_NOINIT_ATTR static uint32_t s_af_magic;
RTC_NOINIT_ATTR static uint32_t s_af_size;
RTC_NOINIT_ATTR static uint32_t s_af_caps;
RTC_NOINIT_ATTR static char s_af_fn[16];
static const uint32_t AF_MAGIC = 0xA110FA11;

// Runs in whatever context lost the allocation, which may be an ISR, so it does the smallest
// thing that survives: a few word writes and a ROM print. It deliberately does not query the
// heap - asking the allocator about itself from inside its own failure path is how a
// diagnostic becomes the crash.
static void IRAM_ATTR alloc_failed_cb(size_t size, uint32_t caps, const char *fn) {
    s_af_magic = AF_MAGIC;
    s_af_size = (uint32_t)size;
    s_af_caps = caps;
    unsigned i = 0;
    if (fn) {
        for (; i < sizeof(s_af_fn) - 1 && fn[i]; i++) s_af_fn[i] = fn[i];
    }
    s_af_fn[i] = '\0';
    esp_rom_printf("[heap] ALLOC FAIL %u bytes caps=0x%x in %s\n",
                   (unsigned)size, (unsigned)caps, s_af_fn);
}

// Reverts to the last image that worked and restarts into it. Only ever called with a `good`
// address that a previous boot wrote after proving itself.
static void revert_to(uint32_t good) {
    // The app partition whose address is `good`. Compared by address rather than by label
    // because that is what a previous boot could record without knowing the table.
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    const esp_partition_t *target = nullptr;
    for (; it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t *cand = esp_partition_get(it);
        if (cand != nullptr && cand->address == good) { target = cand; break; }
    }
    if (it != nullptr) esp_partition_iterator_release(it);

    if (target == nullptr) {
        hlogf("[health] cannot revert: no app partition at 0x%06lX\n",
                      (unsigned long)good);
        return;
    }

    // set_boot_partition verifies the image before accepting it, so a `good` slot that has
    // since been overwritten fails here rather than bricking the board on the next boot.
    esp_err_t e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        hlogf("[health] cannot revert to 0x%06lX: %s\n",
                      (unsigned long)good, esp_err_to_name(e));
        return;
    }

    // Cleared before restarting, so the reverted image starts its own count from zero rather
    // than inheriting the dead one's three strikes.
    s_prefs.putUChar("tries", 0);
    hlogf("[health] reverting to 0x%06lX after %u failed boots\n",
                  (unsigned long)good, (unsigned)MAX_TRIES);
    health_restart("revert");
}

// Reads the panic handler's own record of the last crash out of the coredump partition.
//
// WHY THIS IS WORTH THE CODE. "reset=panic" says the board died; it does not say where, and
// on a panel with no console that was the whole problem. The dump the panic handler already
// writes to flash carries the task name, the exception PC, and a backtrace - the three things
// a person actually needs - and reading a summary of it costs one flash read at boot instead
// of a serial cable and a reproduction.
//
// The dump is NOT erased here. It is erased once the server has actually received it (see
// health_crash_reported), because a board that crashes again before its first successful post
// would otherwise throw away the only evidence of the first crash.
static void read_coredump(void) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) return;

    // A dump written while we were deliberately restarting is not a crash report. Tearing the
    // scheduler down inside esp_restart() leaves a stack-canary watchpoint armed over memory
    // being dismantled, and about two in five of our own restarts trip it - naming IDLE1, or
    // ipc1, or whichever task was current, never the same one twice. Reporting that upward
    // would put a crash in a task that did not crash into the record the operator reads, and
    // that record only has value if everything in it is a real crash. Erased rather than kept,
    // because the partition holds one dump and the next one should be a real crash's.
    if (s_intentional) {
        hlogf("[health] coredump %ub from our own restart - discarded\n", (unsigned)size);
        esp_core_dump_image_erase();
        return;
    }

    // Heap, not stack: the summary carries a whole backtrace array and this runs on the main
    // task during setup, next to the panel driver's own allocations.
    esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)malloc(sizeof(*sum));
    if (sum == nullptr) return;

    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        s_c_have = true;
        snprintf(s_c_task, sizeof(s_c_task), "%s", sum->exc_task);
        snprintf(s_c_pc, sizeof(s_c_pc), "0x%08lx", (unsigned long)sum->exc_pc);
        size_t n = 0;
        for (int i = 0; i < (int)sum->exc_bt_info.depth && n + 12 < sizeof(s_c_bt); i++) {
            n += snprintf(s_c_bt + n, sizeof(s_c_bt) - n, "%s0x%08lx",
                          i ? " " : "", (unsigned long)sum->exc_bt_info.bt[i]);
        }
        hlogf("[health] coredump %ub: task=%s pc=%s%s\n  bt=%s\n", (unsigned)size,
                      s_c_task, s_c_pc, sum->exc_bt_info.corrupted ? " CORRUPT" : "", s_c_bt);
    } else {
        hlogf("[health] coredump %ub present but unreadable\n", (unsigned)size);
    }
    free(sum);
#endif
}

void health_init(void) {
    s_reason = esp_reset_reason();
    s_up_start = millis();

    // The crash count has to outlive the crash, so it lives in NVS. Its own namespace: the
    // "net" one holds credentials and a counter has no business sharing a page with them.
    s_prefs.begin("health", false);
    s_crashes = s_prefs.getUInt("crashes", 0);
    // WHY AN INTENTIONAL RESTART IS EXCLUDED, AND WHY IT HAS TO BE.
    //
    // Measured on this board: after an OTA image is fully written and its boot partition is
    // set, esp_restart() lands a cross-core stall interrupt on whichever stack core 1 happens
    // to be using, and that is often IDLE1 - which the framework gives 1024 bytes and which
    // already runs with 352 free. The canary trips and the "restart" arrives as
    // ESP_RST_PANIC. Measured over four consecutive updates: every transfer reached 100%,
    // every image booted and confirmed, and two of the four restarts came in as panics half a
    // second after the last byte landed. So the panic is real, harmless (the image is already
    // committed when it happens), and not fixable from here: the idle stack size lives in the
    // prebuilt framework's sdkconfig, and the Arduino-as-IDF-component path needed to change
    // it does not build for this board.
    //
    // What is NOT acceptable is a crash counter that counts it. The counter exists so that a
    // server watching telemetry can tell a boot loop from a quiet network, and a number that
    // ticks up every time we deliberately reboot answers a different question than the one it
    // claims to. So an intentional restart writes its intent to NVS first (health_restart)
    // and this reads it back: the reset REASON is still reported exactly as the hardware gave
    // it, because that is a fact, but it does not count as a crash.
    bool ours = s_prefs.getBool("intent", false);
    if (ours) s_prefs.remove("intent");
    s_intentional = ours;

    if (!ours && (s_reason == ESP_RST_PANIC || s_reason == ESP_RST_TASK_WDT ||
                  s_reason == ESP_RST_INT_WDT || s_reason == ESP_RST_WDT)) {
        s_crashes++;
        s_prefs.putUInt("crashes", s_crashes);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    s_addr = (running != nullptr) ? running->address : 0;

    // Zero means no image has ever proved itself on this board - a first boot, or a board
    // whose NVS was erased. There is nothing to revert TO, so the attempt counter still runs
    // (it is what tells the server this image is unproven) but the revert is skipped.
    uint32_t good = s_prefs.getUInt("good", 0);
    s_pending = (s_addr != 0 && s_addr != good);

    uint8_t tries = 0;
    if (s_pending) {
        tries = s_prefs.getUChar("tries", 0);
        if (tries < 255) tries++;
        s_prefs.putUChar("tries", tries);
    } else if (s_prefs.getUChar("tries", 0) != 0) {
        s_prefs.putUChar("tries", 0);  // housekeeping: the proven image owns a clean slate
    }

    hlogf("[health] reset=%s%s crashes=%lu image=%s",
                  health_reset_name(), s_intentional ? " (ours)" : "",
                  (unsigned long)s_crashes,
                  s_pending ? "unproven" : "confirmed");
    if (s_pending) {
        hlogf(" try=%u/%u at 0x%06lX back=0x%06lX",
                      (unsigned)tries, (unsigned)MAX_TRIES,
                      (unsigned long)s_addr, (unsigned long)good);
    }
    hlogf("\n");

    // Registered after the reset line so that line still prints if registration ever fails, and
    // before setup() allocates anything worth losing.
    heap_caps_register_failed_alloc_callback(alloc_failed_cb);
    if (s_af_magic == AF_MAGIC) {
        s_af_magic = 0;
        s_af_fn[sizeof(s_af_fn) - 1] = '\0';
        hlogf("[health] previous boot lost a %luB alloc (caps=0x%lx) in %s\n",
                      (unsigned long)s_af_size, (unsigned long)s_af_caps, s_af_fn);
    }

    read_coredump();

    if (s_pending && tries > MAX_TRIES && good != 0) revert_to(good);
}

// Called by main once the display is up and drawing. Separate from the network check because
// they fail for different reasons and only one of them is ever the greenhouse's fault.
void health_ui_ready(void) { s_ui_ready = true; }

// Called from the main loop. Decides when a probationary image has proved itself, and gives up
// on one that cannot draw.
void health_tick(void) {
    if (!s_pending || s_validated) return;

    uint32_t up = millis() - s_up_start;

    // Nothing on the screen after three minutes. Restart, so the attempt counter can reach
    // MAX_TRIES and hand the board back to the image that could draw.
    if (!s_ui_ready) {
        if (up > UI_DEADLINE_MS) {
            health_restart("no UI");
        }
        return;
    }

    bool wifi_ok = (net_state() == NET_CONNECTED);
    if (!wifi_ok && up < NET_DEADLINE_MS) return;

    // Accepted. `good` is what the next boot compares itself against, so writing it is the
    // whole of "this image works"; tries is cleared so a future image starts from zero.
    s_prefs.putUInt("good", s_addr);
    s_prefs.putUChar("tries", 0);
    s_validated = true;

    // Free if the bootloader ever does support rollback, and a no-op when it does not - the
    // line above is what actually ends probation on this platform.
    esp_ota_mark_app_valid_cancel_rollback();

    hlogf("[health] image at 0x%06lX confirmed (%s)\n", (unsigned long)s_addr,
                  wifi_ok ? "UI + WiFi up" : "UI up, 10 min without a crash");
}

// Called from the main loop. Internal-DRAM headroom is a trend rather than a number: one that
// only falls is a leak, and one that sits low is a design that will eventually lose a small
// allocation to the WiFi driver and abort. Printed in bytes because the movement that matters
// is smaller than a kilobyte, and `low` is the minimum ever seen - the figure that says whether
// this board has margin at all. `used` and `nblk` separate the two shapes a leak takes: many
// small blocks that are never freed (nblk climbs) or one buffer that keeps growing (used
// climbs while nblk stays flat). PSRAM rides along in KB because seeing it fine rules it out.
void health_debug_tick(void) {
    static uint32_t s_print_ms = 0;
    if (millis() - s_print_ms < 4000) return;
    s_print_ms = millis();
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    hlogf("[heap] int free=%u low=%u blk=%u used=%u nblk=%u | psram free=%uK\n",
                  (unsigned)hi.total_free_bytes,
                  (unsigned)hi.minimum_free_bytes,
                  (unsigned)hi.largest_free_block,
                  (unsigned)hi.total_allocated_bytes,
                  (unsigned)hi.allocated_blocks,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// Records the intent, then goes. The write must land before the restart or the next boot
// cannot tell whose reboot it was, so this commits NVS and gives the log a moment to drain
// rather than trusting either to finish inside esp_restart().
//
// WHY THERE IS NO CLEVERNESS HERE, AND WHAT WAS TRIED. About two in five restarts arrive as
// ESP_RST_PANIC with a stack-canary watchpoint, half a second after the last byte of an update
// lands. The first theory was a real overflow: esp_restart() stops the other core with a
// level-4 interrupt, that interrupt runs on whatever stack the other core was using, and an
// idle task's stack is 1024 bytes with about 350 free. So a pad task was made to spin on the
// other core, giving the interrupt a 4KB stack to land on instead.
//
// It did not work, and the way it failed is the useful part: the canary then named the PAD.
// Across these restarts the task blamed has been IDLE1, ipc1, and stallpad - whichever task
// happened to be current. A real overflow names the same task every time. What varies with the
// victim is a watchpoint, not a stack: FreeRTOS arms a debug watchpoint on the current task's
// canary at every context switch, and tearing the scheduler down inside esp_restart() leaves
// one armed over memory that is being dismantled. There is no overflow to prevent.
//
// So the restart stays simple, and the consequence is handled where it lands: the reset reason
// is reported as the hardware gave it, the crash counter skips it because the intent flag says
// it was ours, and read_coredump() drops a dump written during our own restart rather than
// reporting a crash in a task that did not crash.
void health_restart(const char *why) {
    s_prefs.putBool("intent", true);
    s_prefs.end();
    hlogf("[health] restarting on purpose (%s)\n", why);
    Serial.flush();
    delay(80);
    esp_restart();
}

uint32_t health_crashes(void) { return s_crashes; }
bool health_have_crash(void) { return s_c_have; }
const char *health_crash_task(void) { return s_c_task; }
const char *health_crash_pc(void) { return s_c_pc; }
const char *health_crash_bt(void) { return s_c_bt; }

// Called once the server has the crash. Erasing here rather than at boot is the difference
// between "we know what killed it" and "we knew, before it died again on the way to telling
// anyone" - the dump is the only copy, and a board that panics during its first post would
// have thrown it away for nothing.
void health_crash_reported(void) {
    if (!s_c_have) return;
    s_c_have = false;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_core_dump_image_erase();
#endif
    hlogf("[health] coredump delivered and erased" "\n");
}
bool health_image_pending(void) { return s_pending && !s_validated; }

// Short, stable, wire-friendly names. Not esp_reset_reason()'s enum number: a server log
// reading "reset=7" sends the reader to a header file, and the whole point of this field is
// that somebody looking at telemetry learns something immediately.
const char *health_reset_name(void) {
    switch (s_reason) {
    case ESP_RST_POWERON:   return "power";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}
