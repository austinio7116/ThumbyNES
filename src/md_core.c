/*
 * ThumbyNES — PicoDrive glue.
 *
 * Mirror of sms_core.c but for the vendored Sega Mega Drive / Genesis
 * core. Six public operations (init/load/run/buttons/audio/state) map
 * onto PicoDrive's PicoInit / PicoFrame / PicoIn surface.
 */
#include "md_core.h"

#include <stdlib.h>
#include <string.h>

/* pico/pico.h leaks `s16` / `s32` without declaring them — include
 * pico_types.h first to provide the short-name typedefs. */
#include "pico/pico_types.h"
#include "pico/pico.h"
#include "pico/pico_int.h"
#include "pico/state.h"
/* fm68k_shutdown() for device heap release. Prototype inline to avoid
 * pulling in the full FAME header (which wants `cpu/fame/fame.h` on
 * picodrive's private include path). */
void fm68k_shutdown(void);

#ifdef THUMBY_STATE_BRIDGE
/* Device build: PicoDrive's state.c opens files via libc fopen which
 * doesn't exist on bare metal. Route file I/O through the same
 * FatFs-backed shim nofrendo/smsplus/peanut_gb all use for their
 * save states. Host build keeps the libc stdio path inside PicoState. */
#  include "thumby_state_bridge.h"
#endif

#ifdef MD_IRAM_DYNAMIC
/* Dynamic IRAM infra in device/md_iram.c — memcpies hot MD functions
 * from a flash pool into a heap buffer so they execute from SRAM
 * without permanent BSS cost across cores. No-op on host. */
extern int  md_iram_init(void);
extern void md_iram_shutdown(void);
#endif

#ifdef MD_LINE_SCRATCH
/* Device build: line-scratch mode. s_fb is a single-line buffer
 * (MDC_MAX_W shorts = 640 bytes). Each scanline PicoDrive draws
 * goes into it; our PicoScanEnd callback downsamples into the
 * caller-provided 128x128 LCD fb. Saves 153 KB of heap vs the
 * full-frame path. */
static uint16_t   s_line_scratch[MDC_MAX_W];
static uint16_t  *s_fb = s_line_scratch;
static uint16_t  *s_lcd_fb;               /* set by mdc_set_scale_target */
static uint8_t    s_scale_mode;
static uint8_t    s_blend;                /* 0 = nearest (fast), 1 = 2x2 blend */
static int16_t    s_vx, s_vy, s_vw, s_vh;
static int16_t    s_pan_x, s_pan_y;
/* Previous-dst-row tracking for vertical blending. When two+ src
 * lines map to the same dy, the 2nd+ are averaged into whatever's
 * already in the dst row. */
static int16_t    s_prev_dy_fit;
static int16_t    s_prev_dy_fill;
/* Per-dx source-column LUT. Replaces the `(dx * s_vw) / 128`
 * integer div that happened 128 (blend: 256) times per scanline
 * × ~224 scanlines = ~57 k divs/frame. uint16_t: H40 mode goes up
 * to sx=317, won't fit in uint8_t. Rebuilt when s_vw, s_vh, or
 * s_scale_mode changes (FIT and FILL sample X differently). */
static uint16_t   s_sx_lut[128];
static uint16_t   s_sx2_lut[128];
static int16_t    s_lut_vw;
static int16_t    s_lut_vh;
static uint8_t    s_lut_scale;
#else
/* Full 320x240 RGB565 frame. PicoDrive writes one scanline at a time
 * through DrawLineDest; we hand it a contiguous buffer and a row
 * increment (in bytes) so each line lands at the right Y offset. */
static uint16_t *s_fb;              /* MDC_MAX_W * MDC_MAX_H shorts */
#endif
/* Diagnostic flags written by md_core_scan_end, polled by md_run.c
 * overlay. md_core_scan_fired raises on first callback invocation per
 * frame; md_core_line_or ORs pixel values from a few source columns
 * across all scanlines. If line_or stays 0 across a frame, FinalizeLine
 * never put real data into s_line_scratch. */
volatile uint8_t  md_core_scan_fired;
volatile uint16_t md_core_line_or;

/* Debug getters for md_run.c overlay — pull 68K state + VDP state
 * from PicoDrive internals without leaking the full include chain
 * into device code. */
unsigned short *mdbg_get_cram(void)     { return PicoMem.cram; }
unsigned char  *mdbg_get_vdp_regs(void) { return Pico.video.reg; }
unsigned int    mdbg_get_pc(void)       { return PicoCpuFM68k.pc; }
unsigned int    mdbg_get_a7(void)       { return PicoCpuFM68k.areg[7].D; }
unsigned int    mdbg_get_sr(void)       { return PicoCpuFM68k.sr; }
unsigned int    mdbg_get_execinfo(void) { return PicoCpuFM68k.execinfo; }
unsigned int    mdbg_get_d0(void)       { return PicoCpuFM68k.dreg[0].D; }
unsigned int    mdbg_get_d1(void)       { return PicoCpuFM68k.dreg[1].D; }
unsigned int    mdbg_get_a0(void)       { return PicoCpuFM68k.areg[0].D; }
unsigned int    mdbg_get_a1(void)       { return PicoCpuFM68k.areg[1].D; }
unsigned int    mdbg_get_rom_ptr(void)  { return (unsigned int)(uintptr_t)Pico.rom; }
unsigned int    mdbg_get_rom_size(void) { return (unsigned int)Pico.romsize; }

/* Read a 16-bit MD u16 (BE-in-flash) at ROM offset `a` via the
 * UNCACHED XIP alias (RP2350: XIP_BASE=0x10000000, uncached alias
 * at 0x14000000). Bypasses any stale cache line. Returns bswapped
 * value. RP2040's uncached alias was 0x13000000 — different here. */
unsigned short mdbg_read_uncached(unsigned int a) {
    if (!Pico.rom) return 0xDEAD;
    uintptr_t cached   = (uintptr_t)Pico.rom;
    uintptr_t uncached = (cached & 0x00FFFFFFu) | 0x14000000u;
    unsigned short raw = *(volatile unsigned short *)(uncached + a);
    return (unsigned short)((raw << 8) | (raw >> 8));
}

/* Live snapshot of QMI ATRANS slots so we can verify what's actually
 * mapped where at runtime. */
unsigned int mdbg_get_atrans(unsigned int slot) {
    volatile unsigned int *qmi_atrans = (volatile unsigned int *)(0x400D0000 + 0x34);
    if (slot > 3) return 0;
    return qmi_atrans[slot];
}

unsigned int mdbg_get_z80_state(void) {
    /* Pack: bit 0 = z80Run, bit 1 = z80_reset, bits 8..15 = reg[1] */
    return (Pico.m.z80Run & 1)
         | ((Pico.m.z80_reset & 1) << 1)
         | ((Pico.video.reg[1] & 0xFF) << 8);
}

/* Bisect helper: read 1 MB ROM at offset `a` via the XIP pointer. */
unsigned short mdbg_read_rom_u16(unsigned int a) {
    if (!Pico.rom || a >= Pico.romsize) return 0xDEAD;
    unsigned short raw = *(volatile unsigned short *)(Pico.rom + a);
    return (unsigned short)((raw << 8) | (raw >> 8));
}
extern volatile unsigned int md_dbg_vdp_writes;
unsigned int    mdbg_get_vdp_writes(void){ return md_dbg_vdp_writes; }

unsigned int    mdbg_get_vdp_status_word(void) {
    return (unsigned int)Pico.video.status;
}

static int16_t   s_sndbuf[2048];    /* stereo int16, ~735 samples/frame @ 22050/60 */
static int16_t   s_mixbuf[1024];    /* mono int16 after downmix */
static int       s_mixcount;
static int       s_sample_rate;
static bool      s_loaded;
/* We malloc and own the ROM buffer handed to PicoCartInsert. */
static uint8_t  *s_rom_copy;

/* Pre-allocated scratch buffer for state.c's malloc calls in
 * state_save (CHUNK_LIMIT_W = 18772) and state_load (CHUNK_LIMIT_R
 * = 0x10960 = 68448). Sized for the larger of the two so one
 * buffer covers both paths. Exported so state.c's patched malloc
 * can find it. Alloc happens in mdc_init, while the heap is still
 * pristine; the runtime heap state mid-cart is what deadlocked
 * newlib's malloc in the save path. */
#define MDC_STATE_SCRATCH_SIZE 0x10960
uint8_t *mdc_state_scratch = NULL;

/* PicoDrive calls writeSound at end-of-frame with length-in-bytes,
 * IMMEDIATELY before PsndClear wipes the buffer. PicoIn.opt no
 * longer sets POPT_EN_STEREO so samples arrive as mono int16 —
 * capture is a plain copy. */
static void capture_audio(int len_bytes)
{
    int n = len_bytes / 2;                  /* bytes → mono samples */
    int cap = (int)(sizeof(s_mixbuf) / sizeof(s_mixbuf[0]));
    if (n > cap) n = cap;
    memcpy(s_mixbuf, s_sndbuf, n * sizeof(int16_t));
    s_mixcount = n;
}

int mdc_init(int region, int sample_rate)
{
    s_loaded   = false;
    s_mixcount = 0;
    s_sample_rate = sample_rate;

#ifdef MD_SP_GUARD
    {
        const char *trap = getenv("MD_TRAP_PC");
        extern volatile unsigned int md_dbg_trap_pc_dump;
        if (trap) {
            md_dbg_trap_pc_dump = (unsigned int)strtoul(trap, 0, 16);
        }
        fprintf(stderr, "MD_TRAP_PC env=%s -> md_dbg_trap_pc_dump=0x%x\n",
                trap ? trap : "(unset)", md_dbg_trap_pc_dump);
    }
#endif

#ifndef MD_LINE_SCRATCH
    if (!s_fb) {
        s_fb = (uint16_t *)calloc(MDC_MAX_W * MDC_MAX_H, sizeof(uint16_t));
        if (!s_fb) return -1;
    }
#endif

    /* Pre-allocate the state-save scratch buffer at init time while
     * the heap is still pristine. state.c's state_save does
     *   buf2 = malloc(CHUNK_LIMIT_W)  // 18772 bytes
     * as its first heap op, which under our tight-SRAM MD runtime
     * deadlocked in newlib's malloc after FAME / PicoDrive had
     * chewed through the heap mid-cart. Our vendor patch in
     * state.c (see VENDORING.md) reuses mdc_state_scratch when
     * it's non-NULL, avoiding any runtime alloc during save/load. */
    if (!mdc_state_scratch) {
        mdc_state_scratch = (uint8_t *)malloc(MDC_STATE_SCRATCH_SIZE);
        /* Non-fatal if this fails — state_save/load will fall back
         * to malloc() if mdc_state_scratch stays NULL. */
    }

    PicoInit();

    /* sample_rate == 0 is the runtime "silent mode" — the menu
     * AUDIO=OFF picks it. We strip Z80 + FM + PSG from PicoIn.opt
     * so none of those code paths execute. Locks 50 PAL / 60 NTSC
     * with zero audio cost.
     *
     * Dropped POPT_EN_STEREO: Thumby has a single mono speaker, so
     * L/R mixing is wasted — PicoDrive renders mono natively and
     * capture_audio is a plain memcpy.
     *
     * ACC_SPRITES kept on: the fast-sprites path visibly changes
     * priority handling on some carts; the measured savings were
     * negligible. */
    const bool silent = (sample_rate == 0);
    PicoIn.opt = POPT_ACC_SPRITES
               | (silent ? 0 : POPT_EN_PSG | POPT_EN_FM | POPT_EN_Z80);
    /* PsndRerate uses PicoIn.sndRate to size its resampler tables.
     * Keep it at a sane value even in silent mode — the POPT flags
     * above prevent Z80/FM/PSG synthesis from actually running. */
    if (silent) sample_rate = 22050;
    PicoIn.sndRate        = sample_rate;
    PicoIn.sndOut         = s_sndbuf;
    PicoIn.writeSound     = capture_audio;
    PicoIn.regionOverride = (unsigned short)region;
    PicoIn.autoRgnOrder   = 0x148;   /* prefer EU, then US, then JP */

#ifdef MD_IRAM_DYNAMIC
    /* Copy hot Cz80_Exec bytes from flash pool into heap-allocated
     * SRAM, redirect __wrap_Cz80_Exec to point there. Fails open
     * (stays in flash) if malloc fails. */
    md_iram_init();
#endif

    return 0;
}

/* Deinterleave a .smd-headered ROM if the caller didn't strip the
 * 512-byte magic. Returns the stripped size. */
static size_t strip_smd_header(const uint8_t *in, size_t len)
{
    /* 512-byte copier header is present iff (len % 16384) == 512. */
    if (len > 512 && (len & 0x3FFF) == 512) return 512;
    return 0;
}

/* s_owns_rom tracks whether the ROM buffer handed to PicoCartInsert
 * was malloc'd by us (and therefore needs plat_munmap on shutdown)
 * vs borrowed from the caller (XIP flash on device — must NOT free). */
static bool s_owns_rom;

/* Shared tail of the load process — wires up PicoDrive once Pico.rom
 * is set correctly. */
static int mdc_finish_load(void)
{
    /* Matches the libretro frontend sequence: LoopPrepare rebuilds
     * region-dependent timing tables; PsndRerate rebuilds the PCM
     * sample-per-frame tables at our audio rate. */
    PicoLoopPrepare();
    PsndRerate(0);

    /* Output format + buffer must be wired AFTER cart insert — the
     * PicoPower path inside PicoCartInsert memsets Pico.est.rendstatus,
     * which clears the PDRAW_SYNC_NEEDED flag that PicoDrawSetOutBuf
     * sets. Without that flag the first-frame draw short-circuits in
     * PicoDrawSync (line 1887) and our framebuffer stays blank. */
    PicoDrawSetOutFormat(PDF_RGB555, 0);
#ifdef MD_LINE_SCRATCH
    /* increment = 0 → every scanline overwrites the same 640-byte
     * scratch; PicoScanEnd consumes it per-line. */
    PicoDrawSetOutBuf(s_fb, 0);
    {
        extern int md_core_scan_end(unsigned int line);
        PicoDrawSetCallbacks(NULL, md_core_scan_end);
    }
#else
    PicoDrawSetOutBuf(s_fb, MDC_MAX_W * sizeof(uint16_t));
#endif

    s_loaded = true;
    return 0;
}

int mdc_load_rom(const uint8_t *data, size_t len)
{
    if (!data || len < 0x200) return -1;

    size_t off = strip_smd_header(data, len);
    size_t rom_len = len - off;

    /* Copy path: over-allocate by 4 bytes for PicoCartInsert's safety
     * "jump-back" opcode at rom[romsize..romsize+3]. */
    s_rom_copy = (uint8_t *)malloc(rom_len + 4);
    if (!s_rom_copy) return -2;
    memcpy(s_rom_copy, data + off, rom_len);
    memset(s_rom_copy + rom_len, 0, 4);
    s_owns_rom = true;

    if (PicoCartInsert(s_rom_copy, (unsigned int)rom_len, NULL) != 0) {
        free(s_rom_copy);
        s_rom_copy = NULL;
        s_owns_rom = false;
        return -3;
    }

    return mdc_finish_load();
}

int mdc_load_rom_xip(const uint8_t *data, size_t len)
{
    if (!data || len < 0x200) return -1;

    size_t off = strip_smd_header(data, len);
    size_t rom_len = len - off;

    /* Borrow the XIP pointer — no copy. Suppress PicoCartInsert's
     * safety-op write (it wants to stomp 4 bytes past rom end at
     * rom[romsize..romsize+3], but XIP flash is read-only and the
     * write would either be silently dropped or — on some RP2350
     * QMI configurations — bus-fault into HardFault. The op is
     * only ever fetched on 68K runaway execution, which
     * well-behaved carts don't do. */
    extern int PicoCartSuppressSafetyOp;
    PicoCartSuppressSafetyOp = 1;

    s_rom_copy = (uint8_t *)(data + off);
    s_owns_rom = false;

    int rc = PicoCartInsert(s_rom_copy, (unsigned int)rom_len, NULL);
    PicoCartSuppressSafetyOp = 0;
    if (rc != 0) {
        s_rom_copy = NULL;
        return -3;
    }

    return mdc_finish_load();
}

void mdc_run_frame(void)
{
    if (!s_loaded) return;

#ifdef MD_SP_GUARD
    /* Arm the SP-leak guard after a short warmup. Cart init typically
     * loads its real SSP within the first dozen instructions; arming
     * after frame 8 (~140 ms) leaves room for the cart to set up its
     * stack but still catches genuine post-init SP corruption. */
    extern volatile unsigned int md_sp_check_armed;
    static int s_sp_warmup_frames = 0;
    if (s_sp_warmup_frames < 8) {
        md_sp_check_armed = 0;
        s_sp_warmup_frames++;
    } else {
        md_sp_check_armed = 1;
    }
#endif

    /* writeSound callback (capture_audio) runs inside PicoFrame and
     * fills s_mixbuf / s_mixcount before PicoDrive's own PsndClear
     * wipes PicoIn.sndOut. */
    PicoIn.sndOut = s_sndbuf;
    s_mixcount = 0;
#ifdef MD_LINE_SCRATCH
    /* Reset prev-dy trackers at frame boundary — first src line of
     * the frame always writes fresh (no blend with a stale previous
     * row from the prior frame). */
    s_prev_dy_fit  = -1;
    s_prev_dy_fill = -1;
#endif
    PicoFrame();
}

int mdc_refresh_rate(void)
{
    return Pico.m.pal ? 50 : 60;
}

void mdc_viewport(int *x, int *y, int *w, int *h)
{
    /* H40 vs H32: reg[12] bit 0 = 1 → H40 (320), else H32 (256).
     * V28 vs V30: reg[1] bit 3 = 1 → V30 (240), else V28 (224).
     * PicoDrive always renders into a 240-line slot centred on the
     * active area (loffs = (240 - lines) / 2 in PicoFrameStart), so
     * V28 frames live at y = 8..231 — we need to report that offset
     * back to the consumer or the crop will capture the 8 blank rows. */
    int w_out = (Pico.video.reg[12] & 1) ? 320 : 256;
    int h_out = (Pico.video.reg[1]  & 8) ? 240 : 224;
    int x_out = (Pico.video.reg[12] & 1) ? 0 : 32;   /* H32 centred in 320 */
    int y_out = (MDC_MAX_H - h_out) / 2;             /* V28 centred in 240 */
    if (x) *x = x_out;
    if (y) *y = y_out;
    if (w) *w = w_out;
    if (h) *h = h_out;
}

#ifdef MD_LINE_SCRATCH
const uint16_t *mdc_framebuffer(void) { return NULL; }  /* no full fb */

void mdc_set_scale_target(uint16_t *lcd_fb, int scale_mode,
                           int vx, int vy, int vw, int vh,
                           int pan_x, int pan_y)
{
    s_lcd_fb     = lcd_fb;
    s_scale_mode = (uint8_t)scale_mode;
    s_vx = (int16_t)vx; s_vy = (int16_t)vy;
    s_vw = (int16_t)vw; s_vh = (int16_t)vh;
    s_pan_x = (int16_t)pan_x; s_pan_y = (int16_t)pan_y;
    /* Invalidate LUT so next scan callback rebuilds against this vw. */
    s_lut_vw = -1;
}

void mdc_set_blend(int blend) { s_blend = blend ? 1 : 0; }

/* Rebuild the sx/sx2 source-column LUTs for the current viewport +
 * scale-mode. Cheap: 128 muls + 128 divs on mode/viewport change.
 *
 * FIT (scale_mode 0): stretch X to 128 cols, letterboxed Y (128x90).
 *     sx = dx * s_vw / 128
 * FILL (scale_mode 1): preserve aspect — Y uses full 128 rows, X
 *     uses the SAME scale as Y (128/s_vh) applied to a centred
 *     s_vh-wide source window. For H40 PAL (320x224):
 *       scale = 128/224 = 0.571
 *       visible src cols = 224, centred in 320 → crop 48 each side
 *       sx = 48 + dx * 224/128
 *     So MD sprites keep their native aspect; left+right 48px of
 *     the source are cropped off (matches SMS FILL's aspect-
 *     preserving approach).
 * CROP (scale_mode 2): uses memcpy path, no LUT needed. */
static void md_core_rebuild_sx_lut(void) {
    int vw = s_vw;
    int vh = s_vh;
    int mode = s_scale_mode;

    if (mode == 1) {
        /* FILL — preserve aspect by using Y scale in X too, then
         * crop centre-window of width `vh` out of `vw`. */
        int visible = vh;
        int crop    = (vw - visible) / 2;
        if (crop < 0) crop = 0;
        for (int dx = 0; dx < 128; dx++) {
            int sx  = crop + (dx * visible) / 128;
            if (sx >= vw) sx = vw - 1;
            int sx2 = sx + 1; if (sx2 >= vw) sx2 = sx;
            s_sx_lut[dx]  = (uint16_t)sx;
            s_sx2_lut[dx] = (uint16_t)sx2;
        }
    } else {
        /* FIT (and any future scale_mode==0 variants). Stretch X to
         * full 128 cols; Y letterboxing is applied separately in
         * the scan callback via dst_h. */
        for (int dx = 0; dx < 128; dx++) {
            int sx  = (dx * vw) / 128;
            int sx2 = sx + 1; if (sx2 >= vw) sx2 = sx;
            s_sx_lut[dx]  = (uint16_t)sx;
            s_sx2_lut[dx] = (uint16_t)sx2;
        }
    }
    s_lut_vw    = (int16_t)vw;
    s_lut_vh    = (int16_t)vh;
    s_lut_scale = (uint8_t)mode;
}

/* Downsample one MD scanline (just drawn into s_line_scratch) into
 * the 128x128 LCD framebuffer. `line` is 0..223 (V28) or 0..239 (V30).
 * Called from PicoDrive's per-line callback. Ran an IRAM_ATTR
 * variant — no FPS difference on Sonic 2, so this stays in XIP. */
int md_core_scan_end(unsigned int line)
{
    if (!s_lcd_fb) return 0;
    md_core_scan_fired = 1;
    /* Sample pixel values from a few columns to confirm FinalizeLine
     * is actually writing data to s_line_scratch. */
    md_core_line_or |= s_line_scratch[0]  | s_line_scratch[64]
                    |  s_line_scratch[160] | s_line_scratch[200];

    /* Line position inside the active viewport. PicoDrive's DrawSync
     * already handles the V28/V30 letterbox in the 240-slot; `line`
     * here is the absolute line within that 240-slot. Map it back
     * into 0..s_vh by subtracting s_vy. */
    int y_src = (int)line - s_vy;
    if (y_src < 0 || y_src >= s_vh) return 0;

    /* Refresh sx LUT when viewport OR scale_mode changes — FIT and
     * FILL sample X differently (stretch vs aspect-preserving crop). */
    if (s_vw != s_lut_vw || s_vh != s_lut_vh || s_scale_mode != s_lut_scale)
        md_core_rebuild_sx_lut();

    const uint16_t *srow = s_line_scratch + s_vx;

    if (s_scale_mode == 2) {           /* CROP 1:1 */
        int pmax_x = s_vw - 128; if (pmax_x < 0) pmax_x = 0;
        int pmax_y = s_vh - 128; if (pmax_y < 0) pmax_y = 0;
        int px = s_pan_x; if (px < 0) px = 0; if (px > pmax_x) px = pmax_x;
        int py = s_pan_y; if (py < 0) py = 0; if (py > pmax_y) py = pmax_y;
        int copy_h = s_vh < 128 ? s_vh : 128;
        int dy = y_src - py;
        if (dy < 0 || dy >= copy_h) return 0;
        int copy_w = s_vw < 128 ? s_vw : 128;
        int dst_x = (128 - copy_w) / 2;
        int dst_y = (128 - copy_h) / 2;
        uint16_t *drow = s_lcd_fb + (dst_y + dy) * 128 + dst_x;
        memcpy(drow, srow + px, copy_w * 2);
    } else {
        /* FIT (letterboxed 128x90) or FILL (stretched 128x128). */
        int dst_h = (s_scale_mode == 1) ? 128 : 90;
        int letterbox_top = (128 - dst_h) / 2;
        int dy = (y_src * dst_h) / s_vh;
        int16_t *prev_dy_slot = (s_scale_mode == 1)
                                ? &s_prev_dy_fill : &s_prev_dy_fit;
        int blend_with_prev = (dy == *prev_dy_slot);
        *prev_dy_slot = (int16_t)dy;
        uint16_t *drow = s_lcd_fb + (letterbox_top + dy) * 128;

        if (!s_blend) {
            /* Nearest-neighbour — fastest. For the "second src line
             * maps to same dst row" case, just skip; first src line's
             * pixels stay in place. */
            if (blend_with_prev) return 0;
            const uint16_t *sxl = s_sx_lut;
            for (int dx = 0; dx < 128; dx++) {
                drow[dx] = srow[sxl[dx]];
            }
        } else {
            /* Packed RGB565 2x2 box blend. GB-core trick: expand each
             * pixel to 32 bits as (p | p<<16) & 0x07E0F81F — R (5) and
             * B (5) end up in the low 16 bits with a gap bit each,
             * G (6) sits in the top 16 bits with a gap bit. Adding two
             * expanded pixels and shifting right by 1 (then masking)
             * averages all three channels simultaneously with one
             * 32-bit add + shift + AND, no per-channel extract. */
            const uint32_t MASK = 0x07E0F81Fu;
            const uint16_t *sxl  = s_sx_lut;
            const uint16_t *sxl2 = s_sx2_lut;
            for (int dx = 0; dx < 128; dx++) {
                uint32_t ea = ((uint32_t)srow[sxl[dx]]  | ((uint32_t)srow[sxl[dx]]  << 16)) & MASK;
                uint32_t eb = ((uint32_t)srow[sxl2[dx]] | ((uint32_t)srow[sxl2[dx]] << 16)) & MASK;
                uint32_t avg = ((ea + eb) >> 1) & MASK;
                if (blend_with_prev) {
                    uint16_t old_px = drow[dx];
                    uint32_t eo = ((uint32_t)old_px | ((uint32_t)old_px << 16)) & MASK;
                    avg = ((avg + eo) >> 1) & MASK;
                }
                drow[dx] = (uint16_t)((avg | (avg >> 16)) & 0xFFFFu);
            }
        }
    }
    return 0;
}

#else
const uint16_t *mdc_framebuffer(void) { return s_fb; }
#endif

void mdc_set_buttons(uint16_t mask)
{
    /* MD pad bit layout: bit 0 UP, 1 DOWN, 2 LEFT, 3 RIGHT,
     * 4 B, 5 C, 6 A, 7 START, 8 Z, 9 Y, 10 X, 11 MODE. Our MDC_BTN_*
     * defines are already in this order — copy directly. */
    PicoIn.pad[0] = mask;
    PicoIn.pad[1] = 0;
}

void mdc_set_skip_render(int skip)
{
    PicoIn.skipFrame = skip ? 1 : 0;
}

int mdc_audio_pull(int16_t *out, int n)
{
    /* HALF audio mode: s_sample_rate == 11025 but PWM runs at
     * 22050 — duplicate each source sample (zero-order hold).
     * FULL / OFF modes: s_sample_rate == 22050, plain memcpy. */
    if (s_sample_rate == 11025) {
        int src_want  = n / 2;
        int src_copy  = src_want < s_mixcount ? src_want : s_mixcount;
        for (int i = 0; i < src_copy; i++) {
            int16_t v = s_mixbuf[i];
            out[i * 2 + 0] = v;
            out[i * 2 + 1] = v;
        }
        return src_copy * 2;
    }
    int copy = (n < s_mixcount) ? n : s_mixcount;
    if (copy > 0) memcpy(out, s_mixbuf, copy * sizeof(int16_t));
    return copy;
}

uint8_t *mdc_battery_ram (void) { return Pico.sv.data ? (uint8_t *)Pico.sv.data : NULL; }
size_t   mdc_battery_size(void) { return (size_t)(Pico.sv.end - Pico.sv.start + 1); }

#ifdef THUMBY_STATE_BRIDGE
/* Bridge callback shims. thumby_state_bridge's read/write return the
 * fread/fwrite-style item count; PicoStateFP wants size_t return and
 * passes void* for the file handle. Signatures already match, but we
 * can't cast function pointers that take different concrete handle
 * types through an intermediate without a warning, so wrap. eof isn't
 * exposed by the bridge — fake it by returning 0 (PicoDrive's state
 * code only calls eof on gzip-compressed streams and our files are
 * plain-binary). */
static size_t mdc_bridge_read(void *p, size_t sz, size_t n, void *file) {
    return thumby_state_read(p, sz, n, (thumby_state_io_t *)file);
}
/* Per-call progress — ping PicoStateProgressCB so the LCD shows how
 * many areaWrite calls landed before any hang. state_save does two
 * bare header writes (8-byte "PicoSEXT", 4-byte ver) before the first
 * chunk-level CHECKED_WRITE, so healthy early sequence is
 *   W1 #0008, W2 #0004, then chunk-callback 'Saving.. M68K state'.
 * If we hang after 'stage: PicoStateFP' with NO W1 message, the
 * hang is in the buf2 = malloc(CHUNK_LIMIT_W) of state_save before
 * the first areaWrite. If we see W1 but not W2, the first f_write
 * hung. And so on. */
static int mdc_bridge_write_count;
static size_t mdc_bridge_write(void *p, size_t sz, size_t n, void *file) {
    extern void (*PicoStateProgressCB)(const char *str);
    mdc_bridge_write_count++;
    if (PicoStateProgressCB) {
        char msg[24];
        static const char hex[] = "0123456789ABCDEF";
        unsigned total = (unsigned)(sz * n);
        int i = 0;
        msg[i++]='b'; msg[i++]='r'; msg[i++]=':'; msg[i++]=' ';
        msg[i++]='W';
        int c = mdc_bridge_write_count;
        if (c >= 100) msg[i++] = '0' + (c/100)%10;
        if (c >= 10)  msg[i++] = '0' + (c/10)%10;
        msg[i++] = '0' + c%10;
        msg[i++]=' '; msg[i++]='#';
        msg[i++]= hex[(total >> 12) & 0xf];
        msg[i++]= hex[(total >>  8) & 0xf];
        msg[i++]= hex[(total >>  4) & 0xf];
        msg[i++]= hex[(total      ) & 0xf];
        msg[i] = '\0';
        PicoStateProgressCB(msg);
    }
    return thumby_state_write(p, sz, n, (thumby_state_io_t *)file);
}
static size_t mdc_bridge_eof(void *file) {
    (void)file;
    return 0;
}
static int mdc_bridge_seek(void *file, long off, int whence) {
    return thumby_state_seek((thumby_state_io_t *)file, off, whence);
}
#endif

int mdc_save_state(const char *path)
{
    if (!path || !s_loaded) return -1;
#ifdef THUMBY_STATE_BRIDGE
    /* Stage-level progress. The chunk-level PicoStateProgressCB only
     * fires once we're inside state_save's chunk loop; if the hang is
     * BEFORE the first chunk (e.g. f_open, PicoStateFP entry,
     * state_save's SekFinishIdleDet, the "PicoSEXT" header write) we
     * need earlier visibility. Poke the same global callback at each
     * stage boundary so the LCD shows the last completed stage. */
    extern void (*PicoStateProgressCB)(const char *str);
    void (*cb)(const char *) = PicoStateProgressCB;
    if (cb) cb("stage: open");
    thumby_state_io_t *io = thumby_state_open(path, "wb");
    if (!io) { if (cb) cb("stage: open failed"); return -1; }
    if (cb) cb("stage: PicoStateFP");
    int rc = PicoStateFP(io, 1,
                         mdc_bridge_read, mdc_bridge_write,
                         mdc_bridge_eof,  mdc_bridge_seek);
    if (cb) cb("stage: close");
    thumby_state_close(io);
    if (cb) cb("stage: done");
    return rc;
#else
    return PicoState(path, 1);   /* 1 = save */
#endif
}

int mdc_load_state(const char *path)
{
    if (!path || !s_loaded) return -1;
#ifdef THUMBY_STATE_BRIDGE
    extern void (*PicoStateProgressCB)(const char *str);
    void (*cb)(const char *) = PicoStateProgressCB;
    if (cb) cb("stage: open");
    thumby_state_io_t *io = thumby_state_open(path, "rb");
    if (!io) { if (cb) cb("stage: open failed"); return -1; }
    if (cb) cb("stage: PicoStateFP");
    int rc = PicoStateFP(io, 0,
                         mdc_bridge_read, mdc_bridge_write,
                         mdc_bridge_eof,  mdc_bridge_seek);
    if (cb) cb("stage: close");
    thumby_state_close(io);
    if (cb) cb("stage: done");
    return rc;
#else
    return PicoState(path, 0);   /* 0 = load */
#endif
}

void mdc_shutdown(void)
{
#ifdef MD_IRAM_DYNAMIC
    md_iram_shutdown();
#endif
    if (s_loaded) {
        /* PicoCartUnload calls plat_munmap(Pico.rom), which in our
         * thumby_platform.c is backed by free(). For the XIP path we
         * DON'T own Pico.rom, so patch it out before the unload to
         * prevent a free() on a flash pointer. */
        if (!s_owns_rom) {
            Pico.rom = NULL;
            Pico.romsize = 0;
        }
        PicoCartUnload();
        s_rom_copy = NULL;
        s_owns_rom = false;
        PicoExit();
        /* Drop the 256 KB FAME JumpTable so a sibling core (NES, SMS,
         * GB) can reuse that heap when this one isn't active. */
        fm68k_shutdown();
        s_loaded = false;
    }
#ifndef MD_LINE_SCRATCH
    if (s_fb) { free(s_fb); s_fb = NULL; }
#endif
}
