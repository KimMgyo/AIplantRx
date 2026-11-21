// Firmware updates from GitHub Releases (UI.md 8, ARCHITECTURE.md 6).
//
// The release carries manifest.json; releases/latest always points at the newest non-prerelease, so the
// address below never changes. The S3 reads it, compares versions, downloads its own image over HTTPS
// (certificate bundle, redirects followed), checks size and SHA-256 against the manifest and only then
// switches the boot partition.
//
// A new image boots "pending": it has 120 s to get Wi-Fi back with the UI running, then it is confirmed.
// If it crashes or never gets there, the bootloader goes back to the previous image.
//
// Automatic: one check a minute after boot, then every 6 h. With auto-update on, a newer image is
// installed only while the main screen is up and nobody has touched the panel for 10 minutes.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define FWU_MANIFEST_URL "https://github.com/KimMgyo/AIplantRx/releases/latest/download/manifest.json"

enum FwuState : uint8_t { FWU_IDLE, FWU_CHECKING, FWU_LATEST, FWU_AVAILABLE, FWU_INSTALLING, FWU_RESTARTING, FWU_FAIL };

struct FwuInfo {
    FwuState state;
    char latest[24];        // the S3 version in the manifest ("" before the first good check)
    char err[64];           // why the last check or install failed
    uint8_t pct;            // download progress while installing
    uint32_t checked;       // epoch seconds of the last good check, 0 if none (or no clock yet)
    bool pending;           // this image still waits for its self-check
    char rolled_back[24];   // version the bootloader refused at the last boot, or ""
};

void fwu_init();            // after net_init()
FwuInfo fwu_info();
uint32_t fwu_gen();         // bumps on every state change (for redrawing the settings page)
void fwu_check_now();
void fwu_install_now();     // only does something in FWU_AVAILABLE
// Compares "2.0.10" and "2.0.9-dev" style versions: <0, 0, >0. A suffix sorts before the plain version.
int fwu_vercmp(const char *a, const char *b);
// {"state":"available","latest":"2.0.2","pct":0,"pending":false,"rolled_back":"","auto":true,"err":""}
void fwu_json(char *out, size_t n);
