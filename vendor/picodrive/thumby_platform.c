/*
 * ThumbyNES — platform stubs for the vendored PicoDrive core.
 *
 * PicoDrive declares a handful of `plat_*`, `mp3_*`, `cache_flush_*`,
 * `emu_*` functions as `extern` and expects the frontend to provide
 * implementations. For the host bench / SDL2 build we back them with
 * libc; on device we override with flash/SRAM aware versions. None of
 * the stubbed functionality (SegaCD MP3 playback, DRC code caches,
 * 32X BIOS blobs) is reachable when we never set PAHW_MCD / PAHW_32X.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/pico_types.h"
#include "pico/pico.h"

/* ------ logging ------------------------------------------------------ */
void lprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------ memory allocation -------------------------------------------- */
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
    (void)addr; (void)need_exec; (void)is_fixed;
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{
    (void)oldsize;
    return realloc(ptr, newsize);
}

void plat_munmap(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}

/* DRC (dynamic recompilation) — we build with the interpreter FAME core
 * so no code cache is ever requested. Returning NULL / -1 is safe. */
void *plat_mem_get_for_drc(size_t size) { (void)size; return NULL; }
int   plat_mem_set_exec   (void *ptr, size_t size) { (void)ptr; (void)size; return -1; }

/* SH2 DRC cache flush — unused since we define NO_32X. */
void cache_flush_d_inval_i(void *start, void *end) { (void)start; (void)end; }

/* ------ Sega CD MP3 playback (CDDA) ---------------------------------- */
/* Stubs. We never insert a CD image; PAHW_MCD stays clear, these are
 * unreachable at runtime. Keep non-zero return where a live integration
 * would produce a plausible value, just so nothing divides by zero if
 * the code path is ever reached by mistake. */
int  mp3_get_bitrate(void *f, int size) { (void)f; (void)size; return 128000; }
void mp3_start_play (void *f, int pos)  { (void)f; (void)pos; }
void mp3_update     (int *buf, int length, int stereo) { (void)buf; (void)length; (void)stereo; }

/* ------ 32X hooks ---------------------------------------------------- */
/* NO_32X is set so these are only referenced via the extern decl in
 * pico.h. Never called. Stubbed for link-time. */
void emu_32x_startup(void) {}
void *p32x_bios_g = NULL, *p32x_bios_m = NULL, *p32x_bios_s = NULL;

/* ------ 32X link-level stubs ----------------------------------------- */
/* draw.c / memory.c / pico.c / state.c call these unconditionally from
 * PAHW_32X-guarded runtime branches; their bodies live in the 32x/*.c
 * sources we don't compile. PAHW_32X never gets set without a 32X ROM +
 * BIOS so nothing reaches these stubs at run time. */
#include "pico/pico_types.h"
typedef struct { int _unused; } SH2;  /* opaque fallback if not included */
void PicoDrawSetOutFormat32x(int which, int use_32x_line_mode) { (void)which; (void)use_32x_line_mode; }
void PicoDrawSetOutBuf32X   (void *dest, int increment)        { (void)dest; (void)increment; }
u32  PicoRead8_32x (u32 a)           { (void)a; return 0; }
u32  PicoRead16_32x(u32 a)           { (void)a; return 0; }
void PicoWrite8_32x (u32 a, u32 d)   { (void)a; (void)d; }
void PicoWrite16_32x(u32 a, u32 d)   { (void)a; (void)d; }
void Pico32xPrepare (void)           {}
void Pico32xStartup (void)           {}
void Pico32xShutdown(void)           {}

/* ------ Video mode change callback ----------------------------------- */
/* PicoDrive calls this whenever the VDP switches between H32/H40 or
 * between 28-row and 30-row modes. The device/host frontend uses it to
 * reconfigure scaling. For the bench this is a no-op. */
__attribute__((weak))
void emu_video_mode_change(int start_line, int line_count,
                           int start_col,  int col_count)
{
    (void)start_line; (void)line_count; (void)start_col; (void)col_count;
}
