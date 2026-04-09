# Vendored sources

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
LOG_PRINTF degrade to printf and IRAM_ATTR to nothing — verbatim
vendor with **no patches** as of the initial drop.

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
