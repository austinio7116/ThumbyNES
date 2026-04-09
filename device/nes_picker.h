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
