/*
 * ThumbyNES minimal utils.h.
 *
 * The upstream utils.h pulls in WIN32, POSIX shmem, and the htons
 * fallback. We strip it to the declarations the vendored engine/*.c
 * files actually reference. Implementations live in pce_core.c or
 * stubbed directly in the engine's source — the ones we don't need
 * (patch_rom, wait_next_vsync, strupr, strcasestr, stricmp) resolve
 * to weak stubs in pce_core.c; the linker drops them if unused.
 */
#ifndef _UTILS_H_THUMBY
#define _UTILS_H_THUMBY

#include "cleantypes.h"

/* HuExpress's logging facade. The FINAL_RELEASE build already #defines
 * Log away in most call sites, but a few survive. pce_core.c provides
 * a stub that routes to printf on host and to nes_log (if linked) on
 * the device. */
void Log(const char *fmt, ...);

/* CRC32 lookup table used by hucrc.c. Defined in hucrc.c itself. */
extern unsigned long TAB_CONST[256];

/* Used by pce.c when patching ROMs in-place (old homebrew support).
 * We don't ship patches; stub lives in pce_core.c. */
void patch_rom(char *filename, int offset, uchar value);

/* HuExpress pauses to align to vsync when uncapped. Our frame pacing
 * happens in pce_run.c / pcehost, so this is a no-op. */
void wait_next_vsync(void);

#endif
