/*
 * ThumbyNES Phase 0 bench harness.
 *
 * Boots the NES core, runs a fixed number of frames against a ROM
 * supplied on the command line, and prints frames-per-second. No
 * graphics, no audio, no input — this exists purely so Phase 0 can
 * prove the vendored Nofrendo core compiles and links cleanly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "nes_core.h"

#define BENCH_FRAMES 600

int main(int argc, char **argv)
{
    if (nesc_init(44100) != 0) {
        fprintf(stderr, "nesc_init failed\n");
        return 1;
    }

    if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = malloc(sz);
        fread(buf, 1, sz, f);
        fclose(f);
        if (nesc_load_rom(buf, sz) != 0) {
            fprintf(stderr, "ROM load not yet implemented (Phase 1)\n");
            /* Phase 0 still considers a successful link a win. */
            return 0;
        }
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < BENCH_FRAMES; i++) nesc_run_frame();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("%d frames in %.3fs = %.1f fps\n", BENCH_FRAMES, sec, BENCH_FRAMES / sec);
    } else {
        printf("ThumbyNES bench: nesc_init OK (no ROM supplied)\n");
    }

    nesc_shutdown();
    return 0;
}
