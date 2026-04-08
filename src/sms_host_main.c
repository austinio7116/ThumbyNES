/*
 * ThumbyNES Phase 1 SDL2 host runner — SMS / Game Gear edition.
 *
 * Loads an .sms or .gg ROM, runs smsplus, displays the active viewport
 * in a window, pipes audio to the soundcard, maps keyboard to the
 * SMS pad. Sibling of host_main.c.
 *
 * Keys (matches the NES host where it makes sense):
 *   Arrow keys → D-pad
 *   Z          → Button 1
 *   X          → Button 2
 *   Enter      → Start (GG)
 *   RShift     → Pause (SMS)
 *   Esc        → Quit
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_core.h"

#define WIN_SCALE     3
#define SAMPLE_RATE   22050

static uint8_t poll_buttons(void)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint8_t m = 0;
    if (k[SDL_SCANCODE_Z])      m |= SMSC_BTN_BUTTON1;
    if (k[SDL_SCANCODE_X])      m |= SMSC_BTN_BUTTON2;
    if (k[SDL_SCANCODE_RETURN]) m |= SMSC_BTN_START;
    if (k[SDL_SCANCODE_RSHIFT]) m |= SMSC_BTN_PAUSE;
    if (k[SDL_SCANCODE_UP])     m |= SMSC_BTN_UP;
    if (k[SDL_SCANCODE_DOWN])   m |= SMSC_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT])   m |= SMSC_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT])  m |= SMSC_BTN_RIGHT;
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s rom.sms|rom.gg\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long off = ((sz > 0x4000) && ((sz / 512) & 1)) ? 512 : 0;
    long rom_sz = sz - off;
    fseek(f, off, SEEK_SET);
    uint8_t *rom = malloc(rom_sz);
    if (fread(rom, 1, rom_sz, f) != (size_t)rom_sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    if (smsc_init(SMSC_SYS_AUTO, SAMPLE_RATE) != 0) { fprintf(stderr, "smsc_init failed\n"); return 1; }
    if (smsc_load_rom(rom, rom_sz) != 0) { fprintf(stderr, "rom load failed\n"); return 1; }
    /* `rom` is borrowed by smsplus until shutdown — don't free. */

    int vx, vy, vw, vh;
    smsc_viewport(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = SMSC_BITMAP_W; vh = SMSC_BITMAP_H; }
    printf("system: %s, viewport %dx%d at (%d,%d), refresh %d Hz\n",
           smsc_is_gg() ? "GG" : "SMS", vw, vh, vx, vy, smsc_refresh_rate());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    SDL_Window   *win = SDL_CreateWindow("ThumbyNES (SMS/GG)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        vw * WIN_SCALE, vh * WIN_SCALE, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture  *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, vw, vh);

    SDL_AudioSpec want = { .freq = SAMPLE_RATE, .format = AUDIO_S16SYS,
                           .channels = 1, .samples = 1024 };
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);

    int16_t audio_buf[2048];

    /* Frame pacing — cap at the cart's native refresh (60 NTSC / 50 PAL).
     * vsync alone doesn't help if the host display refreshes at 144 Hz. */
    const int    target_fps = smsc_refresh_rate();
    const Uint32 frame_ms   = 1000 / target_fps;
    Uint32       next_tick  = SDL_GetTicks();

    bool running = true;
    int frame = 0;
    while (running) {
        if (++frame % 60 == 0) { fprintf(stderr, "frame %d\n", frame); fflush(stderr); }
        if (getenv("SMSHOST_MAX_FRAMES")) {
            int mx = atoi(getenv("SMSHOST_MAX_FRAMES"));
            if (mx && frame >= mx) running = false;
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        smsc_set_buttons(poll_buttons());
        smsc_run_frame();

        const uint8_t  *fb  = smsc_framebuffer();
        const uint16_t *pal = smsc_palette_rgb565();
        if (fb) {
            void *px;
            int   tex_pitch;
            SDL_LockTexture(tex, NULL, &px, &tex_pitch);
            for (int y = 0; y < vh; y++) {
                const uint8_t *src = fb + (y + vy) * SMSC_BITMAP_W + vx;
                uint16_t *dst = (uint16_t *)((uint8_t *)px + y * tex_pitch);
                for (int x = 0; x < vw; x++) dst[x] = pal[src[x]];
            }
            SDL_UnlockTexture(tex);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        int n = smsc_audio_pull(audio_buf, sizeof(audio_buf) / sizeof(audio_buf[0]));
        if (dev && n > 0) SDL_QueueAudio(dev, audio_buf, n * sizeof(int16_t));

        next_tick += frame_ms;
        Uint32 now = SDL_GetTicks();
        if ((Sint32)(next_tick - now) > 0) SDL_Delay(next_tick - now);
        else next_tick = now;
    }

    smsc_shutdown();
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
