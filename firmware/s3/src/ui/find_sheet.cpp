// Find-plant sheet (UI.md 3, ui/mockups/html/result.html): the steps while identify.cpp works, then the
// result: photo, name and confidence, the eight targets, the AI introduction. Closes itself 10 s after
// the introduction is in; the main screen is rebuilt with the new profile when it closes.
#include "ui_int.h"
#include "ui.h"
#include "app.h"
#include "identify.h"
#include "plants.h"
#include "settings.h"
#include "nodes.h"

static const int SX = 96, SY = 14, SW = 608, SH = 452;
static const uint32_t AUTO_CLOSE_MS = 10000;

static struct {
    lv_obj_t *box, *photo, *img, *count, *cam_icon;
    lv_obj_t *headline, *conf, *sci, *src;
    lv_obj_t *val[8];
    lv_obj_t *intro;
    lv_obj_t *step_ic[4], *step_sp[4], *end;
    lv_timer_t *tmr;
    lv_img_dsc_t dsc;
    uint32_t gen;
    uint32_t intro_ms;          // when the introduction became ready (auto-close timer)
    bool applied;               // the profile changed: rebuild the main screen on close
    bool open;
} F;

static void close_now() {
    if (!F.open) return;
    F.open = false;
    if (F.tmr) lv_timer_del(F.tmr);
    F.tmr = nullptr;
    bool rebuild = F.applied;
    sheet_close();
    memset(&F, 0, sizeof F);
    cam_set_wanted(true);
    if (rebuild) ui_rebuild_main();
}

static void close_ev(lv_event_t *) { lv_async_call([](void *) { close_now(); }, nullptr); }

// ---------------------------------------------------------------- build

static const struct { const char *icon; uint32_t rgb; const char *name; } CELLS[8] = {
    {F7_SUN_MAX_FILL, 0xFF9F0A, "낮 온도"},   {F7_MOON_FILL, 0x5E5CE6, "밤 온도"},
    {F7_DROP_FILL, 0x0A84FF, "흙 수분"},      {F7_BOLT_FILL, 0xBF5AF2, "양분 EC"},
    {F7_EYEDROPPER_HALFFULL, 0x30D158, "pH"}, {F7_LIGHTBULB_FILL, 0xFFCC00, "조명"},
    {F7_DROP_TRIANGLE_FILL, 0xBF5AF2, "물약"}, {F7_TIMER, 0x40C8E0, "물"},
};
static const char *STEPS[4] = {"사진", "식물 확인", "환경 계산", "적용"};

static void build() {
    lv_obj_t *s = F.box = sheet_begin(SX, SY, SW, SH);
    lv_obj_t *x = sheet_circle(s, F7_XMARK, false, close_ev);
    lv_obj_set_pos(x, 14, 11);
    lv_obj_t *t = label(s, &f_s16, P->text, "식물 확인");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 19);
    lv_obj_t *ok = sheet_circle(s, F7_CHECKMARK, true, close_ev);
    lv_obj_set_pos(ok, SW - 14 - 34, 11);

    // Photo 120x90 (the 200x150 picture at 0.6), round corners by clipping.
    F.photo = fill(s, 18, 58, 120, 90, P->sheet_plat, 12);
    lv_obj_set_style_clip_corner(F.photo, true, 0);
    F.cam_icon = labelc(F.photo, &i40, P->text3.c, F7_CAMERA_VIEWFINDER);
    lv_obj_center(F.cam_icon);
    F.count = label(F.photo, &f_b32, P->text, "");
    lv_obj_center(F.count);
    F.img = lv_img_create(F.photo);
    lv_obj_set_pos(F.img, -40, -30);
    lv_img_set_pivot(F.img, 0, 0);
    lv_img_set_zoom(F.img, 154);
    show(F.img, false);

    // f_b32 like the main screen's name (a full-Korean 26 px font would cost 0.8 MB of PSRAM: the app's
    // read-only data is copied there). A name too long for it drops to f_b15 (refresh()).
    F.headline = label(s, &f_b32, P->text, "");
    lv_obj_set_style_text_letter_space(F.headline, -1, 0);
    lv_obj_set_pos(F.headline, 152, 56);
    F.conf = capsule(s, "", &f_s13, P->ok_sheet, P->ok_text, 24, 9);
    show(F.conf, false);
    F.sci = label(s, &f_r14, P->text2, "");
    lv_obj_set_pos(F.sci, 152, 98);
    lv_obj_set_width(F.sci, SW - 152 - 18);
    one_line(F.sci);
    F.src = label(s, &f_r13, P->text2, "");
    lv_obj_set_pos(F.src, 152, 122);
    lv_obj_set_width(F.src, SW - 152 - 18);
    one_line(F.src);

    lv_obj_set_pos(label(s, &f_b15, P->text, "AI가 정한 환경"), 32, 162);
    lv_obj_t *g = fill(s, 18, 186, SW - 36, 4 * 34, P->sheet_plat, 14);
    int cw = (SW - 36) / 2;
    fill(g, cw, 0, 1, 4 * 34, P->sheet_sep, 0);
    for (int i = 0; i < 8; i++) {
        int cx = (i % 2) * cw, cy = (i / 2) * 34;
        if (i >= 2) fill(g, cx + 44, cy, cw - 44, 1, P->sheet_sep, 0);
        lv_obj_t *ic = vic(g, CELLS[i].icon, CELLS[i].rgb, 24, &i13);
        lv_obj_set_pos(ic, cx + 12, cy + 5);
        lv_obj_set_pos(label(g, &f_m15, P->text, CELLS[i].name), cx + 44, cy + 8);
        F.val[i] = label(g, &f_s14, P->text, "–");
        lv_obj_align(F.val[i], LV_ALIGN_TOP_LEFT, cx + cw - 12, cy + 9);
    }

    lv_obj_set_pos(label(s, &f_b15, P->text, "AI 소개"), 32, 332);
    lv_obj_t *ib = fill(s, 18, 356, SW - 36, 64, P->sheet_plat, 14);
    F.intro = label(ib, &f_r14, P->text, "");
    lv_obj_set_pos(F.intro, 14, 9);
    lv_obj_set_width(F.intro, SW - 36 - 28);
    lv_label_set_long_mode(F.intro, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(F.intro, 3, 0);

    int sx = 32;
    for (int i = 0; i < 4; i++) {
        F.step_ic[i] = labelc(s, &i13, P->text3.c, F7_CIRCLE);
        lv_obj_set_pos(F.step_ic[i], sx, 430);
        F.step_sp[i] = spinner(s, 13);
        lv_obj_set_pos(F.step_sp[i], sx, 431);
        show(F.step_sp[i], false);
        lv_obj_t *l = label(s, &f_r13, P->text2, STEPS[i]);
        lv_obj_set_pos(l, sx + 17, 429);
        sx += 17 + text_w(&f_r13, STEPS[i]) + 14;
    }
    F.end = label(s, &f_r13, P->text2, "");
    lv_obj_align(F.end, LV_ALIGN_TOP_RIGHT, -32, 429);
}

// ---------------------------------------------------------------- update

static void right_align(lv_obj_t *l, int right_x) {
    lv_obj_update_layout(l);
    lv_obj_set_x(l, right_x - lv_obj_get_width(l));
}

static void fill_values(bool have) {
    const Profile &p = g_prof;
    char b[8][24];
    if (!have) {
        for (int i = 0; i < 8; i++) strlcpy(b[i], "–", sizeof b[i]);
    } else {
        snprintf(b[0], 24, "%.0f–%.0f°C", p.day_lo, p.day_hi);
        snprintf(b[1], 24, "%.0f–%.0f°C", p.night_lo, p.night_hi);
        snprintf(b[2], 24, "%.0f–%.0f%%", p.soil_lo, p.soil_hi);
        snprintf(b[3], 24, "%.1f–%.1f", p.ec_lo, p.ec_hi);
        snprintf(b[4], 24, "%.1f–%.1f", p.ph_lo, p.ph_hi);
        snprintf(b[5], 24, "%02u:%02u–%02u:%02u", p.light_on_h, p.light_on_m, p.light_off_h, p.light_off_m);
        if (p.llm && p.nutr_ml) snprintf(b[6], 24, "%u일마다 %uml", p.nutr_days, p.nutr_ml);
        else strlcpy(b[6], p.llm ? "주지 않음" : "멈춤", 24);
        strlcpy(b[7], "흙이 마르면", 24);
    }
    int cw = (SW - 36) / 2;
    for (int i = 0; i < 8; i++) {
        set_text(F.val[i], b[i]);
        right_align(F.val[i], (i % 2) * cw + cw - 12);
    }
}

static void steps(FindStep st, bool stopped) {
    // Which step is running (spinner); everything before it is done (check).
    int cur = st <= FIND_PHOTO ? 0 : st == FIND_PLANTNET ? 1 : st == FIND_PROFILE ? 2 : st == FIND_APPLY ? 3 : 4;
    for (int i = 0; i < 4; i++) {
        bool done = i < cur && !(stopped && i == 0);
        bool run = i == cur && !stopped && st != FIND_IDLE;
        show(F.step_sp[i], run);
        show(F.step_ic[i], !run);
        set_text(F.step_ic[i], done ? F7_CHECKMARK_CIRCLE_FILL : F7_CIRCLE);
        lv_obj_set_style_text_color(F.step_ic[i], done ? P->ok_text : P->text3.c, 0);
    }
}

static void refresh() {
    FindInfo i = find_info();
    const Profile &p = g_prof;
    bool stopped = i.step == FIND_STOPPED;
    steps(i.step, stopped);

    // Photo area: countdown, then the picture.
    const uint16_t *ph = find_photo();
    bool pic = ph && i.step >= FIND_PLANTNET && !stopped;
    if (pic && F.dsc.data != (const uint8_t *)ph) {
        F.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        F.dsc.header.w = 200;
        F.dsc.header.h = 150;
        F.dsc.data_size = 200 * 150 * 2;
        F.dsc.data = (const uint8_t *)ph;
        lv_img_set_src(F.img, &F.dsc);
    }
    show(F.img, pic);
    show(F.cam_icon, !pic && i.step != FIND_COUNT);
    char b[160];
    if (i.step == FIND_COUNT) snprintf(b, sizeof b, "%u", i.count);
    else b[0] = 0;
    set_text(F.count, b);
    show(F.count, i.step == FIND_COUNT);

    // Headline.
    const char *h = i.headline;
    if (i.step <= FIND_PHOTO && i.step != FIND_IDLE) h = i.step == FIND_COUNT ? "사진을 찍을게요" : "사진 찍는 중";
    else if (i.step == FIND_PLANTNET) h = "식물 확인 중";
    else if (i.step == FIND_PROFILE && !h[0]) h = "환경 계산 중";
    else if (stopped) h = "사진을 받지 못했어요";
    set_text(F.headline, h);
    // Room for the name: up to the right edge, less the confidence capsule.
    int room = SW - 152 - 18 - 110;
    bool small = text_w(&f_b32, h) > room;
    lv_obj_set_style_text_font(F.headline, small ? &f_b15 : &f_b32, 0);
    lv_obj_set_y(F.headline, small ? 68 : 56);
    lv_obj_update_layout(F.headline);
    bool conf = i.step >= FIND_PROFILE && !stopped && i.conf != CONF_NONE && i.score;
    if (conf) {
        snprintf(b, sizeof b, "확신도 %u%%", i.score);
        set_text(lv_obj_get_child(F.conf, 0), b);
        lv_obj_align_to(F.conf, F.headline, LV_ALIGN_OUT_RIGHT_MID, 8, 1);
    }
    show(F.conf, conf);

    // Scientific name line, source line.
    if (i.step >= FIND_APPLY && !stopped && p.identified) {
        snprintf(b, sizeof b, "%s%s%s · %s", p.sci, p.family_ko[0] ? " · " : "", p.family_ko, group_ko(p.group));
    } else if (i.step == FIND_COUNT) strlcpy(b, "식물이 사진 가운데 오게 해 주세요", sizeof b);
    else if (i.retake && i.step <= FIND_PLANTNET) strlcpy(b, "식물이 가운데 오도록 다시 찍을게요", sizeof b);
    else b[0] = 0;
    set_text(F.sci, b);
    if (i.note[0]) {
        set_text(F.src, i.note);
        lv_obj_set_style_text_color(F.src, P->warn_text, 0);
    } else {
        set_text(F.src, i.step >= FIND_APPLY ? (i.cached ? F7_CAMERA_VIEWFINDER " PlantNet으로 식물 확인 · " F7_SPARKLES " 저장된 AI 환경"
                                                         : F7_CAMERA_VIEWFINDER " PlantNet으로 식물 확인 · " F7_SPARKLES " AI가 환경 계산")
                                             : "");
        lv_obj_set_style_text_color(F.src, P->text2.c, 0);
    }

    fill_values(i.step >= FIND_DONE);
    if (i.step >= FIND_APPLY && !stopped) F.applied = true;

    const char *in = i.step < FIND_DONE ? "" : i.intro_state == 2 ? p.intro : "소개를 쓰는 중…";
    set_text(F.intro, in);

    // Auto close.
    if ((i.step == FIND_DONE && i.intro_state == 2) || stopped) {
        if (!F.intro_ms) F.intro_ms = millis();
    }
    if (F.intro_ms) {
        uint32_t el = millis() - F.intro_ms;
        if (el >= AUTO_CLOSE_MS) { lv_async_call([](void *) { close_now(); }, nullptr); return; }
        snprintf(b, sizeof b, "%lu초 뒤 닫힘", (unsigned long)((AUTO_CLOSE_MS - el + 999) / 1000));
        set_text(F.end, b);
        lv_obj_align(F.end, LV_ALIGN_TOP_RIGHT, -32, 429);
    } else set_text(F.end, "");
}

static void tick(lv_timer_t *) {
    if (!F.open) return;
    if (find_gen() != F.gen || F.intro_ms) {
        F.gen = find_gen();
        refresh();
    }
}

void find_sheet_open() {
    if (F.open) return;
    memset(&F, 0, sizeof F);
    F.open = true;
    cam_set_wanted(false);
    build();
    F.gen = find_gen() - 1;
    refresh();
    F.tmr = lv_timer_create(tick, 200, nullptr);
}

// ---------------------------------------------------------------- change kind

static FindChoice s_choices[8];

static void pick_ev(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    static FindChoice c;
    c = s_choices[k];
    lv_async_call([](void *) {
        sheet_close();
        if (find_choose(c.sci, c.genus, c.family, c.score)) find_sheet_open();
    }, nullptr);
}

void kind_sheet_open() {
    int nc = find_candidates(s_choices, 4);
    int nr = find_recent(s_choices + nc, 8 - nc);
    if (nc + nr == 0) {
        sheet_info("종류 바꾸기", "식물을 한 번 찾으면 PlantNet 후보와 최근 식물 중에서 고를 수 있어요.");
        return;
    }
    int rows = nc + nr;
    int h = 70 + rows * 46 + (nc && nr ? 30 : 0) + 20;
    if (h > 452) h = 452;
    lv_obj_t *s = sheet_begin(170, (480 - h) / 2, 460, h);
    lv_obj_t *x = sheet_circle(s, F7_XMARK, false, [](lv_event_t *) { lv_async_call([](void *) { sheet_close(); }, nullptr); });
    lv_obj_set_pos(x, 14, 11);
    lv_obj_t *t = label(s, &f_s16, P->text, "종류 바꾸기");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 19);
    int y = 60;
    for (int i = 0; i < rows && y + 46 <= h - 10; i++) {
        if (i == nc && nc) {
            lv_obj_set_pos(label(s, &f_s13, P->text2, "최근 식물"), 30, y + 6);
            y += 30;
        }
        lv_obj_t *r = fill(s, 18, y, 424, 42, P->sheet_plat, 12);
        make_button(r, pick_ev, (void *)(intptr_t)i);
        lv_obj_t *n = label(r, &f_m15, P->text, s_choices[i].ko);
        lv_obj_set_pos(n, 14, 4);
        char b[96];
        if (i < nc) snprintf(b, sizeof b, "%s · %u%%", s_choices[i].sci, s_choices[i].score);
        else strlcpy(b, s_choices[i].sci, sizeof b);
        lv_obj_set_pos(label(r, &f_r12, P->text2, b), 14, 23);
        y += 46;
    }
}
