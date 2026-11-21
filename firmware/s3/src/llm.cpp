#include "llm.h"
#include "net.h"
#include "settings.h"
#include <ArduinoJson.h>
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

#include <esp_http_client.h>
static SemaphoreHandle_t s_one;   // one request at a time (the server has one slot)
static esp_http_client_handle_t conn(uint32_t timeout_ms, bool sse);
static int send_req(esp_http_client_handle_t h, const String &body, uint32_t *t_open, bool *reused, char *err, size_t errn);
static void conn_done(esp_http_client_handle_t h);

// Runs on test_task, which holds s_one and the TLS lock. Same kept connection as the real requests, so
// the second test shows what an AI note costs on an open connection.
static void test_body() {
    if (net_state() != NET_UP) { finish(LLM_FAIL, 0, nullptr, nullptr, "Wi-Fi 연결 안 됨"); return; }
    // enable_thinking=false skips Bonsai 2's reasoning block. The server's --reasoning-effort none alone
    // makes its chat template raise ("Unexpected reasoning effort none") and answer 500.
    String body = "{\"messages\":[{\"role\":\"user\",\"content\":\"화분에서 키우기 쉬운 식물 이름 하나만 한국어로 답하세요.\"}],"
                  "\"max_tokens\":24,\"temperature\":0.8,\"chat_template_kwargs\":{\"enable_thinking\":false}}";
    uint32_t t0 = millis(), t_open = 0;
    bool reused = false;
    char e[48] = "";
    esp_http_client_handle_t h = conn(30000, false);
    int code = h ? send_req(h, body, &t_open, &reused, e, sizeof e) : -1;
    String resp;
    if (code > 0) {
        char buf[512];
        int n;
        while ((n = esp_http_client_read(h, buf, sizeof buf)) > 0) resp.concat(buf, n);
        conn_done(h);
    }
    uint32_t ms = millis() - t0;
    Serial.printf("[llm] test HTTP %d in %lu ms (%s connection, open %lu ms)\n", code, (unsigned long)ms,
                  reused ? "kept" : "new", (unsigned long)t_open);
    if (code != 200) {
        if (code > 0) snprintf(e, sizeof e, "HTTP %d", code);
        else if (!e[0]) strlcpy(e, "연결 안 됨", sizeof e);
        finish(LLM_FAIL, ms, nullptr, nullptr, e);
        return;
    }
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
    if (!s_one) s_one = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_one, pdMS_TO_TICKS(20000)) != pdTRUE) {
        finish(LLM_FAIL, 0, nullptr, nullptr, "AI가 다른 일을 하는 중");
    } else {
        if (tls_lock(20000)) {
            test_body();
            tls_unlock();
        } else finish(LLM_FAIL, 0, nullptr, nullptr, "다른 연결을 기다리는 중");
        xSemaphoreGive(s_one);
    }
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

// ---------------------------------------------------------------- one kept-alive connection
//
// Every request (profile, intro, AI note, test) goes over one HTTPS connection that stays open between
// requests. Through the public Funnel ingress a new connection costs a TLS handshake of 3.5-7 s (S3 and
// PC alike, 2026-11-22); on an open connection the connection test took 1.6 s instead of 10 s. An idle
// connection through the Funnel was still usable after 120 s; past LLM_IDLE_MS it is closed and opened
// again instead of risking a dead one (send_req() also retries once on a new connection). Its big TLS
// buffers are in PSRAM (net.cpp), so keeping it open costs about 10 KB of internal RAM.
static const uint32_t LLM_IDLE_MS = 110000;
static esp_http_client_handle_t s_h;
static String s_h_url;
static uint32_t s_h_used;   // millis() the connection last finished a request cleanly, 0 = not open

static String llm_url() {
    String url = g_set.llm_url;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    return url + "/v1/chat/completions";
}

static void conn_close() {
    if (s_h) esp_http_client_close(s_h);
    s_h_used = 0;
}

// The connection, set up for one request. Called with s_one and the TLS lock held.
static esp_http_client_handle_t conn(uint32_t timeout_ms, bool sse) {
    String url = llm_url();
    if (s_h && url != s_h_url) {
        esp_http_client_cleanup(s_h);
        s_h = nullptr;
        s_h_used = 0;
    }
    if (s_h && s_h_used && millis() - s_h_used > LLM_IDLE_MS) conn_close();
    if (!s_h) {
        esp_http_client_config_t c = {};
        c.url = url.c_str();
        c.method = HTTP_METHOD_POST;
        c.timeout_ms = timeout_ms;
        if (url.startsWith("https://")) c.crt_bundle_attach = esp_crt_bundle_attach;
        c.buffer_size = 2048;
        c.buffer_size_tx = 2048;
        s_h = esp_http_client_init(&c);
        if (!s_h) return nullptr;
        s_h_url = url;
        s_h_used = 0;
    }
    esp_http_client_set_timeout_ms(s_h, timeout_ms);
    esp_http_client_set_method(s_h, HTTP_METHOD_POST);
    esp_http_client_set_header(s_h, "Content-Type", "application/json");
    if (sse) esp_http_client_set_header(s_h, "Accept", "text/event-stream");
    else esp_http_client_delete_header(s_h, "Accept");
    if (g_set.llm_key.length()) esp_http_client_set_header(s_h, "Authorization", ("Bearer " + g_set.llm_key).c_str());
    else esp_http_client_delete_header(s_h, "Authorization");
    return s_h;
}

// Sends the body and reads the response headers. A kept connection that the other end closed meanwhile
// fails at the first write or read: then it is tried once more on a new connection. Returns the HTTP
// status, or -1 with err set. *reused says whether the answer came over an already open connection.
static int send_req(esp_http_client_handle_t h, const String &body, uint32_t *t_open, bool *reused, char *err,
                    size_t errn) {
    for (int attempt = 0; attempt < 2; attempt++) {
        bool was_open = s_h_used != 0;
        uint32_t t0 = millis();
        esp_err_t e = esp_http_client_open(h, body.length());
        *t_open = millis() - t0;
        *reused = was_open;
        int code = -1;
        if (e != ESP_OK) snprintf(err, errn, "연결 안 됨 (%s)", esp_err_to_name(e));
        else if (esp_http_client_write(h, body.c_str(), body.length()) != (int)body.length()) strlcpy(err, "보내기 실패", errn);
        else if (esp_http_client_fetch_headers(h) < 0) strlcpy(err, "응답 없음", errn);
        else code = esp_http_client_get_status_code(h);
        if (code > 0) return code;
        conn_close();
        if (!was_open) return -1;   // a new connection failed: another would too
        Serial.printf("[llm] kept connection was closed by the other end, opening a new one\n");
    }
    return -1;
}

// After a response: keep the connection only when the whole response was read.
static void conn_done(esp_http_client_handle_t h) {
    if (esp_http_client_is_complete_data_received(h)) s_h_used = millis();
    else conn_close();
}

bool llm_chat(const char *system, const char *user, const char *schema_json, const char *schema_name, int max_tokens,
              float temperature, String &content, char *err, size_t errn, uint32_t timeout_ms, uint32_t wait_ms) {
    content = "";
    if (net_state() != NET_UP) { strlcpy(err, "Wi-Fi 연결 안 됨", errn); return false; }
    if (!s_one) s_one = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_one, pdMS_TO_TICKS(wait_ms)) != pdTRUE) { strlcpy(err, "AI가 다른 일을 하는 중", errn); return false; }
    if (!tls_lock(wait_ms)) { xSemaphoreGive(s_one); strlcpy(err, "다른 연결을 기다리는 중", errn); return false; }

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

    bool ok = false, reused = false;
    uint32_t t0 = millis(), t_open = 0, t_head = 0;
    esp_http_client_handle_t h = conn(timeout_ms, false);
    int code = h ? send_req(h, body, &t_open, &reused, err, errn) : -1;
    if (!h) strlcpy(err, "메모리 부족", errn);
    if (code > 0) {
        t_head = millis() - t0;
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
        conn_done(h);
    }
    tls_unlock();
    xSemaphoreGive(s_one);
    if (ok) s_ok_ms = millis();
    Serial.printf("[llm] %s %s in %lu ms (%s connection: open %lu, headers %lu)%s%s\n", schema_name, ok ? "ok" : "failed",
                  (unsigned long)(millis() - t0), reused ? "kept" : "new", (unsigned long)t_open, (unsigned long)t_head,
                  ok ? "" : ": ", ok ? "" : err);
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

    bool ok = false, done = false, reused = false;
    uint32_t t0 = millis(), t_open = 0, t_first = 0;
    esp_http_client_handle_t h = conn(first_ms, true);
    int code = h ? send_req(h, body, &t_open, &reused, err, errn) : -1;
    if (!h) strlcpy(err, "메모리 부족", errn);
    if (code > 0) {
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
                                if (!all.length()) t_first = millis() - t0;
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
            // The chunked body ends just after [DONE] (a zero-length chunk): read it, so the connection is
            // clean for the next request.
            if (done) {
                esp_http_client_set_timeout_ms(h, 3000);
                while (esp_http_client_read(h, buf, sizeof buf) > 0) {}
            }
        }
        conn_done(h);
    }
    tls_unlock();
    xSemaphoreGive(s_one);
    if (ok) s_ok_ms = millis();
    Serial.printf("[llm] stream %s in %lu ms (%s connection: open %lu, first text %lu; %u bytes)%s%s\n", ok ? "ok" : "failed",
                  (unsigned long)(millis() - t0), reused ? "kept" : "new", (unsigned long)t_open, (unsigned long)t_first,
                  (unsigned)all.length(), ok ? "" : ": ", ok ? "" : err);
    return ok;
}

// ---------------------------------------------------------------- stream test (GET /llmstream, devsrv.cpp)
// The AI note's path without needing an identified plant: one short streamed answer, logged.

static void stream_test_task(void *) {
    String all;
    char err[64] = "";
    uint32_t t0 = millis();
    bool ok = llm_stream("You answer in one short Korean sentence.", "물을 준 뒤 흙이 마르는 데 보통 얼마나 걸리나요?", nullptr,
                         40, 0.7f, 0.95f, nullptr, nullptr, all, err, sizeof err);
    Serial.printf("[llm] stream test %s in %lu ms: %s\n", ok ? "ok" : "failed", (unsigned long)(millis() - t0),
                  ok ? all.c_str() : err);
    vTaskDelete(nullptr);
}

void llm_stream_test_start() { xTaskCreatePinnedToCore(stream_test_task, "llmst", 12288, nullptr, 2, nullptr, 0); }
