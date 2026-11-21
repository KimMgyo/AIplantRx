# PlatformIO pre-build hook: two LVGL 8.4 drawing fixes, applied to the fetched library before it compiles.
#
# 1. lv_refr.c looks for "the top object that covers the area being redrawn" with draw_ctx->buf_area.
#    In direct mode (this board: two full framebuffers, LVGL_PORT_AVOID_TEARING_MODE 3) buf_area is the
#    whole screen, which only the screen itself covers. So every refresh, however small, drew every
#    layer under it from the screen background up: a camera frame repainted the screen and the tile
#    background below the image first. clip_area is the area being redrawn in every mode (in partial
#    mode the two are the same), so it is the one to test.
# 2. lv_img.c's cover check reads its area from lv_event_get_param(e), which is the cover-check info
#    struct, not an area, so an image never covered anything (later LVGL uses info->area).
# 3. Even with the right area an image could not cover: the ancestor (lv_obj) check runs first and
#    answers "not covered" for the image's transparent background, and the image check never turns
#    that back. An image that passes every image check (true color, opaque, not rotated, inside the
#    area) now covers, unless the object itself is translucent. That lets the camera tile skip the tile
#    under it and the glass theme skip the screen fill and background under its sidebar picture.
# 4. lv_obj_get_transformed_area() grows the area by 5 px on every side, transformed or not, and
#    invalidation and the clip to each parent both go through it. Every redraw area was 5 px larger
#    than its object, so the two video tiles (8 px apart) overlapped, were joined into one 320x448
#    area, and only the screen covered that: each video frame repainted the screen, both tiles and
#    everything on them (measured: 20 ms of opaque fills in a 51 ms refresh). It now returns the area
#    unchanged unless the object (or, when recursive, a parent) has a transform angle or zoom.
#
# Measured on the main screen with live video: ~51 ms per refresh before. Idempotent; a file that has
# neither form fails the build, since the library layout then changed and the fix needs a new look.
Import("env")  # noqa: F821
import os

LIB = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "lvgl", "src")  # noqa: F821
EDITS = [
    (os.path.join("core", "lv_refr.c"),
     "lv_refr_get_top_obj(draw_ctx->buf_area,", "lv_refr_get_top_obj(draw_ctx->clip_area,"),
    (os.path.join("widgets", "lv_img.c"),
     "const lv_area_t * clip_area = lv_event_get_param(e);", "const lv_area_t * clip_area = info->area;"),
    (os.path.join("widgets", "lv_img.c"),
     "        }\n    }\n    else if(code == LV_EVENT_DRAW_MAIN || code == LV_EVENT_DRAW_POST) {\n",
     "        }\n"
     "        /*AIplantRx (scripts/patch_lvgl.py): every image check passed, so the image covers*/\n"
     "        if(lv_obj_get_style_opa(obj, LV_PART_MAIN) < LV_OPA_MAX) {\n"
     "            info->res = LV_COVER_RES_NOT_COVER;\n"
     "            return;\n"
     "        }\n"
     "        info->res = LV_COVER_RES_COVER;\n"
     "    }\n    else if(code == LV_EVENT_DRAW_MAIN || code == LV_EVENT_DRAW_POST) {\n"),
    (os.path.join("core", "lv_obj_pos.c"),
     "                                 bool inv)\n{\n    lv_point_t p[4] = {\n",
     "                                 bool inv)\n{\n"
     "    /*AIplantRx (scripts/patch_lvgl.py): untransformed objects keep their exact area*/\n"
     "    const lv_obj_t * t = obj;\n"
     "    while(t) {\n"
     "        if(lv_obj_get_style_transform_angle(t, 0) != 0 || lv_obj_get_style_transform_zoom(t, 0) != LV_IMG_ZOOM_NONE) break;\n"
     "        t = recursive ? lv_obj_get_parent(t) : NULL;\n"
     "    }\n"
     "    if(t == NULL) return;\n"
     "\n"
     "    lv_point_t p[4] = {\n"),
]

for rel, old, new in EDITS:
    path = os.path.join(LIB, rel)
    if not os.path.isfile(path):
        print("[patch_lvgl] WARN: %s not fetched yet, skipped" % rel)
        continue
    src = open(path, encoding="utf-8", newline="").read().replace("\r\n", "\n")
    if old in src:
        open(path, "w", encoding="utf-8", newline="").write(src.replace(old, new))
        print("[patch_lvgl] patched %s" % rel)
    elif new not in src:
        raise SystemExit("[patch_lvgl] %s: neither form found, LVGL changed; review the fix" % rel)

