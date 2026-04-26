/*
 * ThumbyNES platform shim for HuExpress.
 *
 * Replaces the ESP-IDF headers (esp_system.h + odroid_debug.h) that
 * the upstream core pulls in via myadd.h. Only active when THUMBY_BUILD
 * is defined. Host builds see the same header; the device build layers
 * extra attributes (IRAM_ATTR, .time_critical placement) through build
 * flags rather than here — mirror of smsplus/shared.h pattern.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/* ESP-IDF memory / placement attributes — become no-ops on host.
 * Device build can override IRAM_ATTR via -DIRAM_ATTR=__not_in_flash("...")
 * once we settle on hot-path placement (same pattern as nofrendo + smsplus). */
#ifndef DRAM_ATTR
#  define DRAM_ATTR
#endif
#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif
#ifndef WORD_ALIGNED_ATTR
#  define WORD_ALIGNED_ATTR
#endif
#ifndef IROM_ATTR
#  define IROM_ATTR
#endif
#ifndef EXT_RAM_ATTR
#  define EXT_RAM_ATTR
#endif

/* FreeRTOS handles HuExpress's `extern QueueHandle_t vidQueue` etc.
 * We never use the task-as-queue paths (MY_GFX_AS_TASK / MY_SND_AS_TASK
 * are disabled under THUMBY_BUILD in myadd.h), but the declarations are
 * still type-referenced so we stub the handle types as void*. */
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef void *xTaskHandle;
typedef void *SemaphoreHandle_t;

/* ODROID perf counters — no-op on Thumby. The BENCHMARK build will use
 * pico_sdk time_us_64() through our own wrapper, not these. Upstream's
 * ODROID_DEBUG_PERF_* macros appear in positions that don't permit a
 * trailing statement (`if(...) MACRO(x) else ...`, etc.), so stub them
 * as genuinely empty expansions rather than `do{}while(0)`. */
#define odroid_debug_perf_instr_begin(x)
#define odroid_debug_perf_instr_end(x)
#define ODROID_DEBUG_PERF_INSTR_READ_ROM  0
#define ODROID_DEBUG_PERF_INSTR_READ_RAM  0
#define ODROID_DEBUG_PERF_INSTR_WRITE_ROM 0
#define ODROID_DEBUG_PERF_INSTR_WRITE_RAM 0

#define ODROID_DEBUG_PERF_START2(counter)
#define ODROID_DEBUG_PERF_END2(counter)
#define ODROID_DEBUG_PERF_INCR2(counter, total)
#define ODROID_DEBUG_PERF_TOTAL                  0

/* Some PCE code paths call htons/ntohs on ROM header bytes. netinet/in.h
 * is unavailable on the device; provide LSB-based inline versions. */
static inline uint16_t _thumby_htons(uint16_t x) {
    return (uint16_t)(((x << 8) & 0xFF00) | ((x >> 8) & 0x00FF));
}
static inline uint32_t _thumby_htonl(uint32_t x) {
    return ((x << 24) & 0xFF000000UL) | ((x << 8) & 0x00FF0000UL) |
           ((x >> 8) & 0x0000FF00UL) | ((x >> 24) & 0x000000FFUL);
}
#ifndef htons
#define htons _thumby_htons
#endif
#ifndef ntohs
#define ntohs _thumby_htons
#endif
#ifndef htonl
#define htonl _thumby_htonl
#endif
#ifndef ntohl
#define ntohl _thumby_htonl
#endif
