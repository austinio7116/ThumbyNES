/*
 * ThumbyNES Phase 0 bench harness — Game Boy edition.
 *
 * Boots peanut_gb, runs a fixed number of frames against a .gb ROM
 * on the command line, prints fps. Sibling of bench_main.c and
 * sms_bench_main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gb_core.h"

#define BENCH_FRAMES 600

int main(int argc, char **argv)
{
    if (gbc_init(22050) != 0) {
        fprintf(stderr, "gbc_init failed\n");
        return 1;
    }

    if (argc < 2) {
        printf("gbbench: gbc_init OK (no ROM supplied)\n");
        gbc_shutdown();
        return 0;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "short read\n"); return 1;
    }
    fclose(f);

    int rc = gbc_load_rom(buf, sz);
    if (rc != 0) {
        fprintf(stderr, "ROM load failed (%d)\n", rc);
        return 1;
    }

    printf("loaded %s (%ld bytes), %d Hz refresh\n",
           argv[1], sz, gbc_refresh_rate());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < BENCH_FRAMES; i++) gbc_run_frame();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames in %.3fs = %.1f fps\n", BENCH_FRAMES, sec, BENCH_FRAMES / sec);

    gbc_shutdown();
    /* buf is borrowed by peanut_gb during the session — free after shutdown. */
    free(buf);
    return 0;
}
