// The panel <-> node control plane: firmware updates, log lines, and remote debug pokes.
//
// WHY THIS FILE IS SHARED AND NOT COPIED.
// The three protocol families that came before it - ProvMsg, SensorMsg, ThermalFragMsg - each
// exist twice, once in include/camprov.h and once in the node's own tree, under a comment that
// says "MUST byte-for-byte match". That comment is a promise a human keeps, and it has already
// cost this project one silent widening (see the note above THERMAL_PAYLOAD_BYTES: an old node's
// last fragment is 148 bytes instead of 150, and the receiver had to be taught to count bytes
// rather than trust the buffer). A fourth family with three copies would triple that debt for
// nothing: all three firmwares live in this one repository, so a single header on the include
// path is available to all of them and cannot drift.
//
//   panel                 build_flags = -I shared
//   sensor_node           build_flags = -I ../shared
//   esp32cam-streamer     build_flags = -I ../shared
//
// The older families are deliberately left alone. They work, camprov.h carries panel-only
// declarations alongside them, and churning a working wire format to tidy it is how a greenhouse
// stops reporting for an afternoon.
//
// WHY ESP-NOW CARRIES CONTROL AND WIFI CARRIES BYTES.
// A node fetches its own image over HTTP - 1MB does not belong in 250-byte action frames, and the
// panel has no business proxying a download it would have to buffer. But the *decision* to update,
// and every report about how it is going, travel over ESP-NOW, because ESP-NOW keeps working when
// the thing being debugged is the WiFi. A node that cannot get a DHCP lease is exactly the node
// somebody needs a log line out of, and a control plane that shares the failure it is reporting
// on is a control plane that goes quiet when it matters.
//
// SIZE-BASED DISPATCH. camprov.cpp's single recv callback tells the families apart by payload
// length (see on_recv), so every struct here must have a length no other struct on this air has.
// Taken: 6 (ChannelMsg), 44 (StatusMsg), 64 (SensorMsg), 103 (ProvMsg), 242 (ThermalFragMsg).
// This file adds 192 and 232. The static_asserts below are not decoration - they are the only
// thing standing between a struct that grew by two bytes and a receiver that silently stops
// seeing it.
#pragma once
#include <stdint.h>

// Bumped only for a change that an old peer cannot read correctly. A receiver that sees a
// different version says so on the panel instead of parsing garbage: two boards flash
// independently and a greenhouse can easily run a month with one of them behind.
#define NODEPROTO_VER 1

static const uint32_t NODE_MAGIC = 0x53464E50;  // 'SFNP' - SmartFarm node protocol

// Who a message is about. Not who sent it: a panel-to-node command names its target here, and a
// node's own report names itself, so one field answers "which of the three boards is this row"
// in both directions and the UI needs no second lookup.
enum NodeRole : uint8_t {
    NODE_ROLE_PANEL = 0,   // the S3 touch panel; never a command target (it has fwpull.cpp)
    NODE_ROLE_CAM   = 1,   // ESP32-CAM streamer
    NODE_ROLE_NODE  = 2,   // sensor node (SCD41 + BH1750 + MLX90640)
    NODE_ROLE_COUNT = 3,
};

// The wire spelling of a role, and the value the server's ?role= query takes. Shared so the
// three firmwares and the server cannot disagree about what "node" means. "node" rather than
// "sensor" because that is already the word the telemetry schema uses for this device
// (Links.node_online), and a second name for one board is a bug waiting for a grep.
static inline const char *nodeproto_role_name(uint8_t role) {
    switch (role) {
        case NODE_ROLE_PANEL: return "panel";
        case NODE_ROLE_CAM:   return "cam";
        case NODE_ROLE_NODE:  return "node";
        default:              return "?";
    }
}

// --- node -> panel ---------------------------------------------------------------------------

enum NodeRepKind : uint8_t {
    NODE_HELLO = 1,  // periodic "I exist, here is what I am running". text = "" (unused)
    NODE_LOG   = 2,  // one log line. text = the line, already trimmed of its newline
    NODE_PROG  = 3,  // an update changed state. text = the Korean phrase to show a grower
};

// Where an update is. The panel needs this as an enum and not only as text, because the text is
// written for a human to read and the panel has to decide things with it: whether to raise the
// takeover overlay, whether the button is still offerable, whether a silence is a stall.
//
// The phrase that goes beside it is the node's own (text[]), for the same reason fwpull_status()
// is the panel's own: the board that knows what happened is the board that should say it, and a
// receiver translating an enum into words is a second place for the wording to be wrong.
enum NodePhase : uint8_t {
    NODE_PH_IDLE    = 0,  // nothing running; the only phase HELLO ever carries
    NODE_PH_ASK     = 1,  // fetching the manifest, deciding
    NODE_PH_CURRENT = 2,  // asked, and already running that image - a finished, successful check
    NODE_PH_DL      = 3,  // writing the image into the inactive slot; pct is meaningful
    NODE_PH_DONE    = 4,  // written and verified; a restart is imminent
    NODE_PH_FAIL    = 5,  // gave up; text says why
};

// A phase the panel must keep an overlay up for. DONE is included: the node is about to vanish
// for a reboot, and dropping the overlay one second before it goes quiet reads as a failure.
static inline bool nodeproto_phase_busy(uint8_t ph) {
    return ph == NODE_PH_ASK || ph == NODE_PH_DL || ph == NODE_PH_DONE;
}

#define NODE_PCT_NONE 255   // pct is meaningless (mirrors fwpull_progress()'s -1)

// NODEF_CAN_OTA is the one that earns its place. A node built against a single-app-partition
// table cannot install anything, and it is the DEFAULT for the ESP32-CAM board in PlatformIO
// (huge_app.csv). Without this bit the panel would offer a button whose only possible outcome is
// a failure message, on a device the grower cannot see. The node computes it from
// esp_ota_get_next_update_partition(), so it is a fact about the running image and not a build
// flag somebody remembered to set.
#define NODEF_WIFI     (1u << 0)   // joined to WiFi and holding an IP
#define NODEF_PENDING  (1u << 1)   // running an image that has not been marked valid yet
#define NODEF_CAN_OTA  (1u << 2)   // a second app partition exists to write into
#define NODEF_VERBOSE  (1u << 3)   // log streaming is on (see NODE_DEBUG "log on")

#define NODEPROTO_TEXT 160

struct __attribute__((packed)) NodeRepMsg {
    uint32_t magic;        // NODE_MAGIC
    uint8_t  ver;          // NODEPROTO_VER
    uint8_t  kind;         // NodeRepKind
    uint8_t  role;         // NodeRole - the sender describing itself
    uint8_t  seq;          // per-sender, wraps; a repeat is a duplicate, a gap is a loss
    uint8_t  phase;        // NodePhase; NODE_PH_IDLE on HELLO and LOG
    uint8_t  pct;          // 0..100 during NODE_PH_DL, else NODE_PCT_NONE
    uint8_t  flags;        // NODEF_*
    uint8_t  reserved;     // keeps ip[4] and the two uint32s naturally aligned on the receiver
    uint8_t  ip[4];        // STA IPv4; 0.0.0.0 when not joined
    uint32_t uptime_s;     // since boot. A number that keeps resetting is a node in a crash loop
    uint32_t free_heap;    // bytes. The one figure that predicts a node about to stop talking
    uint8_t  elf_sha[8];   // first 8 of app_elf_sha256: this image's identity, same as fwpull's
    char     text[NODEPROTO_TEXT];  // NUL-terminated; see NodeRepKind for what each kind puts here
};
static_assert(sizeof(NodeRepMsg) == 192,
              "NodeRepMsg must stay 192 bytes: camprov.cpp's on_recv dispatches on payload "
              "length, and a collision with another family silently drops one of them");

// --- panel -> node ---------------------------------------------------------------------------

enum NodeCmdKind : uint8_t {
    // Go and fetch whatever the server has for your role. text = the server's base URL,
    // token = its bearer secret. Both are sent rather than configured on the node for the same
    // reason the CAM never had WiFi credentials compiled in: a node that has to be reflashed to
    // learn where its server moved to is a node somebody has to physically reach.
    NODE_UPDATE = 1,
    // A poke for whoever is looking at a log they cannot reach the console of. text = one verb:
    //   "log on" / "log off"  stream (or stop streaming) log lines over NODE_LOG
    //   "state"               dump the node's own summary of itself, as log lines
    //   "reboot"              restart, so a wedged node can be recovered without a walk
    // Unknown verbs are logged and ignored - a newer panel talking to an older node must degrade
    // to "nothing happened" rather than to undefined behaviour.
    NODE_DEBUG = 2,
};

#define NODEPROTO_URL   128   // matches sitecfg.cpp's SITE_URL_CAP, so no legal URL truncates
#define NODEPROTO_TOKEN  96   // matches sitecfg.cpp's SITE_TOK_CAP, same reason

struct __attribute__((packed)) NodeCmdMsg {
    uint32_t magic;                    // NODE_MAGIC
    uint8_t  ver;                      // NODEPROTO_VER
    uint8_t  kind;                     // NodeCmdKind
    uint8_t  role;                     // the target; a node ignores a command addressed elsewhere
    uint8_t  seq;                      // per-command, wraps; a node ignores a seq it just ran
    char     text[NODEPROTO_URL];      // NODE_UPDATE: base URL. NODE_DEBUG: the verb.
    char     token[NODEPROTO_TOKEN];   // NODE_UPDATE only; "" on every other kind
};
static_assert(sizeof(NodeCmdMsg) == 232,
              "NodeCmdMsg must stay 232 bytes: see the note on NodeRepMsg's assert");

// The URL and the token are sent in the clear, which is the same exposure the WiFi password
// already has on this air (ProvMsg XORs it against a fixed 16-byte key, which is obfuscation and
// not encryption, and its own comment says so). Worth stating rather than implying: anyone who can
// hear this radio can already hear the credentials for the network the server sits on, so a
// bearer token travelling beside them widens nothing. It is on the list of things to fix together
// if this air is ever secured, not one at a time.

// --- HTTP contract, shared so all three firmwares and the server agree ------------------------
//
// GET  <base>/v1/firmware/latest?role=<name>   -> {elf_sha256, size, md5, idf_ver, mtime}
// GET  <base>/v1/firmware/image?role=<name>    -> the raw image, with Content-Length
// POST <base>/v1/nodelog                       <- {device, lines:[{role, ms, text}]}
//
// Both firmware endpoints take Authorization: Bearer <token> and answer 404 when nothing is
// published for that role - which is an ordinary answer and not an error, exactly as it already
// is for the panel. `role` defaults to "panel" so every existing panel keeps working against a
// server that has learned about nodes.
//
// The node compares elf_sha256's first 8 bytes against its own and stops there if they match:
// that is why elf_sha[8] rather than a version string is what NodeRepMsg carries. A build number
// would need somebody to remember to bump it; the ELF hash cannot be forgotten.
#define NODEPROTO_PATH_LATEST "/v1/firmware/latest?role="
#define NODEPROTO_PATH_IMAGE  "/v1/firmware/image?role="
#define NODEPROTO_PATH_LOG    "/v1/nodelog"
