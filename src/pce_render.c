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
static int             s_letterbox_y = 8;
static int             s_draw_h      = 112;

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

void pce_render_set_target(uint16_t *lcd_fb,
                            const uint16_t *palette_rgb565, int blend)
{
    s_lcd_fb = lcd_fb;
    s_pal565 = palette_rgb565;
    s_blend  = blend ? 1 : 0;
    int act_h = io.screen_h ? io.screen_h : 224;
    s_draw_h  = act_h / 2;
    if (s_draw_h > 128) s_draw_h = 128;
    s_letterbox_y = (128 - s_draw_h) / 2;
    if (s_letterbox_y < 0) s_letterbox_y = 0;
}

void pce_render_frame_begin(void)
{
    if (!s_lcd_fb) return;
    /* Clear letterbox bars once. The active band gets fully written
     * row-by-row so we don't need to clear it. */
    if (s_letterbox_y > 0) {
        memset(s_lcd_fb, 0, (size_t)s_letterbox_y * 128 * 2);
        memset(s_lcd_fb + (s_letterbox_y + s_draw_h) * 128, 0,
               (size_t)(128 - s_letterbox_y - s_draw_h) * 128 * 2);
    }
    s_have_prev = 0;
}

/* ---- BG line render ----------------------------------------------- */

/* PCE tile pixel decode: given a tile (32 bytes in VRAM) and an (x,y)
 * within it, return the 4-bit colour index. */
static inline uint8_t tile_pixel(const uchar *tile, int col, int row)
{
    int shift = 7 - (col & 7);
    const uchar *p01 = tile + row * 2;
    const uchar *p23 = tile + 16 + row * 2;
    return (uint8_t)(
          ((p01[0] >> shift) & 1)
        | (((p01[1] >> shift) & 1) << 1)
        | (((p23[0] >> shift) & 1) << 2)
        | (((p23[1] >> shift) & 1) << 3));
}

static void render_bg_line(int pce_y, int screen_w)
{
    /* Screen off → line is the backdrop colour. */
    uint8_t bg = Pal ? Pal[0] : 0;
    if (!(IO_VDC_05_CR.W & 0x80)) {
        memset(s_line, bg, screen_w);
        return;
    }
    int bg_h_px = io.bg_h * 8;
    int bg_w_px = io.bg_w * 8;
    if (bg_h_px <= 0 || bg_w_px <= 0) {
        memset(s_line, bg, screen_w);
        return;
    }
    /* y = Y1 + ScrollY - ScrollYDiff (upstream sprite_RefreshLine.h
     * line 20). pce_y == display_counter == Y1. */
    int wy = (pce_y + IO_VDC_08_BYR.W - ScrollYDiff) & (bg_h_px - 1);
    int row = wy & 7;
    const uint16 *bat_row = ((const uint16 *)VRAM)
                            + (wy >> 3) * io.bg_w;
    int sx     = IO_VDC_07_BXR.W & (bg_w_px - 1);
    int tx     = sx >> 3;
    int subx   = sx & 7;
    int dx     = 0;

    while (dx < screen_w) {
        uint16 entry = bat_row[tx & (io.bg_w - 1)];
        const uchar *bank = Pal + (entry >> 12) * 16;
        const uchar *tile = VRAM + ((entry & 0x7FF) * 32);
        int end = subx + (screen_w - dx);
        if (end > 8) end = 8;
        for (int c = subx; c < end; c++) {
            uint8_t p = tile_pixel(tile, c, row);
            /* Store packed-RGB byte (Pal[bank*16+p]) — matches the
             * upstream renderer's buffer semantics so the 256-entry
             * RGB565 LUT in pce_core.c can decode any pixel. */
            s_line[dx++] = p ? bank[p] : bg;
        }
        subx = 0;
        tx++;
    }
}

/* ---- Sprite scan + per-line draw ---------------------------------- */

static void scan_sprites(int pce_y, int screen_w)
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

static void draw_sprites_line(int pce_y, int screen_w)
{
    /* Walk in REVERSE iteration order from scan_sprites — that built
     * the list with index 63 first, so iterate forward here so low
     * sprite indices end up on top. */
    for (int i = s_spr_n - 1; i >= 0; i--) {
        spr_t *s = &s_spr[i];
        int atr = s->atr;
        int h   = (s->cgy + 1) * 16;
        int w   = (s->cgx + 1) * 16;
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
            int no = s->pattern + cell_y * (s->cgx + 1) + src_xc;
            const uchar *cell = VRAM + (no * 128);
            int dx0 = s->x + xc * 16;
            for (int c = 0; c < 16; c++) {
                int dx = dx0 + (hflip ? (15 - c) : c);
                if (dx < 0 || dx >= screen_w) continue;
                uint8_t p = sprite_pixel(cell, c, row);
                if (p) s_line[dx] = spr_pal[p];
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

static void emit_row(int pce_y, int screen_w)
{
    int dst_y = (pce_y >> 1) + s_letterbox_y;
    if (dst_y < 0 || dst_y >= 128) return;
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

/* ---- Public per-scanline entry ------------------------------------ */

void pce_render_scanline(int pce_y)
{
    if (!s_lcd_fb || !s_pal565) return;
    if (pce_y < 0) return;
    int act_h = io.screen_h ? io.screen_h : 224;
    if (pce_y >= act_h) return;

    int screen_w = io.screen_w ? io.screen_w : 256;
    if (screen_w > PCE_LINE_W) screen_w = PCE_LINE_W;

    render_bg_line(pce_y, screen_w);
    scan_sprites(pce_y, screen_w);
    if (s_spr_n) draw_sprites_line(pce_y, screen_w);
    emit_row(pce_y, screen_w);
}
