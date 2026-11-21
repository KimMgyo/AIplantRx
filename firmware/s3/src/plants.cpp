#include "plants.h"
#include <string.h>
#include <strings.h>

struct Row { const char *key, *ko, *group; };

// Species (accepted names as Pl@ntNet returns them, plus common synonyms).
static const Row SPECIES[] = {
    // herbs
    {"Ocimum basilicum", "바질", "herb"},
    {"Mentha spicata", "스피어민트", "herb"},
    {"Mentha x piperita", "페퍼민트", "herb"},
    {"Mentha piperita", "페퍼민트", "herb"},
    {"Salvia rosmarinus", "로즈메리", "herb"},
    {"Rosmarinus officinalis", "로즈메리", "herb"},
    {"Thymus vulgaris", "타임", "herb"},
    {"Petroselinum crispum", "파슬리", "herb"},
    {"Coriandrum sativum", "고수", "herb"},
    {"Lavandula angustifolia", "라벤더", "herb"},
    {"Salvia officinalis", "세이지", "herb"},
    {"Origanum vulgare", "오레가노", "herb"},
    {"Melissa officinalis", "레몬밤", "herb"},
    {"Allium schoenoprasum", "차이브", "herb"},
    {"Anethum graveolens", "딜", "herb"},
    {"Perilla frutescens", "들깨", "herb"},
    // leaf vegetables
    {"Lactuca sativa", "상추", "leafy_veg"},
    {"Spinacia oleracea", "시금치", "leafy_veg"},
    {"Brassica oleracea", "양배추", "leafy_veg"},
    {"Brassica rapa", "배추", "leafy_veg"},
    {"Brassica juncea", "갓", "leafy_veg"},
    {"Eruca vesicaria", "루콜라", "leafy_veg"},
    {"Eruca sativa", "루콜라", "leafy_veg"},
    {"Beta vulgaris", "근대", "leafy_veg"},
    {"Apium graveolens", "셀러리", "leafy_veg"},
    {"Allium fistulosum", "대파", "leafy_veg"},
    {"Allium tuberosum", "부추", "leafy_veg"},
    // fruiting vegetables
    {"Solanum lycopersicum", "토마토", "fruiting_veg"},
    {"Capsicum annuum", "고추", "fruiting_veg"},
    {"Fragaria x ananassa", "딸기", "fruiting_veg"},
    {"Fragaria ananassa", "딸기", "fruiting_veg"},
    {"Solanum melongena", "가지", "fruiting_veg"},
    {"Cucumis sativus", "오이", "fruiting_veg"},
    {"Cucurbita pepo", "호박", "fruiting_veg"},
    {"Phaseolus vulgaris", "강낭콩", "fruiting_veg"},
    {"Pisum sativum", "완두", "fruiting_veg"},
    // tropical foliage
    {"Monstera deliciosa", "몬스테라", "tropical_foliage"},
    {"Monstera adansonii", "몬스테라 아단소니", "tropical_foliage"},
    {"Epipremnum aureum", "스킨답서스", "tropical_foliage"},
    {"Philodendron hederaceum", "필로덴드론", "tropical_foliage"},
    {"Ficus elastica", "고무나무", "tropical_foliage"},
    {"Ficus lyrata", "떡갈잎고무나무", "tropical_foliage"},
    {"Ficus benjamina", "벤자민고무나무", "tropical_foliage"},
    {"Zamioculcas zamiifolia", "금전수", "tropical_foliage"},
    {"Spathiphyllum wallisii", "스파티필룸", "tropical_foliage"},
    {"Aglaonema commutatum", "아글라오네마", "tropical_foliage"},
    {"Dieffenbachia seguine", "디펜바키아", "tropical_foliage"},
    {"Syngonium podophyllum", "싱고니움", "tropical_foliage"},
    {"Schefflera arboricola", "홍콩야자", "tropical_foliage"},
    {"Heptapleurum arboricola", "홍콩야자", "tropical_foliage"},
    {"Dypsis lutescens", "아레카야자", "tropical_foliage"},
    {"Chamaedorea elegans", "테이블야자", "tropical_foliage"},
    {"Anthurium andraeanum", "안스리움", "tropical_foliage"},
    {"Peperomia obtusifolia", "페페로미아", "tropical_foliage"},
    {"Pilea peperomioides", "필레아 페페로미오이데스", "tropical_foliage"},
    {"Hoya carnosa", "호야", "tropical_foliage"},
    {"Tradescantia zebrina", "제브리나", "tropical_foliage"},
    {"Chlorophytum comosum", "접란", "tropical_foliage"},
    {"Dracaena fragrans", "행운목", "tropical_foliage"},
    {"Pachira aquatica", "파키라", "tropical_foliage"},
    {"Maranta leuconeura", "마란타", "fern_humid"},
    {"Hedera helix", "아이비", "temperate_foliage"},
    // ferns
    {"Nephrolepis exaltata", "보스턴고사리", "fern_humid"},
    {"Asplenium nidus", "아비스", "fern_humid"},
    // succulents
    {"Dracaena trifasciata", "산세베리아", "succulent"},
    {"Sansevieria trifasciata", "산세베리아", "succulent"},
    {"Echeveria elegans", "에케베리아 엘레강스", "succulent"},
    {"Crassula ovata", "염좌", "succulent"},
    {"Aloe vera", "알로에 베라", "succulent"},
    {"Kalanchoe blossfeldiana", "칼랑코에", "succulent"},
    {"Senecio rowleyanus", "녹영", "succulent"},
    {"Curio rowleyanus", "녹영", "succulent"},
    {"Schlumbergera truncata", "게발선인장", "succulent"},
    // flowering
    {"Saintpaulia ionantha", "아프리칸 바이올렛", "flowering"},
    {"Streptocarpus ionanthus", "아프리칸 바이올렛", "flowering"},
    {"Chrysanthemum morifolium", "국화", "flowering"},
    {"Hydrangea macrophylla", "수국", "flowering"},
    {"Cyclamen persicum", "시클라멘", "flowering"},
    {"Hibiscus rosa-sinensis", "하와이무궁화", "flowering"},
    {"Gardenia jasminoides", "치자나무", "flowering"},
};

static const Row GENERA[] = {
    {"Ocimum", "바질", "herb"},           {"Mentha", "민트", "herb"},
    {"Thymus", "타임", "herb"},           {"Lavandula", "라벤더", "herb"},
    {"Rosmarinus", "로즈메리", "herb"},   {"Origanum", "오레가노", "herb"},
    {"Lactuca", "상추", "leafy_veg"},     {"Brassica", "배추속 채소", "leafy_veg"},
    {"Spinacia", "시금치", "leafy_veg"},  {"Capsicum", "고추", "fruiting_veg"},
    {"Fragaria", "딸기", "fruiting_veg"}, {"Cucumis", "오이", "fruiting_veg"},
    {"Monstera", "몬스테라", "tropical_foliage"},     {"Epipremnum", "스킨답서스", "tropical_foliage"},
    {"Philodendron", "필로덴드론", "tropical_foliage"}, {"Ficus", "고무나무", "tropical_foliage"},
    {"Dracaena", "드라세나", "tropical_foliage"},     {"Sansevieria", "산세베리아", "succulent"},
    {"Zamioculcas", "금전수", "tropical_foliage"},    {"Spathiphyllum", "스파티필룸", "tropical_foliage"},
    {"Aglaonema", "아글라오네마", "tropical_foliage"}, {"Calathea", "칼라테아", "fern_humid"},
    {"Goeppertia", "칼라테아", "fern_humid"},         {"Maranta", "마란타", "fern_humid"},
    {"Alocasia", "알로카시아", "tropical_foliage"},    {"Anthurium", "안스리움", "tropical_foliage"},
    {"Peperomia", "페페로미아", "tropical_foliage"},   {"Pilea", "필레아", "tropical_foliage"},
    {"Hoya", "호야", "tropical_foliage"},             {"Tradescantia", "트라데스칸티아", "tropical_foliage"},
    {"Chlorophytum", "접란", "tropical_foliage"},     {"Syngonium", "싱고니움", "tropical_foliage"},
    {"Schefflera", "쉐플레라", "tropical_foliage"},    {"Pachira", "파키라", "tropical_foliage"},
    {"Chamaedorea", "테이블야자", "tropical_foliage"}, {"Dypsis", "아레카야자", "tropical_foliage"},
    {"Dieffenbachia", "디펜바키아", "tropical_foliage"}, {"Hedera", "아이비", "temperate_foliage"},
    {"Nephrolepis", "고사리", "fern_humid"},          {"Asplenium", "아스플레니움", "fern_humid"},
    {"Adiantum", "아디안텀", "fern_humid"},           {"Echeveria", "에케베리아", "succulent"},
    {"Crassula", "크라슐라", "succulent"},            {"Aloe", "알로에", "succulent"},
    {"Haworthia", "하월시아", "succulent"},           {"Haworthiopsis", "하월시아", "succulent"},
    {"Kalanchoe", "칼랑코에", "succulent"},           {"Sedum", "세덤", "succulent"},
    {"Graptopetalum", "그랍토페탈룸", "succulent"},    {"Opuntia", "선인장", "succulent"},
    {"Mammillaria", "선인장", "succulent"},           {"Echinopsis", "선인장", "succulent"},
    {"Schlumbergera", "게발선인장", "succulent"},      {"Phalaenopsis", "호접란", "flowering"},
    {"Saintpaulia", "아프리칸 바이올렛", "flowering"}, {"Begonia", "베고니아", "flowering"},
    {"Pelargonium", "제라늄", "flowering"},           {"Rosa", "장미", "flowering"},
    {"Chrysanthemum", "국화", "flowering"},           {"Hydrangea", "수국", "flowering"},
    {"Cyclamen", "시클라멘", "flowering"},            {"Hibiscus", "히비스커스", "flowering"},
    {"Petunia", "페튜니아", "flowering"},             {"Tulipa", "튤립", "flowering"},
    {"Viola", "제비꽃", "flowering"},
};

// Families: Korean name, and a group only where the family is uniform enough (PLAN.md 4.2).
static const Row FAMILIES[] = {
    {"Lamiaceae", "꿀풀과", "herb"},              {"Araceae", "천남성과", "tropical_foliage"},
    {"Crassulaceae", "돌나물과", "succulent"},     {"Cactaceae", "선인장과", "succulent"},
    {"Solanaceae", "가지과", "fruiting_veg"},      {"Asteraceae", "국화과", ""},
    {"Brassicaceae", "십자화과", "leafy_veg"},     {"Apiaceae", "산형과", "herb"},
    {"Amaranthaceae", "비름과", "leafy_veg"},      {"Cucurbitaceae", "박과", "fruiting_veg"},
    {"Fabaceae", "콩과", ""},                     {"Rosaceae", "장미과", ""},
    {"Moraceae", "뽕나무과", "tropical_foliage"},  {"Asparagaceae", "비짜루과", ""},
    {"Marantaceae", "마란타과", "fern_humid"},     {"Orchidaceae", "난초과", "flowering"},
    {"Gesneriaceae", "제스네리아과", "flowering"},  {"Begoniaceae", "베고니아과", "flowering"},
    {"Geraniaceae", "쥐손이풀과", "flowering"},    {"Piperaceae", "후추과", "tropical_foliage"},
    {"Urticaceae", "쐐기풀과", "tropical_foliage"}, {"Commelinaceae", "닭의장풀과", "tropical_foliage"},
    {"Araliaceae", "두릅나무과", "tropical_foliage"}, {"Arecaceae", "야자나무과", "tropical_foliage"},
    {"Nephrolepidaceae", "줄고사리과", "fern_humid"}, {"Aspleniaceae", "꼬리고사리과", "fern_humid"},
    {"Polypodiaceae", "고란초과", "fern_humid"},   {"Bromeliaceae", "파인애플과", "tropical_foliage"},
    {"Malvaceae", "아욱과", ""},                  {"Liliaceae", "백합과", "flowering"},
    {"Amaryllidaceae", "수선화과", ""},            {"Euphorbiaceae", "대극과", ""},
    {"Primulaceae", "앵초과", "flowering"},        {"Hydrangeaceae", "수국과", "flowering"},
    {"Apocynaceae", "협죽도과", ""},               {"Poaceae", "벼과", ""},
};

static const Row *find(const Row *t, size_t n, const char *key) {
    if (!key || !*key) return nullptr;
    for (size_t i = 0; i < n; i++)
        if (!strcasecmp(t[i].key, key)) return &t[i];
    return nullptr;
}

PlantName plant_lookup(const char *sci, const char *genus, const char *family) {
    PlantName r{"", "", "", 0};
    const Row *f = find(FAMILIES, sizeof FAMILIES / sizeof *FAMILIES, family);
    if (f) {
        r.family_ko = f->ko;
        r.group = f->group;
        r.level = 1;
    }
    const Row *g = find(GENERA, sizeof GENERA / sizeof *GENERA, genus);
    if (g) {
        r.ko = g->ko;
        r.group = g->group;
        r.level = 2;
    }
    // "Mentha × piperita" comes with the multiplication sign; the table spells it with x.
    char s[64] = "";
    if (sci) {
        size_t o = 0;
        for (const char *p = sci; *p && o < sizeof s - 1; p++) {
            if ((unsigned char)p[0] == 0xC3 && (unsigned char)p[1] == 0x97) { s[o++] = 'x'; p++; }
            else s[o++] = *p;
        }
        s[o] = 0;
    }
    const Row *sp = find(SPECIES, sizeof SPECIES / sizeof *SPECIES, s);
    if (sp) {
        r.ko = sp->ko;
        r.group = sp->group;
        r.level = 3;
    }
    return r;
}

static const Row GROUPS[] = {
    {"succulent", "다육식물", ""},        {"tropical_foliage", "관엽식물", ""},
    {"fern_humid", "고사리류", ""},       {"herb", "허브", ""},
    {"leafy_veg", "잎채소", ""},          {"fruiting_veg", "열매채소", ""},
    {"temperate_foliage", "관엽식물", ""}, {"flowering", "꽃식물", ""},
};

const char *group_ko(const char *group) {
    const Row *g = find(GROUPS, sizeof GROUPS / sizeof *GROUPS, group);
    return g ? g->ko : "식물";
}

bool group_valid(const char *group) { return find(GROUPS, sizeof GROUPS / sizeof *GROUPS, group) != nullptr; }

// Last Hangul syllable of a UTF-8 string, 0 if the string does not end in one (digits, Latin: no batchim).
static unsigned last_syllable(const char *w) {
    unsigned last = 0;
    for (const unsigned char *p = (const unsigned char *)w; *p;) {
        unsigned cp;
        if (*p < 0x80) { cp = *p; p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { cp = 0; p++; }
        if (cp != ' ' && cp != ')') last = cp;
    }
    return last;
}

bool ko_batchim(const char *word) {
    unsigned c = last_syllable(word);
    return c >= 0xAC00 && c <= 0xD7A3 && (c - 0xAC00) % 28 != 0;
}

bool ko_rieul(const char *word) {
    unsigned c = last_syllable(word);
    return c >= 0xAC00 && c <= 0xD7A3 && (c - 0xAC00) % 28 == 8;
}
