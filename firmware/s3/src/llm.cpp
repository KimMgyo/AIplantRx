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

// Runs on test_task, which holds the TLS lock around it.
static void test_body() {
    if (net_state() != NET_UP) { finish(LLM_FAIL, 0, nullptr, nullptr, "Wi-Fi 연결 안 됨"); return; }
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
    if (!http.begin(tls ? (WiFiClient &)sc : pc, url)) { finish(LLM_FAIL, 0, nullptr, nullptr, "주소 오류"); return; }
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
        return;
    }
    String resp = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, resp)) { finish(LLM_FAIL, ms, nullptr, nullptr, "응답 형식 오류"); return; }
    const char *model = doc["model"] | "";
    const char *content = doc["choices"][0]["message"]["content"] | "";
    String m = model;
    int slash = m.lastIndexOf('/');
    if (slash >= 0) m = m.substring(slash + 1);
    String c = content;
    c.trim();
    c.replace("\n", " ");
    finish(LLM_OK, ms, m.c_str(), c.c_str(), nullptr);
}

// Its own task: a TLS handshake needs ~5 KB of stack on top of the HTTP client.
static void test_task(void *) {
    if (tls_lock(20000)) {
        test_body();
        tls_unlock();
    } else finish(LLM_FAIL, 0, nullptr, nullptr, "다른 연결을 기다리는 중");
    vTaskDelete(nullptr);
}

void llm_test_start() {
    if (s_t.state == LLM_RUNNING) return;
    portENTER_CRITICAL(&s_mux);
    s_t.state = LLM_RUNNING;
    portEXIT_CRITICAL(&s_mux);
    xTaskCreatePinnedToCore(test_task, "llm", 12288, nullptr, 2, nullptr, 0);
}

// ---------------------------------------------------------------- JSON-schema requests

#include <esp_crt_bundle.h>
#include <esp_http_client.h>

static SemaphoreHandle_t s_one;

bool llm_chat(const char *system, const char *user, const char *schema_json, const char *schema_name, int max_tokens,
              float temperature, String &content, char *err, size_t errn, uint32_t timeout_ms, uint32_t wait_ms) {
    content = "";
    if (net_state() != NET_UP) { strlcpy(err, "Wi-Fi 연결 안 됨", errn); return false; }
    if (!s_one) s_one = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_one, pdMS_TO_TICKS(wait_ms)) != pdTRUE) { strlcpy(err, "AI가 다른 일을 하는 중", errn); return false; }
    if (!tls_lock(wait_ms)) { xSemaphoreGive(s_one); strlcpy(err, "다른 연결을 기다리는 중", errn); return false; }
    String url = g_set.llm_url;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    url += "/v1/chat/completions";

    JsonDocument b;
    JsonArray m = b["messages"].to<JsonArray>();
    JsonObject s = m.add<JsonObject>();
    s["role"] = "system";
    s["content"] = system;
    JsonObject u = m.add<JsonObject>();
    u["role"] = "user";
    u["content"] = user;
    b["temperature"] = temperature;
    b["top_p"] = 0.8;
    b["top_k"] = 20;
    b["max_tokens"] = max_tokens;
    // Bonsai 2: thinking off per request (the server's --reasoning-effort none breaks its template).
    b["chat_template_kwargs"]["enable_thinking"] = false;
    JsonObject rf = b["response_format"].to<JsonObject>();
    rf["type"] = "json_schema";
    rf["json_schema"]["name"] = schema_name;
    rf["json_schema"]["strict"] = true;
    rf["json_schema"]["schema"] = serialized(schema_json);
    String body;
    serializeJson(b, body);

    esp_http_client_config_t c = {};
    c.url = url.c_str();
    c.method = HTTP_METHOD_POST;
    c.timeout_ms = timeout_ms;
    if (url.startsWith("https://")) c.crt_bundle_attach = esp_crt_bundle_attach;
    c.buffer_size = 2048;
    c.buffer_size_tx = 2048;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    String auth;
    if (g_set.llm_key.length()) {
        auth = "Bearer " + g_set.llm_key;
        esp_http_client_set_header(h, "Authorization", auth.c_str());
    }
    bool ok = false;
    uint32_t t0 = millis(), t_open = 0, t_head = 0;
    esp_err_t e = esp_http_client_open(h, body.length());
    t_open = millis() - t0;
    if (e != ESP_OK) snprintf(err, errn, "연결 안 됨 (%s)", esp_err_to_name(e));
    else if (esp_http_client_write(h, body.c_str(), body.length()) != (int)body.length()) strlcpy(err, "보내기 실패", errn);
    else {
        esp_http_client_fetch_headers(h);
        t_head = millis() - t0;
        int code = esp_http_client_get_status_code(h);
        String resp;
        char buf[512];
        int n;
        while ((n = esp_http_client_read(h, buf, sizeof buf)) > 0) resp.concat(buf, n);
        if (code != 200) snprintf(err, errn, "HTTP %d", code);
        else {
            JsonDocument filter;
            filter["choices"][0]["message"]["content"] = true;
            JsonDocument d;
            if (deserializeJson(d, resp, DeserializationOption::Filter(filter))) strlcpy(err, "응답 형식 오류", errn);
            else {
                content = d["choices"][0]["message"]["content"] | "";
                ok = content.length() > 0;
                if (!ok) strlcpy(err, "빈 응답", errn);
            }
        }
    }
    esp_http_client_cleanup(h);
    tls_unlock();
    xSemaphoreGive(s_one);
    if (ok) s_ok_ms = millis();
    Serial.printf("[llm] %s %s in %lu ms (connect %lu, headers %lu)%s%s\n", schema_name, ok ? "ok" : "failed",
                  (unsigned long)(millis() - t0), (unsigned long)t_open, (unsigned long)t_head, ok ? "" : ": ", ok ? "" : err);
    return ok;
}

// ---------------------------------------------------------------- streamed requests

bool llm_stream(const char *system, const char *user, const char *grammar, int max_tokens, float temperature, float top_p,
                llm_piece_fn piece, void *arg, String &all, char *err, size_t errn, uint32_t first_ms, uint32_t total_ms) {
    all = "";
    if (net_state() != NET_UP) { strlcpy(err, "Wi-Fi 연결 안 됨", errn); return false; }
    if (!s_one) s_one = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_one, pdMS_TO_TICKS(1000)) != pdTRUE) { strlcpy(err, "AI가 다른 일을 하는 중", errn); return false; }
    if (!tls_lock(1000)) { xSemaphoreGive(s_one); strlcpy(err, "다른 연결을 기다리는 중", errn); return false; }
    String url = g_set.llm_url;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    url += "/v1/chat/completions";
    JsonDocument b;
    JsonArray m = b["messages"].to<JsonArray>();
    JsonObject s = m.add<JsonObject>();
    s["role"] = "system";
    s["content"] = system;
    JsonObject u = m.add<JsonObject>();
    u["role"] = "user";
    u["content"] = user;
    b["stream"] = true;
    b["temperature"] = temperature;
    b["top_p"] = top_p;
    b["max_tokens"] = max_tokens;
    if (grammar) b["grammar"] = grammar;
    b["chat_template_kwargs"]["enable_thinking"] = false;
    String body;
    serializeJson(b, body);

    esp_http_client_config_t c = {};
    c.url = url.c_str();
    c.method = HTTP_METHOD_POST;
    c.timeout_ms = first_ms;
    if (url.startsWith("https://")) c.crt_bundle_attach = esp_crt_bundle_attach;
    c.buffer_size = 2048;
    c.buffer_size_tx = 2048;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Accept", "text/event-stream");
    String auth;
    if (g_set.llm_key.length()) {
        auth = "Bearer " + g_set.llm_key;
        esp_http_client_set_header(h, "Authorization", auth.c_str());
    }
    bool ok = false, done = false;
    uint32_t t0 = millis();
    esp_err_t e = esp_http_client_open(h, body.length());
    if (e != ESP_OK) snprintf(err, errn, "연결 안 됨 (%s)", esp_err_to_name(e));
    else if (esp_http_client_write(h, body.c_str(), body.length()) != (int)body.length()) strlcpy(err, "보내기 실패", errn);
    else {
        esp_http_client_fetch_headers(h);
        int code = esp_http_client_get_status_code(h);
        if (code != 200) snprintf(err, errn, "HTTP %d", code);
        else {
            // Server-sent events: "data: {json}\n\n" per token, "data: [DONE]" at the end.
            String line;
            char buf[256];
            JsonDocument filter;
            filter["choices"][0]["delta"]["content"] = true;
            while (!done && millis() - t0 < total_ms) {
                int n = esp_http_client_read(h, buf, sizeof buf);
                if (n <= 0) break;   // timeout (no first token in first_ms, or a stall) or the end
                if (!all.length() && millis() - t0 > first_ms + 2000) break;
                for (int i = 0; i < n; i++) {
                    if (buf[i] != '\n') { line += buf[i]; continue; }
                    line.trim();
                    if (line.startsWith("data:")) {
                        String d = line.substring(5);
                        d.trim();
                        if (d == "[DONE]") { done = true; break; }
                        JsonDocument j;
                        if (!deserializeJson(j, d, DeserializationOption::Filter(filter))) {
                            const char *p = j["choices"][0]["delta"]["content"] | "";
                            if (*p) {
                                all += p;
                                if (piece) piece(p, arg);
                            }
                        }
                    }
                    line = "";
                }
            }
            ok = all.length() > 0;
            if (!ok) strlcpy(err, "응답 없음", errn);
        }
    }
    esp_http_client_cleanup(h);
    tls_unlock();
    xSemaphoreGive(s_one);
    if (ok) s_ok_ms = millis();
    Serial.printf("[llm] stream %s in %lu ms (%u bytes)%s%s\n", ok ? "ok" : "failed", (unsigned long)(millis() - t0),
                  (unsigned)all.length(), ok ? "" : ": ", ok ? "" : err);
    return ok;
}
