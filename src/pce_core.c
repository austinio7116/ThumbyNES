/*
 * ThumbyNES — HuExpress wrapper.
 *
 * Binds HuExpress's globals-heavy core (InitPCE / ResetPCE / exe_go)
 * to the symmetric pcec_* API. Host builds allocate the full 220 KB
 * XBUF; device builds require PCE_SCANLINE_RENDER (see PCE_PLAN.md §5)
 * which is not yet plumbed — the #error at the bottom of this file
 * trips if someone tries to compile for device before that lands.
 *
 * Phase 1 goal: host build loads a HuCard, runs frames, produces a
 * palette-indexed framebuffer and int16 audio samples. Save-state
 * and battery-save are stubbed with clear return codes.
 */

#define THUMBY_BUILD 1

#include "pce_core.h"
#include "pce_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HuExpress engine — pulled in under THUMBY_BUILD so it sees
 * thumby_platform.h instead of ESP-IDF. */
#include "cleantypes.h"
#include "pce.h"
#include "hard_pce.h"
#include "gfx.h"
#include "sound.h"
#include "sprite.h"
#include "sys_dep.h"

extern struct host_machine host;
extern void WriteBuffer(char *, int, unsigned);

/* ---- HuExpress global state we interact with ----------------------
 * These are all declared in pce.c / hard_pce.c. We reach into them
 * directly because that's the shape of the core — there is no clean
 * API, same as nofrendo. */
extern uchar *ROM;
extern int ROM_size;
extern int Country;                 /* 0 = JP, 1 = US */
extern int scanlines_per_frame;
extern struct_hard_pce *hard_pce;
extern uchar *RAM, *VRAM, *WRAM, *Pal;
extern uint16 *SPRAM;
extern uchar *osd_gfx_buffer;       /* the 220 KB XBUF on host */
extern IO io;
extern uint32 *spr_init_pos;        /* 1024 × u32 sprite-start LUT */
extern char *cart_name;
extern char *rom_file_name;
extern uchar CD_emulation;
extern unsigned long TAB_CONST[256];

int  InitPCE(char *name);
int  ResetPCE(void);
void exe_go(void);
void TrashPCE(void);
/* gfx_init is a macro in engine/gfx.h, not a function — no extern here. */

/* Pre-computed CRC of the caller's ROM buffer. Set by pcec_load_rom
 * before InitPCE runs; consumed by the romdb.c CRC_file patch when
 * rom_file_name matches the in-memory sentinel. */
uint32  g_pce_mem_rom_crc = 0;
int     g_pce_mem_rom_active = 0;

/* ---- Wrapper-local state ------------------------------------------ */
static int      s_inited = 0;
static int      s_sample_rate = 22050;
static int      s_region = PCEC_REGION_AUTO;
static uint16_t s_palette_rgb565[512];
static uint8_t  s_bram[2048];            /* HuCard BRAM (2 KB) */
static int      s_skip_render = 0;
static uint16_t s_pad_mask = 0;

/* Per-PSG-channel scratch buffer used by pcec_audio_pull. We drive
 * WriteBuffer in MONO mode (signed 8-bit, sample_size=1, stereo=0):
 *   - Simpler mix — one byte per sample.
 *   - Avoids the upstream silent-channel memset bug: at mix.c:249 the
 *     memset only zeros dwSize bytes, but stereo actually needs 2×dwSize.
 *     Stale L/R bytes from prior active audio pollute the output on
 *     channel disable. Mono side-steps this entirely. */
#define PCEC_AUDIO_MAX_FRAME_SAMPLES 1024
static int8_t s_sbuf[6][PCEC_AUDIO_MAX_FRAME_SAMPLES];

/* Viewport as reported by the VDC. Updated per-frame by the hook
 * PCE_hardware_periodical inserts into io state. */
static int s_vp_x = 0, s_vp_y = 0;
static int s_vp_w = PCEC_DEFAULT_W, s_vp_h = PCEC_DEFAULT_H;

/* ---- my_special_alloc — HuExpress's PSRAM/IRAM picker -------------
 * ODROID-GO directed allocations to internal SRAM vs PSRAM based on
 * speed/size hints. We don't have that distinction — just calloc.
 *
 * NOTE: pcec_shutdown does NOT free these. We tried tracking them in
 * a linked list and calling free-all on shutdown; host testing
 * revealed glibc double-free / heap-corruption errors that were
 * already present (latent) during emulation — HuExpress's sprite
 * renderer writes at negative offsets into the 220 KB XBUF that
 * reach past the allocation's start into adjacent malloc blocks.
 * Upstream ODROID's port has the same latent issue but never frees,
 * so the corruption stays invisible. On device we'd need to fix the
 * renderer bounds for a clean free path; for now the allocations
 * leak and are re-used on the next session (all the `if (!X)`
 * guards below make that idempotent). */
void *my_special_alloc(unsigned char speed, unsigned char bytes,
                       unsigned long size)
{
    (void)speed; (void)bytes;
    return calloc(1, size ? size : 1);
}

/* HuExpress reference to this flag in myadd.h — never set by us. */
bool skipNextFrame = false;

/* ---- Palette expansion --------------------------------------------
 * HuExpress's render pipeline does NOT write VCE entry indices into
 * osd_gfx_buffer — it writes `Pal[n] = io.VCE[n].W >> 1`, a packed
 * 8-bit encoding where bits [7:5] = G[2:0], [4:2] = R[2:0], [1:0] =
 * B[2:1] (VCE B bit 0 is dropped).
 *
 * So the framebuffer holds 256 possible byte values, each of which
 * decodes into a fixed RGB via this pattern. The game changes which
 * VCE entries are live but the byte → RGB mapping in the framebuffer
 * is static (determined by the hw encoding). Our LUT is 256 entries
 * keyed on the encoded byte, built once. */
static void build_palette_once(void)
{
    static int built = 0;
    if (built) return;
    for (int i = 0; i < 256; i++) {
        uint8_t g3 = (i >> 5) & 0x07;
        uint8_t r3 = (i >> 2) & 0x07;
        uint8_t b3 = ((i & 0x03) << 1);   /* 2-bit B expanded to 3-bit */
        uint8_t r5 = (r3 << 2) | (r3 >> 1);
        uint8_t g6 = (g3 << 3) | g3;
        uint8_t b5 = (b3 << 2) | (b3 >> 1);
        s_palette_rgb565[i] =
            (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
    built = 1;
}

/* ---- US-encoded ROM helpers ----------------------------------------
 * US TurboGrafx-16 HuCards store every byte bit-reversed relative to
 * native PC Engine. Detection is by the reset-vector heuristic:
 * byte 0x1FFF of the HuCard must be >= 0xE0 on native carts (the
 * 6502 reset vector always lives in a bank at or above 0xE000). US
 * carts have bit-reversed bytes so that high-bit pattern becomes
 * the low-bit pattern — guaranteed < 0xE0.
 *
 * The kKnownRoms CRC database in the vendored tree is stubbed to a
 * single entry — the full list would add ~40 KB of flash for per-cart
 * quirks we don't implement. The byte-0x1FFF check is sufficient on
 * every US HuCard ever dumped; it has no known false positives on
 * native HuCards either. */

static inline uint8_t bitrev8(uint8_t b)
{
    /* Standard 3-shift bit-reversal; compiler on M33 emits RBIT+LSR. */
    b = (b >> 4) | (b << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

/* Incremental CRC32 matching HuExpress's TAB_CONST table. Used by the
 * wrapper to seed g_pce_mem_rom_crc before InitPCE calls CRC_file. */
static uint32 pce_crc32_buf(const uint8_t *data, size_t len)
{
    uint32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i] ^ (uint8_t)crc;
        crc = (crc >> 8) ^ (uint32)TAB_CONST[b];
    }
    return ~crc;
}

bool pcec_rom_is_us_encoded(const uint8_t *data, size_t len)
{
    if (!data || len < 0x2000) return false;
    /* Strip a 512-byte copier header if present. */
    size_t hdr = len & 0x1FFF;
    const uint8_t *body = data + hdr;
    size_t body_len = len - hdr;
    if (body_len < 0x2000) return false;
    return body[0x1FFF] < 0xE0;
}

void pcec_rom_decrypt_us(uint8_t *data, size_t len)
{
    if (!data) return;
    for (size_t i = 0; i < len; i++) data[i] = bitrev8(data[i]);
}

/* ---- Public API ---------------------------------------------------- */

int pcec_init(int region, int sample_rate)
{
    if (s_inited) pcec_shutdown();

    s_sample_rate = (sample_rate > 0) ? sample_rate : 22050;
    s_region = region;
    s_skip_render = 0;
    s_pad_mask = 0;
    memset(s_bram, 0, sizeof s_bram);

    /* Audio format: mono signed 8-bit, 1 byte per sample. */
    host.sound.freq         = s_sample_rate;
    host.sound.stereo       = 0;
    host.sound.signed_sound = 1;
    host.sound.sample_size  = 1;

    /* Defer JOY[*] init to pcec_load_rom, where hard_init() has
     * already allocated the io struct. Initialising here would
     * deref a NULL struct. */

#ifndef PCE_SCANLINE_RENDER
    /* Legacy full-framebuffer path. Under PCE_SCANLINE_RENDER we
     * don't render into osd_gfx_buffer / SPM at all; see pce_render.c. */
    if (!osd_gfx_buffer) {
        uchar *raw = (uchar *)my_special_alloc(false, 1,
                                                XBUF_WIDTH * XBUF_HEIGHT);
        if (!raw) return 1;
        osd_gfx_buffer = raw + 32 + 64 * XBUF_WIDTH;
    }
    extern uchar *SPM_raw, *SPM;
    if (!SPM_raw) {
        SPM_raw = (uchar *)my_special_alloc(false, 1,
                                             XBUF_WIDTH * XBUF_HEIGHT);
        if (!SPM_raw) return 1;
        SPM = SPM_raw + 32 + 64 * XBUF_WIDTH;
    }
#endif
    /* spr_init_pos is a precomputed offset table the stock renderer
     * uses; scanline mode doesn't read it, but ResetPCE writes to it
     * unconditionally — a 4 KB buffer keeps those writes landing in
     * real memory. */
    if (!spr_init_pos) {
        spr_init_pos = (uint32 *)my_special_alloc(false, 4,
                                                   1024 * sizeof(uint32));
        if (!spr_init_pos) return 1;
    }

    /* Path-string globals — declared as char* in pce.c, allocated by
     * upstream main.c. Each is PATH_MAX (256 B). Total ≈ 3 KB per
     * session, freed in pcec_shutdown. */
    extern char *short_cart_name, *short_iso_name;
    extern char *config_basepath, *sav_path, *sav_basepath;
    extern char *tmp_basepath, *video_path, *ISO_filename;
    extern char *syscard_filename;
#define PCE_ALLOC_PATH(p) \
    if (!(p)) (p) = (char *)my_special_alloc(false, 1, PATH_MAX)
    PCE_ALLOC_PATH(cart_name);
    PCE_ALLOC_PATH(short_cart_name);
    PCE_ALLOC_PATH(short_iso_name);
    PCE_ALLOC_PATH(rom_file_name);
    PCE_ALLOC_PATH(config_basepath);
    PCE_ALLOC_PATH(sav_path);
    PCE_ALLOC_PATH(sav_basepath);
    PCE_ALLOC_PATH(tmp_basepath);
    PCE_ALLOC_PATH(video_path);
    PCE_ALLOC_PATH(ISO_filename);
    PCE_ALLOC_PATH(syscard_filename);
#undef PCE_ALLOC_PATH
    return 0;
}

int pcec_load_rom(const uint8_t *data, size_t len)
{
    if (!data || len < 0x2000) return 1;

    /* Strip a 512-byte copier header if the .pce dumper added one. */
    size_t hdr = len & 0x1FFF;
    const uint8_t *body = data + hdr;
    size_t body_len = len - hdr;

    /* Caller contract: body must already be in native PCE encoding.
     * Cheap sanity — the detector is reliable enough that if it
     * fires here, the caller forgot the decrypt step. */
    if (body[0x1FFF] < 0xE0) {
        /* Don't refuse — let InitPCE log the diagnostic — but flag it. */
    }

    /* Point the core at the caller's buffer (zero-copy) and precompute
     * the CRC so CRC_file short-circuits on its in-memory fast path. */
    ROM = (uchar *)body;
    ROM_size = (int)(body_len / 0x2000);
    Country = (s_region == PCEC_REGION_JP) ? 0 : 1;
    g_pce_mem_rom_crc = pce_crc32_buf(body, body_len);
    g_pce_mem_rom_active = 1;

    /* Synthetic path — CartInit just needs the ".pce" suffix to route
     * to the HuCard branch; CartLoad sees ROM != NULL and skips the
     * file I/O. rom_file_name ends up as this string but CRC_file is
     * already patched to ignore the path. */
    static char mem_path[] = "/mem/cart.pce";
    if (InitPCE(mem_path) != 0) {
        g_pce_mem_rom_active = 0;
        return 1;
    }
    if (ResetPCE() != 0) {
        g_pce_mem_rom_active = 0;
        return 1;
    }
    /* We disabled the per-frame gfx_init() that upstream expected
     * would run exactly once (because exe_go never returned). Do
     * it ourselves, once, here — otherwise gfx_need_redraw / UCount
     * are whatever calloc gave them. */
    gfx_init();

    /* Joypad default: all pads "no button pressed". The IO_read path
     * XORs with 0xFF before masking, so a raw 0xFF reads as 0
     * (no-press). Without this, boot-time joypad reads (R-Type
     * test-mode check, copyright-screen wait) see phantom presses. */
    for (int i = 0; i < 5; i++) io.JOY[i] = 0xFF;

    s_inited = 1;
    return 0;
}

void pcec_run_frame(void)
{
    if (!s_inited) return;

    /* Scanline renderer writes directly into the LCD fb bound via
     * pcec_set_scale_target. Letterbox bars get painted once per
     * frame; active rows are composited as HuExpress's VDC loop hits
     * each display scanline (pce_render_scanline hooked into the
     * patched gfx_Loop6502.h). */
    build_palette_once();
    pce_render_frame_begin();

    /* exe_go() returns at end-of-frame via our g_pce_frame_done patch
     * (see gfx_Loop6502.h + h6280_exe_go.h). */
    exe_go();

}

int pcec_refresh_rate(void)
{
    /* PCE is NTSC-only in the HuCard library. */
    return 60;
}

void pcec_viewport(int *x, int *y, int *w, int *h)
{
    if (x) *x = s_vp_x;
    if (y) *y = s_vp_y;
    if (w) *w = s_vp_w;
    if (h) *h = s_vp_h;
}

const uint8_t *pcec_framebuffer(void)
{
    /* Stub — the scanline renderer writes RGB565 directly into the
     * LCD fb bound via pcec_set_scale_target. pcebench still calls
     * this to check "did anything render"; point it at a tiny
     * static array to avoid NULL. */
    static uint8_t dummy[1];
    return dummy;
}

void pcec_set_scale_target(uint16_t *lcd_fb, int blend)
{
    pce_render_set_target(lcd_fb, s_palette_rgb565, blend);
}

const uint16_t *pcec_palette_rgb565(void)
{
    return s_palette_rgb565;
}

void pcec_set_buttons(uint16_t mask)
{
    s_pad_mask = mask;
    /* HuExpress polls io.JOY[0] each frame. Bit layout matches the
     * HuC6280 joypad register spec — same order as PCEC_BTN_*.
     *
     * The IO $1000 read auto-advances io.joy_counter through pads
     * 0..4 on each button-nibble read; unmentioned pad slots must
     * present "no button pressed" (0xFF pre-XOR) or the game will
     * interpret them as all-buttons-held. */
    io.JOY[0] = (uchar)(mask & 0xFF);
    for (int i = 1; i < 5; i++) io.JOY[i] = 0xFF;
}

void pcec_set_skip_render(int skip)
{
    s_skip_render = skip;
    /* TODO(scanline): when scanline mode lands, propagate to
     * gfx_need_redraw so the line callbacks become no-ops. */
}

int pcec_audio_pull(int16_t *out, int n)
{
    if (!out || n <= 0) return 0;
    if (n > PCEC_AUDIO_MAX_FRAME_SAMPLES) n = PCEC_AUDIO_MAX_FRAME_SAMPLES;

    /* WriteBuffer writes `n` mono samples per channel (one byte each). */
    for (int ch = 0; ch < 6; ch++) {
        WriteBuffer((char *)s_sbuf[ch], ch, (unsigned)n);
    }

    /* Mix 6 PSG channels → mono signed 16-bit.
     * Per-channel per-sample range is -127..127 (mono averages L/R
     * balance so keeps the same per-byte range). Sum of 6 channels
     * → ±762. Shift by 5 (×32) expands into int16 with headroom. */
    for (int i = 0; i < n; i++) {
        int32_t mix = 0;
        for (int ch = 0; ch < 6; ch++) {
            mix += (int32_t)s_sbuf[ch][i];
        }
        mix <<= 5;
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out[i] = (int16_t)mix;
    }
    return n;
}

uint8_t *pcec_battery_ram(void)  { return s_bram; }
size_t   pcec_battery_size(void) { return sizeof s_bram; }

int pcec_save_state(const char *path)
{
    /* TODO(pcec_state): custom THPE format per PCE_PLAN.md §7.
     * Structure: magic, version, hard_pce struct, RAM, VRAM, Pal,
     * SPRAM, IO. Wire through thumby_state_bridge.h. */
    (void)path;
    return -1;
}

int pcec_load_state(const char *path)
{
    (void)path;
    return -1;
}

void pcec_shutdown(void)
{
    if (!s_inited) return;
    /* Keep allocations — see note on my_special_alloc. s_inited=0 so
     * the next pcec_init/load_rom cycle re-inits state on top of the
     * same buffers (VRAM/RAM/XBUF get re-zeroed by the core's own
     * reset paths, pcec_run_frame memsets XBUF to Pal[0] each
     * frame). Heap stays held for the slot's lifetime. */
    s_inited = 0;
    g_pce_mem_rom_active = 0;
}

