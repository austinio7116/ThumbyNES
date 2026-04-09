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
#define NES_PICKER_NAME_MAX  48

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
 * ROM in `entries[]`, or -1 if MENU was pressed (back/cancel).
 *
 * If `n_entries == 0` shows a "no ROMs — drag .nes via USB" splash
 * and stays in that state until a ROM appears, returning 0 once it
 * does. The caller is expected to keep pumping USB MSC tasks. */
int nes_picker_run(uint16_t *fb,
                    const nes_rom_entry *entries, int n_entries);

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

#endif
