#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>
#include "camprov.h"
#include "sensornode.h"
#include "thermal.h"
#include "hlog.h"

// Latest WiFi creds, mirrored from net.cpp. Read in the ESP-NOW recv callback,
// so keep the callback's work tiny — no NVS/flash access there.
static char s_ssid[33] = "";
static char s_pass[65] = "";

// Latest CAM status from its beacon (written in the recv callback). Generous
// so a WiFi scan (radio hops channels for a few seconds, missing beacons)
// doesn't briefly flip the camera to "offline".
static const uint32_t CAM_ONLINE_TIMEOUT_MS = 20000;
static volatile uint32_t s_cam_last_ms = 0;
static volatile uint8_t s_cam_ip[4] = {0, 0, 0, 0};
static volatile int8_t s_cam_rssi = 0;
static volatile bool s_cam_connected = false;
// The peer this panel will hand credentials to, learned on first contact and then
// held. Zero means nobody has asked yet.
//
// WHY THIS IS AN AUTHORIZATION DECISION AND NOT JUST AN ADDRESS. A PROV_REQUEST is
// twelve bytes of magic and type, and the reply carries this greenhouse's SSID and
// password. Before this pin, any device in ESP-NOW range that sent those twelve
// bytes got them - no allowlist, no secret, no pairing gesture, the only gate being
// whether the panel had credentials to give. xor_field() is not a defence: the
// access point broadcasts the SSID in the clear, so one captured reply XORed
// against the known SSID yields PROV_KEY and with it the password. The contents
// cannot be protected without changing the CAM's firmware, which is a different
// device and not in this repo - so what is fixed here is WHO gets an answer.
//
// Trust on first use, with one escape. The window where anyone can claim the pin
// is the first request after boot, which in practice is the CAM on the bench next
// to the panel; and a pin that could never move would brick a CAM replacement, so
// it is released after PROV_REPIN_MS of silence from the pinned device. An attacker
// therefore has to wait out the CAM being gone for ten minutes rather than simply
// asking.
static uint8_t s_cam_mac[6] = {0, 0, 0, 0, 0, 0};
static const uint32_t PROV_REPIN_MS = 600000;   // 10 min of silence releases the pin
static volatile uint32_t s_rx_refused = 0;      // requests from an unpinned peer
static char s_cam_ssid[33] = "";  // plaintext, non-volatile (matches s_cam_mac's treatment)

static void xor_field(char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[i] ^= PROV_KEY[i % sizeof(PROV_KEY)];
    }
}

void camprov_set_credentials(const char *ssid, const char *pass) {
    strncpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, pass ? pass : "", sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
}

static void send_reply(const uint8_t *dst) {
    if (s_ssid[0] == '\0') return;  // nothing to hand out yet
    ProvMsg r = {};
    r.magic = PROV_MAGIC;
    r.type = PROV_REPLY;
    strncpy(r.ssid, s_ssid, sizeof(r.ssid) - 1);
    strncpy(r.pass, s_pass, sizeof(r.pass) - 1);
    xor_field(r.ssid, sizeof(r.ssid));
    xor_field(r.pass, sizeof(r.pass));
    if (!esp_now_is_peer_exist(dst)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, dst, 6);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
    esp_now_send(dst, (const uint8_t *)&r, sizeof(r));
}

// Tell the sensor node which channel we are listening on, so it can unicast
// bulk thermal fragments here instead of broadcasting each one 13 times.
static void send_channel(const uint8_t *dst) {
    uint8_t ch = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&ch, &second) != ESP_OK || ch == 0) return;

    ChannelMsg c = {};
    c.magic = SENSOR_MAGIC;
    c.type = SENSOR_CHANNEL;
    c.channel = ch;
    if (!esp_now_is_peer_exist(dst)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, dst, 6);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
    esp_now_send(dst, (const uint8_t *)&c, sizeof(c));
}

// Every packet the radio handed us, bucketed. Without this a message that never
// arrives is indistinguishable from one that arrives and is rejected — and the
// two have completely different causes (RF/channel vs. protocol mismatch).
//
// `refused` is the security-relevant one: it counts well-formed messages from a
// device that is not the pinned CAM, which is either a second camera nobody
// mentioned or somebody asking this panel for the greenhouse WiFi password. Zero
// is the normal reading and any other value is worth a look.
static volatile uint32_t s_rx_total = 0, s_rx_sensor = 0, s_rx_thermal = 0;
static volatile uint32_t s_rx_status = 0, s_rx_prov = 0, s_rx_odd = 0;
static volatile int s_rx_odd_len = -1;
void camprov_debug_tick(void) {
    static uint32_t s_print_ms = 0;
    if (millis() - s_print_ms < 4000) return;
    s_print_ms = millis();
    hlogf("[espnow] rx=%lu sensor=%lu therm=%lu status=%lu prov=%lu "
                  "refused=%lu odd=%lu(len=%d)\n",
                  (unsigned long)s_rx_total, (unsigned long)s_rx_sensor,
                  (unsigned long)s_rx_thermal, (unsigned long)s_rx_status,
                  (unsigned long)s_rx_prov, (unsigned long)s_rx_refused,
                  (unsigned long)s_rx_odd, s_rx_odd_len);
}

// The sensor node's own pin, separate from the CAM's. Two physical devices, two
// producers, and nothing in either message identifies its sender - a SensorMsg is
// a magic, a type and readings, so anything in ESP-NOW range could inject a
// temperature and a humidity. That is not merely a display problem: those two
// numbers become VPD, VPD decides the panel's own judgment rows, and every reading
// goes up to the server where the model prescribes from it. Fabricated air data
// would come back as a prescription.
//
// Same trust-on-first-use shape as the CAM, and the same escape after
// PROV_REPIN_MS of silence so a replaced node can take over. Thermal fragments are
// held to the same pin: the frame carries the false-colour image the panel draws
// and the scene peak the 표면온도 tile and leaf_air_dt_c both read.
static uint8_t s_node_mac[6] = {0, 0, 0, 0, 0, 0};
static volatile uint32_t s_node_last_ms = 0;
// True when `src` may speak for `pin`, and pins it when nobody has yet.
//
// Used by the sensor node's two message types. The CAM's two sites keep the rule
// inline because theirs is not the same rule: its StatusMsg has to admit AND
// absorb a payload in one step, and its PROV_REQUEST has to pin without touching
// the liveness that camprov_cam_online() reads. Both are written out where they
// apply, above.
static bool pin_admits(uint8_t *pin, volatile uint32_t *last_ms, const uint8_t *src) {
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    bool pinned = memcmp(pin, zero, 6) != 0;
    if (pinned && memcmp(pin, src, 6) != 0) {
        // Silence is measured on the pinned device's own traffic, so a stranger
        // cannot age out the pin by talking.
        if (*last_ms != 0 && (millis() - *last_ms) <= PROV_REPIN_MS) return false;
    }
    if (!pinned || memcmp(pin, src, 6) != 0) memcpy(pin, src, 6);
    *last_ms = millis();
    return true;
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    s_rx_total++;
    // Sensor-node telemetry (distinct size from ProvMsg/StatusMsg).
    if (len == (int)sizeof(SensorMsg)) {
        s_rx_sensor++;
        SensorMsg m;
        memcpy(&m, data, sizeof(m));
        if (m.magic != SENSOR_MAGIC || m.type != SENSOR_DATA) return;
        if (!pin_admits(s_node_mac, &s_node_last_ms, info->src_addr)) {
            s_rx_refused++;
            return;
        }
        sensornode_on_recv(m);
        send_channel(info->src_addr);
        return;
    }

    // Thermal frame fragment from the sensor node (242 bytes — distinct from
    // every other message in either family, so size-based dispatch still holds).
    if (len == (int)sizeof(ThermalFragMsg)) {
        s_rx_thermal++;
        ThermalFragMsg m;
        memcpy(&m, data, sizeof(m));
        if (m.magic != THERMAL_MAGIC || m.type != THERMAL_FRAG) return;
        // Admitted against the node's pin but WITHOUT refreshing it: a frame is
        // 6 fragments and arrives many times a second, so counting them as liveness
        // would let a fragment storm hold the pin open forever. SensorMsg is the
        // node's heartbeat and the only thing that keeps its pin alive.
        static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        if (memcmp(s_node_mac, zero, 6) != 0 &&
            memcmp(s_node_mac, info->src_addr, 6) != 0) {
            s_rx_refused++;
            return;
        }
        thermal_on_frag(m);
        return;
    }

    // Health beacon from the CAM (distinct size from a provisioning request).
    if (len == (int)sizeof(StatusMsg)) {
        s_rx_status++;
        StatusMsg s;
        memcpy(&s, data, sizeof(s));
        if (s.magic != PROV_MAGIC || s.type != PROV_STATUS) return;

        // Same pin as the provisioning reply, and for a second reason on top of the
        // credentials: this beacon carries the IP the panel then pulls MJPEG from
        // (camnet reads it through camprov_cam_ip). An unpinned beacon could point
        // the camera feed at any address on the LAN and the panel would show
        // whatever answered, captioned 실시간 영상. So a stranger's beacon is
        // counted and dropped rather than believed.
        static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        bool pinned = memcmp(s_cam_mac, zero, 6) != 0;
        if (pinned && memcmp(s_cam_mac, info->src_addr, 6) != 0) {
            if (s_cam_last_ms == 0 || (millis() - s_cam_last_ms) <= PROV_REPIN_MS) {
                s_rx_refused++;
                return;
            }
        }

        s_cam_ip[0] = s.ip[0];
        s_cam_ip[1] = s.ip[1];
        s_cam_ip[2] = s.ip[2];
        s_cam_ip[3] = s.ip[3];
        s_cam_rssi = s.rssi;
        s_cam_connected = s.connected;
        memcpy(s_cam_mac, info->src_addr, 6);
        strncpy(s_cam_ssid, s.ssid, sizeof(s_cam_ssid) - 1);
        s_cam_ssid[sizeof(s_cam_ssid) - 1] = '\0';
        s_cam_last_ms = millis();
        // If the CAM reports a different WiFi than the S3, hand it our creds
        // so it re-joins ours. Best-effort here (only lands when both share a
        // channel); the reliable push happens at net_connect, pre-switch.
        if (s.connected && s_ssid[0] != '\0' &&
            strncmp(s.ssid, s_ssid, sizeof(s.ssid)) != 0) {
            send_reply(info->src_addr);
        }
        return;
    }

    if (len != (int)sizeof(ProvMsg)) {
        s_rx_odd++;
        s_rx_odd_len = len;
        return;
    }
    s_rx_prov++;
    ProvMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != PROV_MAGIC || m.type != PROV_REQUEST) {
        return;
    }
    if (s_ssid[0] == '\0') {
        return;  // no creds to hand out yet (user hasn't joined WiFi on the S3)
    }

    // Who is asking. See the note on s_cam_mac: this is the whole protection the
    // reply has, because its contents cannot be protected without changing the CAM.
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    bool pinned = memcmp(s_cam_mac, zero, 6) != 0;
    bool same   = pinned && memcmp(s_cam_mac, info->src_addr, 6) == 0;
    // Silence measured on s_cam_last_ms, which only a StatusMsg from the pinned
    // device advances - so a stranger asking repeatedly cannot keep the pin alive
    // and cannot age it out either.
    bool released = pinned && s_cam_last_ms != 0 &&
                    (millis() - s_cam_last_ms) > PROV_REPIN_MS;
    if (pinned && !same && !released) {
        s_rx_refused++;
        return;
    }
    if (!same) {
        // Pin here and not only on StatusMsg: a factory-fresh CAM asks before it
        // has ever connected, so the status path cannot be what establishes the
        // pin or the first request would always be unpinned and always served.
        memcpy(s_cam_mac, info->src_addr, 6);
        s_cam_last_ms = millis();
    }

    send_reply(info->src_addr);
}

// Bring the ESP-NOW responder up. Safe to call repeatedly: taking the radio to
// WIFI_OFF (which net.cpp does to recover a failed association) stops the WiFi
// driver, and that deinitialises ESP-NOW underneath us - sends then fail with
// ESP_ERR_ESPNOW_NOT_INIT and the CAM never receives its credentials. So this
// re-arms rather than latching on a one-shot flag; net.cpp calls it every time
// the radio comes back to STA.
void camprov_init(void) {
    if (esp_now_init() != ESP_OK) {
        // Already up, or the radio is off. Either way there is nothing to arm:
        // a live ESP-NOW keeps its callback, and a dead radio gets another call
        // from net.cpp once it is back in STA mode.
        return;
    }
    esp_now_register_recv_cb(on_recv);
}

void camprov_push_to_cam(const char *ssid, const char *pass) {
    camprov_set_credentials(ssid, pass);   // cache the new creds first
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    if (memcmp(s_cam_mac, zero, 6) == 0) return;  // never heard from the CAM
    // Send a few times so the reply lands before the S3 leaves this channel.
    for (int i = 0; i < 3; i++) {
        send_reply(s_cam_mac);
        delay(30);
    }
}

bool camprov_cam_online(void) {
    return s_cam_connected && s_cam_last_ms != 0 &&
           (millis() - s_cam_last_ms) < CAM_ONLINE_TIMEOUT_MS;
}

void camprov_cam_ip(char *buf, size_t n) {
    if (camprov_cam_online()) {
        snprintf(buf, n, "%u.%u.%u.%u", s_cam_ip[0], s_cam_ip[1], s_cam_ip[2], s_cam_ip[3]);
    } else {
        snprintf(buf, n, "-");
    }
}

bool camprov_cam_ip4(uint8_t out[4]) {
    if (!camprov_cam_online()) return false;
    out[0] = s_cam_ip[0];
    out[1] = s_cam_ip[1];
    out[2] = s_cam_ip[2];
    out[3] = s_cam_ip[3];
    return (out[0] | out[1] | out[2] | out[3]) != 0;
}

int camprov_cam_rssi(void) {
    return camprov_cam_online() ? s_cam_rssi : 0;
}

void camprov_cam_mac(char *buf, size_t n) {
    if (s_cam_last_ms != 0) {
        snprintf(buf, n, "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_cam_mac[0], s_cam_mac[1], s_cam_mac[2], s_cam_mac[3], s_cam_mac[4], s_cam_mac[5]);
    } else {
        snprintf(buf, n, "-");
    }
}

void camprov_cam_ssid(char *buf, size_t n) {
    if (camprov_cam_online() && s_cam_ssid[0] != '\0') {
        snprintf(buf, n, "%s", s_cam_ssid);
    } else {
        snprintf(buf, n, "-");
    }
}
