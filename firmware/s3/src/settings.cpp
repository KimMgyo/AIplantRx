#include "settings.h"
#include <Preferences.h>

Settings g_set;
Profile g_prof;

static const uint8_t PROFILE_VER = 1;

void profile_default(Profile &p) {
    memset(&p, 0, sizeof(p));
    p.ver = PROFILE_VER;
    // LLM_ARCHITECTURE.md: tropical foliage.
    strlcpy(p.name_ko, "관엽식물", sizeof(p.name_ko));
    p.day_lo = 20; p.day_hi = 28; p.night_lo = 18; p.night_hi = 22;
    p.rh_lo = 60; p.rh_hi = 80; p.soil_lo = 40; p.soil_hi = 60;
    p.ec_lo = 0.8f; p.ec_hi = 1.4f; p.ph_lo = 5.5f; p.ph_hi = 6.5f;
    p.light_on_h = 6; p.light_off_h = 18;
    p.nutr_days = 14; p.nutr_ml = 10; p.water_ml = 100;
}

void settings_load() {
    Preferences p;
    p.begin("net", true);
    g_set.wifi_on = p.getBool("en", true);
    g_set.ssid = p.getString("ssid", "");
    g_set.pass = p.getString("pass", "");
    p.end();

    p.begin("app", true);
    g_set.theme = p.getUChar("theme", THEME_GLASS);
    if (g_set.theme > THEME_LIGHT) g_set.theme = THEME_GLASS;
    g_set.llm_url = p.getString("llm_url", g_set.llm_url);
    g_set.llm_key = p.getString("llm_key", "");
    g_set.plantnet_key = p.getString("pn_key", "");
    g_set.ai_on = p.getBool("ai_on", true);
    g_set.ai_idle_min = p.getUChar("ai_idle", 10);
    g_set.ai_hand = p.getBool("ai_hand", true);
    g_set.ai_device = p.getBool("ai_dev", true);
    g_set.ai_values = p.getBool("ai_val", true);
    g_set.demo_fast = p.getBool("demo", false);
    g_set.auto_update = p.getBool("autoupd", true);
    p.end();

    p.begin("plant", true);
    Profile tmp;
    if (p.getBytes("prof", &tmp, sizeof(tmp)) == sizeof(tmp) && tmp.ver == PROFILE_VER) g_prof = tmp;
    else profile_default(g_prof);
    p.end();
}

void settings_save() {
    Preferences p;
    p.begin("app", false);
    p.putUChar("theme", g_set.theme);
    p.putString("llm_url", g_set.llm_url);
    p.putString("llm_key", g_set.llm_key);
    p.putString("pn_key", g_set.plantnet_key);
    p.putBool("ai_on", g_set.ai_on);
    p.putUChar("ai_idle", g_set.ai_idle_min);
    p.putBool("ai_hand", g_set.ai_hand);
    p.putBool("ai_dev", g_set.ai_device);
    p.putBool("ai_val", g_set.ai_values);
    p.putBool("demo", g_set.demo_fast);
    p.putBool("autoupd", g_set.auto_update);
    p.end();
}

void settings_save_wifi() {
    Preferences p;
    p.begin("net", false);
    p.putBool("en", g_set.wifi_on);
    p.putString("ssid", g_set.ssid);
    p.putString("pass", g_set.pass);
    p.end();
}

void profile_save() {
    Preferences p;
    p.begin("plant", false);
    p.putBytes("prof", &g_prof, sizeof(g_prof));
    p.end();
}
