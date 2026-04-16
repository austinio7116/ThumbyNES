/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Library General Public License for more details.  To obtain a
** copy of the GNU Library General Public License, write to the Free
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** nes/state.c: Save state support
**
*/

#include "nes.h"

/* THUMBYNES PATCH: when compiled with -DTHUMBY_STATE_BRIDGE the
 * fopen / fwrite / fread / fseek / fclose calls below redirect
 * through a small FatFs-backed shim instead of libc stdio. The
 * host build (with real stdio) leaves these as the standard
 * library calls. See device/thumby_state_bridge.[ch]. */
#ifdef THUMBY_STATE_BRIDGE
#  include "thumby_state_bridge.h"
#  define STATE_FILE   thumby_state_io_t
#  define STATE_OPEN   thumby_state_open
#  define STATE_CLOSE  thumby_state_close
#  define STATE_WRITE  thumby_state_write
#  define STATE_READ   thumby_state_read
#  define STATE_SEEK   thumby_state_seek
#else
#  define STATE_FILE   FILE
#  define STATE_OPEN   fopen
#  define STATE_CLOSE  fclose
#  define STATE_WRITE  fwrite
#  define STATE_READ   fread
#  define STATE_SEEK   fseek
#endif

/**
 * Save file format:
 *
 * The file has an 8 byte header followed by a series of blocks.
 * Integer values are big endian.
 *
 *    Header format:
 *       magic (4 bytes), always "SNSS"
 *       block count (4 bytes), int32be
 *
 *    Block format:
 *       name (4 bytes)
 *       block version (4 bytes), int32be
 *       block length (4 bytes), int32be
 *       data ("block length" bytes)
 *
 * In actuality, most blocks have a fixed size and all are at version 1.
 * - BASR: 6449 bytes
 * - INFO: 256 bytes
 * - SOUN: 22 bytes
 * - SRAM: prg-ram + 1 bytes
 * - VRAM: chr-ram bytes
 * - MPRD: 152 bytes
 */

typedef struct
{
   uint8  magic[4];
   uint32 blocks;
} header_t;

typedef struct
{
   uint8  name[4];
   uint32 version;
   uint32 length;
   uint8  data[];
} block_t;

#define _fread(buffer, size) {                       \
   if (STATE_READ(buffer, size, 1, file) != 1)       \
   {                                                 \
      MESSAGE_ERROR("state_load: fread failed.\n");  \
      goto _error;                                   \
   }                                                 \
}

#define _fwrite(buffer, size) {                      \
   if (STATE_WRITE(buffer, size, 1, file) != 1)      \
   {                                                 \
      MESSAGE_ERROR("state_save: fwrite failed.\n"); \
      goto _error;                                   \
   }                                                 \
}

#ifndef IS_BIG_ENDIAN
   static inline uint16 swap16(uint16 x)
   {
      return (x << 8) | (x >> 8);
   }

   static inline uint32 swap32(uint32 x)
   {
      return ((x>>24)&0xff) | ((x<<8)&0xff0000) | ((x>>8)&0xff00) | ((x<<24)&0xff000000);
   }
#else
   #define swap16(x) (x)
   #define swap32(x) (x)
#endif


static bool memory_zone_dirty(const void *ptr, size_t size)
{
   size_t pos = 0;

   while (size - pos >= 4)
   {
      if (*((uint32 *)(ptr + pos)) != 0)
         return true;
      pos += 4;
   }

   while (pos < size)
   {
      if (*((uint8 *)(ptr + pos)) != 0)
         return true;
      pos += 1;
   }

   return false;
}


int state_save(const char* fn)
{
   uint32 numberOfBlocks = 0;
   uint8 buffer[600];
   nes_t *machine = nes_getptr();
   STATE_FILE *file;

   if (!(file = STATE_OPEN(fn, "wb")))
   {
       MESSAGE_ERROR("state_save: file '%s' could not be opened.\n", fn);
       return -1; //goto _error;
   }

   MESSAGE_INFO("state_save: file '%s' opened.\n", fn);

   _fwrite("SNSS\x00\x00\x00\x05", 8);


   /****************************************************/

   MESSAGE_INFO("  - Saving base block\n");

   buffer[0] = machine->cpu->a_reg;
   buffer[1] = machine->cpu->x_reg;
   buffer[2] = machine->cpu->y_reg;
   buffer[3] = machine->cpu->p_reg;
   buffer[4] = machine->cpu->s_reg;
   buffer[5] = machine->cpu->pc_reg / 256;
   buffer[6] = machine->cpu->pc_reg % 256;
   buffer[7] = machine->ppu->ctrl0;
   buffer[8] = machine->ppu->ctrl1;

   _fwrite("BASR\x00\x00\x00\x01\x00\x00\x19\x31", 12);
   _fwrite(&buffer, 9);
   _fwrite(machine->mem->ram, 0x800);
   _fwrite(machine->ppu->oam, 0x100);
   _fwrite(machine->ppu->nametab, 0x1000);

   /* Mask off priority color bits */
   for (int i = 0; i < 32; i++)
      buffer[i] = machine->ppu->palette[i] & 0x3F;

   buffer[32] = machine->ppu->nt_map[0];
   buffer[33] = machine->ppu->nt_map[1];
   buffer[34] = machine->ppu->nt_map[2];
   buffer[35] = machine->ppu->nt_map[3];
   buffer[36] = machine->ppu->vaddr / 256;
   buffer[37] = machine->ppu->vaddr % 256;
   buffer[38] = machine->ppu->oam_addr;
   buffer[39] = machine->ppu->tile_xofs;

   _fwrite(&buffer, 40);
   numberOfBlocks++;


   /****************************************************/

   MESSAGE_INFO("  - Saving info block\n");

   _fwrite("INFO\x00\x00\x00\x01\x00\x00\x01\x00", 12);
   _fwrite(&buffer, 0x100);
   numberOfBlocks++;


   /****************************************************/

   MESSAGE_INFO("  - Saving sound block\n");

   buffer[0x00] = machine->apu->rectangle[0].regs[0];
   buffer[0x01] = machine->apu->rectangle[0].regs[1];
   buffer[0x02] = machine->apu->rectangle[0].regs[2];
   buffer[0x03] = machine->apu->rectangle[0].regs[3];
   buffer[0x04] = machine->apu->rectangle[1].regs[0];
   buffer[0x05] = machine->apu->rectangle[1].regs[1];
   buffer[0x06] = machine->apu->rectangle[1].regs[2];
   buffer[0x07] = machine->apu->rectangle[1].regs[3];
   buffer[0x08] = machine->apu->triangle.regs[0];
   buffer[0x0A] = machine->apu->triangle.regs[1];
   buffer[0x0B] = machine->apu->triangle.regs[2];
   buffer[0X0C] = machine->apu->noise.regs[0];
   buffer[0X0E] = machine->apu->noise.regs[1];
   buffer[0x0F] = machine->apu->noise.regs[2];
   buffer[0x10] = machine->apu->dmc.regs[0];
   buffer[0x11] = machine->apu->dmc.regs[1];
   buffer[0x12] = machine->apu->dmc.regs[2];
   buffer[0x13] = machine->apu->dmc.regs[3];
   buffer[0x15] = machine->apu->control_reg;

   _fwrite("SOUN\x00\x00\x00\x01\x00\x00\x00\x16", 12);
   _fwrite(&buffer, 0x16);
   numberOfBlocks++;


   /****************************************************/

   if (memory_zone_dirty(machine->cart->chr_ram, 0x2000 * machine->cart->chr_ram_banks))
   {
      MESSAGE_INFO("  - Saving VRAM block\n");

      _fwrite("VRAM\x00\x00\x00\x01\x00\x00\x20\x00", 12);
      _fwrite(machine->cart->chr_ram, 0x2000 * machine->cart->chr_ram_banks);
      numberOfBlocks++;
   }


   /****************************************************/

   if (memory_zone_dirty(machine->cart->prg_ram, 0x2000 * machine->cart->prg_ram_banks))
   {
      MESSAGE_INFO("  - Saving SRAM block\n");

      // Byte 0 = SRAM enabled (unused)
      // Length is always $2001
      _fwrite("SRAM\x00\x00\x00\x01\x00\x00\x20\x01\x01", 13);
      _fwrite(machine->cart->prg_ram, 0x2000 * machine->cart->prg_ram_banks);
      numberOfBlocks++;
   }


   /****************************************************/

   if (machine->mapper->number > 0)
   {
      MESSAGE_INFO("  - Saving mapper block\n");

      memset(buffer, 0, sizeof(buffer));

      for (int i = 0; i < 4; i++)
      {
         uint16 temp = (mem_getpage((i + 4) * 4) - machine->cart->prg_rom) >> 13;
         temp = swap16(temp);
         buffer[(i * 2) + 0] = ((uint8 *) &temp)[0];
         buffer[(i * 2) + 1] = ((uint8 *) &temp)[1];
      }

      for (int i = 0; i < 8; i++)
      {
         uint16 temp = (machine->cart->chr_rom_banks) ? ((ppu_getpage(i) - machine->cart->chr_rom) >> 10) : (i);
         temp = swap16(temp);
         buffer[8 + (i * 2) + 0] = ((uint8 *) &temp)[0];
         buffer[8 + (i * 2) + 1] = ((uint8 *) &temp)[1];
      }

      if (machine->mapper->get_state)
      {
         machine->mapper->get_state(buffer + 0x18);
      }

      _fwrite("MPRD\x00\x00\x00\x01\x00\x00\x02\x18", 12);
      _fwrite(&buffer, 0x218);
      numberOfBlocks++;
   }


   /****************************************************/

   /* THUMBYNES PATCH: ThumbyNES extension block.
    *
    * The native SNSS format drops a pile of runtime state that games
    * like SMB (NROM) depend on across a power cycle — PPU internal
    * latches and scroll state, APU frame counter state, machine-level
    * scanline/cycle counters, CPU int_pending. For mapper-1/4 carts
    * these happen to be corrected as a side effect of MPRD's mapper
    * bank/state restore; NROM has no MPRD block so the game boots
    * into a limbo state and hangs.
    *
    * This THMB block is a ThumbyNES extension: older readers ignore
    * unknown block types (see "unknown block" branch in state_load).
    * Length is fixed at 44 bytes, version 1. All multi-byte fields
    * are stored as uint32 big-endian to match the SNSS convention;
    * bool fields are a single byte. */
   {
      MESSAGE_INFO("  - Saving ThumbyNES extension block\n");

      memset(buffer, 0, sizeof(buffer));

      uint32 u32;
      int off = 0;

      /* PPU internals not covered by BASR. */
      buffer[off++] = machine->ppu->stat;
      buffer[off++] = machine->ppu->latch;
      buffer[off++] = machine->ppu->vdata_latch;
      buffer[off++] = machine->ppu->nametab_base;
      buffer[off++] = machine->ppu->vram_accessible ? 1 : 0;

      u32 = swap32((uint32)machine->ppu->vaddr_latch);  memcpy(&buffer[off], &u32, 4); off += 4;
      u32 = swap32((uint32)machine->ppu->vaddr_inc);    memcpy(&buffer[off], &u32, 4); off += 4;
      u32 = swap32((uint32)machine->ppu->left_bg_counter); memcpy(&buffer[off], &u32, 4); off += 4;
      u32 = swap32(machine->ppu->strike_cycle);         memcpy(&buffer[off], &u32, 4); off += 4;
      u32 = swap32((uint32)machine->ppu->scanline);     memcpy(&buffer[off], &u32, 4); off += 4;

      /* NES machine-level counters. */
      u32 = swap32((uint32)machine->scanline);          memcpy(&buffer[off], &u32, 4); off += 4;
      u32 = swap32((uint32)machine->cycles);            memcpy(&buffer[off], &u32, 4); off += 4;

      /* APU frame counter state — $4017 is NOT in the SOUN register
       * replay (which only covers $4000-$4015), so fc.state is wrong
       * after a cold reload. */
      buffer[off++] = (uint8)machine->apu->fc.state;
      u32 = swap32(machine->apu->fc.cycles);            memcpy(&buffer[off], &u32, 4); off += 4;
      buffer[off++] = machine->apu->fc.irq_occurred ? 1 : 0;

      /* CPU pending-IRQ flag (APU frame IRQ / mapper IRQ). */
      buffer[off++] = machine->cpu->int_pending ? 1 : 0;

      /* off == 44 here. */
      _fwrite("THMB\x00\x00\x00\x01\x00\x00\x00\x2C", 12);
      _fwrite(&buffer, 44);
      numberOfBlocks++;
   }


   /****************************************************/

   // Update number of blocks
   STATE_SEEK(file, 4, SEEK_SET);
   numberOfBlocks = swap32(numberOfBlocks);
   _fwrite(&numberOfBlocks, 4);

   STATE_CLOSE(file);

   MESSAGE_INFO("state_save: Game saved!\n");

   return 0;

_error:
   MESSAGE_ERROR("state_save: Save failed!\n");
   STATE_CLOSE(file);
   return -1;
}


int state_load(const char* fn)
{
   uint8 buffer[600];

   nes_t *machine = nes_getptr();
   STATE_FILE *file;

   if (!(file = STATE_OPEN(fn, "rb")))
   {
       MESSAGE_ERROR("state_load: file '%s' could not be opened.\n", fn);
       return -1; //goto _error;
  }

   _fread(buffer, 8);

   if (memcmp(buffer, "SNSS", 4) != 0)
   {
      MESSAGE_ERROR("state_load: file '%s' is not a save file.\n", fn);
      goto _error;
   }

   uint32 numberOfBlocks = swap32(*((uint32*)&buffer[4]));
   uint32 nextBlock = 8;

   MESSAGE_INFO("state_load: file '%s' opened, blocks=%u.\n", fn, numberOfBlocks);

   for (uint32 blk = 0; blk < numberOfBlocks; blk++)
   {
      STATE_SEEK(file, nextBlock, SEEK_SET);
      _fread(buffer, 12);

      uint32 blockVersion = swap32(*((uint32*)&buffer[4]));
      uint32 blockLength = swap32(*((uint32*)&buffer[8]));

      UNUSED(blockVersion);

      nextBlock += 12 + blockLength;

      /****************************************************/

      if (memcmp(buffer, "BASR", 4) == 0)
      {
         MESSAGE_INFO("  - Found base block (%u bytes)\n", blockLength);

         _fread(buffer, 9);

         machine->cpu->a_reg = buffer[0x0];
         machine->cpu->x_reg = buffer[0x1];
         machine->cpu->y_reg = buffer[0x2];
         machine->cpu->p_reg = buffer[0x3];
         machine->cpu->s_reg = buffer[0x4];
         machine->cpu->pc_reg = swap16(*((uint16*)&buffer[0x5]));
         machine->ppu->ctrl0 = buffer[0x7];
         machine->ppu->ctrl1 = buffer[0x8];

         _fread(machine->mem->ram, 0x800);
         _fread(machine->ppu->oam, 0x100);
         _fread(machine->ppu->nametab, 0x1000);
         _fread(machine->ppu->palette, 0x20);

         /* TODO: argh, this is to handle nofrendo's filthy sprite priority method */
         for (int i = 0; i < 8; i++)
            machine->ppu->palette[i << 2] = machine->ppu->palette[0] | 0x80; // BG_TRANS;

         _fread(buffer, 8);

         machine->ppu->vaddr = swap16(*((uint16*)&buffer[0x4]));
         machine->ppu->oam_addr = buffer[0x6];
         machine->ppu->tile_xofs = buffer[0x7];

         /* do some extra handling */
         machine->ppu->flipflop = 0;
         machine->ppu->strikeflag = false;

         ppu_setnametable(0, buffer[0]);
         ppu_setnametable(1, buffer[1]);
         ppu_setnametable(2, buffer[2]);
         ppu_setnametable(3, buffer[3]);

         ppu_write(PPU_CTRL0, machine->ppu->ctrl0);
         ppu_write(PPU_CTRL1, machine->ppu->ctrl1);
         ppu_write(PPU_VADDR, machine->ppu->vaddr >> 8);
         ppu_write(PPU_VADDR, machine->ppu->vaddr & 0xFF);
      }


      /****************************************************/

      else if (memcmp(buffer, "VRAM", 4) == 0)
      {
         MESSAGE_INFO("  - Found VRAM block (%u bytes)\n", blockLength);

         if (machine->cart->chr_ram_banks < (blockLength / ROM_CHR_BANK_SIZE))
         {
            MESSAGE_ERROR("Invalid block size!\n");
            continue;
         }

         _fread(machine->cart->chr_ram, blockLength);
      }


      /****************************************************/

      else if (memcmp(buffer, "SRAM", 4) == 0)
      {
         MESSAGE_INFO("  - Found SRAM block (%u bytes)\n", blockLength);

         if (machine->cart->prg_ram_banks < ((blockLength-1) / ROM_PRG_BANK_SIZE))
         {
            MESSAGE_ERROR("Invalid block size!\n");
            continue;
         }

         _fread(buffer, 1); // SRAM enabled (always true)
         _fread(machine->cart->prg_ram, blockLength - 1);
      }


      /****************************************************/

      else if (memcmp(buffer, "MPRD", 4) == 0)
      {
         MESSAGE_INFO("  - Found mapper block (%u bytes)\n", blockLength);

         _fread(buffer, MIN(blockLength, sizeof(buffer)));

         for (int i = 0; i < 4; i++)
            mmc_bankprg(8, 0x8000 + (i * 0x2000), swap16(((uint16*)buffer)[i]), PRG_ROM);

         if (machine->cart->chr_rom_banks)
         {
            for (int i = 0; i < 8; i++)
               mmc_bankchr(1, i * 0x400, swap16(((uint16*)buffer)[4 + i]), CHR_ROM);
         }
         else if (machine->cart->chr_ram)
         {
            for (int i = 0; i < 8; i++)
               mmc_bankchr(1, i * 0x400, i, CHR_RAM);
         }

         if (machine->mapper->set_state)
            machine->mapper->set_state(buffer + 0x18);
      }


      /****************************************************/

      else if (memcmp(buffer, "SOUN", 4) == 0)
      {
         MESSAGE_INFO("  - Found sound block (%u bytes)\n", blockLength);

         _fread(buffer, 0x16);

         apu_reset();

         for (int i = 0; i < 0x16; i++)
            apu_write(0x4000 + i, buffer[i]);
      }


      /****************************************************/

      else if (memcmp(buffer, "INFO", 4) == 0)
      {
         MESSAGE_INFO("  - Found info block (%u bytes)\n", blockLength);

         _fread(buffer, 0x100);

         // We don't currently do anything with it, it's just to help report bugs to me :)
      }


      /****************************************************/

      else if (memcmp(buffer, "THMB", 4) == 0)
      {
         /* ThumbyNES extension block — see state_save for the layout
          * and rationale. Runs AFTER BASR in the save file so the
          * ppu_write calls during BASR-load (which re-derive some
          * internal fields from ctrl0/ctrl1) don't clobber our
          * authoritative values. */
         MESSAGE_INFO("  - Found ThumbyNES extension block (%u bytes)\n", blockLength);

         /* Defensive: only the current version layout is understood;
          * older saves won't have this block at all (they hit the
          * "unknown block" branch in original nofrendo readers and
          * get ignored). */
         uint32 want = 44;
         if (blockLength != want)
         {
            MESSAGE_ERROR("THMB: unexpected length %u (want %u), skipping\n",
                          blockLength, want);
            continue;
         }

         _fread(buffer, want);

         uint32 u32;
         int off = 0;

         machine->ppu->stat            = buffer[off++];
         machine->ppu->latch           = buffer[off++];
         machine->ppu->vdata_latch     = buffer[off++];
         machine->ppu->nametab_base    = buffer[off++];
         machine->ppu->vram_accessible = buffer[off++] ? true : false;

         memcpy(&u32, &buffer[off], 4); off += 4; machine->ppu->vaddr_latch     = (int)swap32(u32);
         memcpy(&u32, &buffer[off], 4); off += 4; machine->ppu->vaddr_inc       = (int)swap32(u32);
         memcpy(&u32, &buffer[off], 4); off += 4; machine->ppu->left_bg_counter = (int)swap32(u32);
         memcpy(&u32, &buffer[off], 4); off += 4; machine->ppu->strike_cycle    = swap32(u32);
         memcpy(&u32, &buffer[off], 4); off += 4; machine->ppu->scanline        = (int)swap32(u32);

         memcpy(&u32, &buffer[off], 4); off += 4; machine->scanline             = (int)swap32(u32);
         memcpy(&u32, &buffer[off], 4); off += 4; machine->cycles               = (int)swap32(u32);

         machine->apu->fc.state = buffer[off++];
         memcpy(&u32, &buffer[off], 4); off += 4; machine->apu->fc.cycles = swap32(u32);
         machine->apu->fc.irq_occurred = buffer[off++] ? true : false;

         machine->cpu->int_pending = buffer[off++] ? true : false;
      }


      /****************************************************/

      else
      {
         MESSAGE_ERROR("Found unknown block type!\n");
      }
   }

   /* close file, we're done */
   STATE_CLOSE(file);

   MESSAGE_INFO("state_load: Game restored\n");

   return 0;

_error:
   MESSAGE_ERROR("state_load: Load failed!\n");
   STATE_CLOSE(file);
   return -1;
}
