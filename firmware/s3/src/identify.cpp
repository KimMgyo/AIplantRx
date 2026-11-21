#include "identify.h"
#include "aitalk.h"
#include "app.h"
#include "llm.h"
#include "lvgl_v8_port.h"
#include "net.h"
#include "nodes.h"
#include "plants.h"
#include "prompts.h"
#include "settings.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <time.h>

// One picture. Three near-identical shots tripled the upload (424 KB took 38 s, 2026-11-21: the S3's TCP
// send window is ~5.7 KB, so a far server gets 10-20 KB/s) and add little for Pl@ntNet.
static const int MAX_SHOTS = 1;
static const uint32_t CAPTURE_TIMEOUT_MS = 10000;
static const uint32_t PLANTNET_TIMEOUT_MS = 30000;
static const uint32_t LLM_TIMEOUT_MS = 25000;   // measured 6-7 s for the profile (2026-11-21)
static const float SCORE_HIGH = 0.5f, SCORE_LOW = 0.15f;
#define PHOTO_W 200
#define PHOTO_H 150

static FindInfo s_info;
static volatile uint32_t s_gen;
static SemaphoreHandle_t s_mu;
static volatile bool s_busy;
static uint16_t *s_photo;               // PHOTO_W x PHOTO_H RGB565
static bool s_photo_ok;
static bool s_fs_ok;

struct Cand {
    char sci[64], genus[32], family[32], ko[48];
    float score;
};
static Cand s_cand[5];
static int s_ncand;

// The choice for find_choose().
static Cand s_pick;
static bool s_pick_mode;

// ---------------------------------------------------------------- state

static void upd(void (*f)(FindInfo &, const void *), const void *arg) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    f(s_info, arg);
    xSemaphoreGive(s_mu);
    s_gen++;
}

static void set_step(FindStep st) {
    upd([](FindInfo &i, const void *a) { i.step = *(const FindStep *)a; }, &st);
}

static void set_note(const char *n) {
    upd([](FindInfo &i, const void *a) { strlcpy(i.note, (const char *)a, sizeof i.note); }, n);
}

static void set_headline(const char *h) {
    upd([](FindInfo &i, const void *a) { strlcpy(i.headline, (const char *)a, sizeof i.headline); }, h);
}

FindInfo find_info() {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    FindInfo i = s_info;
    xSemaphoreGive(s_mu);
    return i;
}
uint32_t find_gen() { return s_gen; }
bool find_busy() { return s_busy; }
const uint16_t *find_photo() { return s_photo_ok ? s_photo : nullptr; }

// ---------------------------------------------------------------- the other boards

static int node_send(const char *method, const char *path, const String &body, String *reply = nullptr) {
    NodeInfo n = node_info(ROLE_NODE);
    if (!n.online) return -1;
    char url[64];
    snprintf(url, sizeof url, "http://%s:%u%s", n.ip, n.port, path);
    WiFiClient c;
    HTTPClient h;
    h.setConnectTimeout(2000);
    h.setTimeout(4000);
    if (!h.begin(c, url)) return -1;
    h.addHeader("Content-Type", "application/json");
    int code = h.sendRequest(method, body);
    if (reply && code > 0) *reply = h.getString();
    h.end();
    return code;
}

static void light(const char *mode) {
    String b = String("{\"mode\":\"") + mode + "\"}";
    int code = node_send("POST", "/light", b);
    Serial.printf("[find] light %s -> %d\n", mode, code);
}

// One /capture into a PSRAM buffer sized from Content-Length.
static uint8_t *capture(int *len) {
    *len = 0;
    NodeInfo n = node_info(ROLE_CAM);
    if (!n.online) return nullptr;
    char url[64];
    // SXGA at quality 12: the short side (1024) is above what Pl@ntNet needs (800), and the file is a
    // third of a UXGA one, which matters at the 10-20 KB/s the upload gets (see MAX_SHOTS).
    snprintf(url, sizeof url, "http://%s:%u/capture?size=sxga&q=12", n.ip, n.port);
    WiFiClient c;
    HTTPClient h;
    h.setConnectTimeout(3000);
    h.setTimeout(CAPTURE_TIMEOUT_MS);
    if (!h.begin(c, url)) return nullptr;
    int code = h.GET();
    int size = h.getSize();
    uint8_t *buf = nullptr;
    if (code == 200 && size > 1000 && size < 2 * 1024 * 1024) {
        buf = (uint8_t *)ps_malloc_or_null(size);
        if (buf) {
            WiFiClient *s = h.getStreamPtr();
            int got = 0;
            uint32_t t0 = millis();
            while (got < size && millis() - t0 < CAPTURE_TIMEOUT_MS) {
                int a = s->available();
                if (a <= 0) {
                    if (!s->connected()) break;
                    vTaskDelay(1);
                    continue;
                }
                int r = s->read(buf + got, min(a, size - got));
                if (r > 0) got += r;
            }
            if (got != size) { free(buf); buf = nullptr; }
            else *len = size;
        }
    }
    h.end();
    Serial.printf("[find] capture -> %d, %d bytes\n", code, *len);
    return buf;
}

// ---------------------------------------------------------------- photo for the sheet

static int photo_draw(JPEGDRAW *d) {
    for (int y = 0; y < d->iHeight; y++) {
        int yy = d->y + y;
        if (yy >= PHOTO_H) break;
        int w = d->iWidth;
        if (d->x + w > PHOTO_W) w = PHOTO_W - d->x;
        if (w > 0) memcpy(s_photo + yy * PHOTO_W + d->x, d->pPixels + y * d->iWidth, w * 2);
    }
    return 1;
}

static void make_photo(const uint8_t *jpg, int len) {
    if (!s_photo) s_photo = (uint16_t *)ps_malloc_or_null(PHOTO_W * PHOTO_H * 2);
    if (!s_photo) return;
    static JPEGDEC *dec;
    if (!dec) dec = new (ps_malloc_or_null(sizeof(JPEGDEC))) JPEGDEC();
    memset(s_photo, 0, PHOTO_W * PHOTO_H * 2);
    if (!dec->openRAM((uint8_t *)jpg, len, photo_draw)) return;
    int w = dec->getWidth();
    int scale = w >= 8 * PHOTO_W ? JPEG_SCALE_EIGHTH : w >= 4 * PHOTO_W ? JPEG_SCALE_QUARTER : w >= 2 * PHOTO_W ? JPEG_SCALE_HALF : 0;
    dec->setPixelType(RGB565_LITTLE_ENDIAN);
    s_photo_ok = dec->decode(0, 0, scale);
    dec->close();
}

static uint16_t *s_thumb;
static bool s_thumb_ok, s_thumb_loaded;

const uint16_t *find_thumb() {
    if (!s_thumb_loaded && s_fs_ok) {
        s_thumb_loaded = true;
        if (!s_thumb) s_thumb = (uint16_t *)ps_malloc_or_null(68 * 68 * 2);
        File f = LittleFS.open("/plant.rgb", "r");
        s_thumb_ok = s_thumb && f && f.read((uint8_t *)s_thumb, 68 * 68 * 2) == 68 * 68 * 2;
        if (f) f.close();
    }
    return s_thumb_ok ? s_thumb : nullptr;
}

// 68x68 centre crop for the main screen, kept across restarts (main_screen.cpp reads it).
static void save_thumb() {
    if (!s_fs_ok || !s_photo_ok) return;
    s_thumb_loaded = false;
    uint16_t *t = (uint16_t *)ps_malloc_or_null(68 * 68 * 2);
    if (!t) return;
    for (int y = 0; y < 68; y++)
        for (int x = 0; x < 68; x++) t[y * 68 + x] = s_photo[(y * PHOTO_H / 68) * PHOTO_W + 25 + x * PHOTO_H / 68];
    File f = LittleFS.open("/plant.rgb", "w");
    if (f) { f.write((uint8_t *)t, 68 * 68 * 2); f.close(); }
    free(t);
}

// ---------------------------------------------------------------- Pl@ntNet

static const char *BOUNDARY = "----aiplantrx7d3f";

// Returns the HTTP status (200, 404 = not a plant, 429 = over quota), -1 for no connection.
// lang=en: Pl@ntNet has no Korean localisation and answers lang=ko with a 404 ("No localization available
// for ko", 2026-11-21). Korean names come from plants.cpp and the LLM.
static int plantnet(uint8_t **img, int *len, int n, String &resp) {
    char url[192];
    snprintf(url, sizeof url, "https://my-api.plantnet.org/v2/identify/all?api-key=%s&lang=en&nb-results=5&include-related-images=false",
             g_set.plantnet_key.c_str());
    char head[160], organ[160], tail[48];
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        int hl = snprintf(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"images\"; filename=\"p%d.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", BOUNDARY, i + 1);
        total += hl + len[i] + 2;
    }
    int ol = snprintf(organ, sizeof organ, "--%s\r\nContent-Disposition: form-data; name=\"organs\"\r\n\r\nauto\r\n", BOUNDARY);
    total += ol * n;
    int tl = snprintf(tail, sizeof tail, "--%s--\r\n", BOUNDARY);
    total += tl;

    esp_http_client_config_t c = {};
    c.url = url;
    c.method = HTTP_METHOD_POST;
    c.timeout_ms = PLANTNET_TIMEOUT_MS;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.buffer_size = 1024;
    c.buffer_size_tx = 512;
    if (!tls_lock(60000)) return -1;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    char ct[80];
    snprintf(ct, sizeof ct, "multipart/form-data; boundary=%s", BOUNDARY);
    esp_http_client_set_header(h, "Content-Type", ct);
    int code = -1;
    uint32_t t0 = millis();
    if (esp_http_client_open(h, total) == ESP_OK) {
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            int hl = snprintf(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"images\"; filename=\"p%d.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", BOUNDARY, i + 1);
            ok = esp_http_client_write(h, head, hl) == hl;
            for (int off = 0; ok && off < len[i];) {
                int k = min(4096, len[i] - off);
                int w = esp_http_client_write(h, (const char *)img[i] + off, k);
                if (w <= 0) ok = false;
                else off += w;
            }
            ok = ok && esp_http_client_write(h, "\r\n", 2) == 2;
        }
        for (int i = 0; i < n && ok; i++) ok = esp_http_client_write(h, organ, ol) == ol;
        ok = ok && esp_http_client_write(h, tail, tl) == tl;
        if (ok) {
            esp_http_client_fetch_headers(h);
            code = esp_http_client_get_status_code(h);
            char buf[512];
            int r;
            while ((r = esp_http_client_read(h, buf, sizeof buf)) > 0) resp.concat(buf, r);
        }
    }
    esp_http_client_cleanup(h);
    tls_unlock();
    // Only "Species not found" means the picture holds no plant; any other 404 is our request's fault.
    if (code == 404 && resp.indexOf("Species not found") < 0) {
        Serial.printf("[find] PlantNet 404: %s\n", resp.c_str());
        code = -404;
    }
    Serial.printf("[find] PlantNet %d in %lu ms, %u bytes sent, %u back\n", code, (unsigned long)(millis() - t0), (unsigned)total,
                  (unsigned)resp.length());
    return code;
}

static bool has_hangul(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (p[0] >= 0xEA && p[0] <= 0xED && p[1] && p[2]) {
            unsigned cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (cp >= 0xAC00 && cp <= 0xD7A3) return true;
        }
    return false;
}

// CJK ideographs: the model sometimes slips a Chinese word into Korean text (seen 2026-11-21).
static bool has_han(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
            unsigned cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) return true;
        }
    return false;
}

static void parse_plantnet(const String &resp) {
    s_ncand = 0;
    JsonDocument filter;
    JsonObject r = filter["results"][0].to<JsonObject>();
    r["score"] = true;
    r["species"]["scientificNameWithoutAuthor"] = true;
    r["species"]["genus"]["scientificNameWithoutAuthor"] = true;
    r["species"]["family"]["scientificNameWithoutAuthor"] = true;
    r["species"]["commonNames"] = true;
    filter["remainingIdentificationRequests"] = true;
    JsonDocument d;
    if (deserializeJson(d, resp, DeserializationOption::Filter(filter))) return;
    for (JsonObject x : d["results"].as<JsonArray>()) {
        if (s_ncand >= 5) break;
        Cand &c = s_cand[s_ncand];
        memset(&c, 0, sizeof c);
        strlcpy(c.sci, x["species"]["scientificNameWithoutAuthor"] | "", sizeof c.sci);
        strlcpy(c.genus, x["species"]["genus"]["scientificNameWithoutAuthor"] | "", sizeof c.genus);
        strlcpy(c.family, x["species"]["family"]["scientificNameWithoutAuthor"] | "", sizeof c.family);
        for (JsonVariant n : x["species"]["commonNames"].as<JsonArray>()) {
            const char *s = n | "";
            if (has_hangul(s) && !has_han(s)) { strlcpy(c.ko, s, sizeof c.ko); break; }
        }
        c.score = x["score"] | 0.0f;
        if (c.sci[0]) s_ncand++;
    }
    Serial.printf("[find] PlantNet: %d results, top %s %.2f, %d left today\n", s_ncand, s_ncand ? s_cand[0].sci : "-",
                  s_ncand ? s_cand[0].score : 0.0f, (int)(d["remainingIdentificationRequests"] | -1));
}

// ---------------------------------------------------------------- profile

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void range(JsonVariantConst r, float &lo, float &hi, float dlo, float dhi, float mn, float mx) {
    float a = r["min"] | NAN, b = r["max"] | NAN;
    if (isnan(a) || isnan(b)) { a = dlo; b = dhi; }
    if (a > b) { float t = a; a = b; b = t; }
    lo = clampf(a, mn, mx);
    hi = clampf(b, mn, mx);
    if (hi - lo < 0.1f) hi = clampf(lo + (mx - mn) * 0.1f, mn, mx);
}

static bool hhmm(const char *s, uint8_t &h, uint8_t &m) {
    int a, b;
    if (!s || sscanf(s, "%d:%d", &a, &b) != 2 || a < 0 || a > 23 || b < 0 || b > 59) return false;
    h = a;
    m = b;
    return true;
}

// LLM answer -> profile, inside the hardware ranges of LLM_ARCHITECTURE.md 4. Missing values come from
// the default profile.
static void apply_llm(Profile &p, JsonDocument &d) {
    Profile def;
    profile_default(def);
    range(d["temp_day_c"], p.day_lo, p.day_hi, def.day_lo, def.day_hi, 10, 32);
    range(d["temp_night_c"], p.night_lo, p.night_hi, def.night_lo, def.night_hi, 10, 32);
    range(d["humidity_pct"], p.rh_lo, p.rh_hi, def.rh_lo, def.rh_hi, 20, 95);
    range(d["soil_moisture_pct"], p.soil_lo, p.soil_hi, def.soil_lo, def.soil_hi, 10, 80);
    range(d["soil_ec_ms_cm"], p.ec_lo, p.ec_hi, def.ec_lo, def.ec_hi, 0.3f, 3.0f);
    range(d["soil_ph"], p.ph_lo, p.ph_hi, def.ph_lo, def.ph_hi, 5.0f, 7.5f);
    if (!hhmm(d["light_on"] | "", p.light_on_h, p.light_on_m)) { p.light_on_h = def.light_on_h; p.light_on_m = def.light_on_m; }
    if (!hhmm(d["light_off"] | "", p.light_off_h, p.light_off_m)) { p.light_off_h = def.light_off_h; p.light_off_m = def.light_off_m; }
    int on = p.light_on_h * 60 + p.light_on_m, off = p.light_off_h * 60 + p.light_off_m;
    int dur = (off - on + 1440) % 1440;
    if (dur < 8 * 60 || dur > 16 * 60) {
        dur = dur < 8 * 60 ? 8 * 60 : 16 * 60;
        off = (on + dur) % 1440;
        p.light_off_h = off / 60;
        p.light_off_m = off % 60;
    }
    p.water_days = (uint8_t)clampf(d["water_interval_days"] | (int)def.water_days, 1, 21);
    p.water_ml = (uint16_t)clampf(d["water_ml"] | (int)def.water_ml, 20, 300);
    p.nutr_days = (uint8_t)clampf(d["nutrient_interval_days"] | (int)def.nutr_days, 3, 30);
    p.nutr_ml = (uint8_t)clampf(d["nutrient_ml"] | (int)def.nutr_ml, 0, 20);
    const char *tip = d["tip_ko"] | "";
    strlcpy(p.tip, has_han(tip) ? "" : tip, sizeof p.tip);
}

static String cache_path(const char *key) {
    String s = "/p/";
    for (const char *c = key; *c && s.length() < 60; c++) s += (isalnum((unsigned char)*c) ? *c : '_');
    return s + ".json";
}

static bool cache_read(const char *key, JsonDocument &d) {
    if (!s_fs_ok) return false;
    File f = LittleFS.open(cache_path(key), "r");
    if (!f) return false;
    bool ok = !deserializeJson(d, f);
    f.close();
    return ok;
}

static void cache_write(const char *key, JsonDocument &d) {
    if (!s_fs_ok) return;
    File f = LittleFS.open(cache_path(key), "w");
    if (!f) return;
    serializeJson(d, f);
    f.close();
}

// The PUT /profile body (INTERFACES.md 2).
static String profile_json(const Profile &p) {
    JsonDocument d;
    d["grade"] = p.llm ? "llm" : "default";
    d["t"] = (uint32_t)time(nullptr);
    d["name_ko"] = p.name_ko;
    d["sci"] = p.sci;
    d["family"] = p.family;
    d["score"] = p.score;
    auto pair = [&](const char *k, float a, float b) {
        JsonArray r = d[k].to<JsonArray>();
        r.add(serialized(String(a, 1)));
        r.add(serialized(String(b, 1)));
    };
    pair("temp_day", p.day_lo, p.day_hi);
    pair("temp_night", p.night_lo, p.night_hi);
    pair("rh", p.rh_lo, p.rh_hi);
    pair("soil", p.soil_lo, p.soil_hi);
    pair("ec", p.ec_lo, p.ec_hi);
    pair("ph", p.ph_lo, p.ph_hi);
    char t[8];
    snprintf(t, sizeof t, "%02u:%02u", p.light_on_h, p.light_on_m);
    d["light_on"] = t;
    snprintf(t, sizeof t, "%02u:%02u", p.light_off_h, p.light_off_m);
    d["light_off"] = t;
    d["water_interval_days"] = p.water_days;
    d["water_ml"] = p.water_ml;
    d["nutr_interval_days"] = p.nutr_days;
    d["nutr_ml"] = p.nutr_ml;
    String s;
    serializeJson(d, s);
    return s;
}

// ---------------------------------------------------------------- wording

static void say(char *o, size_t n, const char *name, FindConf conf, const char *group) {
    if (conf == CONF_HIGH) snprintf(o, n, "%s%s", name, ko_batchim(name) ? "이에요" : "예요");
    else if (conf == CONF_MID) snprintf(o, n, "%s%s 보여요", name, ko_batchim(name) && !ko_rieul(name) ? "으로" : "로");
    else if (conf == CONF_LOW) snprintf(o, n, "%s 종류로 보여요", name);
    else snprintf(o, n, "%s%s 보여요", group, ko_batchim(group) && !ko_rieul(group) ? "으로" : "로");
}

// When the LLM's introduction is missing or unusable.
static void fallback_intro(Profile &p) {
    const char *g = group_ko(p.group);
    snprintf(p.intro, sizeof p.intro, "%s%s %s%s. %s", p.name_ko, ko_batchim(p.name_ko) ? "은" : "는", g,
             ko_batchim(g) ? "이에요" : "예요", p.tip);
}

// ---------------------------------------------------------------- the run

struct Shots {
    uint8_t *img[MAX_SHOTS];
    int len[MAX_SHOTS];
    int n;
    void clear() {
        for (int i = 0; i < n; i++) free(img[i]);
        n = 0;
    }
};

static bool take_photos(Shots &s) {
    light("capture");
    for (int c = 3; c > 0; c--) {
        upd([](FindInfo &i, const void *a) { i.step = FIND_COUNT; i.count = *(const int *)a; }, &c);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    set_step(FIND_PHOTO);
    for (int k = 0; k < MAX_SHOTS + 1 && s.n < MAX_SHOTS; k++) {   // one retry for a failed shot
        int len;
        uint8_t *b = capture(&len);
        if (!b) continue;
        s.img[s.n] = b;
        s.len[s.n] = len;
        if (s.n == 0) make_photo(b, len);
        s.n++;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    light("schedule");
    return s.n > 0;
}

// Name, group and the LLM profile for one candidate; fills p and the headline. conf decides the wording
// and what the LLM is asked (species, or only genus and family when Pl@ntNet is unsure).
static void build_profile(Profile &p, const Cand &c, FindConf conf) {
    profile_default(p);
    PlantName pn = plant_lookup(c.sci, c.genus, c.family);
    strlcpy(p.sci, conf == CONF_LOW ? c.genus : c.sci, sizeof p.sci);
    strlcpy(p.genus, c.genus, sizeof p.genus);
    strlcpy(p.family, c.family, sizeof p.family);
    strlcpy(p.family_ko, pn.family_ko, sizeof p.family_ko);
    if (pn.group[0]) strlcpy(p.group, pn.group, sizeof p.group);
    const char *name = (conf == CONF_LOW || pn.level < 3) ? "" : pn.ko;
    if (conf == CONF_LOW && pn.level >= 2) name = pn.ko;           // genus name ("에케베리아")
    if (!*name && conf != CONF_LOW && c.ko[0]) name = c.ko;        // Pl@ntNet's Korean common name
    if (!*name && conf != CONF_LOW && pn.level == 2) name = pn.ko; // genus, when the species is not listed
    strlcpy(p.name_ko, name, sizeof p.name_ko);
    p.score = (uint8_t)lroundf(c.score * 100);
    p.identified = true;
    p.since = (uint32_t)time(nullptr);

    // LLM, or its saved answer for the same plant.
    char key[72];
    snprintf(key, sizeof key, conf == CONF_LOW ? "genus_%s" : "%s", conf == CONF_LOW ? c.genus : c.sci);
    JsonDocument d;
    bool cached = cache_read(key, d);
    bool ok = cached;
    char err[64] = "";
    if (!cached) {
        set_step(FIND_PROFILE);
        String user;
        if (conf == CONF_LOW)
            user = String("Pl@ntNet is not sure of the species. Genus: ") + c.genus + "\nFamily: " + c.family +
                   "\nGive typical indoor conditions for this genus.";
        else
            user = String("Scientific name: ") + c.sci + "\nGenus: " + c.genus + "\nFamily: " + c.family +
                   "\nCommon names: " + (c.ko[0] ? c.ko : "-") + "\nPl@ntNet score: " + String(c.score, 2);
        if (p.name_ko[0] && pn.group[0]) user += String("\nKorean name: ") + p.name_ko + "\nGroup: " + pn.group;
        else if (pn.group[0]) user += String("\nGroup: ") + pn.group;
        String content;
        ok = llm_chat(PROMPT_PROFILE_SYSTEM, user.c_str(), PROMPT_PROFILE_SCHEMA, "plant_profile", 700, 0.3f, content, err,
                      sizeof err, LLM_TIMEOUT_MS) &&
             !deserializeJson(d, content);
        if (ok) cache_write(key, d);
    }
    if (ok) {
        apply_llm(p, d);
        p.llm = true;
        const char *g = d["group"] | "";
        if (!pn.group[0] && group_valid(g)) strlcpy(p.group, g, sizeof p.group);
        const char *k = d["korean_name"] | "";
        if (!p.name_ko[0] && has_hangul(k) && !has_han(k)) strlcpy(p.name_ko, k, sizeof p.name_ko);
        const char *intro = d["intro_ko"] | "";
        if (*intro) strlcpy(p.intro, intro, sizeof p.intro);   // saved with the profile on an earlier run
    } else {
        Serial.printf("[find] no LLM profile: %s\n", err);
        set_note("AI 서버가 응답하지 않아 기본 환경으로 운영해요");
    }
    if (!p.name_ko[0]) strlcpy(p.name_ko, conf == CONF_LOW ? group_ko(p.group) : c.sci, sizeof p.name_ko);
    const bool src[2] = {ok, cached};
    upd([](FindInfo &i, const void *a) { i.from_llm = ((const bool *)a)[0]; i.cached = ((const bool *)a)[1]; }, src);

    char h[96];
    say(h, sizeof h, p.name_ko, conf, group_ko(p.group));
    set_headline(h);
}

static void apply(Profile &p) {
    set_step(FIND_APPLY);
    if (lvgl_port_lock(1000)) {
        g_prof = p;
        profile_save();
        lvgl_port_unlock();
    }
    int code = node_send("PUT", "/profile", profile_json(p));
    Serial.printf("[find] PUT /profile -> %d\n", code);
    if (code != 200 && node_info(ROLE_NODE).online) set_note("센서 노드에 다시 보내는 중이에요");
}

static void intro(Profile &p, const char *key) {
    if (p.intro[0]) {   // saved earlier
        upd([](FindInfo &i, const void *) { i.intro_state = 2; }, nullptr);
        return;
    }
    upd([](FindInfo &i, const void *) { i.intro_state = 1; }, nullptr);
    String user = String("Name: ") + p.name_ko + "\nScientific name: " + p.sci + "\nGroup: " + p.group +
                  "\nProfile: day " + String(p.day_lo, 0) + "-" + String(p.day_hi, 0) + " C, soil moisture " +
                  String(p.soil_lo, 0) + "-" + String(p.soil_hi, 0) + " %, water every " + p.water_days + " days" +
                  "\nCare tip: " + p.tip;
    String content;
    char err[64];
    JsonDocument d;
    bool ok = p.llm && llm_chat(PROMPT_INTRO_SYSTEM, user.c_str(), PROMPT_INTRO_SCHEMA, "plant_intro", 300, 0.3f, content, err,
                                sizeof err, LLM_TIMEOUT_MS) &&
              !deserializeJson(d, content);
    const char *t = ok ? (d["intro_ko"] | "") : "";
    if (ok && strlen(t) > 20 && has_hangul(t) && !has_han(t)) {
        strlcpy(p.intro, t, sizeof p.intro);
        // Saved next to the profile so the same plant shows it at once next time.
        JsonDocument c;
        if (key && cache_read(key, c)) {
            c["intro_ko"] = p.intro;
            cache_write(key, c);
        }
    } else fallback_intro(p);
    if (lvgl_port_lock(1000)) {
        strlcpy(g_prof.intro, p.intro, sizeof g_prof.intro);
        profile_save();
        lvgl_port_unlock();
    }
    upd([](FindInfo &i, const void *) { i.intro_state = 2; }, nullptr);
}

static void run_once() {
    static Profile p;
    upd([](FindInfo &i, const void *) {
        memset(&i, 0, sizeof i);
        i.started_ms = millis();
    }, nullptr);
    cam_set_wanted(false);
    FindConf conf = CONF_NONE;
    Cand best{};
    char key[72] = "";

    if (s_pick_mode) {
        best = s_pick;
        conf = best.score >= SCORE_HIGH || best.score <= 0 ? CONF_HIGH : best.score >= SCORE_LOW ? CONF_MID : CONF_LOW;
        upd([](FindInfo &i, const void *a) { i.conf = *(const FindConf *)a; i.score = 0; i.step = FIND_PLANTNET; }, &conf);
    } else {
        s_photo_ok = false;
        int code = -1;
        for (int round = 0; round < 2; round++) {
            Shots s{};
            if (!take_photos(s)) {
                set_note("카메라가 사진을 보내지 않았어요. 잠시 뒤 다시 찾아 주세요");
                set_step(FIND_STOPPED);
                cam_set_wanted(true);
                return;
            }
            set_step(FIND_PLANTNET);
            String resp;
            nodes_pause(true);
            code = plantnet(s.img, s.len, s.n, resp);
            nodes_pause(false);
            s.clear();
            if (code == 200) parse_plantnet(resp);
            if (code != 404 || round == 1) break;
            upd([](FindInfo &i, const void *) { i.retake = true; }, nullptr);
            set_note("식물이 가운데 오도록 다시 찍을게요");
        }
        if (code == 200 && s_ncand) {
            best = s_cand[0];
            conf = best.score >= SCORE_HIGH ? CONF_HIGH : best.score >= SCORE_LOW ? CONF_MID : CONF_LOW;
            set_note("");
        } else {
            s_ncand = 0;
            set_note(code == 404 ? "식물을 알아보기 어려워 기본 환경으로 운영해요"
                     : code == 429 ? "오늘 식물 찾기를 많이 해서 기본 환경으로 운영해요"
                                   : "PlantNet에 닿지 않아 기본 환경으로 운영해요");
        }
        const uint8_t cs[2] = {(uint8_t)conf, (uint8_t)lroundf(best.score * 100)};
        upd([](FindInfo &i, const void *a) { i.conf = (FindConf)((const uint8_t *)a)[0]; i.score = ((const uint8_t *)a)[1]; }, cs);
    }

    if (conf == CONF_NONE) {
        profile_default(p);
        p.identified = false;
        char h[96];
        say(h, sizeof h, "", CONF_NONE, group_ko(p.group));
        set_headline(h);
        fallback_intro(p);
    } else {
        build_profile(p, best, conf);
        snprintf(key, sizeof key, conf == CONF_LOW ? "genus_%s" : "%s", conf == CONF_LOW ? best.genus : best.sci);
    }
    apply(p);
    if (s_photo_ok && !s_pick_mode) save_thumb();
    upd([](FindInfo &i, const void *) { i.step = FIND_DONE; i.done_ms = millis(); }, nullptr);
    if (p.identified) ai_new_plant();
    Serial.printf("[find] done in %lu ms: %s (%s, %u%%)\n", (unsigned long)(millis() - s_info.started_ms), p.name_ko, p.sci, p.score);
    if (conf != CONF_NONE) intro(p, key);
    else upd([](FindInfo &i, const void *) { i.intro_state = 2; }, nullptr);
}

// One task for the whole run, made at boot: its 16 KB stack (TLS for Pl@ntNet and the LLM runs on it)
// is taken while internal RAM is still in one piece. Made per run, it split the heap until mbedTLS could
// not get its two 16 KB buffers (largest free block 31 KB, 2026-11-21). Internal stack: it writes flash.
static TaskHandle_t s_task;

static void task(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_once();
        s_pick_mode = false;
        s_busy = false;
    }
}

// ---------------------------------------------------------------- API

void find_init() {
    s_mu = xSemaphoreCreateMutex();
    s_fs_ok = LittleFS.begin(true, "/lfs", 5, "spiffs");
    if (s_fs_ok && !LittleFS.exists("/p")) LittleFS.mkdir("/p");
    Serial.printf("[find] LittleFS %s\n", s_fs_ok ? "ok" : "failed");
    xTaskCreatePinnedToCore(task, "find", 16384, nullptr, 2, &s_task, 0);
}

bool find_start(char *why, size_t n) {
    if (s_busy) { strlcpy(why, "식물을 찾는 중이에요", n); return false; }
    if (!node_info(ROLE_CAM).online) { strlcpy(why, "카메라를 찾는 중이에요. 카메라가 켜지면 사진을 찍어 PlantNet과 AI가 식물을 알아봅니다.", n); return false; }
    if (!g_set.plantnet_key.length()) { strlcpy(why, "설정 > PlantNet에 API 키를 넣으면 식물을 찾을 수 있어요.", n); return false; }
    if (net_state() != NET_UP) { strlcpy(why, "Wi-Fi에 연결되면 식물을 찾을 수 있어요.", n); return false; }
    s_busy = true;
    s_pick_mode = false;
    xTaskNotifyGive(s_task);
    return true;
}

bool find_choose(const char *sci, const char *genus, const char *family, uint8_t score) {
    if (s_busy) return false;
    memset(&s_pick, 0, sizeof s_pick);
    strlcpy(s_pick.sci, sci, sizeof s_pick.sci);
    strlcpy(s_pick.genus, genus, sizeof s_pick.genus);
    strlcpy(s_pick.family, family, sizeof s_pick.family);
    s_pick.score = score / 100.0f;
    s_busy = true;
    s_pick_mode = true;
    xTaskNotifyGive(s_task);
    return true;
}

int find_candidates(FindChoice *out, int max) {
    int k = 0;
    for (int i = 1; i < s_ncand && k < max; i++, k++) {
        PlantName pn = plant_lookup(s_cand[i].sci, s_cand[i].genus, s_cand[i].family);
        strlcpy(out[k].sci, s_cand[i].sci, sizeof out[k].sci);
        strlcpy(out[k].genus, s_cand[i].genus, sizeof out[k].genus);
        strlcpy(out[k].family, s_cand[i].family, sizeof out[k].family);
        strlcpy(out[k].ko, pn.level == 3 ? pn.ko : s_cand[i].ko, sizeof out[k].ko);
        out[k].score = (uint8_t)lroundf(s_cand[i].score * 100);
    }
    return k;
}

// Saved profiles, newest first (the file's write time).
int find_recent(FindChoice *out, int max) {
    if (!s_fs_ok) return 0;
    File dir = LittleFS.open("/p");
    if (!dir) return 0;
    struct E { time_t t; char name[64]; } e[16];
    int n = 0;
    for (File f = dir.openNextFile(); f && n < 16; f = dir.openNextFile()) {
        const char *nm = f.name();
        if (strncmp(nm, "genus_", 6) && strstr(nm, ".json")) {
            e[n].t = f.getLastWrite();
            strlcpy(e[n].name, nm, sizeof e[n].name);
            n++;
        }
        f.close();
    }
    dir.close();
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && e[j].t > e[j - 1].t; j--) { E t = e[j]; e[j] = e[j - 1]; e[j - 1] = t; }
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        // The file name is the scientific name with spaces as '_'.
        char sci[64];
        strlcpy(sci, e[i].name, sizeof sci);
        char *dot = strstr(sci, ".json");
        if (dot) *dot = 0;
        for (char *c = sci; *c; c++)
            if (*c == '_') *c = ' ';
        char genus[32];
        strlcpy(genus, sci, sizeof genus);
        char *sp = strchr(genus, ' ');
        if (sp) *sp = 0;
        PlantName pn = plant_lookup(sci, genus, "");
        JsonDocument d;
        String path = String("/p/") + e[i].name;
        File f = LittleFS.open(path, "r");
        const char *ko = pn.level == 3 ? pn.ko : "";
        String kname;
        if (!*ko && f && !deserializeJson(d, f)) { kname = d["korean_name"] | ""; ko = kname.c_str(); }
        if (f) f.close();
        if (!strcmp(sci, g_prof.sci)) continue;   // the current plant is not a choice
        strlcpy(out[k].sci, sci, sizeof out[k].sci);
        strlcpy(out[k].genus, genus, sizeof out[k].genus);
        out[k].family[0] = 0;
        strlcpy(out[k].ko, *ko ? ko : sci, sizeof out[k].ko);
        out[k].score = 0;
        k++;
    }
    return k;
}
