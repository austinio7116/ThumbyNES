/*
 * ThumbyNES — PC Engine scanline renderer API.
 *
 * Used from pce_core.c's frame loop and from Loop6502's per-scanline
 * hook (gfx_Loop6502.h under PCE_SCANLINE_RENDER). Writes directly
 * into the LCD framebuffer the wrapper passes in via
 * pcec_set_scale_target → pce_render_set_target.
 */
#pragma once

#include <stdint.h>

/* Bind the output LCD framebuffer + palette LUT + blend mode for
 * the upcoming frame(s). Safe to call each frame (the wrapper does).
 *   lcd_fb:           128×128 RGB565 output.
 *   palette_rgb565:   256-entry LUT keyed on the palette-byte encoding
 *                     HuExpress stores in Pal[] (bits [7:5]=G, [4:2]=R,
 *                     [1:0]=B — see pce_core.c::build_palette_once).
 *   blend:            0 = nearest-neighbour 2:1 downscale, 1 = 2×2
 *                     box average in packed RGB565.
 */
void pce_render_set_target(uint16_t *lcd_fb,
                            const uint16_t *palette_rgb565,
                            int blend);

/* Call once at the start of each frame — paints the letterbox bars
 * on the LCD fb and resets the vertical-blend carry state. */
void pce_render_frame_begin(void);

/* Render one PCE source scanline (0 .. io.screen_h-1) directly into
 * the bound LCD framebuffer, compositing BG tiles and sprites from
 * VRAM / SPRAM live — no XBUF, no SPM, no shadow VRAMs. */
void pce_render_scanline(int pce_y);

/* Free the per-session scratch (BG-decode spread LUT). Called from
 * pcec_shutdown so the 2 KB returns to the heap when other emulators
 * in the same firmware partition take over. */
void pce_render_shutdown(void);
