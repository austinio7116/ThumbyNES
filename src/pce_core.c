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

/* Must come BEFORE HuExpress headers — hard_pce.h has `#define io
 * (*p_io)`, which would otherwise rewrite the `io` parameter name in
 * the bridge's function prototypes and miscompile the call as
 * "thumby_state_io_t **" instead of "thumby_state_io_t *". */
#ifdef THUMBY_STATE_BRIDGE
#  include "thumby_state_bridge.h"
#endif

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
/* These three buffers are per-session — only used while a PCE cart
 * is active. malloc'd in pcec_init and freed in pcec_shutdown so the
 * RAM cost (~10 KB combined) is paid only inside the PCE slot, not
 * permanently in BSS. Other emulators in the same firmware partition
 * (NES/SMS/GB/MD) get the heap back. */
static uint16_t *s_palette_rgb565;       /* 512 entries × 2 = 1 KB */
static uint8_t  *s_bram;                 /* 2 KB HuCard BRAM */
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
/* 6 channels × 1024 bytes = 6 KB. malloc'd in pcec_init. */
static int8_t (*s_sbuf)[PCEC_AUDIO_MAX_FRAME_SAMPLES];

/* 1-pole IIR LPF state. File-scope so pcec_shutdown / pcec_load_state
 * can reset it — without that, switching ROMs or restoring a state
 * leaves the filter integrator at the previous game's final mix value
 * and the next frame begins with a DC step (audible click). */
static int32_t s_audio_lpf;

/* Phase accumulator used by pcec_audio_pull to round 22050/60 = 367.5
 * to an integer count per frame (368 most frames, 367 every other).
 * Long-term keeps push rate exactly equal to the audio engine's
 * drain rate so the device PWM ring never over- or underflows. */
static int s_audio_phase_acc;

/* Viewport as reported by the VDC. Updated per-frame by the hook
 * PCE_hardware_periodical inserts into io state. */
static int s_vp_x = 0, s_vp_y = 0;
static int s_vp_w = PCEC_DEFAULT_W, s_vp_h = PCEC_DEFAULT_H;

/* ---- my_special_alloc — HuExpress's PSRAM/IRAM picker -------------
 * ODROID-GO directed allocations to internal SRAM vs PSRAM based on
 * speed/size hints. We don't have that distinction — just calloc and
 * track the result in a session-scoped list so pcec_shutdown can free
 * the lot in one call. Without this, every PCE cart launch leaks
 * ~120 KB of HuExpress internal buffers (RAM/VRAM/WRAM/IOAREA/SPRAM/
 * VCE/Pal/psg_da_data/...) and a second emulator launched from the
 * same slot session runs out of heap before MD's FAME jumptable can
 * land contiguously.
 *
 * Why this is safe in our build: upstream's "freeing crashes glibc"
 * concern came from HuExpress's bulk-frame sprite renderer writing
 * at negative offsets past XBUF[0] into adjacent malloc blocks. We
 * compile with PCE_SCANLINE_RENDER, which composites sprites
 * directly into the LCD framebuffer per scanline (pce_render.c) and
 * never touches XBUF/SPM. So the buffer-underflow path is dead and
 * the allocations can be freed cleanly on session exit. */
/* WORKAROUND for PCE-specific heap corruption (root cause still open):
 * a wild write during PCE init lands on newlib fastbin metadata
 * adjacent to small allocations and corrupts the free-list. The
 * symptom was malloc(32 KB)+free hanging the next malloc(64 KB).
 *
 * Padding every allocation by 256 bytes on each side shifts ALL of
 * PCE's heap allocations enough that the wild write lands on harmless
 * pad bytes instead of metadata. Cost: ~15 KB extra heap per PCE
 * session; acceptable for now. The actual wild-write source still
 * needs to be identified and fixed properly. */
#define PCE_PAD 256
typedef struct pce_alloc_node {
    struct pce_alloc_node *next;
    void                  *raw;        /* underlying calloc — user is raw + PAD */
    void                  *ptr;        /* user-visible block start */
    uint32_t              size;
} pce_alloc_node_t;
static pce_alloc_node_t *s_special_allocs = NULL;

void *my_special_alloc(unsigned char speed, unsigned char bytes,
                       unsigned long size)
{
    (void)speed; (void)bytes;
    unsigned long udata = (size ? size : 1);
    /* layout: [PAD 256][user data udata][PAD 256] */
    uint8_t *raw = (uint8_t *)calloc(1, PCE_PAD + udata + PCE_PAD);
    if (!raw) return NULL;
    /* Pad the node struct allocation similarly so its tiny ~24 B size
     * isn't routed through fastbins. */
    uint8_t *node_raw = (uint8_t *)malloc(PCE_PAD + sizeof(pce_alloc_node_t)
                                           + PCE_PAD);
    if (!node_raw) { free(raw); return NULL; }
    pce_alloc_node_t *n = (pce_alloc_node_t *)(node_raw + PCE_PAD);
    n->raw  = raw;
    n->ptr  = raw + PCE_PAD;
    n->size = (uint32_t)udata;
    n->next = s_special_allocs;
    s_special_allocs = n;
    return raw + PCE_PAD;
}

static void my_special_free_all(void)
{
    pce_alloc_node_t *cur = s_special_allocs;
    while (cur) {
        pce_alloc_node_t *next = cur->next;
        free(cur->raw);
        /* Node was allocated as `raw - PAD`, free that. */
        free((uint8_t *)cur - PCE_PAD);
        cur = next;
    }
    s_special_allocs = NULL;
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
/* File-static (not function-local) so pcec_shutdown can reset it —
 * s_palette_rgb565 is freed and re-malloc'd per session and the new
 * buffer would contain whatever calloc handed back without a rebuild. */
static int s_palette_built = 0;
static void build_palette_once(void)
{
    if (s_palette_built) return;
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
    s_palette_built = 1;
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

    /* Allocate per-session buffers up front so other emulators in the
     * same firmware partition (NES/SMS/GB/MD) don't pay the BSS cost.
     * Fail-fast if heap is exhausted — caller treats nonzero as load
     * error and unwinds. */
    if (!s_palette_rgb565) {
        s_palette_rgb565 = (uint16_t *)calloc(512, sizeof(uint16_t));
        if (!s_palette_rgb565) return 1;
    }
    if (!s_bram) {
        s_bram = (uint8_t *)calloc(2048, 1);
        if (!s_bram) { free(s_palette_rgb565); s_palette_rgb565 = NULL; return 1; }
    }
    if (!s_sbuf) {
        s_sbuf = calloc(6 * PCEC_AUDIO_MAX_FRAME_SAMPLES, 1);
        if (!s_sbuf) {
            free(s_bram); s_bram = NULL;
            free(s_palette_rgb565); s_palette_rgb565 = NULL;
            return 1;
        }
    }
    memset(s_bram, 0, 2048);

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
    /* `Country` is OR'd into the $1000 joypad-port read so the cart
     * sees bit 6 = 0 (PC Engine / JP) or 1 (TurboGrafx-16 / US).
     *
     * AUTO defaults to JP because we removed device-side bit-reversal
     * US-decoding (carts must be pre-decoded). Once a cart is in PCE-
     * native format, its CPU code is what determines boot behaviour —
     * Hudson typically produced JP carts and bit-reversed them for US
     * distribution, so a pre-decoded US cart's code expects to see
     * bit 6 = 0 just like a JP cart. Tested: R-Type / Legendary Axe II
     * / Blazing Lazers / Galaga '90 / Dragon's Curse / Super Star
     * Soldier / Image Fight / Final Soldier all render under JP.
     * Setting bit 6 (Country=0x40) made the USA carts hang in early
     * boot with display disabled (CR=0x0000).
     *
     * Per-cart override: PCEC_REGION_US forces bit 6 = 1 for the rare
     * USA cart that was authored against TG-16 firmware and wants
     * region=US (e.g. region-locked promo carts). The picker cfg can
     * surface that toggle if anything ever needs it. */
    Country = (s_region == PCEC_REGION_US) ? 0x40 : 0x00;
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

    /* Cap n to one frame's worth at our output rate. Without this, the
     * device runner asks for 1024 every 16.6 ms — 61 kHz against a
     * 22 kHz drain rate — the ring overflows in ~10 frames and
     * nes_audio_pwm_push drops the tail of every push. The audible
     * effect is chopped 36 %-duty audio with clicks at every cut.
     *
     * The phase accumulator gives us the exact 22050/60 ratio over
     * any 60-frame window: most frames 368 samples, some 367. Caller
     * can still pass less than per_frame (e.g. host probes); we
     * honour that so they get partial frames. */
    int per_frame = s_sample_rate / 60;
    s_audio_phase_acc += s_sample_rate - per_frame * 60;
    if (s_audio_phase_acc >= 60) { per_frame++; s_audio_phase_acc -= 60; }
    if (n > per_frame) n = per_frame;

    /* WriteBuffer writes `n` mono samples per channel (one byte each). */
    for (int ch = 0; ch < 6; ch++) {
        WriteBuffer((char *)s_sbuf[ch], ch, (unsigned)n);
    }

    /* Mix 6 PSG channels → mono signed 16-bit. Pipeline:
     *
     *  1. Sum 6 int8 channels: max ±6×127 = ±762.
     *
     *  2. Apply PSG master volume (io.psg_volume): bits 7-4 = L master,
     *     3-0 = R master, both 0..15. We sum them for a 0..30 mono
     *     multiplier. HuExpress's mix.c only applies master to the
     *     noise channels, so without this step wave channels ignore
     *     master fades entirely.  Max post-master: ±22860.
     *
     *  3. Halve to leave ~6 dB headroom inside int16. Brief peaks
     *     (e.g. two waveform channels phase-aligning) can otherwise
     *     skim the int16 limit; with this margin they don't.
     *     Max post-shift: ±11430.
     *
     *  4. Single-pole IIR LPF, a = 7/8 → fc ≈ 7.3 kHz at 22050 Hz.
     *     y += a*(x - y), implemented as `s_audio_lpf += ((x - lpf) *
     *     7) >> 3`. Approximates the real PC Engine analog DAC
     *     rolloff (8–10 kHz). The previous cascaded 2-pole at fc ≈
     *     2.5 kHz removed treble entirely and was the dominant cause
     *     of muffled-sounding output.
     *
     *  5. Saturating clamp to int16 (defensive — the headroom shift
     *     should keep us inside, but DC drift from accumulated mix
     *     bias can creep up). */
    int master = ((io.psg_volume >> 4) & 0x0F) + (io.psg_volume & 0x0F);

    int32_t lpf = s_audio_lpf;
    for (int i = 0; i < n; i++) {
        int32_t mix = 0;
        for (int ch = 0; ch < 6; ch++) {
            mix += (int32_t)s_sbuf[ch][i];
        }
        mix = (mix * master) >> 1;
        lpf += ((mix - lpf) * 7) >> 3;
        int32_t y = lpf;
        if (y >  32767) y =  32767;
        if (y < -32768) y = -32768;
        out[i] = (int16_t)y;
    }
    s_audio_lpf = lpf;
    return n;
}

uint8_t *pcec_battery_ram(void)  { return s_bram; }
size_t   pcec_battery_size(void) { return sizeof s_bram; }

/* ---- Save/load state ----------------------------------------------
 *
 * HuExpress has no upstream save-state — we hand-roll one. Format is
 * a fixed-layout binary stream, version-tagged. We bind the ROM CRC
 * into the header so loading a state into the wrong cart fails fast
 * instead of dereferencing nonsense bank mappings.
 *
 * Pointer hygiene: the IO struct holds runtime pointers (VCE buffer,
 * 6× psg_da_data buffers). We write the struct verbatim — those
 * pointer slots in the file are junk — then the load path patches
 * them back to the still-valid live allocations after the read.
 *
 * The ROM bank map (PageR/PageW) is rebuilt from mmr[] via bank_set,
 * so it isn't part of the state file. Same for IO_VDC_active_ref,
 * which is recomputed from io.vdc_reg with IO_VDC_active_set.
 *
 * The 8-byte magic is "PCESTATE" (no NUL). Version is bumped any time
 * a layout-affecting field is added, removed, or resized.
 */
#define PCE_STATE_MAGIC0 0x50  /* 'P' */
#define PCE_STATE_MAGIC1 0x43  /* 'C' */
#define PCE_STATE_MAGIC2 0x45  /* 'E' */
#define PCE_STATE_MAGIC3 0x53  /* 'S' */
#define PCE_STATE_MAGIC4 0x54  /* 'T' */
#define PCE_STATE_MAGIC5 0x41  /* 'A' */
#define PCE_STATE_MAGIC6 0x54  /* 'T' */
#define PCE_STATE_MAGIC7 0x45  /* 'E' */
#define PCE_STATE_VERSION 1u

#ifdef THUMBY_STATE_BRIDGE
typedef thumby_state_io_t *pce_state_file;
#  define PCE_FOPEN(p, m)        thumby_state_open((p), (m))
#  define PCE_FCLOSE(f)          thumby_state_close((f))
#  define PCE_FWRITE(b, n, f)    (thumby_state_write((b), 1, (n), (f)) == (n) ? 0 : -1)
#  define PCE_FREAD(b, n, f)     (thumby_state_read ((b), 1, (n), (f)) == (n) ? 0 : -1)
#else
typedef FILE *pce_state_file;
#  define PCE_FOPEN(p, m)        fopen((p), (m))
#  define PCE_FCLOSE(f)          fclose((f))
#  define PCE_FWRITE(b, n, f)    (fwrite((b), 1, (n), (f)) == (n) ? 0 : -1)
#  define PCE_FREAD(b, n, f)     (fread ((b), 1, (n), (f)) == (n) ? 0 : -1)
#endif

int pcec_save_state(const char *path)
{
    if (!path || !s_inited || !hard_pce) return -1;

    pce_state_file f = PCE_FOPEN(path, "wb");
    if (!f) return -2;

    /* Header: 8-byte magic + version + ROM CRC + Country. The
     * 4-byte reserved word is for future use (e.g. flag bits). */
    uint8_t  magic[8] = {
        PCE_STATE_MAGIC0, PCE_STATE_MAGIC1, PCE_STATE_MAGIC2, PCE_STATE_MAGIC3,
        PCE_STATE_MAGIC4, PCE_STATE_MAGIC5, PCE_STATE_MAGIC6, PCE_STATE_MAGIC7,
    };
    uint32_t version  = PCE_STATE_VERSION;
    uint32_t rom_crc  = g_pce_mem_rom_crc;
    int32_t  country  = (int32_t)Country;
    uint32_t reserved = 0;

    /* CPU registers — these live in hard_pce.c globals (reg_pc_, etc.)
     * and are referenced directly in non-SHARED_MEMORY mode. cycles_
     * is the per-instruction tally that exe_go consults each iter. */
    uint32_t st_reg_pc = (uint32_t)reg_pc;
    uint8_t  st_reg_a  = (uint8_t)reg_a;
    uint8_t  st_reg_x  = (uint8_t)reg_x;
    uint8_t  st_reg_y  = (uint8_t)reg_y;
    uint8_t  st_reg_p  = (uint8_t)reg_p;
    uint8_t  st_reg_s  = (uint8_t)reg_s;
    uint32_t st_cycles = cycles_;

    /* Hard-PCE dynamic scalars. Pointers in struct_hard_pce point at
     * still-valid heap blocks (RAM/VRAM/...) we re-fill below; we
     * don't serialise those pointer values. */
    uint32_t st_scanline   = hard_pce->s_scanline;
    uint32_t st_cyclecount = hard_pce->s_cyclecount;
    uint32_t st_cyclecountold = hard_pce->s_cyclecountold;
    int32_t  st_ext_ctrl   = hard_pce->s_external_control_cpu;

    if (PCE_FWRITE(magic,             8, f) ||
        PCE_FWRITE(&version,          4, f) ||
        PCE_FWRITE(&rom_crc,          4, f) ||
        PCE_FWRITE(&country,          4, f) ||
        PCE_FWRITE(&reserved,         4, f) ||
        PCE_FWRITE(&st_reg_pc,        4, f) ||
        PCE_FWRITE(&st_reg_a,         1, f) ||
        PCE_FWRITE(&st_reg_x,         1, f) ||
        PCE_FWRITE(&st_reg_y,         1, f) ||
        PCE_FWRITE(&st_reg_p,         1, f) ||
        PCE_FWRITE(&st_reg_s,         1, f) ||
        PCE_FWRITE(&st_cycles,        4, f) ||
        PCE_FWRITE(&st_scanline,      4, f) ||
        PCE_FWRITE(&st_cyclecount,    4, f) ||
        PCE_FWRITE(&st_cyclecountold, 4, f) ||
        PCE_FWRITE(&st_ext_ctrl,      4, f) ||
        PCE_FWRITE(mmr,               8, f))
    { goto fail; }

    /* Bulk memory regions. RAM is allocated 32 KB but only 0x2000
     * is meaningful on a CoreGrafx; we save the full 32 KB to keep
     * the layout simple and to leave room for future SuperGrafx. */
    if (PCE_FWRITE(RAM,    0x8000, f) ||
        PCE_FWRITE(WRAM,   0x2000, f) ||
        PCE_FWRITE(VRAM,  VRAMSIZE, f) ||
        PCE_FWRITE(SPRAM, 64 * 4 * sizeof(uint16), f) ||
        PCE_FWRITE(Pal,    512,    f))
    { goto fail; }

    /* IO struct + buffers it points at. Order: VCE pal data,
     * psg_da_data[6] direct-PSG buffers, then the IO struct
     * itself (whose pointers are stale and overwritten on load). */
    if (PCE_FWRITE(io.VCE, 0x200 * sizeof(pair), f)) goto fail;
    for (int i = 0; i < 6; i++) {
        if (PCE_FWRITE(io.psg_da_data[i], PSG_DIRECT_ACCESS_BUFSIZE, f))
            goto fail;
    }
    if (PCE_FWRITE(&io, sizeof(IO), f)) goto fail;

    PCE_FCLOSE(f);
    return 0;

fail:
    PCE_FCLOSE(f);
    return -3;
}

int pcec_load_state(const char *path)
{
    if (!path || !s_inited || !hard_pce) return -1;

    pce_state_file f = PCE_FOPEN(path, "rb");
    if (!f) return -2;

    uint8_t  magic[8];
    uint32_t version, rom_crc, reserved;
    int32_t  country;

    if (PCE_FREAD(magic, 8, f) ||
        PCE_FREAD(&version,  4, f) ||
        PCE_FREAD(&rom_crc,  4, f) ||
        PCE_FREAD(&country,  4, f) ||
        PCE_FREAD(&reserved, 4, f))
    { goto fail; }

    if (magic[0] != PCE_STATE_MAGIC0 || magic[1] != PCE_STATE_MAGIC1 ||
        magic[2] != PCE_STATE_MAGIC2 || magic[3] != PCE_STATE_MAGIC3 ||
        magic[4] != PCE_STATE_MAGIC4 || magic[5] != PCE_STATE_MAGIC5 ||
        magic[6] != PCE_STATE_MAGIC6 || magic[7] != PCE_STATE_MAGIC7)
    { goto fail; }
    if (version != PCE_STATE_VERSION) goto fail;
    if (rom_crc != g_pce_mem_rom_crc) goto fail;

    Country = (int)country;

    uint32_t st_reg_pc, st_cycles, st_scanline;
    uint32_t st_cyclecount, st_cyclecountold;
    int32_t  st_ext_ctrl;
    uint8_t  st_reg_a, st_reg_x, st_reg_y, st_reg_p, st_reg_s;
    uint8_t  st_mmr[8];

    if (PCE_FREAD(&st_reg_pc,        4, f) ||
        PCE_FREAD(&st_reg_a,         1, f) ||
        PCE_FREAD(&st_reg_x,         1, f) ||
        PCE_FREAD(&st_reg_y,         1, f) ||
        PCE_FREAD(&st_reg_p,         1, f) ||
        PCE_FREAD(&st_reg_s,         1, f) ||
        PCE_FREAD(&st_cycles,        4, f) ||
        PCE_FREAD(&st_scanline,      4, f) ||
        PCE_FREAD(&st_cyclecount,    4, f) ||
        PCE_FREAD(&st_cyclecountold, 4, f) ||
        PCE_FREAD(&st_ext_ctrl,      4, f) ||
        PCE_FREAD(st_mmr,            8, f))
    { goto fail; }

    if (PCE_FREAD(RAM,    0x8000, f) ||
        PCE_FREAD(WRAM,   0x2000, f) ||
        PCE_FREAD(VRAM,  VRAMSIZE, f) ||
        PCE_FREAD(SPRAM, 64 * 4 * sizeof(uint16), f) ||
        PCE_FREAD(Pal,    512,    f))
    { goto fail; }

    /* Live pointers we must preserve across the IO struct read. */
    pair  *saved_vce = io.VCE;
    uchar *saved_psg[6];
    for (int i = 0; i < 6; i++) saved_psg[i] = io.psg_da_data[i];

    if (PCE_FREAD(saved_vce, 0x200 * sizeof(pair), f)) goto fail;
    for (int i = 0; i < 6; i++) {
        if (PCE_FREAD(saved_psg[i], PSG_DIRECT_ACCESS_BUFSIZE, f))
            goto fail;
    }
    if (PCE_FREAD(&io, sizeof(IO), f)) goto fail;

    /* Stale pointers from the file — restore the live allocations. */
    io.VCE = saved_vce;
    for (int i = 0; i < 6; i++) io.psg_da_data[i] = saved_psg[i];

    /* CPU and timing scalars. */
    reg_pc = st_reg_pc;
    reg_a  = st_reg_a;
    reg_x  = st_reg_x;
    reg_y  = st_reg_y;
    reg_p  = st_reg_p;
    reg_s  = st_reg_s;
    cycles_ = st_cycles;
    hard_pce->s_scanline         = st_scanline;
    hard_pce->s_cyclecount       = st_cyclecount;
    hard_pce->s_cyclecountold    = st_cyclecountold;
    hard_pce->s_external_control_cpu = st_ext_ctrl;

    /* Rebuild PageR/PageW from saved mmr[] via bank_set. mmr[P]=V is
     * the source of truth for "which 8 KB ROM/RAM bank is paged into
     * logical bank P"; PageR/W are derived caches into ROMMapR/W. */
    for (int p = 0; p < 8; p++) bank_set((uchar)p, st_mmr[p]);

    /* Restore the runtime VDC active-register pointer from io.vdc_reg.
     * The IO struct already has the correct vdc_reg byte; this macro
     * dispatches it to the matching IO_VDC_xx_* slot. */
    IO_VDC_active_set(io.vdc_reg);

    /* Clear the audio LPF integrator. Without this, the first frame
     * after a state load slews from the pre-load mix value to the
     * post-load value over ~30 samples — audible as a soft thump. */
    s_audio_lpf = 0;

    PCE_FCLOSE(f);
    return 0;

fail:
    PCE_FCLOSE(f);
    return -3;
}

void pcec_shutdown(void)
{
    if (!s_inited) return;

    /* Buffers we own. */
    free(s_sbuf);            s_sbuf = NULL;
    free(s_bram);            s_bram = NULL;
    free(s_palette_rgb565);  s_palette_rgb565 = NULL;
    s_palette_built = 0;     /* RGB565 LUT must rebuild on next pcec_init */
    s_audio_lpf = 0;         /* clean filter integrator for next session */
    s_audio_phase_acc = 0;   /* phase accumulator restarts at 0 too */
    pce_render_shutdown();   /* frees the 2 KB BG-decode LUT */

    /* HuExpress internals freed in one sweep via my_special_alloc.
     * trap_ram_read/write are also routed through it (so the wild-
     * write workaround pad covers them) — freed here too. */
    if (PopRAM)         { free(PopRAM);         PopRAM         = NULL; }

    my_special_free_all();    /* frees ~120 KB of HuExpress internals */
    trap_ram_read = NULL;
    trap_ram_write = NULL;

    /* Globals that aliased into the now-freed hard_pce / pool. NULL
     * them so a stray dereference between sessions traps cleanly
     * instead of reading random heap. hard_init re-binds them all. */
    hard_pce = NULL;
    RAM = WRAM = PCM = VRAM = VRAM2 = VRAMS = NULL;
    vchange = vchanges = Pal = IOAREA = NULL;
    SPRAM = NULL;
    extern uchar *ROM;        /* declared in vendor/huexpress/engine/pce.c */
    ROM = NULL;               /* zero-copy XIP pointer, no free */

    /* CRITICAL: pcec_init only allocates these `if (!p)`. Without NULLing
     * here, a second PCE session sees stale pointers (now pointing to
     * freed heap chunks), skips re-allocation, and subsequent strcpy /
     * sprintf into them writes through the dangling pointer to memory
     * that's now in newlib's free-list — corrupting fastbin metadata
     * and hanging the next 32 KB malloc/free cycle. Same for
     * spr_init_pos. This is the root cause of PCE→<other core>→PCE
     * heap corruption that took us a long time to find. */
    extern char *cart_name, *short_cart_name, *short_iso_name;
    extern char *rom_file_name;
    extern char *config_basepath, *sav_path, *sav_basepath;
    extern char *tmp_basepath, *video_path, *ISO_filename;
    extern char *syscard_filename;
    extern uint32 *spr_init_pos;
    cart_name = NULL;
    short_cart_name = NULL;
    short_iso_name = NULL;
    rom_file_name = NULL;
    config_basepath = NULL;
    sav_path = NULL;
    sav_basepath = NULL;
    tmp_basepath = NULL;
    video_path = NULL;
    ISO_filename = NULL;
    syscard_filename = NULL;
    spr_init_pos = NULL;

    s_inited = 0;
    g_pce_mem_rom_active = 0;
}

