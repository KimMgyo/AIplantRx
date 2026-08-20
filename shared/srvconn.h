// One socket to the server, encrypted or not, decided by the stored URL's scheme.
//
// WHY A HOLDER AND NOT A TYPEDEF. Every caller in all three firmwares hand-writes HTTP over a
// WiFiClient& - write_get_head(), read_head(), read_body() and their siblings. WiFiClientSecure
// derives from WiFiClient, so none of those helpers has to change or even know: this hands back a
// WiFiClient* that happens to be one or the other. The alternative was six call sites each
// declaring both types on the stack and branching, which is where the two paths drift apart.
//
// ENCRYPTION, NOT AUTHENTICATION, AND THAT IS A DECISION.
//
// setInsecure() means the certificate is not checked. What this buys is the whole point: the
// bearer token, the telemetry and the firmware image stop crossing the public internet in
// cleartext, where any on-path observer could read the token and then push arbitrary firmware to
// every board in the greenhouse. What it does not buy is protection from an attacker who can
// actively intercept and answer in the server's place - and that attacker can already do exactly
// that today, over plain HTTP, so this is strictly an improvement rather than a false promise.
//
// The alternative was a pinned root, and it was rejected on availability. The server is behind
// Cloudflare, whose leaf is currently a Let's Encrypt cert and whose issuer is theirs to change
// without telling anybody. A board with a pinned root that stops matching cannot reach the server;
// the server is where firmware comes from; so the only repair for a rotation nobody caused would
// be a USB cable and a walk to whichever pole the board is bolted to. Verification that can brick
// the repair channel is worth less than the attack it prevents.
//
// If that trade ever stops being the right one, the honest fix is not a pinned root with an
// insecure fallback - an active attacker just fails the verification and takes the fallback. It is
// a bundle of roots plus an operator who is willing to be locked out.
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdint.h>

#include "srvurl.h"

// STATUS: THE TLS BRANCH IS BUILT AND DOES NOT YET WORK ON THE PANEL. Do not flip a stored URL to
// https:// expecting it to. What was measured, on an ESP32-S3-Touch-LCD-7 against Cloudflare:
//
//   * srvurl_parse() and everything below are proven on the plain branch - the same code path,
//     with tls=0, writes all 862 bytes of a telemetry poll and reads a 200.
//   * With tls=1 the handshake REPORTS success (connect() returns non-zero), then the first write
//     puts zero bytes on the wire, connected() is already false, and mbedtls's own last_error
//     alternates between "CTR_DRBG - The entropy source failed" and 0x0032.
//   * It is not the loop task's stack. 8KB was the first suspect - the default, and an mbedtls
//     handshake wants most of it - but SET_LOOP_TASK_STACK_SIZE(16 * 1024) in src/main.cpp changed
//     nothing. That line stays: the margin is right whatever this turns out to be.
//   * HTTP/1.0 over TLS is not the problem either. The identical request, byte for byte, gets a
//     200 from the same host through openssl s_client.
//
// The RNG is the standing suspect, and not only because mbedtls names it: this same board took a
// task-watchdog reset inside esp_fill_random() under WPA3 SAE (sae_derive_pwe_ecc) earlier in its
// life. Two independent crypto paths failing in the entropy source is one fault, not two. The next
// step is to read esp_random() directly on this board and to try it with WiFi on WPA2 rather than
// WPA3, before touching anything in this file.

class SrvConn {
public:
    SrvConn() : m_c(nullptr) {}
    ~SrvConn() { stop(); }

    SrvConn(const SrvConn &) = delete;
    SrvConn &operator=(const SrvConn &) = delete;

    // Connect to `u`, and hand back the client to write the request on - or nullptr, which the
    // callers already treat exactly as a failed connect() because that is what it replaced.
    //
    // Allocated rather than held as both members: WiFiClientSecure carries an mbedtls context, and
    // a plain-HTTP board should not pay for one on the stack of every poll. Freed by stop(), which
    // the destructor calls, so an early return anywhere in a caller cannot leak the session.
    //
    // THE CONNECT HAPPENS ON THE DERIVED POINTER, AND IT HAS TO.
    // NetworkClient::connect(const char *, uint16_t, int32_t) is NOT virtual - it hides the pure
    // virtual of the same signature on Client (Network/src/NetworkClient.h:34 against :55), and
    // NetworkClientSecure declares its own. So calling it through a WiFiClient* binds at compile
    // time to the PLAIN implementation: the socket opens to port 443, no handshake ever runs, the
    // first write leaves as cleartext, and the far end closes it. That failed silently for an
    // evening - "no-reply" with a plausible round trip time, and lastError() reading a stale
    // sslclient->last_error because nothing had ever set it - so it is written down here rather
    // than rediscovered.
    WiFiClient *connect(const struct SrvUrl &u, uint32_t timeout_ms) {
        stop();
        m_tls = false;
        if (!u.host[0]) return nullptr;
        if (u.tls) {
            WiFiClientSecure *s = new WiFiClientSecure();
            if (s == nullptr) return nullptr;
            s->setInsecure();          // see the block at the top of this file
            // Seconds here, milliseconds below: setTimeout() is Stream's and counts seconds,
            // while connect()'s third argument is the handshake's own budget in milliseconds -
            // NetworkClientSecure.cpp hands it straight to start_ssl_client().
            s->setTimeout(timeout_ms / 1000 + 1);
            if (!s->connect(u.host, u.port, (int32_t)timeout_ms)) {
                delete s;
                return nullptr;
            }
            m_c = s;
            m_tls = true;
            return m_c;
        }
        WiFiClient *p = new WiFiClient();
        if (p == nullptr) return nullptr;
        if (!p->connect(u.host, u.port, (int32_t)timeout_ms)) {
            delete p;
            return nullptr;
        }
        m_c = p;
        return m_c;
    }

    void stop() {
        if (m_c == nullptr) return;
        m_c->stop();
        delete m_c;
        m_c = nullptr;
    }


private:
    WiFiClient *m_c;
    bool m_tls = false;
};
