#ifndef _SHARED_H_
#define _SHARED_H_

/* THUMBYNES PATCH: was `unsigned long int` / `signed long int`. On
 * ESP32 / Cortex-M / any 32-bit ILP32 toolchain that's 4 bytes, but on
 * 64-bit Linux x86_64 (LP64) `long` is 8 bytes — and smsplus indexes
 * `uint32 *bp_lut` expecting a 4-byte stride, walking off the end of
 * its 256 KB buffer on the host build. Switch to fixed-width stdint
 * types so host and device agree on 4-byte uint32 / int32. */
#include <stdint.h>
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;

typedef int8_t  int8;
typedef int16_t int16;
typedef int32_t int32;

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <limits.h>
#include <math.h>

#ifdef RETRO_GO
#include <rg_system.h>
#define LOG_PRINTF(level, x...) rg_system_log(RG_LOG_PRINTF, NULL, x)
#define crc32_le(a, b, c) rg_crc32(a, b, c)
#else
#define LOG_PRINTF(level, x...) printf(x)
#define IRAM_ATTR
#define crc32_le(a, b, c) (0)
#endif

#define MESSAGE_ERROR(x, ...) LOG_PRINTF(1, "!! %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_WARN(x, ...)  LOG_PRINTF(2, "** %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_INFO(x, ...)  LOG_PRINTF(3, " * %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, ">> %s: " x, __func__, ## __VA_ARGS__)

// #include "config.h"
#include "cpu/z80.h"
#include "memz80.h"
#include "loadrom.h"
#include "pio.h"
#include "render.h"
#include "sms.h"
#include "state.h"
#include "system.h"
#include "tms.h"
#include "vdp.h"
#include "sound/sn76489.h"
#include "sound/emu2413.h"
#include "sound/ym2413.h"
#include "sound/fmintf.h"
#include "sound/sound.h"

#endif /* _SHARED_H_ */
