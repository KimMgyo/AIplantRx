// The board's "get out of the way, a firmware update is coming" mode.
//
// See src/updatemode.cpp for why this exists as a mode rather than a flag inside the OTA code:
// the things that have to stand down are spread across four subsystems, and the reason they have
// to is measured, not assumed.
#pragma once
#include <stdint.h>

// Draws the takeover screen and arms the timeout. Idempotent - a second call while already in
// update mode only refreshes the reason line, so the button, the server and an incoming push
// update can all call it without coordinating.
//
// `why` is shown to whoever is standing in front of the panel, so it is a short human phrase
// ("버튼", "서버", "업로드 감지") and not an error code.
void updatemode_enter(const char *why);

// True once update mode has been entered. Every subsystem that competes with an update for the
// network or for core 0 checks this and stops; see updatemode.cpp for the measurements that
// decide who is on the list and who is not.
bool updatemode_active(void);

// From loop(). Enforces the timeout: update mode has no exit except a restart, so a board that
// entered it and then received nothing must not sit there forever.
void updatemode_tick(void);

// Seconds left before the timeout gives up and restarts. 0 when not in update mode.
uint32_t updatemode_left_s(void);
