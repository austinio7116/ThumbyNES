/*
 * ThumbyNES Phase 0 bench harness — SMS edition.
 *
 * Boots smsplus, runs a fixed number of frames against a .sms / .gg ROM
 * on the command line, prints fps. Sibling of bench_main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sms_core.h"

#define BENCH_FRAMES 600

int main(int argc, char **argv)
{
    if (smsc_init(SMSC_SYS_AUTO, 22050) != 0) {
        fprintf(stderr, "smsc_init failed\n");
        return 1;
    }

    if (argc < 2) {
        printf("smsbench: smsc_init OK (no ROM supplied)\n");
        smsc_shutdown();
        return 0;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Skip 512-byte ROM header if present (some old dumps). */
    long off = ((sz > 0x4000) && ((sz / 512) & 1)) ? 512 : 0;
    long rom_sz = sz - off;
    fseek(f, off, SEEK_SET);
    unsigned char *buf = malloc(rom_sz);
    if (fread(buf, 1, rom_sz, f) != (size_t)rom_sz) {
        fprintf(stderr, "short read\n"); return 1;
    }
    fclose(f);

    if (smsc_load_rom(buf, rom_sz) != 0) {
        fprintf(stderr, "ROM load failed\n");
        return 1;
    }
    /* smsc_load_rom borrows the pointer when the size is 16 KB-aligned;
     * we must keep `buf` alive until smsc_shutdown(). Don't free here. */

    printf("loaded %s (%ld bytes), %s, %d Hz refresh\n",
           argv[1], rom_sz, smsc_is_gg() ? "GG" : "SMS",
           smsc_refresh_rate());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < BENCH_FRAMES; i++) smsc_run_frame();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames in %.3fs = %.1f fps\n", BENCH_FRAMES, sec, BENCH_FRAMES / sec);

    smsc_shutdown();
    return 0;
}
