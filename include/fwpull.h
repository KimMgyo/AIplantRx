// "Update me to whatever the server has."
//
// See src/fwpull.cpp for the whole story. The short version: pushing an image with
// `pio run -t upload` needs a laptop with the toolchain on the same network, and the person
// standing in front of a greenhouse panel has neither. This pulls instead - the panel asks the
// server what the newest firmware is, decides whether it is already running it, and installs it
// if not.
#pragma once
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

// One short Korean phrase describing where the pull is: asking, comparing, downloading, already
// up to date, or the reason it failed. Written for the panel, so it is what a grower reads rather
// than what a developer greps.
const char *fwpull_status(void);
