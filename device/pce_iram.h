/*
 * ThumbyNES — PCE dynamic IRAM (device-only).
 *
 * Memcpy the .pce_iram_pool flash section into a heap buffer at
 * pcec_init and free it on shutdown so PCE's hot-function RAM cost
 * is only paid while the user is in the PCE slot. See pce_iram.c.
 */
#pragma once

#include <stddef.h>

int    pce_iram_init(void);          /* 0 = ok, <0 = failure */
void   pce_iram_shutdown(void);
size_t pce_iram_pool_size(void);
int    pce_iram_is_active(void);
