// Korean names and care groups for the plants a demo is likely to see (identify.cpp).
//
// The LLM's numbers are usable but its Korean names and groups are not (2026-11-21, Bonsai 2 27B: "포티스"
// for Epipremnum, tomato as a succulent, monstera as a succulent on the next run). So the name and the
// group come from this table when the species, genus or family is in it, and the LLM is told to use them.
#pragma once
#include <stddef.h>

struct PlantName {
    const char *ko;         // Korean name ("" when only the family matched)
    const char *group;      // care group key (prompts.h enum), "" when the family is mixed
    const char *family_ko;  // Korean family name, "" when unknown
    int level;              // 3 species, 2 genus, 1 family only, 0 nothing
};

// sci: "Genus species" without author; genus and family as Pl@ntNet gives them.
PlantName plant_lookup(const char *sci, const char *genus, const char *family);
const char *group_ko(const char *group);       // "허브", "관엽식물", ... ("식물" for unknown)
bool group_valid(const char *group);

// Korean particles after a word: "바질" -> "이에요"/"예요", "으로"/"로", "은"/"는", "이"/"가".
bool ko_batchim(const char *word);             // the last syllable has a final consonant
bool ko_rieul(const char *word);               // ...and it is ㄹ (takes 로, not 으로)
