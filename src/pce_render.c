/*
 * ThumbyNES — PC Engine VDC scanline renderer.
 *
 * Replaces HuExpress's full-framebuffer sprite/tile pipeline with a
 * per-scanline line-buffer composite, mirroring the discipline of
 * src/md_core.c (PicoDrive's MD_LINE_SCRATCH path):
 *
 *   1. Render the current PCE source scanline into a small 8-bit
 *      palette-indexed line buffer (s_line). BG tiles go down via
 *      per-tile column iteration; sprites overlay via per-sprite
 *      16-pixel slices. Each sprite draws ONCE per scanline.
 *   2. Downsample the line buffer 2:1 horizontally and emit one row
 *      of RGB565 directly into the bound LCD fb. Blend mode averages
 *      with the previous source row in packed RGB565 space (same
 *      `exp565`/`pak565` trick as nes_run / gb_run / md_core).
 *
 * Memory cost per session:
 *   s_line       PCE_LINE_W bytes      256 — palette indices
 *   s_line_prev  PCE_LINE_W bytes      256 — previous line for blend
 *   s_spr        ~96 bytes               16 visible-sprite scan list
 *   ----------------------------------
 *   ~600 bytes total — replaces 568 KB of upstream render state.
 *
 * Hooked from gfx_Loop6502.h on each active scanline (PCE_SCANLINE_RENDER
 * compile flag); CPU/IRQ/audio paths stay stock.
 */

#define THUMBY_BUILD 1

#include "pce_core.h"
#include "pce_render.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cleantypes.h"
#include "pce.h"
#include "hard_pce.h"
#include "sprite.h"

/* On the RP2350 device, the per-scanline composers join the PCE
 * dynamic-IRAM pool — the device CMakeLists passes
 *   -DPCE_HOT_ATTR='__attribute__((section(".pce_iram_pool")))'
 * so the functions land in the .pce_iram_pool flash section, which
 * device/pce_iram.c memcopies to heap at pcec_init time. Host builds
 * leave PCE_HOT_ATTR undefined → the macro expands to nothing. */
#ifndef PCE_HOT_ATTR
#  define PCE_HOT_ATTR
#endif
#define PCE_HOT(name) PCE_HOT_ATTR name

extern uchar  *VRAM;
extern uchar  *Pal;
extern uint16 *SPRAM;
extern IO      io;

/* Under MY_VDC_VARS (set in myadd.h V3), the LIVE VDC register
 * values are these global `pair`s — io.VDC[N] is just a stale shadow.
 * IO_VDC_05_CR is the control register: bit 7 ScreenON, bit 6 SpriteON,
 * bit 3 VBL enable, bit 2 RasHit enable.
 *
 * sprite.h #defines ScrollX/ScrollY to IO_VDC_07_BXR.W / IO_VDC_08_BYR.W
 * (the LIVE values), and ScrollYDiff is the scanline at which BYR was
 * last written this frame (set in IO_write.h). The formula to convert
 * a display line to a BG y is
 *     y = Y1 + ScrollY - ScrollYDiff
 * which lets games like Final Soldier do mid-frame BYR raster effects
 * (logo zone / text zone / ground zone). */
extern pair IO_VDC_05_CR;
extern pair IO_VDC_06_RCR;
extern pair IO_VDC_07_BXR;
extern pair IO_VDC_08_BYR;
extern int  ScrollYDiff;

/* ---- Bound output state ------------------------------------------- */
static uint16_t       *s_lcd_fb      = NULL;
static const uint16_t *s_pal565      = NULL;
static int             s_blend       = 1;
static int             s_scale_mode  = 0;     /* 0 FIT, 1 FILL, 2 CROP */
static int             s_pan_x       = 0;
static int             s_pan_y       = 0;
/* Frame-locked source dimensions — captured at pce_render_set_target
 * time so mid-frame VDC mode flips don't desync the dst row mapping
 * (the renderer composites against a stable Y range per frame). */
static int             s_act_h       = 224;
static int             s_act_w       = 256;
/* FIT-mode letterbox geometry (pre-computed once per frame). */
static int             s_letterbox_y = 8;
static int             s_draw_h      = 112;
/* FILL vertical-blend tracking — when 1.75:1 reduction maps two src
 * rows to the same dst row, the second src row averages into the
 * first one's pixels rather than overwriting. */
static int             s_prev_dy_fill = -1;

#define PCE_LINE_W   384       /* covers the widest standard mode (336) + slack */

static uint8_t s_line[PCE_LINE_W];
static uint8_t s_line_prev[PCE_LINE_W];
static int     s_have_prev = 0;     /* set when the even row is fresh */

/* ---- Visible-sprite scan list (built per scanline) ---------------- */

typedef struct {
    int16_t  x;
    int16_t  y_top;
    uint16_t pattern;     /* base 16x16 cell index in VRAM/128 */
    uint16_t atr;         /* full SATB attribute word */
    uint8_t  cgx;         /* 0 = 16 wide, 1 = 32 wide */
    uint8_t  cgy;         /* 0..3 → 16/32/48/64 tall */
    uint8_t  pal;         /* sprite palette bank (atr & 0x0F) */
    uint8_t  _pad;
} spr_t;

#define MAX_SPR_PER_LINE  16        /* PCE hardware limit per scanline */
static spr_t s_spr[MAX_SPR_PER_LINE];
static int   s_spr_n;

/* ---- Public API --------------------------------------------------- */

static void spread8_init(void);

void pce_render_set_target(uint16_t *lcd_fb,
                            const uint16_t *palette_rgb565,
                            int scale_mode, int blend,
                            int pan_x, int pan_y)
{
    spread8_init();
    s_lcd_fb     = lcd_fb;
    s_pal565     = palette_rgb565;
    s_blend      = blend ? 1 : 0;
    s_scale_mode = scale_mode;
    s_pan_x      = pan_x;
    s_pan_y      = pan_y;

    int act_h = io.screen_h ? io.screen_h : 224;
    int act_w = io.screen_w ? io.screen_w : 256;
    if (act_w > PCE_LINE_W) act_w = PCE_LINE_W;
    s_act_h = act_h;
    s_act_w = act_w;

    /* FIT-mode geometry — 2:1 nearest/blend with symmetric letterbox.
     * Cap dst_h at 119 so 240-line mode leaves ≥5 px bottom letterbox,
     * masking any raster-effect bleed onto the last source rows
     * (e.g. Final Soldier's HUD/Earth band). 240→4+119+5, 224→8+112+8. */
    s_draw_h = act_h / 2;
    if (s_draw_h > 119) s_draw_h = 119;
    s_letterbox_y = (128 - s_draw_h) / 2;
    if (s_letterbox_y < 0) s_letterbox_y = 0;
}

void pce_render_frame_begin(void)
{
    if (!s_lcd_fb) return;
    /* Clear the WHOLE framebuffer (32 KB, ~30 µs at SRAM speed). The
     * active band is normally fully written by pce_render_scanline, but
     * games whose VDC active-display range is narrower than 240 lines
     * (or that haven't finished boot init) leave some rows unpainted.
     * Without this clear, those rows show whatever the previous frame
     * left, or — on first frame — whatever was on the stack/heap when
     * the host (pcehost) allocated the lcd_fb buffer. That's the
     * visible "noise" on screen for games that don't render the full
     * 240 PCE rows.
     *
     * Cost: 32 KB memset per frame. Negligible compared to the per-
     * scanline composer's work, and we get a deterministic black
     * background that immediately reveals which rows the renderer is
     * actually painting. */
    memset(s_lcd_fb, 0, 128 * 128 * 2);
    s_have_prev    = 0;
    s_prev_dy_fill = -1;
}

/* ---- BG line render ----------------------------------------------- */

/* Bit-spread LUT: spread8[b] is a 64-bit value where byte i (LSB-first)
 * holds bit (7-i) of b in its low bit.
 *
 *   spread8[0xA5]  -> 0x01 00 01 00 00 01 00 01   (LSBs spell 1 0 1 0 0 1 0 1)
 *
 * Built once at boot. Used to decode an 8-pixel BG tile row in 4 LUT
 * loads + 3 shift/OR ops, instead of looping 8 times with per-pixel
 * shift+AND. Each output byte gets the 4-bit color index in its low
 * nibble. ~3x faster than per-pixel decode.
 *
 * This is the same identity an emulator like Mednafen uses for fast
 * tile decode; the win is shifting the per-pixel work to once-per-row. */
/* Heap-allocated on first pce_render_set_target so the 2 KB only
 * lives during a PCE session. Other emulators in the same firmware
 * partition (NES/SMS/GB/MD) get the bytes back. */
static uint64_t *s_spread8;

static void spread8_init(void)
{
    if (s_spread8) return;
    s_spread8 = (uint64_t *)malloc(256 * sizeof(uint64_t));
    if (!s_spread8) return;     /* alloc failure handled by render early-out */
    for (int v = 0; v < 256; v++) {
        uint64_t out = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t bit = (uint64_t)((v >> (7 - i)) & 1);
            out |= bit << (i * 8);
        }
        s_spread8[v] = out;
    }
}

void pce_render_shutdown(void)
{
    free(s_spread8);
    s_spread8 = NULL;
}

/* Decode one 8-pixel BG tile row into a uint64_t holding 8 bytes,
 * each byte's low nibble = the 4-bit colour index of that pixel. */
static inline uint64_t PCE_HOT(decode_bg_row64)(const uchar *tile, int row)
{
    uint8_t b0 = tile[row * 2];
    uint8_t b1 = tile[row * 2 + 1];
    uint8_t b2 = tile[16 + row * 2];
    uint8_t b3 = tile[16 + row * 2 + 1];
    return  s_spread8[b0]
         | (s_spread8[b1] << 1)
         | (s_spread8[b2] << 2)
         | (s_spread8[b3] << 3);
}

/* Renders ONLY non-transparent BG pixels into s_line. The caller
 * pre-fills s_line with backdrop (and any back-priority sprites)
 * before calling, so transparent BG pixels (color index 0) leave
 * whatever is underneath visible. Mirrors upstream's RefreshLine
 * which only writes pixels guarded by `if (J & 0xnn)`. */
static void PCE_HOT(render_bg_line)(int pce_y, int screen_w)
{
    /* Screen off → caller's pre-fill is correct (backdrop / back
     * sprites only); nothing to add. */
    if (!(IO_VDC_05_CR.W & 0x80)) return;
    int bg_h_px = io.bg_h * 8;
    int bg_w_px = io.bg_w * 8;
    if (bg_h_px <= 0 || bg_w_px <= 0) return;
    /* y = Y1 + ScrollY - ScrollYDiff (upstream sprite_RefreshLine.h
     * line 20). pce_y == display_counter == Y1. */
    int wy = (pce_y + IO_VDC_08_BYR.W - ScrollYDiff) & (bg_h_px - 1);
    int row = wy & 7;
    const uint16 *bat_row = ((const uint16 *)VRAM)
                            + (wy >> 3) * io.bg_w;
    int bg_w_mask = io.bg_w - 1;
    int sx     = IO_VDC_07_BXR.W & (bg_w_px - 1);
    int tx     = sx >> 3;
    int subx   = sx & 7;
    int dx     = 0;

    while (dx < screen_w) {
        uint16 entry = bat_row[tx & bg_w_mask];
        const uchar *tile = VRAM + ((entry & 0x7FF) * 32);

        /* Zero-tile fast path: if all 4 plane bytes for this row are
         * zero, the entire 8-pixel row is transparent — leave s_line
         * untouched so back sprites (or backdrop) show through.
         * Common case in PCE backgrounds (sky, stars, void tiles). */
        uint32_t plane01 = ((const uint16 *)tile)[row];
        uint32_t plane23 = ((const uint16 *)tile)[8 + row];
        int end = subx + (screen_w - dx);
        if (end > 8) end = 8;
        if ((plane01 | plane23) == 0) {
            dx   += end - subx;
            subx  = 0;
            tx++;
            continue;
        }

        const uchar *bank = Pal + (entry >> 12) * 16;
        uint64_t pix64 = decode_bg_row64(tile, row);
        /* Write only opaque BG pixels (palette index != 0). Pixels
         * where p == 0 are transparent — leave s_line as the caller
         * left it (backdrop or back sprite). Branch is highly
         * predictable: most opaque tiles are mostly opaque. */
        for (int c = subx; c < end; c++) {
            uint8_t p = (uint8_t)((pix64 >> (c * 8)) & 0xF);
            if (p) s_line[dx] = bank[p];
            dx++;
        }
        subx = 0;
        tx++;
    }
}

/* ---- Sprite scan + per-line draw ---------------------------------- */

#ifdef PCEC_SPRITE_SCAN_TRACE
extern uint32_t pcec_scan_picked[256];   /* sprites picked, indexed by pce_y */
extern uint64_t pcec_scan_frame;
#endif
static void PCE_HOT(scan_sprites)(int pce_y, int screen_w)
{
    s_spr_n = 0;
    if (!(IO_VDC_05_CR.W & 0x40)) return;
    if (!SPRAM) return;
    /* SPRAM is 64 sprites × 4 uint16. Hardware scans index 0..63 in
     * order and keeps the FIRST 16 visible per line; later (high)
     * indices get dropped. We then draw in reverse so low indices
     * overwrite high ones — matching hardware front-to-back priority. */
    const uint16 *s = SPRAM;
    for (int n = 0; n < 64 && s_spr_n < MAX_SPR_PER_LINE; n++) {
        int atr = s[n*4 + 3];
        int cgy = (atr >> 12) & 3;
        cgy |= cgy >> 1;
        int h   = (cgy + 1) * 16;
        int y   = (int)(s[n*4 + 0] & 0x3FF) - 64;
        if (pce_y < y || pce_y >= y + h) continue;

        int cgx = (atr >> 8) & 1;
        int w   = (cgx + 1) * 16;
        int x   = (int)(s[n*4 + 1] & 0x3FF) - 32;
        if (x + w <= 0 || x >= screen_w) continue;
#ifdef PCEC_SPRITE_SCAN_TRACE
        /* Per-line picked-sprite log for diagnosis. Emits one line per
         * accepted sprite ONLY at the animal-row scanlines we care
         * about. Limited to once per ~120 frames so output isn't a
         * flood. */
        if ((pce_y == 192 || pce_y == 200 || pce_y == 210)
            && (pcec_scan_frame % 120) == 60) {
            fprintf(stderr, "  scan@frame%llu y=%d picked n=%d "
                            "y(raw)=%d x=%d patt=%04X size=%dx%d\n",
                    (unsigned long long)pcec_scan_frame, pce_y, n,
                    y, x, (unsigned)s[n*4 + 2], (cgx+1)*16, (cgy+1)*16);
        }
#endif

        spr_t *o = &s_spr[s_spr_n++];
        o->x       = (int16_t)x;
        o->y_top   = (int16_t)y;
        int no     = (s[n*4 + 2] & 0x7FF) >> 1;
        no        &= ~(cgy * 2 + cgx);
        o->pattern = (uint16_t)no;
        o->atr     = (uint16_t)atr;
        o->cgx     = (uint8_t)cgx;
        o->cgy     = (uint8_t)cgy;
        o->pal     = (uint8_t)(atr & 0x0F);
    }
}

/* PCE sprite cell layout (128 bytes per 16×16 cell), matching the
 * conventions in sprite.c::PutSprite and sprite_ops_define.h::sp2pixel:
 *
 *   offsets  0..31 : plane 0   (16 rows × 2 bytes/row)
 *   offsets 32..63 : plane 1
 *   offsets 64..95 : plane 2
 *   offsets 96..127: plane 3
 *
 * Within each plane row the TWO bytes are split as:
 *   byte 0 (offset row*2 + 0) → cols 8..15  (bit 7 = col 8, bit 0 = col 15)
 *   byte 1 (offset row*2 + 1) → cols  0..7  (bit 7 = col 0, bit 0 = col 7)
 *
 * Yes, the "high columns first" order is unusual but it's what
 * upstream's bit→pixel mapping in PutSprite implies (bit 0x8000 of
 * the combined J → P[0] = col 0). */
static inline uint8_t sprite_pixel(const uchar *cell, int col, int row)
{
    int half  = (col < 8) ? 1 : 0;            /* 1 = "low cols" half (byte 1) */
    int shift = (col < 8) ? (7 - col) : (15 - col);
    int p_row = row * 2 + half;
    return (uint8_t)(
          ((cell[       p_row] >> shift) & 1)
        | (((cell[ 32 + p_row] >> shift) & 1) << 1)
        | (((cell[ 64 + p_row] >> shift) & 1) << 2)
        | (((cell[ 96 + p_row] >> shift) & 1) << 3));
}

/* Draws the back-priority pass (draw_front=0) or the front-priority
 * pass (draw_front=1) of the per-line sprite list. The priority bit
 * is atr bit 7 (PCE SATB attribute word) — matches upstream's
 * `spbg = (atr >> 7) & 1` test in sprite_RefreshSpriteExact.h. */
static void PCE_HOT(draw_sprites_line)(int pce_y, int screen_w, int draw_front)
{
    /* Walk in REVERSE iteration order from scan_sprites — that built
     * the list with index 63 first, so iterate forward here so low
     * sprite indices end up on top. */
    for (int i = s_spr_n - 1; i >= 0; i--) {
        spr_t *s = &s_spr[i];
        int atr = s->atr;
        int sprite_front = (atr & 0x80) ? 1 : 0;
        if (sprite_front != draw_front) continue;
        int h   = (s->cgy + 1) * 16;
        int local_y = pce_y - s->y_top;
        if (atr & 0x8000) local_y = h - 1 - local_y;     /* V flip */
        int cell_y    = local_y >> 4;
        int row       = local_y & 15;
        int hflip     = (atr & 0x0800) ? 1 : 0;
        const uchar  *spr_pal = Pal + 256 + s->pal * 16;

        for (int xc = 0; xc < (s->cgx + 1); xc++) {
            /* Hflip on a 32-wide sprite swaps the LEFT/RIGHT cell
             * reads as well as flipping each cell internally — without
             * this swap the result is two independently-mirrored
             * halves, which renders as the correct sprite in two
             * pieces in the wrong order. */
            int src_xc = hflip ? (s->cgx - xc) : xc;
            /* PCE pattern memory uses a fixed 2-cell-per-cell-row
             * stride regardless of sprite width — a 16x32 sprite at
             * pattern N occupies cells N and N+2, NOT N and N+1
             * (cell N+1 is hardware-reserved/empty). Upstream's
             * sprite_RefreshSpriteExact.h confirms this with a hard-
             * coded `C += h*inc + 16*7*inc = 256` advance per cell
             * row, plus the alignment mask `~(cgy*2 + cgx)` that
             * reserves (cgy+1)*2 pattern slots. */
            int no = s->pattern + cell_y * 2 + src_xc;
            const uchar *cell = VRAM + (no * 128);
            int dx0 = s->x + xc * 16;
            for (int c = 0; c < 16; c++) {
                int dx = dx0 + (hflip ? (15 - c) : c);
                if (dx < 0 || dx >= screen_w) continue;
#ifdef PCEC_SPRITE_BLOCK_TEST
                /* TEMP: draw every sprite as a solid block — palette-bank
                 * index 1 (a guaranteed non-zero) so we see WHERE sprites
                 * are placed regardless of pixel decode. */
                s_line[dx] = spr_pal[1];
#else
                uint8_t p = sprite_pixel(cell, c, row);
                if (p) s_line[dx] = spr_pal[p];
#endif
            }
        }
    }
}

/* ---- Downsample emit ---------------------------------------------- */

static inline uint32_t exp565(uint16_t p)
{
    uint32_t x = p;
    return (x | (x << 16)) & 0x07E0F81Fu;
}
static inline uint16_t pak565(uint32_t c)
{
    return (uint16_t)((c | (c >> 16)) & 0xFFFFu);
}

/* ---- emit_row: per-mode line dispatch -----------------------------
 *
 * One source scanline (`s_line`, screen_w pixels of palette index)
 * has been composited; turn it into output rows of the 128×128 LCD
 * fb according to the bound scale_mode. Mirrors md_core_scan_end.
 *
 *   FIT  — 2 src rows → 1 dst row; even row stashes, odd row emits
 *          (blend averages the 2×2 quad through the palette LUT).
 *          Letterboxed Y: dst_h = act_h/2 capped at 119.
 *   FILL — preserve aspect via Y scale 128/act_h applied to both
 *          axes. For act_h=224 this gives ~1.75:1 reduction → some
 *          dst rows get 2 src lines (blended) and some get 1.
 *          Visible X window is `act_h` cols wide centred in act_w
 *          (LB+dpad pans s_pan_x within [0, act_w-act_h]).
 *   CROP — 1:1 native 128×128 window into src (pan_x/pan_y).
 *
 * pce_y     — current PCE source scanline (0 .. act_h-1).
 * screen_w  — width of the rendered source line (typically 256, may
 *             differ from s_act_w if the game flipped VDC mode mid-
 *             frame; we still index s_line within screen_w bounds).
 */
static void PCE_HOT(emit_fill)(int pce_y, int screen_w)
{
    int act_h = s_act_h;
    if (act_h <= 0) return;

    /* Visible source-X span — keep aspect square by matching Y scale.
     * If act_w < act_h (rare wide-narrow screens), clamp to act_w.
     * Crop offset (s_pan_x) selects which slice within act_w to show. */
    int visible_w = act_h;
    if (visible_w > s_act_w) visible_w = s_act_w;
    int crop_max = s_act_w - visible_w; if (crop_max < 0) crop_max = 0;
    int crop = s_pan_x;
    if (crop < 0) crop = 0;
    if (crop > crop_max) crop = crop_max;

    int dy = (pce_y * 128) / act_h;
    if (dy < 0 || dy >= 128) return;
    int blend_with_prev = (dy == s_prev_dy_fill);
    s_prev_dy_fill = dy;

    uint16_t *drow = s_lcd_fb + dy * 128;
    int step = (visible_w << 8) / 128;          /* 8.8 fixed */
    int sx0  = crop << 8;

    if (!s_blend) {
        /* Drop the second src line for a given dst row — keep first. */
        if (blend_with_prev) return;
        int sx = sx0;
        for (int dx = 0; dx < 128; dx++) {
            int ix = sx >> 8;
            if (ix >= screen_w) ix = screen_w - 1;
            drow[dx] = s_pal565[s_line[ix]];
            sx += step;
        }
        return;
    }

    /* Blend path: average 2 adjacent src cols, then optionally average
     * with whatever's already in this dst row (= the previous src
     * line for this same dy). GB-core packed-RGB565 trick. */
    const uint32_t MASK = 0x07E0F81Fu;
    int sx = sx0;
    for (int dx = 0; dx < 128; dx++) {
        int ix0 = sx >> 8;
        int ix1 = ix0 + 1;
        if (ix0 >= screen_w) ix0 = screen_w - 1;
        if (ix1 >= screen_w) ix1 = screen_w - 1;
        uint16_t a = s_pal565[s_line[ix0]];
        uint16_t b = s_pal565[s_line[ix1]];
        uint32_t ea = ((uint32_t)a | ((uint32_t)a << 16)) & MASK;
        uint32_t eb = ((uint32_t)b | ((uint32_t)b << 16)) & MASK;
        uint32_t avg = ((ea + eb) >> 1) & MASK;
        if (blend_with_prev) {
            uint16_t old_px = drow[dx];
            uint32_t eo = ((uint32_t)old_px | ((uint32_t)old_px << 16)) & MASK;
            avg = ((avg + eo) >> 1) & MASK;
        }
        drow[dx] = (uint16_t)((avg | (avg >> 16)) & 0xFFFFu);
        sx += step;
    }
}

static void PCE_HOT(emit_crop)(int pce_y, int screen_w)
{
    int act_h = s_act_h;

    int copy_h = act_h     < 128 ? act_h     : 128;
    int copy_w = screen_w  < 128 ? screen_w  : 128;
    int pmax_x = s_act_w - copy_w; if (pmax_x < 0) pmax_x = 0;
    int pmax_y = act_h    - copy_h; if (pmax_y < 0) pmax_y = 0;
    int px = s_pan_x; if (px < 0) px = 0; if (px > pmax_x) px = pmax_x;
    int py = s_pan_y; if (py < 0) py = 0; if (py > pmax_y) py = pmax_y;

    int dy = pce_y - py;
    if (dy < 0 || dy >= copy_h) return;

    int dst_x = (128 - copy_w) / 2;
    int dst_y = (128 - copy_h) / 2 + dy;
    uint16_t *drow = s_lcd_fb + dst_y * 128 + dst_x;

    /* Clamp the source window so we never read past s_line[screen_w]. */
    int last_src = px + copy_w;
    if (last_src > screen_w) {
        int over = last_src - screen_w;
        copy_w -= over;
        if (copy_w <= 0) return;
    }
    for (int dx = 0; dx < copy_w; dx++) {
        drow[dx] = s_pal565[s_line[px + dx]];
    }
}

static void PCE_HOT(emit_fit)(int pce_y, int screen_w)
{
    int dst_y = (pce_y >> 1) + s_letterbox_y;
    if (dst_y < 0 || dst_y >= 128) return;
    /* Don't render into the bottom letterbox even if act_h's source
     * range spills past s_draw_h (240-line mode after the cap). */
    if (dst_y >= s_letterbox_y + s_draw_h) return;
    uint16_t *drow = s_lcd_fb + dst_y * 128;
    int step = (screen_w << 8) / 128;          /* 8.8 fixed */

    if (!s_blend) {
        if (pce_y & 1) return;                  /* drop odd rows */
        int sx = 0;
        for (int dx = 0; dx < 128; dx++) {
            int ix = sx >> 8;
            if (ix >= screen_w) ix = screen_w - 1;
            drow[dx] = s_pal565[s_line[ix]];
            sx += step;
        }
        return;
    }

    /* Blend path: even row stashes; odd row averages with stash. */
    if ((pce_y & 1) == 0) {
        memcpy(s_line_prev, s_line, (size_t)screen_w);
        s_have_prev = 1;
        return;
    }
    if (!s_have_prev) {
        /* First active line is odd (rare); fall back to nearest. */
        int sx = 0;
        for (int dx = 0; dx < 128; dx++) {
            int ix = sx >> 8;
            if (ix >= screen_w) ix = screen_w - 1;
            drow[dx] = s_pal565[s_line[ix]];
            sx += step;
        }
        return;
    }
    int sx = 0;
    for (int dx = 0; dx < 128; dx++) {
        int ix0 = sx >> 8;
        int ix1 = ix0 + 1;
        if (ix0 >= screen_w) ix0 = screen_w - 1;
        if (ix1 >= screen_w) ix1 = screen_w - 1;
        uint32_t A = exp565(s_pal565[s_line_prev[ix0]]);
        uint32_t B = exp565(s_pal565[s_line_prev[ix1]]);
        uint32_t C = exp565(s_pal565[s_line     [ix0]]);
        uint32_t D = exp565(s_pal565[s_line     [ix1]]);
        uint32_t avg = ((A + B + C + D) >> 2) & 0x07E0F81Fu;
        drow[dx] = pak565(avg);
        sx += step;
    }
    s_have_prev = 0;
}

static void PCE_HOT(emit_row)(int pce_y, int screen_w)
{
    switch (s_scale_mode) {
    case 1:  emit_fill(pce_y, screen_w); break;
    case 2:  emit_crop(pce_y, screen_w); break;
    default: emit_fit (pce_y, screen_w); break;
    }
}

/* ---- Public per-scanline entry ------------------------------------ */

void PCE_HOT(pce_render_scanline)(int pce_y)
{
    if (!s_lcd_fb || !s_pal565 || !s_spread8) return;
    if (pce_y < 0) return;
    int act_h = io.screen_h ? io.screen_h : 224;
    if (pce_y >= act_h) return;

    int screen_w = io.screen_w ? io.screen_w : 256;
    if (screen_w > PCE_LINE_W) screen_w = PCE_LINE_W;

    /* PCE sprite-vs-BG priority: each sprite carries a priority bit
     * (atr & 0x80). 0 = "behind BG" — drawn before BG so opaque BG
     * pixels hide it. 1 = "in front of BG" — drawn after BG.
     *
     * Order mirrors HuExpress's gfx_render_lines.h:
     *   1. Pre-fill backdrop into s_line.
     *   2. Back-priority sprites (drawn over backdrop only).
     *   3. BG opaque pixels — leave transparent BG pixels untouched
     *      so back sprites show through tile holes.
     *   4. Front-priority sprites — on top of everything.
     *
     * Without this two-pass split, every sprite renders as if it had
     * priority=1 — which broke title-screen layouts where the cart
     * relies on opaque BG (menu text) hiding background-layer sprites
     * (e.g. an idle ship). */
    uint8_t bg = Pal ? Pal[0] : 0;
    memset(s_line, bg, (size_t)screen_w);

    scan_sprites(pce_y, screen_w);
#ifdef PCEC_SPRITE_SCAN_TRACE
    if ((pce_y == 192 || pce_y == 200 || pce_y == 210)
        && (pcec_scan_frame % 120) == 60 && s_spr_n == 0) {
        fprintf(stderr, "  scan@frame%llu y=%d: NO sprites picked\n",
                (unsigned long long)pcec_scan_frame, pce_y);
    }
#endif
    if (s_spr_n) draw_sprites_line(pce_y, screen_w, /*draw_front=*/0);
    render_bg_line(pce_y, screen_w);
    if (s_spr_n) draw_sprites_line(pce_y, screen_w, /*draw_front=*/1);

    emit_row(pce_y, screen_w);
}
