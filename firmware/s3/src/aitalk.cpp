#include "aitalk.h"
#include "app.h"
#include "llm.h"
#include "net.h"
#include "nodes.h"
#include "prompts.h"
#include "settings.h"
#include "theme.h"
#include <ArduinoJson.h>
#include <freertos/idf_additions.h>
#include <math.h>
#include <time.h>

static const uint32_t REST_MS = 20000;          // between two notes
static const uint32_t HAND_COOLDOWN_MS = 60000;
static const int HISTORY = 40;

enum Kind : uint8_t { K_NONE, K_NEW_PLANT, K_HAND, K_DEVICE, K_VALUE, K_IDLE };

struct Trigger {
    Kind kind;
    bool warn;
    char why[48];
    char rule[24];          // value / anomaly rule name for the LLM
    float a, b;             // trigger values (hand_c / hand_minus_leaf, ml, value / target...)
    float lo, hi;
    char device[12];
};

static SemaphoreHandle_t s_mu;
static AiNote s_now;
static AiNote *s_hist;          // HISTORY notes, in PSRAM (40 KB would not fit in internal RAM)
static int s_nhist;
static volatile uint32_t s_gen;
static volatile bool s_new_plant;
static char s_alert[64];

void ai_new_plant() { s_new_plant = true; }
uint32_t ai_gen() { return s_gen; }
const char *ai_alert() { return s_alert; }

AiNote ai_note() {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    AiNote n = s_now;
    xSemaphoreGive(s_mu);
    return n;
}

int ai_history(AiNote *out, int max) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int k = 0;
    for (int i = s_nhist - 1; s_hist && i >= 0 && k < max; i--) out[k++] = s_hist[i];
    xSemaphoreGive(s_mu);
    return k;
}

static uint32_t now_s() {
    time_t t = time(nullptr);
    return t > 1700000000 ? (uint32_t)t : millis() / 1000;
}

static void hhmm(char *o, size_t n, uint32_t t) {
    time_t tt = t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(o, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// ---------------------------------------------------------------- numbers

// Every number in the input (values, targets, trigger, recent runs) and the differences between the
// readings: what the note may say.
struct Allowed {
    float v[96];
    int n;
    void add(float x) {
        if (!isnan(x) && n < 96) v[n++] = x;
    }
    void add_text(const char *s) {
        while (*s) {
            if (isdigit((unsigned char)*s)) {
                char *e;
                add(strtof(s, &e));
                s = e;
            } else s++;
        }
    }
    bool has(float x) const {
        for (int i = 0; i < n; i++)
            if (fabsf(v[i] - x) <= 0.11f || fabsf(fabsf(v[i]) - x) <= 0.11f || lroundf(v[i]) == x) return true;
        return false;
    }
};

static uint32_t rgb(lv_color_t c) { return lv_color_to32(c) & 0xFFFFFF; }

// Checks each number against the allowed set; returns how many were not in it. With out, writes the
// text with recolor marks on the numbers that check out, coloured by their unit.
static int check_numbers(const char *t, const Allowed &al, char *out, size_t on) {
    int bad = 0;
    size_t o = 0;
    auto put = [&](const char *s, size_t n) {
        if (out && o + n < on) { memcpy(out + o, s, n); o += n; }
    };
    const char *p = t;
    while (*p) {
        bool num_start = isdigit((unsigned char)*p) && (p == t || !isalnum((unsigned char)p[-1]));
        if (!num_start) {
            if (*p == '#') put("##", 2);   // a literal '#' in recolor text
            else put(p, 1);
            p++;
            continue;
        }
        char *e;
        float x = strtof(p, &e);
        bool ok = al.has(x);
        if (!ok) bad++;
        // Unit right after, or EC/pH right before.
        lv_color_t col = P->text.c;
        bool colour = false;
        const char *u = e;
        while (*u == ' ') u++;
        if (!strncmp(u, "°", 2) || !strncmp(u, "도", 3)) { col = P->hl_temp; colour = true; }
        else if (*u == '%' || !strncmp(u, "ml", 2)) { col = P->hl_water; colour = true; }
        else if ((p - t >= 3 && (!strncmp(p - 3, "EC ", 3) || !strncmp(p - 3, "pH ", 3))) ||
                 (p - t >= 2 && (!strncmp(p - 2, "EC", 2) || !strncmp(p - 2, "pH", 2)))) {
            col = P->hl_nutri;
            colour = true;
        }
        if (colour && ok && out) {
            char m[12];
            int k = snprintf(m, sizeof m, "#%06lX ", (unsigned long)rgb(col));
            put(m, k);
            put(p, e - p);
            // The unit belongs to the coloured part too.
            if (!strncmp(u, "°C", 3)) { put(e, u + 3 - e); e = (char *)u + 3; }
            else if (!strncmp(u, "°", 2) || *u == '%') { int l = *u == '%' ? 1 : 2; put(e, u + l - e); e = (char *)u + l; }
            else if (!strncmp(u, "ml", 2)) { put(e, u + 2 - e); e = (char *)u + 2; }
            put("#", 1);
        } else put(p, e - p);
        p = e;
    }
    if (out) out[o < on ? o : on - 1] = 0;
    return bad;
}

// ---------------------------------------------------------------- input

static void build_input(const Trigger &t, const SensorState &s, const ThermalInfo &th, String &json, Allowed &al) {
    JsonDocument d;
    JsonObject tr = d["trigger"].to<JsonObject>();
    switch (t.kind) {
        case K_NEW_PLANT:
            tr["kind"] = "new_plant";
            tr["identification_confidence_pct"] = g_prof.score;
            al.add(g_prof.score);
            break;
        case K_HAND:
            tr["kind"] = "hand";
            tr["hand_c"] = roundf(t.a * 10) / 10;
            tr["hand_minus_leaf"] = roundf(t.b * 10) / 10;
            al.add(roundf(t.a * 10) / 10);
            al.add(roundf(t.b * 10) / 10);
            break;
        case K_DEVICE:
            tr["kind"] = "device";
            tr["device"] = t.device;
            if (t.a > 0) { tr["ml"] = (int)t.a; al.add((int)t.a); }
            break;
        case K_VALUE:
            tr["kind"] = "value";
            tr["rule"] = t.rule;
            if (!isnan(t.a)) { tr["value"] = roundf(t.a * 10) / 10; al.add(roundf(t.a * 10) / 10); }
            if (!isnan(t.b)) { tr["change"] = roundf(t.b * 10) / 10; al.add(roundf(t.b * 10) / 10); }
            if (!isnan(t.lo)) {
                JsonArray r = tr["target"].to<JsonArray>();
                r.add(t.lo);
                r.add(t.hi);
                al.add(t.lo);
                al.add(t.hi);
            }
            break;
        case K_IDLE:
            tr["kind"] = "idle";
            tr["minutes"] = g_set.ai_idle_min;
            al.add(g_set.ai_idle_min);
            break;
        default: break;
    }
    d["plant"] = g_prof.name_ko;
    char tm[8];
    hhmm(tm, sizeof tm, now_s());
    d["time"] = tm;
    d["phase"] = s.day ? "day" : "night";
    auto val = [&](const char *k, float v, int dec) {
        if (isnan(v)) return;
        float r = dec ? roundf(v * 10) / 10 : roundf(v);
        d[k] = r;
        al.add(r);
    };
    val("air_c", s.air_c, 1);
    val("rh", s.rh, 0);
    val("co2", s.co2, 0);
    float leaf = th.live ? th.leaf_c : s.leaf_c;
    val("leaf_c", leaf, 1);
    if (!isnan(leaf) && !isnan(s.air_c)) val("leaf_minus_air", leaf - s.air_c, 1);
    val("soil_pct", s.soil_pct, 0);
    val("ec", s.ec, 1);
    val("ph", s.ph, 1);
    JsonObject tg = d["targets"].to<JsonObject>();
    auto pair = [&](const char *k, float a, float b) {
        JsonArray r = tg[k].to<JsonArray>();
        r.add(a);
        r.add(b);
        al.add(a);
        al.add(b);
    };
    const Profile &p = g_prof;
    pair(s.day ? "temp_day_c" : "temp_night_c", s.day ? p.day_lo : p.night_lo, s.day ? p.day_hi : p.night_hi);
    pair("soil_moisture_pct", p.soil_lo, p.soil_hi);
    pair("soil_ec", p.ec_lo, p.ec_hi);
    pair("soil_ph", p.ph_lo, p.ph_hi);
    // Differences a sentence might use: reading minus each end of its target.
    if (!isnan(s.air_c)) { al.add(fabsf(s.air_c - (s.day ? p.day_lo : p.night_lo))); al.add(fabsf(s.air_c - (s.day ? p.day_hi : p.night_hi))); }
    if (!isnan(s.soil_pct)) { al.add(fabsf(s.soil_pct - p.soil_lo)); al.add(fabsf(s.soil_pct - p.soil_hi)); }
    JsonArray rec = d["recent"].to<JsonArray>();
    for (int i = s.nev - 1, k = 0; i >= 0 && k < 3; i--, k++) {
        char r[40];
        const char *nm = s.ev[i].kind == EV_WATER ? "물" : s.ev[i].kind == EV_NUTR ? "물약" : "pH 조절";
        snprintf(r, sizeof r, "%02u:%02u %s %uml", s.ev[i].at_min / 60, s.ev[i].at_min % 60, nm, s.ev[i].ml);
        rec.add(r);
        al.add_text(r);
    }
    JsonArray said = d["said"].to<JsonArray>();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = s_nhist - 1, k = 0; s_hist && i >= 0 && k < 3; i--, k++) said.add(s_hist[i].text);
    xSemaphoreGive(s_mu);
    al.add(1); al.add(2); al.add(3);   // "한 번", "두 가지": small counts are not readings
    serializeJson(d, json);
}

// ---------------------------------------------------------------- writing

struct Writing {
    String text;
};

static void on_piece(const char *piece, void *arg) {
    Writing *w = (Writing *)arg;
    w->text += piece;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strlcpy(s_now.text, w->text.c_str(), sizeof s_now.text);
    strlcpy(s_now.marked, w->text.c_str(), sizeof s_now.marked);   // colours come when it is checked
    xSemaphoreGive(s_mu);
    s_gen++;
}

// 1 written, 0 the LLM did not answer, -1 the LLM was busy with something else (try again).
static int say(const Trigger &t) {
    SensorState s = sensor_get();
    ThermalInfo th = thermal_info();
    String input;
    Allowed al{};
    build_input(t, s, th, input, al);
    AiNote prev = ai_note();
    uint32_t t0 = millis();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_now.state = 1;
    s_now.warn = t.warn;
    strlcpy(s_now.why, t.why, sizeof s_now.why);
    s_now.text[0] = s_now.marked[0] = 0;
    xSemaphoreGive(s_mu);
    s_gen++;
    String out;
    char err[64];
    bool ok = false;
    bool busy = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        Writing w;
        if (!llm_stream(PROMPT_TALK_SYSTEM, input.c_str(), TALK_GRAMMAR, 120, 0.8f, 0.95f, on_piece, &w, out, err, sizeof err)) {
            busy = !ok && strstr(err, "다른");
            break;
        }
        out.trim();
        int bad = check_numbers(out.c_str(), al, nullptr, 0);
        Serial.printf("[ai] %s (%d unknown numbers)\n", out.c_str(), bad);
        ok = true;
        if (!bad) break;   // otherwise write once more; a second miss keeps the text, those numbers uncoloured
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (ok) {
        s_now.state = 2;
        s_now.at = now_s();
        s_now.ms = millis() - t0;
        strlcpy(s_now.text, out.c_str(), sizeof s_now.text);
        check_numbers(out.c_str(), al, s_now.marked, sizeof s_now.marked);
        if (s_hist) {
            if (s_nhist == HISTORY) { memmove(s_hist, s_hist + 1, sizeof(AiNote) * (HISTORY - 1)); s_nhist--; }
            s_hist[s_nhist++] = s_now;
        }
    } else {
        s_now = prev;   // nothing made up: the last note stays
        Serial.printf("[ai] no note: %s\n", err);
    }
    xSemaphoreGive(s_mu);
    s_gen++;
    return ok ? 1 : busy ? -1 : 0;
}

// ---------------------------------------------------------------- rules

struct Range {
    const char *name;
    bool out, have;
};

static void set_alert(const char *a) { strlcpy(s_alert, a, sizeof s_alert); }

static void task(void *) {
    uint32_t last_said = millis(), last_hand = 0;
    float peak_hist[60];
    int ph_n = 0, ph_i = 0;
    SensorState prev{};
    bool have_prev = false;
    // Range states: air, soil, ec, ph.
    int8_t was[4] = {0, 0, 0, 0};    // -1 below, 0 in, 1 above
    uint32_t leaf_hot_since = 0;
    bool leaf_hot_said = false, co2_said = false, lost_said = false;
    uint32_t water_at = 0;
    float water_soil0 = NAN;
    uint16_t water_ml = 0;
    uint32_t last_sensor_ok = millis();
    int day = -1;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // A new day: today's notes start over.
        time_t tt = time(nullptr);
        struct tm tm;
        localtime_r(&tt, &tm);
        if (tt > 1700000000 && tm.tm_yday != day) {
            if (day >= 0) { xSemaphoreTake(s_mu, portMAX_DELAY); s_nhist = 0; xSemaphoreGive(s_mu); }
            day = tm.tm_yday;
        }
        SensorState s = sensor_get();
        ThermalInfo th = thermal_info();
        const Profile &p = g_prof;
        if (s.live) last_sensor_ok = millis();

        // Hand: the thermal peak over 30 °C and 4 °C above its last-minute average.
        float avg = NAN;
        if (ph_n) { avg = 0; for (int i = 0; i < ph_n; i++) avg += peak_hist[i]; avg /= ph_n; }
        bool hand = th.live && !isnan(avg) && ph_n >= 20 && th.peak_c > 30 && th.peak_c > avg + 4;
        if (th.live) {
            peak_hist[ph_i] = hand ? (isnan(avg) ? th.peak_c : avg) : th.peak_c;   // a hand does not raise the baseline
            ph_i = (ph_i + 1) % 60;
            if (ph_n < 60) ph_n++;
        }

        Trigger t{};
        t.lo = t.hi = t.a = t.b = NAN;
        // Anomaly rules and ranges are watched all the time; what is said waits for its turn below.
        char alert[64] = "";
        Trigger value{};
        value.lo = value.hi = value.a = value.b = NAN;
        if (s.live) {
            // Water not taken up: 10 minutes after watering, soil moisture moved less than 2 points.
            bool new_water = have_prev && s.nev > prev.nev && s.ev[s.nev - 1].kind == EV_WATER;
            if (new_water) { water_at = millis(); water_soil0 = prev.soil_pct; water_ml = s.ev[s.nev - 1].ml; }
            if (water_at && millis() - water_at > 600000) {
                if (!isnan(water_soil0) && !isnan(s.soil_pct) && s.soil_pct - water_soil0 < 2) {
                    value.kind = K_VALUE; value.warn = true;
                    strlcpy(value.rule, "water_not_absorbed", sizeof value.rule);
                    strlcpy(value.why, "물이 안 스며들어요", sizeof value.why);
                    value.a = water_ml;
                    value.b = s.soil_pct - water_soil0;
                    strlcpy(alert, "물이 흙에 들어가지 않아요 · 물통과 센서를 봐 주세요", sizeof alert);
                }
                water_at = 0;
            }
            // Leaf warmer than the air by 2 °C for 10 minutes.
            float leaf = th.live ? th.leaf_c : s.leaf_c;
            if (!isnan(leaf) && !isnan(s.air_c) && leaf - s.air_c > 2) {
                if (!leaf_hot_since) leaf_hot_since = millis();
                if (millis() - leaf_hot_since > 600000) {
                    if (!alert[0]) strlcpy(alert, "잎이 공기보다 따뜻해요 · 물이 부족할 수 있어요", sizeof alert);
                    if (!leaf_hot_said && !value.kind) {
                        value.kind = K_VALUE; value.warn = true;
                        strlcpy(value.rule, "leaf_hot", sizeof value.rule);
                        strlcpy(value.why, "잎이 뜨거워요", sizeof value.why);
                        value.a = leaf - s.air_c;
                        leaf_hot_said = true;
                    }
                }
            } else { leaf_hot_since = 0; leaf_hot_said = false; }
            // CO2.
            if (!isnan(s.co2) && s.co2 > 1500) {
                if (!alert[0]) strlcpy(alert, "CO2가 높아요 · 환기해 주세요", sizeof alert);
                if (!co2_said && !value.kind) {
                    value.kind = K_VALUE; value.warn = true;
                    strlcpy(value.rule, "co2_high", sizeof value.rule);
                    strlcpy(value.why, "환기가 필요해요", sizeof value.why);
                    value.a = s.co2;
                    co2_said = true;
                }
            } else if (isnan(s.co2) || s.co2 < 1400) co2_said = false;
            // A sensor the node says is not answering.
            bool lost = !s.ok_soil || !s.ok_scd41 || !s.ok_mlx;
            if (lost) {
                if (!alert[0]) strlcpy(alert, !s.ok_soil ? "흙 센서가 응답하지 않아요" : !s.ok_scd41 ? "온습도 센서가 응답하지 않아요" : "열화상 카메라가 응답하지 않아요", sizeof alert);
                if (!lost_said && !value.kind) {
                    value.kind = K_VALUE; value.warn = true;
                    strlcpy(value.rule, !s.ok_soil ? "soil_sensor_lost" : !s.ok_scd41 ? "air_sensor_lost" : "thermal_lost", sizeof value.rule);
                    strlcpy(value.why, "센서 확인", sizeof value.why);
                    lost_said = true;
                }
            } else lost_said = false;
            // Targets: leaving or coming back (air, soil, EC, pH).
            const float lo[4] = {s.day ? p.day_lo : p.night_lo, p.soil_lo, p.ec_lo, p.ph_lo};
            const float hi[4] = {s.day ? p.day_hi : p.night_hi, p.soil_hi, p.ec_hi, p.ph_hi};
            const float v[4] = {s.air_c, s.soil_pct, s.ec, s.ph};
            static const char *const RULE[4][3] = {{"air_cold", "air_ok", "air_hot"}, {"soil_dry", "soil_ok", "soil_wet"},
                                                   {"ec_low", "ec_ok", "ec_high"}, {"ph_low", "ph_ok", "ph_high"}};
            static const char *const WHY[4][3] = {{"온도가 낮아요", "온도가 돌아왔어요", "온도가 높아요"},
                                                  {"흙이 말랐어요", "흙 수분이 돌아왔어요", "흙이 젖었어요"},
                                                  {"양분이 적어요", "양분이 돌아왔어요", "양분이 많아요"},
                                                  {"pH가 낮아요", "pH가 돌아왔어요", "pH가 높아요"}};
            for (int i = 0; i < 4; i++) {
                if (isnan(v[i])) continue;
                // A little hysteresis so a value sitting on the edge does not talk every minute.
                float m = (hi[i] - lo[i]) * 0.05f;
                int8_t now = v[i] < lo[i] - (was[i] == -1 ? -m : m) ? -1 : v[i] > hi[i] + (was[i] == 1 ? -m : m) ? 1 : 0;
                if (now != was[i] && have_prev && !value.kind) {
                    value.kind = K_VALUE;
                    value.warn = now != 0;
                    strlcpy(value.rule, RULE[i][now + 1], sizeof value.rule);
                    strlcpy(value.why, WHY[i][now + 1], sizeof value.why);
                    value.a = v[i];
                    value.lo = lo[i];
                    value.hi = hi[i];
                }
                was[i] = now;
                if (now && !alert[0] && i == 1 && now < 0) strlcpy(alert, s.water ? "흙이 말라서 물 주는 중" : "흙이 말랐어요", sizeof alert);
            }
        } else if (millis() - last_sensor_ok > 60000 && node_info(ROLE_NODE).online) {
            strlcpy(alert, "센서 노드 값이 오지 않아요", sizeof alert);
        }
        set_alert(alert);

        if (!g_set.ai_on || !g_prof.identified) { have_prev = s.live; if (s.live) prev = s; continue; }
        bool rested = millis() - last_said > REST_MS;
        if (s_new_plant) { t.kind = K_NEW_PLANT; strlcpy(t.why, "새 식물", sizeof t.why); }
        else if (hand && g_set.ai_hand && millis() - last_hand > HAND_COOLDOWN_MS) {
            t.kind = K_HAND;
            strlcpy(t.why, "열화상에 손", sizeof t.why);
            float leaf = th.leaf_c;
            t.a = th.peak_c;
            t.b = isnan(leaf) ? NAN : th.peak_c - leaf;
        } else if (g_set.ai_device && s.live && have_prev) {
            // A pump run that ended (a new event), or the light switching.
            if (s.nev > prev.nev) {
                const auto &e = s.ev[s.nev - 1];
                t.kind = K_DEVICE;
                t.a = e.ml;
                strlcpy(t.device, e.kind == EV_WATER ? "water" : e.kind == EV_NUTR ? "fertiliser" : "ph", sizeof t.device);
                snprintf(t.why, sizeof t.why, "%s %uml %s", e.kind == EV_WATER ? "물" : e.kind == EV_NUTR ? "물약" : "pH",
                         e.ml, e.kind == EV_WATER ? "줌" : "넣음");
            } else if (s.light != prev.light && !s.capture && !prev.capture) {
                t.kind = K_DEVICE;
                strlcpy(t.device, s.light ? "light_on" : "light_off", sizeof t.device);
                strlcpy(t.why, s.light ? "조명 켬" : "조명 끔", sizeof t.why);
            }
        }
        if (!t.kind && value.kind && g_set.ai_values) t = value;
        if (!t.kind && millis() - last_said > (uint32_t)g_set.ai_idle_min * 60000) {
            t.kind = K_IDLE;
            snprintf(t.why, sizeof t.why, "%u분마다", g_set.ai_idle_min);
        }
        have_prev = s.live;
        if (s.live) prev = s;
        if (!t.kind || !rested) continue;   // a device event while resting is dropped; a value change will come again
        if (t.kind == K_HAND) last_hand = millis();
        int r = say(t);
        if (r < 0) continue;   // the LLM was busy (the plant's introduction): the same trigger next second
        if (t.kind == K_NEW_PLANT) s_new_plant = false;
        last_said = millis();
    }
}

void ai_init() {
    s_mu = xSemaphoreCreateMutex();
    s_hist = (AiNote *)ps_malloc_or_null(sizeof(AiNote) * HISTORY);
    // Internal stack: it opens TCP connections (lwIP asserts against PSRAM stacks there).
    xTaskCreatePinnedToCore(task, "ai", 12288, nullptr, 1, nullptr, 0);
}
