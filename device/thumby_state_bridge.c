/*
 * ThumbyNES — stdio -> FatFs shim for vendored state.c files.
 *
 * See thumby_state_bridge.h. We hold a single static FIL instance
 * because save and load are mutually exclusive — a state save can
 * never run concurrently with a state load on the same device.
 */
#include "thumby_state_bridge.h"

#include <string.h>
#include "ff.h"

struct thumby_state_io {
    FIL  fil;
    int  in_use;
};

static struct thumby_state_io g_io;

thumby_state_io_t *thumby_state_open(const char *fn, const char *mode) {
    if (g_io.in_use) return NULL;
    BYTE flags = 0;
    if (strchr(mode, 'w')) flags = FA_WRITE | FA_CREATE_ALWAYS;
    else if (strchr(mode, 'r')) flags = FA_READ;
    else return NULL;
    if (f_open(&g_io.fil, fn, flags) != FR_OK) return NULL;
    g_io.in_use = 1;
    return &g_io;
}

int thumby_state_close(thumby_state_io_t *io) {
    if (!io || !io->in_use) return -1;
    f_close(&io->fil);
    io->in_use = 0;
    return 0;
}

size_t thumby_state_write(const void *buf, size_t sz, size_t n, thumby_state_io_t *io) {
    if (!io || !io->in_use) return 0;
    UINT bw = 0;
    UINT total = (UINT)(sz * n);
    if (f_write(&io->fil, buf, total, &bw) != FR_OK || bw != total) return 0;
    return n;
}

size_t thumby_state_read(void *buf, size_t sz, size_t n, thumby_state_io_t *io) {
    if (!io || !io->in_use) return 0;
    UINT br = 0;
    UINT total = (UINT)(sz * n);
    if (f_read(&io->fil, buf, total, &br) != FR_OK || br != total) return 0;
    return n;
}

int thumby_state_seek(thumby_state_io_t *io, long off, int whence) {
    if (!io || !io->in_use) return -1;
    FSIZE_t pos = 0;
    /* SEEK_SET=0, SEEK_CUR=1, SEEK_END=2 — same constants the
     * standard says. */
    if (whence == 0) pos = (FSIZE_t)off;
    else if (whence == 1) pos = f_tell(&io->fil) + (FSIZE_t)off;
    else if (whence == 2) pos = f_size(&io->fil) + (FSIZE_t)off;
    else return -1;
    return f_lseek(&io->fil, pos) == FR_OK ? 0 : -1;
}

long thumby_state_tell(thumby_state_io_t *io) {
    if (!io || !io->in_use) return -1;
    return (long)f_tell(&io->fil);
}
