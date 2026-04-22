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

## picodrive/

Sega Mega Drive / Genesis emulation core. Vendored from notaz's
standalone **PicoDrive** tree (not the libretro packaging).

- Upstream: https://github.com/notaz/picodrive
- Commit pinned: `dd762b861ecadf5ddd5fb03e9ca1db6707b54fbb` (HEAD at
  import time, 2026-04-21)
- License: LGPLv2 (see `picodrive/COPYING`) — combines with our GPLv2
- Original author: Dave (2004) + notaz (2006-) + irixxxx (2020-)

Trimmed from the upstream tree (not vendored here):

- `pico/32x/*` — 32X support (build with `-DNO_32X`)
- `cpu/cyclone/*` — ARM-asm 68K core (we use the `cpu/fame/` C core
  via `-DEMU_F68K`; Cyclone is not M33-compatible and would need a
  full port)
- `cpu/DrZ80/*` — ARM-asm Z80 core (we use `cpu/cz80/` via `-D_USE_CZ80`)
- `cpu/sh2/` except `sh2.h` — SH2 core used only by 32X. The header
  is kept because `pico_int.h` includes it unconditionally
- `pico/cd/libchdr/` — CHD/ZSTD/LZMA decoders, gated on `USE_LIBCHDR`
  which we don't set; CD is runtime-disabled
- `pico/*_arm.{s,S}`, `pico/sound/*_arm.S`, `pico/cd/*_arm.*` — all
  ARM / MIPS hand-asm paths
- `platform/*` — libpicofe + libretro + per-device frontends. We
  provide our own stubs in `thumby_platform.c`

Retained for compile-time coverage even though they never execute at
runtime (behind PAHW-gated runtime branches):

- `pico/sms.c`, `pico/mode4.c` — PicoDrive's built-in SMS renderer.
  Four link-level symbols (`PicoResetMS`, `PicoPowerMS`, `PicoMemSetupMS`,
  `PicoFrameMS`) would have to be stubbed if excluded. We already use
  smsplus for SMS/GG so PAHW_SMS is never set here
- `pico/cd/*.c` — CD support. No `NO_CD` flag upstream; runtime stays
  silent because PAHW_MCD never gets set

`thumby_platform.c` provides our implementations of `plat_mmap` /
`plat_munmap` / `plat_mremap` (malloc-backed on host, flash-aware on
device — to be written at Phase 3), `plat_mem_get_for_drc` (NULL, we
use the interpreter core), `cache_flush_d_inval_i` (no-op), the three
MCD `mp3_*` stubs, `emu_video_mode_change` (weak no-op, frontend
overrides), `emu_32x_startup` + `p32x_bios_*` (never reached with
NO_32X), and link-level stubs for the 32X memory-map hooks.

### Patches applied

1. **`pico/state.c`** — three `#ifndef NO_32X` guards around
   `p32x_event_times` accesses and `Pico32xStateLoaded(arg)` calls.
   The upstream NO_32X macro version of `Pico32xStateLoaded()` is
   arity-0, so the load-time calls that pass `0`/`1` fail to compile
   without guarding.

2. **`pico/draw.c`** — one `#ifndef NO_32X` guard around the
   `PicoScan32xBegin` / `PicoScan32xEnd` assignment block inside
   `PicoDrawSetCallbacks`. The NO_32X build doesn't declare the
   symbols. `PicoScanBegin` / `PicoScanEnd` are always set.

3. **`pico/sound/ym2612.{c,h}`** — the 213 KB `ym_tl_tab` and 6.6 KB
   `ym_tl_tab2` log tables were file-scope arrays (BSS). They now live
   as pointers, `calloc`'d on first `init_tables()` call. Added a new
   `YM2612Shutdown_()` that frees them and resets the init guard.
   Called from `PsndExit()` (see patch 4 below). Moves 219 KB of BSS
   to the heap so the MD core doesn't keep that resident when a
   sibling core is active.

4. **`pico/sound/sound.c`** — one-liner: `PsndExit()` now calls
   `YM2612Shutdown_()` to drop the heap-allocated log tables on core
   teardown.

5. **`pico/pico_int.h` + `pico/pico.c` + `pico/draw2.c`** — `PicoMem`
   (140 KB of VRAM / CRAM / VSRAM / zram / ioports) moved from BSS to
   the heap. Approach:
     - Renamed the struct tag from `struct PicoMem` to `struct
       PicoMemMap` (4 sites) so it doesn't collide with the macro.
     - Added `extern struct PicoMemMap *PicoMem_ptr;` and
       `#define PicoMem (*PicoMem_ptr)` in `pico_int.h` so all 170+
       existing `PicoMem.foo` / `&PicoMem` / `sizeof(PicoMem)` access
       sites keep working unchanged.
     - `PicoInit()` lazy-allocs, `PicoExit()` frees.
     - `pico/draw2.c`'s file-scope initializer
       `unsigned short *PicoCramHigh = PicoMem.cram;` moved into
       `PicoDraw2Init()` since `PicoMem.cram` is no longer a constant
       expression.

6. **`cpu/drc/cmn.c` excluded from build** (CMake `add_library`) — it
   declared a 4 MB static `tcache_default[]` for dynamic recompilation.
   We build no DRC (no SH2 DRC, no SVP DRC, no Cyclone), so the only
   reference to `drc_cmn_cleanup` is under `_SVP_DRC` in `svp.c` which
   we don't define. 4 MB of dead BSS gone.

7. **`pico/cd/*.c` excluded from build + MCD stubs in
   `thumby_platform.c`** — Mega-CD support carried ~15 KB of
   unconditionally-resident BSS (`cdd` 4.8K, `Pico_msd` 2.1K,
   `PicoCpuFS68k` 2.2K, `s68k_*_map` ×3 = 6K) that never executes
   (PAHW_MCD never set). The CD sources now don't compile; stubs in
   `thumby_platform.c` stand in for the ~15 entry points reached by
   runtime-gated call sites in `pico.c`/`cart.c`/`state.c`/
   `videoport.c`/`sound.c`/`debug.c`. The three remaining MCD-only
   globals (`PicoCpuFS68k`, `Pico_msd`, `Pico_mcd`) are declared as
   1-byte stubs rather than full-sized structs — compiled code takes
   their addresses inside dead branches but never dereferences them.

8. **`pico/sound/sound.c` + `pico/pico_int.h`** — `cdda_out_buffer`
   (4.6 KB CDDA mix buffer) moved from an array to a pointer, left
   NULL in MD-only builds. The one unconditional `memset` in
   `PsndRerate` now checks for NULL. Header extern updated to match.

Patches 1-5, 7, 8 are all top-of-block-only, document-the-fix style —
no logic changes. Look for `/* ThumbyNES: ... */` markers to find them.
Combined impact on static BSS: **~4.4 MB → ~26 KB**, of which ~353 KB
is reclaimable SRAM on device (the 4 MB tcache was always dead).

**Remaining ~26 KB is genuinely hot MD state** (68K context, VDP state,
memory maps, YM2612 state, Z80 state, sprite line cache). Pointer-
redirecting these would add a dereference per memory access — wrong
trade-off. The device build plan is to put all of `libpicodrive.a`'s
BSS into a named linker section that overlaps with the other emulator
cores' sections, so the combined firmware pays 0 extra resident bytes
when MD isn't the active core.

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
