/*
 * ThumbyNES — MD dynamic IRAM (device-only).
 *
 * Hot MD functions live in a dedicated .md_iram_pool flash section
 * (see device/md_memmap.ld + per-function section attributes in the
 * vendored picodrive source). At mdc_init we malloc a heap buffer
 * sized to the pool, memcpy the whole block, and repoint the --wrap
 * thunks at the heap copy. Zero BSS cost across sibling cores.
 *
 * Current pool: Cz80_Exec (17 KB, leaf). Tried extending to the
 * YM2612 group (UpdateOne_ + chan_render + update_lfo_phase +
 * memset32, ~11 KB) but that caused a consistent +8 ms/frame
 * regression (solid k=25 full-frameskip, versus the baseline of
 * k=0-25 catching up only in heavy scenes). Hypothesis unproven
 * but likely one of: heap cache-line thrash against Cz80's 17 KB,
 * unaligned 32-bit accesses in chan_render's fast ops inner loop,
 * or an interaction with PsndFMUpdate's function-pointer retarget
 * that I haven't audited. Rolled back; --wrap=YM2612UpdateOne_ is
 * no longer linked, and the section attributes are off.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern char __md_iram_pool_start[];
extern char __md_iram_pool_end[];

static void *s_ram_copy;
static size_t s_pool_size;

extern int __real_Cz80_Exec(void *cpu, int cycles);

typedef int (*cz80_exec_fn)(void *cpu, int cycles);
static cz80_exec_fn s_cz80_exec = (cz80_exec_fn)__real_Cz80_Exec;

int __wrap_Cz80_Exec(void *cpu, int cycles) {
    return s_cz80_exec(cpu, cycles);
}

static void *iram_remap(void *flash_fn) {
    uintptr_t flash_addr = ((uintptr_t)flash_fn) & ~1u;
    uintptr_t offset     = flash_addr - (uintptr_t)__md_iram_pool_start;
    uintptr_t heap_addr  = (uintptr_t)s_ram_copy + offset;
    return (void *)(heap_addr | 1u);
}

int md_iram_init(void) {
    if (s_ram_copy) return 0;
    s_pool_size = (size_t)(__md_iram_pool_end - __md_iram_pool_start);
    if (s_pool_size == 0) return -1;
    s_ram_copy = malloc(s_pool_size);
    if (!s_ram_copy) return -2;
    memcpy(s_ram_copy, __md_iram_pool_start, s_pool_size);
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    s_cz80_exec = (cz80_exec_fn)iram_remap((void *)__real_Cz80_Exec);
    return 0;
}

void md_iram_shutdown(void) {
    if (!s_ram_copy) return;
    s_cz80_exec = (cz80_exec_fn)__real_Cz80_Exec;
    __asm__ volatile ("dmb" ::: "memory");
    free(s_ram_copy);
    s_ram_copy  = NULL;
    s_pool_size = 0;
}

size_t md_iram_pool_size(void) { return s_pool_size; }
int    md_iram_is_active(void) { return s_ram_copy != NULL; }
