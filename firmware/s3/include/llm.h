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
void llm_stream_test_start();   // GET /llmstream: one streamed answer, result in the log
bool llm_ok();          // the last request succeeded, within 10 minutes

// One chat completion constrained to a JSON schema (prompts.h). Requests go one at a time (the server has
// one slot, UI.md 6.1): a caller waits up to wait_ms for the previous one. content gets the model's JSON
// text; on failure err gets a short Korean reason and the result is false.
#include <WString.h>
bool llm_chat(const char *system, const char *user, const char *schema_json, const char *schema_name, int max_tokens,
              float temperature, String &content, char *err, size_t errn, uint32_t timeout_ms = 20000,
              uint32_t wait_ms = 30000);

// Streamed chat completion with a GBNF grammar (the AI note, UI.md 6.3). piece() gets each new bit of text
// as it arrives; all gets the whole text. Gives up when nothing has arrived after first_ms.
// first_ms was 4 s: on a new connection through the public Funnel the TLS handshake alone took 0.5-6.8 s
// from a PC and longer from the S3, so every note failed with "응답 없음" (2026-11-22). On the kept
// connection (llm.cpp) the first text comes after about one round trip.
typedef void (*llm_piece_fn)(const char *piece, void *user);
bool llm_stream(const char *system, const char *user, const char *grammar, int max_tokens, float temperature, float top_p,
                llm_piece_fn piece, void *arg, String &all, char *err, size_t errn, uint32_t first_ms = 25000,
                uint32_t total_ms = 45000);
