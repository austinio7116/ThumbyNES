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

/* Full 320x240 RGB565 frame. PicoDrive writes one scanline at a time
 * through DrawLineDest; we hand it a contiguous buffer and a row
 * increment (in bytes) so each line lands at the right Y offset. */
static uint16_t *s_fb;              /* MDC_MAX_W * MDC_MAX_H shorts */
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

    if (!s_fb) {
        s_fb = (uint16_t *)calloc(MDC_MAX_W * MDC_MAX_H, sizeof(uint16_t));
        if (!s_fb) return -1;
    }

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

int mdc_load_rom(const uint8_t *data, size_t len)
{
    if (!data || len < 0x200) return -1;

    size_t off = strip_smd_header(data, len);
    size_t rom_len = len - off;

    /* PicoDrive takes ownership of the buffer handed to PicoCartInsert
     * (calls plat_munmap on unload). Copy so callers keep ownership of
     * their original buffer. Over-allocate by 4 bytes: PicoCartInsert
     * writes a safety "jump-back" opcode at rom[romsize..romsize+3]. */
    s_rom_copy = (uint8_t *)malloc(rom_len + 4);
    if (!s_rom_copy) return -2;
    memcpy(s_rom_copy, data + off, rom_len);
    memset(s_rom_copy + rom_len, 0, 4);

    /* With FAME_BIG_ENDIAN defined at compile time, ROM stays raw
     * big-endian in memory. FAME fetch, memory.c u16 reads, and the
     * rom_read32 helper all byteswap on access (one M33 REV16 each,
     * single cycle). No pre-swap pass — lets the device build borrow
     * the XIP pointer directly for carts up to 8 MB without any RAM
     * copy. */

    /* PicoCartInsert: parses header, allocates SRAM if declared, wires
     * the memory map, and calls PicoPower() internally — so no
     * separate PicoReset needed. */
    if (PicoCartInsert(s_rom_copy, (unsigned int)rom_len, NULL) != 0) {
        free(s_rom_copy);
        s_rom_copy = NULL;
        return -3;
    }

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
    PicoDrawSetOutBuf(s_fb, MDC_MAX_W * sizeof(uint16_t));

    s_loaded = true;
    return 0;
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

const uint16_t *mdc_framebuffer(void) { return s_fb; }

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
        PicoCartUnload();  /* frees s_rom_copy via plat_munmap */
        s_rom_copy = NULL;
        PicoExit();
        s_loaded = false;
    }
    if (s_fb) { free(s_fb); s_fb = NULL; }
}
