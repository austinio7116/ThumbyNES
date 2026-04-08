# Vendored sources

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
