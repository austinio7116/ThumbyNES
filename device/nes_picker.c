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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#include "tusb.h"
#include "ff.h"

#include "nes_flash_disk.h"

#define BTN_LB_GP 6   /* mirror nes_buttons.c — used for filter toggle */

/* The global FATFS object lives in nes_device_main.c. We need it
 * here to walk the cluster chain when computing a ROM file's XIP
 * address for the zero-copy load path. */
extern FATFS g_fs;

#define FB_W 128
#define FB_H 128

extern volatile uint64_t g_msc_last_op_us;

/* --- favorites store ------------------------------------------------ */

/* /.favs is a plain newline-separated list of base file names that
 * the user has marked as favorites. We keep the whole file in a small
 * RAM buffer, mutated in place, and write it back on picker exit if
 * anything changed. 4 KB holds ~80 ROM names, more than fits in
 * the 64-entry NES_PICKER_MAX_ROMS cap anyway. */
#define FAVS_PATH      "/.favs"
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
    char path[80];
    snprintf(path, sizeof(path), "/%s", fname);
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
    if (f_opendir(&dir, "/") != FR_OK) return 0;
    int n = 0;
    while (n < max && f_readdir(&dir, &info) == FR_OK) {
        if (info.fname[0] == 0) break;
        if (info.fattrib & AM_DIR) continue;
        size_t L = strlen(info.fname);
        if (L < 4) continue;
        uint8_t sys = 0xFF;
        if      (strcasecmp(info.fname + L - 4, ".nes") == 0) sys = ROM_SYS_NES;
        else if (strcasecmp(info.fname + L - 4, ".sms") == 0) sys = ROM_SYS_SMS;
        else if (L >= 3 && strcasecmp(info.fname + L - 3, ".gg") == 0) sys = ROM_SYS_GG;
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
    char path[80];
    snprintf(path, sizeof(path), "/%s", name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FSIZE_t sz = f_size(&f);
    if (sz < 16 || sz > 1024 * 1024) { f_close(&f); return NULL; }
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

int nes_picker_mmap_rom(const char *name,
                          const uint8_t **out_data, size_t *out_len) {
    if (!out_data || !out_len) return -1;

    /* Make sure any pending writes for this file have been flushed
     * to physical flash before we try to read it via XIP. */
    nes_flash_disk_flush();

    char path[80];
    snprintf(path, sizeof(path), "/%s", name);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -2;
    FSIZE_t sz = f_size(&f);
    if (sz < 16 || sz > 1024 * 1024) { f_close(&f); return -3; }

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

/* --- splash screens ------------------------------------------------ */

static void draw_no_roms_splash(uint16_t *fb) {
    fb_clear(fb, COL_BG);
    nes_font_draw(fb, "ThumbyNES", 32, 30, COL_TITLE);
    nes_font_draw(fb, "no roms found",  20, 50, COL_FG);
    nes_font_draw(fb, "drag .nes files", 14, 70, COL_DIM);
    nes_font_draw(fb, "to usb drive",    22, 78, COL_DIM);
    nes_font_draw(fb, "then eject",      28, 92, COL_DIM);
}

/* --- list UI ------------------------------------------------------- */

/* Strip a trailing recognised extension so the row reads cleanly. */
static void name_no_ext(char *dst, size_t dstsz, const char *src) {
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = 0;
    size_t L = strlen(dst);
    if (L >= 4 && (strcasecmp(dst + L - 4, ".nes") == 0
                || strcasecmp(dst + L - 4, ".sms") == 0)) {
        dst[L - 4] = 0;
    } else if (L >= 3 && strcasecmp(dst + L - 3, ".gg") == 0) {
        dst[L - 3] = 0;
    }
}

static void draw_list(uint16_t *fb, const nes_rom_entry *e, int n,
                      const int *view, int n_view,
                      int sel, int top, int max_rows,
                      int filter_favs) {
    fb_clear(fb, COL_BG);
    nes_font_draw(fb, "ThumbyNES", 32, 2, COL_TITLE);
    fb_rect(fb, 0, 10, 128, 1, COL_DIM);

    /* Two-line rows: name on top, mapper + size on bottom. */
    const int row_h = 12;
    for (int i = 0; i < max_rows && (top + i) < n_view; i++) {
        int idx = view[top + i];
        int y = 13 + i * row_h;
        int hl = (top + i == sel);
        if (hl) fb_rect(fb, 0, y - 1, 128, row_h, 0x18C3);
        uint16_t fg = hl ? COL_HIGHLT : COL_FG;

        char nm[36];
        name_no_ext(nm, sizeof(nm), e[idx].name);
        if (strlen(nm) > 30) nm[30] = 0;
        /* Star prefix for favorites — eats one glyph of width. */
        int is_fav = nes_picker_is_favorite(e[idx].name);
        char row[36];
        if (is_fav) snprintf(row, sizeof(row), "*%s", nm);
        else        snprintf(row, sizeof(row), " %s", nm);
        nes_font_draw(fb, row, 2, y, fg);

        char meta[32];
        const char *region = e[idx].pal_hint ? "PAL" : "NTSC";
        const char *tag;
        if      (e[idx].system == ROM_SYS_SMS) tag = "SMS";
        else if (e[idx].system == ROM_SYS_GG)  tag = "GG ";
        else                                   tag = NULL;
        if (tag) {
            snprintf(meta, sizeof(meta), "%s  %luK  %s",
                      tag, (unsigned long)(e[idx].size / 1024), region);
        } else if (e[idx].mapper == 0xFF) {
            snprintf(meta, sizeof(meta), "??  %luK  %s",
                      (unsigned long)(e[idx].size / 1024), region);
        } else {
            snprintf(meta, sizeof(meta), "m%d  %luK  %s",
                      (int)e[idx].mapper,
                      (unsigned long)(e[idx].size / 1024), region);
        }
        nes_font_draw(fb, meta, 3, y + 6, COL_DIM);
    }

    /* Footer with paging + filter indicator. */
    char ft[28];
    int page  = (n_view > 0) ? sel / max_rows + 1 : 0;
    int pages = (n_view + max_rows - 1) / max_rows;
    if (filter_favs) {
        snprintf(ft, sizeof(ft), "%d/%d %d/%d FAV",
                  (n_view ? sel + 1 : 0), n_view, page, pages);
    } else {
        snprintf(ft, sizeof(ft), "%d/%d  pg %d/%d",
                  sel + 1, n_view, page, pages);
    }
    nes_font_draw(fb, ft, 3, FB_H - 7, COL_DIM);
}

/* --- public entry --------------------------------------------------- */

/* Build the visible-index map for the current filter state. With
 * the favorites filter off, view[i] = i. With it on, view contains
 * only the indices of favorited ROMs. Returns the visible count. */
static int build_view(const nes_rom_entry *entries, int n_entries,
                       int *view, int filter_favs) {
    int n = 0;
    for (int i = 0; i < n_entries; i++) {
        if (filter_favs && !nes_picker_is_favorite(entries[i].name)) continue;
        view[n++] = i;
    }
    return n;
}

int nes_picker_run(uint16_t *fb,
                    const nes_rom_entry *entries, int n_entries) {
    int sel = 0, top = 0;
    const int row_h = 12;
    const int max_rows = (FB_H - 14 - 8) / row_h;   /* leave footer */

    /* No ROMs: stay in splash, pump USB, exit when caller has files. */
    if (n_entries == 0) {
        draw_no_roms_splash(fb);
        nes_lcd_present(fb);
        nes_lcd_wait_idle();
        return -1;
    }

    favs_load();

    int filter_favs = 0;
    static int view[NES_PICKER_MAX_ROMS];
    int n_view = build_view(entries, n_entries, view, filter_favs);

    /* Edge-detect input. */
    uint8_t prev = 0;
    int prev_lb = 0;
    while (1) {
        uint8_t b = nes_buttons_read();
        uint8_t pressed = b & ~prev;
        prev = b;

        /* LB read directly off the GPIO so it isn't tangled up in
         * nes_buttons_read's diagonal-coalescing logic. */
        int lb_down = !gpio_get(BTN_LB_GP);
        int lb_edge = lb_down && !prev_lb;
        prev_lb = lb_down;

        /* nes_buttons_read returns PICO-8 LRUDOX:
         *   bit0=L bit1=R bit2=U bit3=D bit4=O bit5=X
         * Right face button (the firmware's NES 'A') maps to X
         * = bit 5 (0x20). Left face button (NES 'B') maps to O
         * = bit 4 (0x10) — used here as 'toggle favorite'. */
        if (pressed & 0x04) { /* up */
            if (sel > 0) sel--;
            if (sel < top) top = sel;
        }
        if (pressed & 0x08) { /* down */
            if (sel < n_view - 1) sel++;
            if (sel >= top + max_rows) top = sel - max_rows + 1;
        }
        if ((pressed & 0x10) && n_view > 0) {
            /* B = toggle favorite for the highlighted ROM. */
            int prev_real = view[sel];
            favs_toggle(entries[prev_real].name);
            n_view = build_view(entries, n_entries, view, filter_favs);
            if (filter_favs) {
                /* The toggled entry might have just disappeared
                 * from view; reseat selection on the next visible
                 * entry, or wrap to 0. */
                int new_sel = 0;
                for (int i = 0; i < n_view; i++) {
                    if (view[i] >= prev_real) { new_sel = i; break; }
                }
                if (n_view > 0) sel = new_sel; else sel = 0;
                if (sel < top) top = sel;
                if (top + max_rows <= sel) top = sel - max_rows + 1;
                if (top < 0) top = 0;
            }
        }
        if ((pressed & 0x20) && n_view > 0) { /* X = physical A = launch */
            favs_save();
            return view[sel];
        }
        if (lb_edge) {
            /* LB = toggle favorites filter. Try to keep selection
             * on the same ROM across the rebuild. */
            int prev_real = (n_view > 0) ? view[sel] : -1;
            filter_favs = !filter_favs;
            n_view = build_view(entries, n_entries, view, filter_favs);
            int new_sel = 0;
            for (int i = 0; i < n_view; i++) {
                if (view[i] == prev_real) { new_sel = i; break; }
            }
            sel = new_sel;
            top = (sel >= max_rows) ? sel - max_rows + 1 : 0;
        }
        if (nes_buttons_menu_pressed()) {
            favs_save();
            return -1;
        }

        draw_list(fb, entries, n_entries, view, n_view, sel, top, max_rows, filter_favs);
        nes_lcd_wait_idle();
        nes_lcd_present(fb);

        /* Keep USB alive while picker is up so user can still
         * drop more ROMs in without rebooting. */
        tud_task();

        sleep_ms(16);
    }
}
