#pragma once

// WHERE THIS PANEL'S SERVER ADDRESS AND SHARED SECRET COME FROM, AND WHY IT IS
// NO LONGER JUST THE HEADER.
//
// They used to come from include/plantrx_config.h alone, compiled in. That is
// fine for as long as every image is built on the machine that owns the
// greenhouse. It stops being fine the moment a build is published: this
// repository is public, a GitHub Actions artifact of a public repository is
// public, and a bearer token compiled into a public binary is a token anyone can
// lift back out with `strings` and spend against this greenhouse's API. There is
// no build flag that fixes that. The token has to not be in the image.
//
// So the image and the site are separated. The compile-time defines stay, as the
// way a human states intent on the machine they build from. NVS holds what was
// last stated, so an image built with nothing in it - which is what CI builds -
// inherits the site it lands on instead of forgetting it.
//
// The rule, in full:
//   define non-empty  the define wins, and is written back to NVS. Somebody who
//                     edited the header and flashed over USB meant the value in
//                     the header, and an NVS entry that outranked it would be a
//                     panel that ignores its own source tree.
//   define empty      NVS wins. This is the over-the-air case: a published image
//                     carries no site and keeps the one already provisioned.
//   both empty        unconfigured. plantrx_configured() reads false, no socket
//                     is ever opened, and the panel runs on its local rule
//                     engine - the same degradation as a panel that was never
//                     given a server at all, which is a working greenhouse
//                     controller and not an error state.
//
// Seeding is what removes the need for a keyboard on the provisioning path.
// Flash once from the machine that has the real header and NVS is set; every
// image after that can arrive over the air with no secret in it.

void sitecfg_init(void);

// Both return a NUL-terminated string, never nullptr, "" when unset. Stable for
// the life of the boot - nothing rewrites them after init - so callers may parse
// once and keep the result, which plantrx.cpp and fwpull.cpp both do.
const char *sitecfg_base_url(void);
const char *sitecfg_token(void);
