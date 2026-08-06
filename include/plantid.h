// Plant species identification, done by the plantrx server.
//
// Button-triggered: grabs the CAM's high-res still (http://<cam-ip>/rgb/image)
// and POSTs the JPEG to <server>/v1/identify, where the keys live and the whole
// identification happens - the PlantNet call, the Wikipedia langlink lookup for
// a Korean name and the translation fallback behind it. The board holds no API
// key and opens no TLS session; it reads back one flat JSON object and shows
// what it says. Two mbedTLS record buffers of 16KB each is more contiguous
// internal DRAM than this board has, which is why the work is over there.
//
// The address is plantrx's: plantrx_srv_host() and friends, so there is one
// answer in the firmware to where the server is. All of it runs in a background
// task so the LVGL UI never blocks; the UI polls plantid_state() and reads the
// result strings when it turns OK.
#pragma once

enum PlantIdState { PLANTID_IDLE, PLANTID_BUSY, PLANTID_OK, PLANTID_ERR };

void plantid_init(void);        // allocate buffers + start the (parked) worker task
void plantid_trigger(void);     // request one identification; ignored while BUSY
PlantIdState plantid_state(void);

// Valid when state == PLANTID_OK:
const char *plantid_species(void);    // scientific name (e.g. "Ajuga genevensis")
const char *plantid_common(void);     // first common name (may be "")
const char *plantid_korean(void);     // resolved Korean name (may be "")
float       plantid_score(void);      // confidence 0..1

// Valid when state == PLANTID_ERR:
const char *plantid_error(void);      // short human-readable reason (Korean)

// Valid at any time, because they describe the server's day and not this
// identification: the last figures it reported, and -1 until it has reported
// any. Both pages poll them on a UI timer with no regard for the state.
int         plantid_total_remaining(void);  // calls left today, as the server last counted them
int         plantid_total_quota(void);      // the server's daily allowance behind that count

// Whether plantid_total_remaining() is a measurement or partly a guess.
//
// The device cannot answer this any more and does not try: the keys are the
// server's, so both figures arrive over the wire and this flag arrives with
// them. The caveat it carries is the same one it always was, now stated by the
// side that can actually know it - a key that has never been used has an
// assumed allowance behind it rather than a confirmed count, and those keys may
// be shared with another install.
//
// False means "at most that many", so the two call sites say 최대 and print the
// plain number once it is true. False is also the state before the first
// identification of a boot, where remaining and quota are both -1: nothing has
// been reported yet, which is a thing not known rather than an allowance the
// panel may put on screen.
bool plantid_total_is_measured(void);
