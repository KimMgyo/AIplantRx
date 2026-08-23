// "Update me to whatever the server has."
//
// See src/fwpull.cpp for the whole story. The short version: pushing an image with
// `pio run -t upload` needs a laptop with the toolchain on the same network, and the person
// standing in front of a greenhouse panel has neither. This pulls instead - the panel asks the
// server what the newest firmware is, decides whether it is already running it, and installs it
// if not.
#pragma once
#include "nodeproto.h"   // NodePhase, the vocabulary all three boards report their updates in
#include <stdbool.h>
#include <stdint.h>

// Starts the worker. Call from setup(); it does nothing until something asks for a pull.
void fwpull_init(void);

// Asks for a pull. Enters update mode first - a 2.5MB flash write does not share this board, and
// that is measured, not assumed (see updatemode.cpp). Idempotent while one is already running.
//
// `why` is shown on the panel, so it is a short human phrase ("버튼", "서버").
void fwpull_request(const char *why);

// True from the moment a pull is armed until the board restarts into the new image or gives up.
bool fwpull_active(void);

// 0..100 while the image is downloading, -1 at every other time - same convention as
// ota_progress(), so the takeover screen can show whichever of the two is running.
int fwpull_progress(void);

// One short Korean phrase describing where the pull is: asking, downloading, verifying, already
// up to date, or the reason it failed. Written for the panel, so it is what a grower reads rather
// than what a developer greps - and for the states the two node firmwares also reach, it is one
// of shared/nodeproto.h's phrases and not a wording of this board's own, so one fact reads the
// same on all three cards of the update page.
const char *fwpull_status(void);

// The same state as a NodePhase, which is what code should branch on: the phrase is for a person
// and it is allowed to be reworded, while page_update.cpp has to decide a colour and an overlay
// from it. NODE_PH_IDLE until the first fwpull_request(), exactly as fwpull_status() is empty
// until then. See the note on s_phase in src/fwpull.cpp for why the panel stopped being the one
// board whose card could not be classified.
uint8_t fwpull_phase(void);
