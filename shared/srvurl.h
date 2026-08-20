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
// AND THE REASON TLS WAS ABANDONED IS NOW KNOWN TO BE WRONG, which matters because it is the only
// thing standing between a bearer token and the public internet. The TLS client was dropped on
// the reading that mbedtls could not seed its CTR_DRBG - "the entropy source failed" - which
// pointed at this board's hardware RNG. Measured directly since: mbedtls_ctr_drbg_seed() succeeds
// here in 1.7 ms with the radio off and 2.0 ms with WiFi associated, mbedtls_entropy_func()
// returns real bytes both times, and esp_random() passes a bit-balance and chi-square check. The
// numeric code recorded next to that failure, 0x0032, is MBEDTLS_ERR_DES_INVALID_INPUT_LENGTH,
// which cannot be what a handshake returned - so a real symptom and an unrelated number were read
// as one diagnosis.
//
// The observed facts are unchanged and still unexplained: the handshake reported success and the
// first write put zero bytes on the wire. Entropy is not the reason. Whoever picks HTTPS back up
// starts from that, and from the fact that the server already serves it - https://<host>/health
// answers 200 today - so this is a client-side problem with a known-good far end.
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
