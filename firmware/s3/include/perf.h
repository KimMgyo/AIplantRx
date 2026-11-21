// Frame timing, for finding what makes the panel drop frames (GET /perf, serial "perf").
//
// One LVGL refresh = render (LVGL drawing into the off-screen frame buffer) + wait (for the panel to
// finish showing the previous buffer, up to one 26.8 ms period at 37.3 Hz). busy is the LVGL task's
// own CPU time: everything in lv_timer_handler except that wait.
#pragma once
#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif
void perf_render_done(void);     // lvgl_v8_port flush, last area, before the buffer switch
void perf_wait_done(void);       // ... after the wait
void perf_handler(uint32_t us);  // lvgl_port_task: one lv_timer_handler call
#ifdef __cplusplus
}
#endif

void perf_attach();              // under the LVGL lock
void perf_cam(uint32_t fetch_us, uint32_t decode_us, uint32_t hdr_us);
void perf_thermal(uint32_t us);
String perf_json(bool reset);
