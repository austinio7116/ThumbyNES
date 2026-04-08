/*
 * ThumbyNES Phase 1 SDL2 host runner.
 *
 * Loads a .nes ROM, runs Nofrendo, displays the framebuffer in a window,
 * pipes APU samples to the soundcard, maps keyboard to NES controller.
 *
 * Build: see top-level CMakeLists.txt — only compiled if SDL2 is found.
 *
 * Keys:
 *   Arrow keys → D-pad
 *   Z          → A
 *   X          → B
 *   Enter      → Start
 *   RShift     → Select
 *   Esc        → Quit
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes_core.h"

#define WIN_SCALE     3
#define SAMPLE_RATE   44100

static uint8_t poll_buttons(void)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint8_t m = 0;
    if (k[SDL_SCANCODE_Z])      m |= NESC_BTN_A;
    if (k[SDL_SCANCODE_X])      m |= NESC_BTN_B;
    if (k[SDL_SCANCODE_RETURN]) m |= NESC_BTN_START;
    if (k[SDL_SCANCODE_RSHIFT]) m |= NESC_BTN_SELECT;
    if (k[SDL_SCANCODE_UP])     m |= NESC_BTN_UP;
    if (k[SDL_SCANCODE_DOWN])   m |= NESC_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT])   m |= NESC_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT])  m |= NESC_BTN_RIGHT;
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.nes\n", argv[0]);
        return 1;
    }

    /* Slurp the ROM. */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(sz);
    if (fread(rom, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    if (nesc_init(SAMPLE_RATE) != 0) { fprintf(stderr, "nesc_init failed\n"); return 1; }
    if (nesc_load_rom(rom, sz) != 0)  { fprintf(stderr, "rom load failed\n"); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window   *win = SDL_CreateWindow("ThumbyNES",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        NESC_SCREEN_W * WIN_SCALE, NESC_SCREEN_H * WIN_SCALE, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture  *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, NESC_SCREEN_W, NESC_SCREEN_H);

    /* Audio: 16-bit signed mono at SAMPLE_RATE. We push one frame's worth
     * (~735 samples) per emulator frame from nesc_audio_pull. */
    SDL_AudioSpec want = { .freq = SAMPLE_RATE, .format = AUDIO_S16SYS,
                           .channels = 1, .samples = 1024 };
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);

    const uint16_t *pal = nesc_palette_rgb565();
    int pitch = nesc_framebuffer_pitch();

    int16_t  audio_buf[2048];
    uint16_t scanline[NESC_SCREEN_W];

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        nesc_set_buttons(poll_buttons());
        nesc_run_frame();

        /* Convert palette-indexed framebuffer to RGB565 and upload. */
        const uint8_t *fb = nesc_framebuffer();
        if (fb) {
            void *px;
            int   tex_pitch;
            SDL_LockTexture(tex, NULL, &px, &tex_pitch);
            for (int y = 0; y < NESC_SCREEN_H; y++) {
                const uint8_t *src = fb + y * pitch + 8 /* NES_SCREEN_OVERDRAW */;
                uint16_t *dst = (uint16_t *)((uint8_t *)px + y * tex_pitch);
                for (int x = 0; x < NESC_SCREEN_W; x++) dst[x] = pal[src[x]];
            }
            SDL_UnlockTexture(tex);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        /* Drain one frame of audio. */
        int n = nesc_audio_pull(audio_buf, sizeof(audio_buf) / sizeof(audio_buf[0]));
        if (dev && n > 0) SDL_QueueAudio(dev, audio_buf, n * sizeof(int16_t));
    }

    nesc_shutdown();
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(rom);
    return 0;
}
