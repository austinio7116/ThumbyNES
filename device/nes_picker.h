/*
 * ThumbyNES — ROM picker.
 *
 * Scans the FAT volume for *.nes files in the root directory and
 * presents a simple scrollable list. Returns the selected file
 * name or -1 if cancelled. Phase 3: list-only, no thumbnails.
 */
#ifndef THUMBYNES_PICKER_H
#define THUMBYNES_PICKER_H

#include <stddef.h>
#include <stdint.h>

/* Root directory for all ThumbyNES content. Under THUMBYONE_SLOT_MODE
 * everything lives in /roms/ (ROMs and per-ROM sidecars, plus the
 * picker's own /.favs + /.global state) so the shared FAT stays tidy
 * and each slot owns a single folder. Standalone builds keep the
 * files at the root. Shared with nes_run.c / gb_run.c / sms_run.c /
 * nes_thumb.c so every write path agrees on the layout. */
#ifdef THUMBYONE_SLOT_MODE
#define ROMS_DIR       "/roms"
#define ROMS_DIR_SLASH "/roms/"
#else
#define ROMS_DIR       ""
#define ROMS_DIR_SLASH "/"
#endif

#define NES_PICKER_MAX_ROMS  64
/* Max file name length stored in nes_rom_entry. Real ROM dumps with
 * full no-intro / GoodNES tags push 60+ chars (e.g. "Super Mario
 * Land 2 - 6 Golden Coins (USA, Europe).gb" is 52). 96 covers every
 * realistic name without burning much SRAM. The dependent path[]
 * buffers in the runners are sized as NES_PICKER_NAME_MAX + 16. */
#define NES_PICKER_NAME_MAX  96
#define NES_PICKER_PATH_MAX  (NES_PICKER_NAME_MAX + 16)

/* Which emulator core handles this ROM. Detected from file extension. */
#define ROM_SYS_NES  0
#define ROM_SYS_SMS  1
#define ROM_SYS_GG   2
#define ROM_SYS_GB   3
#define ROM_SYS_MD   4   /* Sega Mega Drive / Genesis — .md/.bin/.gen */

typedef struct {
    char     name[NES_PICKER_NAME_MAX];   /* base file name in / */
    uint32_t size;
    uint8_t  mapper;                      /* iNES mapper number, 0xFF = unknown / N/A */
    uint8_t  pal_hint;                    /* 0 = NTSC default, 1 = PAL detected */
    uint8_t  system;                      /* ROM_SYS_* */
    uint8_t  _pad;
} nes_rom_entry;

/* Scan / for *.nes files. Returns count placed in `out`. */
int nes_picker_scan(nes_rom_entry *out, int max);

/* Run the picker UI against `fb` (128×128 RGB565). Reads buttons,
 * presents to the LCD each frame. Returns the index of the chosen
 * ROM in `entries[]` when the user launches a cart.
 *
 * `entries` is mutable and `n_entries` is in/out — the picker
 * re-scans the FAT volume into the buffer whenever it detects USB
 * MSC activity has gone quiet, so files added or deleted via USB
 * appear/disappear without having to power-cycle. The caller's
 * `*n_entries` is updated to reflect the new total.
 *
 * If `*n_entries == 0` on entry the picker shows a "no ROMs" splash
 * and stays in that state until a ROM appears (then returns 0). The
 * caller is expected to keep pumping USB MSC tasks. */
int nes_picker_run(uint16_t *fb,
                    nes_rom_entry *entries, int *n_entries);

/* Slurp a ROM file into a malloc'd buffer. Caller frees.
 * Use only for small (< ~300 KB) files — see nes_picker_mmap_rom
 * for the zero-copy path used by the ROM runner. */
uint8_t *nes_picker_load_rom(const char *name, size_t *out_len);

/* Map a ROM file directly from XIP flash without copying it into
 * RAM. Returns 0 on success and writes a pointer into flash + the
 * file size. The pointer remains valid until the file is deleted
 * or the FAT volume is reformatted. Returns nonzero if the file
 * isn't contiguous on disk (in which case the caller can fall
 * back to nes_picker_load_rom). */
int nes_picker_mmap_rom(const char *name,
                          const uint8_t **out_data, size_t *out_len);

/* Defragmenter — rewrites every fragmented file in / so its cluster
 * chain becomes contiguous, which lets the XIP mmap path serve large
 * carts that would otherwise fall back to malloc and OOM. Walks the
 * root, checks each file with the same chain_is_contiguous logic the
 * mmap path uses, and rewrites the offenders via the f_expand
 * temp-file dance. Progress is drawn into `fb` so the user sees what
 * the picker is doing.
 *
 * Returns the number of files rewritten (>= 0) or a negative error
 * code if the pass aborted. The function pumps tud_task() between
 * files so USB stays alive while it runs. */
int nes_picker_defrag(uint16_t *fb);

/* Defragment one named ROM in /roms/ — rewrites the file so its
 * cluster chain is contiguous. Used by the runner loaders when
 * nes_picker_mmap_rom returns -5 (fragmented) and the cart is too
 * large to fall back to a RAM load. Draws a progress splash into
 * `fb` so the user sees the pause. Returns 0 on success. */
int nes_picker_defrag_one(const char *name, uint16_t *fb);

/* Compacting pass — rewrites every non-trivial file in /roms/,
 * including files that are already individually contiguous. That's
 * what actually consolidates free space into one big run at the
 * end of the volume, which is what a new upload (or a fresh big
 * ROM's f_expand) needs. This is the one the runners fall back to
 * when a targeted rewrite can't land a 1 MB ROM because scattered
 * already-contiguous files are blocking consolidation.
 *
 * Loud overlay: full-screen "DO NOT POWER OFF" message plus red
 * front indicator for the duration of the pass. Returns the number
 * of files rewritten (>= 0) or a negative error. */
int nes_picker_defrag_compact(uint16_t *fb);

/* Error code of the first defrag_one_path failure during the most
 * recent nes_picker_defrag_compact pass. 0 = every file succeeded.
 * Use it to differentiate failure modes in the summary UI. */
int nes_picker_defrag_last_error(void);

/* Chained-XIP mmap for fragmented carts. Builds a per-cluster table
 * of XIP pointers so a fragmented file can still be read straight
 * out of flash without copying anything — reads walk the LBA table
 * instead of assuming the cluster chain is contiguous.
 *
 * Use when the contiguous path (nes_picker_mmap_rom) returns -5 and
 * a compacting defrag can't fix it (volume too full to hold a
 * rewrite copy).
 *
 * On success:
 *   out->cluster_ptrs[i] = XIP pointer to cluster i's first byte
 *   out->cluster_shift   = log2(bytes_per_cluster) (typically 12)
 *   out->cluster_mask    = bytes_per_cluster - 1  (typically 0xFFF)
 *   out->n_clusters      = number of clusters covering the file
 *   out->size            = total file size in bytes
 *
 * The caller owns cluster_ptrs and must free() it when the cart is
 * unloaded (wrappers usually call nes_picker_mmap_rom_chain_free).
 * Returns 0 on success; negative on error (same -2/-3/-4 classes as
 * nes_picker_mmap_rom). */
typedef struct {
    const uint8_t **cluster_ptrs;
    uint32_t        cluster_shift;
    uint32_t        cluster_mask;
    uint32_t        n_clusters;
    size_t          size;
} nes_picker_rom_chain_t;

int  nes_picker_mmap_rom_chain(const char *name, nes_picker_rom_chain_t *out);
void nes_picker_mmap_rom_chain_free(nes_picker_rom_chain_t *chain);

/* Walk the whole shared FAT (same traversal as defrag_compact) and
 * count files whose cluster chain isn't contiguous. Useful as a
 * post-compact verification: if the pass rewrote N files but this
 * still returns > 0, the compact algorithm itself (or our
 * chain_is_contiguous probe) is broken. */
int  nes_picker_count_fragmented(void);

/* Same as nes_picker_count_fragmented but also writes the absolute
 * path of the FIRST fragmented file encountered into the caller's
 * buffer. If none are fragmented, out_first_name[0] is set to 0.
 * Useful for debugging a stuck rewrite. */
int  nes_picker_count_fragmented_named(char *out_first_name, size_t out_sz);

/* Global preferences — applies across every cart. Lives in /.global
 * on the FAT volume. The picker menu and the in-game menus both
 * adjust the same values; per-cart .cfg sidecars no longer carry
 * volume.
 *
 * Overclock choices are MHz values: 125 / 150 / 200 / 250. The
 * default is 250 (the same fixed clock the firmware booted with
 * before this option existed). Setting takes effect on the next
 * ROM launch — nes_device_main re-applies the saved value before
 * each runner starts.  */
int  nes_picker_global_volume(void);
void nes_picker_global_set_volume(int v);

int  nes_picker_global_clock_mhz(void);
void nes_picker_global_set_clock_mhz(int mhz);

#endif
