/*
 * ThumbyNES — small CRC32 helper for cart-battery autosave gating.
 *
 * Each runner's autosave fires every 30 s if the cart was running,
 * but most carts don't actually touch their battery RAM that often
 * — title screens, menus, and ordinary gameplay leave the SRAM
 * untouched. battery_save() can take 50-200 ms inside f_write
 * (FAT sector erase + program), which blocks the PWM audio IRQ for
 * the duration and cuts the audio. CRC the RAM, compare against the
 * last-saved CRC, skip the write if unchanged. Single-call cost on a
 * 64 KB Sonic 3 SRAM is ~3 ms at 250 MHz — small enough to slip
 * inside one audio buffer fill (PWM ring is ~186 ms at 22050 Hz)
 * and far cheaper than the flash write it dodges.
 *
 * Reflected polynomial 0xEDB88320 (the standard zlib / PNG CRC32).
 * Table-free: 5 KB of .rodata across 5 runners isn't worth saving
 * the cycles back — autosave runs once every 30 s.
 */
#ifndef THUMBYNES_CRC32_H
#define THUMBYNES_CRC32_H

#include <stddef.h>
#include <stdint.h>

static inline uint32_t nes_crc32(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

#endif
