/*
 * ThumbyNES Phase 1 SDL2 host runner — MD / Genesis edition.
 *
 * Loads a .md / .bin / .gen ROM, runs PicoDrive, displays the active
 * viewport in a window, pipes audio to the soundcard, maps keyboard to
 * the MD pad. Sibling of sms_host_main.c.
 *
 * Keys (matches the SMS host where sensible):
 *   Arrow keys → D-pad
 *   Z          → B   (primary "jump")
 *   X          → C   (primary "attack")
 *   A          → A
 *   S          → X   (6-button)
 *   D          → Y
 *   F          → Z
 *   Enter      → START
 *   RShift     → MODE
 *   Esc        → Quit
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md_core.h"

#define WIN_SCALE     2
#define SAMPLE_RATE   22050

static uint16_t poll_buttons(void)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint16_t m = 0;
    if (k[SDL_SCANCODE_UP])     m |= MDC_BTN_UP;
    if (k[SDL_SCANCODE_DOWN])   m |= MDC_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT])   m |= MDC_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT])  m |= MDC_BTN_RIGHT;
    if (k[SDL_SCANCODE_Z])      m |= MDC_BTN_B;
    if (k[SDL_SCANCODE_X])      m |= MDC_BTN_C;
    if (k[SDL_SCANCODE_A])      m |= MDC_BTN_A;
    if (k[SDL_SCANCODE_S])      m |= MDC_BTN_X;
    if (k[SDL_SCANCODE_D])      m |= MDC_BTN_Y;
    if (k[SDL_SCANCODE_F])      m |= MDC_BTN_Z;
    if (k[SDL_SCANCODE_RETURN]) m |= MDC_BTN_START;
    if (k[SDL_SCANCODE_RSHIFT]) m |= MDC_BTN_MODE;
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s rom.md|rom.gen|rom.bin\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(sz);
    if (!rom || fread(rom, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    if (mdc_init(MDC_REGION_AUTO, SAMPLE_RATE) != 0) { fprintf(stderr, "mdc_init failed\n"); return 1; }
    if (mdc_load_rom(rom, sz) != 0) { fprintf(stderr, "rom load failed\n"); return 1; }
    /* md_core.c copies the ROM into its own buffer — we can free ours. */
    free(rom);

    int vx, vy, vw, vh;
    mdc_viewport(&vx, &vy, &vw, &vh);
    printf("viewport %dx%d at (%d,%d), refresh %d Hz\n",
           vw, vh, vx, vy, mdc_refresh_rate());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    SDL_Window   *win = SDL_CreateWindow("ThumbyNES (Mega Drive)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        vw * WIN_SCALE, vh * WIN_SCALE, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture  *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, MDC_MAX_W, MDC_MAX_H);

    SDL_AudioSpec want = { .freq = SAMPLE_RATE, .format = AUDIO_S16SYS,
                           .channels = 1, .samples = 1024 };
    SDL_AudioSpec got = {0};
    /* Allow the audio backend to negotiate freq/format if it can't hit
     * the exact ask — better to resample than get a zero device. */
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (dev) {
        printf("audio: %d Hz, fmt=0x%04x, channels=%d, samples=%d\n",
               got.freq, got.format, got.channels, got.samples);
        SDL_PauseAudioDevice(dev, 0);
    } else {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    }

    int16_t audio_buf[2048];

    const int    target_fps = mdc_refresh_rate();
    const Uint32 frame_ms   = 1000 / target_fps;
    Uint32       next_tick  = SDL_GetTicks();

    bool running = true;
    int frame = 0;
    while (running) {
        if (++frame % 120 == 0) { fprintf(stderr, "frame %d\n", frame); fflush(stderr); }
        if (getenv("MDHOST_MAX_FRAMES")) {
            int mx = atoi(getenv("MDHOST_MAX_FRAMES"));
            if (mx && frame >= mx) running = false;
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        mdc_set_buttons(poll_buttons());
        mdc_run_frame();

        /* Re-sample the viewport every frame — VDP can switch between
         * H32/H40 and V28/V30 mid-game. */
        mdc_viewport(&vx, &vy, &vw, &vh);

        const uint16_t *fb = mdc_framebuffer();
        SDL_Rect src = { vx, vy, vw, vh };
        SDL_UpdateTexture(tex, NULL, fb, MDC_MAX_W * sizeof(uint16_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src, NULL);
        SDL_RenderPresent(ren);

        int n = mdc_audio_pull(audio_buf, sizeof(audio_buf) / sizeof(audio_buf[0]));
        if (dev && n > 0) SDL_QueueAudio(dev, audio_buf, n * sizeof(int16_t));

        next_tick += frame_ms;
        Uint32 now = SDL_GetTicks();
        if ((Sint32)(next_tick - now) > 0) SDL_Delay(next_tick - now);
        else next_tick = now;
    }

    mdc_shutdown();
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
