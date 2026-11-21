// The PC's llama-server (OpenAI-compatible). For now: the connection test on the AI server page and the
// "AI server reachable" icon. The AI note (UI.md 6.3) builds on the same request code.
#pragma once
#include <stdint.h>

enum LlmState : uint8_t { LLM_IDLE, LLM_RUNNING, LLM_OK, LLM_FAIL };

struct LlmTest {
    LlmState state;
    uint32_t ms;
    char model[40];
    char reply[96];
    char err[48];
};

void llm_test_start();
LlmTest llm_test();
bool llm_ok();          // the last request succeeded, within 10 minutes
