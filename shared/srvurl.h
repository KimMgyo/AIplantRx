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
// https:// IS REFUSED ON PURPOSE, and refusing it is the whole reason this note is long. There is
// no TLS client on any of these three boards - it was built, it did not work, and it was taken
// back out; see the commit that removed shared/srvconn.h. Parsing https:// and handing back
// port 443 would leave every caller opening a PLAIN socket to a TLS port and writing a request
// with a bearer token in it as cleartext, which the far end would drop on the floor. The failure
// would read as "the server stopped answering". So a URL somebody types with the wrong scheme on
// the settings page fails loudly at the parse instead of quietly on the wire.
//
// AND HTTPS IS NOT BLOCKED - IT WORKS. THIS IS THE ONE THING TO READ BEFORE TOUCHING THIS FILE.
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
// So https:// still parses to a refusal HERE, and that is now a statement about this parser and
// not about the hardware: nothing in these three firmwares can speak it yet, and handing back
// port 443 to a caller holding a plain WiFiClient writes a bearer token at a TLS port in the
// clear. Teach the callers first, then this.
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
};

// True when `url` parsed. `out` is untouched on failure except for being zeroed, so a caller that
// ignores the return value cannot end up pointing at a half-parsed host.
static inline bool srvurl_parse(const char *url, struct SrvUrl *out) {
    memset(out, 0, sizeof(*out));
    if (url == NULL) return false;

    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return false;   // https:// included - see the note above
    out->port = 80;
    p += 7;

    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < sizeof(out->host)) out->host[i++] = *p++;
    out->host[i] = '\0';
    if (!out->host[0]) return false;
    while (*p && *p != ':' && *p != '/') p++;          // an over-long host is a typo
    if (*p == ':') {
        int port = atoi(p + 1);
        if (port <= 0 || port > 65535) return false;
        out->port = (uint16_t)port;
        while (*p && *p != '/') p++;
    }
    i = 0;
    while (*p && i + 1 < sizeof(out->prefix)) out->prefix[i++] = *p++;
    out->prefix[i] = '\0';
    size_t n = strlen(out->prefix);                     // "…:8000/" would double the slash
    if (n && out->prefix[n - 1] == '/') out->prefix[n - 1] = '\0';
    return true;
}
