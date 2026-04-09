/******************************************************************************
 *  Sega Master System / GameGear Emulator
 *  Copyright (C) 1998-2007  Charles MacDonald
 *
 *  additionnal code by Eke-Eke (SMS Plus GX)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *   Freeze State support
 *
 ******************************************************************************/

#ifndef _STATE_H_
#define _STATE_H_


#define STATE_VERSION   0x0104      /* Version 1.4 (BCD) */
#define STATE_HEADER    "SST\0"     /* State file header */

/* THUMBYNES PATCH: parameter type switches between FILE* (host
 * stdio build) and thumby_state_io_t* (device FatFs bridge) — see
 * the same #ifdef in state.c. */
#ifdef THUMBY_STATE_BRIDGE
#  include "thumby_state_bridge.h"
extern int  system_save_state(thumby_state_io_t *mem);
extern void system_load_state(thumby_state_io_t *mem);
#else
#  include <stdio.h>
extern int  system_save_state(FILE *mem);
extern void system_load_state(FILE *mem);
#endif

#endif /* _STATE_H_ */
