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

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = 0;
        }
        pcec_set_buttons(read_pad(SDL_GetKeyboardState(NULL)));
        /* Re-bind each frame so the user can flip the blend toggle
         * later without restarting. */
        pcec_set_scale_target(lcd_fb, /*scale_mode=*/0, /*blend=*/1, 0, 0);
        pcec_run_frame();

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
    pcec_shutdown();
    free(buf);
    return 0;
}
