/*
 * ThumbyNES — PCE dynamic IRAM (device-only).
 *
 * Hot HuExpress functions live in a dedicated `.pce_iram_pool` flash
 * section (see device/md_memmap.ld + per-function IRAM_ATTR tags in
 * the vendored engine). At pcec_init time we malloc a heap buffer
 * sized to the pool, memcpy the whole block into it, and repoint the
 * --wrap thunks at the heap copy. On shutdown the heap is freed.
 * Result: PCE pays its ~28 KB IRAM cost only while the user is in
 * the PCE slot — other emulators get the heap back.
 *
 * The pool contains:
 *   - exe_go (~23 KB) — the HuC6280 dispatch/opcode body
 *   - IO_read_, IO_read_raw, IO_write_ — called from every load/store opcode
 *   - adc, sbc — math helpers called by 23 ADC/SBC opcodes
 *   - WriteBuffer — PSG audio synth, called per pcec_audio_pull
 *   - pce_render_scanline — scanline composer, 240 calls/frame from
 *     the gfx_Loop6502 macro inlined into exe_go
 *   - RefreshScreen / CheckSprites / change_pce_screen_height /
 *     wait_next_vsync / osd_keyboard — once-per-frame helpers exe_go
 *     reaches via PC-relative BL, so they have to be in the pool too
 *
 * Same lifecycle as md_iram.c.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern char __pce_iram_pool_start[];
extern char __pce_iram_pool_end[];

static void *s_ram_copy;
static size_t s_pool_size;

/* --wrap'd entry points dispatch via these pointers. */
extern void  __real_exe_go(void);
extern int   __real_pcec_audio_pull(int16_t *out, int n);

/* exe_go has no return value; signature matches HuExpress. */
typedef void (*exe_go_fn)(void);
static exe_go_fn s_exe_go = (exe_go_fn)__real_exe_go;

void __wrap_exe_go(void) {
    s_exe_go();
}

/* Translate a flash address inside the pool to its heap-copy address. */
static void *iram_remap(void *flash_fn) {
    uintptr_t flash_addr = ((uintptr_t)flash_fn) & ~1u;
    uintptr_t offset     = flash_addr - (uintptr_t)__pce_iram_pool_start;
    uintptr_t heap_addr  = (uintptr_t)s_ram_copy + offset;
    /* Restore the Thumb bit so the indirect call dispatches correctly. */
    return (void *)(heap_addr | 1u);
}

int pce_iram_init(void) {
    /* THUMBYNES PATCH: relocation is currently DISABLED.
     *
     * The pool members (exe_go / IO_read_ / IO_write_ / pce_render_scanline
     * etc.) make PC-relative BL calls to libc helpers (memset, memcpy,
     * printf, puts, abort) and to a few HuExpress error-path helpers
     * (Log) that are NOT in `.pce_iram_pool`. After memcpy'ing the pool
     * to heap (~256 MB further away from flash), those BL offsets target
     * garbage SRAM addresses and the emulator crashes / hangs on the
     * first scanline / first VDC write.
     *
     * Running exe_go from flash via XIP costs us ~10-30% PCE performance
     * but is correct. The wrap thunk still dispatches via s_exe_go, which
     * remains pointing at __real_exe_go (the original flash address), so
     * --wrap=exe_go is a no-op functionally.
     *
     * To re-enable later: provide pool-resident wrappers for memset,
     * memcpy, and the error-path libc/Log calls, OR move libc into the
     * pool via linker section directives, OR replace the stragglers
     * with always_inline byte-loop equivalents. */
    (void)__pce_iram_pool_start;
    (void)__pce_iram_pool_end;
    (void)iram_remap;
    return 0;
}

void pce_iram_shutdown(void) {
    if (!s_ram_copy) return;
    s_exe_go = (exe_go_fn)__real_exe_go;
    /* Make sure no caller is still mid-flight before we free. */
    __asm__ volatile ("dmb" ::: "memory");
    free(s_ram_copy);
    s_ram_copy  = NULL;
    s_pool_size = 0;
}

size_t pce_iram_pool_size(void) { return s_pool_size; }
int    pce_iram_is_active(void) { return s_ram_copy != NULL; }
