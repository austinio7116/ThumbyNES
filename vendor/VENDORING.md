# Vendored sources

## State save patches

Both `nofrendo/nes/state.c` and `smsplus/state.c` have been minimally
patched to route their `fopen` / `fwrite` / `fread` / `fseek` / `fclose`
calls through `device/thumby_state_bridge.[ch]` (a tiny FatFs-backed
shim) instead of libc stdio when compiled with `-DTHUMBY_STATE_BRIDGE`.
The host build is unaffected — without the macro both files build
against real stdio. The patch is a top-of-file `#ifdef` block that
defines `STATE_FILE` / `STATE_OPEN` / `STATE_WRITE` / etc., plus a
sed-style replacement of every `fopen` / `fwrite` / `fread` / `fseek`
/ `fclose` token to its `STATE_*` equivalent. The associated header
files were updated to declare the function signatures with the right
parameter type per build mode. Required so the in-game pause menu's
"Save state" / "Load state" actions can write the same `.sta`
sidecars across all three vendored cores.

## peanut_gb/

Game Boy (DMG) emulation core. Single-header library + minigb_apu
audio synthesis, vendored from the **TinyCircuits Tiny Game Engine**'s
GBEmu C user-module (which itself wraps upstream Peanut-GB and
MiniGBS / minigb_apu).

- Upstream: https://github.com/deltabeard/Peanut-GB (peanut_gb.h)
            https://github.com/baines/MiniGBS    (minigb_apu)
- Source-of-truth used: TinyCircuits-Tiny-Game-Engine `gbemu/`
- License: MIT (both libraries; see file headers)
- Files vendored: `peanut_gb.h`, `minigb_apu.h`, `minigb_apu.c`,
  `minigb_apu_impl.c` (the .c is a small shim that sets the audio
  format defines and includes the impl). **Zero patches** — both
  libraries port cleanly to standalone.

The MicroPython binding glue (`gb_emu_module.c` and the engine-coupled
`gb_emu_core.c`) was **not** vendored. We write our own thin wrapper
in `src/gb_core.[ch]` that mirrors the `nes_core` / `sms_core` shape.

## smsplus/

Sega Master System / Game Gear emulation core. Vendored from
**retro-go**'s `retro-core/components/smsplus` directory.

- Upstream: https://github.com/ducalex/retro-go
- Commit pinned: `4ced120669750ca7228fd0414211430c1d923166` (same as nofrendo)
- License: GPLv2 (see `smsplus/COPYING`)
- Original author: Charles MacDonald (1998-2007), with additional code
  from Eke-Eke (SMS Plus GX) and the retro-go maintainers.

The standalone fallback in `shared.h` (`#ifndef RETRO_GO`) makes
LOG_PRINTF degrade to printf and IRAM_ATTR to nothing.

### Patches applied

1. **`shared.h`** — wrapped the `#define IRAM_ATTR` fallback in
   `#ifndef` so the device build can override it with a platform-
   specific attribute via `-DIRAM_ATTR=...`. Mirrors the
   `nofrendo/nes/utils.h` patch. No effect on host builds (still
   expands to nothing). `cpu/z80.c`'s existing upstream
   `IRAM_ATTR int z80_execute(...)` then resolves to the Pico SDK
   section attribute on device, placing the Z80 dispatch loop in
   `.time_critical.sms` (SRAM) instead of XIP flash.

## nofrendo/

NES emulation core. Vendored from the **retro-go** project's `retro-core/components/nofrendo` directory.

- Upstream: https://github.com/ducalex/retro-go
- Commit pinned: `4ced120669750ca7228fd0414211430c1d923166`
- License: GPLv2 (see `nofrendo/COPYING`)
- Original author: Matthew Conte (1998-2000), with contributions from Neil Stevens and the retro-go maintainers.

We may make local modifications under `vendor/nofrendo/` to (a) remove the
retro-go OSD coupling (`rg_*` calls, `MESSAGE_INFO` macros), (b) replace
`FILE*` ROM/save loading with in-memory buffers so we can mmap from XIP
flash, and (c) tag hot functions with `__not_in_flash_func` for the device
build. Any such patches are listed below as they happen.

### Patches applied

1. **`nes/utils.h`** — wrapped `#define IRAM_ATTR` in `#ifndef` so the
   device build can override it with a platform-specific attribute via
   `-DIRAM_ATTR=...`. No effect on host builds (still expands to
   nothing).

2. **`nes/ppu.c`** — added `IRAM_ATTR` to `ppu_renderline()` so it
   joins `nes6502_execute()` (already tagged upstream) in being
   placed in `.time_critical.nes` on the device build.

   The Pico SDK linker script copies `.time_critical.*` into SRAM at
   boot, so the two hottest functions in the emulator run from RAM
   rather than XIP flash and avoid cache-miss stalls on the inner
   loops.

3. **`nes/state.c`** — added a `THMB` extension block to the save
   file format. The native SNSS format drops a pile of runtime state
   (PPU `stat` / `latch` / `vdata_latch` / `vaddr_latch` / `vaddr_inc`
   / `nametab_base` / `left_bg_counter` / `vram_accessible` /
   `strike_cycle` / `scanline`, NES-level `scanline` / `cycles`, APU
   frame counter `fc.state` / `fc.cycles` / `fc.irq_occurred`, CPU
   `int_pending`). For mapper-1/4 carts these are incidentally
   corrected as a side effect of MPRD's mapper bank restore, but
   NROM has no MPRD block so the game boots into a limbo state and
   hangs (observed with SMB — see the diagnostic trail in state.c
   comments).

   `THMB` is 44 data bytes, version 1, big-endian multi-byte fields
   matching SNSS convention. The save path writes it unconditionally
   after MPRD; the load path handles it in the usual if/else block
   chain and skips it gracefully on unexpected length. Older readers
   without the patch fall through to the "unknown block type" branch
   and ignore it — forward and backward compatible with the format
   itself; however, old `.sta` files made before the patch still
   hang SMB on load because they don't contain the missing state.
   Re-save on the patched firmware to get working saves.
