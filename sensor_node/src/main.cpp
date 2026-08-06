// Sensor node: SCD41 (CO2 + temp/humidity) + BH1750FVI (lux)
// + MLX90640 (32x24 thermal camera).
// Broadcasts SensorMsg to the S3 over ESP-NOW every ~2s (the SCD41 itself only
// produces a reading every ~5s, so this is about not adding pipeline lag on top
// of it), and a palette-mapped thermal frame per sensor subpage as a burst of
// fragments. The thermal camera used to hang off the
// ESP32-CAM over UART; it lives here because the CAM was already saturated by
// UXGA capture plus streaming, and its regulator had no headroom left.
//
// Wiring — one shared I2C bus, no multiplexer. Every device has a distinct
// fixed address, so they coexist directly; a mux would only be needed for two
// devices sharing an address (e.g. a second SCD41, which is stuck at 0x62).
// An earlier build routed these through a PCA9548A, and the extra contacts in
// the path starved the SCD41's ~200mA CO2 measurement spike.
//   I2C: SDA -> GPIO21, SCL -> GPIO22
//        (SCD41 0x62, BH1750FVI 0x23, MLX90640 0x33)
//
// The BH1750FVI is physically absent right now — the unit was destroyed — so
// lux stays at its < -999 sentinel and the S3 shows it as unavailable. Its
// code stays in: it degrades gracefully on a silent bus, and a replacement
// part will simply start reporting with no change here.
#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <string.h>
#include "sensor_protocol.h"
#include "sensor_scd41.h"
#include "sensor_bh1750.h"
#include "thermal_mlx.h"
#include "oled.h"

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static float s_co2 = -1000.0f, s_temp = -1000.0f, s_hum = -1000.0f, s_lux = -1000.0f;
// Peak temperature of the last thermal frame, purely so the periodic log line
// can show at a glance whether the MLX90640 is alive.
static float s_thermal_max = -1000.0f;
// Where the S3 is: the channel it told us it listens on, its MAC, and when it
// last spoke. Zero/false until the first ChannelMsg arrives. The S3 answers
// every telemetry send, so a healthy link refreshes this every ~4s; a TTL of
// three cycles means a stale fix is abandoned quickly.
//
// The MAC matters as much as the channel. ESP-NOW BROADCAST is unacknowledged
// and never retried, so a broadcast that collides is simply gone — and since a
// sweep puts exactly one copy on the S3's channel, telemetry gets a single
// unprotected attempt every 4s. That is what fails the moment the ESP32-CAM
// powers up and starts pushing RTSP/MJPEG on the same channel. UNICAST to a
// known MAC is acknowledged and retried by the MAC layer, which is the actual
// fix rather than sending more copies and hoping.
static volatile uint8_t s_s3_channel = 0;
static volatile uint32_t s_s3_channel_ms = 0;
static uint8_t s_s3_mac[6];
static volatile bool s_s3_mac_known = false;
static const uint32_t CHANNEL_TTL_MS = 12000;

static bool s3_channel(uint8_t *ch) {
    uint8_t c = s_s3_channel;
    if (c == 0 || millis() - s_s3_channel_ms > CHANNEL_TTL_MS) return false;
    *ch = c;
    return true;
}

// True when we can unicast: we know both where the S3 is and how to address it.
static bool s3_reachable(uint8_t *ch) {
    return s_s3_mac_known && s3_channel(ch);
}

// ESP-NOW defaults to a 1 Mbps long-range rate, where a single 242-byte thermal
// fragment holds the channel for roughly 2ms. At the ~55 fragments/s a 7.9fps
// thermal stream needs, that is over 10% of the airtime spent before the S3's
// TCP video pull gets a turn, and measurably it cost the video most of its frame
// rate: video ran at 13.6fps while the node was sweeping at 1fps thermal, and
// collapsed to 2.5fps once thermal locked on and went to full rate.
//
// Every radio here sits in one room at about -35dBm, so HT20 MCS3 (26 Mbps) is a
// comfortable rate and cuts that airtime by more than 20x. Set per peer, because
// that is the only interface the current IDF offers that isn't deprecated.
static void set_fast_rate(const uint8_t *mac) {
    esp_now_rate_config_t rc = {};
    rc.phymode = WIFI_PHY_MODE_HT20;
    rc.rate = WIFI_PHY_RATE_MCS3_LGI;
    rc.ersu = false;
    rc.dcm = false;
    esp_now_set_peer_rate_config(mac, &rc);
}

// The S3 answers every telemetry send with the channel it is on. Keep this tiny:
// it runs in the ESP-NOW receive callback.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != (int)sizeof(ChannelMsg)) return;
    ChannelMsg c;
    memcpy(&c, data, sizeof(c));
    if (c.magic != SENSOR_MAGIC || c.type != SENSOR_CHANNEL) return;
    if (c.channel < 1 || c.channel > 13) return;
    s_s3_channel = c.channel;
    s_s3_channel_ms = millis();
    // Register as a unicast peer the first time we learn the MAC. Done here
    // rather than per-send because add_peer is the expensive part, and the
    // peer's channel is left at 0 (follow the radio) since we hop before every
    // send anyway.
    if (!s_s3_mac_known) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, info->src_addr, 6);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
        set_fast_rate(peer.peer_addr);
        memcpy(s_s3_mac, info->src_addr, 6);
        s_s3_mac_known = true;
    }
}

// Channel the radio is parked on. Both senders below hop it, so they share this
// so neither skips a hop the other made, nor pays a settle delay it doesn't owe.
static uint8_t s_cur_ch = 0;

// esp_now_send only QUEUES a packet; the radio keys it some time later. Changing
// channel in between transmits it on the WRONG channel, or drops it. That is not
// theoretical: a 3ms post-send delay lost every single telemetry broadcast once
// the CAM's HTTP stream filled the TX queue, while thermal survived purely
// because seven back-to-back fragments happen to dwell ~7ms per channel. The
// queue-accept return value cannot see this — it reports queueing, not keying.
//
// So the send callback gates the hop. It also gives real delivery status: for
// unicast, SUCCESS means the peer ACKed.
static volatile bool s_tx_done = true;
static uint32_t s_tx_keyed = 0, s_tx_lost = 0;

static void on_sent(const uint8_t *mac, esp_now_send_status_t status) {
    (void)mac;
    if (status == ESP_NOW_SEND_SUCCESS) s_tx_keyed++;
    else s_tx_lost++;
    s_tx_done = true;
}

// Queues one packet, marking the radio busy so the next hop waits for it.
static bool send_marked(const uint8_t *dst, const void *p, size_t n) {
    s_tx_done = false;
    if (esp_now_send(dst, (const uint8_t *)p, n) == ESP_OK) return true;
    s_tx_done = true;  // never queued, so no callback is coming
    return false;
}

// How long to let an outstanding packet finish before hopping anyway. Generous:
// a congested channel is exactly when this matters, and the alternative is
// silently transmitting on the wrong one.
static const uint32_t TX_DRAIN_MS = 40;

static void drain_tx(void) {
    uint32_t t0 = millis();
    while (!s_tx_done && millis() - t0 < TX_DRAIN_MS) delay(1);
}

static void espnow_init(void) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("esp_now_init failed");
        return;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    set_fast_rate(peer.peer_addr);
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_sent);
}

// Hops the radio, caching the result only on success. Recording a channel the
// call did not actually reach would poison the cache: every later send would
// believe it was already there and skip the hop, transmitting on whatever
// channel the radio really sat on.
static bool hop_to(uint8_t ch) {
    if (ch == s_cur_ch) return true;  // no hop, so nothing to drain
    drain_tx();
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    s_cur_ch = ch;
    delay(5);  // let the radio settle before keying up
    return true;
}

// Result of the last telemetry attempt, surfaced in the log line.
static uint8_t s_bcast_sent = 0, s_bcast_failed = 0;
static bool s_bcast_unicast = false;

// Steady state is a single UNICAST to the S3, which the MAC layer acknowledges
// and retries — the only way a 4s-cadence message survives a channel busy with
// the CAM's RTSP stream.
//
// A full broadcast sweep still runs every SWEEP_EVERY-th cycle, and that is not
// redundancy for its own sake: it is the only path back if the S3 moves channel
// or its MAC changes, since the S3 answers a telemetry packet with its channel
// and nothing else tells us. Making unicast the ONLY path would deadlock exactly
// when recovery is needed. ~104ms every 20s buys guaranteed rediscovery.
static const uint8_t SWEEP_EVERY = 5;

static void broadcast_reading(void) {
    static uint8_t seq = 0;
    SensorMsg m = {};
    m.magic = SENSOR_MAGIC;
    m.type = SENSOR_DATA;
    m.seq = ++seq;
    m.co2_ppm = s_co2;
    m.temp_c = s_temp;
    m.hum_pct = s_hum;
    m.lux = s_lux;
    m.soil_pct = -1000.0f;  // not wired on this node yet

    static uint8_t n = 0;
    uint8_t ch;
    bool due_sweep = (n++ % SWEEP_EVERY) == 0;
    uint8_t sent = 0, failed = 0;

    s_bcast_unicast = s3_reachable(&ch);
    if (s_bcast_unicast) {
        if (!hop_to(ch)) failed++;
        else if (send_marked(s_s3_mac, &m, sizeof(m))) sent++;
        else failed++;
    }

    // Burst per channel rather than a single shot. A broadcast is unacknowledged
    // and never retried, so one packet per channel gives the S3 exactly one
    // unprotected chance every 4s — and empirically it got none at all while
    // thermal, which sends seven back-to-back on each channel, arrived fine. The
    // burst matches that shape and costs 39 packets per sweep, once every 20s
    // in steady state.
    if (due_sweep || !s_bcast_unicast) {
        for (uint8_t c = 1; c <= 13; c++) {
            if (!hop_to(c)) { failed++; continue; }
            for (uint8_t r = 0; r < 3; r++) {
                if (send_marked(BROADCAST, &m, sizeof(m))) sent++;
                else failed++;
                delay(1);
            }
        }
        // The last channel gets no following hop to drain it, so wait here.
        drain_tx();
    }
    s_bcast_sent = sent;
    s_bcast_failed = failed;
}

// A thermal frame is THERMAL_FRAG_COUNT packets and the S3 discards any frame it
// cannot fully reassemble, so every fragment must reach the same radio.
//
// Once the S3's MAC is known the fragments go out as UNICAST: the MAC layer
// acknowledges and retries each one, which matters because losing a single
// fragment costs the whole frame. Broadcast has no such protection, and with the
// CAM streaming on the same channel that showed up as whole frames vanishing.
//
// Without a MAC there is nothing to address, so fall back to a broadcast sweep —
// with the sweep on the OUTER loop so each channel sees the whole frame back to
// back; sweeping per fragment would scatter one fragment per channel and no
// channel would ever see a complete frame. That fallback is throttled, and the
// throttle is load-bearing: a swept frame is 13 hops and ~156ms of near-solid
// transmit, which at the sensor's full rate saturates the radio and starves the
// telemetry that carries the S3's address back to us — deadlocking exactly when
// recovery is needed.
static const uint32_t UNLOCKED_FRAME_MS = 1000;

static void broadcast_thermal(const uint16_t *px, float max_c, uint16_t peak_idx) {
    static uint8_t frame_id = 0;
    static uint8_t payload[THERMAL_PAYLOAD_BYTES];
    static ThermalFragMsg frags[THERMAL_FRAG_COUNT];
    static uint32_t last_unlocked_ms = 0;

    uint8_t first = 1, last = 13, ch;
    bool unicast = s3_reachable(&ch);
    if (unicast) {
        first = last = ch;
    } else {
        if (millis() - last_unlocked_ms < UNLOCKED_FRAME_MS) return;
        last_unlocked_ms = millis();
    }

    memcpy(payload, px, THERMAL_PIX_BYTES);
    memcpy(payload + THERMAL_PIX_BYTES, &max_c, sizeof(max_c));
    memcpy(payload + THERMAL_PIX_BYTES + sizeof(max_c), &peak_idx, sizeof(peak_idx));

    // Cut the frame up once; the fragments are identical whichever channel
    // carries them, and frame_id groups them on the receiving end.
    frame_id++;
    for (uint8_t f = 0; f < THERMAL_FRAG_COUNT; f++) {
        uint16_t off = (uint16_t)f * THERMAL_FRAG_BYTES;
        uint16_t len = THERMAL_PAYLOAD_BYTES - off;
        if (len > THERMAL_FRAG_BYTES) len = THERMAL_FRAG_BYTES;
        frags[f].magic = THERMAL_MAGIC;
        frags[f].type = THERMAL_FRAG;
        frags[f].frame_id = frame_id;
        frags[f].frag_index = f;
        frags[f].frag_count = THERMAL_FRAG_COUNT;
        frags[f].frag_len = len;
        memcpy(frags[f].data, payload + off, len);
    }

    const uint8_t *dst = unicast ? s_s3_mac : BROADCAST;
    for (uint8_t c = first; c <= last; c++) {
        if (!hop_to(c)) continue;
        for (uint8_t f = 0; f < THERMAL_FRAG_COUNT; f++) {
            // The tail fragment only fills 148 of its 232 data bytes, but
            // sending the fixed-size struct regardless keeps the framing on
            // both ends trivial; frag_len tells the S3 what to trust.
            send_marked(dst, &frags[f], sizeof(frags[f]));
            delay(1);  // just enough to keep the send queue from overrunning
        }
    }
    // Nothing hops after the final fragment, so drain it here rather than
    // leaving it to be killed by whatever hops next.
    drain_tx();
}

// Lists every device answering on the bus.
static void scan_bus(const char *label) {
    Serial.printf("%s:", label);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() != 0) continue;
        const char *who = a == 0x62                ? "SCD41"
                        : (a == 0x23 || a == 0x5C) ? "BH1750"
                        : a == 0x33                ? "MLX90640"
                                                   : "?";
        Serial.printf(" 0x%02X(%s)", a, who);
        found++;
    }
    Serial.println(found ? "" : " empty");
}

// Nine clocks and a STOP - the published way out of a stuck I2C bus.
//
// Nine because a byte is eight bits plus the ACK, so that many is enough for any slave to
// finish whatever transfer it thinks is in progress and release SDA. Stops early the
// moment SDA comes back, so a healthy bus costs one read.
//
// Bit-banged deliberately: this runs before Wire.begin(), and the point is to drive the
// lines in a way the I2C peripheral has no way to ask for.
static void i2c_bus_recover(int sda, int scl) {
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
    if (digitalRead(sda) == HIGH) return;   // idle already; nothing is being held

    Serial.println("[i2c] SDA held low - clocking the bus free");
    pinMode(scl, OUTPUT);
    for (int i = 0; i < 9 && digitalRead(sda) == LOW; i++) {
        digitalWrite(scl, LOW);
        delayMicroseconds(5);
        digitalWrite(scl, HIGH);
        delayMicroseconds(5);
    }

    // A manual STOP: SDA rises while SCL is high. Without it a slave that just released
    // SDA can still consider the transfer open.
    pinMode(sda, OUTPUT);
    digitalWrite(sda, LOW);
    delayMicroseconds(5);
    digitalWrite(scl, HIGH);
    delayMicroseconds(5);
    digitalWrite(sda, HIGH);
    delayMicroseconds(5);

    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
    Serial.printf("[i2c] after recovery SDA=%s\n",
                  digitalRead(sda) == HIGH ? "released" : "STILL LOW");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Sensor node: SCD41 + BH1750FVI + MLX90640 over ESP-NOW ===");

    // Un-wedge the bus before claiming it.
    //
    // An MCU reset in the middle of a byte leaves the slave that was talking still
    // holding SDA low, waiting for clocks that will never come. Every transaction after
    // that fails - the first with a timeout (Wire err 5, SDA never releases) and the rest
    // with an address NACK (err 2). Measured on exactly that: a firmware upload resets
    // this board but not the devices on its bus, and after one the display went from
    // working to answering nothing, while a power cycle - which resets both ends - was
    // always fine. The display was the visible casualty; the SCD41 and the MLX90640 hang
    // off the same two wires and are no less exposed.
    //
    // The remedy is the standard one and it has to happen before Wire.begin(), because
    // the peripheral cannot drive a recovery sequence: clock SCL by hand until SDA comes
    // free, then put a STOP on the wire so every slave returns to idle.
    i2c_bus_recover(21, 22);

    // BEFORE begin(), and that ordering is the whole point.
    //
    // A display page is 129 bytes - one control byte plus 128 of data - and Wire's default
    // buffer is exactly 128. One byte short, so it has to be raised; the question is when.
    // Raising it AFTER begin() takes the documented realloc path (Wire.cpp:194, "data may
    // be lost. We don't care!"), and what that costs is measurable: the first long transfer
    // after the realloc times out with Wire err 5 while two- and three-byte writes to the
    // same address keep succeeding. That was the display initialising on the second attempt
    // instead of the first, and before the retry existed it was the display not
    // initialising at all. Called here, Wire allocates the right size in begin() and no
    // transfer is ever handed a buffer that moved under it.
    Wire.setBufferSize(256);
    Wire.begin(21, 22, 100000);


    // The SCD41 comes up first. An MCU reset does NOT power-cycle it, so it
    // can still be in periodic measurement mode from the previous run — a
    // state where it rejects get_serial_number and start_periodic. scd41_init
    // stops it and settles it, and scanning before that would hammer the bus
    // with 112 address probes while it is mid-measurement.
    scd41_init();

    bh1750_init();

    // Only sets up this module's state; the MLX90640 itself is acquired lazily
    // from loop(), so an absent or later-plugged camera changes nothing here.
    thermal_mlx_init();

    // Report what actually answered, now that every sensor has been brought
    // up. A silent bus is almost always a pulled wire, and without this a
    // wiring fault is indistinguishable from a faulty sensor.
    scan_bus("I2C bus");

    // The display, after the scan that would have shown it. A node whose OLED is absent
    // must still broadcast, so this reports and moves on rather than gating anything on
    // it. 0x3C is free on this bus - SCD41 0x62, BH1750 0x23, MLX90640 0x33 - which is
    // most of why the display ended up here; see oled.cpp for the rest.
    if (oled_init()) {
        Serial.printf("[oled] up at 0x%02X on the sensor bus\n", oled_address());
    } else {
        Serial.println("[oled] none at 0x3C/0x3D - not wired, or no 5V");
    }

    espnow_init();
    Serial.println("Setup done. Broadcasting readings every ~2s, thermal as fast as the sensor allows.\n");
}

// What the display shows: the three numbers this board measures, on the 2s broadcast
// tick so the screen and the wire never disagree.
//
// Air temperature large, because that is the one a grower checks walking past and the
// only one worth reading from across a room. CO2 and humidity under it in small text.
// Not the thermal peak: it is a 32x24 scene maximum, so "hottest thing in frame" needs a
// picture beside it to mean anything, and there is no room for one here.
//
// Absent readings draw "--", the same convention the panel uses, for the same reason: a
// held number on a second screen is a stale reading with better reach. The SCD41 takes
// ~5s to produce its first sample and this runs at 2s, so "--" is what the first frame
// legitimately shows.
static void draw_oled(void) {
    if (!oled_ready()) return;
    char buf[20];
    oled_clear();

    oled_text(0, 0, "TEMP", 1);
    if (s_temp > -999.0f) snprintf(buf, sizeof(buf), "%.1f\xB0" "C", s_temp);
    else                  snprintf(buf, sizeof(buf), "--");
    oled_text(0, 2, buf, 2);          // pages 2-3: 10x14 glyphs

    if (s_co2 > -999.0f) snprintf(buf, sizeof(buf), "CO2 %.0f PPM", s_co2);
    else                 snprintf(buf, sizeof(buf), "CO2 --");
    oled_text(0, 5, buf, 1);

    if (s_hum > -999.0f) snprintf(buf, sizeof(buf), "RH  %.0f%%", s_hum);
    else                 snprintf(buf, sizeof(buf), "RH  --");
    oled_text(0, 6, buf, 1);

    // Uptime in seconds, bottom line. Not decoration: "the display went off" and "the
    // display stopped updating" are different faults with different causes, and from this
    // side they are indistinguishable - every write keeps succeeding either way. A number
    // that climbs says the panel is live; one that sits still says exactly when it
    // stopped; a blank line says the pixels went away. The screen answers the question
    // that three rounds of write-side logging could not.
    snprintf(buf, sizeof(buf), "UP %lu S", (unsigned long)(millis() / 1000));
    oled_text(0, 7, buf, 1);

    bool ok = oled_flush();
    // TEMP: prints once every 2s while the display is being brought up.
    static int n = 0;
    if (n++ < 12) {
        int st = oled_status();
        Serial.printf("[oled] draw: pixels=%d flush=%s status=%s%s temp=%.1f\n",
                      oled_pixels_set(), ok ? "ok" : "FAIL",
                      st < 0 ? "noread" : (st & 0x40) ? "DISPLAY-OFF" : "on",
                      st < 0 ? "" : "", s_temp);
    }
}

void loop() {
    static uint32_t last_scd = 0, last_bh = 0, last_bcast = 0;
    uint32_t now = millis();

    // 500ms, not 1s: this is one short I2C status read, and during bring-up the
    // state machine advances a step per tick - at 1s the granularity alone cost
    // seconds off the time to the first reading.
    if (now - last_scd > 500) {
        last_scd = now;
        scd41_tick();
    }

    if (now - last_bh > 1000) {
        last_bh = now;
        float lux = bh1750_read();
        if (lux > -999.0f) s_lux = lux;
    }

    // 2s, not 4s. The SCD41 itself only produces a reading every ~5s, so this
    // adds no new data - what it removes is the pipeline's own lag, which used
    // to stack a 4s broadcast wait on top of the S3's 4s refresh. Three 64-byte
    // packets every 2s is nothing next to the thermal stream's ~56 a second.
    if (now - last_bcast > 2000) {
        last_bcast = now;
        float scd_t = scd41_temp(), scd_h = scd41_hum();
        s_co2 = scd41_co2();
        s_temp = scd_t;
        s_hum = scd_h;

        broadcast_reading();
        // Re-init if the display is not up, every tick, forever.
        //
        // Giving up at boot was wrong. The failure this recovers from is a stuck SSD1306:
        // after a firmware upload resets this board mid-transfer the module can be left
        // NACKing its own address, and it stays that way until its power is pulled - so a
        // one-shot init at boot means a display that is dark for the rest of the run. The
        // same state is reachable in the field without an upload, because the module's
        // charge pump is the largest current draw on the rail and it browns out before the
        // MCU does. Retrying costs one probe every two seconds and turns "dark until
        // somebody notices" into "dark until the module answers again".
        if (!oled_ready()) {
            static int tries = 0;
            if (oled_init()) {
                Serial.printf("[oled] recovered after %d retries\n", tries);
                tries = 0;
            } else if (tries++ < 3) {
                Serial.println("[oled] still not answering; will keep retrying");
            }
        }
        draw_oled();
        uint8_t locked;
        // keyed/lost come from the send callback, i.e. what the radio actually
        // did. The queued count alone hid a total delivery failure once.
        Serial.printf("[bcast] co2=%.0fppm temp=%.1fC hum=%.0f%% lux=%.0flx thermal=%.1fC "
                      "| q %u/%u | keyed=%lu lost=%lu | %s ch %s\n",
                      s_co2, s_temp, s_hum, s_lux, s_thermal_max,
                      s_bcast_sent, (unsigned)(s_bcast_sent + s_bcast_failed),
                      (unsigned long)s_tx_keyed, (unsigned long)s_tx_lost,
                      s_bcast_unicast ? "unicast" : "bcast",
                      s3_channel(&locked) ? String(locked).c_str() : "sweeping");
    }

    // Last in the iteration on purpose: the cheap time-gated sensors above get
    // their turn against a fresh millis() first, and the thermal path absorbs
    // whatever is left. It is paced by the MLX90640's own 8Hz subpage rate — two
    // subpages per image, so ~4 frames a second — not by a software timer.
    static uint16_t thermal_px[THERMAL_W * THERMAL_H];
    float thermal_max;
    uint16_t thermal_peak;
    if (thermal_mlx_poll(thermal_px, &thermal_max, &thermal_peak)) {
        s_thermal_max = thermal_max;
        broadcast_thermal(thermal_px, thermal_max, thermal_peak);
    }
}
