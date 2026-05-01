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

    const char *fb_dump_path = getenv("MDHOST_FB_DUMP");
    const int   max_frames   = getenv("MDHOST_MAX_FRAMES")
                               ? atoi(getenv("MDHOST_MAX_FRAMES")) : 0;

    /* MDHOST_WAV: capture pulled audio into a 16-bit mono PCM .wav for
     * offline analysis (used to compare YM2612 implementations against
     * a real-hardware reference). Header is patched on close so we
     * don't need to know the total sample count up front. */
    const char *wav_path = getenv("MDHOST_WAV");
    FILE       *wav_f    = NULL;
    long        wav_data_bytes = 0;
    if (wav_path) {
        wav_f = fopen(wav_path, "wb");
        if (wav_f) {
            unsigned char hdr[44] = {
                'R','I','F','F', 0,0,0,0,
                'W','A','V','E','f','m','t',' ',
                16,0,0,0,                    /* fmt chunk size */
                1,0,                         /* PCM */
                1,0,                         /* channels = 1 */
                0,0,0,0, 0,0,0,0,            /* sample rate, byte rate */
                2,0,                         /* block align */
                16,0,                        /* bits/sample */
                'd','a','t','a', 0,0,0,0,
            };
            unsigned int rate = SAMPLE_RATE;
            hdr[24] = (unsigned char)(rate);
            hdr[25] = (unsigned char)(rate >> 8);
            hdr[26] = (unsigned char)(rate >> 16);
            hdr[27] = (unsigned char)(rate >> 24);
            unsigned int byte_rate = rate * 2;  /* mono, 16-bit */
            hdr[28] = (unsigned char)(byte_rate);
            hdr[29] = (unsigned char)(byte_rate >> 8);
            hdr[30] = (unsigned char)(byte_rate >> 16);
            hdr[31] = (unsigned char)(byte_rate >> 24);
            fwrite(hdr, 1, sizeof(hdr), wav_f);
        } else {
            fprintf(stderr, "MDHOST_WAV: fopen %s failed\n", wav_path);
        }
    }
    /* Peak non-black tracking — guards against the final-frame
     * snapshot landing on a fade/transition and looking "stuck on
     * black" when the game was actually rendering content earlier.
     * Sampled every 60 frames (~1s at 60Hz) so the cost is trivial
     * even at the 320x224 worst case. */
    long peak_nonblack = 0, peak_total = 0;
    int  peak_frame    = 0;
    /* MDHOST_AUTO_START: synthesise a START-button pulse on a slow
     * cycle so the headless compat sweep advances past Sega-logo /
     * title / mode-select screens into actual gameplay. Active after
     * frame 60 (let the cart finish its splash sequence first), then
     * presses START for 8 frames every 48-frame cycle (~0.8s real) —
     * a press-release pattern, not a hold, so games that latch on
     * just-pressed advance reliably. */
    const int   auto_start   = getenv("MDHOST_AUTO_START")
                               ? atoi(getenv("MDHOST_AUTO_START")) : 0;

    bool running = true;
    int frame = 0;
    while (running) {
        if (++frame % 120 == 0) { fprintf(stderr, "frame %d\n", frame); fflush(stderr); }
        if (max_frames && frame >= max_frames) running = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        uint16_t btns = poll_buttons();
        if (auto_start && frame > 60) {
            int phase = (frame - 60) % 48;
            if (phase < 8) btns |= MDC_BTN_START;
        }
        mdc_set_buttons(btns);
        mdc_run_frame();

        /* Re-sample the viewport every frame — VDP can switch between
         * H32/H40 and V28/V30 mid-game. */
        mdc_viewport(&vx, &vy, &vw, &vh);

        const uint16_t *fb = mdc_framebuffer();

        if (frame % 60 == 0 && fb && vw > 0 && vh > 0) {
            long nb = 0, tot = 0;
            for (int yy = vy; yy < vy + vh; ++yy) {
                const uint16_t *row = fb + yy * MDC_MAX_W;
                for (int xx = vx; xx < vx + vw; ++xx) {
                    if (row[xx] != 0) ++nb;
                    ++tot;
                }
            }
            /* Compare nb/tot vs peak_nonblack/peak_total without div. */
            if (peak_total == 0
                || (long long)nb * peak_total
                   > (long long)peak_nonblack * tot) {
                peak_nonblack = nb;
                peak_total    = tot;
                peak_frame    = frame;
            }
        }

        SDL_Rect src = { vx, vy, vw, vh };
        SDL_UpdateTexture(tex, NULL, fb, MDC_MAX_W * sizeof(uint16_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src, NULL);
        SDL_RenderPresent(ren);

        int n = mdc_audio_pull(audio_buf, sizeof(audio_buf) / sizeof(audio_buf[0]));
        if (dev && n > 0) SDL_QueueAudio(dev, audio_buf, n * sizeof(int16_t));
        if (wav_f && n > 0) {
            fwrite(audio_buf, sizeof(int16_t), (size_t)n, wav_f);
            wav_data_bytes += (long)n * (long)sizeof(int16_t);
        }

        next_tick += frame_ms;
        Uint32 now = SDL_GetTicks();
        if ((Sint32)(next_tick - now) > 0) SDL_Delay(next_tick - now);
        else next_tick = now;
    }

#ifdef MD_SP_GUARD
    /* If a trap PC was set, surface the visit count at exit so we can
     * tell "stuck inside" (count = 1) from "outer loop re-enters" (count
     * grows linearly with frames). */
    {
        extern volatile unsigned int md_dbg_trap_visits;
        extern volatile unsigned int md_dbg_trap_pc_dump;
        if (md_dbg_trap_pc_dump) {
            fprintf(stderr, "MDHOST_TRAP visits=%u (trap_pc=0x%06x)\n",
                    md_dbg_trap_visits, md_dbg_trap_pc_dump);
        }
    }
#endif

    /* Final-frame analysis for batch compat testing: dump the raw
     * RGB565 framebuffer and print a summary line that the test
     * driver can grep for. Non-black count is computed across the
     * active viewport only — borders are always background colour
     * but tell us nothing about whether the game booted. */
    {
        const uint16_t *fb_final = mdc_framebuffer();
        int fvx, fvy, fvw, fvh;
        mdc_viewport(&fvx, &fvy, &fvw, &fvh);
        long nonblack = 0, total = 0;
        if (fb_final && fvw > 0 && fvh > 0) {
            for (int yy = fvy; yy < fvy + fvh; ++yy) {
                const uint16_t *row = fb_final + yy * MDC_MAX_W;
                for (int xx = fvx; xx < fvx + fvw; ++xx) {
                    if (row[xx] != 0) ++nonblack;
                    ++total;
                }
            }
        }
        if (fb_dump_path && fb_final) {
            FILE *df = fopen(fb_dump_path, "wb");
            if (df) {
                fwrite(fb_final, sizeof(uint16_t),
                       (size_t)MDC_MAX_W * MDC_MAX_H, df);
                fclose(df);
            } else {
                fprintf(stderr, "fb dump: fopen %s failed\n", fb_dump_path);
            }
        }
        double peak_pct = peak_total
            ? (100.0 * (double)peak_nonblack / (double)peak_total) : 0.0;
        printf("MDHOST_SUMMARY frames=%d viewport=%dx%d@(%d,%d) "
               "refresh=%d nonblack=%ld total=%ld pct=%.1f "
               "peak_pct=%.1f peak_frame=%d\n",
               frame, fvw, fvh, fvx, fvy, mdc_refresh_rate(),
               nonblack, total,
               total ? (100.0 * (double)nonblack / (double)total) : 0.0,
               peak_pct, peak_frame);
        fflush(stdout);
    }

    if (wav_f) {
        /* Patch RIFF + data chunk lengths now that we know the total. */
        unsigned int data_size = (unsigned int)wav_data_bytes;
        unsigned int riff_size = 36 + data_size;
        unsigned char tmp[4];
        fseek(wav_f, 4, SEEK_SET);
        tmp[0] = (unsigned char)(riff_size);
        tmp[1] = (unsigned char)(riff_size >> 8);
        tmp[2] = (unsigned char)(riff_size >> 16);
        tmp[3] = (unsigned char)(riff_size >> 24);
        fwrite(tmp, 1, 4, wav_f);
        fseek(wav_f, 40, SEEK_SET);
        tmp[0] = (unsigned char)(data_size);
        tmp[1] = (unsigned char)(data_size >> 8);
        tmp[2] = (unsigned char)(data_size >> 16);
        tmp[3] = (unsigned char)(data_size >> 24);
        fwrite(tmp, 1, 4, wav_f);
        fclose(wav_f);
    }

    mdc_shutdown();
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
