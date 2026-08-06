// The console, kept in memory and served over TCP. See src/hlog.cpp for why the board needs one
// it can hand out over the network: the display owns UART0's pins, so running the UI means
// running with no serial console at all.
#pragma once
#include <stdint.h>

// Ring in PSRAM, the framework log hook, and the tcp/23 listener. Call early in setup(), before
// anything whose output is worth having - lines printed before this runs reach the UART only.
void hlog_init(void);

// Prints to the UART and to the ring. printf semantics. Use this instead of Serial.printf for
// anything a person diagnosing this board from elsewhere would want to read; lines longer than
// 512 bytes are clipped rather than heap-allocated.
//
// NOT named logf: that is <math.h>'s single-precision natural logarithm, and shadowing it would
// compile in most places and then pick the wrong overload somewhere quiet.
void hlogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// How many times a reader fell more than a ring behind and had to be snapped forward. Nonzero
// means the client's link, not the board, is losing lines.
uint32_t hlog_overruns(void);
