// Comparing two firmware versions, so a board can tell "behind" from "ahead" from "the same".
//
// WHY A HASH WAS NOT ENOUGH. Every board decided whether to install by comparing app_elf_sha256
// against the server's. That answers "same or different", and different turned out to mean two
// opposite things:
//
//   Identical source, rebuilt. Measured on this project's releases - sensor_node's source is
//   byte-identical between v0.4.2 and v0.4.3, both images are 1,085,584 bytes, and 68 bytes differ:
//   the descriptor's elf_sha256 and the trailing integrity hash. A node already running the newest
//   code fetched a megabyte to install it again.
//
//   A local build. A USB-flashed image is newer than what the server has, and the panel's update
//   button replaced it three times, each time losing unreleased work.
//
// esp_app_desc.version cannot answer this: it reads "76b7a3f" in every image this project has ever
// built, because it describes the precompiled Arduino core. So scripts/fwver.py compiles in
// `git describe --tags --dirty --always` as FW_VERSION, and this file orders those strings.
//
// WHAT IS COMPARABLE, and the shape is git's rather than semver's on purpose - it is what the tool
// that produces it emits, so there is no format to keep in agreement:
//
//   v0.4.3                    an exact tag; what CI builds
//   v0.4.3-1-g0ab2efa         one commit past that tag - later than the tag
//   v0.4.3-1-g0ab2efa-dirty   uncommitted work on top - later still
//
// ANYTHING ELSE IS NOT A VERSION AND SAYS SO. An empty string, a bare commit hash from a tree with
// no tags, a release named something else - fwver_cmp() reports FWVER_UNKNOWN and the caller falls
// back to the hash it was using before. That is the whole compatibility story: a board or a server
// that has no version behaves exactly as it did, and ordering is a bonus that appears once both
// ends have one.
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Deliberately not an enum with a comparison operator: the UNKNOWN case is not "equal" and must not
// be reachable by accident from a caller that only tested for < and >.
#define FWVER_UNKNOWN  (-2)
#define FWVER_OLDER    (-1)
#define FWVER_SAME     (0)
#define FWVER_NEWER    (1)

struct FwVer {
    unsigned major, minor, patch;
    unsigned commits;   // "-N-g<hash>" after the tag; 0 on an exact tag
    bool     dirty;     // "-dirty" suffix
};

// True when `s` is a version this can order. Leading 'v' optional, three numeric components
// required - a two-component tag is refused rather than guessed at, because guessing whether
// "v0.4" means 0.4.0 is how two boards disagree about the same string.
static inline bool fwver_parse(const char *s, struct FwVer *out) {
    if (s == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (*s == 'v' || *s == 'V') s++;

    unsigned *fields[3] = {&out->major, &out->minor, &out->patch};
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10u + (unsigned)(*s - '0');
            s++;
        }
        *fields[i] = n;
        if (i < 2) {
            if (*s != '.') return false;
            s++;
        }
    }

    // What git appends, in the order it appends it. Anything not of this shape leaves the version
    // unparsed rather than half-parsed: a suffix this does not understand may well carry ordering
    // information, and silently dropping it would order two builds by a prefix they share.
    if (*s == '\0') return true;
    if (*s != '-') return false;
    s++;

    if (*s >= '0' && *s <= '9') {
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10u + (unsigned)(*s - '0');
            s++;
        }
        out->commits = n;
        // "-N-g<hash>" is one unit; N on its own is not something git emits.
        if (*s != '-' || s[1] != 'g') return false;
        s += 2;
        while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')) s++;
        if (*s == '\0') return true;
        if (*s != '-') return false;
        s++;
    }

    if (strcmp(s, "dirty") == 0) {
        out->dirty = true;
        return true;
    }
    return false;
}

// How `theirs` compares to `mine`: FWVER_NEWER means theirs is later and worth installing.
// FWVER_UNKNOWN when either side is not a version, which is the caller's cue to compare hashes.
static inline int fwver_cmp(const char *mine, const char *theirs) {
    struct FwVer a, b;
    if (!fwver_parse(mine, &a) || !fwver_parse(theirs, &b)) return FWVER_UNKNOWN;

    if (b.major != a.major) return b.major > a.major ? FWVER_NEWER : FWVER_OLDER;
    if (b.minor != a.minor) return b.minor > a.minor ? FWVER_NEWER : FWVER_OLDER;
    if (b.patch != a.patch) return b.patch > a.patch ? FWVER_NEWER : FWVER_OLDER;
    // Commits past the same tag order the two builds. This is what protects a USB-flashed board:
    // its own version carries commits the published tag does not, so the server's image reads as
    // OLDER and nothing offers to install it.
    if (b.commits != a.commits) return b.commits > a.commits ? FWVER_NEWER : FWVER_OLDER;
    // Uncommitted work is later than the commit it sits on. It cannot be ordered against ANOTHER
    // dirty tree - two people's uncommitted edits are not comparable - but both dirty is SAME here,
    // which is the honest answer: neither is newer, so neither replaces the other.
    if (b.dirty != a.dirty) return b.dirty ? FWVER_NEWER : FWVER_OLDER;
    return FWVER_SAME;
}
