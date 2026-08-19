// Log lines from the boards without a console.
//
// The panel has hlog.cpp and a USB-UART bridge, so a developer standing at the panel can read it.
// The CAM is bolted to a wall and the sensor node is in a tray, and both of them are exactly the
// boards whose failures nobody sees: an I2C bus that stopped answering, a WiFi join that keeps
// retrying, a heap that has been shrinking for three days. Their serial output has always
// existed and has always gone nowhere.
//
// So it goes somewhere instead. The ring here is a send buffer and not a view: lines land in it
// from an ESP-NOW callback that must not block, and nodelog_tick() POSTs whatever has accumulated
// on the panel's own schedule. The reader is at the other end - the interesting failures happen at
// 4am and the useful question is what the last twenty lines were before a board went quiet.
//
// IT USED TO HAVE A SECOND READER, the 업데이트 page's log list, and does not any more: that page
// is now three identical device cards with one control each, and a twenty-line list belonged to
// none of them. Each card's hint line still carries its own board's newest words. If the list ever
// comes back it needs an index over this ring, which is what nodelog_count()/nodelog_line() were -
// they were deleted rather than left unused, so nothing here has to stay true for a caller that
// does not exist.
//
// WHY IT IS NOT hlog. hlog writes to Serial synchronously and is called from anywhere including
// interrupt-adjacent paths. This buffers and forwards over TCP on the poll's schedule. Mixing the
// two would put a socket write behind a printf, and the panel's own log is the one thing that has
// to keep working when the network is what broke.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nodeproto.h"

// How many lines are kept. Sized against what it is for rather than against what fits: twenty
// lines is a node's last few seconds, which is the window that explains a crash, and at
// NODEPROTO_TEXT bytes a line the whole ring is 3.4KB of internal DRAM - the same order as one
// LVGL card.
#define NODELOG_LINES 20

// Not a second opinion about how long a line is. Every buffer between the radio and this ring is
// NODEPROTO_TEXT wide: NodeRepMsg.text, and then the NUL-terminated copy of it that nodeota.cpp
// makes before handing the pointer over (nodeota.cpp:395 - this takes a `const char *` and could
// not tell an unterminated one apart). A narrower ring would be the single place a line that
// arrived intact gets shortened, and the truncation would read as the node's own. Written as the
// wire macro rather than as 160 because the two being equal is the invariant, and a literal here
// is a place for them to stop being equal quietly.
#define NODELOG_TEXT  NODEPROTO_TEXT

void nodelog_init(void);

// One line from a node, as it arrived. Newlines are already stripped by the sender; this does not
// re-check, because a line that somehow carries one renders as a taller label and that is a
// cosmetic problem, not a reason to spend a scan on every line.
void nodelog_add(uint8_t role, const char *line);

// From loop(). Forwards whatever has accumulated to the server, at most one POST per call and
// only when the panel has a server and a network. Silence is the correct behaviour on an
// unconfigured panel: the ring keeps working, and nothing retries against a host that does not
// exist.
void nodelog_tick(void);
