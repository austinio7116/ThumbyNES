/*
**
** software implementation of Yamaha FM sound generator (YM2612/YM3438)
**
** Original code (MAME fm.c)
**
** Copyright (C) 2001, 2002, 2003 Jarek Burczynski (bujar at mame dot net)
** Copyright (C) 1998 Tatsuyuki Satoh , MultiArcadeMachineEmulator development
**
** Version 1.4 (final beta)
**
** Additional code & fixes by Eke-Eke for Genesis Plus GX
**
*/

/* ThumbyNES vendor: imported from Genesis Plus GX
 * Source: https://github.com/ekeeke/Genesis-Plus-GX
 * Commit: c7ecd07f0a08db1cdd6871d788476bfac55be638
 *
 * Public functions are renamed YMGP_*() so the GenPlus implementation
 * can coexist with PicoDrive's YM2612 in the same library; only one is
 * compiled at a time, selected by the THUMBYNES_YM2612_GENPLUS CMake
 * option. The shim in ym2612_shim.c provides the PicoDrive-API entry
 * points (YM2612Init_, YM2612Write_, YM2612UpdateOne_, ...) and forwards
 * to these.
 */

#ifndef _H_YM2612_GENPLUS_
#define _H_YM2612_GENPLUS_

#ifdef __cplusplus
extern "C" {
#endif

enum {
  YMGP_DISCRETE = 0,
  YMGP_INTEGRATED,
  YMGP_ENHANCED
};

extern void         YMGP_Init(void);
extern void         YMGP_Shutdown(void);
extern void         YMGP_Config(int type);
extern void         YMGP_ResetChip(void);
extern void         YMGP_Update(int *buffer, int length);
extern void         YMGP_Write(unsigned int a, unsigned int v);
extern unsigned int YMGP_Read(void);

/* Shim accessors — see ym2612_genplus.c. */
extern unsigned int YMGP_GetStatus(void);
extern void         YMGP_SetDac(int dacen, int dacout);
extern void         YMGP_SetModeBits(unsigned int mode);

#ifdef __cplusplus
}
#endif

#endif /* _H_YM2612_GENPLUS_ */
