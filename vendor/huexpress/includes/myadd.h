#pragma once

/* ThumbyNES patch:
 *   Upstream pulls in ESP-IDF's esp_system.h and the odroid debug
 *   perf macros unconditionally. Guarded those behind THUMBY_BUILD
 *   so the same file can be compiled against either the ESP-IDF
 *   environment (original ODROID-GO target) or the Thumby Color
 *   portable shim (thumby_platform.h). No other changes to this
 *   file — all the MY_* feature flags below are upstream as-is.
 *   See vendor/VENDORING.md for the patch list.
 */
#ifdef THUMBY_BUILD
#  include "thumby_platform.h"
#else
#  include "esp_system.h"
#  include "../../odroid/odroid_debug.h"
#endif

extern void *my_special_alloc(unsigned char speed, unsigned char bytes, unsigned long size);

#define MY_EXCLUDE
/* MY_GFX_AS_TASK / MY_SND_AS_TASK assume FreeRTOS; not defined on THUMBY_BUILD. */
#ifndef THUMBY_BUILD
#  define MY_GFX_AS_TASK
#  define MY_SND_AS_TASK
#endif

//#define ODROID_DEBUG_PERF_CPU_ALL_INSTR

//#define MY_VIDEO_MODE_SCANLINES

#define MY_INLINE_bank_set
#define MY_INLINE_GFX

// ------------------------------------------------- V1
/*
#define MY_VDC_VARS
#define MY_USE_FAST_RAM
*/
// -------------------------------------------------

// ------------------------------------------------- V2
/*
#define MY_INLINE_GFX_Loop6502
#define MY_INLINE_SPRITE 1
//#define MY_INLINE_SPRITE 2
#define MY_INLINE_SPRITE_sp2pixel
#define MY_INLINE_SPRITE_PutSpriteHflipMakeMask
#define MY_INLINE_SPRITE_PutSpriteHflipM
#define MY_INLINE_SPRITE_PutSpriteHflip
#define MY_INLINE_SPRITE_plane2pixel
#define MY_INLINE_SPRITE_CheckSprites
#define MY_INLINE_SPRITE_RefreshScreen

// .. #define MY_h6280_flnz_list
// .. #define MY_INLINE_IO_ReadWrite

//#define MY_h6280_INT_cycle_counter // Negativ
#define MY_VDC_VARS // Pos
#define MY_USE_FAST_RAM
*/
// -------------------------------------------------

// ------------------------------------------------- V3
#define MY_INLINE_GFX_Loop6502
#define MY_VDC_VARS // Pos
#define MY_USE_FAST_RAM
// -------------------------------------------------


#define USE_INSTR_SWITCH // ***
#define MY_INLINE_h6280_opcodes //***
#define MY_h6280_exe_go // ***
#define MY_INLINE // ***


//#define MY_h6280_ON_CPU0  // ;-)

//#define MY_REALLOC_MEMORY_SIDEARMS
//#define MY_PCENGINE_LOGGING
#define MY_LOG_CPU_NOT_INLINED // Slower without?!
//#define BENCHMARK
//#define MY_VSYNC_DISABLE
//#define MY_DEBUG_CHECKS


extern bool skipNextFrame;



#ifdef BENCHMARK
#ifndef MY_VSYNC_DISABLE
#define MY_VSYNC_DISABLE
#endif
#endif
