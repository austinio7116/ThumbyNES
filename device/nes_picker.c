/*
 * ThumbyNES — ROM picker (Phase 3 / 5).
 *
 * Phase 3 ships the file scanner and a "no ROMs" splash. The
 * interactive list view (Phase 5) is wired up here too — it draws
 * a paged list of file names and lets the user pick one with the
 * D-pad and A button. If the disk is empty the splash stays up
 * and the main loop keeps pumping USB so the user can drag ROMs
 * onto the volume; once tud_msc activity goes idle and a rescan
 * finds files, the picker swaps from splash → list automatically.
 */
#include "nes_picker.h"
#include "nes_font.h"
#include "nes_lcd_gc9107.h"
#include "nes_buttons.h"
#include "nes_thumb.h"
#include "nes_menu.h"
#include "nes_battery.h"

#ifdef THUMBYONE_SLOT_MODE
#  include "thumbyone_fs_stats.h"
#  include "thumbyone_settings.h"
#  include "thumbyone_backlight.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#ifndef THUMBYONE_SLOT_MODE
#include "tusb.h"
#endif
#include "ff.h"
#ifdef THUMBYONE_SLOT_MODE
#include "thumbyone_handoff.h"
#endif

#include "nes_flash_disk.h"

#define BTN_LB_GP 6   /* mirror nes_buttons.c — used for tab nav   */
#define BTN_RB_GP 22

/* The global FATFS object lives in nes_device_main.c. We need it
 * here to walk the cluster chain when computing a ROM file's XIP
 * address for the zero-copy load path. */
extern FATFS g_fs;

#define FB_W 128
#define FB_H 128

#ifdef THUMBYONE_SLOT_MODE
/* Lobby owns USB in slot mode — no MSC timestamp, and tud_task() is
 * a no-op because tinyUSB isn't compiled into this slot. Macro both
 * out at translation time so the call sites below stay readable. */
#define tud_task()             do { } while (0)
static volatile uint64_t g_msc_last_op_us = 0;
#else
extern volatile uint64_t g_msc_last_op_us;
#endif

/* --- favorites store ------------------------------------------------ */

/* ROMS_DIR / ROMS_DIR_SLASH are defined in nes_picker.h so every
 * ThumbyNES source file agrees on the layout. Under slot mode all
 * content + sidecars + state files live in /roms/. */

/* Favourites: a plain newline-separated list of base file names that
 * the user has marked as favorites. Kept in a small RAM buffer,
 * mutated in place, written back on picker exit if anything changed.
 * 4 KB holds ~80 ROM names, more than fits in the 64-entry
 * NES_PICKER_MAX_ROMS cap anyway. */
#define FAVS_PATH      ROMS_DIR_SLASH ".favs"
#define FAVS_BUF_SIZE  4096

static char   favs_buf[FAVS_BUF_SIZE];
static size_t favs_len   = 0;
static bool   favs_dirty = false;

static void favs_load(void) {
    favs_len = 0;
    favs_dirty = false;
    FIL f;
    if (f_open(&f, FAVS_PATH, FA_READ) != FR_OK) return;
    UINT br = 0;
    f_read(&f, favs_buf, FAVS_BUF_SIZE - 1, &br);
    f_close(&f);
    favs_len = br;
    favs_buf[favs_len] = 0;
}

static void favs_save(void) {
    if (!favs_dirty) return;
    FIL f;
    if (f_open(&f, FAVS_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, favs_buf, (UINT)favs_len, &bw);
    f_close(&f);
    favs_dirty = false;
}

/* Find a name as a complete line in favs_buf. Returns the byte
 * offset of the line start, or -1 if not present. */
static int favs_find(const char *name) {
    size_t name_len = strlen(name);
    size_t i = 0;
    while (i < favs_len) {
        size_t j = i;
        while (j < favs_len && favs_buf[j] != '\n') j++;
        size_t line_len = j - i;
        if (line_len == name_len && memcmp(&favs_buf[i], name, name_len) == 0)
            return (int)i;
        i = j + 1;
    }
    return -1;
}

static int nes_picker_is_favorite(const char *name) {
    return favs_find(name) >= 0;
}

static void favs_toggle(const char *name) {
    int off = favs_find(name);
    if (off >= 0) {
        /* Remove the existing line. */
        size_t end = (size_t)off;
        while (end < favs_len && favs_buf[end] != '\n') end++;
        if (end < favs_len) end++;
        size_t remove_len = end - (size_t)off;
        memmove(&favs_buf[off], &favs_buf[end], favs_len - end);
        favs_len -= remove_len;
        favs_buf[favs_len] = 0;
    } else {
        /* Append a new line. */
        size_t name_len = strlen(name);
        if (favs_len + name_len + 2 >= FAVS_BUF_SIZE) return;
        memcpy(&favs_buf[favs_len], name, name_len);
        favs_len += name_len;
        favs_buf[favs_len++] = '\n';
        favs_buf[favs_len] = 0;
    }
    favs_dirty = true;
}

/* --- file scan ------------------------------------------------------ */

/* Read the 16-byte iNES header for `fname`. Returns 0 on success
 * and a 16-byte buffer that the caller can poke at for mapper /
 * region / flags. Returns nonzero if the file can't be read or the
 * iNES magic isn't present. */
static int read_ines_header(const char *fname, uint8_t hdr[16]) {
    char path[NES_PICKER_PATH_MAX];
    snprintf(path, sizeof(path), ROMS_DIR_SLASH "%s", fname);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    FRESULT r = f_read(&f, hdr, 16, &br);
    f_close(&f);
    if (r != FR_OK || br != 16) return -2;
    if (hdr[0] != 'N' || hdr[1] != 'E' || hdr[2] != 'S' || hdr[3] != 0x1A)
        return -3;
    return 0;
}

/* Detect PAL vs NTSC from a combination of three signals — any one
 * of them firing classifies the ROM as PAL. Headers are notoriously
 * unreliable on real-world dumps, so the filename heuristic is the
 * most useful in practice (no-intro / GoodNES naming flags region
 * in parentheses).
 *
 *   1. iNES 2.0 byte 12 bits 0..1 == 1 → PAL
 *   2. iNES 1.0 byte 9 bit 0 == 1     → PAL
 *   3. Filename contains any of (E), (Europe), (PAL), (A), (Australia)
 */
static int filename_says_pal(const char *fname) {
    /* Case-insensitive substring search for the common region tags. */
    static const char *needles[] = {
        "(e)", "(eu)", "(europe)", "(pal)", "(a)", "(australia)",
        " (e ", " (e)", "[!p]",
    };
    char lower[NES_PICKER_NAME_MAX];
    size_t L = strlen(fname);
    if (L >= sizeof(lower)) L = sizeof(lower) - 1;
    for (size_t i = 0; i < L; i++) {
        char c = fname[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[L] = 0;
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        if (strstr(lower, needles[i])) return 1;
    }
    return 0;
}

static uint8_t detect_region(const char *fname, const uint8_t hdr[16], int header_ok) {
    if (header_ok) {
        int is_ines2 = ((hdr[7] >> 2) & 0x03) == 2;
        if (is_ines2) {
            if ((hdr[12] & 0x03) == 1) return 1;   /* PAL */
        } else {
            if (hdr[9] & 0x01) return 1;            /* PAL */
        }
    }
    if (filename_says_pal(fname)) return 1;
    return 0;
}

/* Pull mapper number from the iNES header. */
static uint8_t mapper_from_header(const uint8_t hdr[16]) {
    return (uint8_t)(((hdr[6] >> 4) & 0x0F) | (hdr[7] & 0xF0));
}

int nes_picker_scan(nes_rom_entry *out, int max) {
    DIR dir;
    FILINFO info;
    if (f_opendir(&dir, ROMS_DIR) != FR_OK) return 0;
    int n = 0;
    while (n < max && f_readdir(&dir, &info) == FR_OK) {
        if (info.fname[0] == 0) break;
        if (info.fattrib & AM_DIR) continue;
        size_t L = strlen(info.fname);
        if (L < 4) continue;
        uint8_t sys = 0xFF;
        if      (strcasecmp(info.fname + L - 4, ".nes") == 0) sys = ROM_SYS_NES;
        else if (strcasecmp(info.fname + L - 4, ".sms") == 0) sys = ROM_SYS_SMS;
        else if (L >= 3 && strcasecmp(info.fname + L - 3, ".gb")  == 0) sys = ROM_SYS_GB;
        else if (L >= 3 && strcasecmp(info.fname + L - 3, ".gg")  == 0) sys = ROM_SYS_GG;
        else continue;
        strncpy(out[n].name, info.fname, NES_PICKER_NAME_MAX - 1);
        out[n].name[NES_PICKER_NAME_MAX - 1] = 0;
        out[n].size   = (uint32_t)info.fsize;
        out[n].system = sys;
        if (sys == ROM_SYS_NES) {
            uint8_t hdr[16];
            int header_ok = (read_ines_header(info.fname, hdr) == 0);
            out[n].mapper   = header_ok ? mapper_from_header(hdr) : 0xFF;
            out[n].pal_hint = detect_region(info.fname, hdr, header_ok);
        } else {
            /* SMS / GG: smsplus does its own region work; we just use
             * the filename heuristic for the picker meta column. */
            out[n].mapper   = 0xFF;
            out[n].pal_hint = filename_says_pal(info.fname);
        }
        n++;
    }
    f_closedir(&dir);
    return n;
}

uint8_t *nes_picker_load_rom(const char *name, size_t *out_len) {
    char path[NES_PICKER_PATH_MAX];
    snprintf(path, sizeof(path), ROMS_DIR_SLASH "%s", name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FSIZE_t sz = f_size(&f);
    /* Reject anything obviously larger than the available heap up
     * front — saves a malloc-then-fail churn. The XIP mmap path
     * handles the multi-megabyte cases. */
    if (sz < 16 || sz > 256 * 1024) { f_close(&f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { f_close(&f); return NULL; }
    UINT br;
    if (f_read(&f, buf, (UINT)sz, &br) != FR_OK || br != sz) {
        free(buf); f_close(&f); return NULL;
    }
    f_close(&f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* Convert a FatFs cluster number to its absolute LBA on the
 * physical disk. Cluster 2 is the first data cluster. */
static LBA_t cluster_to_lba(DWORD cluster) {
    return g_fs.database + (LBA_t)(cluster - 2) * g_fs.csize;
}

/* Walk the FAT chain starting at `start_cluster` and verify it is
 * physically contiguous. Returns 1 if every cluster in the chain
 * is the previous cluster + 1; 0 otherwise. We can only mmap
 * contiguous files because the XIP region is a flat physical map.
 *
 * This pokes FatFs internals via the public-but-undocumented
 * `get_fat()` helper, which we replace with a sector-level read
 * of the FAT itself to stay portable across FatFs configs. */
static int chain_is_contiguous(DWORD start_cluster, DWORD n_clusters) {
    if (n_clusters <= 1) return 1;

    /* The FAT lives at sector `g_fs.fatbase` and each FAT16 entry
     * is 2 bytes. Read the table sector-by-sector via diskio. */
    DWORD prev = start_cluster;
    DWORD sector_buf_lba = (DWORD)-1;
    static uint8_t fat_sec[512];

    for (DWORD i = 1; i < n_clusters; i++) {
        DWORD entry_byte = prev * 2;     /* FAT16 */
        DWORD entry_sec  = entry_byte / 512;
        DWORD entry_off  = entry_byte % 512;
        LBA_t lba = (LBA_t)g_fs.fatbase + entry_sec;
        if (lba != sector_buf_lba) {
            if (nes_flash_disk_read(fat_sec, (uint32_t)lba, 1) != 0) return 0;
            sector_buf_lba = lba;
        }
        DWORD next = (DWORD)fat_sec[entry_off] | ((DWORD)fat_sec[entry_off + 1] << 8);
        if (next == 0xFFFF) {
            /* End-of-chain. Should only happen at i == n_clusters - 1.
             * If it happens earlier the file is shorter than expected. */
            return (i == n_clusters - 1);
        }
        if (next != prev + 1) return 0;
        prev = next;
    }
    return 1;
}

/* Cheap "is this file contiguous" probe used by the defragmenter to
 * decide which files to rewrite. Same logic the mmap path uses but
 * exposed locally so we can call it without the size checks. */
static int file_is_contiguous(const char *name) {
    char path[NES_PICKER_PATH_MAX];
    snprintf(path, sizeof(path), ROMS_DIR_SLASH "%s", name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 1;  /* assume yes if we can't tell */
    FSIZE_t sz = f_size(&f);
    DWORD   sc = f.obj.sclust;
    f_close(&f);
    if (sz == 0 || sc < 2) return 1;
    DWORD bpc = (DWORD)g_fs.csize * 512u;
    DWORD nc  = ((DWORD)sz + bpc - 1) / bpc;
    return chain_is_contiguous(sc, nc) ? 1 : 0;
}

/* --- defragmenter --------------------------------------------------- */

/* fb helpers + colour palette — defined here so the defragmenter
 * progress overlay below can use them. The picker / hero / list
 * draw paths further down also reference these names. */
static void fb_clear(uint16_t *fb, uint16_t c) {
    for (int i = 0; i < FB_W * FB_H; i++) fb[i] = c;
}
static void fb_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= FB_H) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if ((unsigned)xx >= FB_W) continue;
            fb[yy * FB_W + xx] = c;
        }
    }
}

#define COL_BG     0x0000   /* black */
#define COL_FG     0xFFFF   /* white */
#define COL_DIM    0x8410   /* mid grey */
#define COL_HIGHLT 0x07E0   /* green */
#define COL_TITLE  0xFD20   /* orange */
#define COL_ERR    0xF800   /* red */

#define DEFRAG_TMP   "/.defrag.tmp"
#define DEFRAG_BUFSZ 4096

static void defrag_progress(uint16_t *fb, const char *name, int done, int total) {
    fb_clear(fb, COL_BG);
    nes_font_draw(fb, "DEFRAGMENTING",     16, 24, COL_TITLE);
    nes_font_draw(fb, "do not unplug",     22, 36, COL_DIM);
    char line[40];
    snprintf(line, sizeof(line), "%d / %d", done, total);
    int w = nes_font_width(line);
    nes_font_draw(fb, line, (FB_W - w) / 2, 56, COL_FG);
    if (name) {
        char nm[24];
        strncpy(nm, name, sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = 0;
        if (strlen(nm) > 22) nm[22] = 0;
        int nw = nes_font_width(nm);
        nes_font_draw(fb, nm, (FB_W - nw) / 2, 72, COL_FG);
    }
    /* Progress bar. */
    int bar_x = 12, bar_y = 92, bar_w = FB_W - 24, bar_h = 6;
    fb_rect(fb, bar_x, bar_y, bar_w, bar_h, 0x18C3);
    int fill = total > 0 ? (bar_w * done) / total : 0;
    if (fill > bar_w) fill = bar_w;
    fb_rect(fb, bar_x, bar_y, fill, bar_h, COL_HIGHLT);
    nes_lcd_wait_idle();
    nes_lcd_present(fb);
    tud_task();
}

/* Rewrite a single file via the f_expand temp-file dance. The
 * temp file is allocated as a contiguous chain via f_expand, the
 * original is streamed into it 4 KB at a time, and on success the
 * original is unlinked and the temp is renamed in its place. */
static int defrag_one(const char *name, uint16_t *fb, int done, int total) {
    char src_path[NES_PICKER_PATH_MAX], tmp_path[16];
    snprintf(src_path, sizeof(src_path), ROMS_DIR_SLASH "%s", name);
    snprintf(tmp_path, sizeof(tmp_path), DEFRAG_TMP);

    FIL src;
    if (f_open(&src, src_path, FA_READ) != FR_OK) return -1;
    FSIZE_t sz = f_size(&src);

    /* Make sure no leftover temp from a previous interrupted run is
     * holding clusters we want. */
    f_unlink(tmp_path);

    FIL dst;
    if (f_open(&dst, tmp_path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        f_close(&src);
        return -2;
    }
    /* Pre-allocate a contiguous cluster chain of exactly the right
     * size. opt=1 → "prepare to allocate". The file is then writable
     * normally and stays contiguous. */
    if (f_expand(&dst, sz, 1) != FR_OK) {
        f_close(&dst); f_close(&src); f_unlink(tmp_path);
        return -3;   /* not enough contiguous free space */
    }

    static uint8_t buf[DEFRAG_BUFSZ];
    UINT br, bw;
    FSIZE_t copied = 0;
    while (copied < sz) {
        UINT want = (sz - copied > DEFRAG_BUFSZ) ? DEFRAG_BUFSZ : (UINT)(sz - copied);
        if (f_read(&src, buf, want, &br) != FR_OK || br != want) {
            f_close(&dst); f_close(&src); f_unlink(tmp_path);
            return -4;
        }
        if (f_write(&dst, buf, br, &bw) != FR_OK || bw != br) {
            f_close(&dst); f_close(&src); f_unlink(tmp_path);
            return -5;
        }
        copied += br;
        /* Repaint the progress bar with the per-file fraction so
         * the user sees movement on the bigger ROMs. */
        if ((copied & 0xFFFF) == 0) {
            defrag_progress(fb, name, done, total);
        }
    }
    f_close(&dst);
    f_close(&src);

    /* Atomically replace the original. */
    if (f_unlink(src_path) != FR_OK) {
        f_unlink(tmp_path);
        return -6;
    }
    if (f_rename(tmp_path, src_path) != FR_OK) {
        return -7;   /* original is gone — temp still has the data */
    }

    /* Make sure the new file's clusters land in flash before the
     * next file's f_expand walks the FAT. */
    nes_flash_disk_flush();
    return 0;
}

int nes_picker_defrag(uint16_t *fb) {
    /* Pass 1: collect names of fragmented files. We can't iterate
     * the directory while we mutate it, so snapshot first. */
    static char frag_names[NES_PICKER_MAX_ROMS][NES_PICKER_NAME_MAX];
    int n_frag = 0;

    DIR dir;
    FILINFO info;
    if (f_opendir(&dir, ROMS_DIR) != FR_OK) return -1;
    while (n_frag < NES_PICKER_MAX_ROMS && f_readdir(&dir, &info) == FR_OK) {
        if (info.fname[0] == 0) break;
        if (info.fattrib & AM_DIR) continue;
        /* Skip the picker's own bookkeeping files — they're tiny and
         * change often. */
        if (info.fname[0] == '.') continue;
        if (info.fsize < 64 * 1024) continue;   /* small files don't matter */
        if (file_is_contiguous(info.fname)) continue;
        strncpy(frag_names[n_frag], info.fname, NES_PICKER_NAME_MAX - 1);
        frag_names[n_frag][NES_PICKER_NAME_MAX - 1] = 0;
        n_frag++;
    }
    f_closedir(&dir);

    if (n_frag == 0) return 0;

    /* Pass 2: rewrite each fragmented file. */
    int rewritten = 0;
    for (int i = 0; i < n_frag; i++) {
        defrag_progress(fb, frag_names[i], i, n_frag);
        if (defrag_one(frag_names[i], fb, i, n_frag) == 0) {
            rewritten++;
        }
        tud_task();
    }
    defrag_progress(fb, "done", n_frag, n_frag);
    nes_flash_disk_flush();
    return rewritten;
}

int nes_picker_mmap_rom(const char *name,
                          const uint8_t **out_data, size_t *out_len) {
    if (!out_data || !out_len) return -1;

    /* Make sure any pending writes for this file have been flushed
     * to physical flash before we try to read it via XIP. */
    nes_flash_disk_flush();

    char path[NES_PICKER_PATH_MAX];
    snprintf(path, sizeof(path), ROMS_DIR_SLASH "%s", name);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -2;
    FSIZE_t sz = f_size(&f);
    /* XIP mmap supports up to 8 MB — well past any GB / SMS / NES cart. */
    if (sz < 16 || sz > 8 * 1024 * 1024) { f_close(&f); return -3; }

    DWORD start_cluster = f.obj.sclust;
    f_close(&f);
    if (start_cluster < 2) return -4;

    /* Round file size up to whole clusters when checking contiguity. */
    DWORD bytes_per_cluster = (DWORD)g_fs.csize * 512u;
    DWORD n_clusters = ((DWORD)sz + bytes_per_cluster - 1) / bytes_per_cluster;
    if (!chain_is_contiguous(start_cluster, n_clusters)) return -5;

    /* Compute XIP address. The flash disk lives at FLASH_DISK_OFFSET
     * inside the QSPI flash; XIP_BASE maps the start of flash. */
    LBA_t lba = cluster_to_lba(start_cluster);
    uintptr_t xip = (uintptr_t)XIP_BASE + (uintptr_t)FLASH_DISK_OFFSET
                  + (uintptr_t)lba * 512u;
    *out_data = (const uint8_t *)xip;
    *out_len  = (size_t)sz;
    return 0;
}

/* --- drawing helpers ----------------------------------------------- */

/* --- splash screens ------------------------------------------------ */

static void draw_no_roms_splash(uint16_t *fb) {
    fb_clear(fb, COL_BG);
    nes_font_draw(fb, "ThumbyNES", 32, 30, COL_TITLE);
    nes_font_draw(fb, "no roms found",  20, 50, COL_FG);
    nes_font_draw(fb, "drag .nes files", 14, 70, COL_DIM);
    nes_font_draw(fb, "to usb drive",    22, 78, COL_DIM);
    nes_font_draw(fb, "then eject",      28, 92, COL_DIM);
}

/* --- shared row helpers -------------------------------------------- */

/* Strip a trailing recognised extension so the row reads cleanly. */
static void name_no_ext(char *dst, size_t dstsz, const char *src) {
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = 0;
    size_t L = strlen(dst);
    if (L >= 4 && (strcasecmp(dst + L - 4, ".nes") == 0
                || strcasecmp(dst + L - 4, ".sms") == 0)) {
        dst[L - 4] = 0;
    } else if (L >= 3 && (strcasecmp(dst + L - 3, ".gg") == 0
                       || strcasecmp(dst + L - 3, ".gb") == 0)) {
        dst[L - 3] = 0;
    }
}

/* Format the per-ROM meta line shared by hero / list views. */
static void format_meta(char *out, size_t outsz, const nes_rom_entry *e) {
    const char *region = e->pal_hint ? "PAL" : "NTSC";
    if      (e->system == ROM_SYS_SMS) snprintf(out, outsz, "SMS  %luK  %s",
                                                 (unsigned long)(e->size / 1024), region);
    else if (e->system == ROM_SYS_GG ) snprintf(out, outsz, "GG   %luK  %s",
                                                 (unsigned long)(e->size / 1024), region);
    else if (e->system == ROM_SYS_GB ) snprintf(out, outsz, "GB   %luK",
                                                 (unsigned long)(e->size / 1024));
    else if (e->mapper == 0xFF)        snprintf(out, outsz, "??   %luK  %s",
                                                 (unsigned long)(e->size / 1024), region);
    else                               snprintf(out, outsz, "m%d   %luK  %s",
                                                 (int)e->mapper,
                                                 (unsigned long)(e->size / 1024), region);
}

/* --- global preferences (volume etc.) ------------------------------ */

#define GLOBAL_PATH       ROMS_DIR_SLASH ".global"
#define GLOBAL_MAGIC      0x47424C31u   /* 'GBL1' */
#define VOL_DEFAULT       15
#define VOL_LIMIT         30
#define CLOCK_DEFAULT_MHZ 250

typedef struct {
    uint32_t magic;
    uint8_t  volume;
    uint8_t  _pad;
    uint16_t clock_mhz;
} picker_global_t;

static int g_volume_cached = -1;
static int g_clock_cached  = -1;

static int valid_clock_mhz(int m) {
    return m == 125 || m == 150 || m == 200 || m == 250;
}

static void global_load(void) {
    if (g_volume_cached >= 0 && g_clock_cached >= 0) return;
#ifdef THUMBYONE_SLOT_MODE
    /* Slot mode: volume lives in /.volume, NOT /.global — we don't
     * want this path to populate g_volume_cached with the /.global
     * byte (which would shadow the /.volume lookup in
     * nes_picker_global_volume below). Only load the clock. */
    g_clock_cached = CLOCK_DEFAULT_MHZ;
#else
    g_volume_cached = VOL_DEFAULT;
    g_clock_cached  = CLOCK_DEFAULT_MHZ;
#endif
    FIL f;
    if (f_open(&f, GLOBAL_PATH, FA_READ) != FR_OK) return;
    picker_global_t g = {0};
    UINT br = 0;
    f_read(&f, &g, sizeof(g), &br);
    f_close(&f);
    if (br < 4 || g.magic != GLOBAL_MAGIC) return;
#ifndef THUMBYONE_SLOT_MODE
    if (g.volume <= VOL_LIMIT) g_volume_cached = g.volume;
#endif
    /* The clock_mhz field was added in a later version of the file —
     * older 8-byte writes may have left it as 0 or arbitrary, so
     * fall back to the default unless the value validates. */
    if (br >= sizeof(g) && valid_clock_mhz(g.clock_mhz)) {
        g_clock_cached = g.clock_mhz;
    }
}

static void global_save(void) {
    picker_global_t g = {
        .magic     = GLOBAL_MAGIC,
        .volume    = (uint8_t)(g_volume_cached < 0 ? VOL_DEFAULT : g_volume_cached),
        ._pad      = 0,
        .clock_mhz = (uint16_t)(g_clock_cached  < 0 ? CLOCK_DEFAULT_MHZ : g_clock_cached),
    };
    FIL f;
    if (f_open(&f, GLOBAL_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, &g, sizeof(g), &bw);
    f_close(&f);
}

int nes_picker_global_volume(void) {
#ifdef THUMBYONE_SLOT_MODE
    /* Under ThumbyOne: the user-facing volume lives in /.volume
     * (range 0..20, shared by lobby + NES + P8 + MPY picker / games).
     * Rescale to the NES internal 0..VOL_LIMIT range the audio
     * path expects. Standalone NES still uses /.global.volume.
     *
     * Cached so repeated-per-sample callers (nes_run.c pulls this
     * at the top of the audio callback) don't hit FatFs every call. */
    if (g_volume_cached < 0) {
        int ui = thumbyone_settings_load_volume();   /* 0..20 */
        g_volume_cached = (ui * VOL_LIMIT) / THUMBYONE_VOLUME_MAX;
    }
    return g_volume_cached;
#else
    global_load();
    return g_volume_cached;
#endif
}

void nes_picker_global_set_volume(int v) {
    if (v < 0)         v = 0;
    if (v > VOL_LIMIT) v = VOL_LIMIT;
    if (v == g_volume_cached) return;
    g_volume_cached = v;
#ifdef THUMBYONE_SLOT_MODE
    /* Translate NES internal (0..30) -> unified 0..20 and persist
     * to /.volume so the next lobby / P8 / MPY boot picks it up. */
    int ui = (v * THUMBYONE_VOLUME_MAX + VOL_LIMIT / 2) / VOL_LIMIT;
    if (ui < 0) ui = 0; if (ui > THUMBYONE_VOLUME_MAX) ui = THUMBYONE_VOLUME_MAX;
    thumbyone_settings_save_volume((uint8_t)ui);
#else
    global_save();
#endif
    nes_flash_disk_flush();
}

int nes_picker_global_clock_mhz(void) {
    global_load();
    return g_clock_cached;
}

void nes_picker_global_set_clock_mhz(int mhz) {
    if (!valid_clock_mhz(mhz)) return;
    if (mhz == g_clock_cached) return;
    g_clock_cached = mhz;
    global_save();
    nes_flash_disk_flush();
}

/* --- view persistence ---------------------------------------------- */

#define VIEW_PATH "/.picker_view"
#define VIEW_HERO 0
#define VIEW_LIST 1

#define TAB_FAV 0
#define TAB_NES 1
#define TAB_SMS 2
#define TAB_GB  3
#define TAB_GG  4
#define TAB_COUNT 5

#define SORT_ALPHA 0   /* case-insensitive name */
#define SORT_FAV   1   /* favorites first, then alpha */
#define SORT_SIZE  2   /* largest first */
#define SORT_COUNT 3

typedef struct {
    uint8_t view;   /* VIEW_HERO / VIEW_LIST */
    uint8_t tab;    /* TAB_*                  */
    uint8_t sort;   /* SORT_*                 */
    uint8_t _pad;
    /* Per-tab last-selected ROM name. Empty string == no memory.
     * Loaded from /.picker_view if the file is large enough; older
     * files (4-byte version) leave the array zero-filled which the
     * picker treats as "no remembered selection". */
    char    tab_sel[TAB_COUNT][NES_PICKER_NAME_MAX];
} picker_pref_t;

static void pref_load(picker_pref_t *p) {
    memset(p, 0, sizeof(*p));
    p->view = VIEW_HERO;
    p->tab  = TAB_NES;
    p->sort = SORT_ALPHA;
    FIL f;
    if (f_open(&f, VIEW_PATH, FA_READ) != FR_OK) return;
    UINT br = 0;
    f_read(&f, p, sizeof(*p), &br);
    f_close(&f);
    if (p->view >  VIEW_LIST ) p->view = VIEW_HERO;
    if (p->tab  >= TAB_COUNT ) p->tab  = TAB_NES;
    if (p->sort >= SORT_COUNT) p->sort = SORT_ALPHA;
    /* Defensive: clamp every tab_sel string to the buffer length so
     * a partially-truncated file can't cause an unterminated read. */
    for (int t = 0; t < TAB_COUNT; t++) {
        p->tab_sel[t][NES_PICKER_NAME_MAX - 1] = 0;
    }
}

static void pref_save(const picker_pref_t *p) {
    FIL f;
    if (f_open(&f, VIEW_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, p, sizeof(*p), &bw);
    f_close(&f);
}

/* --- view (filtered + sorted index list) --------------------------- */

static int sort_mode_g;   /* read by qsort comparators below */
static const nes_rom_entry *sort_entries_g;

static int cmp_alpha(const void *a, const void *b) {
    int ai = *(const int *)a, bi = *(const int *)b;
    return strcasecmp(sort_entries_g[ai].name, sort_entries_g[bi].name);
}
static int cmp_fav(const void *a, const void *b) {
    int ai = *(const int *)a, bi = *(const int *)b;
    int fa = nes_picker_is_favorite(sort_entries_g[ai].name) ? 0 : 1;
    int fb = nes_picker_is_favorite(sort_entries_g[bi].name) ? 0 : 1;
    if (fa != fb) return fa - fb;
    return strcasecmp(sort_entries_g[ai].name, sort_entries_g[bi].name);
}
static int cmp_size(const void *a, const void *b) {
    int ai = *(const int *)a, bi = *(const int *)b;
    /* descending */
    if (sort_entries_g[ai].size != sort_entries_g[bi].size)
        return (int64_t)sort_entries_g[bi].size - (int64_t)sort_entries_g[ai].size > 0 ? 1 : -1;
    return strcasecmp(sort_entries_g[ai].name, sort_entries_g[bi].name);
}

/* Build the visible-index map for the active tab and sort it in place. */
static int build_view(const nes_rom_entry *entries, int n_entries,
                       int *view, int tab, int sort) {
    int n = 0;
    for (int i = 0; i < n_entries; i++) {
        switch (tab) {
        case TAB_FAV:
            if (!nes_picker_is_favorite(entries[i].name)) continue;
            break;
        case TAB_NES: if (entries[i].system != ROM_SYS_NES) continue; break;
        case TAB_SMS: if (entries[i].system != ROM_SYS_SMS) continue; break;
        case TAB_GB : if (entries[i].system != ROM_SYS_GB ) continue; break;
        case TAB_GG : if (entries[i].system != ROM_SYS_GG ) continue; break;
        }
        view[n++] = i;
    }
    sort_mode_g    = sort;
    sort_entries_g = entries;
    int (*cmp)(const void *, const void *) = cmp_alpha;
    if      (sort == SORT_FAV ) cmp = cmp_fav;
    else if (sort == SORT_SIZE) cmp = cmp_size;
    qsort(view, (size_t)n, sizeof(int), cmp);
    return n;
}

static const char *sort_label(int sort) {
    switch (sort) {
    case SORT_FAV:  return "favs";
    case SORT_SIZE: return "size";
    default:        return "alpha";
    }
}

/* Count entries per tab — used for tab badge labels. */
static void tab_counts(const nes_rom_entry *e, int n, int counts[TAB_COUNT]) {
    for (int i = 0; i < TAB_COUNT; i++) counts[i] = 0;
    for (int i = 0; i < n; i++) {
        if (nes_picker_is_favorite(e[i].name)) counts[TAB_FAV]++;
        if      (e[i].system == ROM_SYS_NES) counts[TAB_NES]++;
        else if (e[i].system == ROM_SYS_SMS) counts[TAB_SMS]++;
        else if (e[i].system == ROM_SYS_GB ) counts[TAB_GB ]++;
        else if (e[i].system == ROM_SYS_GG ) counts[TAB_GG ]++;
    }
}

/* --- tab strip ----------------------------------------------------- */

#define TAB_BAR_H 11

static void draw_tab_bar(uint16_t *fb, int active_tab, const int counts[TAB_COUNT]) {
    static const uint8_t icon_for[TAB_COUNT] = {
        ICON_SYS_STAR, ICON_SYS_NES, ICON_SYS_SMS, ICON_SYS_GB, ICON_SYS_GG,
    };
    /* 5 tabs in 128 px = 25 px each (loses 3 px on the right edge,
     * which is fine — we treat it as overscan). */
    int cell_w = FB_W / TAB_COUNT;
    for (int t = 0; t < TAB_COUNT; t++) {
        int x = t * cell_w;
        int hl = (t == active_tab);
        uint16_t bg = hl ? 0x39E7 /* lighter grey */ : 0x10A2 /* very dim */;
        fb_rect(fb, x, 0, cell_w, TAB_BAR_H - 1, bg);
        if (hl) fb_rect(fb, x, TAB_BAR_H - 2, cell_w, 1, COL_TITLE);
        uint16_t tint = hl ? COL_TITLE : COL_DIM;
        nes_thumb_icon(fb, x + 2, 1, icon_for[t], tint);
        char lab[8];
        snprintf(lab, sizeof(lab), "%d", counts[t]);
        int lw = nes_font_width(lab);
        nes_font_draw(fb, lab, x + cell_w - lw - 1, 3, hl ? COL_FG : COL_DIM);
    }
    fb_rect(fb, 0, TAB_BAR_H, FB_W, 1, COL_DIM);
}

/* --- hero view (default, single ROM per screen) -------------------- */

static void draw_hero(uint16_t *fb, const nes_rom_entry *e, int sel,
                      int n_view, int active_tab, const int counts[TAB_COUNT],
                      int sort, int marquee_offset) {
    fb_clear(fb, COL_BG);
    draw_tab_bar(fb, active_tab, counts);

    if (n_view == 0 || !e) {
        nes_font_draw(fb, "no roms",      40, 50, COL_FG);
        nes_font_draw(fb, "in this tab",  28, 62, COL_DIM);
        return;
    }

    /* 64×64 thumbnail centred horizontally just under the tab bar. */
    int thumb_x = 32, thumb_y = TAB_BAR_H + 2;
    if (!nes_thumb_draw(fb, thumb_x, thumb_y, 64, e->name)) {
        nes_thumb_placeholder(fb, thumb_x, thumb_y, 64, e->system);
    }

    /* Title in 2× font. If it overflows the screen width we marquee
     * it horizontally; otherwise centre it. Favorites are tinted
     * yellow (COL_TITLE) instead of plain white. */
    char nm[64];
    name_no_ext(nm, sizeof(nm), e->name);
    int  is_fav = nes_picker_is_favorite(e->name);
    uint16_t title_col = is_fav ? COL_TITLE : COL_FG;
    int title_y = thumb_y + 64 + 4;
    int tw = nes_font_width_2x(nm);
    if (tw <= FB_W - 4) {
        nes_font_draw_2x(fb, nm, (FB_W - tw) / 2, title_y, title_col);
    } else {
        /* Marquee: render the title at a negative x offset, then a
         * second copy after a gap so the loop is seamless. put()
         * inside the font renderer clips against fb edges, so any
         * glyph cells drawn off-screen on either side are dropped. */
        const int gap = 24;
        int loop = tw + gap;
        int x0 = 2 - (marquee_offset % loop);
        nes_font_draw_2x(fb, nm, x0,        title_y, title_col);
        nes_font_draw_2x(fb, nm, x0 + loop, title_y, title_col);
    }

    /* Meta line in small font under the title. */
    char meta[32];
    format_meta(meta, sizeof(meta), e);
    int mw = nes_font_width(meta);
    nes_font_draw(fb, meta, (FB_W - mw) / 2, title_y + 13, COL_DIM);

    /* Tab / system name in the larger 2× font, sitting between the
     * meta line and the position counter. Same dim grey as the meta
     * so the cart title above stays the focal point. */
    static const char * const tab_label[TAB_COUNT] = {
        "FAVORITES", "NES", "MASTER SYSTEM", "GAME BOY", "GAME GEAR",
    };
    if (active_tab >= 0 && active_tab < TAB_COUNT) {
        const char *lbl = tab_label[active_tab];
        int lw = nes_font_width_2x(lbl);
        if (lw > FB_W - 4) lw = FB_W - 4;
        nes_font_draw_2x(fb, lbl, (FB_W - lw) / 2, FB_H - 21, COL_DIM);
    }

    /* Position counter on the bottom row. Favorite signal is the
     * yellow title above. */
    char foot[24];
    snprintf(foot, sizeof(foot), "%d/%d", sel + 1, n_view);
    int fw = nes_font_width(foot);
    nes_font_draw(fb, foot, (FB_W - fw) / 2, FB_H - 8, COL_DIM);

    /* Prev / next ROM arrow hints — both D-pad axes step ROMs. */
    if (sel > 0)          nes_font_draw(fb, "<", 2,        FB_H - 8, COL_DIM);
    if (sel < n_view - 1) nes_font_draw(fb, ">", FB_W - 6, FB_H - 8, COL_DIM);

    /* Sort mode badge in the top right corner of the title row. */
    nes_font_draw(fb, sort_label(sort), FB_W - 24, title_y - 7, COL_DIM);
}

/* --- list view (3 rows of 32×32 thumbnail + name + meta) ----------- */

#define LIST_ROWS    3
#define LIST_ROW_H   34
#define LIST_ROW_TOP (TAB_BAR_H + 2)

static void draw_list_view(uint16_t *fb, const nes_rom_entry *e,
                            const int *view, int n_view, int sel, int top,
                            int active_tab, const int counts[TAB_COUNT]) {
    fb_clear(fb, COL_BG);
    draw_tab_bar(fb, active_tab, counts);

    if (n_view == 0) {
        nes_font_draw(fb, "no roms",      40, 50, COL_FG);
        nes_font_draw(fb, "in this tab",  28, 62, COL_DIM);
        return;
    }

    for (int i = 0; i < LIST_ROWS && (top + i) < n_view; i++) {
        int idx = view[top + i];
        int y = LIST_ROW_TOP + i * LIST_ROW_H;
        int hl = (top + i == sel);
        if (hl) fb_rect(fb, 0, y - 1, FB_W, LIST_ROW_H, 0x18C3);

        /* 32×32 thumbnail on the left. */
        if (!nes_thumb_draw(fb, 1, y, 32, e[idx].name)) {
            nes_thumb_placeholder(fb, 1, y, 32, e[idx].system);
        }

        /* Name + meta to the right of the thumbnail. Favorites use
         * the yellow title colour; the highlighted row still uses
         * green for the active marker. */
        char nm[24];
        name_no_ext(nm, sizeof(nm), e[idx].name);
        if (strlen(nm) > 22) nm[22] = 0;
        int is_fav = nes_picker_is_favorite(e[idx].name);
        uint16_t fg = hl     ? COL_HIGHLT
                    : is_fav ? COL_TITLE
                             : COL_FG;
        nes_font_draw(fb, nm, 36, y + 4, fg);

        char meta[32];
        format_meta(meta, sizeof(meta), &e[idx]);
        nes_font_draw(fb, meta, 36, y + 14, COL_DIM);

        /* Position-in-tab on the third line of the row. */
        if (hl) {
            char pos[16];
            snprintf(pos, sizeof(pos), "%d/%d", top + i + 1, n_view);
            nes_font_draw(fb, pos, 36, y + 24, COL_DIM);
        }
    }
}

/* --- public entry --------------------------------------------------- */

/* Reseat selection across a build_view rebuild so the user stays on
 * (or near) the same ROM. Returns the new sel. */
static int reseat_sel(int *view, int n_view, int prev_real) {
    if (n_view == 0) return 0;
    /* Try exact match first. */
    for (int i = 0; i < n_view; i++) if (view[i] == prev_real) return i;
    /* Otherwise the closest entry by absolute index distance. */
    int best = 0, best_d = 0x7fffffff;
    for (int i = 0; i < n_view; i++) {
        int d = view[i] - prev_real; if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Find the entry in the current view whose name matches `target`,
 * or 0 if not found / target is empty. Used by the per-tab selection
 * memory to restore "the rom you were on last time you visited this
 * tab". */
static int sel_by_name(const nes_rom_entry *entries, const int *view,
                        int n_view, const char *target) {
    if (!target || !target[0]) return 0;
    for (int i = 0; i < n_view; i++) {
        if (strcmp(entries[view[i]].name, target) == 0) return i;
    }
    return 0;
}

/* Delete a ROM and its sidecars (.sav .cfg .scr32 .scr64). Also
 * removes it from the favorites list. Returns 0 on success. */
static int delete_rom_and_sidecars(const char *name) {
    char p[NES_PICKER_PATH_MAX];
    snprintf(p, sizeof(p), ROMS_DIR_SLASH "%s", name); f_unlink(p);

    /* Strip the extension once and append each sidecar suffix. */
    char base[NES_PICKER_NAME_MAX];
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;

    snprintf(p, sizeof(p), ROMS_DIR_SLASH "%s.sav",   base); f_unlink(p);
    snprintf(p, sizeof(p), ROMS_DIR_SLASH "%s.cfg",   base); f_unlink(p);
    snprintf(p, sizeof(p), ROMS_DIR_SLASH "%s.scr32", base); f_unlink(p);
    snprintf(p, sizeof(p), ROMS_DIR_SLASH "%s.scr64", base); f_unlink(p);

    /* Drop from favorites if present. favs_toggle removes if there. */
    if (nes_picker_is_favorite(name)) favs_toggle(name);

    nes_flash_disk_flush();
    return 0;
}

int nes_picker_run(uint16_t *fb,
                    nes_rom_entry *entries, int *n_entries_io) {
    int n_entries = *n_entries_io;

    /* No ROMs: stay in splash, pump USB, exit when caller has files. */
    if (n_entries == 0) {
        draw_no_roms_splash(fb);
        nes_lcd_present(fb);
        nes_lcd_wait_idle();
        return -1;
    }

    favs_load();
    picker_pref_t pref;
    pref_load(&pref);

    int counts[TAB_COUNT];
    tab_counts(entries, n_entries, counts);

    /* Land on a tab that actually has ROMs. */
    if (counts[pref.tab] == 0) {
        for (int t = 0; t < TAB_COUNT; t++) {
            if (counts[t] > 0) { pref.tab = t; break; }
        }
    }

    static int view[NES_PICKER_MAX_ROMS];
    int n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
    /* Restore per-tab selection from the saved name. Falls back to
     * 0 if the file isn't there any more or no name was remembered. */
    int sel = sel_by_name(entries, view, n_view, pref.tab_sel[pref.tab]);
    int top = (sel >= LIST_ROWS) ? sel - LIST_ROWS + 1 : 0;

    uint8_t prev = 0;
    int marquee = 0;          /* horizontal scroll offset for hero title */
    int last_sel = -1;        /* reset marquee on selection change       */

    /* MENU short-tap detection — opens the picker menu. */
    int menu_press_ms = 0;
    int menu_consumed = 0;
    int open_menu     = 0;

    /* B short-tap vs long-hold-to-delete disambiguation.
     *   <  300 ms : toggle favorite (on release)
     *   >= 5000 ms: show DELETE warning overlay (cancellable)
     *   >=10000 ms: actually delete the highlighted ROM + sidecars
     */
    int b_press_ms = 0;
    int b_consumed = 0;       /* set when delete fires; release is then ignored */

    /* USB MSC rescan watchdog — when MSC writes have gone quiet for
     * RESCAN_QUIET_MS we re-scan the FAT volume so files added /
     * deleted via the host appear / disappear without a power cycle.
     * `last_seen_op` tracks the last MSC op timestamp we noticed so
     * we only rescan once per quiet window. */
    const uint64_t RESCAN_QUIET_MS = 400;
    uint64_t last_seen_op = g_msc_last_op_us;

    /* Brief OSD overlay shown after sort / view changes. */
    char osd[24] = {0};
    int  osd_ms  = 0;

    while (1) {
        uint8_t b = nes_buttons_read();
        uint8_t pressed = b & ~prev;
        prev = b;

        /* Read shoulder + menu GPIOs directly so we don't tangle them
         * up in nes_buttons_read's diagonal-coalescing logic. */
        static int prev_lb = 0, prev_rb = 0;
        int lb_down = !gpio_get(BTN_LB_GP);
        int rb_down = !gpio_get(BTN_RB_GP);
        int lb_edge = lb_down && !prev_lb;
        int rb_edge = rb_down && !prev_rb;
        prev_lb = lb_down;
        prev_rb = rb_down;

        /* ----- D-pad: navigate ROMs (wraps at both ends) ----- */
        /* LEFT / RIGHT and UP / DOWN all step prev / next ROM. */
        if ((pressed & (0x01 | 0x04)) && n_view > 0) {
            sel = (sel > 0) ? sel - 1 : n_view - 1;
            /* Re-anchor `top` so the cursor stays on screen after
             * the wrap. */
            if (sel < top) top = sel;
            if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
            if (top < 0) top = 0;
        }
        if ((pressed & (0x02 | 0x08)) && n_view > 0) {
            sel = (sel < n_view - 1) ? sel + 1 : 0;
            if (sel < top) top = sel;
            if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
            if (top < 0) top = 0;
        }

        /* ----- shoulder buttons: tab nav ----- */
        int tab_dir = 0;
        if (lb_edge) tab_dir = -1;
        if (rb_edge) tab_dir = +1;
        if (tab_dir) {
            /* Save the current tab's selection so we can come back
             * to the same ROM later. */
            if (n_view > 0) {
                strncpy(pref.tab_sel[pref.tab],
                         entries[view[sel]].name,
                         NES_PICKER_NAME_MAX - 1);
                pref.tab_sel[pref.tab][NES_PICKER_NAME_MAX - 1] = 0;
            }
            for (int tries = 0; tries < TAB_COUNT; tries++) {
                pref.tab = (pref.tab + TAB_COUNT + tab_dir) % TAB_COUNT;
                if (counts[pref.tab] > 0) break;
            }
            n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
            /* Restore the new tab's last-known selection. */
            sel = sel_by_name(entries, view, n_view, pref.tab_sel[pref.tab]);
            top = (sel >= LIST_ROWS) ? sel - LIST_ROWS + 1 : 0;
        }

        /* ----- MENU: tap to open picker menu ----- */
        if (nes_buttons_menu_pressed()) {
            menu_press_ms += 16;
        } else {
            if (menu_press_ms > 0 && !menu_consumed) {
                open_menu = 1;
            }
            menu_press_ms = 0;
            menu_consumed = 0;
        }

        /* ----- B: short tap toggles favorite, long hold deletes ----- */
        int b_held = (b & 0x10) != 0;
        if (b_held && n_view > 0) {
            b_press_ms += 16;
            if (b_press_ms >= 10000 && !b_consumed) {
                /* Threshold crossed — actually delete the highlighted
                 * ROM and all of its sidecars, then re-scan. */
                int real = view[sel];
                char doomed[NES_PICKER_NAME_MAX];
                strncpy(doomed, entries[real].name, sizeof(doomed) - 1);
                doomed[sizeof(doomed) - 1] = 0;
                delete_rom_and_sidecars(doomed);

                n_entries  = nes_picker_scan(entries, NES_PICKER_MAX_ROMS);
                *n_entries_io = n_entries;
                tab_counts(entries, n_entries, counts);
                n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
                if (sel >= n_view) sel = n_view - 1;
                if (sel < 0)       sel = 0;
                if (sel < top)     top = sel;
                if (top + LIST_ROWS <= sel) top = sel - LIST_ROWS + 1;
                if (top < 0)       top = 0;
                snprintf(osd, sizeof(osd), "deleted");
                osd_ms = 900;
                b_consumed = 1;

                if (n_entries == 0) {
                    /* Last ROM gone — bounce back to the lobby splash. */
                    favs_save();
                    pref_save(&pref);
                    return -1;
                }
            }
        } else {
            if (b_press_ms > 0 && !b_consumed && b_press_ms < 300) {
                /* Short tap on release: toggle favorite. */
                int prev_real = view[sel];
                favs_toggle(entries[prev_real].name);
                tab_counts(entries, n_entries, counts);
                n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
                sel = reseat_sel(view, n_view, prev_real);
                if (sel < top) top = sel;
                if (top + LIST_ROWS <= sel) top = sel - LIST_ROWS + 1;
                if (top < 0) top = 0;
            }
            b_press_ms = 0;
            b_consumed = 0;
        }

        /* A (PICO-8 X = bit 5) = launch. */
        if ((pressed & 0x20) && n_view > 0) {
            /* Remember which ROM the user just launched so the
             * picker comes back here when they exit the game. */
            strncpy(pref.tab_sel[pref.tab],
                     entries[view[sel]].name,
                     NES_PICKER_NAME_MAX - 1);
            pref.tab_sel[pref.tab][NES_PICKER_NAME_MAX - 1] = 0;
            favs_save();
            pref_save(&pref);
            return view[sel];
        }

        /* ----- USB rescan watchdog ----- */
        uint64_t now_us = (uint64_t)time_us_64();
        if (g_msc_last_op_us != last_seen_op) {
            /* Activity is happening — defer the rescan until it goes
             * quiet. Just remember the latest timestamp. */
            last_seen_op = g_msc_last_op_us;
        } else if (last_seen_op != 0
                && (now_us - last_seen_op) >= RESCAN_QUIET_MS * 1000ull) {
            /* MSC has been quiet for the watchdog window since the
             * last write — refresh the directory. */
            int prev_real = (n_view > 0) ? view[sel] : -1;
            n_entries  = nes_picker_scan(entries, NES_PICKER_MAX_ROMS);
            *n_entries_io = n_entries;
            tab_counts(entries, n_entries, counts);
            /* If the active tab is now empty, slide to the next
             * non-empty tab so we never freeze on a deleted view. */
            if (counts[pref.tab] == 0) {
                for (int t = 0; t < TAB_COUNT; t++) {
                    if (counts[t] > 0) { pref.tab = t; break; }
                }
            }
            n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
            sel = reseat_sel(view, n_view, prev_real);
            if (sel < top) top = sel;
            if (top + LIST_ROWS <= sel) top = sel - LIST_ROWS + 1;
            if (top < 0) top = 0;
            last_seen_op = 0;       /* arm for the next activity burst */
            if (n_entries == 0) {
                favs_save();
                pref_save(&pref);
                return -1;
            }
        }

        /* ----- picker menu (MENU tap) ----- */
        if (open_menu) {
            open_menu = 0;

            int v_view = (pref.view == VIEW_HERO) ? 0 : 1;
            int v_sort = pref.sort;
            int v_vol  = nes_picker_global_volume();
#ifdef THUMBYONE_SLOT_MODE
            /* ThumbyOne global brightness (0..255). */
            int v_bri  = thumbyone_settings_load_brightness();
            int old_bri = v_bri;
#endif
            int v_clock_mhz = nes_picker_global_clock_mhz();
            int v_clock = (v_clock_mhz == 125) ? 0
                        : (v_clock_mhz == 150) ? 1
                        : (v_clock_mhz == 200) ? 2 : 3;

            /* Storage info — read once when the menu opens. Under
             * ThumbyOne slot mode, format via the shared helper so
             * the NES picker matches the lobby / P8 / MPY picker
             * readouts exactly. Standalone NES keeps the old
             * "X.XX/Y.YYMB" format. */
            int v_storage_used_kb = 0;
            int v_storage_total_kb = 0;
            char storage_text[24];
#ifdef THUMBYONE_SLOT_MODE
            {
                uint64_t used_b = 0, total_b = 0;
                thumbyone_fs_get_usage(&used_b, NULL, &total_b);
                thumbyone_fs_fmt_used_total(used_b, total_b,
                                            storage_text, sizeof(storage_text));
                v_storage_used_kb  = (int)(used_b / 1024);
                v_storage_total_kb = (int)(total_b / 1024);
            }
#else
            {
                DWORD free_clusters = 0;
                FATFS *fs_ptr = NULL;
                uint32_t total_kb = 0, free_kb = 0;
                if (f_getfree("", &free_clusters, &fs_ptr) == FR_OK && fs_ptr) {
                    uint32_t bytes_per_cluster = (uint32_t)fs_ptr->csize * 512u;
                    uint32_t total_clusters    = (uint32_t)(fs_ptr->n_fatent - 2);
                    total_kb = (total_clusters * bytes_per_cluster) / 1024u;
                    free_kb  = ((uint32_t)free_clusters * bytes_per_cluster) / 1024u;
                }
                uint32_t used_kb = (total_kb > free_kb) ? (total_kb - free_kb) : 0;
                v_storage_used_kb  = (int)used_kb;
                v_storage_total_kb = (int)total_kb;
                snprintf(storage_text, sizeof(storage_text), "%lu.%02lu/%lu.%02luMB",
                          (unsigned long)(used_kb / 1024),
                          (unsigned long)((used_kb % 1024) * 100 / 1024),
                          (unsigned long)(total_kb / 1024),
                          (unsigned long)((total_kb % 1024) * 100 / 1024));
            }
#endif

            /* Battery info. */
            int   v_batt_pct  = nes_battery_percent();
            float batt_v      = nes_battery_voltage();
            bool  charging    = nes_battery_charging();
            char  battery_text[24];
            int   batt_v_int  = (int)batt_v;
            int   batt_v_dec  = (int)((batt_v - batt_v_int) * 100.0f + 0.5f);
#ifdef THUMBYONE_SLOT_MODE
            if (charging) {
                snprintf(battery_text, sizeof(battery_text), "CHRG %d.%02dV",
                          batt_v_int, batt_v_dec);
            } else {
                snprintf(battery_text, sizeof(battery_text), "%d%% %d.%02dV",
                          v_batt_pct, batt_v_int, batt_v_dec);
            }
#else
            if (charging) {
                snprintf(battery_text, sizeof(battery_text), "CHRG %d.%02dV",
                          batt_v_int, batt_v_dec);
            } else {
                snprintf(battery_text, sizeof(battery_text), "%d%% %d.%02dV",
                          v_batt_pct, batt_v_int, batt_v_dec);
            }
#endif

            char about_text[24];
            snprintf(about_text, sizeof(about_text), "ThumbyNES v1.02");

            static const char * const view_choices[]  = { "HERO", "LIST" };
            static const char * const sort_choices[]  = { "ALPHA", "FAVS", "SIZE" };
            static const char * const clock_choices[] = { "125MHz", "150MHz", "200MHz", "250MHz" };
            static const int          clock_mhz[]     = {  125,      150,      200,      250 };

            /* ACT_LOBBY is only offered when compiled into ThumbyOne
             * (standalone NES has nothing to fall back to). The
             * menu_item enum stays stable otherwise so the picker
             * state machine below doesn't care about conditional
             * item ordering. */
            enum { ACT_NONE, ACT_LOBBY };

            nes_menu_item_t items[] = {
                { .kind = NES_MENU_KIND_ACTION, .label = "Resume",
                  .enabled = true, .action_id = ACT_NONE },
                { .kind = NES_MENU_KIND_SLIDER, .label = "Volume",
                  .value_ptr = &v_vol, .min = 0, .max = VOL_LIMIT, .enabled = true },
#ifdef THUMBYONE_SLOT_MODE
                { .kind = NES_MENU_KIND_SLIDER, .label = "Brightness",
                  .value_ptr = &v_bri, .min = 0, .max = 255, .enabled = true },
#endif
                { .kind = NES_MENU_KIND_CHOICE, .label = "Overclock",
                  .value_ptr = &v_clock, .choices = clock_choices, .num_choices = 4,
                  .enabled = true, .suffix = "next launch" },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Display",
                  .value_ptr = &v_view, .choices = view_choices, .num_choices = 2,
                  .enabled = true },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Sort",
                  .value_ptr = &v_sort, .choices = sort_choices, .num_choices = 3,
                  .enabled = true },
                { .kind = NES_MENU_KIND_INFO, .label = "Battery",
                  .info_text = battery_text,
                  .value_ptr = &v_batt_pct, .min = 0, .max = 100,
                  .enabled = true },
                { .kind = NES_MENU_KIND_INFO, .label = "Storage",
                  .info_text = storage_text,
                  .value_ptr = &v_storage_used_kb, .min = 0,
                  .max = v_storage_total_kb > 0 ? v_storage_total_kb : 1,
                  .enabled = true },
                { .kind = NES_MENU_KIND_INFO, .label = "About",
                  .info_text = about_text, .enabled = true },
#ifdef THUMBYONE_SLOT_MODE
                /* "Back to lobby" is always the last item so a single
                 * UP from the top wraps to it — muscle-memory "go back". */
                { .kind = NES_MENU_KIND_ACTION, .label = "Back to lobby",
                  .enabled = true, .action_id = ACT_LOBBY },
#endif
            };

            nes_menu_result_t r = nes_menu_run(fb, "PICKER", "settings",
                                                items, sizeof(items) / sizeof(items[0]));

            /* Apply value changes. */
            if (v_vol != nes_picker_global_volume()) {
                nes_picker_global_set_volume(v_vol);
            }
#ifdef THUMBYONE_SLOT_MODE
            /* Brightness: write /.brightness + flush + apply PWM.
             * Clamp before cast — the slider max is 255 so the int
             * value always fits in uint8_t, but be defensive. */
            if (v_bri != old_bri) {
                if (v_bri < 0)   v_bri = 0;
                if (v_bri > 255) v_bri = 255;
                thumbyone_settings_save_brightness((uint8_t)v_bri);
                nes_flash_disk_flush();
                thumbyone_backlight_set((uint8_t)v_bri);
            }
#endif
            int new_mhz = clock_mhz[v_clock];
            if (new_mhz != nes_picker_global_clock_mhz()) {
                nes_picker_global_set_clock_mhz(new_mhz);
                snprintf(osd, sizeof(osd), "clock: %d (next)", new_mhz);
                osd_ms = 1500;
            }
            int new_view = (v_view == 0) ? VIEW_HERO : VIEW_LIST;
            if (new_view != pref.view) { pref.view = new_view; }
            if (v_sort != pref.sort) {
                pref.sort = v_sort;
                int prev_real = (n_view > 0) ? view[sel] : -1;
                n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
                sel = reseat_sel(view, n_view, prev_real);
                top = (sel >= LIST_ROWS) ? sel - LIST_ROWS + 1 : 0;
            }

#ifdef THUMBYONE_SLOT_MODE
            if (r.kind == NES_MENU_ACTION && r.action_id == ACT_LOBBY) {
                /* Save prefs + favourites + shut down the flash
                 * cache before firing the handoff — any dirty
                 * state left behind would be lost when the bootrom
                 * chain-reloads the lobby from a cold state. */
                pref_save(&pref);
                favs_save();
                nes_flash_disk_flush();
                f_unmount("");
                nes_lcd_wait_idle();
                thumbyone_handoff_request_lobby();
                /* does not return */
            }
#endif

            pref_save(&pref);
        }

        /* Reset marquee whenever the highlighted ROM changes. */
        if (sel != last_sel) { marquee = 0; last_sel = sel; }
        else                 { marquee += 1; }

        /* Always redraw — the marquee ticks every frame. */
        if (pref.view == VIEW_HERO) {
            draw_hero(fb, n_view > 0 ? &entries[view[sel]] : NULL,
                       sel, n_view, pref.tab, counts, pref.sort, marquee);
        } else {
            draw_list_view(fb, entries, view, n_view, sel, top,
                            pref.tab, counts);
        }

        /* Delete-confirmation overlay. Shown once B has been held for
         * at least 5 seconds and updates every frame so the user sees
         * the countdown. Releasing B before 10 s cancels harmlessly. */
        if (b_held && b_press_ms >= 5000 && !b_consumed) {
            int remaining = (10000 - b_press_ms + 999) / 1000;
            if (remaining < 0) remaining = 0;
            fb_rect(fb, 0, 40, FB_W, 48, 0x4000);
            fb_rect(fb, 0, 41, FB_W, 1,  COL_ERR);
            fb_rect(fb, 0, 86, FB_W, 1,  COL_ERR);
            const char *line1 = "DELETE ROM?";
            int w1 = nes_font_width_2x(line1);
            nes_font_draw_2x(fb, line1, (FB_W - w1) / 2, 46, COL_ERR);
            const char *line2 = "release B to cancel";
            int w2 = nes_font_width(line2);
            nes_font_draw(fb, line2, (FB_W - w2) / 2, 64, COL_FG);
            char cd[16];
            snprintf(cd, sizeof(cd), "in %d", remaining);
            int w3 = nes_font_width(cd);
            nes_font_draw(fb, cd, (FB_W - w3) / 2, 76, COL_ERR);
        }

        if (osd_ms > 0) {
            int w = nes_font_width(osd);
            int x = (FB_W - w) / 2;
            fb_rect(fb, x - 2, FB_H - 18, w + 4, 9, 0x0000);
            nes_font_draw(fb, osd, x, FB_H - 17, COL_TITLE);
            osd_ms -= 16;
        }
        nes_lcd_wait_idle();
        nes_lcd_present(fb);

        /* Keep USB alive while picker is up. */
        tud_task();

        sleep_ms(16);
    }
}
