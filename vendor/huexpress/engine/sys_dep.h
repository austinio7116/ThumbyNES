#ifndef _INCLUDE_SYS_DEP_H
#define _INCLUDE_SYS_DEP_H

/* This header include all functionalities needed to be implemented in order
 * to make a port of Hu-Go! on various platforms
 */

#include "cleantypes.h"
#include "sys_cd.h"
#include "sys_inp.h"
#include "sys_gfx.h"
#include "sys_misc.h"
#include "sys_snd.h"

/* ThumbyNES patch: <netinet/in.h> pulls in BSD socket machinery
 * we never use. On THUMBY_BUILD the htons/ntohs helpers come
 * from thumby_platform.h. See vendor/VENDORING.md. */
#ifndef THUMBY_BUILD
#  include <netinet/in.h>
#endif

#endif
