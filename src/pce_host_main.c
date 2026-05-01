/*
 * ThumbyNES SDL2 host runner — PC Engine / TurboGrafx-16 edition.
 *
 * Mirror of md_host_main.c. Boots HuExpress, opens an SDL2 window
 * scaled to the active viewport, pumps input → pcec_set_buttons,
 * drives the PSG mixer → SDL audio.
 *
 * Keybindings (consistent with the MD/GB host runners):
 *   Arrows    — D-pad
 *   Z         — I (A)
 *   X         — II (B)
 *   Shift     — SELECT
 *   Enter     — RUN
 *   Escape    — quit
 *
 * TODO(pcehost): wire SDL audio once pcec_audio_pull produces samples.
 * TODO(pcehost): on-host PNG screenshot helper parity with mdhost.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pce_core.h"

#ifdef PCEC_DEBUG_TRACE
/* Per-PC visit histogram + last-seen MPR slot. The vendored h6280
 * dispatch loop bumps these on every opcode under PCEC_DEBUG_TRACE.
 * Total: 64K * 4 + 64K * 1 = 320 KB BSS. Host-only — never compiled
 * into the device build. */
uint32_t pcec_pc_hist[65536];
uint8_t  pcec_mpr_at_pc[65536];
uint16_t pcec_pc_ring[65536];
uint32_t pcec_pc_ring_idx;
/* IRQ delivery counters: [NMI, TIMER, IRQ1/VDC, IRQ2/CD]. */
uint32_t pcec_irq_called[4];
uint32_t pcec_irq_delivered[4];
/* VRAM-write counters: total + writes targeted to the $6800-$7400
 * range (where Bonk's Revenge's missing animal sprite data should
 * land). Comparing total vs target tells us if the cart is writing
 * but to the wrong place, or not writing at all. */
uint32_t pcec_vwr_writes_total;
uint32_t pcec_vwr_writes_target;
uint32_t pcec_vwr_target_zero;
uint32_t pcec_vwr_target_nonzero;
uint32_t pcec_dma_writes_total;
uint32_t pcec_dma_writes_target;
uint64_t pcec_scan_frame;
uint32_t pcec_scan_picked[256];

static int pc_cmp(const void *a, const void *b) {
    uint32_t pa = pcec_pc_hist[*(const uint16_t*)a];
    uint32_t pb = pcec_pc_hist[*(const uint16_t*)b];
    if (pa < pb) return  1;
    if (pa > pb) return -1;
    return 0;
}

static void dump_pc_hist(void) {
    uint64_t total = 0;
    for (int i = 0; i < 65536; i++) total += pcec_pc_hist[i];
    if (total == 0) { fprintf(stderr, "[trace] no PCs sampled\n"); return; }
    /* Build a sortable index of PCs that were ever hit. */
    uint16_t *idx = (uint16_t *)malloc(65536 * sizeof(uint16_t));
    int n = 0;
    for (int i = 0; i < 65536; i++) if (pcec_pc_hist[i]) idx[n++] = (uint16_t)i;
    qsort(idx, n, sizeof(uint16_t), pc_cmp);
    int top = n < 30 ? n : 30;
    fprintf(stderr, "[trace] top %d PCs (total opcodes = %llu):\n",
            top, (unsigned long long)total);
    for (int i = 0; i < top; i++) {
        uint16_t pc = idx[i];
        uint32_t hits = pcec_pc_hist[pc];
        double pct = (100.0 * hits) / (double)total;
        fprintf(stderr, "  %2d: PC=$%04X bank=$%02X (slot=%u)  %10u  (%.2f%%)\n",
                i, pc, pcec_mpr_at_pc[pc],
                (unsigned)((pc >> 13) & 7), hits, pct);
    }
    /* Dump bytes around the hottest PC via the LIVE memory map
     * (PageR). PageR[P] is biased by -P * 0x2000 in bank_set() so
     * that `PageR[slot][full_pc]` resolves to ROMMapR[bank][pc & 0x1FFF].
     * Index with the full PC, not pc_in_slot — earlier version had the
     * latter, which read uninitialised heap before ROM (slot 7's
     * bias is -0xE000, so page[$011E] = ROM-base + $011E - $E000). */
    if (n >= 1) {
        uint16_t pc = idx[0];
        uint8_t bank = pcec_mpr_at_pc[pc];
        unsigned slot = (pc >> 13) & 7;
        /* hard_pce.h declares this as `uchar **` (the storage is a
         * heap-allocated 8-pointer array, indirected). Get the type
         * right or the compiler reads adjacent symbol storage. */
        extern unsigned char **PageR;
        unsigned char *page = PageR[slot];
        uint32_t start = pc > 8 ? (uint32_t)pc - 8 : 0;
        fprintf(stderr, "[trace] bytes via PageR[%u] (recorded MPR bank "
                        "$%02X) around hot PC $%04X:\n",
                slot, bank, pc);
        fprintf(stderr, "  ");
        for (int k = 0; k < 32; k++) {
            uint32_t off = start + k;
            if (off >= 0x10000) break;
            fprintf(stderr, "%02X ", page[off]);
            if (off == pc) fprintf(stderr, "<-- PC ");
        }
        fprintf(stderr, "\n");
        /* Also dump live mmr[] state so we know what banks each
         * MPR slot points to RIGHT NOW (not just slot 7's record). */
        extern unsigned char *mmr;
        fprintf(stderr, "[trace] live mmr[0..7] = ");
        for (int s = 0; s < 8; s++) fprintf(stderr, "%02X ", mmr[s]);
        fprintf(stderr, "\n");
    }
    free(idx);

    /* IRQ + interrupt-related state at exit. */
    {
        extern uint32_t pcec_irq_called[4], pcec_irq_delivered[4];
        static const char *names[4] = { "NMI", "TIMER", "IRQ1/VDC", "IRQ2/CD" };
        fprintf(stderr, "[trace] IRQ delivery (called → delivered):\n");
        for (int i = 0; i < 4; i++) {
            fprintf(stderr, "  %-9s %10u → %10u\n",
                    names[i], pcec_irq_called[i], pcec_irq_delivered[i]);
        }
    }
    /* VRAM-write counters: how much landed in the missing-sprite range. */
    {
        extern uint32_t pcec_vwr_writes_total, pcec_vwr_writes_target;
        extern uint32_t pcec_dma_writes_total, pcec_dma_writes_target;
        extern uint32_t pcec_vwr_target_zero, pcec_vwr_target_nonzero;
        fprintf(stderr, "[trace] VWR writes: %u total, %u to $6800..$73FF "
                        "(%u zero-words, %u non-zero-words)\n",
                pcec_vwr_writes_total, pcec_vwr_writes_target,
                pcec_vwr_target_zero, pcec_vwr_target_nonzero);
        fprintf(stderr, "[trace] DMA writes: %u total, %u to $6800..$73FF\n",
                pcec_dma_writes_total, pcec_dma_writes_target);
    }

    /* IRQ-gate state. */
    {
        extern void pcec_debug_irq_state(uint8_t*, uint8_t*, uint8_t*,
                                          uint16_t*, uint8_t*);
        uint8_t vdc_status, irq_mask, irq_status, reg_p_out;
        uint16_t vdc_cr;
        pcec_debug_irq_state(&vdc_status, &irq_mask, &irq_status, &vdc_cr,
                              &reg_p_out);
        fprintf(stderr, "[trace] vdc_cr=$%04X (VBlankON=%s)  vdc_status=$%02X  "
                        "irq_mask=$%02X (IRQ1 %s)  irq_status=$%02X  "
                        "reg_p=$%02X (FL_I %s)\n",
                vdc_cr,
                (vdc_cr & 0x08) ? "ON" : "off",
                vdc_status,
                irq_mask,
                (irq_mask & 0x02) ? "MASKED" : "unmasked",
                irq_status,
                reg_p_out,
                (reg_p_out & 0x04) ? "SET (irqs blocked)" : "clear");
    }
    /* zp $01 = the variable Bonk's Revenge wait loop polls. PCE zero
     * page is at MPR[1]'s mapping at $2000-$2001 by default (MPR[1] =
     * RAM, $20xx is the bottom of zp on PCE). */
    {
        extern unsigned char **PageR;
        unsigned char *zp = PageR[1];          /* slot 1 covers $2000-$3FFF */
        fprintf(stderr, "[trace] zp $00..$0F = ");
        for (int i = 0; i <= 0x0F; i++) fprintf(stderr, "%02X ", zp[0x2000 + i]);
        fprintf(stderr, "\n");
    }

    /* SATB dump — live sprite attribute table, 64 entries × 4 words.
     * Word 0 = y, word 1 = x, word 2 = pattern, word 3 = attribute.
     * Y/X have hardware offsets (-64/-32). Attr bit 7 = priority
     * (0 = behind BG, 1 = in front). Useful for confirming whether
     * a specific cart-side sprite ever made it into SATB. */
    {
        extern void pcec_debug_satb(uint16_t *out, int max_entries);
        extern void pcec_debug_vram_satb(uint16_t *out, int max_entries);
        extern uint16_t pcec_debug_satb_addr(void);
        uint16_t satb[64 * 4];
        uint16_t vsatb[64 * 4];
        pcec_debug_satb(satb, 64);
        pcec_debug_vram_satb(vsatb, 64);
        fprintf(stderr, "[trace] SATB.W = $%04X (VRAM word addr; byte offset $%05X)\n",
                pcec_debug_satb_addr(), (unsigned)pcec_debug_satb_addr() * 2);
        /* Compare SPRAM vs VRAM-SATB — if they differ, DMA is wrong. */
        int diff = 0;
        for (int i = 0; i < 64 * 4; i++) if (satb[i] != vsatb[i]) diff++;
        fprintf(stderr, "[trace] SPRAM vs VRAM-SATB: %d / 256 words differ\n", diff);
        /* Dump all NON-PARKED entries in VRAM SATB. */
        fprintf(stderr, "[trace] VRAM-SATB non-parked entries:\n");
        int active_v = 0;
        for (int i = 0; i < 64; i++) {
            uint16_t y    = vsatb[i*4 + 0];
            uint16_t x    = vsatb[i*4 + 1];
            uint16_t patt = vsatb[i*4 + 2];
            uint16_t attr = vsatb[i*4 + 3];
            if (y == 0 && x == 0 && patt == 0 && attr == 0) continue;
            int yr = (int)(y & 0x3FF) - 64;
            int xr = (int)(x & 0x3FF) - 32;
            int cgx = (attr >> 8) & 1, cgy = (attr >> 12) & 3;
            int pri = (attr & 0x80) ? 1 : 0;
            fprintf(stderr, "  v%2d y=%04X x=%04X patt=%04X attr=%04X "
                            "(yr=%d xr=%d cgx=%d cgy=%d pri=%d)\n",
                    i, y, x, patt, attr, yr, xr, cgx, cgy, pri);
            if (++active_v >= 20) break;
        }
        fprintf(stderr, "[trace] SATB (live SPRAM):\n");
        fprintf(stderr, "       y(raw)  x(raw)  patt   attr   "
                        "(  yreal  xreal  cgx cgy pal pri)\n");
        int active = 0;
        for (int i = 0; i < 64; i++) {
            uint16_t y    = satb[i*4 + 0];
            uint16_t x    = satb[i*4 + 1];
            uint16_t patt = satb[i*4 + 2];
            uint16_t attr = satb[i*4 + 3];
            int yr = (int)(y & 0x3FF) - 64;
            int xr = (int)(x & 0x3FF) - 32;
            int cgx = (attr >> 8) & 1;
            int cgy = (attr >> 12) & 3;
            int pal = attr & 0x0F;
            int pri = (attr & 0x80) ? 1 : 0;
            /* Skip empty/parked sprites (Y far off-screen). */
            if (yr >= 240 || yr < -64) continue;
            fprintf(stderr, "  %2d   %04X   %04X   %04X   %04X   "
                            "(%5d  %5d  %2d  %2d  %2d  %2d)\n",
                    i, y, x, patt, attr, yr, xr, cgx, cgy, pal, pri);
            active++;
            if (active >= 30) break;
        }
        fprintf(stderr, "  (%d active sprites in SATB)\n", active);
    }

    /* Print the last 80 PCs in execution order — reveals the actual
     * control-flow loop the cart is stuck in (vs. the histogram's
     * frequency-only view). */
    fprintf(stderr, "[trace] last 80 PCs (oldest → newest):\n  ");
    int N = 80;
    int line = 0;
    for (int k = N; k >= 1; k--) {
        uint16_t pc = pcec_pc_ring[(pcec_pc_ring_idx - k) & 0xFFFF];
        fprintf(stderr, "%04X ", pc);
        if (++line % 12 == 0) fprintf(stderr, "\n  ");
    }
    fprintf(stderr, "\n");
}


#endif

static uint16_t read_pad(const Uint8 *keys)
{
    uint16_t m = 0;
    if (keys[SDL_SCANCODE_UP])     m |= PCEC_BTN_UP;
    if (keys[SDL_SCANCODE_DOWN])   m |= PCEC_BTN_DOWN;
    if (keys[SDL_SCANCODE_LEFT])   m |= PCEC_BTN_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])  m |= PCEC_BTN_RIGHT;
    if (keys[SDL_SCANCODE_Z])      m |= PCEC_BTN_I;
    if (keys[SDL_SCANCODE_X])      m |= PCEC_BTN_II;
    if (keys[SDL_SCANCODE_LSHIFT] ||
        keys[SDL_SCANCODE_RSHIFT]) m |= PCEC_BTN_SELECT;
    if (keys[SDL_SCANCODE_RETURN]) m |= PCEC_BTN_RUN;
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: pcehost <rom.pce>\n");
        return 1;
    }
    if (pcec_init(PCEC_REGION_AUTO, 22050) != 0) {
        fprintf(stderr, "pcec_init failed\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);

    if (pcec_rom_is_us_encoded(buf, sz)) {
        printf("US-encoded cart detected — decrypting %ld bytes\n", sz);
        pcec_rom_decrypt_us(buf, sz);
    }

    if (pcec_load_rom(buf, sz) != 0) {
        fprintf(stderr, "ROM load failed\n");
        return 1;
    }
#ifdef PCEC_DEBUG_TRACE
    {
        extern unsigned char *ROM;
        extern unsigned char *ROMMapR[256];
        fprintf(stderr, "[trace] ROM=%p, sz=%ld bytes (=$%X banks)\n",
                (void*)ROM, sz, (int)(sz / 0x2000));
        fprintf(stderr, "[trace] ROMMapR[0..7]   :");
        for (int i = 0; i < 8; i++) {
            ptrdiff_t off = ROMMapR[i] - ROM;
            fprintf(stderr, " [%d]=ROM+$%lX", i, (long)off);
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "[trace] ROMMapR[$3F..$42]:");
        for (int i = 0x3F; i <= 0x42; i++) {
            ptrdiff_t off = ROMMapR[i] - ROM;
            fprintf(stderr, " [%02X]=ROM+$%lX", i, (long)off);
        }
        fprintf(stderr, "\n");
        /* First 16 bytes of ROM bank 0 — sanity vs python read of file. */
        fprintf(stderr, "[trace] ROM[0..15] = ");
        for (int i = 0; i < 16; i++) fprintf(stderr, "%02X ", ROM[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "[trace] ROM[$11E..$125] = ");
        for (int i = 0x11E; i < 0x126; i++) fprintf(stderr, "%02X ", ROM[i]);
        fprintf(stderr, "\n");
    }
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* The scanline renderer writes the device-shape 128x128 LCD fb
     * directly. We allocate that on the host too and present it
     * upscaled so pcehost shows the same downsample/blend you'd see
     * on the Thumby Color. */
    const int LCD_W = 128, LCD_H = 128;
    const int scale = 4;                 /* SDL window: 512×512 */
    SDL_Window *win = SDL_CreateWindow(
        "ThumbyNES — pcehost",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LCD_W * scale, LCD_H * scale, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         LCD_W, LCD_H);

    uint16_t lcd_fb[128 * 128];          /* 32 KB on host stack region */
    pcec_set_scale_target(lcd_fb, /*scale_mode=*/0, /*blend=*/1, 0, 0);

    /* Audio: 22050 Hz mono signed 16-bit, pushed per frame.
     * SDL_QueueAudio batches samples; we just ensure the queue
     * never grows unbounded (the sleep-based frame pacer keeps
     * the producer at ~60 Hz). */
    const int AUDIO_RATE = 22050;
    const int SAMPLES_PER_FRAME = AUDIO_RATE / 60 + 1;  /* 368 */
    /* 2048-sample chunk = ~93 ms — large enough that the OS audio
     * subsystem won't see a momentary frame stretch on host as a
     * starvation event. Latency on host doesn't matter much; on the
     * device we use a separate DMA path. */
    SDL_AudioSpec want = {
        .freq = AUDIO_RATE,
        .format = AUDIO_S16SYS,
        .channels = 1,
        .samples = 2048,
        .callback = NULL,
    };
    SDL_AudioDeviceID audio_dev =
        SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    int16_t audio_scratch[1024];
    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);

    /* Frame pacer. PCE is 60 Hz NTSC. sibling host runners (md/gb)
     * use the same pattern. */
    const uint32_t FRAME_MS = 1000 / 60;
    uint32_t next_frame_ms = SDL_GetTicks();
#ifdef PCEC_DEBUG_TRACE
    /* Track active-sprite count across frames so we can see if the
     * cart's title-sequence state machine is advancing (sprite count
     * should change as animals enter / leave). */
    int prev_active = -1;
    uint64_t frame_n = 0;
#endif

#ifdef PCEC_DEBUG_TRACE
    /* SDL_SCANCODE_F12 resets all trace counters so we can capture
     * only what the cart does post-key-press. Useful for "press Start,
     * then F12 to start tracing the level-load hang in isolation." */
    extern uint32_t pcec_pc_hist[65536];
    extern uint32_t pcec_pc_ring_idx;
    extern uint32_t pcec_irq_called[4], pcec_irq_delivered[4];
    int prev_f12 = 0;
#endif
    int running = 1;
    /* PCEC_MAX_FRAMES env: hard cap, exit cleanly so the trace dumps.
     * Useful when WSLg breaks and we need to run pcehost headless via
     * SDL_VIDEODRIVER=dummy. */
    int max_frames = -1;
    {
        const char *env = getenv("PCEC_MAX_FRAMES");
        if (env) max_frames = atoi(env);
    }
    int frame_idx = 0;
    while (running) {
        if (max_frames > 0 && frame_idx >= max_frames) running = 0;
        frame_idx++;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = 0;
#ifdef PCEC_DEBUG_TRACE
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12 && !prev_f12) {
                memset(pcec_pc_hist, 0, sizeof(pcec_pc_hist));
                pcec_pc_ring_idx = 0;
                memset(pcec_irq_called,    0, sizeof(pcec_irq_called));
                memset(pcec_irq_delivered, 0, sizeof(pcec_irq_delivered));
                fprintf(stderr, "[trace] F12: counters reset\n");
                prev_f12 = 1;
            }
            if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_F12) prev_f12 = 0;
            /* F11 = save current 128x128 RGB565 framebuffer to a PPM
             * so we can inspect what the cart actually renders. */
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11) {
                static int shot_idx = 0;
                char path[64];
                snprintf(path, sizeof(path), "/tmp/pcehost_shot_%02d.ppm", shot_idx++);
                FILE *p = fopen(path, "wb");
                if (p) {
                    fprintf(p, "P6\n128 128\n255\n");
                    for (int i = 0; i < LCD_W * LCD_H; i++) {
                        uint16_t px = lcd_fb[i];
                        unsigned char r = ((px >> 11) & 0x1F) << 3;
                        unsigned char g = ((px >>  5) & 0x3F) << 2;
                        unsigned char b = ( px        & 0x1F) << 3;
                        fputc(r, p); fputc(g, p); fputc(b, p);
                    }
                    fclose(p);
                    fprintf(stderr, "[trace] F11: framebuffer → %s\n", path);
                }
            }
#endif
        }
        pcec_set_buttons(read_pad(SDL_GetKeyboardState(NULL)));
        /* Re-bind each frame so the user can flip the blend toggle
         * later without restarting. */
        pcec_set_scale_target(lcd_fb, /*scale_mode=*/0, /*blend=*/1, 0, 0);
#ifdef PCEC_SPRITE_SCAN_TRACE
        pcec_scan_frame = frame_n;
#endif
        pcec_run_frame();
#ifdef PCEC_DEBUG_TRACE
        /* Auto-capture key frames so the user doesn't have to time
         * F11 presses through the title sequence. Frames chosen to
         * span pre-animals → mid-scene → post-scene. */
        if (frame_n == 600 || frame_n == 750 || frame_n == 900 || frame_n == 1050) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/pcehost_auto_%04llu.ppm",
                     (unsigned long long)frame_n);
            FILE *p = fopen(path, "wb");
            if (p) {
                fprintf(p, "P6\n128 128\n255\n");
                for (int i = 0; i < LCD_W * LCD_H; i++) {
                    uint16_t px = lcd_fb[i];
                    unsigned char r = ((px >> 11) & 0x1F) << 3;
                    unsigned char g = ((px >>  5) & 0x3F) << 2;
                    unsigned char b = ( px        & 0x1F) << 3;
                    fputc(r, p); fputc(g, p); fputc(b, p);
                }
                fclose(p);
                fprintf(stderr, "[trace] auto-capture frame %llu → %s\n",
                        (unsigned long long)frame_n, path);
            }
        }
#endif

#ifdef PCEC_DEBUG_TRACE
        {
            extern void pcec_debug_satb(uint16_t *out, int max_entries);
            uint16_t satb[64 * 4];
            pcec_debug_satb(satb, 64);
            int active = 0;
            for (int i = 0; i < 64; i++) {
                uint16_t y = satb[i*4 + 0];
                int yr = (int)(y & 0x3FF) - 64;
                if (yr >= -16 && yr < 240) active++;
            }
            /* Periodic VRAM samples — every 30 frames, count non-zero
             * bytes across each animal-sprite cell. */
            if ((frame_n % 30) == 0) {
                extern int pcec_debug_vram_zero(uint32_t, uint8_t*);
                /* Cell offsets for animal sprites (frame 663/792 dump):
                 * s5=$6C80 s6=$6D00 s7=$6E00 s8=$6F00 s9=$7000
                 * s10=$7100 s11=$7200 s12=$7300 s13=$7400 */
                static const uint32_t cells[] = {
                    0x6800, 0x6C80, 0x6D00, 0x6E00, 0x6F00,
                    0x7000, 0x7100, 0x7200, 0x7300, 0x7400,
                    0x8200, 0x8400 /* Bonk — should always have data */
                };
                int totals[12] = {0};
                for (int c = 0; c < 12; c++) {
                    for (int b = 0; b < 128; b++) {
                        uint8_t bb;
                        pcec_debug_vram_zero(cells[c] + b, &bb);
                        if (bb) totals[c]++;
                    }
                }
                /* Only log when interesting (any non-zero across the
                 * animal range). */
                int any_animal_nonz = 0;
                for (int c = 1; c < 10; c++) if (totals[c] > 0) any_animal_nonz = 1;
                if (any_animal_nonz || totals[10] || totals[11]) {
                    fprintf(stderr, "[trace] frame %llu cells:", (unsigned long long)frame_n);
                    for (int c = 0; c < 12; c++) {
                        fprintf(stderr, " $%04X=%d", cells[c], totals[c]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (active != prev_active) {
                fprintf(stderr, "[trace] frame %llu: active sprites = %d\n",
                        (unsigned long long)frame_n, active);
                prev_active = active;
                /* When the count jumps to >= 6 (animal-march territory),
                 * dump every active sprite so we can see what the cart
                 * is asking us to draw. */
                if (active >= 6) {
                    extern int pcec_debug_vram_zero(uint32_t off, uint8_t *out);
                    /* Also: dump the VRAM byte-range $6800..$683F right now,
                     * during the active scene — distinct from the at-exit
                     * dump which captures post-title-screen state. */
                    {
                        uint8_t buf[64];
                        int all_zero = 1;
                        for (int b = 0; b < 64; b++) {
                            pcec_debug_vram_zero(0x6800 + b, &buf[b]);
                        }
                        for (int b = 0; b < 64; b++) if (buf[b]) { all_zero = 0; break; }
                        fprintf(stderr, "    VRAM[$6800..$683F] live: %s",
                                all_zero ? "ALL ZERO\n" : "");
                        if (!all_zero) {
                            for (int b = 0; b < 64; b++) {
                                fprintf(stderr, "%02X ", buf[b]);
                                if ((b & 15) == 15) fprintf(stderr, "\n                                  ");
                            }
                            fprintf(stderr, "\n");
                        }
                    }
                    for (int i = 0; i < 64; i++) {
                        uint16_t y    = satb[i*4 + 0];
                        uint16_t x    = satb[i*4 + 1];
                        uint16_t patt = satb[i*4 + 2];
                        uint16_t attr = satb[i*4 + 3];
                        int yr = (int)(y & 0x3FF) - 64;
                        if (yr < -16 || yr >= 240) continue;
                        int xr = (int)(x & 0x3FF) - 32;
                        int cgx = (attr >> 8) & 1;
                        int cgy = (attr >> 12) & 3;
                        int pri = (attr & 0x80) ? 1 : 0;
                        /* Same math the renderer uses to find the
                         * cell's first byte in VRAM. */
                        int no = (patt & 0x7FF) >> 1;
                        no &= ~(cgy * 2 + cgx);
                        uint32_t cell_off = (uint32_t)no * 128;
                        uint8_t cell0[16];
                        int zero = pcec_debug_vram_zero(cell_off, cell0);
                        fprintf(stderr, "    s%2d y=%4d x=%4d patt=$%04X "
                                        "size=%dx%d pri=%d attr=$%04X "
                                        "cell$%05X[0..7]=%02X%02X%02X%02X%02X%02X%02X%02X %s\n",
                                i, yr, xr, patt, (cgx+1)*16, (cgy+1)*16, pri, attr,
                                cell_off, cell0[0],cell0[1],cell0[2],cell0[3],
                                cell0[4],cell0[5],cell0[6],cell0[7],
                                zero ? "(EMPTY)" : "");
                    }
                }
            }
            frame_n++;
        }
#endif

        SDL_UpdateTexture(tex, NULL, lcd_fb, LCD_W * 2);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        /* Pull one frame of audio and push to SDL. We aim for a queue
         * depth of ~4–8 frames so the audio stream survives short
         * scheduling stalls in the wall-clock pacer; dropping samples
         * mid-stream causes audible clicks (the wraparound at the next
         * sample is a step discontinuity). Cap at 16 frames so latency
         * doesn't creep unboundedly. */
        if (audio_dev) {
            int got = pcec_audio_pull(audio_scratch, SAMPLES_PER_FRAME);
            uint32_t q = SDL_GetQueuedAudioSize(audio_dev);
            if (q < (uint32_t)(SAMPLES_PER_FRAME * 16 * sizeof(int16_t))) {
                SDL_QueueAudio(audio_dev, audio_scratch,
                               (uint32_t)got * sizeof(int16_t));
            }
        }

        /* Sleep until next 60 Hz tick. If we're already behind, catch
         * up without sleeping (up to a frame) so transient stalls
         * don't compound. */
        next_frame_ms += FRAME_MS;
        uint32_t now = SDL_GetTicks();
        if ((int32_t)(next_frame_ms - now) > 0)
            SDL_Delay(next_frame_ms - now);
        else if ((int32_t)(now - next_frame_ms) > (int32_t)(2 * FRAME_MS))
            next_frame_ms = now;
    }

    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
#ifdef PCEC_DEBUG_TRACE
    dump_pc_hist();
#endif
    pcec_shutdown();
    free(buf);
    return 0;
}
