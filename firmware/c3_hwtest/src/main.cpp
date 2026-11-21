// AIplantRx display: C3 hardware test (not the real C3 firmware).
//
// What it checks, wired as in cases/main-display-v2/out/wiring.html:
//   - TCA9548A at 0x70 on IO5 SDA / IO6 SCL (writes each channel mask and reads it back)
//   - the main bus with all channels off answers only 0x70 (an OLED there means it skips the TCA)
//   - every TCA channel 0-7 for an OLED at 0x3C/0x3D; each one found is lit full white for 0.5 s
//     (dead pixel and supply check), then shows its panel of UI.md 5 with demo values (oled_ui.cpp;
//     ch2..6 = OLED 1..5)
//   - the S3 over UART: C3 IO21 TX -> S3 RXD, IO20 RX <- S3 TXD.  Sends "info" every 3 s; the S3 app
//     answers "info ver=..." (devsrv.cpp).  Needs the S3 UART switch on UART2.
//
// While something is missing it re-scans every 5 s (prints only changes), so wiring can be fixed live.
// SDA/SCL line levels are checked first: OPEN = no pull-up (wire off / TCA VIN dead), LOW = held down.
// S3 upload relay: esptool on this USB port (--before no_reset --after watchdog_reset); see relay_start().
// USB serial (the C3's own USB-C): a report every 10 s.  Commands: "scan" re-checks the I2C side,
// "s3 <line>" sends a line to the S3 (for example "s3 info"), "dump <ch>" prints that OLED's frame buffer
// (1024 bytes hex, SSD1306 page order) for tools/oled_dump.py, "upload-screen" shows the S3 upload
// screen for 8 s.
// Blue LED (IO8): slow blink = everything found, fast blink = something missing.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "oled_ui.h"

#define PIN_SDA 5
#define PIN_SCL 6
#define PIN_S3_RX 20  // C3 RX <- S3 TXD (GPIO43)
#define PIN_S3_TX 21  // C3 TX -> S3 RXD (GPIO44)
#define PIN_LED 8     // SuperMini blue LED, LOW = on
#define TCA 0x70
#define FIRST_CH 2    // OLED 1 is on channel 2 (user wiring 2026-11-20)
#define USB_TX_TIMEOUT_MS 10  // see setup()
#define WHITE_MS 500          // full-white check per OLED at start

struct Oled {
    bool found = false, ok = false;
    uint8_t addr = 0;
    uint32_t err = 0, frames = 0;
    uint32_t ms_max = 0;  // slowest refresh of this OLED since the last report (select + ping + draw)
    Adafruit_SSD1306 *d = nullptr;
};
static Oled oled[8];
static bool tca_ok = false, main_extra = false;
static uint32_t tca_err = 0;

// S3 link
static char s3line[256];
static int s3n = 0;
static uint32_t s3_bytes = 0, s3_last_ok = 0, s3_last_print = 0;
static String s3_ver;

static bool ping(uint8_t a) {
    Wire.beginTransmission(a);
    return Wire.endTransmission() == 0;
}

// ch < 0: all channels off.  Reads the control register back to be sure the TCA took it.
static bool tca_sel(int ch) {
    uint8_t m = ch < 0 ? 0 : (1 << ch);
    Wire.beginTransmission(TCA);
    Wire.write(m);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)TCA, (uint8_t)1) != 1) return false;
    return Wire.read() == m;
}

// Reads SDA/SCL as plain inputs with the C3's weak pull-down, then pull-up (Wire released meanwhile).
//   pull-down reads HIGH: an external pull-up is there (TCA board powered, wire connected)
//   pull-up reads LOW:    the line is held low (TCA board unpowered, its 10k go to a 0 V VIN, or a short)
static const char *line_state(int pin) {
    pinMode(pin, INPUT_PULLDOWN);
    delayMicroseconds(200);
    int pd = digitalRead(pin);
    pinMode(pin, INPUT_PULLUP);
    delayMicroseconds(200);
    int pu = digitalRead(pin);
    pinMode(pin, INPUT);
    if (pd && pu) return "ok";
    if (pu) return "OPEN (no pull-up: wire off or TCA VIN dead)";
    return "LOW (held down: TCA unpowered or short to GND)";
}

static String last_scan;

// verbose: always print; otherwise print only when the result differs from the last scan.
static void scan(bool verbose) {
    String out = "\n[i2c] scan: SDA IO5, SCL IO6, 400 kHz\n";
    Wire.end();
    const char *sda = line_state(PIN_SDA), *scl = line_state(PIN_SCL);
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    out += String("[i2c] lines: SDA ") + sda + ", SCL " + scl + "\n";
    bool lines_ok = !strcmp(sda, "ok") && !strcmp(scl, "ok");
    tca_ok = lines_ok && ping(TCA) && tca_sel(-1);
    main_extra = false;
    if (lines_ok) {
        String s;
        for (uint8_t a = 0x08; a < 0x78; a++)
            if (ping(a)) {
                s += " 0x" + String(a, HEX);
                if (a != TCA) main_extra = true;
            }
        out += "[i2c] main bus, all channels off:" + (s.length() ? s : String(" nothing")) + "\n";
    }
    if (!tca_ok) {
        out += "[i2c] TCA9548A 0x70 NOT FOUND. Check VIN=3V3, GND, SDA->IO5, SCL->IO6, A0-A2 not tied high.\n";
        for (auto &o : oled) o.found = o.ok = false;
    } else {
        if (main_extra) out += "[i2c] WARNING: something besides 0x70 answers with all channels off (OLED on the main bus?)\n";
        for (int ch = 0; ch < 8; ch++) {
            if (!tca_sel(ch)) {
                out += "[i2c] ch" + String(ch) + ": select FAILED\n";
                oled[ch].found = false;
                continue;
            }
            String f;
            uint8_t oa = 0;
            for (uint8_t a = 0x08; a < 0x78; a++) {
                if (a == TCA || !ping(a)) continue;
                f += " 0x" + String(a, HEX);
                if (!oa && (a == 0x3C || a == 0x3D)) oa = a;
            }
            if (!oa) oled[ch].ok = false;
            oled[ch].found = oa != 0;
            oled[ch].addr = oa;
            out += "[i2c] ch" + String(ch) + ":" + (f.length() ? f : String(" -")) + "\n";
        }
        tca_sel(-1);
    }
    if (verbose || out != last_scan) Serial.print(out);
    last_scan = out;
}

static int n_found() {
    int k = 0;
    for (auto &o : oled) k += o.found;
    return k;
}

static bool oled_begin(int ch) {
    Oled &o = oled[ch];
    if (!o.d) o.d = new Adafruit_SSD1306(128, 64, &Wire, -1, 400000, 400000);
    if (!tca_sel(ch)) return o.ok = false;
    // periphBegin=false: Wire is already on IO5/IO6; letting the library call Wire.begin() could move it.
    o.ok = o.d->begin(SSD1306_SWITCHCAPVCC, o.addr, true, false);
    return o.ok;
}

// Starts the OLEDs that were found but are not running yet (all of them at boot), ONE AT A TIME:
// each one goes full white (its highest current), then the TCA is watched for WHITE_MS.  If the TCA stops
// answering right then, that OLED is what pulls the shared 3.3 V down, and it is named in the log.
static void init_oleds() {
    for (int ch = 0; ch < 8; ch++) {
        if (!oled[ch].found) {
            oled[ch].ok = false;
            continue;
        }
        if (oled[ch].ok) continue;
        bool ok = oled_begin(ch);
        Serial.printf("[oled] ch%d 0x%02X init %s\n", ch, oled[ch].addr, ok ? "ok" : "FAILED");
        if (!ok) continue;
        oled[ch].d->clearDisplay();  // full white: dead pixel check, and the biggest current draw
        oled[ch].d->fillRect(0, 0, 128, 64, SSD1306_WHITE);
        oled[ch].d->display();
        tca_sel(-1);
        uint32_t t0 = millis();
        while (millis() - t0 < WHITE_MS) {
            if (!ping(TCA)) {
                Serial.printf("[pwr] TCA DROPPED %lu ms after OLED ch%d went white: ch%d (or its wiring) pulls the 3.3 V down\n",
                              (unsigned long)(millis() - t0), ch, ch);
                tca_ok = false;
                for (auto &o : oled) o.ok = false;
                return;
            }
            delay(50);
        }
        Serial.printf("[pwr] ch%d white for %d ms, TCA still fine\n", ch, WHITE_MS);
    }
    tca_sel(-1);
}

static bool s3_alive() { return s3_last_ok && millis() - s3_last_ok < 10000; }

static void s3_status(char *buf, size_t n) {
    if (s3_alive()) snprintf(buf, n, "S3 OK %s", s3_ver.c_str());
    else if (s3_bytes) snprintf(buf, n, "S3 rx %luB no reply", (unsigned long)s3_bytes);
    else snprintf(buf, n, "S3 UART: nothing");
}

static uint32_t upload_preview_until = 0;  // "upload-screen" command: show the relay screen for 8 s

static void draw(int ch, uint32_t now) {
    if (now < upload_preview_until) ui_upload(oled[ch].d);
    else ui_panel(oled[ch].d, ch - FIRST_CH, now);
    oled[ch].d->display();
    oled[ch].frames++;
}

static void refresh_oleds(uint32_t now) {
    for (int ch = 0; ch < 8; ch++) {
        Oled &o = oled[ch];
        if (!o.found) continue;
        uint32_t t0 = millis();
        struct Timer {  // records how long this channel took, whichever way it leaves
            Oled &o; uint32_t t0;
            ~Timer() { uint32_t dt = millis() - t0; if (dt > o.ms_max) o.ms_max = dt; }
        } timer{o, t0};
        if (!tca_sel(ch)) {
            tca_err++;
            continue;
        }
        if (!ping(o.addr)) {  // unplugged or loose: count it, re-init when it comes back
            if (o.ok) Serial.printf("[oled] ch%d LOST\n", ch);
            o.err++;
            o.ok = false;
            continue;
        }
        if (!o.ok) {
            if (!oled_begin(ch)) continue;
            Serial.printf("[oled] ch%d back\n", ch);
        }
        draw(ch, now);
    }
    tca_sel(-1);
}

static void on_s3_line(char *l) {
    for (char *p = l; *p; p++)
        if (*p < 32 || *p > 126) *p = '.';
    char *p = strstr(l, "info ver=");
    if (p) {
        bool was = s3_alive();
        p += 9;
        char *e = strchr(p, ' ');
        s3_ver = e ? String(p).substring(0, e - p) : String(p);
        s3_last_ok = millis();
        if (was && millis() - s3_last_print < 60000) return;  // don't print every 3 s
        s3_last_print = millis();
    }
    Serial.printf("[s3] %s\n", l);
}

static void poll_s3() {
    while (Serial0.available()) {
        int c = Serial0.read();
        s3_bytes++;
        if (c == '\n' || c == '\r') {
            if (s3n) {
                s3line[s3n] = 0;
                on_s3_line(s3line);
                s3n = 0;
            }
        } else if (s3n < (int)sizeof(s3line) - 1) s3line[s3n++] = c;
    }
}

// ---------------------------------------------------------------- S3 upload relay (ARCHITECTURE.md 4.4)
// esptool on the PC talks to the C3's own USB port:
//   esptool --chip esp32s3 --port COMxx --before no_reset --after watchdog_reset -b 460800 write_flash ...
// The first esptool SYNC packet (SLIP C0 00 08) switches the C3 into relay mode: it stops its own output,
// sends "aipx-download" to the S3 app (which restarts into the ROM serial loader, devsrv.cpp) and from
// then on copies bytes both ways.  esptool's CHANGE_BAUDRATE (command 0x0F) is followed: the new rate is
// read from the request and applied to the S3 UART right after the S3's reply has gone to the PC (the
// C3's USB side has no baud rate).  8 s without bytes from the PC ends relay mode.
struct Slip {
    bool in = false, esc = false;
    uint8_t b[12];
    int n = 0;
};

// Feeds one byte.  True when a frame has just ended; its first 12 decoded bytes are in b, n = length.
static bool slip_feed(Slip &s, uint8_t c) {
    if (c == 0xC0) {
        if (s.in && s.n) {
            s.in = false;
            return true;
        }
        s.in = true;
        s.n = 0;
        s.esc = false;
        return false;
    }
    if (!s.in) return false;
    if (s.esc) {
        c = c == 0xDC ? 0xC0 : (c == 0xDD ? 0xDB : c);
        s.esc = false;
    } else if (c == 0xDB) {
        s.esc = true;
        return false;
    }
    if (s.n < 12) s.b[s.n] = c;
    s.n++;
    return false;
}

#define RELAY_SETTLE_MS 1500
static char relay_log[400];  // what the S3 printed while it restarted (shown when the relay ends)
static size_t relay_log_n = 0;
static uint32_t relay_banner_ms = 0;  // 0 = no "waiting for download" seen
static int relay_selftest = 0;        // which of our own SYNC tries the loader answered (0 = none)
static uint32_t relay_found_baud = 0;  // a stub left at this rate by a broken-off upload (0 = none)
static bool relay = false;
static uint32_t relay_t = 0, relay_up = 0, relay_down = 0, relay_baud = 115200, pending_baud = 0;
static Slip slip_pc, slip_s3;
static uint32_t gap_max = 0;  // longest time between two OLED refreshes since the last report

static void oled_upload_screen() {
    for (int ch = 0; ch < 8; ch++) {
        if (!oled[ch].ok || !tca_sel(ch)) continue;
        ui_upload(oled[ch].d);
        oled[ch].d->display();
    }
    tca_sel(-1);
}

// Sends our own SYNC up to `tries` times; returns the try the loader (or a running stub) answered, 0 if none.
static int sync_test(int tries) {
    static const uint8_t SYNC[] = {0xC0, 0x00, 0x08, 0x24, 0, 0, 0, 0, 0, 0x07, 0x07, 0x12, 0x20,
                                   0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                   0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                   0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xC0};
    for (int attempt = 1; attempt <= tries; attempt++) {
        while (Serial0.available()) Serial0.read();
        Serial0.write(SYNC, sizeof SYNC);
        Serial0.flush();
        uint32_t t1 = millis();
        Slip sl;
        while (millis() - t1 < 250) {
            if (!Serial0.available()) {
                delay(1);
                continue;
            }
            if (slip_feed(sl, Serial0.read()) && sl.n >= 2 && sl.b[0] == 1 && sl.b[1] == 0x08) return attempt;
        }
    }
    return 0;
}

static void relay_start() {
    relay = true;
    relay_t = millis();
    relay_up = relay_down = pending_baud = 0;
    relay_baud = 115200;
    relay_found_baud = 0;
    slip_pc = Slip();
    slip_s3 = Slip();
    oled_upload_screen();
    Serial.setTxTimeoutMs(200);  // replies to esptool must not be dropped
    Serial0.updateBaudRate(115200);
    while (Serial0.available()) Serial0.read();
    Serial0.print("\naipx-download\n");
    Serial0.flush();
    // Wait for the ROM's "waiting for download" (an S3 already in the loader prints nothing: give up
    // after 2.5 s).  Nothing from the PC reaches the S3 before that; esptool keeps repeating SYNC.
    // Tested 2026-11-21: forwarding after a fixed 700 ms left the loader deaf until the next reset.
    const char *want = "waiting for download";
    size_t got = 0;
    uint32_t t0 = millis();
    relay_log_n = 0;
    while (millis() - t0 < 2500 && want[got]) {
        if (!Serial0.available()) {
            delay(1);
            continue;
        }
        char c = Serial0.read();
        if (relay_log_n < sizeof relay_log - 1) relay_log[relay_log_n++] = c;
        got = c == want[got] ? got + 1 : (c == want[0] ? 1 : 0);
    }
    relay_banner_ms = want[got] ? 0 : millis() - t0;
    // Forwarding esptool's SYNC straight after the banner left the loader deaf until the S3 was
    // power-cycled (2026-11-21, twice), while a loader that had first received a few plain text lines
    // answered fine.  So: settle, send a plain line first, then check with our own SYNC before esptool's
    // traffic goes through.  The result is kept for the "relay" command.
    delay(RELAY_SETTLE_MS);
    while (Serial0.available()) Serial0.read();  // rest of the banner
    Serial0.print("\r\n");
    Serial0.flush();
    delay(100);
    relay_selftest = sync_test(5);
    // Nothing at 115200: an upload that broke off leaves esptool's stub running at its faster rate.
    // Find it there and keep the UART at that rate (the PC side has no baud rate, so esptool still works).
    // 1843200 = what a stub at 460800 jumps to when told "old rate 115200" (it scales its divider by
    // that), which is what happened when the PC side was simply bridged at the faster rate.
    static const uint32_t FALLBACK[] = {460800, 921600, 230400, 1843200, 3686400};
    for (uint32_t b : FALLBACK) {
        if (relay_selftest) break;
        Serial0.updateBaudRate(b);
        int a = sync_test(3);
        if (a) {
            relay_selftest = a;
            relay_found_baud = b;
        }
    }
    if (relay_found_baud) {
        // Bring the stub back to 115200 with its own CHANGE_BAUDRATE (new, old = the real current rate),
        // so esptool starts from the rate it assumes.
        uint32_t nb = 115200, ob = relay_found_baud;
        uint8_t f[] = {0xC0, 0x00, 0x0F, 0x08, 0x00, 0, 0, 0, 0,
                       (uint8_t)nb, (uint8_t)(nb >> 8), (uint8_t)(nb >> 16), (uint8_t)(nb >> 24),
                       (uint8_t)ob, (uint8_t)(ob >> 8), (uint8_t)(ob >> 16), (uint8_t)(ob >> 24), 0xC0};
        while (Serial0.available()) Serial0.read();
        Serial0.write(f, sizeof f);  // none of these rates contains a 0xC0 or 0xDB byte: no escaping needed
        Serial0.flush();
        uint32_t t1 = millis();
        Slip sl;
        bool ok = false;
        while (millis() - t1 < 300 && !ok) {
            if (!Serial0.available()) {
                delay(1);
                continue;
            }
            ok = slip_feed(sl, Serial0.read()) && sl.n >= 2 && sl.b[0] == 1 && sl.b[1] == 0x0F;
        }
        delay(20);
    }
    Serial0.updateBaudRate(115200);
    if (relay_found_baud) relay_selftest = sync_test(3);  // confirm at 115200
    delay(50);
    while (Serial0.available()) Serial0.read();  // the loader answers each SYNC several times
    while (Serial.available()) Serial.read();    // SYNCs sent before the loader was listening
}

static void relay_report() {
    Serial.printf("[relay] last: PC->S3 %lu B, S3->PC %lu B, last baud %lu, banner after %lu ms, own SYNC answered %s, stub found at %lu\n",
                  (unsigned long)relay_up, (unsigned long)relay_down, (unsigned long)relay_baud,
                  (unsigned long)relay_banner_ms, relay_selftest ? String("at try " + String(relay_selftest)).c_str() : "NO",
                  (unsigned long)relay_found_baud);
    relay_log[relay_log_n] = 0;
    for (size_t i = 0; i < relay_log_n; i++)
        if (relay_log[i] != '\n' && (relay_log[i] < 32 || relay_log[i] > 126)) relay_log[i] = '.';
    Serial.printf("[relay] S3 said while restarting:\n%s\n", relay_log);
}

static void relay_loop() {
    static uint8_t buf[512];
    uint32_t now = millis();
    int k;
    while ((k = Serial.available()) > 0) {
        k = Serial.read(buf, k < (int)sizeof buf ? k : (int)sizeof buf);
        if (k <= 0) break;
        for (int i = 0; i < k; i++)
            if (slip_feed(slip_pc, buf[i]) && slip_pc.n >= 12 && slip_pc.b[0] == 0 && slip_pc.b[1] == 0x0F)
                pending_baud = slip_pc.b[8] | slip_pc.b[9] << 8 | (uint32_t)slip_pc.b[10] << 16 | (uint32_t)slip_pc.b[11] << 24;
        Serial0.write(buf, k);
        relay_up += k;
        relay_t = now;
    }
    while ((k = Serial0.available()) > 0) {
        k = Serial0.read(buf, k < (int)sizeof buf ? k : (int)sizeof buf);
        if (k <= 0) break;
        int from = 0;
        for (int i = 0; i < k; i++)
            if (slip_feed(slip_s3, buf[i]) && pending_baud && slip_s3.n >= 2 && slip_s3.b[0] == 1 && slip_s3.b[1] == 0x0F) {
                Serial.write(buf + from, i + 1 - from);  // the reply still goes out at the old rate
                from = i + 1;
                Serial.flush();
                Serial0.updateBaudRate(pending_baud);
                relay_baud = pending_baud;
                pending_baud = 0;
            }
        if (k > from) Serial.write(buf + from, k - from);
        relay_down += k;
    }
    if (now - relay_t > 8000) {  // esptool finished or gave up
        relay = false;
        Serial0.updateBaudRate(115200);
        Serial.setTxTimeoutMs(USB_TX_TIMEOUT_MS);
        delay(2000);  // let the S3 app boot before the C3 starts asking it things again
        while (Serial0.available()) Serial0.read();
        relay_report();
        for (auto &o : oled) o.ok = false;  // re-init and redraw
    }
}

static void poll_usb() {
    static char line[200];
    static int n = 0;
    static int h0 = -1, h1 = -1;  // the two bytes before this one
    while (Serial.available()) {
        int c = Serial.read();
        if (h0 == 0xC0 && h1 == 0x00 && c == 0x08) {  // esptool SYNC: become the S3 upload relay
            h0 = h1 = -1;
            n = 0;
            relay_start();
            return;
        }
        h0 = h1;
        h1 = c;
        if (c != '\n' && c != '\r') {
            if (n < (int)sizeof(line) - 1) line[n++] = c;
            continue;
        }
        if (!n) continue;
        line[n] = 0;
        n = 0;
        if (!strcmp(line, "scan")) {
            scan(true);
            init_oleds();
        } else if (!strcmp(line, "relay")) {
            relay_report();
        } else if (!strncmp(line, "dump ", 5)) {
            int ch = atoi(line + 5);
            if (ch < 0 || ch > 7 || !oled[ch].d) {
                Serial.println("[dump] no OLED there");
                continue;
            }
            const uint8_t *b = oled[ch].d->getBuffer();
            Serial.printf("[dump] ch%d ", ch);
            for (int i = 0; i < 1024; i++) Serial.printf("%02x", b[i]);
            Serial.println();
        } else if (!strcmp(line, "upload-screen")) {
            upload_preview_until = millis() + 8000;
        } else if (!strncmp(line, "s3 ", 3)) {
            Serial0.printf("%s\n", line + 3);
            Serial.printf("[c3] sent to S3: %s\n", line + 3);
        } else Serial.println("[c3] commands: scan | relay | dump <ch> | upload-screen | s3 <line>");
    }
}

static int n_oled_ok() {
    int k = 0;
    for (auto &o : oled) k += o.ok;
    return k;
}

// Nothing heard from the S3: look at the two UART pins as plain inputs (UART released for a moment).
// An idle UART TX is driven HIGH, so the pin wired to the S3's TXD should read HIGH even with the C3's
// pull-down on.  The S3's RXD is an input (maybe with a weak pull-up), so IO21 can read either way.
static int pin_level(int pin) {  // 1 = high against pull-down, 0 = low against pull-up, -1 = floating
    pinMode(pin, INPUT_PULLDOWN);
    delayMicroseconds(300);
    int pd = digitalRead(pin);
    pinMode(pin, INPUT_PULLUP);
    delayMicroseconds(300);
    int pu = digitalRead(pin);
    pinMode(pin, INPUT);
    return pd ? 1 : (pu ? -1 : 0);
}

static void uart_check() {
    Serial0.end();
    int rx = pin_level(PIN_S3_RX), tx = pin_level(PIN_S3_TX);
    Serial0.begin(115200, SERIAL_8N1, PIN_S3_RX, PIN_S3_TX);
    auto nm = [](int v) { return v == 1 ? "HIGH" : (v == 0 ? "LOW" : "floating"); };
    Serial.printf("[uart] IO20 (should be S3 TXD): %s, IO21 (to S3 RXD): %s -> ", nm(rx), nm(tx));
    if (rx == 1) Serial.println("S3 TXD reaches IO20; the S3 is just not sending (firmware? baud?)");
    else if (tx == 1) Serial.println("IO21 is HIGH and IO20 is not: TX/RX look SWAPPED (or only the S3 RXD pull-up)");
    else if (rx == -1) Serial.println("nothing drives IO20: UART switch on UART1, wrong terminal, or wire off");
    else Serial.println("IO20 held LOW: S3 TXD shorted to GND or S3 in reset");
}

static void report(uint32_t now) {
    String s;
    for (int ch = 0; ch < 8; ch++)
        if (oled[ch].found) s += " ch" + String(ch) + (oled[ch].ok ? ":ok" : ":FAIL") + (oled[ch].err ? "(err " + String(oled[ch].err) + ")" : "");
    char b[48];
    s3_status(b, sizeof b);
    Serial.printf("[c3] up %lus | TCA %s%s | OLED %d/5%s | %s (rx %lu B)\n", (unsigned long)(now / 1000), tca_ok ? "ok" : "MISSING",
                  tca_err ? (" err " + String(tca_err)).c_str() : "", n_oled_ok(), s.c_str(), b, (unsigned long)s3_bytes);
    String t;
    bool slow = false;
    for (int ch = 0; ch < 8; ch++)
        if (oled[ch].found) {
            t += " ch" + String(ch) + ":" + String(oled[ch].ms_max);
            if (oled[ch].ms_max > 200) slow = true;
            oled[ch].ms_max = 0;
        }
    if (t.length())
        Serial.printf("[time] slowest refresh per OLED, ms (normal ~30):%s%s | longest pause between refreshes %lu ms%s\n", t.c_str(),
                      slow ? "  <- SLOW: that channel stalls the bus" : "", (unsigned long)gap_max, gap_max > 1000 ? "  <- OLEDs FROZE" : "");
    gap_max = 0;
    if (!s3_bytes) uart_check();
}

void setup() {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    Serial.setRxBufferSize(24576);  // relay: esptool sends a whole 16 KB flash block before it waits; USB is
                                    // much faster than the UART, so the block has to fit (8 KB overflowed)
    Serial.begin(115200);
    // Don't stall when the PC has the port closed.  Not 0: HWCDC::write (core 3.1) counts its no-progress
    // retries down from this value, so 0 underflows and write() spins until a PC opens the port (the
    // OLEDs froze for minutes whenever no program was reading, 2026-11-21).  With 10 it gives up after
    // 10 ms, marks the host gone and drops output until the PC reads again.
    Serial.setTxTimeoutMs(USB_TX_TIMEOUT_MS);
    Serial0.setRxBufferSize(2048);
    Serial0.setTxBufferSize(2048);
    Serial0.begin(115200, SERIAL_8N1, PIN_S3_RX, PIN_S3_TX);
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    delay(1500);  // time to open the monitor
    Serial.println("\n=== AIplantRx C3 hardware test ===");
    scan(true);
    init_oleds();
    report(millis());
}

void loop() {
    static uint32_t t_draw = 0, t_info = 0, t_rep = 0, t_led = 0, t_scan = 0;
    static bool led = false;
    uint32_t now = millis();
    if (relay) {  // S3 upload in progress: nothing else touches the UART or the USB port
        relay_loop();
        if (now - t_led >= 60) {
            t_led = now;
            led = !led;
            digitalWrite(PIN_LED, led ? LOW : HIGH);
        }
        return;
    }
    poll_s3();
    poll_usb();
    if (relay) return;
    if (now - t_scan >= 5000 && (!tca_ok || n_found() < 5)) {  // something missing: look again (rewiring live)
        t_scan = now;
        scan(false);
        init_oleds();
    }
    if (now - t_draw >= 150) {
        if (t_draw && now - t_draw > gap_max) gap_max = now - t_draw;  // a long gap = OLEDs look frozen
        t_draw = now;
        refresh_oleds(now);
    }
    if (now - t_info >= 3000) {
        t_info = now;
        Serial0.print("info\n");
    }
    if (now - t_rep >= 10000) {
        t_rep = now;
        report(now);
    }
    bool good = tca_ok && n_oled_ok() == 5 && s3_alive();
    if (now - t_led >= (good ? 500u : 120u)) {
        t_led = now;
        led = !led;
        digitalWrite(PIN_LED, led ? LOW : HIGH);
    }
    delay(2);
}
