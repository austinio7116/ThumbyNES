/*
 * ThumbyNES — mdbench variant that drives mdc_load_rom_xip() against a
 * read-only mmap. Simulates the device's XIP-borrow load path so we
 * can reproduce device-only crashes on host.
 *
 * Usage: mdbench_xip <rom.md>
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "md_core.h"

#include <stdint.h>

/* Dump the current framebuffer as PPM (P6, RGB 8:8:8) so we can
 * visually verify rendering without SDL. */
static void dump_ppm(const char *path, const uint16_t *fb, int w, int h,
                     int vx, int vy, int vw, int vh)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", vw, vh);
    for (int y = 0; y < vh; y++) {
        for (int x = 0; x < vw; x++) {
            uint16_t px = fb[(vy + y) * w + (vx + x)];
            /* PicoDrive native is RGB555 (PDF_RGB555): bbbbb gggggg rrrrr
             * (actually 555 with bit15=0; check md_core.c) */
            uint8_t r = (px >> 11) & 0x1F;
            uint8_t g = (px >>  5) & 0x3F;
            uint8_t b = (px      ) & 0x1F;
            uint8_t rgb[3] = { (uint8_t)(r << 3), (uint8_t)(g << 2), (uint8_t)(b << 3) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* Sum all non-zero pixels across the active viewport for a cheap
 * "is anything rendered?" check. */
static uint64_t fb_nonzero_count(const uint16_t *fb, int w,
                                  int vx, int vy, int vw, int vh)
{
    uint64_t n = 0;
    for (int y = 0; y < vh; y++)
        for (int x = 0; x < vw; x++)
            if (fb[(vy + y) * w + (vx + x)] != 0) n++;
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.md [frame-count] [dump.ppm]\n", argv[0]);
        return 1;
    }
    int bench_frames = (argc >= 3) ? atoi(argv[2]) : 600;
    const char *dump_path = (argc >= 4) ? argv[3] : "md_shot.ppm";

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
    size_t sz = (size_t)st.st_size;

    void *p = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);

    if (mdc_init(MDC_REGION_AUTO, 22050) != 0) {
        fprintf(stderr, "mdc_init failed\n"); return 1;
    }

    fprintf(stderr, "xip-test: calling mdc_load_rom_xip against %p (%zu bytes, read-only mmap)\n", p, sz);
    if (mdc_load_rom_xip((const uint8_t *)p, sz) != 0) {
        fprintf(stderr, "mdc_load_rom_xip failed\n"); return 1;
    }

    int vx, vy, vw, vh;
    mdc_viewport(&vx, &vy, &vw, &vh);
    printf("loaded %s (%zu bytes), viewport %dx%d at (%d,%d), %d Hz\n",
           argv[1], sz, vw, vh, vx, vy, mdc_refresh_rate());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < bench_frames; i++) mdc_run_frame();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames in %.3fs = %.1f fps\n", bench_frames, sec, bench_frames / sec);

    /* Pull framebuffer + dump. */
    const uint16_t *fb = mdc_framebuffer();
    if (fb) {
        mdc_viewport(&vx, &vy, &vw, &vh);   /* refresh; game may have switched */
        uint64_t n = fb_nonzero_count(fb, MDC_MAX_W, vx, vy, vw, vh);
        /* Also check the FULL frame (320x240) not just the reported
         * viewport — maybe PicoDrive is drawing somewhere but our
         * viewport reporting is wrong. */
        uint64_t nfull = 0;
        for (int y = 0; y < MDC_MAX_H; y++)
            for (int x = 0; x < MDC_MAX_W; x++)
                if (fb[y*MDC_MAX_W + x] != 0) nfull++;
        printf("viewport now %dx%d at (%d,%d); non-zero in viewport = %llu / %d; full-frame non-zero = %llu / %d\n",
               vw, vh, vx, vy,
               (unsigned long long)n, vw * vh,
               (unsigned long long)nfull, MDC_MAX_W * MDC_MAX_H);
        /* Dump the FULL 320x240, not just the viewport, so we see any
         * out-of-viewport pixel activity. */
        dump_ppm(dump_path, fb, MDC_MAX_W, MDC_MAX_H, 0, 0, MDC_MAX_W, MDC_MAX_H);
        printf("screenshot saved → %s\n", dump_path);
    } else {
        printf("mdc_framebuffer() returned NULL (MD_LINE_SCRATCH build?) — no dump\n");
    }

    mdc_shutdown();
    munmap(p, sz);
    return 0;
}
