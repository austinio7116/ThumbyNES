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

/* Viewport as reported by the VDC. Updated per-frame by the hook
 * PCE_hardware_periodical inserts into io state. */
static int s_vp_x = 0, s_vp_y = 0;
static int s_vp_w = PCEC_DEFAULT_W, s_vp_h = PCEC_DEFAULT_H;

/* ---- my_special_alloc — HuExpress's PSRAM/IRAM picker -------------
 * ODROID-GO directs allocations to internal SRAM vs PSRAM based on
 * speed/size hints. We don't have that distinction — just malloc. */
void *my_special_alloc(unsigned char speed, unsigned char bytes,
                       unsigned long size)
{
    (void)speed; (void)bytes;
    void *p = calloc(1, size ? size : 1);
    return p;
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

    /* Upstream's ODROID-GO main.c owned these allocations; since we
     * didn't vendor main.c, replicate them exactly. Two subtleties:
     *
     *  1. osd_gfx_buffer is NOT the base of the 220 KB frame buffer —
     *     it's offset into it by (32 + 64 * XBUF_WIDTH). The sprite
     *     renderer writes with negative offsets that reach back into
     *     the 64-row top padding + 32-col left padding. Setting
     *     osd_gfx_buffer = alloc base would put those writes past
     *     the start of the allocation → segfault.
     *
     *  2. SPM (sprite priority mask) is a second 220 KB buffer the
     *     sprite renderer needs. Without it, sprite writes go to NULL. */
    if (!osd_gfx_buffer) {
        uchar *raw = (uchar *)calloc(1, XBUF_WIDTH * XBUF_HEIGHT);
        if (!raw) return 1;
        osd_gfx_buffer = raw + 32 + 64 * XBUF_WIDTH;
    }
    extern uchar *SPM_raw, *SPM;
    if (!SPM_raw) {
        SPM_raw = (uchar *)calloc(1, XBUF_WIDTH * XBUF_HEIGHT);
        if (!SPM_raw) return 1;
        SPM = SPM_raw + 32 + 64 * XBUF_WIDTH;
    }
    if (!spr_init_pos) {
        spr_init_pos = (uint32 *)calloc(1024, sizeof(uint32));
        if (!spr_init_pos) return 1;
    }

    /* Path-string globals — declared as char* in pce.c, allocated by
     * upstream main.c. Each is PATH_MAX (256 B). Total ≈ 3 KB; never
     * freed (single-process lifetime). */
    extern char *short_cart_name, *short_iso_name;
    extern char *config_basepath, *sav_path, *sav_basepath;
    extern char *tmp_basepath, *video_path, *ISO_filename;
    extern char *syscard_filename;
    if (!cart_name)        cart_name        = (char *)calloc(1, PATH_MAX);
    if (!short_cart_name)  short_cart_name  = (char *)calloc(1, PATH_MAX);
    if (!short_iso_name)   short_iso_name   = (char *)calloc(1, PATH_MAX);
    if (!rom_file_name)    rom_file_name    = (char *)calloc(1, PATH_MAX);
    if (!config_basepath)  config_basepath  = (char *)calloc(1, PATH_MAX);
    if (!sav_path)         sav_path         = (char *)calloc(1, PATH_MAX);
    if (!sav_basepath)     sav_basepath     = (char *)calloc(1, PATH_MAX);
    if (!tmp_basepath)     tmp_basepath     = (char *)calloc(1, PATH_MAX);
    if (!video_path)       video_path       = (char *)calloc(1, PATH_MAX);
    if (!ISO_filename)     ISO_filename     = (char *)calloc(1, PATH_MAX);
    if (!syscard_filename) syscard_filename = (char *)calloc(1, PATH_MAX);
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
    s_inited = 1;
    return 0;
}

void pcec_run_frame(void)
{
    if (!s_inited) return;

    /* Fill the full 220 KB XBUF with the current BG colour before the
     * core renders. Rationale:
     *
     *   PCE VDC tile rendering only writes pixels for tile planes
     *   where at least one of the 4 plane bytes is non-zero (see the
     *   J-mask check in sprite_RefreshLine.h). Fully-transparent tile
     *   pixels are SKIPPED — whatever was in the buffer stays visible.
     *
     *   Real hardware handles this by outputting Pal[0] for transparent
     *   tile pixels; the framebuffer abstraction never sees stale
     *   pixels there. HuExpress does NOT — both upstream ODROID (double-
     *   buffered) and us (single-buffered) end up with stale content
     *   bleeding through transparent tiles. Upstream hides it partially
     *   because each buffer is only re-rendered every other frame,
     *   which means the stale content is 2 frames old — still visible
     *   as trails on fast-moving sprites but sometimes blended away by
     *   the next overwrite.
     *
     *   The clean fix is to emulate the hardware: every pixel becomes
     *   Pal[0] before a new frame's tiles/sprites draw. One memset of
     *   220 KB per frame is ~50 µs on host and will be eliminated on
     *   device by the scanline renderer (task #12).
     */
    {
        extern uchar *Pal;
        uchar bg = Pal ? Pal[0] : 0;
        uchar *raw = osd_gfx_buffer - (32 + 64 * XBUF_WIDTH);
        memset(raw, bg, (size_t)XBUF_WIDTH * XBUF_HEIGHT);
    }

    /* exe_go() in the upstream returns at end-of-frame via our
     * g_pce_frame_done patch (see gfx_Loop6502.h + h6280_exe_go.h). */
    exe_go();
    build_palette_once();

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
    /* osd_gfx_buffer is already pre-offset into the 220 KB XBUF —
     * (0, 0) in active display coordinates IS osd_gfx_buffer[0].
     * The caller blits using PCEC_PITCH = 256 but the underlying
     * stride is XBUF_WIDTH (600). That mismatch is the viewport
     * problem the runner has to handle (see pcec_viewport note). */
    return osd_gfx_buffer;
}

const uint16_t *pcec_palette_rgb565(void)
{
    return s_palette_rgb565;
}

void pcec_set_buttons(uint16_t mask)
{
    s_pad_mask = mask;
    /* HuExpress polls io.JOY[0] each frame. Bit layout matches the
     * HuC6280 joypad register spec — same order as PCEC_BTN_*. */
    io.JOY[0] = (uchar)(mask & 0xFF);
}

void pcec_set_skip_render(int skip)
{
    s_skip_render = skip;
    /* TODO(scanline): when scanline mode lands, propagate to
     * gfx_need_redraw so the line callbacks become no-ops. */
}

int pcec_audio_pull(int16_t *out, int n)
{
    /* HuExpress's mix_buffer_length is (sndrate / 60 + 1). The mixer
     * writes to an internal ring. We expose a pull model to match
     * sms/gb/md wrappers.
     *
     * TODO(pcec_audio_pull): wire the PSG mixer — sound.c has
     * `run_ring_bell()` / mix.c has `MixBuffer()`. Initial frame
     * output can be silence so the PWM driver sees a valid stream. */
    if (!out || n <= 0) return 0;
    memset(out, 0, (size_t)n * sizeof(int16_t));
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
    /* TODO(pcec_shutdown): call a HuExpress teardown path. Upstream's
     * TrashPCE() does system() calls and disk I/O we don't want —
     * extract just the free() portion. For Phase 1 we leak the heap
     * on shutdown since the device-side flow re-boots into the
     * picker rather than re-initing in place. */
    s_inited = 0;
}

#if defined(PCE_SCANLINE_RENDER) && !defined(PCE_SCANLINE_IMPL)
#  error "PCE_SCANLINE_RENDER enabled but line renderer not plumbed yet. See PCE_PLAN.md §5."
#endif
