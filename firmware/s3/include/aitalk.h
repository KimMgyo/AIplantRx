// AI note (UI.md 6.3) and the anomaly rules (6.4).
//
// The note is written only when something happens: a new plant, a hand in front of the thermal camera, a
// pump or the light, a value leaving or returning to its target, an anomaly rule, or 10 quiet minutes.
// One at a time, 20 s apart. The LLM streams it in; numbers that are not in the input make it write once
// more. Nothing is made up when the LLM is away: the last note stays and only its age moves on.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct AiNote {
    uint8_t state;          // 0 nothing said yet, 1 being written, 2 done
    bool warn;              // an anomaly or a value out of range (warning colour)
    uint32_t at;            // epoch seconds (millis()/1000 when there is no clock)
    uint32_t ms;            // how long the LLM took
    char why[48];           // the bubble: "열화상에 손", "물 39ml 줌", "흙이 말랐어요"
    char text[360];         // plain text
    char marked[640];       // with LVGL recolor marks on the verified numbers (#RRGGBB 12.3°C#)
};

void ai_init();
AiNote ai_note();
uint32_t ai_gen();
int ai_history(AiNote *out, int max);   // today's notes, newest first
// The rule that is firing now, for the status line ("물이 흙에 들어가지 않아요"), or "".
const char *ai_alert();
void ai_new_plant();                    // identify.cpp: a profile was just applied
