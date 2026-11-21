// LLM prompts and JSON schemas (UI.md 6.1: one file; LLM_ARCHITECTURE.md 3).
//
// tools/llm_profile_test.py reads the raw strings out of this file and sends the same requests from the
// PC, so prompts can be tuned against the server before they go into a build. Keep each one as a single
// R"P( ... )P" literal.
#pragma once

// 1. Plant profile and schedule, after Pl@ntNet. The user message lists what Pl@ntNet said.
static const char PROMPT_PROFILE_SYSTEM[] = R"P(You set up a small indoor plant box for a plant identified by Pl@ntNet. Return typical optimal indoor growing conditions as JSON matching the schema.
Box: one pot of about 1 litre, a soil probe, an LED light, a water pump, a liquid fertiliser pump and a pH pump. There is no heater or humidifier: temperature and humidity are only monitored.
Units: temperature in Celsius, humidity and soil moisture in percent (volumetric water content as a soil probe reads it), soil EC in mS/cm, pH unitless. Times are 24-hour HH:MM.
Keep ranges realistic and no wider than needed. water_ml is per watering for a 1 L pot. nutrient_ml is undiluted liquid fertiliser per dose (0 if the plant should not be fed).
If the message gives a Korean name and a group, return exactly those and base the ranges on them. Otherwise korean_name is the usual Korean common name of the species; if you do not know one for certain, give the Korean name of the genus or of a well-known group it belongs to (for example 에케베리아, 다육식물), never an invented name.
group: fruiting_veg for plants grown for fruit (tomato, pepper, strawberry), leafy_veg for salad and leaf vegetables, herb for culinary herbs, succulent only for succulents and cacti.
tip_ko: one complete Korean sentence of care advice, under 50 characters.)P";

static const char PROMPT_PROFILE_SCHEMA[] = R"P({"type":"object","additionalProperties":false,"required":["korean_name","group","temp_day_c","temp_night_c","humidity_pct","soil_moisture_pct","soil_ec_ms_cm","soil_ph","light_on","light_off","water_interval_days","water_ml","nutrient_interval_days","nutrient_ml","tip_ko"],"properties":{"korean_name":{"type":"string","maxLength":20},"group":{"type":"string","enum":["succulent","tropical_foliage","fern_humid","herb","leafy_veg","fruiting_veg","temperate_foliage","flowering"]},"temp_day_c":{"$ref":"#/$defs/r"},"temp_night_c":{"$ref":"#/$defs/r"},"humidity_pct":{"$ref":"#/$defs/r"},"soil_moisture_pct":{"$ref":"#/$defs/r"},"soil_ec_ms_cm":{"$ref":"#/$defs/r"},"soil_ph":{"$ref":"#/$defs/r"},"light_on":{"type":"string","pattern":"^([01][0-9]|2[0-3]):[0-5][0-9]$"},"light_off":{"type":"string","pattern":"^([01][0-9]|2[0-3]):[0-5][0-9]$"},"water_interval_days":{"type":"integer","minimum":1,"maximum":30},"water_ml":{"type":"integer","minimum":10,"maximum":500},"nutrient_interval_days":{"type":"integer","minimum":1,"maximum":60},"nutrient_ml":{"type":"integer","minimum":0,"maximum":50},"tip_ko":{"type":"string","maxLength":80}},"$defs":{"r":{"type":"object","additionalProperties":false,"required":["min","max"],"properties":{"min":{"type":"number"},"max":{"type":"number"}}}}})P";

// 2. Short introduction for the result sheet (UI.md 3), asked right after the profile is applied.
static const char PROMPT_INTRO_SYSTEM[] = R"P(Write a short Korean introduction of the given plant for a visitor looking at a small indoor plant box: 2 plain sentences, at most 100 characters in total.
Use only facts you are certain of: what kind of plant it is (use the given group), how it looks, or how it is commonly used or grown. Do not name its plant family, its origin or any number. If unsure, describe its care needs from the given profile instead. Correct, natural Korean; no lists, emoji or greeting.)P";

static const char PROMPT_INTRO_SCHEMA[] = R"P({"type":"object","additionalProperties":false,"required":["intro_ko"],"properties":{"intro_ko":{"type":"string","maxLength":120}}})P";
