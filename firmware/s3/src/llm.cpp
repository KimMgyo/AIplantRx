#include "llm.h"
#include "net.h"
#include "settings.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

static LlmTest s_t{};
static volatile uint32_t s_ok_ms = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

LlmTest llm_test() {
    portENTER_CRITICAL(&s_mux);
    LlmTest t = s_t;
    portEXIT_CRITICAL(&s_mux);
    return t;
}

bool llm_ok() { return s_ok_ms && millis() - s_ok_ms < 600000; }

static void finish(LlmState st, uint32_t ms, const char *model, const char *reply, const char *err) {
    portENTER_CRITICAL(&s_mux);
    s_t.state = st;
    s_t.ms = ms;
    strlcpy(s_t.model, model ? model : "", sizeof s_t.model);
    strlcpy(s_t.reply, reply ? reply : "", sizeof s_t.reply);
    strlcpy(s_t.err, err ? err : "", sizeof s_t.err);
    portEXIT_CRITICAL(&s_mux);
    if (st == LLM_OK) s_ok_ms = millis();
}

// Its own task: a TLS handshake needs ~5 KB of stack on top of the HTTP client.
static void test_task(void *) {
    if (net_state() != NET_UP) { finish(LLM_FAIL, 0, nullptr, nullptr, "Wi-Fi 연결 안 됨"); vTaskDelete(nullptr); }
    String url = g_set.llm_url;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    url += "/v1/chat/completions";
    bool tls = url.startsWith("https://");
    NetworkClientSecure sc;
    WiFiClient pc;
    if (tls) sc.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(20000);
    uint32_t t0 = millis();
    if (!http.begin(tls ? (WiFiClient &)sc : pc, url)) { finish(LLM_FAIL, 0, nullptr, nullptr, "주소 오류"); vTaskDelete(nullptr); }
    http.addHeader("Content-Type", "application/json");
    if (g_set.llm_key.length()) http.addHeader("Authorization", "Bearer " + g_set.llm_key);
    // enable_thinking=false skips Bonsai 2's reasoning block. The server's --reasoning-effort none alone
    // makes its chat template raise ("Unexpected reasoning effort none") and answer 500.
    const char *body = "{\"messages\":[{\"role\":\"user\",\"content\":\"화분에서 키우기 쉬운 식물 이름 하나만 한국어로 답하세요.\"}],"
                       "\"max_tokens\":24,\"temperature\":0.8,\"chat_template_kwargs\":{\"enable_thinking\":false}}";
    int code = http.POST((uint8_t *)body, strlen(body));
    uint32_t ms = millis() - t0;
    if (code != 200) {
        char e[48];
        if (code < 0) snprintf(e, sizeof e, "연결 안 됨 (%d)", code);
        else snprintf(e, sizeof e, "HTTP %d", code);
        http.end();
        finish(LLM_FAIL, ms, nullptr, nullptr, e);
        vTaskDelete(nullptr);
    }
    String resp = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, resp)) { finish(LLM_FAIL, ms, nullptr, nullptr, "응답 형식 오류"); vTaskDelete(nullptr); }
    const char *model = doc["model"] | "";
    const char *content = doc["choices"][0]["message"]["content"] | "";
    String m = model;
    int slash = m.lastIndexOf('/');
    if (slash >= 0) m = m.substring(slash + 1);
    String c = content;
    c.trim();
    c.replace("\n", " ");
    finish(LLM_OK, ms, m.c_str(), c.c_str(), nullptr);
    vTaskDelete(nullptr);
}

void llm_test_start() {
    if (s_t.state == LLM_RUNNING) return;
    portENTER_CRITICAL(&s_mux);
    s_t.state = LLM_RUNNING;
    portEXIT_CRITICAL(&s_mux);
    xTaskCreatePinnedToCore(test_task, "llm", 12288, nullptr, 2, nullptr, 0);
}
