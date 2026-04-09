/*
 * ThumbyNES — stdio shim for vendored state.c files.
 *
 * nofrendo's `nes/state.c` and smsplus's `state.c` both serialize
 * cart state through libc stdio (fopen / fwrite / fread / fseek /
 * fclose). The device build doesn't have libc-stdio for FAT files —
 * we use FatFs's FIL / f_open / f_write directly. This header provides
 * a tiny bridge that lets the vendored state.c files keep their
 * stdio surface unchanged: they include this header, redefine the
 * relevant calls via the STATE_* macros, and the macros expand to
 * thin FatFs-backed wrappers in thumby_state_bridge.c.
 *
 * Activated by compiling state.c with -DTHUMBY_STATE_BRIDGE so the
 * host build (which has real stdio) is unaffected.
 */
#ifndef THUMBY_STATE_BRIDGE_H
#define THUMBY_STATE_BRIDGE_H

#include <stddef.h>

typedef struct thumby_state_io thumby_state_io_t;

extern thumby_state_io_t *thumby_state_open (const char *fn, const char *mode);
extern int                thumby_state_close(thumby_state_io_t *io);
extern size_t             thumby_state_write(const void *buf, size_t sz, size_t n, thumby_state_io_t *io);
extern size_t             thumby_state_read (void *buf, size_t sz, size_t n, thumby_state_io_t *io);
extern int                thumby_state_seek (thumby_state_io_t *io, long off, int whence);
extern long               thumby_state_tell (thumby_state_io_t *io);

#endif
