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

/* ------ zlib crc32 --------------------------------------------------- *
 * PicoDrive's cart.c calls crc32() from rom_crc32() when carthw.cfg has
 * sections that check_crc32 without a preceding check_str — those run
 * unconditionally for every loaded ROM. Previous host-target builds
 * link zlib; the device build stubbed crc32 to address 0 on the
 * assumption it was never called, which hard-faulted on the first
 * MD ROM load. 30-line polynomial implementation — no 1 KB CRC table.
 * Signature matches zlib's unsigned-long interface. */
unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len)
{
    crc = ~crc;
    for (unsigned int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
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
void mp3_update     (s32 *buf, int length, int stereo) { (void)buf; (void)length; (void)stereo; }

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

/* ------ Mega-CD link-level stubs ------------------------------------- */
/* Same pattern as 32X: all call sites are PAHW_MCD-guarded at runtime,
 * but the CD sources themselves carry ~20 KB of dead BSS (cdd 4.8K,
 * cdda_out_buffer 4.6K, PicoCpuFS68k 2.2K, Pico_msd 2.1K, s68k_*_map
 * x3 = 6K) that would be resident even when other emulator cores are
 * active. Excluding pico/cd/*.c from the build kills all of that BSS;
 * these stubs stand in for the handful of functions cart.c / pico.c /
 * media.c / state.c call on CD-inactive paths or for completeness.
 * None of these execute — PAHW_MCD is never set in our build.         */
#define PCD_EVENT_COUNT 4
unsigned int pcd_event_times[PCD_EVENT_COUNT];

void PicoInitMCD    (void)                              {}
void PicoExitMCD    (void)                              {}
void PicoPowerMCD   (void)                              {}
int  PicoResetMCD   (void)                              { return 0; }
void PicoFrameMCD   (void)                              {}
void PicoMCDPrepare (void)                              {}
void PicoMemSetupCD (void)                              {}
int  cdd_load          (const char *f, int t)           { (void)f; (void)t; return -1; }
int  cdd_unload        (void)                           { return 0; }
int  pcd_sync_s68k     (unsigned int m, int p)          { (void)m; (void)p; return 0; }
void pcd_state_loaded  (void)                           {}
int  cdd_context_save  (unsigned char *s)               { (void)s; return 0; }
int  cdd_context_load_old(unsigned char *s)             { (void)s; return 0; }
int  cdc_context_save  (unsigned char *s)               { (void)s; return 0; }
int  gfx_context_save  (unsigned char *s)               { (void)s; return 0; }
void wram_1M_to_2M     (unsigned char *w)               { (void)w; }
void wram_2M_to_1M     (unsigned char *w)               { (void)w; }
void DmaSlowCell       (u32 src, u32 a, int l, unsigned char i) { (void)src; (void)a; (void)l; (void)i; }
void pcd_pcm_update    (s32 *b, int l, int st)          { (void)b; (void)l; (void)st; }
u32  pcd_base_address;
int  cdd_context_load    (unsigned char *s)             { (void)s; return 0; }
int  cdc_context_load    (unsigned char *s)             { (void)s; return 0; }
int  cdc_context_load_old(unsigned char *s)             { (void)s; return 0; }
int  gfx_context_load    (unsigned char *s)             { (void)s; return 0; }
/* s68k memory maps (sub-CPU bus mapping table). Sized minimally — no
 * code ever indexes them since PAHW_MCD never fires. */
char s68k_read8_map[1], s68k_read16_map[1];
char s68k_write8_map[1], s68k_write16_map[1];
unsigned int SekCycleCntS68k;
unsigned int SekCycleAimS68k;

/* ThumbyNES: stub globals for symbols the MCD cd/*.c sources used to
 * define. Compiled code in non-CD files takes their address under
 * `if (is_sub)` / `if (PicoIn.AHW & PAHW_MCD)` branches that never
 * execute — the linker just needs a symbol by name. Sizing each as
 * a 1-byte array drops 2.2 KB (PicoCpuFS68k) + 2.1 KB (Pico_msd) +
 * 4.6 KB (cdda_out_buffer) of otherwise-resident BSS. Declared
 * without the real type so the size mismatch stays invisible to
 * anything that would warn. */
char PicoCpuFS68k[1];
char Pico_msd[1];
void *Pico_mcd;
/* cdda_out_buffer is patched to a pointer in sound.c (see VENDORING.md),
 * so no BSS cost here either — stays NULL in MD-only build. */

#ifdef THUMBY_YM2413_EXCLUDED
/* ------ YM2413 link-level stubs ------------------------------------- */
/* Mega-Drive cartridges never enable the YM2413 (it's the Japanese SMS
 * FM add-on, POPT_EN_YM2413). Excluded from device build (see device/
 * CMakeLists.txt) — this saves 128 KB of tll_table BSS plus the 6 KB
 * default_patch + emu2413 internal state. sound.c and sms.c still
 * call the OPLL_* and YM2413_* entry points; these stubs resolve
 * link-time. The code paths themselves are runtime-guarded on
 * PicoIn.opt & POPT_EN_YM2413 which never fires. Host builds keep
 * the real ym2413.c in the link so these stubs stay out of the way. */
void *OPLL_new(unsigned int clk, unsigned int rate) { (void)clk; (void)rate; return 0; }
void  OPLL_delete(void *o)                         { (void)o; }
void  OPLL_setChipType(void *o, int type)          { (void)o; (void)type; }
void  OPLL_reset(void *o)                          { (void)o; }
void  OPLL_setRate(void *o, int rate)              { (void)o; (void)rate; }
int16_t OPLL_calc(void *o)                         { (void)o; return 0; }
void  YM2413_regWrite(unsigned reg)                { (void)reg; }
void  YM2413_dataWrite(unsigned data)              { (void)data; }
void  PsndDoYM2413(int cyc_to)                     { (void)cyc_to; }
void  ym2413_unpack_state(const void *buf, size_t size) { (void)buf; (void)size; }
size_t ym2413_pack_state(void *buf, size_t size)        { (void)buf; (void)size; return 0; }
void  YMFM_setup_FIR(int sample_rate, int output_rate, int use_tone) {
    (void)sample_rate; (void)output_rate; (void)use_tone;
}
/* Global state pointer that sound.c tests and passes into OPLL_*
 * stubs. Stays NULL — MD never initializes it. */
void *opll;
#endif /* THUMBY_YM2413_EXCLUDED */

#ifdef THUMBY_DRAW2_EXCLUDED
/* ------ draw2.c link-level stubs (ALT_RENDERER not used) ------------ */
/* draw2.c is the alternate fast renderer (POPT_ALT_RENDERER). The
 * Thumby runner always uses the per-line renderer in draw.c. Excluded
 * from device build — saves ~103 KB BSS (PicoDraw2FB_ 82K + sprite
 * caches). Stubs for the three externally-visible entry points; their
 * callers are guarded on POPT_ALT_RENDERER. */
void PicoDraw2SetOutBuf(void *dest, int incr)      { (void)dest; (void)incr; }
void PicoDraw2Init(void)                           {}
void PicoFrameFull(void)                           {}
#endif /* THUMBY_DRAW2_EXCLUDED */

/* ThumbyNES debug counters for FinalizeLine555 diagnostics. */
volatile unsigned int md_dbg_finalize_calls;
volatile unsigned int md_dbg_finalize_early;
volatile unsigned int md_dbg_vdp_writes;

/* Odd-branch-target capture — first 4 distinct events. Site PC is the
 * calling instruction's PC (GET_PC at exception site); target is the
 * odd value the branch wanted to jump to. */
volatile unsigned int md_dbg_odd_count;
volatile unsigned int md_dbg_odd_site[4];
volatile unsigned int md_dbg_odd_target[4];

volatile unsigned int   md_dbg_cram_writes;
volatile unsigned short md_dbg_cram_val[4];
volatile unsigned char  md_dbg_cram_addr[4];
volatile unsigned int   md_dbg_cram_pc[4];   /* 0x100|pc = direct, 0x200|(src_is_rom<<4) = DMA */

volatile unsigned int   md_dbg_rom_reads;
volatile unsigned int   md_dbg_rom_addr[4];
volatile unsigned short md_dbg_rom_val[4];

volatile unsigned short md_dbg_rom_peek_18e;     /* header checksum */
volatile unsigned short md_dbg_rom_peek_1a4_hi;  /* ROM end high word */
volatile unsigned short md_dbg_rom_peek_1a6_lo;  /* ROM end low word */
volatile unsigned int   md_dbg_rom_maxaddr;      /* highest addr ever read */
volatile unsigned short md_dbg_rom_xor;          /* 16-bit SUM of reads in 0x200..0xFFFFF */
volatile unsigned short md_dbg_rom_qsum[16];     /* per-64KB-chunk sums */
volatile unsigned short md_dbg_ch15_f0000;       /* read at ROM[0xF0000] — expect 0x0280 */
volatile unsigned short md_dbg_ch15_f8000;       /* read at ROM[0xF8000] — expect 0xF684 */
volatile unsigned short md_dbg_ch15_fc000;       /* read at ROM[0xFC000] — expect 0xB020 */
volatile unsigned short md_dbg_ch15_ffffe;       /* read at ROM[0xFFFFE] — expect 0x0000 */

volatile unsigned int md_dbg_io_poll_count;     /* total IO reads (MD $A10000+) */
volatile unsigned int md_dbg_io_poll_addr;      /* last address read twice in a row */
volatile unsigned int md_dbg_io_last_addr;      /* previous IO read addr */

volatile unsigned int   md_dbg_vdp_reads;       /* total VDP reads ($C00000+) */
volatile unsigned short md_dbg_vdp_last_val;    /* last value PicoVideoRead returned */
volatile unsigned int   md_dbg_int_count[8];    /* per-IPL interrupt delivery count */

/* PC trap — set md_dbg_trap_pc to a 68K address before load; the
 * emulator captures D1 the first time PC reaches that address. */
volatile unsigned int md_dbg_trap_pc  = 0x3C2;   /* Sonic 2 error-handler entry */
volatile unsigned int md_dbg_trap_d1;
volatile unsigned int md_dbg_trap_hit;
void md_dbg_log_odd_branch(unsigned int site_pc, unsigned int target)
{
    unsigned int n = md_dbg_odd_count;
    if (n < 4) {
        md_dbg_odd_site[n]   = site_pc;
        md_dbg_odd_target[n] = target;
    }
    md_dbg_odd_count = n + 1;
}

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
