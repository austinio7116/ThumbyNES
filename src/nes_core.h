/*
 * ThumbyNES — thin wrapper around the vendored Nofrendo NES core.
 *
 * The wrapper exposes the only five operations the host runner and the
 * device firmware actually need. Everything else (palette, scaling,
 * audio resampling, controller mapping) lives one layer above this.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NESC_SCREEN_W  256
#define NESC_SCREEN_H  240

/* NES controller bits — match the standard nofrendo input layout. */
#define NESC_BTN_A       0x01
#define NESC_BTN_B       0x02
#define NESC_BTN_SELECT  0x04
#define NESC_BTN_START   0x08
#define NESC_BTN_UP      0x10
#define NESC_BTN_DOWN    0x20
#define NESC_BTN_LEFT    0x40
#define NESC_BTN_RIGHT   0x80

/* Initialize the NES core. sample_rate is the APU output rate (Hz),
 * typically 22050 on device, 44100 on host. Returns 0 on success. */
int  nesc_init(int sample_rate);

/* Load an iNES ROM image already resident in memory. The buffer must
 * remain valid for the lifetime of the emulation session (we point
 * PRG/CHR banks at it, no copy). Returns 0 on success. */
int  nesc_load_rom(const uint8_t *data, size_t len);

/* Run the emulator for exactly one NTSC frame (262 scanlines). After
 * this returns, nesc_framebuffer() points at a freshly rendered frame. */
void nesc_run_frame(void);

/* Pointer to the most recently rendered frame, as 8-bit palette indices.
 * Pitch is NESC_SCREEN_PITCH (= 256 + 16 overdraw — see nofrendo). */
const uint8_t *nesc_framebuffer(void);
int            nesc_framebuffer_pitch(void);

/* RGB565 palette (256 entries). Built once at init time. */
const uint16_t *nesc_palette_rgb565(void);

/* Set the controller bitmask for player 1. */
void nesc_set_buttons(uint8_t mask);

/* Pull up to `n` 16-bit signed mono samples from the APU into `out`.
 * Returns the number of samples actually written. */
int  nesc_audio_pull(int16_t *out, int n);

/* Battery-backed PRG-RAM access. Returns NULL / 0 if the loaded
 * ROM doesn't declare a battery. The pointer is owned by Nofrendo
 * and remains valid for the lifetime of the loaded cart. */
uint8_t *nesc_battery_ram   (void);
size_t   nesc_battery_size  (void);

/* Tear down. */
void nesc_shutdown(void);
