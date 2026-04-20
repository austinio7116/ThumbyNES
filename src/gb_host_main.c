/*
 * ThumbyNES Phase 1 SDL2 host runner — Game Boy edition.
 *
 * Loads a .gb ROM, runs peanut_gb, displays the framebuffer in a
 * window, pipes audio to the soundcard, maps keyboard to the GB pad.
 *
 * Keys (consistent with neshost / smshost where it makes sense):
 *   Arrow keys → D-pad
 *   Z          → A
 *   X          → B
 *   Enter      → Start
 *   RShift     → Select
 *   1..6       → cycle palette
 *   Esc        → quit
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_core.h"

#define WIN_SCALE     3
#define SAMPLE_RATE   22050

static uint8_t poll_buttons(void)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint8_t m = 0;
    if (k[SDL_SCANCODE_Z])      m |= GBC_BTN_A;
    if (k[SDL_SCANCODE_X])      m |= GBC_BTN_B;
    if (k[SDL_SCANCODE_RETURN]) m |= GBC_BTN_START;
    if (k[SDL_SCANCODE_RSHIFT]) m |= GBC_BTN_SELECT;
    if (k[SDL_SCANCODE_UP])     m |= GBC_BTN_UP;
    if (k[SDL_SCANCODE_DOWN])   m |= GBC_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT])   m |= GBC_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT])  m |= GBC_BTN_RIGHT;
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s rom.gb\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(sz);
    if (fread(rom, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    if (gbc_init(SAMPLE_RATE) != 0) { fprintf(stderr, "gbc_init failed\n"); return 1; }
    int rc = gbc_load_rom(rom, sz);
    if (rc != 0) { fprintf(stderr, "rom load failed (%d)\n", rc); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    SDL_Window   *win = SDL_CreateWindow("ThumbyNES (GB)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        GBC_SCREEN_W * WIN_SCALE, GBC_SCREEN_H * WIN_SCALE, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture  *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, GBC_SCREEN_W, GBC_SCREEN_H);

    SDL_AudioSpec want = { .freq = SAMPLE_RATE, .format = AUDIO_S16SYS,
                           .channels = 1, .samples = 1024 };
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);

    int16_t audio_buf[2048];

    /* 60 Hz pacing — vsync alone doesn't help on a 144 Hz host display. */
    const Uint32 frame_ms  = 1000 / gbc_refresh_rate();
    Uint32       next_tick = SDL_GetTicks();

    bool running = true;
    int  palette = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE) running = false;
                if (k >= SDLK_1 && k <= SDLK_6) {
                    palette = k - SDLK_1;
                    gbc_set_palette(palette);
                    fprintf(stderr, "palette: %s\n", gbc_palette_name(palette));
                }
            }
        }

        gbc_set_buttons(poll_buttons());
        gbc_run_frame();

        const uint16_t *fb = gbc_framebuffer();
        if (fb) {
            void *px;
            int   tex_pitch;
            SDL_LockTexture(tex, NULL, &px, &tex_pitch);
            for (int y = 0; y < GBC_SCREEN_H; y++) {
                const uint16_t *src = fb + y * GBC_SCREEN_W;
                uint16_t *dst = (uint16_t *)((uint8_t *)px + y * tex_pitch);
                memcpy(dst, src, GBC_SCREEN_W * sizeof(uint16_t));
            }
            SDL_UnlockTexture(tex);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        int n = gbc_audio_pull(audio_buf, sizeof(audio_buf) / sizeof(audio_buf[0]));
        if (dev && n > 0) SDL_QueueAudio(dev, audio_buf, n * sizeof(int16_t));

        next_tick += frame_ms;
        Uint32 now = SDL_GetTicks();
        if ((Sint32)(next_tick - now) > 0) SDL_Delay(next_tick - now);
        else next_tick = now;
    }

    gbc_shutdown();
    /* rom is borrowed by peanut_gb during the session — free after shutdown. */
    free(rom);
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
