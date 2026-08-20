// The server's address, parsed once for all three firmwares.
//
// WHY THIS FILE EXISTS. There were four byte-identical copies of this parser - src/plantrx.cpp,
// src/fwpull.cpp, sensor_node/src/nodeota.cpp and esp32cam-streamer/src/nodeagent.cpp - each
// carrying a comment saying it was the same shape and the same rule as the panel's. That was
// tolerable while the rule was one line long. It stopped being tolerable when the scheme started
// deciding whether the socket is encrypted: four copies of THAT is three chances for one board to
// keep talking plaintext to a server the other two reach over TLS, and the symptom would be a
// bearer token in the clear from whichever board nobody remembered to edit.
//
// Header-only and static inline, like nodeproto.h beside it and for the same reason: three
// PlatformIO projects share this directory with -I and none of them has a build step that could
// link a fourth translation unit.
//
// WHAT IS AND IS NOT ACCEPTED. "http://host[:port][/prefix]", and nothing else. Anything that
// does not parse reads as unconfigured, which is the same rule all four copies had: a base URL
// nobody can parse is a typo, and guessing at it would point a firmware install at a server that
// does not exist.
//
// BOTH SCHEMES PARSE, AND THE SCHEME IS THE ONLY THING THAT DECIDES ENCRYPTION. https:// sets
// SrvUrl.tls and defaults the port to 443; http:// clears it and defaults to 80; an explicit
// ":port" beats either default. Nothing else in these firmwares chooses: srvconn.h reads that one
// flag and hands back a secure or a plain client accordingly, so switching the whole fleet from
// cleartext to TLS is editing one string in NVS.
//
// This file used to refuse https:// on purpose, because there was no TLS client to hand it to and
// returning port 443 to a caller holding a plain WiFiClient writes a bearer token at a TLS port in
// the clear. That refusal is gone because the reason is gone.
//
// HTTPS WORKS ON THIS HARDWARE. THIS IS THE ONE THING TO READ BEFORE TOUCHING THIS FILE.
//
// The client was dropped on the reading that mbedtls could not seed its CTR_DRBG from this board's
// hardware RNG. That was measured and it is false: mbedtls_ctr_drbg_seed() succeeds here in 1.7ms
// with the radio off and 2.0ms associated, esp_random() passes bit-balance and chi-square, and the
// hex code recorded beside the original failure (0x0032) is MBEDTLS_ERR_DES_INVALID_INPUT_LENGTH,
// which no handshake returns. A real symptom and an unrelated number had been read as one cause.
//
// A probe then ran a real request from this panel against this server. Three cases, because a
// positive result on its own proves nothing - a client that ignores its CA argument also reports
// success:
//
//   insecure              connect rc=1   791ms  -> HTTP/1.1 200 OK
//   pinned ISRG Root X1   connect rc=1  1844ms  -> HTTP/1.1 200 OK
//   deliberately wrong CA connect rc=0  1304ms  -> -0x2700 X509 verification failed
//
// The third line is what makes the second mean something: chain verification is enforced, so a
// pinned root is real authentication and not decoration. Cost per connection: 4.4KB of stack,
// no net internal DRAM once the session is closed, and 1.84s of handshake with verification
// against 0.79s without - the difference being four signature checks.
//
// TWO LIMITS TO CARRY FORWARD, both measured rather than assumed:
//
//   Expiry is NOT checked. CONFIG_MBEDTLS_HAVE_TIME_DATE is unset in this framework (the IDF
//   default), so notBefore/notAfter are ignored: a chain that verifies cryptographically is
//   accepted whatever its dates say. Signatures are checked, expiry is not. Pinning the LEAF
//   would be wrong regardless - this server's certificate is a 90-day Let's Encrypt one, renewed
//   under the panel. Pin the root: ISRG Root X1 runs to 2035 and validates the chain alone.
//
//   Handshake cost versus push cadence. hlog.cpp pushes the console every 3s, and it carries the
//   same bearer token, so it cannot stay on port 80 while the poll moves. At 1.84s a handshake
//   that is a 60% duty cycle of TLS setup for logs alone. Whoever does the migration holds one
//   connection open for the push rather than handshaking per chunk; the batching is already there.
//
// Both limits above are handled rather than left as warnings: srvconn.h pins ISRG Root X1, and
// hlog.cpp holds one connection across pushes instead of handshaking per chunk.
//
// THREE BUILD TRAPS THE LAST ATTEMPT PAID FOR, so the next one does not:
//
//   The include must live under src/. PlatformIO's dependency finder walks includes it finds in
//   src/, and does NOT walk includes reached through -I shared - so <WiFiClientSecure.h> inside
//   shared/srvconn.h meant NetworkClientSecure was never compiled and the build failed on a
//   missing header. A probe including it from src/ compiled with no lib_deps change at all.
//
//   NetworkClient::connect(const char *, uint16_t, int32_t) IS NOT VIRTUAL. Holding a
//   WiFiClient* to a WiFiClientSecure and calling connect() through it reaches the PLAIN
//   implementation: a bare TCP connect to 443, no handshake, and a first write that goes nowhere -
//   which is precisely the symptom that got TLS abandoned. Keep the concrete type.
//
//   It costs 98.5KB of flash on the panel, measured (3,566,934 -> 3,665,490 bytes) and the two
//   nodes ran about 90KB each. The panel has room; the nodes are the ones to check first.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// 64 and 48 are what all four copies used, and they are not arbitrary: NODEPROTO_URL is 128, so a
// URL that fits on the wire to a node fits in a host plus a prefix here with room over.
#define SRVURL_HOST_CAP   64
#define SRVURL_PREFIX_CAP 48

struct SrvUrl {
    char     host[SRVURL_HOST_CAP];
    char     prefix[SRVURL_PREFIX_CAP];  // path prefix, "" for none, never a trailing slash
    uint16_t port;
    bool     tls;                        // https:// - the caller must open a secure socket
};

// True when `url` parsed. `out` is untouched on failure except for being zeroed, so a caller that
// ignores the return value cannot end up pointing at a half-parsed host.
//
// `tls` is the whole of what the scheme decides here. This function does not know how to open a
// socket and must not: srvconn.h is where the flag turns into a client, and a caller that reads
// `port` without reading `tls` writes a bearer token in the clear at a TLS port. That is why the
// field is not called `secure` or `https` - it is named after the thing the caller has to DO.
static inline bool srvurl_parse(const char *url, struct SrvUrl *out) {
    memset(out, 0, sizeof(*out));
    if (url == NULL) return false;

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        out->tls = true;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->port = 80;
        p += 7;
    } else {
        return false;                                  // anything else is a typo, not a scheme
    }

    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < sizeof(out->host)) out->host[i++] = *p++;
    out->host[i] = '\0';
    if (!out->host[0]) return false;
    while (*p && *p != ':' && *p != '/') p++;          // an over-long host is a typo
    if (*p == ':') {
        int port = atoi(p + 1);
        if (port <= 0 || port > 65535) return false;
        out->port = (uint16_t)port;                    // an explicit port beats the scheme default
        while (*p && *p != '/') p++;
    }
    i = 0;
    while (*p && i + 1 < sizeof(out->prefix)) out->prefix[i++] = *p++;
    out->prefix[i] = '\0';
    size_t n = strlen(out->prefix);                     // "…:8000/" would double the slash
    if (n && out->prefix[n - 1] == '/') out->prefix[n - 1] = '\0';
    return true;
}
