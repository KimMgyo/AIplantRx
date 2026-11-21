// Settings and the plant profile, kept in NVS.
//
// Wi-Fi lives in the "net" namespace with the keys smartfarm used (ssid, pass, en), so a board moved
// from smartfarm to this firmware joins the same network on its first boot.
#pragma once
#include <Arduino.h>

enum Theme : uint8_t { THEME_GLASS = 0, THEME_DARK = 1, THEME_LIGHT = 2 };

struct Settings {
    uint8_t theme = THEME_GLASS;
    bool wifi_on = true;
    String ssid, pass;
    String llm_url = "https://2.tailb0610e.ts.net";
    String llm_key;
    String plantnet_key;
    bool ai_on = true;
    uint8_t ai_idle_min = 10;
    bool ai_hand = true, ai_device = true, ai_values = true;
    bool demo_fast = false;
    bool auto_update = true;      // install new releases by itself (fwupdate.h)
};

// Targets and schedule the devkit runs (LLM_ARCHITECTURE.md). Stored as one blob.
struct Profile {
    uint8_t ver;
    bool identified;            // false: the default tropical-foliage profile
    uint8_t score;              // PlantNet score, %
    char name_ko[48];
    char sci[64];
    char family[32];
    uint32_t since;             // epoch seconds when identified
    float day_lo, day_hi, night_lo, night_hi;
    float rh_lo, rh_hi, soil_lo, soil_hi, ec_lo, ec_hi, ph_lo, ph_hi;
    uint8_t light_on_h, light_on_m, light_off_h, light_off_m;
    uint8_t nutr_days, nutr_ml;
    uint16_t water_ml;
};

extern Settings g_set;
extern Profile g_prof;

void settings_load();
void settings_save();                 // everything except Wi-Fi
void settings_save_wifi();            // ssid, pass, en
void profile_default(Profile &p);
void profile_save();
