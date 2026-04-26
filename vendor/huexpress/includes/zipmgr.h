/* ThumbyNES zipmgr.h stub.
 *
 * The original zipmgr provided ZIP file ROM loading. Not vendored —
 * engine/pce.c's ZIP branches are dead code on this build because we
 * never feed it a .zip path. The constants are provided so pce.c still
 * compiles; the matching helpers are declared but never defined so the
 * link stage fails loudly if anything actually calls them. */
#pragma once

#define ZIP_ERROR      (-1)
#define ZIP_HAS_NONE    0
#define ZIP_HAS_PCE     1
#define ZIP_HAS_SGX     2
#define ZIP_HAS_BOTH    3

#ifndef PATH_SLASH
#  if defined(_WIN32)
#    define PATH_SLASH "\\"
#  else
#    define PATH_SLASH "/"
#  endif
#endif

int unzip_rom(const char *src, char *out_name);
int rezip_rom(const char *src);
