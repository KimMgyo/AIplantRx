// One socket to the server, encrypted or not, decided by the URL the operator stored.
//
// WHY THIS EXISTS AND WHY IT IS A HOLDER, NOT A TYPEDEF. Seven call sites across three firmwares
// hand-write "connect to the server and write a request": plantrx.cpp, fwpull.cpp, nodelog.cpp,
// plantid.cpp and hlog.cpp on the panel, nodeota.cpp on the sensor node, nodeagent.cpp on the
// camera. Each one used to declare `WiFiClient c;` on the stack. Making TLS a per-site decision
// would be seven places to get it wrong, and the failure mode of getting it wrong is a bearer
// token in cleartext - which is exactly the failure this file exists to end.
//
// AN EARLIER VERSION OF THIS FILE WAS DELETED, AND KNOWING WHY IS THE WHOLE DESIGN.
//
//   It returned a WiFiClient* from `new WiFiClientSecure` and called connect() through it.
//   NetworkClient::connect(const char *, uint16_t, int32_t) IS NOT VIRTUAL, so that reached the
//   PLAIN implementation: a bare TCP connect to port 443, no handshake, and a first write that
//   went nowhere. The symptom - "handshake reported success, wrote 0 bytes" - got TLS blamed and
//   abandoned for months.
//
//   So connect() is called on the CONCRETE type here, both members live in the object rather than
//   behind a pointer, and what escapes is a Client* used only for read/write/available/stop -
//   which ARE virtual on Client. The one non-virtual call never crosses a base pointer.
//
// THE OTHER TRAP: PlatformIO's dependency finder walks includes it sees in src/ and does NOT walk
// includes reached through -I shared. <WiFiClientSecure.h> below is therefore invisible to it, and
// each firmware has to name the framework library in platformio.ini lib_deps:
//
//   framework-arduinoespressif32/libraries/NetworkClientSecure
//
// Without that line the build fails on a missing header, which reads like a broken include path
// rather than a dependency-scanner limitation.
//
// VERIFICATION IS REAL, AND WAS PROVEN WITH A NEGATIVE CONTROL. Measured on an
// ESP32-S3-Touch-LCD-7 against this project's server: a pinned ISRG Root X1 connects and gets a
// 200, and a deliberately wrong self-signed root is REFUSED with -0x2700, X509 certificate
// verification failed. Without that third case the first two prove nothing - a client that ignores
// its CA argument also reports success.
//
// WHAT THIS DOES NOT CHECK, stated because it is a real gap and not an oversight: expiry.
// CONFIG_MBEDTLS_HAVE_TIME_DATE is unset in this framework (the IDF default), so notBefore and
// notAfter are ignored and a chain that verifies cryptographically is accepted whatever its dates
// say. Found the honest way - a validation succeeded with the clock reading 12, three seconds after
// association and before SNTP answered. So a leaked leaf key for this domain stays usable after it
// expires. That is a large improvement on cleartext and it is not full validation.
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdint.h>
#include "srvurl.h"

// ISRG Root X1. sha256 96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6
//
// The ROOT and not the leaf, deliberately. This server's certificate is a 90-day Let's Encrypt
// one that Cloudflare renews without telling the panel, so a pinned leaf would brick every board
// in the greenhouse four times a year. This root runs to 2035-06-04 and was checked to validate
// the server's chain on its own: the chain arrives leaf -> Let's Encrypt YE1 -> ISRG Root YE ->
// ISRG Root X2, cross-signed by X1.
static const char SRVCONN_ROOT_CA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

// ONE TLS SESSION AT A TIME, ACROSS THE WHOLE FIRMWARE, AND THE REASON IS MEASURED.
//
// A session costs about 50KB of INTERNAL DRAM while it is open - mbedtls's 16KB inbound and 4KB
// outbound content buffers (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y in this framework, and the content
// lengths are not tunable without building the IDF from source), the ssl context, and the four
// certificates Cloudflare sends parsed at once beside our pinned root. Measured on the panel:
// internal free sits at 103,740 bytes idle and falls to 53,896 with one session open, with a
// low-water mark of 32,128. Two overlapping sessions do not fit, and the failure is not graceful:
//
//   [E][ssl_client.cpp] start_ssl_client(): (-32512) SSL - Memory allocation failed
//
// So a TLS connect takes a gate held for the life of the connection. Everything about that is
// deliberate:
//   - The plain path never touches it. An http:// board behaves exactly as it did.
//   - The wait is bounded by the caller's own connect timeout, and a caller that cannot get in
//     fails the way it already fails a refused connect. Blocking forever would turn a memory
//     ceiling into a hang.
//   - It means NO SITE MAY HOLD A CONNECTION IDLE. hlog.cpp's console push used to keep one open
//     across pushes to avoid a handshake every three seconds; under this gate that would starve
//     the telemetry poll forever, so it reconnects per push on a slower cadence instead. See the
//     note there.
//
// How long a TLS connect will queue behind another one before giving up. Not the connect timeout:
// see the note where it is taken. Thirty seconds is long enough that ordinary contention between a
// one-minute poll and a twenty-second log push never surfaces, and short enough that a gate leaked
// by some future bug shows up as a slow uplink rather than a permanently silent one.
static const uint32_t GATE_WAIT_MS = 30000;

// The least a TLS connect gets, whatever the caller asked for. Every CONNECT_MS in this project is
// 4000 and every one of them says "same LAN, same server" - which was true of a TCP connect and is
// not true of a handshake plus a queue.
//
// IT IS CONTENTION AND NOT LINK SPEED, and the first version of this note had that wrong. The
// failure was "[hlog] push failed, status=-1 after 5330ms" on a phone hotspot, which reads as a
// slow link until you subtract it: 1330ms of that was waiting behind the poll at the gate, and the
// remaining 4000ms was the connect timeout expiring. Then both nodes downloaded a firmware image
// over TLS over the same hotspot on the same 4000ms constant without a single retry - because each
// of those boards is the only TLS user on it and never queues. So what 4s cannot absorb is a
// handshake that starts late, on the one board where three callers share one gate.
//
// Floored here rather than raised at five call sites, because the call sites were not wrong: 4s IS
// enough to open a socket, and this is the only code that knows whether it is also negotiating one
// behind somebody else. Bounded at ten seconds and not more: plantrx_poll() runs on the loop task,
// so this is time the node ticks beside it do not get, and a dead server should read as a failed
// poll rather than a stalled panel.
static const uint32_t TLS_CONNECT_FLOOR_MS = 10000;

// Holds whichever client the URL called for and hands out the Client* the caller writes through.
// Copying is meaningless - it owns a live socket - so it is not copyable.

class SrvConn {
public:
    SrvConn() {}
    SrvConn(const SrvConn &) = delete;
    SrvConn &operator=(const SrvConn &) = delete;
    ~SrvConn() { stop(); }

    // A connected Client on success, nullptr on failure. `u.tls` decides which socket; nothing
    // else does, and no caller gets to override it - a per-site opt-out is how six of seven sites
    // end up encrypted and the seventh leaks the token.
    Client *connect(const struct SrvUrl &u, uint32_t timeout_ms) {
        stop();
        if (!u.host[0]) return nullptr;

        // Set BEFORE the attempt and not after it. tls() has to answer "was this socket supposed
        // to be encrypted", because the caller that most needs the answer is the one whose connect
        // just FAILED and is deciding whether there is a handshake error worth printing. A flag
        // set only on success is false on exactly that path, which is a footgun in shared code
        // rather than a thing to work around at each of seven call sites.
        m_tls = u.tls;

        if (u.tls) {
            // What the caller asked for, or the floor, whichever is longer - see
            // TLS_CONNECT_FLOOR_MS. A caller that already asks for more keeps it.
            if (timeout_ms < TLS_CONNECT_FLOOR_MS) timeout_ms = TLS_CONNECT_FLOOR_MS;
            // The gate, taken before anything is allocated and released only by stop().
            //
            // Its wait has a budget of its OWN and does not come out of timeout_ms, which was the
            // first version and was wrong: a caller that waited 2.4s for the poll ahead of it then
            // had 1.6s left of a 4s budget for a handshake that measures 1.84s, so contention
            // presented as "connect failed after exactly your timeout". Queueing is not a network
            // operation and must not be charged as one. GATE_WAIT_MS is generous because the
            // longest thing that can hold this is one firmware download, and the panel stands its
            // poll down during an update anyway.
            if (!gate_take(GATE_WAIT_MS)) return nullptr;
            m_held = true;
            m_tls_attempted = true;
            // setCACert() before connect(): it is read during the handshake, and setting it after
            // is a call that compiles, returns, and validates nothing.
            m_secure.setCACert(SRVCONN_ROOT_CA);
            // MILLISECONDS, despite the name reading like seconds - Stream::setTimeout documents
            // itself that way and NetworkClientSecure does not override it. It is set anyway and
            // then immediately overwritten by connect()'s third argument (NetworkClientSecure.cpp
            // assigns _timeout = timeout there), which is what actually reaches the socket. Kept
            // because a caller that only ever calls read() through Stream still wants a sane one.
            m_secure.setTimeout(timeout_ms);
            if (m_secure.connect(u.host, u.port, (int32_t)timeout_ms) != 1) {
                m_secure.stop();
                gate_give();
                m_held = false;
                return nullptr;
            }
            m_c = &m_secure;
        } else {
            if (m_plain.connect(u.host, u.port, (int32_t)timeout_ms) != 1) {
                m_plain.stop();
                return nullptr;
            }
            m_c = &m_plain;
        }
        return m_c;
    }

    // True while the socket this handed out is still usable, so a caller holding a connection
    // across requests can ask before reusing it rather than discovering it on a failed write.
    bool alive() const { return m_c != nullptr && const_cast<Client *>(m_c)->connected(); }

    // Whether this connection was ASKED to be encrypted - true from the moment connect() is
    // called with a tls URL, including after it fails. See the note in connect().
    bool tls() const { return m_tls; }

    void stop() {
        if (m_c) {
            m_c->stop();
            m_c = nullptr;
        }
        // The gate goes back here and nowhere else, which is why every failure path in connect()
        // gives it up explicitly and why ~SrvConn() calls stop(). A leaked gate is a panel whose
        // uplink is permanently down with nothing in the log to say why.
        if (m_held) {
            gate_give();
            m_held = false;
        }
        // m_tls deliberately survives stop(): a caller that stops and then asks why is asking
        // about the connection it just had, not about the absence of one.
    }

    // mbedtls's own text for the last failure, empty on the plain path. Worth surfacing because
    // "connect failed" and "the certificate did not verify" want completely different fixes.
    int last_error(char *buf, size_t cap) {
        if (buf && cap) buf[0] = '\0';
        return m_tls_attempted ? m_secure.lastError(buf, cap) : 0;
    }

private:
    // Created on first use rather than at static-init time: this header is included from several
    // translation units in three firmwares, and a FreeRTOS object built before the scheduler is
    // running is a class of bug nobody enjoys. First use is always inside a task, and C++ makes
    // the initialisation of a function-local static thread-safe, so one accessor is the whole of
    // it - two statics would be the race this is avoiding.
    static SemaphoreHandle_t gate() {
        static SemaphoreHandle_t s = xSemaphoreCreateMutex();
        return s;
    }
    static bool gate_take(uint32_t timeout_ms) {
        SemaphoreHandle_t g = gate();
        // No gate is worse than no uplink: if the mutex could not be created there is nothing to
        // serialise on, and refusing every TLS connect would be a worse failure than the memory
        // pressure this exists to bound.
        if (g == nullptr) return true;
        return xSemaphoreTake(g, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }
    static void gate_give() {
        SemaphoreHandle_t g = gate();
        if (g) xSemaphoreGive(g);
    }

    WiFiClient m_plain;
    WiFiClientSecure m_secure;
    Client *m_c = nullptr;
    bool m_tls = false;
    bool m_tls_attempted = false;  // any TLS connect, so last_error() knows who to ask
    bool m_held = false;           // this instance owns the TLS gate and must give it back
};
