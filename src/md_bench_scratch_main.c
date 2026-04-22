/*
 * ThumbyNES — mdbench variant that exercises the MD_LINE_SCRATCH
 * render path on host. Same setup the device uses:
 *   mdc_init + mdc_load_rom, PicoDrawSetOutBuf(scratch, 0),
 *   mdc_set_scale_target(lcd_fb_128x128, FIT, vx,vy,vw,vh, 0,0) per frame,
 *   md_core_scan_end downsamples into lcd_fb.
 *
 * If this harness renders correctly, the device bug is hardware-/
 * timing-specific. If it's black, the device bug is in the scan
 * callback and we can iterate fast from here.
 *
 * Usage: mdbench_scratch <rom.md> [frame-count] [dump.ppm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "md_core.h"

#define LCD_W 128
#define LCD_H 128

static uint16_t lcd_fb[LCD_W * LCD_H];

static void dump_ppm(const char *path, const uint16_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint16_t px = fb[i];
        uint8_t r = (px >> 11) & 0x1F;
        uint8_t g = (px >>  5) & 0x3F;
        uint8_t b = (px      ) & 0x1F;
        uint8_t rgb[3] = { (uint8_t)(r << 3), (uint8_t)(g << 2), (uint8_t)(b << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.md [frames] [out.ppm] [--xip]\n", argv[0]);
        return 1;
    }
    int bench_frames = (argc >= 3) ? atoi(argv[2]) : 600;
    const char *dump_path = (argc >= 4) ? argv[3] : "md_scratch.ppm";
    int use_xip = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--xip") == 0) use_xip = 1;

    long sz;
    unsigned char *buf = NULL;
    void *xip_ptr = NULL;

    if (use_xip) {
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) { perror(argv[1]); return 1; }
        struct stat st;
        if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
        sz = st.st_size;
        xip_ptr = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (xip_ptr == MAP_FAILED) { perror("mmap"); return 1; }
        close(fd);
        fprintf(stderr, "using XIP-borrow path: mmap %p, %ld bytes\n", xip_ptr, sz);
    } else {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc(sz);
        if (!buf || fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read failed\n"); return 1; }
        fclose(f);
    }

    if (mdc_init(MDC_REGION_AUTO, 22050) != 0) {
        fprintf(stderr, "mdc_init failed\n"); return 1;
    }
    if (use_xip) {
        if (mdc_load_rom_xip((const uint8_t *)xip_ptr, sz) != 0) {
            fprintf(stderr, "mdc_load_rom_xip failed\n"); return 1;
        }
    } else {
        if (mdc_load_rom(buf, sz) != 0) {
            fprintf(stderr, "mdc_load_rom failed\n"); return 1;
        }
        free(buf);
    }

    int vx, vy, vw, vh;
    mdc_viewport(&vx, &vy, &vw, &vh);
    printf("loaded %s (%ld bytes), viewport %dx%d at (%d,%d), %d Hz\n",
           argv[1], sz, vw, vh, vx, vy, mdc_refresh_rate());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < bench_frames; i++) {
        /* Per-frame scale target, matching md_run.c on device. FIT
         * mode (letterbox 128x90). Clear letterbox rows each frame. */
        mdc_viewport(&vx, &vy, &vw, &vh);
        memset(lcd_fb, 0, LCD_W * 19 * 2);
        memset(lcd_fb + (19 + 90) * LCD_W, 0, LCD_W * 19 * 2);
        mdc_set_scale_target(lcd_fb, /*FIT=*/0, vx, vy, vw, vh, 0, 0);
        mdc_run_frame();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames in %.3fs = %.1f fps\n", bench_frames, sec, bench_frames / sec);

    uint64_t nz = 0;
    for (int i = 0; i < LCD_W * LCD_H; i++) if (lcd_fb[i] != 0) nz++;
    printf("non-zero pixels in lcd_fb (128x128): %llu / %d\n",
           (unsigned long long)nz, LCD_W * LCD_H);
    dump_ppm(dump_path, lcd_fb, LCD_W, LCD_H);
    printf("screenshot saved → %s\n", dump_path);

    mdc_shutdown();
    return 0;
}
