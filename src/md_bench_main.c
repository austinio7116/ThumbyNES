/*
 * ThumbyNES Phase 0 bench harness — MD / Genesis edition.
 *
 * Boots PicoDrive, runs a fixed number of frames against a .md / .bin /
 * .gen ROM on the command line, prints fps. Sibling of sms_bench_main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "md_core.h"

#define BENCH_FRAMES 600

int main(int argc, char **argv)
{
    if (mdc_init(MDC_REGION_AUTO, 22050) != 0) {
        fprintf(stderr, "mdc_init failed\n");
        return 1;
    }

    if (argc < 2) {
        printf("mdbench: mdc_init OK (no ROM supplied)\n");
        mdc_shutdown();
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

    if (mdc_load_rom(buf, sz) != 0) {
        fprintf(stderr, "ROM load failed\n");
        return 1;
    }
    /* Our wrapper copied the ROM, so we can free the reader buffer. */
    free(buf);

    int vx, vy, vw, vh;
    mdc_viewport(&vx, &vy, &vw, &vh);
    printf("loaded %s (%ld bytes), viewport %dx%d at (%d,%d), %d Hz refresh\n",
           argv[1], sz, vw, vh, vx, vy, mdc_refresh_rate());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < BENCH_FRAMES; i++) mdc_run_frame();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames in %.3fs = %.1f fps\n", BENCH_FRAMES, sec, BENCH_FRAMES / sec);

    mdc_shutdown();
    return 0;
}
