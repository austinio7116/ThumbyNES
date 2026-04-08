/*
 * ThumbyNES — device-side SMS / Game Gear runner.
 *
 * Sibling of nes_run.h. Same shape: pick a ROM, drive it through
 * smsplus, render into the 128x128 LCD framebuffer, return when
 * the user holds MENU.
 */
#ifndef THUMBYNES_SMS_RUN_H
#define THUMBYNES_SMS_RUN_H

#include <stdint.h>

#include "nes_picker.h"

int sms_run_rom(const nes_rom_entry *e, uint16_t *fb);

#endif
