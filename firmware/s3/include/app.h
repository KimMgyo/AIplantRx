// Shared constants for the S3 firmware.
#pragma once
#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION "2.0.0-dev"
#endif
#define APP_HOST "aiplantrx-s3"      // mDNS name, espota target, DHCP hostname
#define APP_TZ "KST-9"               // Asia/Seoul
#define DISCOVERY_PORT 4210          // UDP: every board announces itself here (UI.md 7)

#define SCR_W 800
#define SCR_H 480

// PSRAM allocation (never for DMA here).
static inline void *ps_malloc_or_null(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
