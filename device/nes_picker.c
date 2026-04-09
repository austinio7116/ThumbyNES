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

#define BTN_LB_GP 6   /* mirror nes_buttons.c — used for tab nav   */
#define BTN_RB_GP 22

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

/* --- shared row helpers -------------------------------------------- */

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

/* Format the per-ROM meta line shared by hero / list views. */
static void format_meta(char *out, size_t outsz, const nes_rom_entry *e) {
    const char *region = e->pal_hint ? "PAL" : "NTSC";
    if      (e->system == ROM_SYS_SMS) snprintf(out, outsz, "SMS  %luK  %s",
                                                 (unsigned long)(e->size / 1024), region);
    else if (e->system == ROM_SYS_GG ) snprintf(out, outsz, "GG   %luK  %s",
                                                 (unsigned long)(e->size / 1024), region);
    else if (e->mapper == 0xFF)        snprintf(out, outsz, "??   %luK  %s",
                                                 (unsigned long)(e->size / 1024), region);
    else                               snprintf(out, outsz, "m%d   %luK  %s",
                                                 (int)e->mapper,
                                                 (unsigned long)(e->size / 1024), region);
}

/* --- view persistence ---------------------------------------------- */

#define VIEW_PATH "/.picker_view"
#define VIEW_HERO 0
#define VIEW_LIST 1

#define TAB_FAV 0
#define TAB_NES 1
#define TAB_SMS 2
#define TAB_GG  3
#define TAB_COUNT 4

#define SORT_ALPHA 0   /* case-insensitive name */
#define SORT_FAV   1   /* favorites first, then alpha */
#define SORT_SIZE  2   /* largest first */
#define SORT_COUNT 3

typedef struct {
    uint8_t view;   /* VIEW_HERO / VIEW_LIST */
    uint8_t tab;    /* TAB_*                  */
    uint8_t sort;   /* SORT_*                 */
    uint8_t _pad;
} picker_pref_t;

static void pref_load(picker_pref_t *p) {
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
        else if (e[i].system == ROM_SYS_GG ) counts[TAB_GG ]++;
    }
}

/* --- tab strip ----------------------------------------------------- */

#define TAB_BAR_H 11

static void draw_tab_bar(uint16_t *fb, int active_tab, const int counts[TAB_COUNT]) {
    static const uint8_t icon_for[TAB_COUNT] = {
        ICON_SYS_STAR, ICON_SYS_NES, ICON_SYS_SMS, ICON_SYS_GG,
    };
    int cell_w = FB_W / TAB_COUNT;     /* 32 */
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
        nes_font_draw(fb, lab, x + cell_w - lw - 2, 3, hl ? COL_FG : COL_DIM);
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
     * it horizontally; otherwise centre it. */
    char nm[64];
    name_no_ext(nm, sizeof(nm), e->name);
    int title_y = thumb_y + 64 + 4;
    int tw = nes_font_width_2x(nm);
    if (tw <= FB_W - 4) {
        nes_font_draw_2x(fb, nm, (FB_W - tw) / 2, title_y, COL_FG);
    } else {
        /* Marquee: render the title at a negative x offset, then a
         * second copy after a gap so the loop is seamless. put()
         * inside the font renderer clips against fb edges, so any
         * glyph cells drawn off-screen on either side are dropped. */
        const int gap = 24;
        int loop = tw + gap;
        int x0 = 2 - (marquee_offset % loop);
        nes_font_draw_2x(fb, nm, x0,        title_y, COL_FG);
        nes_font_draw_2x(fb, nm, x0 + loop, title_y, COL_FG);
    }

    /* Meta line in small font under the title. */
    char meta[32];
    format_meta(meta, sizeof(meta), e);
    int mw = nes_font_width(meta);
    nes_font_draw(fb, meta, (FB_W - mw) / 2, title_y + 13, COL_DIM);

    /* Favorite star + position counter on the bottom row. */
    char foot[24];
    int is_fav = nes_picker_is_favorite(e->name);
    snprintf(foot, sizeof(foot), "%c %d/%d", is_fav ? '*' : ' ',
              sel + 1, n_view);
    int fw = nes_font_width(foot);
    nes_font_draw(fb, foot, (FB_W - fw) / 2, FB_H - 8, is_fav ? COL_TITLE : COL_DIM);

    /* Up / down arrow hints (matches new control scheme). */
    if (sel > 0)          nes_font_draw(fb, "^", 2,        FB_H - 8, COL_DIM);
    if (sel < n_view - 1) nes_font_draw(fb, "v", FB_W - 6, FB_H - 8, COL_DIM);

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

        /* Name + meta to the right of the thumbnail. */
        char nm[24];
        name_no_ext(nm, sizeof(nm), e[idx].name);
        if (strlen(nm) > 22) nm[22] = 0;
        uint16_t fg = hl ? COL_HIGHLT : COL_FG;
        int is_fav = nes_picker_is_favorite(e[idx].name);
        char row[26];
        snprintf(row, sizeof(row), "%c%s", is_fav ? '*' : ' ', nm);
        nes_font_draw(fb, row, 36, y + 4, fg);

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

int nes_picker_run(uint16_t *fb,
                    const nes_rom_entry *entries, int n_entries) {
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
    int sel = 0, top = 0;

    uint8_t prev = 0;
    int marquee = 0;          /* horizontal scroll offset for hero title */
    int last_sel = -1;        /* reset marquee on selection change       */

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

        /* ----- D-pad: spatial navigation ----- */
        /* LEFT / RIGHT = prev / next tab (skip empty tabs).          */
        int tab_dir = 0;
        if (pressed & 0x01) tab_dir = -1;
        if (pressed & 0x02) tab_dir = +1;
        if (tab_dir) {
            int prev_real = (n_view > 0) ? view[sel] : -1;
            for (int tries = 0; tries < TAB_COUNT; tries++) {
                pref.tab = (pref.tab + TAB_COUNT + tab_dir) % TAB_COUNT;
                if (counts[pref.tab] > 0) break;
            }
            n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
            sel = reseat_sel(view, n_view, prev_real);
            top = (sel >= LIST_ROWS) ? sel - LIST_ROWS + 1 : 0;
        }
        /* UP / DOWN = prev / next ROM (in either view). */
        if ((pressed & 0x04) && sel > 0) {
            sel--;
            if (sel < top) top = sel;
        }
        if ((pressed & 0x08) && sel < n_view - 1) {
            sel++;
            if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
        }

        /* ----- shoulder buttons: sort + view ----- */
        if (lb_edge) {
            pref.sort = (pref.sort + 1) % SORT_COUNT;
            int prev_real = (n_view > 0) ? view[sel] : -1;
            n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
            sel = reseat_sel(view, n_view, prev_real);
            top = (sel >= LIST_ROWS) ? sel - LIST_ROWS + 1 : 0;
            snprintf(osd, sizeof(osd), "sort: %s", sort_label(pref.sort));
            osd_ms = 900;
        }
        if (rb_edge) {
            pref.view = (pref.view == VIEW_HERO) ? VIEW_LIST : VIEW_HERO;
            top = (sel >= LIST_ROWS) ? sel - LIST_ROWS + 1 : 0;
            snprintf(osd, sizeof(osd),
                      pref.view == VIEW_HERO ? "view: hero" : "view: list");
            osd_ms = 900;
        }

        /* ----- face buttons ----- */
        /* B (PICO-8 O = bit 4) = toggle favorite. */
        if ((pressed & 0x10) && n_view > 0) {
            int prev_real = view[sel];
            favs_toggle(entries[prev_real].name);
            tab_counts(entries, n_entries, counts);
            n_view = build_view(entries, n_entries, view, pref.tab, pref.sort);
            sel = reseat_sel(view, n_view, prev_real);
            if (sel < top) top = sel;
            if (top + LIST_ROWS <= sel) top = sel - LIST_ROWS + 1;
            if (top < 0) top = 0;
        }
        /* A (PICO-8 X = bit 5) = launch. */
        if ((pressed & 0x20) && n_view > 0) {
            favs_save();
            pref_save(&pref);
            return view[sel];
        }

        if (nes_buttons_menu_pressed()) {
            favs_save();
            pref_save(&pref);
            return -1;
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
