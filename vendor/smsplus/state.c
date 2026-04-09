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
 *   Nintendo Gamecube State Management
 *
 ******************************************************************************/

#include "shared.h"

/* THUMBYNES PATCH: when compiled with -DTHUMBY_STATE_BRIDGE the
 * fwrite / fread calls below redirect through a small FatFs-backed
 * shim instead of libc stdio. The host build (with real stdio)
 * leaves these as the standard library calls. The caller passes a
 * STATE_FILE pointer (= thumby_state_io_t* on device) instead of a
 * FILE*. See device/thumby_state_bridge.[ch]. */
#ifdef THUMBY_STATE_BRIDGE
#  include "thumby_state_bridge.h"
#  define STATE_FILE   thumby_state_io_t
#  define STATE_WRITE  thumby_state_write
#  define STATE_READ   thumby_state_read
#else
#  define STATE_FILE   FILE
#  define STATE_WRITE  fwrite
#  define STATE_READ   fread
#endif

/*
state: sizeof sms=8216
state: sizeof vdp=16524
state: sizeof Z80=72
state: sizeof SN76489_Context=92
state: sizeof coleco=8
*/

int system_save_state(STATE_FILE *mem)
{
  uint8 padding[16] = {0};
  int i;

  /*** Save SMS Context ***/
  STATE_WRITE(sms.wram, 0x2000, 1, mem);
  STATE_WRITE(&sms.paused, 1, 1, mem);
  STATE_WRITE(&sms.save, 1, 1, mem);
  STATE_WRITE(&sms.territory, 1, 1, mem);
  STATE_WRITE(&sms.console, 1, 1, mem);
  STATE_WRITE(&sms.display, 1, 1, mem);
  STATE_WRITE(&sms.fm_detect, 1, 1, mem);
  STATE_WRITE(&sms.glasses_3d, 1, 1, mem);
  STATE_WRITE(&sms.hlatch, 1, 1, mem);
  STATE_WRITE(&sms.use_fm, 1, 1, mem);
  STATE_WRITE(&sms.memctrl, 1, 1, mem);
  STATE_WRITE(&sms.ioctrl, 1, 1, mem);
  STATE_WRITE(&padding, 1, 1, mem);
  STATE_WRITE(&sms.sio, 8, 1, mem);
  STATE_WRITE(&sms.device, 2, 1, mem);
  STATE_WRITE(&sms.gun_offset, 1, 1, mem);
  STATE_WRITE(&padding, 1, 1, mem);

  /*** Save VDP state ***/
  STATE_WRITE(vdp.vram, 0x4000, 1, mem);
  STATE_WRITE(vdp.cram, 0x40, 1, mem);
  STATE_WRITE(vdp.reg, 0x10, 1, mem);
  STATE_WRITE(&vdp.vscroll, 1, 1, mem);
  STATE_WRITE(&vdp.status, 1, 1, mem);
  STATE_WRITE(&vdp.latch, 1, 1, mem);
  STATE_WRITE(&vdp.pending, 1, 1, mem);
  STATE_WRITE(&vdp.addr, 2, 1, mem);
  STATE_WRITE(&vdp.code, 1, 1, mem);
  STATE_WRITE(&vdp.buffer, 1, 1, mem);
  STATE_WRITE(&vdp.pn, 4, 1, mem);
  STATE_WRITE(&vdp.ct, 4, 1, mem);
  STATE_WRITE(&vdp.pg, 4, 1, mem);
  STATE_WRITE(&vdp.sa, 4, 1, mem);
  STATE_WRITE(&vdp.sg, 4, 1, mem);
  STATE_WRITE(&vdp.ntab, 4, 1, mem);
  STATE_WRITE(&vdp.satb, 4, 1, mem);
  STATE_WRITE(&vdp.line, 4, 1, mem);
  STATE_WRITE(&vdp.left, 4, 1, mem);
  STATE_WRITE(&vdp.lpf, 2, 1, mem);
  STATE_WRITE(&vdp.height, 1, 1, mem);
  STATE_WRITE(&vdp.extended, 1, 1, mem);
  STATE_WRITE(&vdp.mode, 1, 1, mem);
  STATE_WRITE(&vdp.irq, 1, 1, mem);
  STATE_WRITE(&vdp.vint_pending, 1, 1, mem);
  STATE_WRITE(&vdp.hint_pending, 1, 1, mem);
  STATE_WRITE(&vdp.cram_latch, 2, 1, mem);
  STATE_WRITE(&vdp.spr_col, 2, 1, mem);
  STATE_WRITE(&vdp.spr_ovr, 1, 1, mem);
  STATE_WRITE(&vdp.bd, 1, 1, mem);
  STATE_WRITE(&padding, 2, 1, mem);

  /*** Save cart info ***/
  for (i = 0; i < 4; i++)
  {
    STATE_WRITE(&cart.fcr[i], 1, 1, mem);
  }

  /*** Save SRAM ***/
  STATE_WRITE(cart.sram, 0x8000, 1, mem);

  /*** Save Z80 Context ***/
  STATE_WRITE(&Z80.pc, 4, 1, mem);
  STATE_WRITE(&Z80.sp, 4, 1, mem);
  STATE_WRITE(&Z80.af, 4, 1, mem);
  STATE_WRITE(&Z80.bc, 4, 1, mem);
  STATE_WRITE(&Z80.de, 4, 1, mem);
  STATE_WRITE(&Z80.hl, 4, 1, mem);
  STATE_WRITE(&Z80.ix, 4, 1, mem);
  STATE_WRITE(&Z80.iy, 4, 1, mem);
  STATE_WRITE(&Z80.wz, 4, 1, mem);
  STATE_WRITE(&Z80.af2, 4, 1, mem);
  STATE_WRITE(&Z80.bc2, 4, 1, mem);
  STATE_WRITE(&Z80.de2, 4, 1, mem);
  STATE_WRITE(&Z80.hl2, 4, 1, mem);
  STATE_WRITE(&Z80.r, 1, 1, mem);
  STATE_WRITE(&Z80.r2, 1, 1, mem);
  STATE_WRITE(&Z80.iff1, 1, 1, mem);
  STATE_WRITE(&Z80.iff2, 1, 1, mem);
  STATE_WRITE(&Z80.halt, 1, 1, mem);
  STATE_WRITE(&Z80.im, 1, 1, mem);
  STATE_WRITE(&Z80.i, 1, 1, mem);
  STATE_WRITE(&Z80.nmi_state, 1, 1, mem);
  STATE_WRITE(&Z80.nmi_pending, 1, 1, mem);
  STATE_WRITE(&Z80.irq_state, 1, 1, mem);
  STATE_WRITE(&Z80.after_ei, 1, 1, mem);
  STATE_WRITE(&padding, 9, 1, mem);

#if 0
  /*** Save YM2413 ***/
  memcpy (&state[bufferptr], FM_GetContextPtr (), FM_GetContextSize ());
  bufferptr += FM_GetContextSize ();
#endif

  /*** Save SN76489 ***/
  STATE_WRITE(SN76489_GetContextPtr(0), SN76489_GetContextSize(), 1, mem);

  STATE_WRITE(&coleco.pio_mode, 1, 1, mem);
  STATE_WRITE(&coleco.port53, 1, 1, mem);
  STATE_WRITE(&coleco.port7F, 1, 1, mem);
  STATE_WRITE(&padding, 5, 1, mem);

  return 0;
}


void system_load_state(STATE_FILE *mem)
{
  uint8 padding[16] = {0};
  int i;

  /* Initialize everything */
  system_reset();

  /*** Set SMS Context ***/
  int current_console = sms.console;
  sms.console = 0xFF;

  STATE_READ(sms.wram, 0x2000, 1, mem);
  STATE_READ(&sms.paused, 1, 1, mem);
  STATE_READ(&sms.save, 1, 1, mem);
  STATE_READ(&sms.territory, 1, 1, mem);
  STATE_READ(&sms.console, 1, 1, mem);
  STATE_READ(&sms.display, 1, 1, mem);
  STATE_READ(&sms.fm_detect, 1, 1, mem);
  STATE_READ(&sms.glasses_3d, 1, 1, mem);
  STATE_READ(&sms.hlatch, 1, 1, mem);
  STATE_READ(&sms.use_fm, 1, 1, mem);
  STATE_READ(&sms.memctrl, 1, 1, mem);
  STATE_READ(&sms.ioctrl, 1, 1, mem);
  STATE_READ(&padding, 1, 1, mem);
  STATE_READ(&sms.sio, 8, 1, mem);
  STATE_READ(&sms.device, 2, 1, mem);
  STATE_READ(&sms.gun_offset, 1, 1, mem);
  STATE_READ(&padding, 1, 1, mem);

  if(sms.console != current_console)
  {
      MESSAGE_ERROR("Bad save data\n");
      set_rom_config();
      system_reset();
      return;
  }

  /*** Set vdp state ***/
  STATE_READ(vdp.vram, 0x4000, 1, mem);
  STATE_READ(vdp.cram, 0x40, 1, mem);
  STATE_READ(vdp.reg, 0x10, 1, mem);
  STATE_READ(&vdp.vscroll, 1, 1, mem);
  STATE_READ(&vdp.status, 1, 1, mem);
  STATE_READ(&vdp.latch, 1, 1, mem);
  STATE_READ(&vdp.pending, 1, 1, mem);
  STATE_READ(&vdp.addr, 2, 1, mem);
  STATE_READ(&vdp.code, 1, 1, mem);
  STATE_READ(&vdp.buffer, 1, 1, mem);
  STATE_READ(&vdp.pn, 4, 1, mem);
  STATE_READ(&vdp.ct, 4, 1, mem);
  STATE_READ(&vdp.pg, 4, 1, mem);
  STATE_READ(&vdp.sa, 4, 1, mem);
  STATE_READ(&vdp.sg, 4, 1, mem);
  STATE_READ(&vdp.ntab, 4, 1, mem);
  STATE_READ(&vdp.satb, 4, 1, mem);
  STATE_READ(&vdp.line, 4, 1, mem);
  STATE_READ(&vdp.left, 4, 1, mem);
  STATE_READ(&vdp.lpf, 2, 1, mem);
  STATE_READ(&vdp.height, 1, 1, mem);
  STATE_READ(&vdp.extended, 1, 1, mem);
  STATE_READ(&vdp.mode, 1, 1, mem);
  STATE_READ(&vdp.irq, 1, 1, mem);
  STATE_READ(&vdp.vint_pending, 1, 1, mem);
  STATE_READ(&vdp.hint_pending, 1, 1, mem);
  STATE_READ(&vdp.cram_latch, 2, 1, mem);
  STATE_READ(&vdp.spr_col, 2, 1, mem);
  STATE_READ(&vdp.spr_ovr, 1, 1, mem);
  STATE_READ(&vdp.bd, 1, 1, mem);
  STATE_READ(&padding, 2, 1, mem);

  /** restore video & audio settings (needed if timing changed) ***/
  vdp_init();
  sound_init();

  /*** Set cart info ***/
  for (i = 0; i < 4; i++)
  {
    STATE_READ(&cart.fcr[i], 1, 1, mem);
  }

  /*** Set SRAM ***/
  STATE_READ(cart.sram, 0x8000, 1, mem);

  /*** Set Z80 Context ***/
  STATE_READ(&Z80.pc, 4, 1, mem);
  STATE_READ(&Z80.sp, 4, 1, mem);
  STATE_READ(&Z80.af, 4, 1, mem);
  STATE_READ(&Z80.bc, 4, 1, mem);
  STATE_READ(&Z80.de, 4, 1, mem);
  STATE_READ(&Z80.hl, 4, 1, mem);
  STATE_READ(&Z80.ix, 4, 1, mem);
  STATE_READ(&Z80.iy, 4, 1, mem);
  STATE_READ(&Z80.wz, 4, 1, mem);
  STATE_READ(&Z80.af2, 4, 1, mem);
  STATE_READ(&Z80.bc2, 4, 1, mem);
  STATE_READ(&Z80.de2, 4, 1, mem);
  STATE_READ(&Z80.hl2, 4, 1, mem);
  STATE_READ(&Z80.r, 1, 1, mem);
  STATE_READ(&Z80.r2, 1, 1, mem);
  STATE_READ(&Z80.iff1, 1, 1, mem);
  STATE_READ(&Z80.iff2, 1, 1, mem);
  STATE_READ(&Z80.halt, 1, 1, mem);
  STATE_READ(&Z80.im, 1, 1, mem);
  STATE_READ(&Z80.i, 1, 1, mem);
  STATE_READ(&Z80.nmi_state, 1, 1, mem);
  STATE_READ(&Z80.nmi_pending, 1, 1, mem);
  STATE_READ(&Z80.irq_state, 1, 1, mem);
  STATE_READ(&Z80.after_ei, 1, 1, mem);
  STATE_READ(&padding, 9, 1, mem);

#if 0
  /*** Set YM2413 ***/
  buf = malloc(FM_GetContextSize());
  memcpy (buf, &state[bufferptr], FM_GetContextSize ());
  FM_SetContext(buf);
  free(buf);
  bufferptr += FM_GetContextSize ();
#endif

  // Preserve clock rate
  SN76489_Context* psg = (SN76489_Context*)SN76489_GetContextPtr(0);
  float psg_Clock = psg->Clock;
  float psg_dClock = psg->dClock;

  /*** Set SN76489 ***/
  STATE_READ(SN76489_GetContextPtr(0), SN76489_GetContextSize(), 1, mem);

  // Restore clock rate
  psg->Clock = psg_Clock;
  psg->dClock = psg_dClock;

  STATE_READ(&coleco.pio_mode, 1, 1, mem);
  STATE_READ(&coleco.port53, 1, 1, mem);
  STATE_READ(&coleco.port7F, 1, 1, mem);
  STATE_READ(&padding, 5, 1, mem);

  if (sms.console == CONSOLE_COLECO)
  {
    coleco_port_w(0x53, coleco.port53);
    coleco_port_w(0x7F, coleco.port7F);
  }
  else if (sms.console != CONSOLE_SG1000)
  {
    /* Cartridge by default */
    slot.rom    = cart.rom;
    slot.pages  = cart.pages;
    slot.mapper = cart.mapper;
    slot.fcr = &cart.fcr[0];

    /* Restore mapping */
    mapper_reset();
    cpu_readmap[0]  = &slot.rom[0];
    if (slot.mapper != MAPPER_KOREA_MSX)
    {
      mapper_16k_w(0,slot.fcr[0]);
      mapper_16k_w(1,slot.fcr[1]);
      mapper_16k_w(2,slot.fcr[2]);
      mapper_16k_w(3,slot.fcr[3]);
    }
    else
    {
      mapper_8k_w(0,slot.fcr[0]);
      mapper_8k_w(1,slot.fcr[1]);
      mapper_8k_w(2,slot.fcr[2]);
      mapper_8k_w(3,slot.fcr[3]);
    }
  }

  // /* Force full pattern cache update */
  // bg_list_index = 0x200;
  // for(i = 0; i < 0x200; i++)
  // {
  //   bg_name_list[i] = i;
  //   bg_name_dirty[i] = -1;
  // }

  /* Restore palette */
  for(i = 0; i < PALETTE_SIZE; i++)
    palette_sync(i);
}
