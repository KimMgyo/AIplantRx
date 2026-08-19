#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "provision.h"
#include "nodeagent.h"
#include "nodeproto.h"

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static volatile bool s_got_reply = false;
static ProvMsg s_reply;

// XOR is symmetric, so the same routine obfuscates and de-obfuscates.
static void xor_field(char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[i] ^= PROV_KEY[i % sizeof(PROV_KEY)];
    }
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    // ONE recv callback exists on this board. esp_now_register_recv_cb() replaces rather than
    // chains, so every family the CAM listens to is told apart here, by payload length - the same
    // rule the panel's camprov.cpp uses, and the reason shared/nodeproto.h lists every length
    // already taken (6/44/64/103/242, plus its own 192 and 232). A second registration anywhere
    // in this firmware would silently unhook provisioning, and the symptom would read as "the S3
    // stopped answering", which is the last place anybody would look.
    if (len == (int)sizeof(NodeCmdMsg)) {
        nodeagent_on_cmd(info != nullptr ? info->src_addr : nullptr, data, len);
        return;
    }
    (void)info;
    if (len != (int)sizeof(ProvMsg)) {
        return;
    }
    ProvMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != PROV_MAGIC || m.type != PROV_REPLY) {
        return;
    }
    s_reply = m;
    s_got_reply = true;
}

static bool wait_connected(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        if (WiFi.status() == WL_CONNECTED) {
            // Associating reinstates modem sleep, whatever setup() asked for. The
            // S3 already learned this (see thermal.cpp's ps= reporting); the CAM
            // had the call only before WiFi.begin, where the association promptly
            // undid it.
            WiFi.setSleep(false);
            return true;
        }
        delay(200);
    }
    return false;
}

static void save_creds(const char *ssid, const char *pass) {
    Preferences p;
    p.begin("cam", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
}

// ESP-NOW does not survive the WiFi driver stopping, and try_connect() stops it
// on purpose. Re-arm instead of leaving the provisioning channel dead - the S3
// lost its camera link to exactly this oversight. The peer list is wiped along
// with everything else, so the broadcast peer has to come back too or the
// status beacon silently fails and the S3 shows the camera as offline.
// esp_now_init() failing here means it is already up, which is equally fine.
static void espnow_rearm(void) {
    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(on_recv);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;  // 0 = whatever channel we are currently on
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

// Associate, cycling the radio between tries. This AP lets the first
// authentication after boot expire and then refuses every identical retry
// (reason 202, then 2 - measured on the S3 against this same SSID, where a
// plain retry loop failed 19 times in a row). Taking the radio down to
// WIFI_OFF and back is what clears it.
static bool try_connect(const char *ssid, const char *pass, int attempts) {
    for (int i = 0; i < attempts; i++) {
        WiFi.begin(ssid, (pass && pass[0]) ? pass : NULL);
        // A working association completes in well under a second here; the old
        // 20s wait just made a doomed attempt expensive. Cut it short and spend
        // the time on a cycled retry, which is what actually succeeds.
        if (wait_connected(4000)) return true;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(150);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        espnow_rearm();
    }
    return false;
}

// Block until WiFi is connected, using NVS creds first and, failing that, the
// channel-hopping ESP-NOW handshake with the S3.
static void acquire_wifi(void) {
    // 1. Try creds already in NVS from a previous provisioning.
    {
        Preferences p;
        p.begin("cam", true);
        String ssid = p.getString("ssid", "");
        String pass = p.getString("pass", "");
        p.end();
        if (ssid.length() && try_connect(ssid.c_str(), pass.c_str(), 4)) {
            return;
        }
        // Keep them either way. A failed association is not evidence that the
        // password is wrong - on this AP most first attempts fail - and wiping
        // the creds strands the CAM off the network entirely unless the S3 is
        // up to re-provision it. That is how this board ended up with an empty
        // NVS and the UI stuck on "카메라 노드 오프라인". Provisioning below
        // overwrites them if they really have changed.
    }

    // 2. Hop channels asking the S3 for creds until one connects.
    ProvMsg req = {};
    req.magic = PROV_MAGIC;
    req.type = PROV_REQUEST;
    for (;;) {
        for (uint8_t ch = 1; ch <= 13; ch++) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            s_got_reply = false;
            esp_now_send(BROADCAST, (const uint8_t *)&req, sizeof(req));

            uint32_t t = millis();
            while (millis() - t < 200 && !s_got_reply) {
                delay(10);
            }
            if (!s_got_reply) {
                continue;
            }

            ProvMsg m = s_reply;
            xor_field(m.ssid, sizeof(m.ssid));
            xor_field(m.pass, sizeof(m.pass));
            m.ssid[sizeof(m.ssid) - 1] = '\0';
            m.pass[sizeof(m.pass) - 1] = '\0';

            if (try_connect(m.ssid, m.pass, 2)) {
                save_creds(m.ssid, m.pass);  // only once they are known to work
                return;
            }
            // Reply connected us nowhere; keep hopping and ask again.
        }
        delay(500);
    }
}

static void provision_task(void *arg) {
    (void)arg;
    acquire_wifi();

    // Online: beacon our IP to the S3 every few seconds so its settings page
    // can show where to reach the camera. Same channel as the AP == the S3's.
    StatusMsg st = {};
    st.magic = PROV_MAGIC;
    st.type = PROV_STATUS;
    uint32_t disc_since = 0;  // millis when the link first dropped (0 = up)
    for (;;) {
        // The S3 pushes fresh creds (unsolicited PROV_REPLY) when it moves to a
        // new WiFi. Adopt them and re-join so the CAM follows the S3's network,
        // even while currently connected to a different one.
        if (s_got_reply) {
            s_got_reply = false;
            ProvMsg m = s_reply;
            xor_field(m.ssid, sizeof(m.ssid));
            xor_field(m.pass, sizeof(m.pass));
            m.ssid[sizeof(m.ssid) - 1] = '\0';
            m.pass[sizeof(m.pass) - 1] = '\0';
            if (m.ssid[0] && WiFi.SSID() != String(m.ssid)) {
                // Same rules as acquire_wifi(): cycle the radio between tries,
                // and only commit the creds once they have actually worked. A
                // single blind attempt here would fail most of the time on this
                // AP and leave the CAM off the network with unverified creds
                // written over the ones that were fine.
                WiFi.disconnect();
                if (try_connect(m.ssid, m.pass, 3)) {
                    save_creds(m.ssid, m.pass);
                }
                continue;
            }
        }
        bool conn = (WiFi.status() == WL_CONNECTED);
        st.connected = conn ? 1 : 0;
        if (conn) {
            disc_since = 0;
            IPAddress ip = WiFi.localIP();
            st.ip[0] = ip[0];
            st.ip[1] = ip[1];
            st.ip[2] = ip[2];
            st.ip[3] = ip[3];
            st.rssi = (int8_t)WiFi.RSSI();
            strncpy(st.ssid, WiFi.SSID().c_str(), sizeof(st.ssid) - 1);
            st.ssid[sizeof(st.ssid) - 1] = '\0';
        } else {
            memset(st.ip, 0, sizeof(st.ip));
            st.rssi = 0;
            st.ssid[0] = '\0';
            // Down for good (AP gone / password changed): auto-reconnect won't
            // recover a credential change, so re-provision fresh creds from the
            // S3 over ESP-NOW instead of staying dark until a reboot.
            if (disc_since == 0) {
                disc_since = millis();
            } else if (millis() - disc_since > 30000) {
                acquire_wifi();
                disc_since = 0;
                continue;
            }
        }
        esp_now_send(BROADCAST, (const uint8_t *)&st, sizeof(st));
        delay(3000);
    }
}

void provision_start(void) {
    espnow_rearm();
    // Start the task even if that failed: it re-arms after every radio cycle,
    // so a radio that was not ready yet is not a permanent loss.
    xTaskCreatePinnedToCore(provision_task, "prov", 4096, NULL, 1, NULL, 0);
}
