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
#include "nes_led.h"

#ifdef THUMBYONE_SLOT_MODE
#  include "thumbyone_fs_stats.h"
#  include "thumbyone_settings.h"
#  include "thumbyone_backlight.h"
#  include "thumbyone_battery.h"
#endif

#include <limits.h>
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
        else if (strcasecmp(info.fname + L - 4, ".gbc") == 0) sys = ROM_SYS_GB;
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
        /* FAT16 end-of-chain is ANY value in 0xFFF8..0xFFFF. Checking
         * only 0xFFFF misses valid EOC variants that FatFs and other
         * FAT writers may produce, causing chain_is_contiguous to
         * treat a legitimate EOC as an invalid next-cluster pointer
         * and incorrectly return 0 (fragmented). */
        if (next >= 0xFFF8) {
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

#define DEFRAG_TMP     "/.defrag.tmp"
#define DEFRAG_SCRATCH "/.defrag.scratch"
#define DEFRAG_BUFSZ   4096

/* Live cluster map. Each cell in the 60×40 grid is ~1 cluster on the
 * 9.6 MB volume. Colour per cell reflects cluster ownership so the
 * user can see individual files as distinct coloured regions and
 * watch them shift / consolidate as defrag progresses:
 *   - dark slate     = free cluster
 *   - rotating hue   = allocated, coloured by file index modulo 15
 *     (different files get different hues; each file's cluster chain
 *     shows up as blocks of one colour)
 *
 * Building the owner map requires walking every directory + every
 * file's FAT chain once per redraw. On our 2400-cluster volume with
 * ~134 files this touches ~10 FAT sectors + ~5 dir sectors, all
 * cached by the flash-disk layer — measured at single-digit ms. */
static void draw_cluster_map(uint16_t *fb, const char *active_file_tag) {
    (void)active_file_tag;
    enum {
        MAP_X = 4, MAP_Y = 22,
        CELL_W = 2, CELL_H = 2,
        MAP_COLS = 60, MAP_ROWS = 40,
        N_CELLS = MAP_COLS * MAP_ROWS,
        MAX_CLUSTERS = 2600,       /* upper bound for 10 MB FAT16 vol */
        MAX_DIRS = 32,
        PATH_LEN_LOCAL = NES_PICKER_NAME_MAX,
    };

    /* Static buffers: ~6 KB permanent BSS. Only exercised during
     * defrag so the cost is minor; the alternative (malloc+free per
     * redraw) thrashes the heap ~60x/sec during active copies. */
    static uint8_t cluster_owner[MAX_CLUSTERS];
    static char    visdir_queue[MAX_DIRS][PATH_LEN_LOCAL];
    static uint8_t fat_sec[512];

    memset(cluster_owner, 0, sizeof(cluster_owner));

    DWORD n_clust = g_fs.n_fatent - 2;
    if (n_clust == 0) return;
    if (n_clust > MAX_CLUSTERS) n_clust = MAX_CLUSTERS;

    /* BFS over the volume directory tree, assigning an incrementing
     * owner_id to each file and marking its cluster chain. */
    int q_head = 0, q_tail = 0;
    strncpy(visdir_queue[q_tail], "/", PATH_LEN_LOCAL - 1);
    visdir_queue[q_tail][PATH_LEN_LOCAL - 1] = 0;
    q_tail++;

    LBA_t cached_lba = (LBA_t)-1;
    int file_counter = 0;

    while (q_head < q_tail) {
        const char *dir_path = visdir_queue[q_head++];
        DIR dir;
        FILINFO info;
        if (f_opendir(&dir, dir_path) != FR_OK) continue;
        while (f_readdir(&dir, &info) == FR_OK) {
            if (info.fname[0] == 0) break;
            if (info.fname[0] == '.') continue;

            char full[PATH_LEN_LOCAL];
            if (dir_path[0] == '/' && dir_path[1] == 0)
                snprintf(full, sizeof(full), "/%s", info.fname);
            else
                snprintf(full, sizeof(full), "%s/%s", dir_path, info.fname);

            if (info.fattrib & AM_DIR) {
                if (q_tail < MAX_DIRS) {
                    strncpy(visdir_queue[q_tail], full, PATH_LEN_LOCAL - 1);
                    visdir_queue[q_tail][PATH_LEN_LOCAL - 1] = 0;
                    q_tail++;
                }
                continue;
            }
            if (info.fsize == 0) continue;

            FIL f;
            if (f_open(&f, full, FA_READ) != FR_OK) continue;
            DWORD sc = f.obj.sclust;
            f_close(&f);
            if (sc < 2 || sc >= g_fs.n_fatent) continue;

            file_counter++;
            /* owner_id = 1..255 packed into a byte; 0 reserved for
             * "free". Anything past 255 re-uses slots mod 255 so the
             * palette still shows distinct files (at worst two files
             * share a hue). */
            uint8_t owner_id = 1 + ((file_counter - 1) & 0xFF);
            if (owner_id == 0) owner_id = 1;

            DWORD clst = sc;
            int safety = MAX_CLUSTERS + 1;
            while (clst >= 2 && clst < g_fs.n_fatent && safety-- > 0) {
                if ((DWORD)(clst - 2) < n_clust) {
                    cluster_owner[clst - 2] = owner_id;
                }
                DWORD eb = clst * 2;
                DWORD es = eb / 512;
                DWORD eo = eb % 512;
                LBA_t lba = (LBA_t)g_fs.fatbase + es;
                if (lba != cached_lba) {
                    if (nes_flash_disk_read(fat_sec, (uint32_t)lba, 1) != 0) break;
                    cached_lba = lba;
                }
                uint16_t next = (uint16_t)fat_sec[eo]
                              | ((uint16_t)fat_sec[eo + 1] << 8);
                if (next == 0xFFFF || next < 2) break;
                clst = next;
            }
        }
        f_closedir(&dir);
    }

    /* Distinct-hue palette (15 colours + slate-for-free). Picked so
     * adjacent files read as clearly different blocks. */
    static const uint16_t file_palette[15] = {
        0x07FF,  /* cyan */
        0xFFE0,  /* yellow */
        0x07E0,  /* green */
        0xF81F,  /* magenta */
        0xFD20,  /* orange */
        0xAFE5,  /* mint */
        0x87FF,  /* light blue */
        0xFB56,  /* salmon */
        0x5E7F,  /* sky */
        0xFCE0,  /* gold */
        0xA8F4,  /* lavender */
        0x6E7D,  /* teal */
        0xFA28,  /* coral */
        0xBE5B,  /* pale green */
        0xEF7E,  /* ivory */
    };
    const uint16_t FREE_COLOR = 0x10A2;    /* dark slate */

    fb_rect(fb, MAP_X - 1, MAP_Y - 1,
            MAP_COLS * CELL_W + 2, MAP_ROWS * CELL_H + 2, 0x4208);

    for (int r = 0; r < MAP_ROWS; r++) {
        for (int co = 0; co < MAP_COLS; co++) {
            int cell_idx = r * MAP_COLS + co;
            uint32_t first_cluster =
                (uint32_t)(((uint64_t)cell_idx * n_clust) / N_CELLS);
            uint16_t color;
            if (first_cluster >= n_clust
             || cluster_owner[first_cluster] == 0) {
                color = FREE_COLOR;
            } else {
                uint8_t id = cluster_owner[first_cluster];
                color = file_palette[(id - 1) % 15];
            }
            fb_rect(fb, MAP_X + co * CELL_W, MAP_Y + r * CELL_H,
                    CELL_W, CELL_H, color);
        }
    }
}

/* Writing ~10 MB to the flash disk is uninterruptible — if power
 * drops mid-rename the current file's data is lost. The overlay
 * keeps a red "DO NOT POWER OFF" banner pinned to the top at all
 * times and shows a live cluster map below it so the user can watch
 * the compaction happen. The front indicator LED also flips to red
 * for the duration of a compact pass. */
static void defrag_progress_impl(uint16_t *fb, const char *title,
                                  const char *name, int done, int total,
                                  bool loud) {
    fb_clear(fb, COL_BG);

    if (loud) {
        /* Red warning bar at the very top. */
        fb_rect(fb, 0, 0, FB_W, 12, COL_ERR);
        const char *warn = "DO NOT POWER OFF";
        nes_font_draw(fb, warn, (FB_W - nes_font_width(warn)) / 2,
                       3, COL_FG);
    } else {
        nes_font_draw(fb, "DEFRAGMENTING", 16, 2, COL_TITLE);
    }
    /* Operation sub-line. */
    if (title) {
        nes_font_draw(fb, title,
                       (FB_W - nes_font_width(title)) / 2,
                       13, loud ? COL_TITLE : COL_DIM);
    }

    /* Live cluster map — the star of the overlay. */
    draw_cluster_map(fb, name);

    /* Filename (trimmed to fit). Show the tail so the cart name shows
     * rather than the dir prefix on deeply-nested paths. */
    if (name) {
        const char *tail = strrchr(name, '/');
        tail = tail ? tail + 1 : name;
        char nm[26];
        strncpy(nm, tail, sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = 0;
        int nw = nes_font_width(nm);
        /* Truncate until it fits. */
        while (nw > FB_W - 4 && strlen(nm) > 3) {
            nm[strlen(nm) - 1] = 0;
            nw = nes_font_width(nm);
        }
        nes_font_draw(fb, nm, (FB_W - nw) / 2, 106, COL_FG);
    }

    /* Progress count + thin bar at the bottom. */
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d / %d", done, total);
    nes_font_draw(fb, cnt, (FB_W - nes_font_width(cnt)) / 2,
                   116, COL_DIM);

    int bar_y = 124, bar_h = 2;
    fb_rect(fb, 4, bar_y, FB_W - 8, bar_h, 0x2104);
    int fill = total > 0 ? ((FB_W - 8) * done) / total : 0;
    if (fill > FB_W - 8) fill = FB_W - 8;
    fb_rect(fb, 4, bar_y, fill, bar_h, COL_HIGHLT);

    nes_lcd_wait_idle();
    nes_lcd_present(fb);
    tud_task();
}

static void defrag_progress(uint16_t *fb, const char *name, int done, int total) {
    defrag_progress_impl(fb, "defragmenting", name, done, total, false);
}

/* Generic single-file compactor — takes an absolute path, so it
 * works for files under /roms, /carts, /games/<name>/assets/... etc.
 * Same temp-file f_expand dance as defrag_one_roms below; the temp
 * lives at "/.defrag.tmp" regardless of the source directory because
 * FatFs' f_rename moves files across directories on the same volume. */
static int defrag_one_path(const char *src_path, uint16_t *fb,
                            int done, int total) {
    const char *tmp_path = DEFRAG_TMP;

    FIL src;
    if (f_open(&src, src_path, FA_READ) != FR_OK) return -1;
    FSIZE_t sz = f_size(&src);

    f_unlink(tmp_path);

    FIL dst;
    if (f_open(&dst, tmp_path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        f_close(&src);
        return -2;
    }
    if (f_expand(&dst, sz, 1) != FR_OK) {
        f_close(&dst); f_close(&src); f_unlink(tmp_path);
        return -3;
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
        if ((copied & 0xFFFF) == 0) {
            /* Repaint the loud overlay so the progress bar moves on
             * bigger files, and keep tud_task ticking for USB health. */
            defrag_progress_impl(fb, "compacting disk",
                                  src_path, done, total, true);
        }
    }
    f_close(&dst);
    f_close(&src);

    if (f_unlink(src_path) != FR_OK) {
        f_unlink(tmp_path);
        return -6;
    }
    if (f_rename(tmp_path, src_path) != FR_OK) {
        return -7;
    }

    nes_flash_disk_flush();

    /* Verify the rewrite actually landed on a contiguous chain. If
     * chain_is_contiguous still returns false here, either f_expand
     * with opt=1 didn't deliver a contiguous chain (FatFs bug or
     * cluster pressure we didn't expect) or the probe itself is
     * misreading the FAT. Either way we want to fail loudly so
     * debug output tells us which case. */
    {
        FIL verify;
        if (f_open(&verify, src_path, FA_READ) == FR_OK) {
            DWORD sc = verify.obj.sclust;
            FSIZE_t vsz = f_size(&verify);
            f_close(&verify);
            if (vsz > 0 && sc >= 2) {
                DWORD bpc = (DWORD)g_fs.csize * 512u;
                DWORD nc  = ((DWORD)vsz + bpc - 1) / bpc;
                if (!chain_is_contiguous(sc, nc)) {
                    return -8;   /* rewrite succeeded by FatFs but
                                  * the chain isn't contiguous */
                }
            }
        }
    }
    return 0;
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

int nes_picker_defrag_one(const char *name, uint16_t *fb) {
    if (!name) return -1;
    defrag_progress(fb, name, 0, 1);
    int rc = defrag_one(name, fb, 0, 1);
    defrag_progress(fb, "done", 1, 1);
    nes_flash_disk_flush();
    return rc;
}

/* Captures the return code of the first defrag_one_path call that
 * didn't return 0 during the most recent nes_picker_defrag_compact
 * run. Read it via nes_picker_defrag_last_error after the pass. */
static int s_defrag_last_error = 0;

int nes_picker_defrag_last_error(void) {
    return s_defrag_last_error;
}

/* Scratch-based rewrite for files whose own clusters block their
 * target allocation. Sequence:
 *   1. Copy source -> /.defrag.scratch (FRAGMENTED allocation — we
 *      just need to stash the data somewhere).
 *   2. Unlink source. Its clusters become free and are now available
 *      to the allocator.
 *   3. Set fs->last_clst = 1 so f_expand searches from cluster 2.
 *   4. Create source_path again with f_expand size, opt=1 (contig).
 *   5. Copy /.defrag.scratch -> source_path.
 *   6. Unlink /.defrag.scratch.
 *
 * Peak disk usage: 2x file size (source + scratch coexist in step 1).
 * Required contig free for step 4: file size, but now satisfiable
 * because the source's old fragments joined the free pool in step 2.
 * Used for the stuck big file the donor-evacuation pattern alone
 * can't help. Returns 0 on success, negative on error:
 *   -1 f_open source, -2 f_open scratch, -3 read/write copy to scratch,
 *   -4 unlink source, -5 f_open new file, -6 f_expand contig,
 *   -7 read/write copy from scratch, -8 verify not contiguous */
static int defrag_one_via_scratch(const char *src_path, uint16_t *fb,
                                   int done, int total) {
    FIL src;
    if (f_open(&src, src_path, FA_READ) != FR_OK) return -1;
    FSIZE_t sz = f_size(&src);

    f_unlink(DEFRAG_SCRATCH);

    FIL dst;
    if (f_open(&dst, DEFRAG_SCRATCH, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        f_close(&src);
        return -2;
    }

    static uint8_t buf[DEFRAG_BUFSZ];
    UINT br, bw;
    FSIZE_t copied = 0;
    while (copied < sz) {
        UINT want = (sz - copied > DEFRAG_BUFSZ) ? DEFRAG_BUFSZ : (UINT)(sz - copied);
        if (f_read(&src, buf, want, &br) != FR_OK || br != want) {
            f_close(&dst); f_close(&src); f_unlink(DEFRAG_SCRATCH);
            return -3;
        }
        if (f_write(&dst, buf, br, &bw) != FR_OK || bw != br) {
            f_close(&dst); f_close(&src); f_unlink(DEFRAG_SCRATCH);
            return -3;
        }
        copied += br;
        if ((copied & 0x3FFF) == 0) {
            defrag_progress_impl(fb, "saving cart", src_path, done, total, true);
        }
    }
    f_close(&dst);
    f_close(&src);
    nes_flash_disk_flush();

    /* Free the original's clusters so they rejoin the free pool. */
    if (f_unlink(src_path) != FR_OK) {
        f_unlink(DEFRAG_SCRATCH);
        return -4;
    }
    nes_flash_disk_flush();

    /* Force the allocator to prefer cluster 2 so the rewrite lands
     * at the front of the volume. */
    g_fs.last_clst = 1;

    FIL newf;
    if (f_open(&newf, src_path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        f_unlink(DEFRAG_SCRATCH);
        return -5;
    }
    if (f_expand(&newf, sz, 1) != FR_OK) {
        f_close(&newf);
        f_unlink(src_path);      /* partial file left behind */
        f_unlink(DEFRAG_SCRATCH);
        return -6;
    }

    FIL scr;
    if (f_open(&scr, DEFRAG_SCRATCH, FA_READ) != FR_OK) {
        f_close(&newf); f_unlink(src_path); f_unlink(DEFRAG_SCRATCH);
        return -7;
    }
    copied = 0;
    while (copied < sz) {
        UINT want = (sz - copied > DEFRAG_BUFSZ) ? DEFRAG_BUFSZ : (UINT)(sz - copied);
        if (f_read(&scr, buf, want, &br) != FR_OK || br != want) {
            f_close(&scr); f_close(&newf);
            f_unlink(src_path); f_unlink(DEFRAG_SCRATCH);
            return -7;
        }
        if (f_write(&newf, buf, br, &bw) != FR_OK || bw != br) {
            f_close(&scr); f_close(&newf);
            f_unlink(src_path); f_unlink(DEFRAG_SCRATCH);
            return -7;
        }
        copied += br;
        if ((copied & 0x3FFF) == 0) {
            defrag_progress_impl(fb, "placing contig", src_path, done, total, true);
        }
    }
    f_close(&scr);
    f_close(&newf);
    f_unlink(DEFRAG_SCRATCH);
    nes_flash_disk_flush();

    /* Verify. */
    if (f_open(&src, src_path, FA_READ) == FR_OK) {
        DWORD sc = src.obj.sclust;
        FSIZE_t vsz = f_size(&src);
        f_close(&src);
        if (vsz > 0 && sc >= 2) {
            DWORD bpc = (DWORD)g_fs.csize * 512u;
            DWORD nc = ((DWORD)vsz + bpc - 1) / bpc;
            if (!chain_is_contiguous(sc, nc)) return -8;
        }
    }
    return 0;
}

/* ==========================================================================
 * Cluster-level defragmenter (replaces file-level Norton+evacuate approach).
 *
 * The file-level approach failed on its own failure mode: a file whose
 * scattered fragments occupy the only region where it could be placed
 * contiguously. Rewriting via a scratch file needs 2x file-size free, which
 * is unavailable on a near-full volume.
 *
 * This cluster-level approach works at physical cluster granularity, using
 * an in-place cycle sort with a single 8 KB RAM pivot. It can compact a
 * 99%-full volume as long as we have 2 clusters of free space anywhere
 * (for the two pivot buffers in RAM + the cycle dynamics).
 *
 * Algorithm (from Norton SpeedDisk + e4defrag adapted to FAT16):
 *
 *   1. ANALYZE:
 *      - BFS the directory tree. For each SUBDIRECTORY, mark its cluster
 *        chain as PINNED (we don't move dir clusters because their `.`
 *        and `..` entries would need fixup — out of scope).
 *      - For each FILE, record path, current sclust, n_clusters, and the
 *        (LBA, offset) of its short directory entry — captured from
 *        DIR.sect / DIR.dir while FatFs has them positioned on the SFN.
 *      - Sort files by cluster count DESC.
 *      - Build current_owner[] map from each file's FAT chain.
 *      - Compute target layout: for each file (in size-DESC order), find
 *        the leftmost n_clusters consecutive non-pinned clusters. That's
 *        the file's target sclust range. Build target_owner[] alongside.
 *
 *   2. PREVIEW: render "current" + "after" cluster maps stacked, plus a
 *      move-count summary. Wait for A (apply) or B (cancel).
 *
 *   3. EXECUTE (only if user confirmed):
 *      a. Cycle sort. For each cluster c where current_owner[c] !=
 *         target_owner[c], start a cycle:
 *           - read cluster c's data into buf_a
 *           - follow permutation: data at c belongs at dest = target_of(c).
 *             Read dest into buf_b, write buf_a there, swap buffers, repeat.
 *           - Cycle closes when dest == start (write buf_a back) or when
 *             dest was free (start becomes free).
 *      b. FAT rebuild. Build in-RAM image: file clusters get contiguous
 *         chain entries, free clusters get 0, pinned clusters keep their
 *         original value. Write to all FAT copies.
 *      c. Directory entry patches. For each moved file, rewrite the SFN's
 *         first-cluster-lo field (bytes 26-27 of the 32-byte entry) to
 *         point at its new target_sclust.
 *      d. Remount FatFs (f_unmount + f_mount) so all caches are dropped.
 *
 * Risks (surface them to the user via the preview step):
 *   - No journal. Crash mid-move corrupts one cluster. Crash mid-FAT-
 *     rebuild gives inconsistent state. "DO NOT POWER OFF" is shown.
 *   - Subdirectory clusters are pinned. Files can't be truly contiguous
 *     if their target range crosses a pinned cluster — we skip those for
 *     them (target placement advances past pinned). Chained-XIP mmap
 *     still handles the residual.
 *   - Only tested on 9.6 MB / FAT16 / 4 KB clusters / ~134 files.
 */

enum {
    /* Must cover every file+dir on the shared FAT, or phase 3b's
     * from-scratch FAT rebuild zeros the missing ones' chains —
     * corrupting them even though the defrag itself is correct.
     *
     * Sized for the worst realistic case: a MicroPython game with
     * many small asset files (sprites, tilemaps, fonts) alongside
     * NES/GB/SMS ROMs and their sidecars. 2000 is the physical
     * ceiling at which allocations start crowding the defrag's
     * cluster maps on a 9.4 MB volume; above that, files would
     * average < 1 cluster and the volume is effectively full of
     * directory entries.
     *
     * Cost is dynamic-only — malloc'd in nes_picker_defrag_compact
     * and free'd on exit. 2000 × 24 B = 48 KB during defrag, 0
     * permanent SRAM. */
    CLD_MAX_FILES        = 2000,
    CLD_MAX_DIRS         = 32,
    /* Defrag-local path buffer size. Must be large enough for the
     * deepest nested path on the shared FAT — e.g.
     * /games/<longname>/assets/sprites/<longname>.bmp which can
     * easily run to 120+ chars. NES_PICKER_NAME_MAX (96) was the
     * original ROM filename cap, too short here, causing snprintf
     * to silently truncate and f_opendir to fail on the mangled
     * path — dropping entire subtrees from the analysis. */
    CLD_PATH_MAX         = 192,
    /* Upper cap on cluster count so our heap allocations can't run
     * away on a misidentified volume. 32k clusters × 4 bytes ×
     * 2 owner maps = 256 KB — too much. Cap at 16k (128 KB for maps).
     * Anything beyond that, the user can't realistically defrag with
     * this code; we'd need the dir walk + buffers strategy. */
    CLD_ABS_MAX_CLUSTERS = 16384,
};

typedef struct {
    DWORD    current_sclust;
    DWORD    target_sclust;       /* 0 = couldn't place, leave alone */
    uint32_t n_clusters;
    /* Parent-relative address of this entry's 32-byte SFN slot.
     *   parent_idx == -1  : entry lives in the root directory
     *                       (byte_in_parent is an offset from g_fs.dirbase)
     *   parent_idx >= 0   : entry lives inside files[parent_idx]
     *                       (byte_in_parent is an offset from the parent's
     *                       first data cluster; parent is contiguous
     *                       post-defrag so cluster_idx = byte / bpc). */
    int      parent_idx;
    DWORD    byte_in_parent;
    uint8_t  is_dir;              /* 1 = subdirectory, 0 = regular file */
} cld_file_t;

typedef struct {
    cld_file_t *files;
    int         n_files;
    uint32_t   *current_owner;   /* (fidx+1) << 16 | offset; 0 = free */
    uint32_t   *target_owner;    /* same encoding for target layout */
    uint8_t    *pinned_bits;     /* bit set => cluster pinned (subdir) */
    uint8_t    *done_bits;       /* bit set => cluster correctly placed */
    uint8_t    *buf_a;
    uint8_t    *buf_b;
    DWORD       n_clust;
    DWORD       bpc;
} cld_ctx_t;

#define CLD_BIT_GET(arr, i)  ((arr)[(i) >> 3] & (1u << ((i) & 7)))
#define CLD_BIT_SET(arr, i)  ((arr)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)))

/* Walk a FAT16 chain starting at `sclust`, invoking cb(cluster) for each
 * cluster in the chain. Stops on EOF / bad entry / chain longer than the
 * volume. `cb_arg` is an opaque passthrough. Returns # clusters walked. */
static int cld_walk_fat(DWORD sclust,
                         void (*cb)(DWORD clst, void *arg),
                         void *cb_arg) {
    if (sclust < 2 || sclust >= g_fs.n_fatent) return 0;
    static uint8_t fat_sec[512];
    LBA_t cached = (LBA_t)-1;
    DWORD clst = sclust;
    int n = 0;
    while (clst >= 2 && clst < g_fs.n_fatent && n < CLD_ABS_MAX_CLUSTERS) {
        cb(clst, cb_arg);
        n++;
        DWORD eb = clst * 2;
        DWORD es = eb / 512;
        DWORD eo = eb % 512;
        LBA_t lba = (LBA_t)g_fs.fatbase + es;
        if (lba != cached) {
            if (nes_flash_disk_read(fat_sec, (uint32_t)lba, 1) != 0) break;
            cached = lba;
        }
        uint16_t next = (uint16_t)fat_sec[eo] | ((uint16_t)fat_sec[eo + 1] << 8);
        if (next == 0xFFFF || next < 2) break;
        clst = next;
    }
    return n;
}

static void cld_cb_pin(DWORD clst, void *arg) {
    cld_ctx_t *ctx = (cld_ctx_t *)arg;
    DWORD ci = clst - 2;
    if (ci < ctx->n_clust) CLD_BIT_SET(ctx->pinned_bits, ci);
}

typedef struct {
    cld_ctx_t *ctx;
    int        fidx;
    uint32_t   off;
} cld_owner_arg_t;

static void cld_cb_set_owner(DWORD clst, void *arg) {
    cld_owner_arg_t *oa = (cld_owner_arg_t *)arg;
    DWORD ci = clst - 2;
    if (ci < oa->ctx->n_clust) {
        oa->ctx->current_owner[ci] = ((uint32_t)(oa->fidx + 1) << 16) | (oa->off & 0xFFFF);
    }
    oa->off++;
}

/* --- target-layout planner: block-subset search --------------------
 *
 * Files on the volume partition naturally into "blocks" (maximal runs
 * of contiguous files with no gaps between them). Each block has a
 * binary choice in the target layout: STAY at its current position,
 * or SHIFT left into the growing "packed" region.
 *
 * For N blocks, 2^N configurations. For each we can compute
 * (moves, max_free_run) exactly, then score by a cost function that
 * trades writes against free-space consolidation. We enumerate all
 * subsets up to 2^16 = 65536; above that we fall back to a greedy
 * hill-climb that evaluates ~N² candidate shifts.
 *
 * Fragmented files (non-contiguous current chains) MUST move and are
 * placed by best-fit-decreasing into the resulting free runs. Pinned
 * clusters (orphan FAT entries, subdirectory clusters ThumbyOne
 * preserves, etc.) are immovable obstacles that shifted blocks must
 * route around.
 *
 * Scoring function:
 *     score = moves − K × max(0, plan_free − current_max_free)
 *                   + P × max(0, current_max_free − plan_free)
 * with K=5 (a cluster of consolidated free worth 5 writes) and
 * P = a very large penalty so plans that regress the current
 * max-free-run lose to any non-regressing plan with reasonable moves.
 *
 * This planner replaces the earlier sweep of heuristics (Plan A /
 * Plan B / smart-surgical at multiple thresholds). Those heuristics
 * each corresponded to a fixed subset choice; the search here visits
 * every possible subset in one pass. */

#define CLD_SCORE_REGRESS_P    1000000
#define CLD_BLOCK_EXHAUSTIVE   16     /* 2^16 = 65k subsets; any more falls back to greedy */

/* K weights (writes per cluster of contiguous-free gained). User
 * adjusts with LEFT/RIGHT on the preview screen to bias the plan
 * toward fewer writes (low K) or more free-space consolidation
 * (high K). Log-ish scale so a single step makes a visible
 * difference across the spectrum. Default starts mid-range. */
static const int s_plan_k_values[] = { 0, 1, 2, 3, 5, 8, 13, 25, 50, 100, 200, 500 };
#define CLD_PLAN_K_COUNT ((int)(sizeof(s_plan_k_values) / sizeof(s_plan_k_values[0])))
#define CLD_PLAN_K_DEFAULT_IDX 4   /* K=5 */
static int s_plan_k_idx = CLD_PLAN_K_DEFAULT_IDX;


typedef struct {
    DWORD    start;        /* current cluster position (0-based) */
    uint32_t size;         /* total clusters in block */
    int      first_file;   /* offset into block_files[] for this block's files */
    int      n_files;      /* count */
} cld_block_t;

typedef struct {
    cld_block_t *blocks;
    int         *block_files;   /* flat array of file indices, grouped per block */
    int          n_blocks;
    int         *fragments;     /* file indices whose chain is fragmented */
    int          n_fragments;
    DWORD       *pinned_list;   /* sorted pinned cluster positions */
    int          n_pinned;
    uint32_t     current_max_free;
} cld_layout_t;

static int cld_cmp_file_by_sclust(cld_ctx_t *ctx, int a, int b) {
    DWORD sa = ctx->files[a].current_sclust;
    DWORD sb = ctx->files[b].current_sclust;
    return (sa < sb) ? -1 : (sa > sb ? 1 : 0);
}

/* Partition ctx->files[] into contiguous blocks + fragments. */
static int cld_layout_build(cld_ctx_t *ctx, cld_layout_t *out) {
    int n_files = ctx->n_files;

    int *contig = (int *)malloc((size_t)n_files * sizeof(int));
    int *frag   = (int *)malloc((size_t)n_files * sizeof(int));
    if (!contig || !frag) {
        free(contig); free(frag);
        return -1;
    }
    int n_contig = 0, n_frag = 0;
    for (int f = 0; f < n_files; f++) {
        uint32_t nc = ctx->files[f].n_clusters;
        if (nc == 0) continue;
        if (chain_is_contiguous(ctx->files[f].current_sclust, nc)) {
            contig[n_contig++] = f;
        } else {
            frag[n_frag++] = f;
        }
    }

    /* Sort contig by current start position. */
    for (int i = 1; i < n_contig; i++) {
        int key = contig[i];
        DWORD key_s = ctx->files[key].current_sclust;
        int j = i - 1;
        while (j >= 0 && ctx->files[contig[j]].current_sclust > key_s) {
            contig[j + 1] = contig[j];
            j--;
        }
        contig[j + 1] = key;
    }

    /* Merge back-to-back contiguous files into blocks. */
    cld_block_t *blocks = (cld_block_t *)malloc((size_t)(n_contig + 1) * sizeof(cld_block_t));
    if (!blocks) { free(contig); free(frag); return -1; }
    int n_blocks = 0;
    int i = 0;
    while (i < n_contig) {
        DWORD bs = ctx->files[contig[i]].current_sclust - 2;
        uint32_t bsize = ctx->files[contig[i]].n_clusters;
        int j = i + 1;
        while (j < n_contig) {
            DWORD next_s = ctx->files[contig[j]].current_sclust - 2;
            if (next_s == bs + bsize) {
                bsize += ctx->files[contig[j]].n_clusters;
                j++;
            } else break;
        }
        blocks[n_blocks].start      = bs;
        blocks[n_blocks].size       = bsize;
        blocks[n_blocks].first_file = i;
        blocks[n_blocks].n_files    = j - i;
        n_blocks++;
        i = j;
    }

    /* Pinned-cluster list. */
    DWORD *pinned = (DWORD *)malloc(((size_t)ctx->n_clust + 1) * sizeof(DWORD));
    if (!pinned) { free(contig); free(frag); free(blocks); return -1; }
    int n_pinned = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        if (CLD_BIT_GET(ctx->pinned_bits, c)) pinned[n_pinned++] = c;
    }

    /* Current max-free-run (for scoring). */
    uint32_t cur_free = 0, max_free = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        int pin = CLD_BIT_GET(ctx->pinned_bits, c) ? 1 : 0;
        if (!pin && ctx->current_owner[c] == 0) {
            cur_free++;
            if (cur_free > max_free) max_free = cur_free;
        } else cur_free = 0;
    }

    out->blocks           = blocks;
    out->block_files      = contig;
    out->n_blocks         = n_blocks;
    out->fragments        = frag;
    out->n_fragments      = n_frag;
    out->pinned_list      = pinned;
    out->n_pinned         = n_pinned;
    out->current_max_free = max_free;
    return 0;
}

static void cld_layout_free(cld_layout_t *layout) {
    free(layout->blocks);       layout->blocks = NULL;
    free(layout->block_files);  layout->block_files = NULL;
    free(layout->fragments);    layout->fragments = NULL;
    free(layout->pinned_list);  layout->pinned_list = NULL;
}

/* Find smallest P >= cursor such that [P, P + size) contains no
 * pinned cluster and P + size <= n_clust. Returns n_clust on fail. */
static DWORD cld_find_clear_range(const cld_layout_t *layout, DWORD n_clust,
                                    DWORD cursor, uint32_t size) {
    while (cursor + size <= n_clust) {
        int p = 0;
        while (p < layout->n_pinned && layout->pinned_list[p] < cursor) p++;
        if (p >= layout->n_pinned || layout->pinned_list[p] >= cursor + size) {
            return cursor;
        }
        cursor = layout->pinned_list[p] + 1;
    }
    return n_clust;
}

/* Evaluate one subset mask — writes out block/fragment targets,
 * moves, and max_free_run. Returns 0 on valid config, -1 otherwise. */
typedef struct {
    DWORD    start;
    uint32_t length;
} cld_run_t;

static int cld_sim_subset(cld_ctx_t *ctx, const cld_layout_t *layout,
                           uint32_t mask,
                           DWORD *block_targets,     /* size n_blocks */
                           DWORD *fragment_targets,  /* size n_fragments */
                           int *out_moves,
                           uint32_t *out_max_free) {
    int moves = 0;
    DWORD cursor = 0;
    for (int i = 0; i < layout->n_blocks; i++) {
        const cld_block_t *b = &layout->blocks[i];
        bool shift = (mask >> i) & 1u;
        if (shift) {
            DWORD t = cld_find_clear_range(layout, ctx->n_clust, cursor, b->size);
            if (t >= ctx->n_clust) return -1;
            block_targets[i] = t;
            if (t != b->start) moves += (int)b->size;
            cursor = t + b->size;
        } else {
            if (b->start < cursor) return -1;
            block_targets[i] = b->start;
            cursor = b->start + b->size;
        }
    }

    /* Compute free runs of the block-placed layout. */
    /* Occupied intervals: each block [target, target+size) + each pinned cluster. */
    int max_runs = layout->n_blocks + layout->n_pinned + 2;
    cld_run_t *runs = (cld_run_t *)malloc((size_t)max_runs * sizeof(cld_run_t));
    if (!runs) return -1;

    /* Build sorted occupied-interval list. Blocks are NOT sorted by
     * target (they're sorted by current position); sort into position
     * order for run computation. */
    typedef struct { DWORD s, e; } cld_iv_t;
    cld_iv_t *iv = (cld_iv_t *)malloc((size_t)(layout->n_blocks + layout->n_pinned) * sizeof(cld_iv_t));
    if (!iv) { free(runs); return -1; }
    int n_iv = 0;
    for (int i = 0; i < layout->n_blocks; i++) {
        iv[n_iv].s = block_targets[i];
        iv[n_iv].e = block_targets[i] + layout->blocks[i].size;
        n_iv++;
    }
    for (int p = 0; p < layout->n_pinned; p++) {
        iv[n_iv].s = layout->pinned_list[p];
        iv[n_iv].e = layout->pinned_list[p] + 1;
        n_iv++;
    }
    /* Insertion sort by start. */
    for (int i = 1; i < n_iv; i++) {
        cld_iv_t key = iv[i];
        int j = i - 1;
        while (j >= 0 && iv[j].s > key.s) { iv[j+1] = iv[j]; j--; }
        iv[j+1] = key;
    }
    /* Walk, emit free runs. */
    int n_runs = 0;
    DWORD pos = 0;
    for (int i = 0; i < n_iv; i++) {
        if (iv[i].s > pos && n_runs < max_runs) {
            runs[n_runs].start  = pos;
            runs[n_runs].length = iv[i].s - pos;
            n_runs++;
        }
        if (iv[i].e > pos) pos = iv[i].e;
    }
    if (pos < ctx->n_clust && n_runs < max_runs) {
        runs[n_runs].start  = pos;
        runs[n_runs].length = ctx->n_clust - pos;
        n_runs++;
    }
    free(iv);

    /* Fragments sorted by size DESC, BFD-place into runs. */
    int *frag_sorted = (int *)malloc((size_t)layout->n_fragments * sizeof(int));
    if (!frag_sorted && layout->n_fragments > 0) { free(runs); return -1; }
    for (int i = 0; i < layout->n_fragments; i++) frag_sorted[i] = layout->fragments[i];
    for (int i = 1; i < layout->n_fragments; i++) {
        int key = frag_sorted[i];
        uint32_t key_sz = ctx->files[key].n_clusters;
        int j = i - 1;
        while (j >= 0 && ctx->files[frag_sorted[j]].n_clusters < key_sz) {
            frag_sorted[j+1] = frag_sorted[j]; j--;
        }
        frag_sorted[j+1] = key;
    }
    int i_frag_target = 0;
    for (int i = 0; i < layout->n_fragments; i++) {
        int f = frag_sorted[i];
        uint32_t sz = ctx->files[f].n_clusters;
        /* Best-fit: smallest run >= sz. */
        int best = -1;
        for (int r = 0; r < n_runs; r++) {
            if (runs[r].length >= sz) {
                if (best < 0 || runs[r].length < runs[best].length) best = r;
            }
        }
        if (best < 0) { free(frag_sorted); free(runs); return -1; }
        /* Map back to the original fragments[] index so caller can
         * reconstruct. fragments[] order mirrors layout->fragments. */
        int orig_idx = -1;
        for (int k = 0; k < layout->n_fragments; k++) {
            if (layout->fragments[k] == f) { orig_idx = k; break; }
        }
        if (orig_idx >= 0) fragment_targets[orig_idx] = runs[best].start;
        runs[best].start  += sz;
        runs[best].length -= sz;
        moves += (int)sz;
        i_frag_target++;
    }
    free(frag_sorted);

    /* max_free = largest remaining run length. */
    uint32_t max_free = 0;
    for (int r = 0; r < n_runs; r++) {
        if (runs[r].length > max_free) max_free = runs[r].length;
    }
    free(runs);

    *out_moves    = moves;
    *out_max_free = max_free;
    return 0;
}

/* Weighted score — lower is better. K is read from the module-level
 * s_plan_k_idx so the preview's LEFT/RIGHT buttons can re-plan
 * without re-plumbing the parameter through every call site. */
static int cld_score_weighted(int moves, uint32_t plan_free, uint32_t current_max_free) {
    if (plan_free < current_max_free) {
        return moves + CLD_SCORE_REGRESS_P * (int)(current_max_free - plan_free);
    }
    int k = s_plan_k_values[s_plan_k_idx];
    return moves - k * (int)(plan_free - current_max_free);
}

/* Apply a winning (mask, targets) to ctx->target_owner and
 * ctx->files[].target_sclust. */
static void cld_apply_mask(cld_ctx_t *ctx, const cld_layout_t *layout,
                            const DWORD *block_targets,
                            const DWORD *fragment_targets) {
    memset(ctx->target_owner, 0, ctx->n_clust * sizeof(uint32_t));
    for (int f = 0; f < ctx->n_files; f++) ctx->files[f].target_sclust = 0;

    /* Blocks: walk each block's files in order, placing contiguously
     * starting at block_targets[b]. */
    for (int b = 0; b < layout->n_blocks; b++) {
        DWORD base = block_targets[b];
        DWORD off  = 0;
        for (int k = 0; k < layout->blocks[b].n_files; k++) {
            int f = layout->block_files[layout->blocks[b].first_file + k];
            ctx->files[f].target_sclust = base + off + 2;
            uint32_t sz = ctx->files[f].n_clusters;
            for (uint32_t j = 0; j < sz; j++) {
                ctx->target_owner[base + off + j] =
                    ((uint32_t)(f + 1) << 16) | (j & 0xFFFF);
            }
            off += sz;
        }
    }

    /* Fragments. */
    for (int i = 0; i < layout->n_fragments; i++) {
        int f = layout->fragments[i];
        DWORD base = fragment_targets[i];
        uint32_t sz = ctx->files[f].n_clusters;
        ctx->files[f].target_sclust = base + 2;
        for (uint32_t j = 0; j < sz; j++) {
            ctx->target_owner[base + j] =
                ((uint32_t)(f + 1) << 16) | (j & 0xFFFF);
        }
    }
}

/* Main block-search planner. */
static int cld_plan_block_search(cld_ctx_t *ctx) {
    cld_layout_t layout = {0};
    if (cld_layout_build(ctx, &layout) != 0) return -1;

    int n_blocks = layout.n_blocks;

    /* Scratch buffers for the current candidate and the best-so-far. */
    DWORD *bt_cur  = (DWORD *)malloc((size_t)(n_blocks + 1) * sizeof(DWORD));
    DWORD *ft_cur  = (DWORD *)malloc((size_t)(layout.n_fragments + 1) * sizeof(DWORD));
    DWORD *bt_best = (DWORD *)malloc((size_t)(n_blocks + 1) * sizeof(DWORD));
    DWORD *ft_best = (DWORD *)malloc((size_t)(layout.n_fragments + 1) * sizeof(DWORD));
    if ((n_blocks > 0 && (!bt_cur || !bt_best)) ||
        (layout.n_fragments > 0 && (!ft_cur || !ft_best))) {
        free(bt_cur); free(ft_cur); free(bt_best); free(ft_best);
        cld_layout_free(&layout);
        return -1;
    }

    int       best_score = INT_MAX;
    int       best_moves = INT_MAX;
    uint32_t  best_free  = 0;
    bool      have_best  = false;
    uint32_t  best_mask  = 0;

    int trials_tried = 0, trials_valid = 0;
    int initial_score = 0;

    if (n_blocks <= CLD_BLOCK_EXHAUSTIVE) {
        /* Exhaustive enumeration of every block-shift subset. */
        uint32_t limit = (n_blocks == 0) ? 1u : (1u << n_blocks);
        for (uint32_t mask = 0; mask < limit; mask++) {
            int moves;
            uint32_t max_free;
            trials_tried++;
            if (cld_sim_subset(ctx, &layout, mask, bt_cur, ft_cur,
                                &moves, &max_free) != 0) continue;
            trials_valid++;
            int score = cld_score_weighted(moves, max_free, layout.current_max_free);
            if (mask == 0) initial_score = score;
            if (score < best_score) {
                best_score = score; best_moves = moves; best_free = max_free;
                best_mask = mask; have_best = true;
                for (int i = 0; i < n_blocks; i++) bt_best[i] = bt_cur[i];
                for (int i = 0; i < layout.n_fragments; i++) ft_best[i] = ft_cur[i];
            }
        }
    } else {
        /* Multi-seed bit-flip hill-climb. Forward-only greedy (add
         * one shift at a time starting from mask=0) gets stuck when
         * the mask=0 config is invalid — e.g. a fragmented file too
         * large for any free run at current layout, so NO single
         * shift creates a big enough run either. From that start,
         * every trial returns -1 and greedy exits with no plan.
         *
         * Bit-flip hill-climb from multiple seed masks escapes this.
         * Seeds cover the corners of the search space: all-stay,
         * all-shift, and two half-splits. From each seed we flip any
         * bit that improves the score and loop until no flip helps.
         * Tracks global best across seeds. */
        uint32_t all_mask = (n_blocks >= 32) ? ~0u : ((1u << n_blocks) - 1);
        uint32_t seeds[4];
        int n_seeds = 0;
        seeds[n_seeds++] = 0;
        seeds[n_seeds++] = all_mask;
        if (n_blocks >= 2) {
            int half = n_blocks / 2;
            uint32_t first_half = ((1u << half) - 1);
            seeds[n_seeds++] = first_half;
            seeds[n_seeds++] = all_mask & ~first_half;
        }

        for (int s = 0; s < n_seeds; s++) {
            uint32_t mask = seeds[s];
            int      cur_moves = 0;
            uint32_t cur_free  = 0;
            int      cur_score = INT_MAX;
            bool     cur_valid = false;

            trials_tried++;
            if (cld_sim_subset(ctx, &layout, mask, bt_cur, ft_cur,
                                &cur_moves, &cur_free) == 0) {
                trials_valid++;
                cur_valid = true;
                cur_score = cld_score_weighted(cur_moves, cur_free,
                                                 layout.current_max_free);
                if (mask == 0) initial_score = cur_score;
            }

            /* Hill-climb: try flipping each bit; accept best strict
             * improvement each round. Cap rounds at 2*n_blocks so we
             * can't loop forever on degenerate layouts. */
            for (int round = 0; round < 2 * n_blocks; round++) {
                int      best_flip = -1;
                int      best_flip_score = cur_valid ? cur_score : INT_MAX;
                int      flip_moves = 0;
                uint32_t flip_free  = 0;

                for (int b = 0; b < n_blocks; b++) {
                    uint32_t trial = mask ^ (1u << b);
                    int      tm;
                    uint32_t tf;
                    trials_tried++;
                    if (cld_sim_subset(ctx, &layout, trial, bt_cur, ft_cur,
                                        &tm, &tf) != 0) continue;
                    trials_valid++;
                    int ts = cld_score_weighted(tm, tf,
                                                  layout.current_max_free);
                    if (ts < best_flip_score) {
                        best_flip = b;
                        best_flip_score = ts;
                        flip_moves = tm;
                        flip_free = tf;
                    }
                }
                if (best_flip < 0) break;
                mask ^= (1u << best_flip);
                cur_score = best_flip_score;
                cur_moves = flip_moves;
                cur_free  = flip_free;
                cur_valid = true;
            }

            if (cur_valid && cur_score < best_score) {
                best_score = cur_score;
                best_moves = cur_moves;
                best_free  = cur_free;
                best_mask  = mask;
                /* Re-run sim to populate bt_best / ft_best with the
                 * winning layout's actual targets. */
                if (cld_sim_subset(ctx, &layout, mask, bt_best, ft_best,
                                    &best_moves, &best_free) == 0) {
                    have_best = true;
                }
            }
        }
    }


    if (have_best) {
        cld_apply_mask(ctx, &layout, bt_best, ft_best);
    } else {
        /* Degenerate: no valid config. Treat all as unplaceable. */
        memset(ctx->target_owner, 0, ctx->n_clust * sizeof(uint32_t));
        for (int f = 0; f < ctx->n_files; f++) ctx->files[f].target_sclust = 0;
    }

    (void)best_moves; (void)best_free; (void)best_mask; (void)best_score;
    (void)initial_score; (void)trials_tried; (void)trials_valid;
    free(bt_cur); free(ft_cur); free(bt_best); free(ft_best);
    cld_layout_free(&layout);
    return 0;
}

/* Score the current plan: (unplaceable files, cluster writes,
 * largest contiguous free run in the planned target layout).
 *
 * Writes = non-pinned clusters where target_owner is non-zero and
 * differs from current_owner. These are the destination slots phase
 * 3a actually writes new data into. Clusters where target is 0 but
 * current is non-zero (vacated sources) are NOT writes — cycle-sort
 * drops those in-place. Previously we double-counted both directions,
 * which distorted plan comparison in favour of plans that wasted
 * writes.
 *
 * max_free_run = the longest run of non-pinned clusters in the target
 * layout where target_owner == 0. This is what the host's FAT driver
 * sees when allocating a new file, so maximising it is the practical
 * post-defrag win we care about. */
static void cld_plan_score(const cld_ctx_t *ctx,
                            int *out_unplaceable, int *out_moves,
                            uint32_t *out_max_free_run) {
    int u = 0;
    for (int f = 0; f < ctx->n_files; f++) {
        uint32_t nc = ctx->files[f].n_clusters;
        if (nc == 0) continue;
        if (ctx->files[f].target_sclust == 0) u++;
    }
    int m = 0;
    uint32_t cur_free = 0, max_free = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        int pinned = CLD_BIT_GET(ctx->pinned_bits, c) ? 1 : 0;
        if (!pinned && ctx->target_owner[c] != 0
            && ctx->target_owner[c] != ctx->current_owner[c]) m++;
        if (!pinned && ctx->target_owner[c] == 0) {
            cur_free++;
            if (cur_free > max_free) max_free = cur_free;
        } else cur_free = 0;
    }
    *out_unplaceable   = u;
    *out_moves         = m;
    *out_max_free_run  = max_free;
}


/* ANALYZE — returns 0 on success, negative on error. The caller has
 * NOT yet allocated the per-cluster maps; this is the function that
 * actually does the right-sized heap allocation once g_fs.n_fatent
 * is known. */
static int cld_analyze(cld_ctx_t *ctx) {
    ctx->n_clust = (g_fs.n_fatent >= 2) ? (g_fs.n_fatent - 2) : 0;
    if (ctx->n_clust == 0) return -1;
    if (ctx->n_clust > CLD_ABS_MAX_CLUSTERS) return -4;
    ctx->bpc = (DWORD)g_fs.csize * 512u;

    /* Size the per-cluster maps to the actual cluster count. */
    ctx->current_owner =
        (uint32_t *)malloc((size_t)ctx->n_clust * sizeof(uint32_t));
    ctx->target_owner =
        (uint32_t *)malloc((size_t)ctx->n_clust * sizeof(uint32_t));
    ctx->pinned_bits = (uint8_t *)calloc((ctx->n_clust + 7) / 8, 1);
    ctx->done_bits   = (uint8_t *)calloc((ctx->n_clust + 7) / 8, 1);
    if (!ctx->current_owner || !ctx->target_owner
        || !ctx->pinned_bits || !ctx->done_bits) {
        return -5;
    }

    /* The BFS queue has to remember each pending dir's INDEX in
     * files[] so children we enumerate inside it can set parent_idx.
     * Root-level dirs use parent_idx = -1. */
    typedef struct { char path[CLD_PATH_MAX]; int parent_idx; } queue_ent_t;
    queue_ent_t *dir_q = (queue_ent_t *)malloc((size_t)CLD_MAX_DIRS * sizeof(queue_ent_t));
    if (!dir_q) return -6;

    int q_head = 0, q_tail = 0;
    strncpy(dir_q[0].path, "/", CLD_PATH_MAX - 1);
    dir_q[0].path[CLD_PATH_MAX - 1] = 0;
    dir_q[0].parent_idx = -1;
    q_tail = 1;

    int n_files = 0;
    while (q_head < q_tail && n_files < CLD_MAX_FILES) {
        char        cur_dir_path[CLD_PATH_MAX];
        int         cur_parent_idx;
        strncpy(cur_dir_path, dir_q[q_head].path, CLD_PATH_MAX - 1);
        cur_dir_path[CLD_PATH_MAX - 1] = 0;
        cur_parent_idx = dir_q[q_head].parent_idx;
        q_head++;

        DIR dir;
        FILINFO info;
        if (f_opendir(&dir, cur_dir_path) != FR_OK) continue;
        while (n_files < CLD_MAX_FILES && f_readdir(&dir, &info) == FR_OK) {
            if (info.fname[0] == 0) break;
            /* Skip only "." and ".." (FatFs shouldn't surface them
             * but be safe). DO NOT skip dotfiles like /.volume or
             * /.global — they occupy real clusters. */
            if (info.fname[0] == '.'
                && (info.fname[1] == 0
                    || (info.fname[1] == '.' && info.fname[2] == 0))) {
                continue;
            }

            /* DIR.dptr is the SFN's byte offset within the parent's
             * directory stream — BUT FatFs's f_readdir calls
             * dir_next(dp, 0) as its last step, which advances dptr
             * past the SFN we just returned. So after f_readdir,
             * dptr == SFN_offset + 32 (SZDIRE). Subtract the slot
             * size to recover the actual SFN offset.
             *
             * Without this subtraction every subdir entry's
             * byte_in_parent was 32 too high, and phase 3c patched
             * the attribute/reserved bytes of the NEXT entry instead
             * of updating this child's sclust — corrupting the
             * volume and leaving children pointing at their old
             * (now-freed) cluster positions. */
            DWORD e_byte_in_parent = (dir.dptr >= 32) ? (dir.dptr - 32) : 0;

            char full[CLD_PATH_MAX];
            if (cur_dir_path[0] == '/' && cur_dir_path[1] == 0)
                snprintf(full, sizeof(full), "/%s", info.fname);
            else
                snprintf(full, sizeof(full), "%s/%s", cur_dir_path, info.fname);

            DWORD entry_sclust = 0;
            uint8_t is_dir     = (info.fattrib & AM_DIR) ? 1 : 0;
            uint32_t nc        = 0;

            if (is_dir) {
                DIR subd;
                if (f_opendir(&subd, full) != FR_OK) continue;
                entry_sclust = subd.obj.sclust;
                f_closedir(&subd);
                /* Subdirectory size in clusters: walk FAT until EOC.
                 * Most subdirs fit in 1 cluster but a folder with
                 * many entries takes more. */
                nc = 0;
                {
                    DWORD clst = entry_sclust;
                    static uint8_t fs[512];
                    LBA_t cached_fat = (LBA_t)-1;
                    int safety = 64;  /* subdir chains don't get long */
                    while (clst >= 2 && clst < g_fs.n_fatent && safety-- > 0) {
                        nc++;
                        DWORD eb = clst * 2;
                        DWORD es = eb / 512;
                        DWORD eo = eb % 512;
                        LBA_t lba = (LBA_t)g_fs.fatbase + es;
                        if (lba != cached_fat) {
                            if (nes_flash_disk_read(fs, (uint32_t)lba, 1) != 0) break;
                            cached_fat = lba;
                        }
                        uint16_t next = (uint16_t)fs[eo] | ((uint16_t)fs[eo + 1] << 8);
                        if (next == 0xFFFF || next < 2) break;
                        clst = next;
                    }
                }
            } else {
                FIL probe;
                if (f_open(&probe, full, FA_READ) != FR_OK) continue;
                entry_sclust = probe.obj.sclust;
                FSIZE_t sz   = f_size(&probe);
                f_close(&probe);
                if (sz == 0) continue;
                nc = (uint32_t)(((DWORD)sz + ctx->bpc - 1) / ctx->bpc);
            }

            int my_idx = n_files;
            ctx->files[my_idx].current_sclust = entry_sclust;
            ctx->files[my_idx].n_clusters     = nc;
            ctx->files[my_idx].parent_idx     = cur_parent_idx;
            ctx->files[my_idx].byte_in_parent = e_byte_in_parent;
            ctx->files[my_idx].is_dir         = is_dir;
            ctx->files[my_idx].target_sclust  = 0;
            n_files++;

            /* Enqueue subdir for later walking. */
            if (is_dir && q_tail < CLD_MAX_DIRS) {
                strncpy(dir_q[q_tail].path, full, CLD_PATH_MAX - 1);
                dir_q[q_tail].path[CLD_PATH_MAX - 1] = 0;
                dir_q[q_tail].parent_idx = my_idx;
                q_tail++;
            }
        }
        f_closedir(&dir);
    }
    free(dir_q);
    ctx->n_files = n_files;
    if (n_files == 0) return 0;

    /* Build current_owner by walking each entry's FAT chain via the
     * raw FAT16 walker (cld_walk_fat). Works for both files AND
     * subdirectories uniformly. We previously used f_open + f_lseek
     * but that doesn't handle subdirectories (f_open rejects them),
     * which left subdir clusters unmarked in current_owner even
     * though they're counted in target. */
    memset(ctx->current_owner, 0, ctx->n_clust * sizeof(uint32_t));
    for (int f = 0; f < n_files; f++) {
        cld_owner_arg_t oa = { .ctx = ctx, .fidx = f, .off = 0 };
        cld_walk_fat(ctx->files[f].current_sclust, cld_cb_set_owner, &oa);
    }

    /* ORPHAN-CLUSTER SCAN — critical correctness fix.
     *
     * Any cluster the on-disk FAT says is in use but which isn't owned
     * by some file in ctx->files[] belongs to a file the BFS missed
     * (deep subdir, CLD_MAX_FILES/CLD_MAX_DIRS cap hit, f_opendir
     * transient failure, etc.). If we leave those clusters unmarked,
     * phase 3b's from-scratch FAT rebuild ZEROES them — their dir
     * entries still point at the old sclust but the chain is now
     * broken, and count_fragmented_named reports them as fragmented.
     * Worst case: the cycle-sort in phase 3a overwrites the cluster
     * data before phase 3b wipes the chain, losing user content.
     *
     * Pin every such orphan cluster so: (a) phase 3a skips it, (b)
     * phase 3b preserves its existing FAT entry, (c) target placement
     * routes around it. User data survives even when enumeration is
     * incomplete. */
    {
        static uint8_t fat_sec[512];
        LBA_t cached_lba = (LBA_t)-1;
        for (DWORD ci = 0; ci < ctx->n_clust; ci++) {
            if (ctx->current_owner[ci] != 0) continue;
            if (CLD_BIT_GET(ctx->pinned_bits, ci)) continue;
            DWORD fat_c = ci + 2;
            DWORD eb = fat_c * 2;
            DWORD es = eb / 512;
            DWORD eo = eb % 512;
            LBA_t lba = (LBA_t)g_fs.fatbase + es;
            if (lba != cached_lba) {
                if (nes_flash_disk_read(fat_sec, (uint32_t)lba, 1) != 0) break;
                cached_lba = lba;
            }
            uint16_t v = (uint16_t)fat_sec[eo]
                       | ((uint16_t)fat_sec[eo + 1] << 8);
            /* FAT[c] = 0 → free; = 1 → reserved; = 0xFFF7 → bad; else
             * (2..EOC) → used. Pin only the used entries. */
            if (v == 0 || v == 1 || v == 0xFFF7) continue;
            CLD_BIT_SET(ctx->pinned_bits, ci);
        }
    }

    /* Block-subset search planner: enumerate every block-shift
     * configuration (up to 2^16 subsets, greedy hill-climb above
     * that), score each by (moves − K × free_gain + P × regression),
     * pick the winner. Writes ctx->target_owner and
     * ctx->files[].target_sclust directly. */
    return cld_plan_block_search(ctx);
}

/* Execute-phase overlay. Renders the live cluster state from
 * ctx->current_owner (which cycle sort keeps up to date) instead of
 * walking FatFs — during moves the on-disk FAT is still the OLD FAT
 * so FatFs would return bogus chains pointing at already-shuffled
 * clusters. We also render the FULL volume (n_clust, not a 2600 cap)
 * so the picture matches the preview.
 *
 * Layout: top red DO NOT POWER OFF bar, operation label, full-height
 * cluster map, "N / M" counter + thin progress bar at the bottom. */
static void cld_draw_execute_overlay(uint16_t *fb, const cld_ctx_t *ctx,
                                      const char *title,
                                      int moves, int moves_planned) {
    enum {
        MAP_X = 4, MAP_Y = 22,
        CELL_W = 2, CELL_H = 2,
        MAP_COLS = 60, MAP_ROWS = 40,
        N_CELLS = MAP_COLS * MAP_ROWS,
    };
    static const uint16_t palette[15] = {
        0x07FF, 0xFFE0, 0x07E0, 0xF81F, 0xFD20,
        0xAFE5, 0x87FF, 0xFB56, 0x5E7F, 0xFCE0,
        0xA8F4, 0x6E7D, 0xFA28, 0xBE5B, 0xEF7E,
    };
    const uint16_t FREE_COLOR = 0x10A2;

    fb_clear(fb, COL_BG);
    fb_rect(fb, 0, 0, FB_W, 12, COL_ERR);
    const char *warn = "DO NOT POWER OFF";
    nes_font_draw(fb, warn, (FB_W - nes_font_width(warn)) / 2, 3, COL_FG);
    if (title) {
        nes_font_draw(fb, title,
                       (FB_W - nes_font_width(title)) / 2, 13, COL_TITLE);
    }

    fb_rect(fb, MAP_X - 1, MAP_Y - 1,
            MAP_COLS * CELL_W + 2, MAP_ROWS * CELL_H + 2, 0x4208);

    DWORD n_clust = ctx->n_clust;
    for (int r = 0; r < MAP_ROWS; r++) {
        for (int co = 0; co < MAP_COLS; co++) {
            uint32_t cell_idx = (uint32_t)(r * MAP_COLS + co);
            uint32_t ci_start =
                (uint32_t)(((uint64_t)cell_idx * n_clust) / N_CELLS);
            uint32_t ci_end =
                (uint32_t)(((uint64_t)(cell_idx + 1) * n_clust) / N_CELLS);
            if (ci_end <= ci_start) ci_end = ci_start + 1;
            if (ci_end > n_clust)   ci_end = n_clust;

            int fidx = -1;
            for (uint32_t ci = ci_start; ci < ci_end; ci++) {
                if (ctx->current_owner[ci] != 0) {
                    fidx = (int)((ctx->current_owner[ci] >> 16) - 1);
                    break;
                }
            }
            uint16_t color = (fidx >= 0) ? palette[fidx % 15] : FREE_COLOR;
            fb_rect(fb, MAP_X + co * CELL_W, MAP_Y + r * CELL_H,
                    CELL_W, CELL_H, color);
        }
    }

    /* Counter + bar at bottom. */
    char cnt[24];
    int denom = moves_planned > 0 ? moves_planned : 1;
    int num   = moves > denom ? denom : moves;
    snprintf(cnt, sizeof(cnt), "%d / %d", num, denom);
    nes_font_draw(fb, cnt, (FB_W - nes_font_width(cnt)) / 2, 116, COL_DIM);

    int bar_y = 124, bar_h = 2;
    fb_rect(fb, 4, bar_y, FB_W - 8, bar_h, 0x2104);
    int fill = denom > 0 ? ((FB_W - 8) * num) / denom : 0;
    if (fill > FB_W - 8) fill = FB_W - 8;
    fb_rect(fb, 4, bar_y, fill, bar_h, COL_HIGHLT);

    nes_lcd_wait_idle();
    nes_lcd_present(fb);
    tud_task();
}

/* Render one cluster map as a 60×20 grid (small preview version).
 * Each cell spans (n_clust / N_CELLS) consecutive clusters; we scan
 * the whole span and pick a representative colour so fragmented data
 * that doesn't happen to fall on cell boundaries still shows up. */
static void cld_draw_map_small(uint16_t *fb, const uint32_t *owner_map,
                                const uint8_t *pinned_bits,
                                DWORD n_clust, int y_top) {
    /* 17-row map (34 px tall) leaves 30 px below the bottom map
     * which fits three 8 px stat lines + prompt. Each cell covers
     * n_clust / 1020 clusters (~10 on a 9788-cluster volume). */
    enum { COLS = 60, ROWS = 17, CELL = 2, X0 = 4, N_CELLS = COLS * ROWS };
    static const uint16_t palette[15] = {
        0x07FF, 0xFFE0, 0x07E0, 0xF81F, 0xFD20,
        0xAFE5, 0x87FF, 0xFB56, 0x5E7F, 0xFCE0,
        0xA8F4, 0x6E7D, 0xFA28, 0xBE5B, 0xEF7E,
    };
    /* Border */
    fb_rect(fb, X0 - 1, y_top - 1, COLS * CELL + 2, ROWS * CELL + 2, 0x4208);
    for (int r = 0; r < ROWS; r++) {
        for (int co = 0; co < COLS; co++) {
            uint32_t cell_idx = (uint32_t)(r * COLS + co);
            uint32_t ci_start =
                (uint32_t)(((uint64_t)cell_idx * n_clust) / N_CELLS);
            uint32_t ci_end =
                (uint32_t)(((uint64_t)(cell_idx + 1) * n_clust) / N_CELLS);
            if (ci_end <= ci_start) ci_end = ci_start + 1;
            if (ci_end > n_clust)   ci_end = n_clust;

            /* Priority: pick the FIRST non-free cluster in the span
             * so any colour within the cell wins over free. That's
             * the visually useful thing — seeing that the cell is
             * "occupied" even if only one of N clusters in it is
             * allocated. */
            int  fidx    = -1;
            bool pinned  = false;
            for (uint32_t ci = ci_start; ci < ci_end; ci++) {
                if (CLD_BIT_GET(pinned_bits, ci)) pinned = true;
                if (owner_map[ci] != 0) {
                    fidx = (int)((owner_map[ci] >> 16) - 1);
                    break;
                }
            }
            uint16_t color;
            if (fidx >= 0)      color = palette[fidx % 15];
            else if (pinned)    color = 0x632C;   /* pinned subdir */
            else                color = 0x10A2;   /* free */
            fb_rect(fb, X0 + co * CELL, y_top + r * CELL, CELL, CELL, color);
        }
    }
}

static void cld_show_preview(const cld_ctx_t *ctx, uint16_t *fb,
                              int moves_planned, int unplaceable) {
    fb_clear(fb, COL_BG);

    const char *title = "DEFRAG PREVIEW";
    nes_font_draw(fb, title,
                   (FB_W - nes_font_width(title)) / 2, 2, COL_TITLE);

    nes_font_draw(fb, "before", 4, 12, COL_DIM);
    cld_draw_map_small(fb, ctx->current_owner, ctx->pinned_bits,
                        ctx->n_clust, 20);

    nes_font_draw(fb, "after", 4, 56, COL_DIM);
    cld_draw_map_small(fb, ctx->target_owner, ctx->pinned_bits,
                        ctx->n_clust, 64);

    /* Count files whose CURRENT chain isn't contiguous (skip 1-cluster
     * files — trivially contig). After-defrag count is simply the
     * "stuck" count (every placed entry is contig by construction). */
    int frag_before = 0;
    for (int f = 0; f < ctx->n_files; f++) {
        uint32_t nc = ctx->files[f].n_clusters;
        if (nc <= 1) continue;
        if (!chain_is_contiguous(ctx->files[f].current_sclust, nc)) {
            frag_before++;
        }
    }
    int frag_after = unplaceable;

    /* Largest contiguous free run — before uses current_owner, after
     * uses target_owner. Shows how much contiguous free space a new
     * big upload would find. */
    uint32_t cur_before = 0, max_free_before = 0;
    uint32_t cur_after  = 0, max_free_after  = 0;
    for (DWORD c = 0; c < ctx->n_clust; c++) {
        int pinned = CLD_BIT_GET(ctx->pinned_bits, c) ? 1 : 0;
        if (ctx->current_owner[c] == 0 && !pinned) {
            cur_before++;
            if (cur_before > max_free_before) max_free_before = cur_before;
        } else cur_before = 0;
        if (ctx->target_owner[c] == 0 && !pinned) {
            cur_after++;
            if (cur_after > max_free_after) max_free_after = cur_after;
        } else cur_after = 0;
    }
    uint32_t free_b_kb = (uint32_t)((max_free_before * ctx->bpc) / 1024);
    uint32_t free_a_kb = (uint32_t)((max_free_after  * ctx->bpc) / 1024);

    /* Helper to print a KB count in compact form (e.g. 4700K -> 4.7M). */
    #define CLD_FMT_KB(buf, kb) do { \
        if ((kb) >= 1024) snprintf((buf), sizeof(buf), "%lu.%luM", \
                                    (unsigned long)((kb)/1024), \
                                    (unsigned long)(((kb)%1024)/103)); \
        else              snprintf((buf), sizeof(buf), "%luK", \
                                    (unsigned long)(kb)); \
    } while (0)
    char free_b[16], free_a[16];
    CLD_FMT_KB(free_b, free_b_kb);
    CLD_FMT_KB(free_a, free_a_kb);
    #undef CLD_FMT_KB

    char line1[40];
    snprintf(line1, sizeof(line1), "frag: %d -> %d", frag_before, frag_after);
    nes_font_draw(fb, line1,
                   (FB_W - nes_font_width(line1)) / 2, 100,
                   (frag_after == 0) ? COL_FG : COL_ERR);

    char line2[40];
    snprintf(line2, sizeof(line2), "free: %s -> %s", free_b, free_a);
    nes_font_draw(fb, line2,
                   (FB_W - nes_font_width(line2)) / 2, 107, COL_HIGHLT);

    /* Cost line — how much flash wear this plan commits to. */
    char line3[40];
    snprintf(line3, sizeof(line3), "cost: %d writes", moves_planned);
    nes_font_draw(fb, line3,
                   (FB_W - nes_font_width(line3)) / 2, 114, COL_DIM);

    /* K-weight indicator — user-adjustable with LEFT/RIGHT. Lower K
     * biases toward fewer writes, higher K toward more free-space
     * consolidation. */
    char line4[40];
    snprintf(line4, sizeof(line4), "<K=%d> A=go B=x",
             s_plan_k_values[s_plan_k_idx]);
    nes_font_draw(fb, line4,
                   (FB_W - nes_font_width(line4)) / 2, 121, COL_HIGHLT);

    nes_lcd_wait_idle();
    nes_lcd_present(fb);
}

/* Preview-screen interaction codes. */
enum {
    CLD_UI_APPLY  = 1,
    CLD_UI_CANCEL = 0,
    CLD_UI_K_DOWN = 2,   /* LEFT pressed — bias toward fewer writes */
    CLD_UI_K_UP   = 3,   /* RIGHT pressed — bias toward more free-space */
};

/* Wait for a button edge on the preview screen. Returns one of the
 * CLD_UI_* codes. LEFT / RIGHT edge-detected (single press, not
 * repeat) so a held button doesn't slam through the K range. */
static int cld_wait_interaction(void) {
    /* Drain currently-held buttons first. */
    while (nes_buttons_read() != 0 || nes_buttons_menu_pressed()) {
        tud_task();
        sleep_ms(10);
    }
    while (1) {
        tud_task();
        sleep_ms(10);
        uint8_t b = nes_buttons_read();
        if (b & 0x20) return CLD_UI_APPLY;    /* A */
        if (b & 0x10) return CLD_UI_CANCEL;   /* B */
        if (nes_buttons_menu_pressed()) return CLD_UI_CANCEL;
        if (b & 0x01) return CLD_UI_K_DOWN;   /* LEFT */
        if (b & 0x02) return CLD_UI_K_UP;     /* RIGHT */
    }
}

/* EXECUTE — returns move count on success, negative on error. */
static int cld_execute(cld_ctx_t *ctx, uint16_t *fb, int moves_planned) {
    DWORD n_clust = ctx->n_clust;
    memset(ctx->done_bits, 0, (n_clust + 7) / 8);

    int moves = 0;
    int progress_tick = 0;

    /* --- Phase 3a: cycle sort over file clusters --- */
    for (DWORD c = 0; c < n_clust; c++) {
        if (CLD_BIT_GET(ctx->done_bits, c))   continue;
        if (CLD_BIT_GET(ctx->pinned_bits, c)) continue;
        if (ctx->current_owner[c] == ctx->target_owner[c]) {
            CLD_BIT_SET(ctx->done_bits, c);
            continue;
        }

        /* Start cycle at c. */
        DWORD    pos = c;
        uint32_t carried_id = ctx->current_owner[c];

        LBA_t start_lba = (LBA_t)g_fs.database + (LBA_t)c * g_fs.csize;
        if (nes_flash_disk_read(ctx->buf_a, (uint32_t)start_lba, g_fs.csize) != 0)
            return -10;

        int cycle_safety = (int)n_clust + 4;
        while (cycle_safety-- > 0) {
            DWORD dest;
            if (carried_id == 0) {
                /* Nothing carried — the start cluster is now free. */
                ctx->current_owner[c] = 0;
                CLD_BIT_SET(ctx->done_bits, c);
                break;
            }
            int fidx = (int)((carried_id >> 16) - 1);
            int off  = (int)(carried_id & 0xFFFF);
            if (fidx < 0 || fidx >= ctx->n_files
                || ctx->files[fidx].target_sclust == 0) {
                /* File wasn't placed; write carried back to pos and stop. */
                LBA_t cur_lba =
                    (LBA_t)g_fs.database + (LBA_t)pos * g_fs.csize;
                nes_flash_disk_write(ctx->buf_a, (uint32_t)cur_lba, g_fs.csize);
                CLD_BIT_SET(ctx->done_bits, pos);
                break;
            }
            dest = (ctx->files[fidx].target_sclust - 2) + (DWORD)off;

            if (dest == c) {
                /* Cycle closes at the start. */
                if (nes_flash_disk_write(ctx->buf_a, (uint32_t)start_lba, g_fs.csize) != 0)
                    return -11;
                ctx->current_owner[c] = carried_id;
                CLD_BIT_SET(ctx->done_bits, c);
                moves++;
                break;
            }

            LBA_t dest_lba =
                (LBA_t)g_fs.database + (LBA_t)dest * g_fs.csize;
            if (nes_flash_disk_read(ctx->buf_b, (uint32_t)dest_lba, g_fs.csize) != 0)
                return -12;
            uint32_t dest_id = ctx->current_owner[dest];
            if (nes_flash_disk_write(ctx->buf_a, (uint32_t)dest_lba, g_fs.csize) != 0)
                return -13;
            ctx->current_owner[dest] = carried_id;
            CLD_BIT_SET(ctx->done_bits, dest);
            moves++;

            carried_id = dest_id;
            { uint8_t *tmp = ctx->buf_a; ctx->buf_a = ctx->buf_b; ctx->buf_b = tmp; }
            pos = dest;

            /* Periodic progress + flush. Renders the live cluster
             * state from ctx->current_owner (which cycle sort keeps
             * current) rather than walking FatFs, since FatFs's
             * chains still point at old cluster positions that now
             * hold shuffled data. */
            if ((++progress_tick & 0xF) == 0) {
                nes_flash_disk_flush();
                cld_draw_execute_overlay(fb, ctx, "moving clusters",
                                          moves, moves_planned);
            }
        }
    }
    nes_flash_disk_flush();

    /* --- Phase 3b: rebuild FAT --- */
    cld_draw_execute_overlay(fb, ctx, "rewriting FAT",
                              moves_planned, moves_planned);

    uint16_t *new_fat = (uint16_t *)malloc(n_clust * sizeof(uint16_t));
    if (!new_fat) return -20;
    memset(new_fat, 0, n_clust * sizeof(uint16_t));

    /* Preserve pinned clusters' existing FAT entries. */
    {
        static uint8_t fat_sec[512];
        LBA_t cached = (LBA_t)-1;
        for (DWORD ci = 0; ci < n_clust; ci++) {
            if (!CLD_BIT_GET(ctx->pinned_bits, ci)) continue;
            DWORD c = ci + 2;
            DWORD eb = c * 2;
            DWORD es = eb / 512;
            DWORD eo = eb % 512;
            LBA_t lba = (LBA_t)g_fs.fatbase + es;
            if (lba != cached) {
                if (nes_flash_disk_read(fat_sec, (uint32_t)lba, 1) != 0) {
                    free(new_fat);
                    return -21;
                }
                cached = lba;
            }
            new_fat[ci] = (uint16_t)fat_sec[eo] | ((uint16_t)fat_sec[eo + 1] << 8);
        }
    }

    /* Write contiguous chain for each placed file. For files that
     * couldn't be placed (target_sclust == 0), preserve the original
     * chain by copying from existing FAT. */
    for (int f = 0; f < ctx->n_files; f++) {
        if (ctx->files[f].target_sclust == 0) {
            /* Copy existing chain over — it's still valid. */
            static uint8_t fat_sec[512];
            LBA_t cached = (LBA_t)-1;
            DWORD clst = ctx->files[f].current_sclust;
            int safety = (int)n_clust + 1;
            while (clst >= 2 && clst < g_fs.n_fatent && safety-- > 0) {
                DWORD ci = clst - 2;
                DWORD eb = clst * 2;
                DWORD es = eb / 512;
                DWORD eo = eb % 512;
                LBA_t lba = (LBA_t)g_fs.fatbase + es;
                if (lba != cached) {
                    if (nes_flash_disk_read(fat_sec, (uint32_t)lba, 1) != 0) break;
                    cached = lba;
                }
                uint16_t next = (uint16_t)fat_sec[eo]
                              | ((uint16_t)fat_sec[eo + 1] << 8);
                if (ci < n_clust) new_fat[ci] = next;
                if (next == 0xFFFF || next < 2) break;
                clst = next;
            }
            continue;
        }
        DWORD    tstart = ctx->files[f].target_sclust;
        uint32_t nc     = ctx->files[f].n_clusters;
        for (uint32_t i = 0; i < nc; i++) {
            DWORD ci = tstart + i - 2;
            if (ci >= n_clust) break;
            new_fat[ci] = (i == nc - 1)
                            ? 0xFFFF
                            : (uint16_t)(tstart + i + 1);
        }
    }

    /* Flush new_fat to every FAT copy, preserving bytes we don't own
     * (e.g. reserved entries at cluster 0/1). */
    for (int fatno = 0; fatno < g_fs.n_fats; fatno++) {
        LBA_t fat_start = (LBA_t)g_fs.fatbase + (LBA_t)fatno * g_fs.fsize;
        uint8_t fat_sec[512];
        DWORD total_sectors = g_fs.fsize;
        for (DWORD sec = 0; sec < total_sectors; sec++) {
            if (nes_flash_disk_read(fat_sec, (uint32_t)(fat_start + sec), 1) != 0) {
                free(new_fat);
                return -22;
            }
            DWORD first_c = sec * 256;      /* 256 FAT16 entries per sector */
            for (DWORD i = 0; i < 256; i++) {
                DWORD c = first_c + i;
                if (c < 2 || c >= g_fs.n_fatent) continue;
                DWORD ci = c - 2;
                if (ci >= n_clust) continue;
                uint16_t v = new_fat[ci];
                fat_sec[i * 2]     = (uint8_t)(v & 0xFF);
                fat_sec[i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
            }
            if (nes_flash_disk_write(fat_sec, (uint32_t)(fat_start + sec), 1) != 0) {
                free(new_fat);
                return -23;
            }
        }
    }
    free(new_fat);
    nes_flash_disk_flush();

    /* --- Phase 3c: patch directory entries.
     *
     * Two distinct updates per moved entry:
     *   (i)  Parent's SFN slot for this child: bytes 26/27 = new sclust.
     *        Located at parent's new data stream + byte_in_parent.
     *   (ii) For SUBDIRECTORIES: their own first data cluster contains
     *        the `.` entry (bytes 0-31) whose sclust must point at the
     *        subdir's new location, and the `..` entry (bytes 32-63)
     *        whose sclust must point at the parent's new location
     *        (or 0 if parent is root).
     *
     * Parent-relative addressing: for a child of parent_idx != -1,
     * the parent is contiguous post-move starting at parent.target_sclust
     * (which we set to current_sclust if the parent itself wasn't
     * moved — handled in the effective_sclust helper below).
     * For parent_idx == -1 (root-level), bytes are offset from
     * g_fs.dirbase (fixed root-dir region on FAT16). */

    /* Helper: effective (post-move) sclust for an entry — either its
     * target or (if unmoved or couldn't be placed) its current. */
#define CLD_EFFECTIVE_SCLUST(file) \
    ((file).target_sclust != 0 ? (file).target_sclust : (file).current_sclust)

    for (int f = 0; f < ctx->n_files; f++) {
        cld_file_t *ent = &ctx->files[f];
        DWORD new_sc = CLD_EFFECTIVE_SCLUST(*ent);
        if (new_sc == ent->current_sclust) {
            /* Not moved; nothing to patch for THIS entry's SFN slot.
             * But if we're a subdir whose parent moved, we still need
             * to fix `..` (handled below). */
        } else {
            /* Step (i): patch parent's SFN slot for this child. */
            LBA_t sfn_lba;
            uint32_t sfn_off;
            if (ent->parent_idx == -1) {
                /* Root-level: entry is in the fixed root-dir region. */
                LBA_t root_base = (LBA_t)g_fs.dirbase;
                sfn_lba = root_base + ent->byte_in_parent / 512;
                sfn_off = ent->byte_in_parent % 512;
            } else {
                cld_file_t *par = &ctx->files[ent->parent_idx];
                DWORD par_sclust = CLD_EFFECTIVE_SCLUST(*par);
                DWORD b = ent->byte_in_parent;
                DWORD in_cluster = b / ctx->bpc;
                DWORD in_byte    = b % ctx->bpc;
                DWORD clst       = par_sclust + in_cluster;
                sfn_lba = (LBA_t)g_fs.database + (LBA_t)(clst - 2) * g_fs.csize
                          + in_byte / 512;
                sfn_off = in_byte % 512;
            }
            uint8_t sec[512];
            if (nes_flash_disk_read(sec, (uint32_t)sfn_lba, 1) != 0) return -30;
            sec[sfn_off + 26] = (uint8_t)(new_sc & 0xFF);
            sec[sfn_off + 27] = (uint8_t)((new_sc >> 8) & 0xFF);
            if (nes_flash_disk_write(sec, (uint32_t)sfn_lba, 1) != 0) return -31;
        }
    }
    nes_flash_disk_flush();

    /* Step (ii): subdirectory self-patches (`.` and `..`).
     * Done AFTER all parents' SFN updates so parent.target_sclust
     * is committed. For each subdir, read its new first sector,
     * fix `.` sclust and `..` sclust, write back. */
    for (int f = 0; f < ctx->n_files; f++) {
        cld_file_t *ent = &ctx->files[f];
        if (!ent->is_dir) continue;
        DWORD my_sc = CLD_EFFECTIVE_SCLUST(*ent);
        DWORD parent_sc = 0;
        if (ent->parent_idx >= 0) {
            parent_sc = CLD_EFFECTIVE_SCLUST(ctx->files[ent->parent_idx]);
        }
        /* First sector of the subdir's first data cluster holds
         * both `.` (offset 0) and `..` (offset 32) 32-byte entries. */
        LBA_t first_lba = (LBA_t)g_fs.database + (LBA_t)(my_sc - 2) * g_fs.csize;
        uint8_t sec[512];
        if (nes_flash_disk_read(sec, (uint32_t)first_lba, 1) != 0) return -32;
        /* `.` at offset 26/27 */
        sec[26] = (uint8_t)(my_sc & 0xFF);
        sec[27] = (uint8_t)((my_sc >> 8) & 0xFF);
        /* `..` at offset 32+26, 32+27 */
        sec[32 + 26] = (uint8_t)(parent_sc & 0xFF);
        sec[32 + 27] = (uint8_t)((parent_sc >> 8) & 0xFF);
        if (nes_flash_disk_write(sec, (uint32_t)first_lba, 1) != 0) return -33;
    }
    nes_flash_disk_flush();
#undef CLD_EFFECTIVE_SCLUST

    /* --- Phase 3d: remount so every FatFs cache is rebuilt fresh --- */
    f_unmount("");
    if (f_mount(&g_fs, "", 1) != FR_OK) return -40;

    return moves;
}

int nes_picker_defrag_compact(uint16_t *fb) {
    cld_ctx_t ctx = {0};

    /* Only allocate things we know the size of up front. The per-
     * cluster maps are allocated inside cld_analyze once n_fatent is
     * known so we don't oversize. */
    ctx.files = (cld_file_t *)malloc(CLD_MAX_FILES * sizeof(cld_file_t));
    ctx.buf_a = (uint8_t *)malloc((size_t)g_fs.csize * 512);
    ctx.buf_b = (uint8_t *)malloc((size_t)g_fs.csize * 512);

    int rc = 0;
    if (!ctx.files || !ctx.buf_a || !ctx.buf_b) {
        rc = -2;
        goto cleanup;
    }

    s_defrag_last_error = 0;

    /* Analysis. Encode any failure point so the error splash tells us
     * WHY it failed rather than just "-3":
     *   -1  n_fatent said 0 clusters (volume not mounted?)
     *   -4  cluster count > CLD_ABS_MAX_CLUSTERS (volume too big)
     *   -5  per-cluster map malloc failed (heap exhausted)
     *   -6  dir-walk queue malloc failed
     * The n_clust value is encoded too: returned as -1000 - n_clust
     * when the -4 path fires, so the user sees something like -3400
     * which tells us the volume has 2400 clusters. */
    int ar = cld_analyze(&ctx);
    if (ar != 0) {
        if (ar == -4) {
            rc = -1000 - (int)ctx.n_clust;   /* too-big, shows actual count */
        } else {
            rc = ar * 10;                      /* -10 / -40 / -50 / -60 */
        }
        goto cleanup;
    }

    if (ctx.n_files == 0) {
        /* Nothing to defrag — show an empty summary quickly and return. */
        rc = 0;
        goto cleanup;
    }

    /* Preview + interactive K tuning + confirm. Loops until the user
     * either applies (A) or cancels (B/MENU); LEFT/RIGHT step through
     * s_plan_k_values[] and trigger a full replan so the cluster map
     * and the move/free numbers reflect the new K. */
    int apply = 0;
    int moves_planned = 0, unplaceable = 0;
    for (;;) {
        /* Count moves + unplaceable from the current plan. */
        moves_planned = 0;
        unplaceable = 0;
        for (int f = 0; f < ctx.n_files; f++) {
            if (ctx.files[f].target_sclust == 0) unplaceable++;
        }
        for (DWORD c = 0; c < ctx.n_clust; c++) {
            if (CLD_BIT_GET(ctx.pinned_bits, c)) continue;
            if (ctx.target_owner[c] != 0
                && ctx.target_owner[c] != ctx.current_owner[c]) moves_planned++;
        }

        cld_show_preview(&ctx, fb, moves_planned, unplaceable);
        int action = cld_wait_interaction();
        if (action == CLD_UI_APPLY)  { apply = 1; break; }
        if (action == CLD_UI_CANCEL) { apply = 0; break; }

        /* K adjust — step the index, re-plan, loop back to redraw. */
        int new_idx = s_plan_k_idx + (action == CLD_UI_K_UP ? 1 : -1);
        if (new_idx < 0) new_idx = 0;
        if (new_idx >= CLD_PLAN_K_COUNT) new_idx = CLD_PLAN_K_COUNT - 1;
        if (new_idx != s_plan_k_idx) {
            s_plan_k_idx = new_idx;
            cld_plan_block_search(&ctx);
        }
    }
    if (!apply) {
        rc = 0;
        goto cleanup;
    }

    nes_led_red();
    int moves = cld_execute(&ctx, fb, moves_planned);
    nes_led_off();
    if (moves < 0) {
        s_defrag_last_error = moves;
        rc = moves;
    } else {
        rc = moves;
    }

cleanup:
    free(ctx.files);
    free(ctx.current_owner);
    free(ctx.target_owner);
    free(ctx.pinned_bits);
    free(ctx.done_bits);
    free(ctx.buf_a);
    free(ctx.buf_b);
    return rc;
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

/* Small helper: log2 of a known power-of-two in [1, 1<<20]. */
static uint32_t ilog2_pow2(uint32_t v) {
    uint32_t n = 0;
    while ((v >>= 1) != 0) n++;
    return n;
}

int nes_picker_mmap_rom_chain(const char *name, nes_picker_rom_chain_t *out) {
    if (!out) return -1;
    out->cluster_ptrs = NULL;
    out->size = 0;
    out->n_clusters = 0;

    nes_flash_disk_flush();

    char path[NES_PICKER_PATH_MAX];
    snprintf(path, sizeof(path), ROMS_DIR_SLASH "%s", name);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -2;
    FSIZE_t sz = f_size(&f);
    if (sz < 16 || sz > 8 * 1024 * 1024) { f_close(&f); return -3; }

    DWORD start_cluster = f.obj.sclust;
    f_close(&f);
    if (start_cluster < 2) return -4;

    DWORD bytes_per_cluster = (DWORD)g_fs.csize * 512u;
    DWORD n_clusters = ((DWORD)sz + bytes_per_cluster - 1) / bytes_per_cluster;

    const uint8_t **ptrs = (const uint8_t **)malloc((size_t)n_clusters * sizeof(const uint8_t *));
    if (!ptrs) return -6;

    /* Walk the FAT16 chain, recording each cluster's XIP pointer.
     * Mirrors the sector-level FAT read used by chain_is_contiguous;
     * we need the LBA of every cluster, contiguous or not. */
    DWORD clst = start_cluster;
    DWORD sector_buf_lba = (DWORD)-1;
    static uint8_t fat_sec[512];

    for (DWORD i = 0; i < n_clusters; i++) {
        LBA_t lba = cluster_to_lba(clst);
        uintptr_t xip = (uintptr_t)XIP_BASE + (uintptr_t)FLASH_DISK_OFFSET
                      + (uintptr_t)lba * 512u;
        ptrs[i] = (const uint8_t *)xip;

        if (i == n_clusters - 1) break;

        DWORD entry_byte = clst * 2;   /* FAT16 */
        DWORD entry_sec  = entry_byte / 512;
        DWORD entry_off  = entry_byte % 512;
        LBA_t fat_lba = (LBA_t)g_fs.fatbase + entry_sec;
        if (fat_lba != sector_buf_lba) {
            if (nes_flash_disk_read(fat_sec, (uint32_t)fat_lba, 1) != 0) {
                free(ptrs);
                return -7;
            }
            sector_buf_lba = fat_lba;
        }
        DWORD next = (DWORD)fat_sec[entry_off]
                   | ((DWORD)fat_sec[entry_off + 1] << 8);
        if (next == 0xFFFF || next < 2) {
            /* Chain ended earlier than expected. Treat the short
             * tail as readable (file_size header might have lied). */
            out->n_clusters = i + 1;
            out->cluster_ptrs  = ptrs;
            out->cluster_shift = ilog2_pow2(bytes_per_cluster);
            out->cluster_mask  = bytes_per_cluster - 1u;
            out->size          = (size_t)sz;
            return 0;
        }
        clst = next;
    }

    out->n_clusters    = n_clusters;
    out->cluster_ptrs  = ptrs;
    out->cluster_shift = ilog2_pow2(bytes_per_cluster);
    out->cluster_mask  = bytes_per_cluster - 1u;
    out->size          = (size_t)sz;
    return 0;
}

void nes_picker_mmap_rom_chain_free(nes_picker_rom_chain_t *chain) {
    if (!chain) return;
    free((void *)chain->cluster_ptrs);
    chain->cluster_ptrs = NULL;
    chain->n_clusters   = 0;
    chain->size         = 0;
}

/* Helper for diagnostics: walks the volume iteratively (same BFS as
 * nes_picker_defrag_compact) and counts files where the cluster chain
 * is non-contiguous. Skips dotfiles and sub-cluster files. Returns the
 * count, or negative on error. */
int nes_picker_count_fragmented_named(char *out_first_name, size_t out_sz) {
    if (out_first_name && out_sz > 0) out_first_name[0] = 0;
    /* Use CLD_PATH_MAX (192) rather than NES_PICKER_NAME_MAX (96) so
     * deeply nested paths like /games/<long>/assets/sprites/foo.bmp
     * don't get truncated by snprintf and cause f_opendir to fail —
     * which silently drops entire subtrees from the count. */
    enum { PATH_LEN = CLD_PATH_MAX, MAX_DIRS = 32 };
    char (*queue)[PATH_LEN] = (char(*)[PATH_LEN])malloc((size_t)MAX_DIRS * PATH_LEN);
    if (!queue) return -1;

    int q_head = 0, q_tail = 0, frag = 0;
    strncpy(queue[q_tail], "/", PATH_LEN - 1);
    queue[q_tail][PATH_LEN - 1] = 0;
    q_tail++;

    while (q_head < q_tail) {
        const char *dir_path = queue[q_head++];
        DIR dir;
        FILINFO info;
        if (f_opendir(&dir, dir_path) != FR_OK) continue;
        while (f_readdir(&dir, &info) == FR_OK) {
            if (info.fname[0] == 0) break;
            /* Skip only "." / ".." — dotfiles like /.volume are real
             * files we want to count. */
            if (info.fname[0] == '.'
                && (info.fname[1] == 0
                    || (info.fname[1] == '.' && info.fname[2] == 0))) {
                continue;
            }
            char full[PATH_LEN];
            if (dir_path[0] == '/' && dir_path[1] == 0)
                snprintf(full, sizeof(full), "/%s", info.fname);
            else
                snprintf(full, sizeof(full), "%s/%s", dir_path, info.fname);

            if (info.fattrib & AM_DIR) {
                if (q_tail < MAX_DIRS) {
                    strncpy(queue[q_tail], full, PATH_LEN - 1);
                    queue[q_tail][PATH_LEN - 1] = 0;
                    q_tail++;
                }
                continue;
            }
            if (info.fsize < 4096) continue;

            /* Use the same chain_is_contiguous check the preview uses
             * so before/after numbers are produced by identical logic
             * — eliminates the "preview said 0 but summary said 15"
             * discrepancy we were seeing when the two checks used
             * different implementations. */
            FIL f;
            if (f_open(&f, full, FA_READ) != FR_OK) continue;
            DWORD sc = f.obj.sclust;
            FSIZE_t sz = f_size(&f);
            f_close(&f);
            if (sz == 0 || sc < 2) continue;
            DWORD bpc = (DWORD)g_fs.csize * 512u;
            DWORD nc  = ((DWORD)sz + bpc - 1) / bpc;
            if (!chain_is_contiguous(sc, nc)) {
                if (frag == 0 && out_first_name && out_sz > 0) {
                    const char *tail = full;
                    size_t len = strlen(full);
                    if (len > 16) tail = full + len - 16;
                    snprintf(out_first_name, out_sz, "%s s%lu n%lu",
                             tail, (unsigned long)sc, (unsigned long)nc);
                }
                frag++;
            }
        }
        f_closedir(&dir);
    }

    free(queue);
    return frag;
}

int nes_picker_count_fragmented(void) {
    return nes_picker_count_fragmented_named(NULL, 0);
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
                || strcasecmp(dst + L - 4, ".sms") == 0
                || strcasecmp(dst + L - 4, ".gbc") == 0)) {
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
    return m == 125 || m == 150 || m == 200 || m == 250 || m == 300;
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
                        : (v_clock_mhz == 200) ? 2
                        : (v_clock_mhz == 250) ? 3
                        : (v_clock_mhz == 300) ? 4 : 3;

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

#if defined(THUMBYONE_SLOT_MODE) && defined(THUMBYONE_BATT_DEBUG)
            /* Diagnostic readout — shows the internal numbers feeding
             * the percent calculation so we can confirm whether the
             * "45 % / 3.29 V" report reflects a real low-voltage cell
             * or a firmware/ADC calibration bug. Remove once the
             * calibration question is resolved. */
            int   dbg_raw   = 0;
            float dbg_hfresh = 0.0f;
            float dbg_hema   = 0.0f;
            thumbyone_battery_read_debug(&dbg_raw, &dbg_hfresh, &dbg_hema);
            char battery_dbg[28];
            int hf_int = (int)dbg_hfresh;
            int hf_dec = (int)((dbg_hfresh - hf_int) * 1000.0f + 0.5f);
            snprintf(battery_dbg, sizeof(battery_dbg),
                      "r%d h%d.%03d", dbg_raw, hf_int, hf_dec);
#endif
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
            snprintf(about_text, sizeof(about_text), "ThumbyNES v1.04");

            static const char * const view_choices[]  = { "HERO", "LIST" };
            static const char * const sort_choices[]  = { "ALPHA", "FAVS", "SIZE" };
            static const char * const clock_choices[] = { "125MHz", "150MHz", "200MHz", "250MHz", "300MHz" };
            static const int          clock_mhz[]     = {  125,      150,      200,      250,      300 };

            /* ACT_LOBBY is only offered when compiled into ThumbyOne
             * (standalone NES has nothing to fall back to). The
             * menu_item enum stays stable otherwise so the picker
             * state machine below doesn't care about conditional
             * item ordering. */
            enum { ACT_NONE, ACT_DEFRAG, ACT_LOBBY };

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
                  .value_ptr = &v_clock, .choices = clock_choices, .num_choices = 5,
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
#if defined(THUMBYONE_SLOT_MODE) && defined(THUMBYONE_BATT_DEBUG)
                /* Diagnostic row — r=raw ADC counts, h=fresh half
                 * voltage in volts. True pack voltage = 2 × h
                 * (assuming the 1:2 divider is correctly wired).
                 * If a fully-charged device reads r≈2600 / h≈2.10
                 * the ADC path is healthy; lower values point at a
                 * calibration issue. */
                { .kind = NES_MENU_KIND_INFO, .label = "BatDbg",
                  .info_text = battery_dbg,
                  .enabled = true },
#endif
                { .kind = NES_MENU_KIND_INFO, .label = "Storage",
                  .info_text = storage_text,
                  .value_ptr = &v_storage_used_kb, .min = 0,
                  .max = v_storage_total_kb > 0 ? v_storage_total_kb : 1,
                  .enabled = true },
                { .kind = NES_MENU_KIND_ACTION, .label = "Defragment now",
                  .enabled = true, .action_id = ACT_DEFRAG },
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

            if (r.kind == NES_MENU_ACTION && r.action_id == ACT_DEFRAG) {
                /* Manual whole-volume compact. Reports both the
                 * number of files rewritten and how many are still
                 * fragmented after. If the latter > 0 despite a
                 * successful pass, either the compact algorithm or
                 * chain_is_contiguous has a bug.
                 *
                 * Show a dedicated summary splash the user dismisses
                 * with any button — a brief OSD gets missed on long
                 * runs where the eye is tracking the red overlay. */
                char stuck[CLD_PATH_MAX] = {0};
                int before = nes_picker_count_fragmented_named(NULL, 0);
                int rc     = nes_picker_defrag_compact(fb);
                int after  = nes_picker_count_fragmented_named(stuck, sizeof(stuck));
                int last_err = nes_picker_defrag_last_error();

                fb_clear(fb, COL_BG);
                nes_font_draw(fb, "DEFRAG DONE", 28, 22, COL_TITLE);
                char l1[32], l2[32], l3[32], l4[40];
                if (rc >= 0) {
                    snprintf(l1, sizeof(l1), "%d rewrote", rc);
                    snprintf(l2, sizeof(l2), "frag: %d -> %d", before, after);
                    const char *verdict =
                        (after == 0)        ? "all contiguous"
                      : (after < before)    ? "partial progress"
                      : (before == 0)       ? "nothing to do"
                                            : "algo stuck!";
                    snprintf(l3, sizeof(l3), "%s", verdict);
                    if (after > 0) {
                        snprintf(l4, sizeof(l4), "err code: %d", last_err);
                    } else {
                        l4[0] = 0;
                    }
                } else {
                    snprintf(l1, sizeof(l1), "error %d", rc);
                    snprintf(l2, sizeof(l2), "pass aborted");
                    snprintf(l3, sizeof(l3), " ");
                    l4[0] = 0;
                }
                nes_font_draw(fb, l1, (FB_W - nes_font_width(l1)) / 2, 40, COL_FG);
                nes_font_draw(fb, l2, (FB_W - nes_font_width(l2)) / 2, 52, COL_FG);
                nes_font_draw(fb, l3, (FB_W - nes_font_width(l3)) / 2, 64,
                              (after == 0) ? COL_HIGHLT : COL_ERR);
                if (l4[0]) {
                    nes_font_draw(fb, l4,
                                   (FB_W - nes_font_width(l4)) / 2, 80,
                                   COL_ERR);
                }
                /* Show the first path count_fragmented_named flagged —
                 * tells us whether the residual 15 are real fragments
                 * in specific files or phantoms from a buggy check.
                 * stuck now holds "<tail> s<sclust> n<nclusters>". */
                if (after > 0 && stuck[0]) {
                    nes_font_draw(fb, stuck,
                                   (FB_W - nes_font_width(stuck)) / 2, 92,
                                   COL_DIM);
                }
                nes_font_draw(fb, "press any button",
                              (FB_W - nes_font_width("press any button")) / 2,
                              108, COL_DIM);
                nes_lcd_wait_idle();
                nes_lcd_present(fb);

                /* Drain current button state so "Resume"'s A doesn't
                 * immediately satisfy the wait. nes_buttons_read covers
                 * dpad + A + B; nes_buttons_menu_pressed covers MENU;
                 * shoulders via direct GPIO. */
                while (nes_buttons_read() != 0
                       || nes_buttons_menu_pressed()
                       || !gpio_get(BTN_LB_GP)
                       || !gpio_get(BTN_RB_GP)) {
                    tud_task();
                    sleep_ms(10);
                }
                /* Now wait for a fresh press. */
                while (nes_buttons_read() == 0
                       && !nes_buttons_menu_pressed()
                       && gpio_get(BTN_LB_GP)
                       && gpio_get(BTN_RB_GP)) {
                    tud_task();
                    sleep_ms(10);
                }
                prev = nes_buttons_read();   /* so we don't treat it as a picker press */
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
