/*
 * ThumbyNES platform glue for HuExpress.
 *
 * Supplies the handful of symbols the vendored core links against but
 * whose implementations lived in the SDL / Haiku / ODROID-GO frontends
 * we don't vendor. Pattern mirrors vendor/picodrive/thumby_platform.c.
 *
 *  - osd_gfx_buffer   — the 220 KB XBUF render target. Allocated here
 *                       on host (malloc'd lazily on first init). Device
 *                       build will eventually switch to the scanline
 *                       renderer and never allocate this (see
 *                       PCE_PLAN.md §5 and task #12).
 *  - osd_gfx_*        — driver-registration + message helpers. No-op.
 *  - osd_keyboard     — upstream polled SDL keys; we drive input via
 *                       io.JOY[] directly from pcec_set_buttons, so
 *                       this is a no-op.
 *  - handle_bp*       — breakpoint handlers left behind in h6280.c's
 *                       per-opcode dispatch table. We stripped bp.c
 *                       so the tables reference missing bodies;
 *                       satisfy the linker with no-ops.
 *  - pce_cd_handle_read_1800 — CD IO port reader. Returns 0xFF (idle).
 *  - Log / wait_next_vsync / patch_rom — misc helpers.
 *  - TAB_CONST        — CRC32 table. The upstream idiom defines it by
 *                       including crc_ctl.h in a single translation
 *                       unit (hucrc.c originally). Since we removed
 *                       hucrc.c, do the include here instead.
 */
#define THUMBY_BUILD 1

#include "cleantypes.h"
#include "pce.h"
#include <stdio.h>
#include <stdarg.h>

/* CD_track definition stays in pce.c (Phase 1). */

/* Provide TAB_CONST by including the upstream header exactly once.
 * crc_ctl.h defines `unsigned long TAB_CONST[256] = { ... };` at file
 * scope. Any other TU that references TAB_CONST must see only the
 * extern declaration in utils.h. */
#include "crc_ctl.h"

#include "sys_gfx.h"
#include "sys_snd.h"
#include "sys_inp.h"
#include "sys_cd.h"

/* Video driver registration — upstream tracks multiple back-ends
 * (SDL, Haiku, Allegro). We register one stub driver so indexed
 * accesses (draw / init / mode / shut) are safe even though the
 * runtime never actually calls them from the HuCard path. */
static int stub_drv_init(void) { return 0; }
static int stub_drv_mode(void) { return 0; }
static void stub_drv_draw(void) {}
static void stub_drv_shut(void) {}
osd_gfx_driver osd_gfx_driver_list[] = {
    { stub_drv_init, stub_drv_mode, stub_drv_draw, stub_drv_shut },
};

/* The rendering target XBUF. pce_core.c's pcec_init allocates this
 * on host (~220 KB); device build with PCE_SCANLINE_RENDER will bind
 * it to a tiny line scratch instead. */
uchar *osd_gfx_buffer = NULL;

void osd_gfx_set_color(uchar idx, uchar r, uchar g, uchar b) {
    (void)idx; (void)r; (void)g; (void)b;
    /* We build our own RGB565 LUT in pce_core.c's rebuild_palette(). */
}

uint16 osd_gfx_savepict(void) { return 0; }
void osd_gfx_set_message(char *msg) { (void)msg; }

/* Upstream sys_inp polls the keyboard via osd_keyboard() each frame.
 * We drive input through io.JOY[0] directly from pcec_set_buttons,
 * so this is a no-op returning "no key pressed". */
/* Pool-tagged for the same reason as wait_next_vsync above. */
IRAM_ATTR int osd_keyboard(void) { return 0; }

/* Breakpoint table entries — bp.c was stripped. Provide weak no-ops.
 * h6280's opcode table has one entry per 256 opcodes referencing a
 * handle_bpN for each top-nibble. 16 slots. */
#define BP_STUB(n) void handle_bp##n(void) {}
BP_STUB(0)  BP_STUB(1)  BP_STUB(2)  BP_STUB(3)
BP_STUB(4)  BP_STUB(5)  BP_STUB(6)  BP_STUB(7)
BP_STUB(8)  BP_STUB(9)  BP_STUB(10) BP_STUB(11)
BP_STUB(12) BP_STUB(13) BP_STUB(14) BP_STUB(15)
#undef BP_STUB

/* CD sector reader — HuCard cart loads bypass this, but pce.c
 * holds a function-pointer table that references it. */
void osd_cd_read(uchar *buf, uint32 lba) { (void)buf; (void)lba; }
void osd_cd_close(void) {}

/* Freeze-state diagnostics from the upstream debugger. Stripped.
 * cheat.h declares these with specific types (freezed_value[] and
 * uchar); provide dummy-sized backing so the linker resolves the
 * extern. Actual size unimportant since the cheat code path is dead. */
#include "cheat.h"
freezed_value list_to_freeze[MAX_FREEZED_VALUE];
uchar current_freezed_values;

/* Disassembler + breakpoint tables — dis.c and bp.c stripped.
 * debug.h declares: uchar Op6502(unsigned int), extern Breakpoint Bp_list[].
 *
 * Op6502 is NOT disassembler-only: pce.c's ResetPCE uses it as its
 * memory-read primitive to fetch the reset vector at 0xFFFE/0xFFFF.
 * Stubbing it to 0 would mean reg_pc=0 after reset and the CPU jumps
 * into unmapped space. Implement it the same way upstream debug.c
 * does — one line indirection through PageR. */
#include "debug.h"
extern uchar **PageR;
uchar Op6502(unsigned int A) { return PageR[(A >> 13) & 7][A]; }
Breakpoint Bp_list[MAX_BP];

/* zipmgr stubs — pce.c's ROM loader has a ZIP branch we never trigger. */
int zipmgr_probe_file(const char *path) { (void)path; return 0; }
int zipmgr_extract_to_disk(const char *src, const char *dst) {
    (void)src; (void)dst; return -1;
}
int zipmgr_extract_to_memory(const char *src, uchar **buf, int *len) {
    (void)src; (void)buf; (void)len; return -1;
}

/* CD stubs. pce.h declares pce_cd_sectoraddy as a uint32 variable,
 * not a function. sys_snd.h declares osd_snd_set_volume(uchar). */
uint32 pce_cd_sectoraddy;
void osd_snd_set_volume(uchar vol) { (void)vol; }
void read_sector_HCD(uchar *buf, unsigned long lba) { (void)buf; (void)lba; }
void pce_cd_handle_write_1800(uint16 A, uchar V) { (void)A; (void)V; }

/* hcd.h: int fill_HCD_info(char *); void HCD_shutdown(); */
int fill_HCD_info(char *name) { (void)name; return 0; }
void HCD_shutdown(void) {}

/* POSIX stubs referenced by pce.c::TrashPCE (which our wrapper never
 * actually calls). Pico SDK libc has no rmdir / system so we satisfy
 * the linker with no-ops. */
int rmdir(const char *path) { (void)path; return 0; }

/* pce.h declares `extern char *log_filename` (pointer, not array). */
char *log_filename = NULL;

/* Per-frame control flag set by gfx_Loop6502.h and consumed by
 * h6280_exe_go.h. Volatile because interrupts (on device) might
 * touch it; signed int for atomicity. */
volatile int g_pce_frame_done = 0;


/* hard_init is the actual HuExpress init entry (in hard_pce.c).
 * Alias it as hardware_init so pce_core.c's wrapper can call the
 * upstream name without a rename. */
extern void hard_init(void);
int hardware_init(void) {
    hard_init();
    return 0;
}

/* CD port 0x1800 read — returns idle/no-media signature. */
uchar pce_cd_handle_read_1800(int port) { (void)port; return 0xFF; }

/* Upstream Log() is a variadic status/debug print. FINAL_RELEASE=1
 * kills most callers, but a handful remain (ROM load diagnostics,
 * CD errors). Route to printf — the device will #define this to a
 * UART logger later. */
void Log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* Frame pacing is handled by the runner — this is a no-op. */
/* exe_go BLs into wait_next_vsync from inside the .pce_iram_pool, so
 * the stub has to live in the pool too — otherwise the BL's PC-rel
 * offset breaks once exe_go is memcpy'd onto the heap. */
IRAM_ATTR void wait_next_vsync(void) {}

/* Runtime ROM patch helper — we don't ship patch files. */
void patch_rom(char *filename, int offset, uchar value) {
    (void)filename; (void)offset; (void)value;
}
