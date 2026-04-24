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

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int vx, vy, vw, vh;
    pcec_viewport(&vx, &vy, &vw, &vh);
    const int scale = 2;
    SDL_Window *win = SDL_CreateWindow(
        "ThumbyNES — pcehost",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        vw * scale, vh * scale, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                         SDL_TEXTUREACCESS_STREAMING, vw, vh);

    uint16_t *scratch = malloc((size_t)vw * vh * 2);

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = 0;
        }
        pcec_set_buttons(read_pad(SDL_GetKeyboardState(NULL)));
        pcec_run_frame();
        pcec_viewport(&vx, &vy, &vw, &vh);

        const uint8_t  *fb  = pcec_framebuffer();
        const uint16_t *pal = pcec_palette_rgb565();
        if (fb && pal) {
            for (int y = 0; y < vh; y++) {
                const uint8_t *row = fb + (vy + y) * PCEC_PITCH + vx;
                uint16_t *dst = scratch + y * vw;
                for (int x = 0; x < vw; x++) dst[x] = pal[row[x]];
            }
            SDL_UpdateTexture(tex, NULL, scratch, vw * 2);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(scratch);
    pcec_shutdown();
    free(buf);
    return 0;
}
