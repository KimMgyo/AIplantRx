// Finding the plant (UI.md 3, LLM_ARCHITECTURE.md): camera -> Pl@ntNet -> name table / LLM profile ->
// range check -> saved and sent to the sensor node (PUT /profile) -> introduction.
//
// Runs on its own task; the UI reads find_info() and redraws when find_gen() changes. Nothing on screen
// ever says "failed": when Pl@ntNet or the LLM cannot help, the plant gets the default profile and the
// wording says what it looks like instead (LLM_ARCHITECTURE.md 5).
#pragma once
#include <stddef.h>
#include <stdint.h>

enum FindStep : uint8_t {
    FIND_IDLE,
    FIND_COUNT,      // countdown with the white light on (count = seconds left)
    FIND_PHOTO,      // taking the pictures
    FIND_PLANTNET,   // identifying
    FIND_PROFILE,    // LLM profile (or the saved one)
    FIND_APPLY,      // saving and sending to the sensor node
    FIND_DONE,       // result shown; intro may still be on its way
    FIND_STOPPED,    // could not take a picture: nothing changed
};

enum FindConf : uint8_t { CONF_NONE, CONF_LOW, CONF_MID, CONF_HIGH };

struct FindInfo {
    FindStep step;
    uint8_t count;
    bool retake;            // second round after "not a plant"
    FindConf conf;
    uint8_t score;          // Pl@ntNet score, %
    bool from_llm;          // targets from the LLM (or its saved answer)
    bool cached;            // ...from the saved answer, no LLM call
    uint8_t intro_state;    // 0 not yet, 1 being written, 2 ready
    char headline[96];      // "바질이에요", "허브 종류로 보여요", "관엽식물로 보여요"
    char note[96];          // a line about what happened ("PlantNet이 바빠서 기본 환경으로 했어요")
    uint32_t started_ms, done_ms;
};

void find_init();
// Starts a run; false (and why, in Korean) when it cannot: no camera, no key, already running.
bool find_start(char *why, size_t n);
// "Change kind": the profile step only, for a Pl@ntNet candidate or a saved plant.
bool find_choose(const char *sci, const char *genus, const char *family, uint8_t score);
bool find_busy();
FindInfo find_info();
uint32_t find_gen();
// The first picture of the last run, 200x150 RGB565 (PSRAM), or nullptr.
const uint16_t *find_photo();
// 68x68 RGB565 of the current plant for the main screen (kept in LittleFS), or nullptr.
const uint16_t *find_thumb();

// Pl@ntNet's other candidates from the last run, and plants identified before (newest first).
struct FindChoice {
    char sci[64], genus[32], family[32], ko[48];
    uint8_t score;
};
int find_candidates(FindChoice *out, int max);
int find_recent(FindChoice *out, int max);
