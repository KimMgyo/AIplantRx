#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>
#include "net.h"
#include "camprov.h"
#include "hlog.h"

static Preferences s_prefs;
static bool s_enabled = true;

static char s_target_ssid[33] = "";
static char s_pending_pass[65] = "";
static bool s_save_pending = false;
static uint32_t s_connect_start = 0;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;
static char s_target_pass[65] = "";                    // kept for self-managed reconnect
static const uint32_t RECONNECT_INTERVAL_MS = 20000;   // gap between reconnect tries (> timeout so state can show 연결안됨)
// This AP routinely lets the first authentication of a fresh boot expire, and
// the driver takes ~10s to give up on it (reason=2, AUTH_EXPIRE) even at
// -34 dBm. A retry then associates in about a second. So stop waiting on the
// driver's own timeout: give one attempt this long, then tear down and retry.
static const uint32_t ATTEMPT_TIMEOUT_MS = 4000;

static bool s_ntp_started = false;
static bool s_was_connected = false;

static NetScanItem s_items[NET_SCAN_MAX];
static int s_count = 0;
static bool s_fresh = false;
static bool s_scanning = false;
static uint32_t s_scan_retry_at = 0;  // nonzero: start failed, retry from poll
static uint8_t s_scan_attempts = 0;
static uint8_t s_empty_streak = 0;    // consecutive empty scans; avoids flashing "none" mid-connect

// Modem sleep parks the radio between beacons, and anything that arrives while
// it is parked is simply lost — including the sensor node's ESP-NOW telemetry
// and its 7-fragment thermal frames, which go out in a single ~7ms burst and so
// vanish whole. It must be re-disabled here rather than only at init: both
// WiFi.mode() and association reinitialise the driver, and each reinstates the
// IDF default of WIFI_PS_MIN_MODEM.
// The AP the last scan chose: connect_picked() hands these to WiFi.begin() so
// the driver does not get to pick for itself. They are runtime-only - a BSSID
// restored from NVS and dialled without a scan behind it was answered with
// AUTH_FAIL on every boot tried, so there is nothing to persist.
static uint8_t s_ap_bssid[6] = {0};
static uint8_t s_ap_ch = 0;
static bool s_need_scan = false;   // scan is for connecting, not just the list

// Set from the WiFi task when an association drops or fails; consumed by
// net_poll() on the LVGL thread. The disconnects we ask for ourselves (scan,
// network switch) raise the same event, and the echo arrives a few ms later on
// another task - so suppress by deadline rather than by a flag we would clear
// before the echo lands.
static volatile bool s_retry_req = false;
static volatile uint8_t s_last_reason = 0;
// The same code, latched for the screen rather than consumed by the retry. It has
// only ever reached the serial log (net_poll's retry line), so the settings page
// said "연결 끊김" whether the password was wrong, the network was not there, or
// the AP was rebooting - three different things a grower does three different
// things about, and the driver told us which within a second or two. Cleared the
// moment an association succeeds: a reason that outlives its failure is a claim
// about a link that is currently up.
static volatile uint8_t s_fail_reason = 0;
static bool s_connect_pending = false;  // net_init() armed a connect; see net_poll()
static volatile uint32_t s_quiet_until = 0;
static uint8_t s_fail_streak = 0;
// Per-channel scan dwell. 120ms is IDF's own active-scan default; Arduino's
// 300ms costs 3.9s across 13 channels and found nothing extra here. Raising it
// on a missed target was tried and is a trap: one miss made every later sweep
// 5.2s and turned a steady 8s boot into 8-70s. One dwell, always.
static const uint32_t SCAN_DWELL_MS = 120;
static uint8_t s_scan_miss = 0;    // connect scans in a row that missed the target

// Uplink liveness watchdog. See net.h. s_last_uplink_ok is stamped on connect
// (a grace window) and on every LAN success; s_uplink_fail_votes counts connect
// timeouts and is cleared by any success. The radio is cycled only when BOTH
// fire: several failures in a row (a healthy link's connects take milliseconds,
// so it never votes) AND enough time with nothing getting through (so one
// transient timeout on an otherwise-live link does nothing). UPLINK_DEAD_MS sits
// above the default 60s poll but below plantrx's 300s ceiling, which is why the
// vote gate carries the correctness: a camera-off panel on a slow poll stays
// quiet without voting, so silence alone never cycles it.
static const uint32_t UPLINK_DEAD_MS = 30000;
static const uint8_t  UPLINK_FAIL_VOTES = 4;
static volatile uint32_t s_last_uplink_ok = 0;
static volatile uint8_t  s_uplink_fail_votes = 0;

// After a mode(OFF)->mode(STA) driver re-init, do NOT scan immediately: an
// esp_wifi_scan_start() issued right after esp_wifi_start() crashed the WiFi task
// in the coex timer setup (ets_timer_setfn ESP_ERROR_CHECK failure, observed 5x
// in the field - see the crash log). Let the driver and its coex arbiter settle
// first, then begin_target() (which scans) fires from net_poll.
static const uint32_t RADIO_SETTLE_MS = 400;
static uint32_t s_settle_until = 0;
static bool s_scan_after_settle = false;

void net_note_uplink(void) {
    s_last_uplink_ok = millis();
    s_uplink_fail_votes = 0;
}

void net_note_uplink_fail(void) {
    if (s_uplink_fail_votes < 255) s_uplink_fail_votes++;
}

// Disconnect on purpose, without arming a retry.
static void quiet_disconnect(bool wifi_off) {
    s_quiet_until = millis() + 400;
    WiFi.disconnect(wifi_off);
}

static void scan_try_start(void);

static void begin_target(const char *ssid, const char *pass) {
    strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
    s_target_ssid[sizeof(s_target_ssid) - 1] = '\0';
    strncpy(s_target_pass, pass ? pass : "", sizeof(s_target_pass) - 1);
    s_target_pass[sizeof(s_target_pass) - 1] = '\0';
    s_connect_start = millis();
    // Always find the AP with a scan before connecting. A begin() carrying a
    // BSSID with no scan behind it was answered with AUTH_FAIL on every boot
    // tried; the same begin() straight after a scan associates in ~600ms. So
    // the AP is chosen from live scan results, never from NVS.
    s_need_scan = true;
    scan_try_start();
}

// Connect without a scan behind us: hand the SSID to the driver and let it find
// the AP itself. Slower and it picks the BSSID for us, but it is the only path
// that still works when our own scan cannot see the target - a hidden SSID, or
// one too weak or too intermittent to answer a probe. Never the first choice,
// always the last one.
static void connect_blind(void) {
    s_connect_start = millis();
    s_need_scan = false;
    hlogf("[net] connect (driver search)" "\n");
    WiFi.begin(s_target_ssid, s_target_pass[0] ? s_target_pass : NULL);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_15dBm);  // below the 20dBm default: spare the RGB panel's shared supply a droop on each TX burst
}

// Dial the AP the scan just chose.
static void connect_picked(void) {
    s_connect_start = millis();
    hlogf("[net] connect ch%u\n", (unsigned)s_ap_ch);
    // Aim at the BSSID the scan chose rather than letting the driver pick: this
    // SSID is carried by several APs and the driver's own choice was the one
    // thing that could send us to a weaker one.
    //
    // sae_pwe_h2e is stated rather than left at Arduino's zero-filled
    // UNSPECIFIED because the AP runs WPA2/WPA3 transition mode (scan reports
    // authmode 7). Measured: it makes no difference to the first-attempt
    // failure below, but leaving SAE parameters unstated against a mixed-mode
    // AP is not something to rely on either.
    WiFi.begin(s_target_ssid, s_target_pass[0] ? s_target_pass : NULL,
               s_ap_ch, s_ap_bssid, false /*connect*/);
    wifi_config_t conf = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
        conf.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        esp_wifi_set_config(WIFI_IF_STA, &conf);
    }
    esp_wifi_connect();
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_15dBm);  // below the 20dBm default: spare the RGB panel's shared supply a droop on each TX burst
}

void net_init(void) {
    s_prefs.begin("net", false);
    s_enabled = s_prefs.getBool("en", true);

    WiFi.persistent(false);  // we manage creds in our own NVS namespace
    WiFi.setAutoReconnect(false);  // retry manually so a stuck connect never blocks WiFi scans

    // A failed association is only visible here. WiFi.status() just says
    // WL_DISCONNECTED, so waiting on it meant waiting out CONNECT_TIMEOUT_MS and
    // then RECONNECT_INTERVAL_MS - 20s of dead air - when the driver already knew
    // within a second or two. Record it and let net_poll() retry on the next tick.
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
        s_last_reason = info.wifi_sta_disconnected.reason;
        // The display copy skips the disconnects we asked for. A scan and a network
        // switch both raise this event, and reporting "인증 실패" because the user
        // pressed 검색 would be the loudest possible wrong answer. Same deadline the
        // retry suppression uses, for the same reason.
        if ((int32_t)(millis() - s_quiet_until) >= 0) {
            s_retry_req = true;
            s_fail_reason = info.wifi_sta_disconnected.reason;
        }
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    if (!s_enabled) {
        WiFi.mode(WIFI_OFF);
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // after mode(): mode() starts the driver at the IDF default
    WiFi.setTxPower(WIFI_POWER_15dBm);  // below the 20dBm default: spare the RGB panel's shared supply a droop on each TX burst
    // Keep the 802.11b rate set ENABLED, explicitly. The flag survives a reboot,
    // and a firmware that experimented with disabling it left this board unable to
    // associate at all - auth/assoc frames go out at the AP's basic rate set,
    // which on a 2.4GHz b/g/n AP is 11b. State it rather than inherit it.
    esp_wifi_config_11b_rate(WIFI_IF_STA, false);
    String ssid = s_prefs.getString("ssid", "");
    if (ssid.length()) {
        String pass = s_prefs.getString("pass", "");
        camprov_set_credentials(ssid.c_str(), pass.c_str());  // let the CAM provision from stored creds
        strncpy(s_target_ssid, ssid.c_str(), sizeof(s_target_ssid) - 1);
        s_target_ssid[sizeof(s_target_ssid) - 1] = '\0';
        strncpy(s_target_pass, pass.c_str(), sizeof(s_target_pass) - 1);
        s_target_pass[sizeof(s_target_pass) - 1] = '\0';
        s_connect_start = millis();
        // Arm the connect, don't start it. net_init() runs at ~1.6s, in the
        // middle of board/LVGL/ESP-NOW bring-up, and a WiFi.begin() issued
        // there died silently every time - no association, not even a failure
        // event. Started from the first net_poll() tick instead, it behaves.
        s_connect_pending = true;
    }
}

void net_set_enabled(bool en) {
    if (en == s_enabled) return;
    s_enabled = en;
    s_prefs.putBool("en", en);
    if (en) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        esp_wifi_config_11b_rate(WIFI_IF_STA, false);  // restated: mode() reset the driver to IDF defaults
        camprov_init();  // radio is on now — start the responder if boot skipped it
        String ssid = s_prefs.getString("ssid", "");
        if (ssid.length()) {
            String pass = s_prefs.getString("pass", "");
            camprov_set_credentials(ssid.c_str(), pass.c_str());  // so the CAM can provision too
            strncpy(s_target_ssid, ssid.c_str(), sizeof(s_target_ssid) - 1);
            s_target_ssid[sizeof(s_target_ssid) - 1] = '\0';
            strncpy(s_target_pass, pass.c_str(), sizeof(s_target_pass) - 1);
            s_target_pass[sizeof(s_target_pass) - 1] = '\0';
            s_connect_start = millis();
            // Scan after the driver settles, not now — see the connect_pending note in net_poll.
            s_settle_until = millis() + RADIO_SETTLE_MS;
            s_scan_after_settle = true;
        }
    } else {
        s_scanning = false;
        s_scan_retry_at = 0;
        s_target_ssid[0] = '\0';
        quiet_disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
}

bool net_enabled(void) { return s_enabled; }

NetState net_state(void) {
    if (!s_enabled) return NET_OFF;
    if (WiFi.status() == WL_CONNECTED) return NET_CONNECTED;
    if (s_target_ssid[0] != '\0' && millis() - s_connect_start < CONNECT_TIMEOUT_MS) return NET_CONNECTING;
    return NET_DISCONNECTED;
}

int net_rssi(void) { return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0; }

static int rssi_to_bars(int rssi) {
    if (rssi > -55) return 4;
    if (rssi > -65) return 3;
    if (rssi > -75) return 2;
    return 1;
}

int net_strength(void) {
    if (WiFi.status() != WL_CONNECTED) return 0;
    return rssi_to_bars(WiFi.RSSI());
}

const char *net_ssid(void) {
    static char buf[33];
    if (WiFi.status() == WL_CONNECTED) {
        strncpy(buf, WiFi.SSID().c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
    return s_target_ssid;
}

void net_ip(char *buf, size_t n) {
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, n, "%s", WiFi.localIP().toString().c_str());
    } else {
        snprintf(buf, n, "-");
    }
}

void net_mac(char *buf, size_t n) {
    snprintf(buf, n, "%s", WiFi.macAddress().c_str());  // stable regardless of link state
}

bool net_time_valid(void) { return time(NULL) > 1700000000; /* 2023-11 */ }

static void scan_try_start(void) {
    // scanNetworks() fails with WIFI_SCAN_FAILED while the STA is busy
    // (e.g. mid-connect); retry from net_poll instead of hanging the UI.
    // A pending STA connection (auto-reconnect to a saved network that isn't
    // here, e.g. after moving locations) keeps the radio busy, so the async scan
    // aborts with WIFI_SCAN_FAILED and no networks ever show. Free it first.
    if (WiFi.status() != WL_CONNECTED) {
        quiet_disconnect(false);
    }
    // One sweep serves both the settings list and the connect. Arduino's default
    // dwell is 300ms per channel, which is 3.9s across all 13 and the single
    // largest part of boot-to-online; IDF's own active-scan default is 120ms and
    // finds the same APs here. The sweep takes the radio off our home channel
    // for its whole duration, stalling the camera stream - that is what
    // net_scan_abort() is for.
    int16_t r = WiFi.scanNetworks(true /*async*/, false /*hidden*/, false /*passive*/,
                                  SCAN_DWELL_MS);
    if (r == WIFI_SCAN_RUNNING) {
        s_scanning = true;
        s_scan_retry_at = 0;
    } else if (++s_scan_attempts >= 6) {
        s_scan_retry_at = 0;    // give up; publish whatever we have
        s_fresh = true;
    } else {
        s_scan_retry_at = millis() + 1500;
    }
}

void net_scan_start(void) {
    if (!s_enabled || s_scanning || s_scan_retry_at != 0) return;
    s_scan_attempts = 0;
    scan_try_start();
}

// Give the radio back to the camera stream immediately. A full scan parks the
// STA off-channel for close to four seconds, which is longer than the camera's
// liveness window - leave the settings page mid-scan and the monitor page came
// up on the "disconnected" placeholder instead of live video. The results
// already collected are kept, so the network list does not blank out either.
void net_scan_abort(void) {
    if (!s_scanning && s_scan_retry_at == 0) return;
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    s_scanning = false;
    s_scan_retry_at = 0;
    s_scan_attempts = 0;
}

bool net_scanning(void) { return s_scanning || s_scan_retry_at != 0; }
bool net_scan_fresh(void) { return s_fresh; }
void net_scan_clear_fresh(void) { s_fresh = false; }
int net_scan_count(void) { return s_count; }

const NetScanItem *net_scan_item(int i) {
    if (i < 0 || i >= s_count) return NULL;
    return &s_items[i];
}

void net_connect(const char *ssid, const char *pass) {
    if (!s_enabled || ssid == NULL || ssid[0] == '\0') return;
    strncpy(s_pending_pass, pass ? pass : "", sizeof(s_pending_pass) - 1);
    s_pending_pass[sizeof(s_pending_pass) - 1] = '\0';
    s_save_pending = true;
    s_fail_streak = 0;  // a fresh request must not inherit the old network's
    s_scan_miss = 0;    // backoff and blind-fallback progress
    s_fail_reason = 0;  // nor the old network's verdict: a password just retyped
                        // has not failed yet, and net_fail() gates on the streak
                        // anyway, so this only stops a stale code surviving into
                        // the third failure of a different SSID.
    // Hand the new creds to both ESP-NOW boards while we're still on the old
    // network's channel — once we disconnect below, ESP-NOW can't reach a peer on a
    // different channel until it re-provisions on its own.
    //
    // The sensor node is here too now, and it is the same one action: it needs
    // credentials to download its own firmware (nodeota.h), and a grower who typed the
    // new password once should not have to discover that one of two invisible boards
    // kept the old one. Costs another ~90ms of pre-switch window on top of the CAM's,
    // spent on a press that is about to drop the link anyway.
    camprov_push_to_cam(ssid, s_pending_pass);
    camprov_push_to_node(ssid, s_pending_pass);
    quiet_disconnect(false);
    begin_target(ssid, s_pending_pass);
}

// Silent until three consecutive failures, and the reason is the note inside
// net_poll(): this AP rejects the FIRST association of every boot with
// AUTH_FAIL(202) on a correct password, then associates on the retry. A screen that
// trusted attempt #1 would accuse the grower's password on every single boot - the
// loudest possible wrong answer, on the one board we have measured. Three clears
// that known one-shot with margin and still answers within a few seconds of a
// password that really is wrong, because the quick tier retries fast.
NetFail net_fail(void) {
    if (WiFi.status() == WL_CONNECTED || s_fail_streak < 3) return NET_FAIL_NONE;
    switch (s_fail_reason) {
    case 0:
        return NET_FAIL_NONE;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return NET_FAIL_NOT_FOUND;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        // The four the supplicant raises when the PSK does not verify. ASSOC_FAIL
        // and CONNECTION_FAIL are deliberately NOT here: they are generic, and
        // naming a password on them would be a guess dressed as a diagnosis.
        return NET_FAIL_AUTH;
    default:
        return NET_FAIL_OTHER;
    }
}

uint8_t net_fail_code(void) { return s_fail_reason; }

void net_poll(void) {
    bool now_connected = (WiFi.status() == WL_CONNECTED);

    if (now_connected && !s_was_connected) {
        hlogf("[net] online after %lums, ch%d %ddBm\n",
                      (unsigned long)millis(), WiFi.channel(), (int)WiFi.RSSI());
        s_fail_streak = 0;
        // The failure is over, so the word describing it must go with it. Leaving it
        // set would have the settings page explaining a password while the link it
        // supposedly broke is carrying telemetry.
        s_fail_reason = 0;
        // Give the fresh link a full window before the uplink watchdog can judge
        // it: association is up, but DHCP/ARP and the first camnet/plantrx round
        // trips have not happened yet, and an unstamped clock would read as dead.
        s_last_uplink_ok = millis();
        s_uplink_fail_votes = 0;

        if (!s_ntp_started) {
            // KST (UTC+9); SNTP keeps re-syncing in the background.
            configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com");
            s_ntp_started = true;
        }
        if (s_save_pending) {
            s_prefs.putString("ssid", s_target_ssid);
            s_prefs.putString("pass", s_pending_pass);
            camprov_set_credentials(s_target_ssid, s_pending_pass);  // push new creds to the CAM
            s_save_pending = false;
        }
    }
    s_was_connected = now_connected;

    // Associated but not reachable. WiFi.status() stays WL_CONNECTED through the
    // "connected but no data" state this driver falls into after some roams and
    // scans: the association is nominally up and ESP-NOW still carries the sensor
    // node, so the settings page shows the CAM online - but every outbound TCP to
    // the LAN times out, so the dashboard's camera card goes offline and a pull or
    // an update-button press reports a server it cannot reach. The reconnect below
    // never fires because it is gated on a LOST association, which this is not; by
    // hand, only a reboot cleared it. So when the server poll has timed out several
    // times in a row and nothing from any LAN peer has landed since we came up, put
    // the link through the same radio teardown a dropped association uses - the one
    // thing measured to recover it - instead of stranding the panel until a power
    // cycle. Only the server votes it dead (a camera-only outage must not cycle a
    // working link, see net.h), but any peer's success clears it.
    if (now_connected && s_uplink_fail_votes >= UPLINK_FAIL_VOTES &&
        millis() - s_last_uplink_ok > UPLINK_DEAD_MS &&
        !s_scanning && s_scan_retry_at == 0 && !s_connect_pending) {
        hlogf("[net] uplink dead %lums, %u fails while associated - cycling radio\n",
                      (unsigned long)(millis() - s_last_uplink_ok),
                      (unsigned)s_uplink_fail_votes);
        s_uplink_fail_votes = 0;
        s_last_uplink_ok = millis();       // don't re-fire before the new link's window
        s_quiet_until = millis() + 1200;   // the mode change raises its own event
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        s_connect_pending = true;          // next tick brings it back and retries
        s_was_connected = false;           // so the reconnect logs and re-inits
        return;
    }

    // This AP answers the first association of every boot with AUTH_FAIL(202),
    // deterministically - at -30 dBm, with the right password, whether or not
    // the BSSID is pinned, and even after the board has been held off for 75s.
    // A second attempt behind a fresh scan then associates in ~600ms. It runs
    // WPA2/WPA3 transition mode (scan reports authmode 7), which is the only
    // unusual thing about it; the IDF libs ship with supplicant logging
    // compiled out, so the handshake itself is not visible from here.
    //
    // So: treat the first failure as expected and rescan-and-retry promptly.
    // Repeating the identical call is what does not work - the original code
    // did that and failed 19 times in a row without ever recovering.
    bool failed = s_retry_req;
    if (failed) s_retry_req = false;
    if (s_connect_pending) {
        s_connect_pending = false;
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        esp_wifi_config_11b_rate(WIFI_IF_STA, false);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        // Stopping the driver deinitialises ESP-NOW; without this the CAM's
        // provisioning requests are heard but never answered, and the camera
        // card sits on "offline" while the camera is in fact fine.
        camprov_init();
        // Do NOT scan yet - that is what crashes the coex layer here. Arm the
        // settle timer; the block below scans once the driver has settled.
        s_settle_until = millis() + RADIO_SETTLE_MS;
        s_scan_after_settle = true;
        return;
    }
    if (s_scan_after_settle) {
        if ((int32_t)(millis() - s_settle_until) < 0) return;  // still settling
        s_scan_after_settle = false;
        begin_target(s_target_ssid, s_target_pass);
        return;
    }
    if (s_enabled && !now_connected && !s_scanning && s_scan_retry_at == 0 &&
        s_target_ssid[0]) {
        // A failure is a reason to retry, not a licence to retry now. `failed` used
        // to short-circuit `wait` outright, which made straight failures the ONE
        // case that never backed off - the exact opposite of what this comment
        // claimed, and measurably worse than a stall: 13 attempts in 45s, ~20 per
        // minute, forever. That is the shape a router's brute-force protection
        // reacts to, and it holds the radio in a permanent scan/connect/mode-cycle
        // loop that camnet and ESP-NOW have to share.
        //
        // The quick tier stays quick, because that is what clears the documented
        // first-attempt rejection above and what makes a normal boot fast. Past it,
        // a failure waits like everything else: 4 fast tries, then 3 a minute.
        uint32_t wait = s_fail_streak < 4 ? ATTEMPT_TIMEOUT_MS : RECONNECT_INTERVAL_MS;
        bool quick = s_fail_streak < 4;
        if ((failed && quick) || millis() - s_connect_start > wait) {
            // "paced" and not "stalled": past the quick tier the driver's failure
            // event still arrives promptly, it just no longer buys an immediate
            // retry, so the attempt that follows was triggered by the timer while
            // the attempt before it did fail. Calling that a stall would report a
            // silent AP when the reason code beside it says otherwise.
            hlogf("[net] retry: %s reason=%u (try %u)\n",
                          failed ? "failed" : (s_fail_reason ? "paced" : "stalled"),
                          (unsigned)s_last_reason, (unsigned)s_fail_streak + 1);
            if (s_fail_streak < 255) s_fail_streak++;
            // Rescanning alone is measurably worse than rescanning behind a
            // radio cycle: 5-50s and erratic, against a steady ~8s. Whatever
            // the driver carries over from a failed attempt, restarting it is
            // what clears it.
            s_quiet_until = millis() + 1200;   // the mode change raises its own event
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            s_connect_pending = true;          // next tick brings it back and retries
        }
    }

    if (s_scan_retry_at != 0 && !s_scanning && millis() >= s_scan_retry_at) {
        scan_try_start();
    }

    if (s_scanning) {
        int n = WiFi.scanComplete();
        if (n > 0) {
            int best_rssi = -32768, best_i = -1;
            s_count = 0;
            for (int i = 0; i < n; i++) {
                String ssid = WiFi.SSID(i);
                if (!ssid.length()) continue;
                // While we're here: the strongest BSSID advertising the network
                // we want to join. A blind WiFi.begin(ssid, pass) leaves that
                // choice to the driver, and on this multi-AP SSID that answered
                // with AUTH_FAIL and then refused every retry. Choosing the AP
                // ourselves turns it into one attempt that works.
                if (s_need_scan && strcmp(ssid.c_str(), s_target_ssid) == 0 &&
                    WiFi.RSSI(i) > best_rssi) {
                    best_rssi = WiFi.RSSI(i);
                    best_i = i;
                }
                if (s_count >= NET_SCAN_MAX) continue;
                bool dup = false;
                for (int k = 0; k < s_count; k++) {
                    if (strcmp(s_items[k].ssid, ssid.c_str()) == 0) { dup = true; break; }
                }
                if (dup) continue;  // scan results are RSSI-sorted; keep the strongest
                strncpy(s_items[s_count].ssid, ssid.c_str(), sizeof(s_items[0].ssid) - 1);
                s_items[s_count].ssid[sizeof(s_items[0].ssid) - 1] = '\0';
                s_items[s_count].rssi = WiFi.RSSI(i);
                s_items[s_count].secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                s_count++;
            }
            bool picked = false;
            if (best_i >= 0) {
                const uint8_t *b = WiFi.BSSID(best_i);
                int32_t ch = WiFi.channel(best_i);
                if (b != NULL && ch >= 1 && ch <= 14) {
                    memcpy(s_ap_bssid, b, sizeof(s_ap_bssid));
                    s_ap_ch = (uint8_t)ch;
                    s_need_scan = false;
                    picked = true;
                }
            }
            bool wanted = s_need_scan;   // this sweep was looking for an AP to join
            WiFi.scanDelete();
            s_scanning = false;
            s_empty_streak = 0;
            s_fresh = true;
            if (picked) {
                s_scan_miss = 0;
                connect_picked();
            } else if (wanted) {
                // The AP we were told to join answered the list scan but not
                // this one. Keep sweeping - that is nearly always transient -
                // but do not depend on our scan forever: after enough misses
                // hand the SSID to the driver, which searches its own way and
                // can reach an AP our probes cannot. Deliberately a high bar,
                // because touching the healthy path costs far more than the
                // rare network this rescues.
                if (++s_scan_miss >= 10) {
                    s_scan_miss = 0;
                    connect_blind();
                }
            }
        } else if (n == 0) {
            WiFi.scanDelete();
            s_scanning = false;
            if (++s_empty_streak >= 2) {
                // Completed but empty. A busy radio (mid-connect) returns
                // spurious empties; keep the last list and only declare "no
                // networks" once two in a row confirm it - no flashing.
                s_count = 0;
                s_fresh = true;
            }
            // An empty result is NOT evidence the AP is gone: a scan started
            // right after the radio comes back routinely completes with nothing
            // because the driver is not ready yet. Counting those as misses
            // pushed the patient dwell and the blind fallback on to a perfectly
            // healthy AP and turned an 8s boot into 70s. Let the stall timeout
            // cycle and rescan instead.
        } else if (n == WIFI_SCAN_FAILED) {
            // The driver aborted the scan (busy STA); go through the retry path.
            s_scanning = false;
            if (++s_scan_attempts >= 6) {
                s_fresh = true;
            } else {
                s_scan_retry_at = millis() + 1500;
            }
        }
    }
}
