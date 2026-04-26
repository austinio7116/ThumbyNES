/*
 * ThumbyNES Phase 0 bench harness — PC Engine / TurboGrafx-16 edition.
 *
 * Boots HuExpress, runs a fixed number of frames against a .pce ROM on
 * the command line, prints fps. Sibling of md_bench_main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pce_core.h"

#define BENCH_FRAMES 600

int main(int argc, char **argv)
{
    if (pcec_init(PCEC_REGION_AUTO, 22050) != 0) {
        fprintf(stderr, "pcec_init failed\n");
        return 1;
    }

    if (argc < 2) {
        printf("pcebench: pcec_init OK (no ROM supplied)\n");
        pcec_shutdown();
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

    /* US TurboGrafx-16 carts are bit-reversed relative to native PCE.
     * Detect and decrypt in place before handing to the core. On device
     * this happens once, written back to flash; on host we just mutate
     * the read buffer. */
    if (pcec_rom_is_us_encoded(buf, sz)) {
        printf("US-encoded cart detected — decrypting %ld bytes\n", sz);
        pcec_rom_decrypt_us(buf, sz);
    }

    if (pcec_load_rom(buf, sz) != 0) {
        fprintf(stderr, "ROM load failed\n");
        return 1;
    }
    /* HuExpress holds the ROM pointer — do NOT free buf until shutdown. */

    int vx, vy, vw, vh;
    pcec_viewport(&vx, &vy, &vw, &vh);
    printf("loaded %s (%ld bytes), viewport %dx%d at (%d,%d), %d Hz refresh\n",
           argv[1], sz, vw, vh, vx, vy, pcec_refresh_rate());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < BENCH_FRAMES; i++) pcec_run_frame();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames in %.3fs = %.1f fps\n",
           BENCH_FRAMES, sec, BENCH_FRAMES / sec);

    /* Framebuffer scan removed: under PCE_SCANLINE_RENDER the core
     * writes pixels straight into the LCD fb passed to
     * pcec_set_scale_target — pcec_framebuffer() is a 1-byte stub.
     * The bench just confirms the cart boots and runs 600 frames. */

    pcec_shutdown();
    free(buf);
    return 0;
}
