#include "control.h"
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <aipnode.h>
#include <sys/time.h>
#include <time.h>

#define PIN_PUMP_WATER 26   // LR7843 module inputs, HIGH = on (gate pull-down keeps them off in reset)
#define PIN_PUMP_NUTR 27
#define PIN_PUMP_PH 32      // pH-down only
#define PIN_RING 23         // NeoPixel ring, 24 LEDs
#define RING_N 24

// Hardware safety limits: firmware constants, never from the profile (ARCHITECTURE.md 2.3).
enum { P_WATER, P_NUTR, P_PH, P_N };
static const uint8_t PUMP_PIN[P_N] = {PIN_PUMP_WATER, PIN_PUMP_NUTR, PIN_PUMP_PH};
static const char *PUMP_NAME[P_N] = {"water", "nutr", "ph"};
static const uint32_t MAX_RUN_MS = 15000;               // one run
static const uint16_t DAY_MAX_ML[P_N] = {600, 40, 20};  // per day (KST)
static const float ML_PER_S[P_N] = {13.0f, 13.0f, 13.0f};   // not calibrated yet: measure with the real pumps
static const uint32_t CAPTURE_MS = 30000;
static const uint8_t GROW_BRIGHTNESS = 80;              // of 255: about 0.5 A of the ring's 1.44 A

struct Range { float lo, hi; };
struct Profile {
    char grade[8];
    char name_ko[48], sci[64], family[48];
    int score;
    Range temp_day, temp_night, rh, soil, ec, ph;
    uint16_t light_on, light_off;   // minutes after 0:00 KST
    uint16_t water_days, water_ml, nutr_days, nutr_ml;
};

static SemaphoreHandle_t s_mu;
static Profile P;
static Adafruit_NeoPixel s_ring(RING_N, PIN_RING, NEO_GRB + NEO_KHZ800);

static int s_run = -1;                    // the running pump, -1 none
static uint32_t s_run_start, s_run_ms;
static time_t s_run_epoch;
static uint16_t s_ml_today[P_N];
static time_t s_last_water;
struct Ev { uint8_t kind; uint16_t at_min, len_s, ml; };
static Ev s_ev[16];
static int s_nev;
static int s_day = -1;                    // KST day of year the counters belong to

enum LightMode { LM_SCHEDULE, LM_ON, LM_OFF, LM_CAPTURE };
static const char *LM_NAME[] = {"schedule", "on", "off", "capture"};
static LightMode s_mode = LM_SCHEDULE;
static bool s_manual_sched;               // the schedule's state when on/off was set by hand
static uint32_t s_capture_ms;
static int s_ring_shown = -1;             // 0 off, 1 grow, 2 capture
static volatile bool s_hold;              // outputs held off (OTA)

// ---------------------------------------------------------------- clock (KST)

static bool clock_ok() { return time(nullptr) > 1700000000; }

static int kst_min(time_t t, int *yday = nullptr) {
    struct tm tm;
    localtime_r(&t, &tm);
    if (yday) *yday = tm.tm_yday + 1000 * tm.tm_year;
    return tm.tm_hour * 60 + tm.tm_min;
}

static bool sched_on() {
    if (!clock_ok()) return true;   // no clock yet: keep the plant lit
    int m = kst_min(time(nullptr));
    int on = P.light_on, off = P.light_off;
    return on <= off ? (m >= on && m < off) : (m >= on || m < off);
}

// ---------------------------------------------------------------- profile

static void profile_default(Profile &p) {
    memset(&p, 0, sizeof p);
    strlcpy(p.grade, "default", sizeof p.grade);
    p.temp_day = {20, 28};
    p.temp_night = {18, 22};
    p.rh = {60, 80};
    p.soil = {40, 60};
    p.ec = {0.8f, 1.4f};
    p.ph = {5.5f, 6.5f};
    p.light_on = 6 * 60;
    p.light_off = 18 * 60;
    p.water_days = 2;
    p.water_ml = 100;
    p.nutr_days = 7;
    p.nutr_ml = 10;
}

static bool get_range(JsonVariantConst v, Range &r) {
    if (!v.is<JsonArrayConst>() || v.size() != 2 || !v[0].is<float>() || !v[1].is<float>()) return false;
    r.lo = v[0].as<float>();
    r.hi = v[1].as<float>();
    return isfinite(r.lo) && isfinite(r.hi);
}

static bool get_hhmm(JsonVariantConst v, uint16_t &m) {
    const char *s = v.as<const char *>();
    int h, mi;
    if (!s || sscanf(s, "%d:%d", &h, &mi) != 2 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    m = h * 60 + mi;
    return true;
}

static bool get_num(JsonVariantConst v, uint16_t &out) {
    if (!v.is<float>()) return false;
    float f = v.as<float>();
    if (!isfinite(f) || f < 0 || f > 10000) return false;
    out = (uint16_t)lroundf(f);
    return true;
}

static void clamp_range(Range &r, float lo, float hi) {
    if (r.lo > r.hi) { float t = r.lo; r.lo = r.hi; r.hi = t; }
    r.lo = constrain(r.lo, lo, hi);
    r.hi = constrain(r.hi, lo, hi);
}

// The firmware's safety ranges (INTERFACES.md 2, PUT /profile).
static void clamp(Profile &p) {
    clamp_range(p.temp_day, 10, 32);
    clamp_range(p.temp_night, 10, 32);
    clamp_range(p.rh, 20, 95);
    clamp_range(p.soil, 10, 80);
    clamp_range(p.ec, 0.3f, 3.0f);
    clamp_range(p.ph, 5.0f, 7.5f);
    int len = ((int)p.light_off - p.light_on + 1440) % 1440;
    if (len == 0) len = 1440;
    if (len < 8 * 60) p.light_off = (p.light_on + 8 * 60) % 1440;
    if (len > 16 * 60) p.light_off = (p.light_on + 16 * 60) % 1440;
    p.water_days = constrain(p.water_days, 1, 21);
    p.water_ml = constrain(p.water_ml, 20, 300);
    p.nutr_days = constrain(p.nutr_days, 3, 30);
    p.nutr_ml = constrain(p.nutr_ml, 0, 20);
    if (strcmp(p.grade, "llm")) strlcpy(p.grade, "default", sizeof p.grade);
}

// Required fields missing or malformed: false (and *why says which).
static bool from_json(JsonObjectConst o, Profile &p, const char **why) {
    profile_default(p);
    const char *g = o["grade"].as<const char *>();
    if (!g || (strcmp(g, "llm") && strcmp(g, "default"))) { *why = "grade"; return false; }
    strlcpy(p.grade, g, sizeof p.grade);
    if (!get_range(o["temp_day"], p.temp_day)) { *why = "temp_day"; return false; }
    if (!get_range(o["temp_night"], p.temp_night)) { *why = "temp_night"; return false; }
    if (!get_range(o["rh"], p.rh)) { *why = "rh"; return false; }
    if (!get_range(o["soil"], p.soil)) { *why = "soil"; return false; }
    if (!get_range(o["ec"], p.ec)) { *why = "ec"; return false; }
    if (!get_range(o["ph"], p.ph)) { *why = "ph"; return false; }
    if (!get_hhmm(o["light_on"], p.light_on)) { *why = "light_on"; return false; }
    if (!get_hhmm(o["light_off"], p.light_off)) { *why = "light_off"; return false; }
    if (!get_num(o["water_interval_days"], p.water_days)) { *why = "water_interval_days"; return false; }
    if (!get_num(o["water_ml"], p.water_ml)) { *why = "water_ml"; return false; }
    if (!get_num(o["nutr_interval_days"], p.nutr_days)) { *why = "nutr_interval_days"; return false; }
    if (!get_num(o["nutr_ml"], p.nutr_ml)) { *why = "nutr_ml"; return false; }
    strlcpy(p.name_ko, o["name_ko"] | "", sizeof p.name_ko);
    strlcpy(p.sci, o["sci"] | "", sizeof p.sci);
    strlcpy(p.family, o["family"] | "", sizeof p.family);
    p.score = o["score"] | 0;
    clamp(p);
    return true;
}

static void put_range(JsonObject o, const char *k, const Range &r) {
    JsonArray a = o[k].to<JsonArray>();
    a.add(roundf(r.lo * 100) / 100);
    a.add(roundf(r.hi * 100) / 100);
}

static void put_hhmm(JsonObject o, const char *k, uint16_t m) {
    char b[6];
    snprintf(b, sizeof b, "%02u:%02u", m / 60, m % 60);
    o[k] = b;
}

static void to_json(JsonObject o, const Profile &p) {
    o["grade"] = p.grade;
    if (p.name_ko[0]) o["name_ko"] = p.name_ko;
    if (p.sci[0]) o["sci"] = p.sci;
    if (p.family[0]) o["family"] = p.family;
    if (p.score) o["score"] = p.score;
    put_range(o, "temp_day", p.temp_day);
    put_range(o, "temp_night", p.temp_night);
    put_range(o, "rh", p.rh);
    put_range(o, "soil", p.soil);
    put_range(o, "ec", p.ec);
    put_range(o, "ph", p.ph);
    put_hhmm(o, "light_on", p.light_on);
    put_hhmm(o, "light_off", p.light_off);
    o["water_interval_days"] = p.water_days;
    o["water_ml"] = p.water_ml;
    o["nutr_interval_days"] = p.nutr_days;
    o["nutr_ml"] = p.nutr_ml;
}

static void profile_save() {
    JsonDocument d;
    to_json(d.to<JsonObject>(), P);
    String s;
    serializeJson(d, s);
    Preferences pr;
    pr.begin("prof", false);
    pr.putString("json", s);
    pr.end();
}

static void profile_load() {
    Preferences pr;
    String s;
    if (pr.begin("prof", true)) {
        s = pr.getString("json", "");
        pr.end();
    }
    JsonDocument d;
    const char *why = "";
    if (s.length() && !deserializeJson(d, s) && from_json(d.as<JsonObjectConst>(), P, &why)) return;
    profile_default(P);
}

// ---------------------------------------------------------------- pumps

static void pump_stop_locked() {
    if (s_run < 0) return;
    digitalWrite(PUMP_PIN[s_run], LOW);
    uint32_t ran = millis() - s_run_start;
    uint16_t ml = (uint16_t)lroundf(ran / 1000.0f * ML_PER_S[s_run]);
    s_ml_today[s_run] += ml;
    if (s_run == P_WATER && s_run_epoch) s_last_water = s_run_epoch;
    if (s_nev == 16) { memmove(s_ev, s_ev + 1, sizeof s_ev[0] * 15); s_nev = 15; }
    s_ev[s_nev++] = {(uint8_t)s_run, (uint16_t)(s_run_epoch ? kst_min(s_run_epoch) : 0),
                     (uint16_t)((ran + 500) / 1000), ml};
    aipnode_logf("[pump] %s off after %lu ms (~%u ml)\n", PUMP_NAME[s_run], (unsigned long)ran, ml);
    s_run = -1;
}

static void all_off_locked() {
    pump_stop_locked();
    for (int i = 0; i < P_N; i++) digitalWrite(PUMP_PIN[i], LOW);
}

// ---------------------------------------------------------------- light

static bool light_wanted() {
    switch (s_mode) {
        case LM_ON: return true;
        case LM_OFF: return false;
        case LM_CAPTURE: return true;
        default: return sched_on();
    }
}

static void ring_update() {
    int want = s_hold ? 0 : s_mode == LM_CAPTURE ? 2 : light_wanted() ? 1 : 0;
    if (want == s_ring_shown) return;
    s_ring_shown = want;
    if (want == 2) {
        s_ring.setBrightness(255);
        s_ring.fill(s_ring.Color(255, 255, 255));
    } else if (want == 1) {
        s_ring.setBrightness(GROW_BRIGHTNESS);
        s_ring.fill(s_ring.Color(255, 190, 140));   // warm white
    } else {
        s_ring.clear();
    }
    s_ring.show();
}

// ---------------------------------------------------------------- state

static void state_json(JsonObject o) {
    time_t now = time(nullptr);
    o["api"] = 1;
    if (clock_ok()) o["t"] = (uint32_t)now;
    o["day"] = sched_on();
    o["light"] = s_ring_shown > 0;
    o["water"] = s_run == P_WATER;
    o["nutr"] = s_run == P_NUTR;
    o["ph_pump"] = s_run == P_PH;
    o["capture"] = s_mode == LM_CAPTURE;
    o["water_ml_today"] = s_ml_today[P_WATER];
    o["nutr_ml_today"] = s_ml_today[P_NUTR];
    o["ph_ml_today"] = s_ml_today[P_PH];
    o["last_water"] = (uint32_t)s_last_water;
    o["next_water"] = 0;   // no schedule runs in this version
    o["next_nutr"] = 0;
    JsonArray ev = o["events"].to<JsonArray>();
    for (int i = 0; i < s_nev; i++) {
        JsonObject e = ev.add<JsonObject>();
        e["kind"] = PUMP_NAME[s_ev[i].kind];
        e["at_min"] = s_ev[i].at_min;
        e["len_s"] = s_ev[i].len_s;
        e["ml"] = s_ev[i].ml;
    }
    JsonObject s = o["sensors"].to<JsonObject>();
    s["scd41"] = false;
    s["mlx"] = false;
    s["soil"] = false;
    to_json(o["profile"].to<JsonObject>(), P);
}

void control_print(Print &out) {
    JsonDocument d;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    state_json(d.to<JsonObject>());
    xSemaphoreGive(s_mu);
    serializeJson(d, out);
    out.println();
}

// ---------------------------------------------------------------- HTTP

static esp_err_t h_state(httpd_req_t *req) {
    JsonDocument d;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    state_json(d.to<JsonObject>());
    xSemaphoreGive(s_mu);
    return aipnode_send_json(req, d);
}

static esp_err_t h_thermal(httpd_req_t *req) {
    return aipnode_send_text(req, "503 Service Unavailable", "no thermal camera");
}

static esp_err_t h_profile(httpd_req_t *req) {
    static char body[2048];
    int n = aipnode_read_body(req, body, sizeof body);
    JsonDocument in;
    if (n <= 0 || deserializeJson(in, body, n)) return aipnode_send_text(req, "400 Bad Request", "JSON body");
    Profile p;
    const char *why = "";
    if (!from_json(in.as<JsonObjectConst>(), p, &why)) {
        char b[64];
        snprintf(b, sizeof b, "missing or bad: %s", why);
        return aipnode_send_text(req, "400 Bad Request", b);
    }
    uint32_t t = in["t"] | 0;
    if (t > 1700000000 && !clock_ok()) {   // no NTP yet: take the S3's clock
        timeval tv = {(time_t)t, 0};
        settimeofday(&tv, nullptr);
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    P = p;
    xSemaphoreGive(s_mu);
    profile_save();
    aipnode_logf("[profile] %s %s %s\n", P.grade, P.sci, P.name_ko);
    JsonDocument r;
    r["ok"] = true;
    to_json(r["profile"].to<JsonObject>(), P);
    return aipnode_send_json(req, r);
}

static esp_err_t h_pump(httpd_req_t *req) {
    char body[160];
    int n = aipnode_read_body(req, body, sizeof body);
    JsonDocument in;
    if (n <= 0 || deserializeJson(in, body, n)) return aipnode_send_text(req, "400 Bad Request", "JSON body");
    const char *name = in["pump"] | "";
    int k = -1;
    for (int i = 0; i < P_N; i++)
        if (!strcmp(name, PUMP_NAME[i])) k = i;
    if (k < 0) return aipnode_send_text(req, "400 Bad Request", "pump: water | nutr | ph");
    float ms;
    if (in["ml"].is<float>()) ms = in["ml"].as<float>() / ML_PER_S[k] * 1000;
    else if (in["s"].is<float>()) ms = in["s"].as<float>() * 1000;
    else return aipnode_send_text(req, "400 Bad Request", "ml or s");
    if (!(ms > 0)) return aipnode_send_text(req, "400 Bad Request", "ml or s must be > 0");

    const char *busy = nullptr;
    uint32_t run_ms = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int left_ml = (int)DAY_MAX_ML[k] - s_ml_today[k];
    if (s_hold || aipnode_ota_busy()) busy = "updating";
    else if (s_run >= 0) busy = "another pump is running";
    else if (s_mode == LM_CAPTURE) busy = "capture light is on";
    else if (left_ml <= 0) busy = "daily limit reached";
    else {
        float cap = min((float)MAX_RUN_MS, left_ml / ML_PER_S[k] * 1000);
        run_ms = (uint32_t)min(ms, cap);
        s_run = k;
        s_run_start = millis();
        s_run_ms = run_ms;
        s_run_epoch = clock_ok() ? time(nullptr) : 0;
        digitalWrite(PUMP_PIN[k], HIGH);
    }
    xSemaphoreGive(s_mu);
    if (busy) {
        JsonDocument r;
        r["ok"] = false;
        r["error"] = busy;
        return aipnode_send_json(req, r, "409 Conflict");
    }
    aipnode_logf("[pump] %s on for %lu ms\n", PUMP_NAME[k], (unsigned long)run_ms);
    JsonDocument r;
    r["ok"] = true;
    r["ms"] = run_ms;
    r["ml"] = lroundf(run_ms / 1000.0f * ML_PER_S[k]);
    return aipnode_send_json(req, r);
}

static esp_err_t h_light(httpd_req_t *req) {
    char body[96];
    int n = aipnode_read_body(req, body, sizeof body);
    JsonDocument in;
    if (n <= 0 || deserializeJson(in, body, n)) return aipnode_send_text(req, "400 Bad Request", "JSON body");
    const char *m = in["mode"] | "";
    int k = -1;
    for (int i = 0; i < 4; i++)
        if (!strcmp(m, LM_NAME[i])) k = i;
    if (k < 0) return aipnode_send_text(req, "400 Bad Request", "mode: capture | schedule | on | off");
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_mode = (LightMode)k;
    if (s_mode == LM_CAPTURE) {
        pump_stop_locked();   // no pump runs under the capture light (12 V budget)
        s_capture_ms = millis();
    }
    if (s_mode == LM_ON || s_mode == LM_OFF) s_manual_sched = sched_on();
    ring_update();
    bool lit = s_ring_shown > 0;
    xSemaphoreGive(s_mu);
    JsonDocument r;
    r["ok"] = true;
    r["mode"] = LM_NAME[k];
    r["light"] = lit;
    return aipnode_send_json(req, r);
}

void control_register(httpd_handle_t h) {
    httpd_uri_t u = {};
    u.uri = "/state"; u.method = HTTP_GET; u.handler = h_state; httpd_register_uri_handler(h, &u);
    u.uri = "/thermal"; u.method = HTTP_GET; u.handler = h_thermal; httpd_register_uri_handler(h, &u);
    u.uri = "/profile"; u.method = HTTP_PUT; u.handler = h_profile; httpd_register_uri_handler(h, &u);
    u.uri = "/pump"; u.method = HTTP_POST; u.handler = h_pump; httpd_register_uri_handler(h, &u);
    u.uri = "/light"; u.method = HTTP_POST; u.handler = h_light; httpd_register_uri_handler(h, &u);
}

// ---------------------------------------------------------------- loop

void control_poll() {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_run >= 0 && millis() - s_run_start >= s_run_ms) pump_stop_locked();
    if (s_mode == LM_CAPTURE && millis() - s_capture_ms > CAPTURE_MS) s_mode = LM_SCHEDULE;
    // A light switched by hand holds until the schedule's next edge.
    if ((s_mode == LM_ON || s_mode == LM_OFF) && sched_on() != s_manual_sched) s_mode = LM_SCHEDULE;
    if (clock_ok()) {
        int yday;
        kst_min(time(nullptr), &yday);
        if (yday != s_day) {
            if (s_day >= 0) {   // a new day (KST): the counters and the event list start over
                memset(s_ml_today, 0, sizeof s_ml_today);
                s_nev = 0;
            }
            s_day = yday;
        }
    }
    ring_update();
    xSemaphoreGive(s_mu);
}

void control_safe() {
    s_hold = true;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    all_off_locked();
    ring_update();
    xSemaphoreGive(s_mu);
}

void control_resume() {
    s_hold = false;
}

void control_begin() {
    s_mu = xSemaphoreCreateMutex();
    for (int i = 0; i < P_N; i++) {
        pinMode(PUMP_PIN[i], OUTPUT);
        digitalWrite(PUMP_PIN[i], LOW);
    }
    profile_load();
    configTzTime("KST-9", "pool.ntp.org", "time.google.com");
    s_ring.begin();
    s_ring.clear();
    s_ring.show();
    s_ring_shown = 0;
}
