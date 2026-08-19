// "Go and fetch whatever the server has for a sensor node."
//
// The panel decides, the node downloads. src/fwpull.cpp on the panel does the same two GETs
// against the same two endpoints and its comments carry the measurements behind every constant
// here; this is that file aimed at a board with no display, no PSRAM and no WiFi.
//
// WHY THE NODE DOES THE FETCHING. A megabyte does not fit in 250-byte action frames, and a panel
// that proxied the transfer would have to buffer an image it has no use for on the board whose
// internal DRAM is already the scarce thing. The node has a TCP stack it was simply never using.
// All it was missing was somebody to tell it where the server is, and that fits in one 232-byte
// NodeCmdMsg.
#pragma once
#include <stdbool.h>

// Runs the whole update inline and returns when it is over - which for a successful install
// means it does not return, because the last thing it does is restart.
//
// BLOCKING, ON THE LOOP TASK, DELIBERATELY. A worker task would have to share main.cpp's radio
// with the sensor and thermal senders, and those hop the channel between sends; a hop from
// another task in the middle of this download drops the association and loses the transfer. Held
// on the one task that owns the radio, the interleaving cannot happen at all. What it costs is
// the sensor loop for the length of the download - the SCD41 keeps measuring, the display holds
// its last frame - on a board that is about to reboot anyway.
//
// `base_url` is the panel's server URL ("http://host:port/prefix"), `token` its bearer secret;
// both arrive in the NodeCmdMsg. Every exit reports its own phase through nodeagent_report(), so
// a caller has nothing to say afterwards.
void nodeota_run(const char *base_url, const char *token);
