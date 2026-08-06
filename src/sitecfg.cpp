#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include "sitecfg.h"
#include "plantrx_config.h"   // PLANTRX_BASE_URL, PLANTRX_TOKEN - the build's intent
#include "hlog.h"

// Sized to what the fields downstream can actually hold rather than generously.
// plantrx.cpp parses the URL into a 64-byte host and a 48-byte prefix, so a URL
// past this could not survive that parse anyway. The token is a 43-character
// base64url secret today; 96 leaves room for a longer one without inviting a
// paste of something that was never a token.
#define SITE_URL_CAP 128
#define SITE_TOK_CAP 96

// Its own namespace, for the reason health.cpp gives for having one: "net" holds
// WiFi credentials, and the shared secret for the greenhouse API has no business
// sharing a page with them.
static Preferences s_prefs;

static char s_base[SITE_URL_CAP];
static char s_tok[SITE_TOK_CAP];

// One field. `seed` is what the build stated - empty means the build said nothing
// and whatever is stored stands.
static void load_one(const char *key, const char *seed, const char *what,
                     char *out, size_t cap) {
    out[0] = '\0';
    s_prefs.getString(key, out, cap);   // leaves out[] alone and returns 0 when absent

    if (!seed[0]) return;

    // Refused rather than truncated. A truncated URL fails the parse loudly, but
    // a truncated token is a 401 on every request forever with nothing on the
    // panel to say why, and falling back to what NVS already holds is far more
    // likely to be right than half a secret.
    size_t n = strlen(seed);
    if (n >= cap) {
        hlogf("[sitecfg] %s from the build is %u bytes, cap is %u; keeping stored" "\n",
              what, (unsigned)n, (unsigned)cap - 1);
        return;
    }

    // The write is skipped when it would change nothing. This runs on every boot
    // of every image built with a real header, NVS is flash, and ui_prefs_load()
    // avoids the same needless write for the same reason.
    if (strcmp(out, seed) != 0) {
        s_prefs.putString(key, seed);
        hlogf("[sitecfg] %s provisioned from the build" "\n", what);
    }
    memcpy(out, seed, n + 1);
}

void sitecfg_init(void) {
    s_prefs.begin("site", false);

    load_one("base", PLANTRX_BASE_URL, "server address", s_base, sizeof(s_base));
    load_one("tok",  PLANTRX_TOKEN,    "shared secret",  s_tok,  sizeof(s_tok));

    // The address is worth a line because it decides whether there is an uplink
    // at all, and an over-the-air image that came up pointed at the wrong
    // greenhouse should say so at boot rather than be inferred from failures.
    // The secret gets only its length: writing a shared secret into a log ring
    // that hlog serves to anything on the LAN would undo the point of moving it
    // out of the image.
    hlogf("[sitecfg] server=%s secret=%s" "\n",
          s_base[0] ? s_base : "(none)",
          s_tok[0] ? "set" : "(none)");
}

const char *sitecfg_base_url(void) { return s_base; }
const char *sitecfg_token(void)    { return s_tok; }
