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
/* fm68k_shutdown() for device heap release. Prototype inline to avoid
 * pulling in the full FAME header (which wants `cpu/fame/fame.h` on
 * picodrive's private include path). */
void fm68k_shutdown(void);

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
static int16_t    s_vx, s_vy, s_vw, s_vh;
static int16_t    s_pan_x, s_pan_y;
#else
/* Full 320x240 RGB565 frame. PicoDrive writes one scanline at a time
 * through DrawLineDest; we hand it a contiguous buffer and a row
 * increment (in bytes) so each line lands at the right Y offset. */
static uint16_t *s_fb;              /* MDC_MAX_W * MDC_MAX_H shorts */
#endif
static int16_t   s_sndbuf[2048];    /* stereo int16, ~735 samples/frame @ 22050/60 */
static int16_t   s_mixbuf[1024];    /* mono int16 after downmix */
static int       s_mixcount;
static int       s_sample_rate;
static bool      s_loaded;
/* We malloc and own the ROM buffer handed to PicoCartInsert. */
static uint8_t  *s_rom_copy;

/* PicoDrive calls writeSound at end-of-frame with length-in-bytes,
 * IMMEDIATELY before PsndClear wipes the buffer. We have to capture
 * the stereo samples here and downmix into s_mixbuf — reading
 * PicoIn.sndOut from the caller after PicoFrame returns gets zeros. */
static void capture_audio(int len_bytes)
{
    int stereo = (PicoIn.opt & POPT_EN_STEREO) ? 1 : 0;
    int n = len_bytes / (stereo ? 4 : 2);   /* bytes → stereo sample pairs */
    int cap = (int)(sizeof(s_mixbuf) / sizeof(s_mixbuf[0]));
    if (n > cap) n = cap;
    if (stereo) {
        for (int i = 0; i < n; i++) {
            int32_t l = s_sndbuf[i * 2 + 0];
            int32_t r = s_sndbuf[i * 2 + 1];
            int32_t m = (l + r) >> 1;
            if (m >  32767) m =  32767;
            if (m < -32768) m = -32768;
            s_mixbuf[i] = (int16_t)m;
        }
    } else {
        memcpy(s_mixbuf, s_sndbuf, n * sizeof(int16_t));
    }
    s_mixcount = n;
}

int mdc_init(int region, int sample_rate)
{
    s_loaded   = false;
    s_mixcount = 0;
    s_sample_rate = sample_rate;

#ifndef MD_LINE_SCRATCH
    if (!s_fb) {
        s_fb = (uint16_t *)calloc(MDC_MAX_W * MDC_MAX_H, sizeof(uint16_t));
        if (!s_fb) return -1;
    }
#endif

    PicoInit();

    PicoIn.opt = POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
               | POPT_EN_STEREO | POPT_ACC_SPRITES;
    PicoIn.sndRate        = sample_rate;
    PicoIn.sndOut         = s_sndbuf;
    PicoIn.writeSound     = capture_audio;
    PicoIn.regionOverride = (unsigned short)region;
    PicoIn.autoRgnOrder   = 0x148;   /* prefer EU, then US, then JP */

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

    /* Borrow the XIP pointer — no copy. PicoCartInsert wants to stomp
     * a 4-byte safety opcode at rom[romsize..romsize+3], which we
     * can't do on XIP flash. Accept that the safety write falls on
     * whatever is at offset romsize in flash (padding / next file);
     * the 68K only ever fetches from there on runaway execution,
     * which well-behaved carts don't do. */
    s_rom_copy = (uint8_t *)(data + off);
    s_owns_rom = false;

    if (PicoCartInsert(s_rom_copy, (unsigned int)rom_len, NULL) != 0) {
        s_rom_copy = NULL;
        return -3;
    }

    return mdc_finish_load();
}

void mdc_run_frame(void)
{
    if (!s_loaded) return;

    /* writeSound callback (capture_audio) runs inside PicoFrame and
     * fills s_mixbuf / s_mixcount before PicoDrive's own PsndClear
     * wipes PicoIn.sndOut. */
    PicoIn.sndOut = s_sndbuf;
    s_mixcount = 0;
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
}

/* Downsample one MD scanline (just drawn into s_line_scratch) into
 * the 128x128 LCD framebuffer. `line` is 0..223 (V28) or 0..239 (V30).
 * Called from PicoDrive's per-line callback. */
int md_core_scan_end(unsigned int line)
{
    if (!s_lcd_fb) return 0;

    /* Line position inside the active viewport. PicoDrive's DrawSync
     * already handles the V28/V30 letterbox in the 240-slot; `line`
     * here is the absolute line within that 240-slot. Map it back
     * into 0..s_vh by subtracting s_vy. */
    int y_src = (int)line - s_vy;
    if (y_src < 0 || y_src >= s_vh) return 0;

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
        /* Map source y to dest dy. */
        int dy = (y_src * dst_h) / s_vh;
        /* Which src lines map to this dy? Draw only when this is
         * the first src line for that dy — avoids redundant writes
         * for multi-src-per-dy cases. */
        int prev_dy = (y_src > 0) ? ((y_src - 1) * dst_h) / s_vh : -1;
        if (dy == prev_dy) return 0;
        uint16_t *drow = s_lcd_fb + (letterbox_top + dy) * 128;
        for (int dx = 0; dx < 128; dx++) {
            int sx = (dx * s_vw) / 128;
            drow[dx] = srow[sx];
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

int mdc_audio_pull(int16_t *out, int n)
{
    int copy = (n < s_mixcount) ? n : s_mixcount;
    if (copy > 0) memcpy(out, s_mixbuf, copy * sizeof(int16_t));
    return copy;
}

uint8_t *mdc_battery_ram (void) { return Pico.sv.data ? (uint8_t *)Pico.sv.data : NULL; }
size_t   mdc_battery_size(void) { return (size_t)(Pico.sv.end - Pico.sv.start + 1); }

int mdc_save_state(const char *path)
{
    if (!path || !s_loaded) return -1;
    return PicoState(path, 1);   /* 1 = save */
}

int mdc_load_state(const char *path)
{
    if (!path || !s_loaded) return -1;
    return PicoState(path, 0);   /* 0 = load */
}

void mdc_shutdown(void)
{
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
