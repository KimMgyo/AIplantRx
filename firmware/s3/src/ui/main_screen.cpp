// Main screen (UI.md 2): plant, today's schedule, AI note, buttons, camera and thermal tiles.
// Positions follow ui/mockups/html/style.css (the main section) pixel for pixel.
#include "ui_int.h"
#include "app.h"
#include "llm.h"
#include "net.h"
#include "nodes.h"
#include "settings.h"
#include <math.h>
#include <time.h>

static const int CAM_X = 464, CAM_Y[2] = {16, 244}, CAM_W = 320, CAM_H = 220, CAM_R = 20;

static struct {
    lv_obj_t *time, *date, *icons[4];
    lv_obj_t *name, *aicap, *sub, *run_dot, *run;
    lv_obj_t *track[4], *bold[4], *val[4], *now, *now_gap;
    lv_obj_t *ai_meta, *ai_why, *ai_text;
    lv_obj_t *tile[2], *img[2], *wait[2];
    lv_obj_t *fps_tag, *fps_lbl, *th_tag, *th_lbl, *th_box, *th_plabel, *th_plabel_l, *th_xh[2], *th_xhv, *th_xhv_l, *th_leg;
    lv_obj_t *th_hi, *th_lo, *th_hi_p, *th_lo_p;
    lv_img_dsc_t dsc[2];
    lv_img_dsc_t *bg, *caps[8];
    lv_timer_t *tmr;
    uint32_t seq[2], lanes_sig, tick, cam_t, slow_t;
    bool th_have;
    ThermalGrid grid;
} M;

static int text_w(const lv_font_t *f, const char *t) {
    lv_point_t p;
    lv_txt_get_size(&p, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

// A small translucent tag over video (one of the allowed translucent pieces).
static lv_obj_t *tag(lv_obj_t *par, const char *txt, bool live, lv_obj_t **lbl) {
    lv_obj_t *o = box(par, 0, 0, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, 115, 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_pad_hor(o, 9, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 5, 0);
    if (live) fill(o, 0, 0, 6, 6, lv_color_hex(0xFF453A), 3);
    lv_obj_t *l = labelc(o, &f_s12, lv_color_white(), txt);
    if (lbl) *lbl = l;
    return o;
}

static lv_obj_t *pill_dark(lv_obj_t *par, const lv_font_t *f, int h, int padx, lv_obj_t **lbl) {
    lv_obj_t *o = box(par, 0, 0, LV_SIZE_CONTENT, h);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, 115, 0);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_pad_hor(o, padx, 0);
    *lbl = labelc(o, f, lv_color_white(), "");
    lv_obj_center(*lbl);
    return o;
}

// ---------------------------------------------------------------- schedule lanes

static lv_obj_t *seg(lv_obj_t *track, float a, float b, lv_color_t c) {
    int x0 = (int)lroundf(a * 224), x1 = (int)lroundf(b * 224);
    if (x1 - x0 < 4) x1 = x0 + 4;
    return fill(track, x0, 0, x1 - x0, 8, c, 4);
}

static void hm(char *o, size_t n, int h, int m) { snprintf(o, n, "%02d:%02d", h, m); }

static void lanes_update(bool force) {
    SensorState s = sensor_get();
    const Profile &p = g_prof;
    uint32_t sig = s.live ? 1 : 0;
    sig = sig * 31 + s.light + 2 * s.water + 4 * s.nutr + 8 * s.ph_pump;
    sig = sig * 31 + s.water_ml_today;
    sig = sig * 31 + s.ph_ml_today;
    sig = sig * 31 + (isnan(s.ph) ? 0 : (int)lroundf(s.ph * 10));
    sig = sig * 31 + s.nev;
    for (int i = 0; i < s.nev; i++) sig = sig * 31 + s.ev[i].at_min * 7 + s.ev[i].kind;
    sig = sig * 31 + p.light_on_h * 60 + p.light_on_m + (p.light_off_h * 60 + p.light_off_m) * 1440;
    sig = sig * 31 + s.next_nutr;
    if (!force && sig == M.lanes_sig) return;
    M.lanes_sig = sig;
    for (int i = 0; i < 4; i++) lv_obj_clean(M.track[i]);
    char b[32], v[32];

    // Light: the schedule is the plan, whether or not the node is there.
    float on = (p.light_on_h * 60 + p.light_on_m) / 1440.0f, off = (p.light_off_h * 60 + p.light_off_m) / 1440.0f;
    lv_color_t yel = lv_color_hex(0xFFD60A);
    if (off > on) seg(M.track[0], on, off, yel);
    else { seg(M.track[0], 0, off, yel); seg(M.track[0], on, 1, yel); }
    char a1[8], a2[8];
    hm(a1, sizeof a1, p.light_on_h, p.light_on_m);
    hm(a2, sizeof a2, p.light_off_h, p.light_off_m);
    if (s.live) {
        set_text(M.bold[0], s.light ? "켜짐" : "꺼짐");
        snprintf(v, sizeof v, s.light ? "%s 끔" : "%s 켬", s.light ? a2 : a1);
    } else {
        set_text(M.bold[0], "");
        snprintf(v, sizeof v, "%s–%s", a1, a2);
    }
    set_text(M.val[0], v);

    // Water, nutrient, pH: when each pump ran today (a dot per run); the rule when the node is not there.
    // No heater: the box has no actuator for temperature (ARCHITECTURE.md 8, 11-13).
    lv_color_t cyan = lv_color_hex(0x32ADE6), pur = lv_color_hex(0xBF5AF2), grn = lv_color_hex(0x30D158);
    if (s.live) {
        for (int i = 0; i < s.nev; i++) {
            int x = (int)lroundf(s.ev[i].at_min / 1440.0f * 224) - 4;
            int lane = s.ev[i].kind == EV_WATER ? 1 : s.ev[i].kind == EV_NUTR ? 2 : 3;
            fill(M.track[lane], x, 0, 8, 8, lane == 1 ? cyan : lane == 2 ? pur : grn, 4);
        }
        set_text(M.bold[1], s.water ? "주는 중" : "대기");
        snprintf(v, sizeof v, "오늘 %uml", s.water_ml_today);
        set_text(M.val[1], v);
        if (s.next_nutr > 1700000000) {
            time_t t = s.next_nutr;
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(b, sizeof b, "%d/%d", tm.tm_mon + 1, tm.tm_mday);
        } else strlcpy(b, "-", sizeof b);
        set_text(M.bold[2], s.nutr ? "주는 중" : b);
        snprintf(v, sizeof v, "다음 %uml", p.nutr_ml);
        set_text(M.val[2], v);
        set_text(M.bold[3], s.ph_pump ? "넣는 중" : "대기");
        if (isnan(s.ph)) strlcpy(v, "pH -", sizeof v);
        else snprintf(v, sizeof v, "pH %.1f", s.ph);
        set_text(M.val[3], v);
    } else {
        set_text(M.bold[1], "");
        snprintf(v, sizeof v, "흙 %.0f%% 아래면", p.soil_lo);
        set_text(M.val[1], v);
        set_text(M.bold[2], "");
        snprintf(v, sizeof v, "%u일마다 %uml", p.nutr_days, p.nutr_ml);
        set_text(M.val[2], v);
        set_text(M.bold[3], "");
        snprintf(v, sizeof v, "%.1f–%.1f 유지", p.ph_lo, p.ph_hi);
        set_text(M.val[3], v);
    }
    for (int i = 0; i < 4; i++) show(M.bold[i], *lv_label_get_text(M.bold[i]));
}

// ---------------------------------------------------------------- status line under the name

static void run_update() {
    SensorState s = sensor_get();
    const Profile &p = g_prof;
    const char *t;
    Col c{P->ok_text, LV_OPA_COVER};
    if (!s.live) {
        t = "센서 노드를 기다리는 중";
        c = P->text2;
    } else if (!isnan(s.soil_pct) && s.soil_pct < p.soil_lo) {
        t = s.water ? "흙이 말라서 물 주는 중" : "흙이 말랐어요";
        c = Col{P->warn_text, LV_OPA_COVER};
    } else if (!isnan(s.air_c) && s.air_c < (s.day ? p.day_lo : p.night_lo) - 2) {
        t = "온도가 낮아요";
        c = Col{P->warn_text, LV_OPA_COVER};
    } else if (!isnan(s.air_c) && s.air_c > p.day_hi + 2) {
        t = "온도가 높아요";
        c = Col{P->warn_text, LV_OPA_COVER};
    } else {
        t = p.identified ? "AI 맞춤 환경으로 자동 운영 중" : "기본 환경으로 자동 운영 중";
    }
    set_text(M.run, t);
    set_col(M.run, c);
    set_bg(M.run_dot, c.c, c.o);
}

// ---------------------------------------------------------------- tick
//
// One 33 ms timer. Video: point each tile at its newest frame (no copy). Everything else: every 250 ms,
// but in the same refresh as a video frame while video is on screen.
//
// Why together: the display runs LVGL's direct mode on two framebuffers. A refresh that leaves out an
// area the previous refresh drew makes LVGL copy that area into the other framebuffer first. With the
// tiles updating on their own (camera 10 Hz, thermal 8 Hz, labels 4 Hz) nearly every refresh copied a
// 320x220 tile through PSRAM besides drawing one. Drawn together, consecutive refreshes cover the same
// areas and the copy disappears.

// LVGL 8.4 reads no header from an image descriptor whose data is still NULL, so a tile built before
// its first frame got a 0x0 image that never drew. The first frame sets the source again.
static void tile_frame(int k, const uint16_t *f) {
    bool first = !M.dsc[k].data;
    M.dsc[k].data = (const uint8_t *)f;
    if (first) lv_img_set_src(M.img[k], &M.dsc[k]);
    else lv_obj_invalidate(M.img[k]);
}

// The thermal tile has no image: its draw event interpolates the 32x24 grid (in internal RAM) into the
// framebuffer row by row. The old way built a 320x240 picture in PSRAM on core 0 and then had LVGL
// copy it through PSRAM again, the most expensive thing on the main screen after the camera tile.
static uint8_t s_x0[320], s_xw[320], s_y0[240], s_yw[240];

// Where output pixel i falls between cell centers, at 10 px per cell: (i + 0.5) / 10 - 0.5 cells, as
// the left (top) cell and a weight in 1/256.
static void cell_map(int n, int cells, uint8_t *c0, uint8_t *w) {
    for (int i = 0; i < n; i++) {
        int num = 2 * i - 9;                    // in 1/20 cell
        if (num < 0) { c0[i] = 0; w[i] = 0; continue; }
        c0[i] = num / 20 < cells - 1 ? num / 20 : cells - 1;
        w[i] = c0[i] == cells - 1 ? 0 : (num % 20) * 256 / 20;
    }
}

static void th_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_COVER_CHECK) {
        lv_cover_check_info_t *info = (lv_cover_check_info_t *)lv_event_get_param(e);
        if (M.th_have && _lv_area_is_in(info->area, &o->coords, 0)) info->res = LV_COVER_RES_COVER;
        return;
    }
    if (!M.th_have) return;
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_area_t a;
    if (!_lv_area_intersect(&a, dc->clip_area, &o->coords)) return;
    const lv_coord_t bw = lv_area_get_width(dc->buf_area);
    const uint16_t *lut = inferno_lut(), *u = M.grid.u;
    int32_t row[33];
    for (int y = a.y1; y <= a.y2; y++) {
        int ty = y - o->coords.y1, c0 = s_y0[ty];
        const uint16_t *ra = u + c0 * 32, *rb = u + (c0 < 23 ? c0 + 1 : 23) * 32;
        int wy = s_yw[ty];
        for (int j = 0; j < 32; j++) row[j] = ra[j] + (((int32_t)rb[j] - ra[j]) * wy >> 8);
        row[32] = row[31];
        lv_color_t *d = (lv_color_t *)dc->buf + (y - dc->buf_area->y1) * bw + (a.x1 - dc->buf_area->x1);
        for (int x = a.x1 - o->coords.x1, x2 = a.x2 - o->coords.x1; x <= x2; x++) {
            int c = s_x0[x];
            int k = (row[c] + ((row[c + 1] - row[c]) * s_xw[x] >> 8)) >> 8;
            (d++)->full = lut[k > 255 ? 255 : k];
        }
    }
}


static void slow_update();

static void vtick(lv_timer_t *) {
    if (lv_scr_act() != g_main) return;
    bool cam_on = !lv_obj_has_flag(M.img[0], LV_OBJ_FLAG_HIDDEN);
    bool th_on = !lv_obj_has_flag(M.img[1], LV_OBJ_FLAG_HIDDEN);
    bool n0 = cam_seq() != M.seq[0], n1 = thermal_seq() != M.seq[1];
    // A thermal frame waits for the next camera frame, unless the camera has gone quiet.
    bool video = n0 || (n1 && (!cam_on || lv_tick_elaps(M.cam_t) > 150));
    uint32_t seq;
    if (n0) {
        const uint16_t *f = cam_take(&seq);
        if (f) { M.seq[0] = seq; M.cam_t = lv_tick_get(); tile_frame(0, f); }
    }
    if (video && n1) {
        if (thermal_grid(&M.grid, &seq)) {
            M.seq[1] = seq;
            M.th_have = true;
            lv_obj_invalidate(M.img[1]);
        }
    }
    // Both visible tiles in every video refresh, even when only one has a new frame: redrawing a tile
    // from its source (camera 9 ms, thermal 6 ms, measured) is cheaper than the PSRAM-to-PSRAM copy
    // LVGL makes of a tile the previous refresh drew and this one leaves out.
    if (video) {
        if (cam_on) lv_obj_invalidate(M.img[0]);
        if (th_on) lv_obj_invalidate(M.img[1]);
    }
    uint32_t since = lv_tick_elaps(M.slow_t);
    if (since >= 250 && (video || !(cam_on || th_on) || since >= 1000)) {
        M.slow_t = lv_tick_get();
        slow_update();
    }
}

// Everything else. Each call below changes (and redraws) only what differs.
static void slow_update() {
    M.tick++;
    char buf[48];
    fmt_time(buf, sizeof buf);
    bool moved = set_text(M.time, buf);
    fmt_date(buf, sizeof buf);
    moved |= set_text(M.date, buf);
    if (moved) lv_obj_align_to(M.date, M.time, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    bool up[4] = {net_state() == NET_UP, llm_ok(), node_info(ROLE_CAM).online, node_info(ROLE_NODE).online};
    for (int i = 0; i < 4; i++) set_col(M.icons[i], up[i] ? P->status : P->text3);

    bool live = cam_live() && M.dsc[0].data;
    ThermalInfo ti = thermal_info();
    bool tlive = ti.live && M.th_have;
    bool lv[2] = {live, tlive};
    for (int k = 0; k < 2; k++) {
        show(M.img[k], lv[k]);
        show(M.wait[k], !lv[k]);
        set_bg(M.tile[k], lv[k] ? lv_color_black() : P->plat, LV_OPA_COVER);
    }
    lv_obj_t *th_parts[] = {M.th_box, M.th_plabel, M.th_xh[0], M.th_xh[1], M.th_xhv, M.th_leg, M.th_hi_p, M.th_lo_p, M.th_tag};
    for (lv_obj_t *o : th_parts) show(o, tlive);
    show(M.fps_tag, live);

    if (M.tick % 4 == 1) {          // once a second (and on the first tick)
        if (live) { snprintf(buf, sizeof buf, "%.0f fps", cam_fps()); set_text(M.fps_lbl, buf); }
        if (tlive) {
            SensorState s = sensor_get();
            if (s.live && !isnan(s.air_c) && !isnan(ti.leaf_c))
                snprintf(buf, sizeof buf, "공기 %.1f° · 잎 %.1f° · 차이 %+.1f°", s.air_c, ti.leaf_c, ti.leaf_c - s.air_c);
            else snprintf(buf, sizeof buf, "잎 %.1f°", ti.leaf_c);
            set_text(M.th_lbl, buf);
            snprintf(buf, sizeof buf, "잎 %.1f°", ti.leaf_c);
            set_text(M.th_plabel_l, buf);
            int px = ti.peak_x, py = ti.peak_y - 10;
            lv_obj_set_pos(M.th_xh[0], px - 1, py - 8);
            lv_obj_set_pos(M.th_xh[1], px - 8, py - 1);
            snprintf(buf, sizeof buf, "%.1f°", ti.peak_c);
            set_text(M.th_xhv_l, buf);
            lv_obj_set_pos(M.th_xhv, px + 10 > 250 ? px - 70 : px + 10, py - 18 < 0 ? py + 10 : py - 18);
            snprintf(buf, sizeof buf, "%.0f", ti.hi);
            set_text(M.th_hi, buf);
            snprintf(buf, sizeof buf, "%.0f", ti.lo);
            set_text(M.th_lo, buf);
        }
        run_update();
        lanes_update(false);
        time_t t = time(nullptr);
        if (t > 1700000000) {
            struct tm tm;
            localtime_r(&t, &tm);
            float fr = (tm.tm_hour * 60 + tm.tm_min) / 1440.0f;
            int x = 82 + (int)lroundf(fr * 224) - 1;
            lv_obj_set_x(M.now_gap, x - 2);
            lv_obj_set_x(M.now, x);
            show(M.now_gap, true);
            show(M.now, true);
        }
    }
}

// ---------------------------------------------------------------- events

static void gear_ev(lv_event_t *) { ui_show_settings(0); }

static void find_ev(lv_event_t *) {
    if (!node_info(ROLE_CAM).online)
        sheet_info("식물 찾기", "카메라를 찾는 중이에요. 카메라가 켜지면 사진을 찍어 PlantNet과 AI가 식물을 알아봅니다.");
    else if (!g_set.plantnet_key.length())
        sheet_info("식물 찾기", "설정 > PlantNet에 API 키를 넣으면 식물을 찾을 수 있어요.");
    else
        sheet_info("식물 찾기", "카메라와 PlantNet 키가 준비됐어요. 식물 찾기는 다음 펌웨어에 들어갑니다.");
}

static void kind_ev(lv_event_t *) {
    sheet_info("종류 바꾸기", "식물을 한 번 찾으면 PlantNet 후보와 최근 식물 중에서 고를 수 있어요.");
}

// ---------------------------------------------------------------- build

static void build_lane(lv_obj_t *plan, int i, const char *icon, uint32_t rgb, const char *name) {
    int y = 36 + i * 26;
    lv_obj_t *ib = box(plan, 14, y, 18, 22);
    lv_obj_center(labelc(ib, &i15, lv_color_hex(rgb), icon));
    lv_obj_t *n = label(plan, &f_s13, P->text, name);
    lv_obj_set_pos(n, 40, y + 3);
    M.track[i] = fill(plan, 82, y + 7, 224, 8, P->prog, 4);
    lv_obj_t *st = box(plan, 314, y, 104, 22);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 4, 0);
    M.bold[i] = label(st, &f_s12, P->text, "");
    M.val[i] = label(st, &f_r12, P->text2, "");
}

static void build_tile(lv_obj_t *scr, int k) {
    M.tile[k] = fill(scr, CAM_X, CAM_Y[k], CAM_W, CAM_H, P->plat, 0);
    if (k == 0) {
        lv_img_dsc_t &d = M.dsc[k];
        d.header.cf = LV_IMG_CF_TRUE_COLOR;
        d.header.w = 320;
        d.header.h = 240;
        d.data_size = 320 * 240 * 2;
        d.data = nullptr;
        M.img[k] = lv_img_create(M.tile[k]);
        lv_img_set_src(M.img[k], &d);
    } else {
        cell_map(320, 32, s_x0, s_xw);
        cell_map(240, 24, s_y0, s_yw);
        M.img[k] = box(M.tile[k], 0, 0, 320, 240);
        lv_obj_add_event_cb(M.img[k], th_draw, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_add_event_cb(M.img[k], th_draw, LV_EVENT_COVER_CHECK, nullptr);
    }
    lv_obj_set_pos(M.img[k], 0, -10);     // 1:1, 10 px cut top and bottom
    lv_obj_add_flag(M.img[k], LV_OBJ_FLAG_HIDDEN);
    M.wait[k] = box(M.tile[k], 0, 0, CAM_W, CAM_H);
    lv_obj_t *ic = label(M.wait[k], &i40, P->text3, k ? F7_THERMOMETER : F7_CAMERA_FILL);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *t = label(M.wait[k], &f_r13, P->text2, k ? "열화상을 기다리는 중" : "카메라를 기다리는 중");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 22);
    lv_obj_t *title = tag(M.tile[k], k ? "열화상" : "RGB 카메라", true, nullptr);
    lv_obj_set_pos(title, 10, 10);
}

lv_obj_t *main_build() {
    memset(&M, 0, sizeof(M));
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, P->screen, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    const uint16_t *bg = P->glass ? glass_bg() : nullptr;
    if (bg) {
        M.bg = img_dsc(bg, SCR_W, SCR_H);
        lv_obj_t *im = lv_img_create(scr);
        lv_img_set_src(im, M.bg);
    }

    lv_obj_t *L = box(scr, 16, 16, 432, 448);
    // status line
    M.time = label(L, &f_s13, P->text, "--:--");
    lv_obj_set_pos(M.time, 0, 1);
    M.date = label(L, &f_s13, P->text2, "");
    lv_obj_t *ic = box(L, 300, 0, 132, 18);
    lv_obj_set_flex_flow(ic, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ic, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ic, 11, 0);
    const char *icons[4] = {F7_WIFI, F7_SPARKLES, F7_CAMERA_FILL, F7_THERMOMETER};
    for (int i = 0; i < 4; i++) M.icons[i] = label(ic, &i14, P->text3, icons[i]);

    // plant
    const Profile &p = g_prof;
    lv_obj_t *ph = fill(L, 0, 30, 68, 68, P->plat, 18);
    lv_obj_center(labelc(ph, &i40, lv_color_hex(0x30D158), F7_LEAF_ARROW_CIRCLEPATH));
    M.name = label(L, &f_b32, P->text, p.identified ? p.name_ko : "식물을 찾아 주세요");
    lv_obj_set_style_text_letter_space(M.name, -1, 0);
    lv_obj_set_pos(M.name, 82, 27);
    if (p.identified) {
        char cap[32];
        snprintf(cap, sizeof cap, F7_SPARKLES " AI 확인 %u%%", p.score);
        M.aicap = capsule(L, cap, &f_s13, P->ok_win, P->ok_text, 26, 10);
        lv_obj_update_layout(L);
        lv_obj_align_to(M.aicap, M.name, LV_ALIGN_OUT_RIGHT_MID, 8, 1);
    }
    char sub[128];
    if (p.identified) snprintf(sub, sizeof sub, "%s · %s", p.sci, p.family);
    else strlcpy(sub, "기본 프로필 · 열대 관엽식물 기준", sizeof sub);
    M.sub = label(L, &f_r14, P->text2, sub);
    lv_obj_set_pos(M.sub, 82, 64);
    lv_obj_set_width(M.sub, 432 - 82 - 40);
    one_line(M.sub);
    M.run_dot = fill(L, 82, 87, 7, 7, P->text3.c, 4);
    M.run = label(L, &f_s13, P->text3, "센서 노드를 기다리는 중");
    lv_obj_set_pos(M.run, 95, 82);
    lv_obj_t *gear = fill(L, 396, 30, 36, 36, P->cap, 18);
    lv_obj_center(label(gear, &i19, P->text, F7_GEAR_ALT_FILL));
    lv_obj_set_ext_click_area(gear, 10);
    make_button(gear, gear_ev, nullptr);

    // today
    lv_obj_t *plan = fill(L, 0, 110, 432, 162, P->plat, 18);
    lv_obj_set_pos(label(plan, &f_s14, P->text, "오늘"), 14, 10);
    lv_obj_t *r = label(plan, &f_r12, P->text2, "AI가 정한 일정");
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -14, 12);
    build_lane(plan, 0, F7_LIGHTBULB_FILL, 0xFFD60A, "조명");
    build_lane(plan, 1, F7_DROP_FILL, 0x32ADE6, "물");
    build_lane(plan, 2, F7_DROP_TRIANGLE_FILL, 0xBF5AF2, "물약");
    build_lane(plan, 3, F7_EYEDROPPER_HALFFULL, 0x30D158, "pH");
    // The "now" line: a 2 px line over a 6 px gap in the platter color (an outline on an object this thin
    // is drawn over the object itself).
    M.now_gap = fill(plan, 80, 33, 6, 106, P->plat, 0);
    M.now = fill(plan, 82, 33, 2, 106, P->text.c, 1);
    lv_obj_add_flag(M.now_gap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(M.now, LV_OBJ_FLAG_HIDDEN);
    const char *hours[5] = {"0시", "6", "12", "18", "24시"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *h = label(plan, &f_r11, P->text2, hours[i]);
        int w = text_w(&f_r11, hours[i]);
        int cx = 82 + i * 56;
        lv_obj_set_pos(h, i == 0 ? cx : i == 4 ? cx - w : cx - w / 2, 140);
    }
    lanes_update(true);

    // AI note (UI.md 6.3). Until the AI has said something: a gray hint with no model, time or reason.
    lv_obj_t *ai = fill(L, 0, 284, 432, 112, P->plat, 18);
    lv_obj_t *g = lv_img_create(ai);
    lv_img_set_src(g, ai_glyph());
    lv_obj_set_pos(g, 14, 12);
    lv_obj_t *gs = labelc(ai, &i12, lv_color_white(), F7_SPARKLES);
    lv_obj_align_to(gs, g, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *at = label(ai, &f_s14, P->text, "AI 한마디");
    lv_obj_set_pos(at, 43, 14);
    M.ai_meta = label(ai, &f_r12, P->text2, "");
    lv_obj_update_layout(ai);
    lv_obj_align_to(M.ai_meta, at, LV_ALIGN_OUT_RIGHT_MID, 9, 1);
    M.ai_why = capsule(ai, "", &f_s12, P->cap_plat, P->text.c, 22, 9);
    lv_obj_align(M.ai_why, LV_ALIGN_TOP_RIGHT, -14, 12);
    lv_obj_add_flag(M.ai_why, LV_OBJ_FLAG_HIDDEN);
    M.ai_text = label(ai, &f_r15, P->text2, "식물을 찾으면 AI가 말을 걸어요");
    lv_obj_set_pos(M.ai_text, 14, 40);
    lv_obj_set_width(M.ai_text, 404);
    lv_label_set_long_mode(M.ai_text, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(M.ai_text, true);
    lv_obj_set_style_text_line_space(M.ai_text, 2, 0);

    // buttons
    lv_obj_t *act = box(L, 0, 408, 432, 40);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(act, 8, 0);
    lv_obj_t *pb = box(act, 0, 0, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_bg_color(pb, P->go, 0);
    lv_obj_set_style_bg_opa(pb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pb, 20, 0);
    lv_obj_set_style_pad_hor(pb, 18, 0);
    lv_obj_set_flex_flow(pb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pb, 8, 0);
    labelc(pb, &i19, lv_color_white(), F7_CAMERA_VIEWFINDER);
    labelc(pb, &f_s16, lv_color_white(), "식물 다시 찾기");
    make_button(pb, find_ev, nullptr);
    lv_obj_t *sb = capsule(act, "종류 바꾸기", &f_s16, P->cap, P->cap_text, 40, 18);
    make_button(sb, kind_ev, nullptr);

    // video tiles
    build_tile(scr, 0);
    build_tile(scr, 1);
    M.fps_tag = tag(M.tile[0], "0 fps", false, &M.fps_lbl);
    lv_obj_align(M.fps_tag, LV_ALIGN_TOP_RIGHT, -10, 10);
    M.th_tag = tag(M.tile[1], "", false, &M.th_lbl);
    lv_obj_align(M.th_tag, LV_ALIGN_TOP_RIGHT, -10, 10);
    int bx = LEAF_GX0 * 10, by = LEAF_GY0 * 10 - 10, bw = (LEAF_GX1 - LEAF_GX0) * 10, bh = (LEAF_GY1 - LEAF_GY0) * 10;
    M.th_box = box(M.tile[1], bx, by, bw, bh);
    lv_obj_set_style_border_width(M.th_box, 2, 0);
    lv_obj_set_style_border_color(M.th_box, lv_color_white(), 0);
    lv_obj_set_style_border_opa(M.th_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(M.th_box, 10, 0);
    M.th_plabel = capsule(M.tile[1], "잎 --°", &f_b13, lv_color_white(), lv_color_black(), 22, 8);
    M.th_plabel_l = lv_obj_get_child(M.th_plabel, 0);
    lv_obj_set_pos(M.th_plabel, bx, by + bh + 6);
    M.th_xh[0] = fill(M.tile[1], 0, 0, 2, 16, lv_color_white(), 0);
    M.th_xh[1] = fill(M.tile[1], 0, 0, 16, 2, lv_color_white(), 0);
    M.th_xhv = pill_dark(M.tile[1], &f_s12, 20, 6, &M.th_xhv_l);
    M.th_leg = lv_img_create(M.tile[1]);
    lv_img_set_src(M.th_leg, legend_img());
    lv_obj_set_pos(M.th_leg, CAM_W - 16, 40);
    M.th_hi_p = pill_dark(M.tile[1], &f_b11, 18, 5, &M.th_hi);
    M.th_lo_p = pill_dark(M.tile[1], &f_b11, 18, 5, &M.th_lo);
    lv_obj_align(M.th_hi_p, LV_ALIGN_TOP_RIGHT, -26, 34);
    lv_obj_align(M.th_lo_p, LV_ALIGN_TOP_RIGHT, -26, 158);

    // Round tile corners: pieces of the background on top (no corner clipping).
    for (int k = 0; k < 2; k++)
        for (int c = 0; c < 4; c++) {
            int x = CAM_X + ((c & 1) ? CAM_W - CAM_R : 0), y = CAM_Y[k] + ((c & 2) ? CAM_H - CAM_R : 0);
            M.caps[k * 4 + c] = piece(bg, SCR_W, P->screen, x, y, CAM_R, CAM_R, corner_outside(CAM_R, c));
            lv_obj_t *im = lv_img_create(scr);
            lv_img_set_src(im, M.caps[k * 4 + c]);
            lv_obj_set_pos(im, x, y);
        }

    M.tmr = lv_timer_create(vtick, 33, nullptr);
    return scr;
}

void main_destroy() {
    if (M.tmr) lv_timer_del(M.tmr);
    if (g_main) lv_obj_del(g_main);
    g_main = nullptr;
    for (auto &c : M.caps) piece_free(c);
    if (M.bg) free(M.bg);
    memset(&M, 0, sizeof(M));
}

