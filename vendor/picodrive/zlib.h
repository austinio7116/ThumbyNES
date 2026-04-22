/*
 * ThumbyNES — minimal zlib.h stub for the device build.
 *
 * PicoDrive's cart.c and cd/cdd.c reference zlib types and functions
 * to handle ZIP / CSO / GZ container formats. On the RP2350 device we
 * only ever load raw Mega-Drive ROMs via XIP, so none of the zlib
 * code paths ever execute at runtime — the symbols just need to
 * resolve at link time. Link-time stubs come from --defsym in the
 * device CMakeLists, but the compile still needs the header.
 *
 * This file provides just enough declarations for the uses in
 * picodrive's codebase. It is NOT a functional zlib — do not include
 * on builds that actually use zlib (host build links the system one).
 */
#ifndef THUMBYNES_MINIMAL_ZLIB_H
#define THUMBYNES_MINIMAL_ZLIB_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned char Bytef;
typedef unsigned int  uInt;
typedef unsigned long uLong;
typedef void         *voidpf;

typedef struct z_stream_s {
    const Bytef *next_in;
    uInt         avail_in;
    uLong        total_in;
    Bytef       *next_out;
    uInt         avail_out;
    uLong        total_out;
    const char  *msg;
    void        *state;
    voidpf       (*zalloc)(voidpf, uInt, uInt);
    void         (*zfree) (voidpf, voidpf);
    voidpf       opaque;
    int          data_type;
    uLong        adler;
    uLong        reserved;
} z_stream;

typedef uintptr_t gzFile;

/* Return codes */
#define Z_OK                   0
#define Z_STREAM_END           1
#define Z_FINISH               4
#define Z_NO_FLUSH             0
#define Z_NULL                 0
#define Z_DEFAULT_STRATEGY     0
#define Z_DEFAULT_COMPRESSION (-1)
#define MAX_WBITS             15
#define ZLIB_VERSION          "1.2.11"

/* Functions — all stubbed to 0/error at link time via --defsym. */
int    inflateInit2_(z_stream *strm, int windowBits,
                     const char *version, int stream_size);
#define inflateInit2(strm, windowBits) \
        inflateInit2_((strm), (windowBits), "0", (int)sizeof(z_stream))
int    inflate      (z_stream *strm, int flush);
int    inflateEnd   (z_stream *strm);
int    inflateReset (z_stream *strm);
uLong  crc32        (uLong crc, const Bytef *buf, uInt len);

/* gzip file I/O — also stubbed. */
gzFile gzopen      (const char *path, const char *mode);
int    gzwrite     (gzFile file, const void *buf, unsigned len);
int    gzread      (gzFile file, void *buf, unsigned len);
int    gzclose     (gzFile file);
int    gzeof       (gzFile file);
long   gzseek      (gzFile file, long offset, int whence);
int    gzsetparams (gzFile file, int level, int strategy);

#endif
