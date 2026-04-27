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
trade-off. The device build keeps MD's BSS live only while MD is the
active core; sibling cores (NES/SMS/GB) reuse the heap once MD shuts
down.

#### Further patches for performance + device correctness

9. **`cpu/cz80/cz80.c` + `cpu/cz80/cz80macro.h`** — CZ80's Z80 function-
    pointer memory map packs handler addresses as `(addr >> 1) | MAP_FLAG`.
    On ARMv8-M Thumb-2 (Cortex-M33) a function pointer must have bit 0
    set as the Thumb indicator; the shift-and-mask round-trip discards
    it, and calling the reconstructed pointer bus-faults. Added a
    `CZ80_MAP_FP(v)` / `CZ80_WR_MAP_FP(v)` macro that OR-s bit 0 back
    at dispatch under `__thumb__`. Without this patch, Cz80 wedges on
    the first Z80→YM2612 / Z80→SN76489 write of every cart (Sonic 2
    hung in the sound-driver-upload wait loop, boot crash on others).
    Same fix pattern as `cpu68k_map`'s `MAP_FP` in `pico/memory.h`.

10. **`pico/cart.c`** — call-site change in `PicoCartInsert`: the
    `*(u32 *)(rom + romsize) = CPU_BE2(0x6000FFFE)` "runaway safety"
    opcode write is skipped when `PicoCartSuppressSafetyOp = 1`. Set
    from our `mdc_load_rom_xip()` — the XIP path borrows a flash
    pointer that can't be written, and on some RP2350 QMI
    configurations an unaligned write to XIP flash bus-faults into
    HardFault. Guarding the write lets Sonic 3 (and several other
    carts) boot from XIP without a bus fault. The safety opcode is
    only ever fetched by 68K runaway execution which well-behaved
    carts never do.

11. **`pico/videoport.c`** — VRAM DMA source reads. PicoDrive's
    `DmaSlow` reads source words via `base[(source+i) & mask]` which
    assumes native-u16 source. Under `FAME_BIG_ENDIAN` our ROM stays
    raw-BE; RAM sources stay native. Added a runtime `src_is_rom`
    check (`base` inside `[Pico.rom, Pico.rom + Pico.romsize)`) and a
    `DMA_READ16` macro that bswaps only when source is ROM. Both the
    per-word `for (;len;--)` inner loop and the fast `memcpy` short-
    circuit have `src_is_rom` branches; `PicoDrawBgcDMA` stashes the
    same flag into `BgcDMA_is_rom` for the deferred scanline copy.
    Without this, all ROM→VRAM tile-load DMAs land byte-swapped and
    the screen fills with nonsense.

12. **`pico/draw.c`** — per-scanline diag counters (`md_dbg_*`) wired
    at the top of `FinalizeLine555` and around the SONIC-mode palette
    update so the device runner's overlay can confirm rendering
    actually advances during hangs / black-screens. `DrawLineDest`
    sentinel switched from `DrawLineDestIncrement==0` to
    `DrawLineDestBase==DefOutBuff` — our `MD_LINE_SCRATCH` mode sets
    `increment=0` deliberately so every scanline overwrites a shared
    640 B scratch, and the upstream "no user buffer" short-circuit
    would skip our output. Small one-liner.

13. **`cpu/cz80/cz80.c`** `Cz80_Set_IRQ` behaviour under `MD_DUAL_CORE`
    — the upstream guards `CZ80_HAS_INT` behind `if (zIFF1)` at assert
    time, meaning an IRQ asserted while Z80 interrupts are masked is
    dropped entirely. That works for single-core where IRQs are
    asserted in lockstep with Z80 execution, but breaks dual-core
    where core0 asserts VINT asynchronously. Under `MD_DUAL_CORE` the
    patch always sets `CZ80_HAS_INT` on assert; `CHECK_INT` still
    gates on `zIFF1` at dispatch time, matching real-hardware
    level-sensitive behaviour. Only active in dual-core builds (which
    are currently a parked WIP branch — see `ThumbyNES/dualboot.md`
    and the `dual-core-wip` branch).

14. **`pico/sound/sound.c`** — split `PsndGetSamples` into an inner
    `PsndGetSamples_body(int y)` with the original render+writeSound+
    PsndClear logic, and an outer `PsndGetSamples(int y)` wrapper
    that under `MD_DUAL_CORE` dispatches the body to core1 via our
    `md_dc_request_render` queue instead of running it on core0.
    Single-core builds collapse the wrapper to just calling the body
    directly (no runtime branch). Same pattern applied to `PsndDoFM`
    / `PsndDoPSG` / `PsndDoDAC` — early-return on core0 under dual-
    core so the whole sound path runs on core1. Currently only active
    on the WIP dual-core branch.

15. **`pico/pico_int.h`** — under `MD_DUAL_CORE`, `z80_int_assert()`
    macro uses `HOLD_LINE` (auto-clear after Z80 services the IRQ)
    on assert and no-ops the explicit clear. Upstream uses
    `ASSERT_LINE` + `CLEAR_LINE` with the assumption that Z80
    execution is in lockstep with the assert/clear pair. Dual-core
    breaks that assumption. Paired with patch 13 above. Dual-core
    WIP only.

#### Build-time generated tables (moved to flash via `-DXXX_IN_FLASH`)

Three CPU/sound tables are huge enough to matter for BSS even after
the heap-allocation work above. They're now **precomputed at build
time by host-side generators** in `tools/`, emitted as `const u8[]`
C files linked into flash:

- `FAME_JUMPTABLE_IN_FLASH` — the 256 KB FAME 68K opcode jumptable
  (`pico/cpu/fame/famec.c`'s `JumpTable_ro[0x10000]`). Generated from
  `tools/gen_md_fame_jumptable.c` via a `#include
  "famec_jumptable_data.inc"` designated-initialiser dump. Without
  this the table is built at every `PicoInit()` by walking the
  `opcode_table[]` parser, burning ~220 KB of heap at session start.

- `YM2612_TABLES_IN_FLASH` — 213 KB of YM2612 log-sine + DAC tables
  (`ym_tl_tab` 213 K, `ym_tl_tab2` 6.6 K). Generator
  `tools/gen_md_ym2612_tab.c`. Previously computed at `YM2612Init_()`
  on heap.

- `CZ80_SZHVC_IN_FLASH` — 256 KB of cz80 Z80 flag lookup tables
  (`SZHVC_add`, `SZHVC_sub`, 128 KB each). Generator
  `tools/gen_md_cz80_szhvc.c`. Previously computed at `Cz80_Init()`
  on heap.

All three generators run on the host compiler (`cc -O2`) during the
CMake build. Output `.c` files land in `${CMAKE_BINARY_DIR}` and are
added to the `picodrive` target's sources. Total flash cost: ~730 KB
(this is why ThumbyNES outgrew the 1 MB ThumbyOne NES partition and
needed `THUMBYONE_WITH_MD=ON` to grow it to 2 MB — see
`ThumbyOne/common/slot_layout.h`).

#### Dynamic IRAM for the Z80 dispatch loop

`Cz80_Exec` (the hot 17 KB Z80 opcode dispatch loop) is the single
hottest function in the emulator — called ~262× per scanline × 224
scanlines per frame, reading opcode bytes from Z80 RAM and dispatching
through a 256-entry jumptable. Running it from XIP flash thrashed the
16 KB XIP cache on heavier carts; moving it to SRAM reclaimed ~2-3 ms
/ frame on Sonic 2 but a static `.time_critical.md` placement would
steal 17 KB permanently across all four emulator cores' BSS. So:

16. **Custom flash section `.md_iram_pool`** in `device/md_memmap.ld`
    (forked from pico-sdk's `memmap_default.ld`). `Cz80_Exec` is tagged
    with `__attribute__((section(".md_iram_pool.Cz80_Exec")))` so it
    lands in this section, adjacent to the pool start/end symbols
    exported by the linker script.

17. **`-Wl,--wrap=Cz80_Exec`** at link time. The linker renames direct
    calls to `Cz80_Exec` (from macros in `pico_int.h` expanded at
    `PicoSyncZ80` call sites) into `__wrap_Cz80_Exec`. Our thunk in
    `device/md_iram.c` dispatches through a function pointer that's
    initialised to `__real_Cz80_Exec` (flash) and repointed to a
    heap-resident copy after `mdc_init`.

18. **`md_iram_init()`** in `device/md_iram.c` (device-only) —
    `malloc`s 17 KB from heap, `memcpy`s the pool bytes from flash,
    issues a `dsb; isb` pair, and updates the function pointer to
    `(heap_addr + offset) | 1` (Thumb bit). `md_iram_shutdown()`
    resets the pointer to `__real_Cz80_Exec` first (in case of
    re-entrant calls during teardown) and `free`s the buffer.

This only costs heap while MD is the active emulator. Other cores
(NES/SMS/GB) get the 17 KB back during their sessions. Cz80_Exec is
a "perfect leaf" from a linker-relocation perspective — it has zero
direct `bl` instructions (everything internal is macro-expanded or
goes through `CPU->` function pointers), so the memcpy'd copy needs
no relocation fixup.

Attempted extending the pool to `YM2612UpdateOne_` + `chan_render` +
`update_lfo_phase` + `memset32` (another 11 KB) but hit a consistent
+8 ms/frame regression (k=25 full-skip lock). Hypothesis unproven;
rolled back. Pool stays at just `Cz80_Exec`.

#### Runtime audio modes + adaptive frame pacing

Not strictly vendor patches, but worth documenting alongside the
above because they shape the runtime behaviour of the emulator:

- `mdc_init(sample_rate)` honours `0` / `11025` / `22050`:
    - `22050` (default, FULL) — YM2612 + PSG + Z80 all active.
    - `11025` (HALF) — halves FM synthesis cost; `mdc_audio_pull`
      zero-order-hold upsamples to 22050 for the PWM path.
    - `0` (OFF) — strips `POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80`
      from `PicoIn.opt`. Locks 50 PAL / 60 NTSC with zero audio path.
  Runtime menu item in `device/md_run.c` chooses between them per-
  cart (`md_cfg_t.audio_mode` → CFG_MAGIC `MDEX`).

- `mdc_set_skip_render(int)` → sets `PicoIn.skipFrame`. Called each
  frame in `device/md_run.c` from an adaptive catch-up heuristic: if
  last frame's emulation time exceeded `FRAME_US`, skip VDP render
  on this one. Caps at 2 consecutive skips. Saves ~6-8 ms on skipped
  frames; locks 50 PAL on heavy Sonic 2 action scenes.

- `md_core_rebuild_sx_lut()` in `src/md_core.c` — per-dx source-column
  LUT for the line-scratch downsample, keyed on `(s_vw, s_vh,
  scale_mode)`. Replaces ~57 k integer divs / frame. Also implements
  the **aspect-preserving FILL mode** (scale-by-height, crop X sides)
  which matches SMS's FILL behaviour; previous FILL was an axis-
  independent stretch that squashed MD sprites horizontally.

19. **`pico/state.c`** — state_save / state_load's first heap op is
    `buf2 = malloc(CHUNK_LIMIT_W)` (18 KB). That deadlocked inside
    newlib's malloc mid-cart on the device — heap fragmentation plus
    the allocator mutex combined into a hang rather than a NULL
    return. Patched both functions to reuse an externally-provided
    scratch pointer (`mdc_state_scratch`) when non-NULL, falling back
    to `malloc` only when the pointer is missing (host build, or if
    the init-time alloc failed). Matching `free()` calls at both `out:`
    labels skip the static buffer. `src/md_core.c` provisions a 4 KB
    scratch in `mdc_init` — big enough for every serializer we
    actually use (YM2612 ~800 B is the peak) and small enough not to
    squeeze out PicoInit or cart load.

20. **`cpu/fame/fame.h` + `cpu/fame/famec.c` + `pico/memory.c`** —
    per-bank fetch endianness for FAME's PC pipeline. ROM is stored
    raw big-endian (XIP from flash on device, malloc'd buffer on
    host); WRAM is stored host-native u16 layout (so 68K data reads
    via `MAKE_68K_ROM_READ16` are a single native u16 access). FAME's
    fetch macros (`FETCH_WORD` / `FETCH_LONG` / `FETCH_SWORD` /
    `FETCH_BYTE` / `FETCH_SBYTE` / `GET_SWORD` / `DECODE_EXT_WORD`)
    used to unconditionally bswap under `FAME_BIG_ENDIAN` — correct
    for ROM, silently wrong for WRAM. Carts that JSR into a
    RAM-resident routine got the wrong opcode (Xenon 2's JMP table
    read as F-line exceptions → vector to header → hang) or the
    wrong immediate byte (Brian Lara's `BTST #1, (A5)` read as
    `BTST #0, (A5)` — bit 0 = SR_PAL = always 1 in PAL mode → DMA
    poll never exits).

    Fix: `M68K_CONTEXT` carries a parallel `FetchSwap[256]`
    populated at `cpu68k_map_set` time (1 = raw-BE source, 0 =
    native-host source). The direct-to-Fetch[] write in
    `PicoMemSetup` for the ROM mapping (under `FAME_BIG_ENDIAN`)
    sets FetchSwap[i]=1 for the same banks. `SET_PC` caches the
    per-bank flag into `fetch_swap_now` once per rebase. The fetch
    macros branch on the cached flag in the hot path. Cost: +256
    bytes BSS in `PicoCpuFM68k`, one byte-load + one branch per
    opcode dispatch; the branch predictor pins it (ROM is 99%+ of
    fetches) so steady-state cost is effectively zero.

    Test set went from 27/41 → 41/41 OK on 1 MB MD ROMs in our
    EMUBackup/ corpus. No host/device divergence — FAME_BIG_ENDIAN
    is set on both LE-relative-to-68K targets.

21. **`pico/memory.c`** — `padTHLatency[3]` and `padTLLatency[3]` are
    file-scope statics that store absolute `SekCyclesDone()` values
    (set when the cart writes the pad control register and consumed
    in `port_read` via `CYCLES_GE`). PicoInit memsets struct Pico so
    SekCyclesDone restarts at ~0 on each cart load — but these
    statics keep the previous cart's huge cycle counts.
    `CYCLES_GE(0, 999999)` then evaluates as signed-false, so
    `port_read` flips bit 4 of the pad input via `in ^= 0x10`. Bit
    4 happens to be MD pad **B** when TH=1, so the cart sees B as
    held until SekCyclesDone catches up — Cannon Fodder fires
    constantly, Sonic ignores the first jump press. New
    `PicoMemReset()` clears both arrays + defensively resets
    `port_readers[]` to `{3btn, 3btn, nothing}`. Called from
    `mdc_init` after `PicoInit`.

    Bug only manifests on second-and-later cart loads within the
    same slot session (picker → cart → picker → cart). A full slot
    reload (cart → in-game-menu → "Back to lobby" → re-enter slot)
    re-zeros BSS at chain-image boot, masking the bug — which is
    why fresh-boot launches always work.

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


## huexpress/

PC Engine / TurboGrafx-16 HuCard emulation core. Vendored from the
ODROID-GO port of Hu-Go! / HuExpress (fifth core after nofrendo,
smsplus, peanut_gb, and picodrive).

- Upstream: https://github.com/pelle7/odroid-go-pcengine-huexpress
- Commit pinned: `d6a84fd2928aaf704668ad2bee94e93c120d74e2`
- Original author: HuExpress / Hu-Go! authors (Zeograd et al.),
  ESP-IDF port by pelle7 et al.
- License: GPLv2 (see `huexpress/COPYING`)

Only the core emulation is vendored. The following upstream
directories / files were **not** imported:

- `pcengine-go/main/` — ODROID-GO main loop + ESP-IDF glue
- `pcengine-go/components/odroid-go-common/` — ODROID hardware layer
- `pcengine-go/components/huexpress/PCEngine.cpp`, `huexpress.cpp`,
  `huexpressd.c`, `huexpress.rdef`, `SConscript`, `utils.c`,
  `view_inf.c`, `view_zp.c`, `osd_*_cd.c`, `osd_sdl_*.c`,
  `osd_keyboard.c`, `iniconfig.c`, `zipmgr.c` — SDL / Haiku / BSD /
  Linux frontends, INI config, zip loader, netplay, keyboard
  bindings, Linux CD-ROM driver, CD audio mixer
- `engine/cd.c`, `pcecd.c`, `hcd.c`, `lsmp3.c`, `ogglength.c` — CD
  emulation, MP3 decoder, OGG length helper (CD is out of scope for
  v1; see PCE_PLAN.md §10)
- `engine/debug.c`, `dis.c`, `bp.c`, `cheat.c`, `edit_ram.c` —
  in-core debugger, disassembler, breakpoint manager, memory editor
- `engine/trans_fx.c`, `subs_eagle.c` — desktop scaler effects
- `includes/PCEngine.h`, `globals.h`, `huexpressd.h`, `iniconfig.h`,
  `interf.h`, `osd_*.h`, `utils.h`, `view_*.h`, `zipmgr.h` —
  matching headers for removed .c files

### Patches applied

1. **`includes/myadd.h`** — guarded the two ESP-IDF
   `#include "esp_system.h"` / `#include "../../odroid/odroid_debug.h"`
   lines behind `#ifdef THUMBY_BUILD`, pulling
   `thumby_platform.h` instead. The feature flags and V3 inline set
   are unchanged. Also guarded `MY_GFX_AS_TASK` / `MY_SND_AS_TASK`
   (which assume FreeRTOS) behind the same macro.

2. **`includes/thumby_platform.h`** — new file that defines the
   ESP-IDF placement attributes (`DRAM_ATTR`, `IRAM_ATTR`,
   `WORD_ALIGNED_ATTR`, `IROM_ATTR`, `EXT_RAM_ATTR`) as no-ops,
   stubs `QueueHandle_t` + task handles to `void*`, makes the
   `odroid_debug_perf_*` macros no-ops, and provides inline
   `htons`/`ntohs`/`htonl`/`ntohl` so the engine doesn't need
   `<netinet/in.h>`. Mirrors the `thumby_platform.c/h` pattern used
   by picodrive.

3. **`engine/sys_dep.h`** — wrapped `#include <netinet/in.h>` in
   `#ifndef THUMBY_BUILD`. The device toolchain has no BSD sockets.

4. **`engine/pce.c`** — `Track CD_track[0x100]` (~74 KB BSS) wrapped
   in `#ifndef PCE_HUCARD_ONLY`. HuCard-only builds have no CD track
   table, so the array is pure waste. The CD code paths that
   reference it are dead when `CD_emulation != 1` and no callers run
   them from the HuCard boot path.

5. **`engine/hard_pce.c`** — `hard_pce->PCM` allocation (64 KB for
   CD ADPCM playback) reduced to a 4-byte placeholder under
   `PCE_HUCARD_ONLY`, mirroring the idiom the ODROID-GO port
   already applied to `ac_extra_mem` and `cd_extra_mem`. The pointer
   stays non-NULL so unused CD fallback paths don't deref NULL.

### Pending audit (1.08 release)

Both items previously listed as Pending work shipped in 1.08:

- **Scanline render mode** — implemented; the core now compiles
  under `PCE_SCANLINE_RENDER` and routes through our own
  `src/pce_render.c` per-scanline compositor (~600 bytes of state,
  vs the 568 KB of upstream scratch the full-frame path needed).
  See the [v1.08 changelog in the main README](../README.md#v108--pc-engine--turbograf16-tab-strip-facelift-lcd-reliability)
  for the pipeline overview.
- **Save-state serialisation** — implemented as the `THPE` format
  (magic + `hard_pce` + RAM + VRAM + Pal + SPRAM + IO blocks),
  written directly through FatFs rather than through
  `thumby_state_bridge.h` (HuExpress had no upstream `state.c` to
  bridge).

Beyond patches 1–5 above, getting PCE running on real carts and
coexisting with the other five cores in ThumbyOne required further
modifications to the vendored tree (region detect, joypad-port
read wiring, HuCard strict-mode abort softening, mix-path audio
tuning, leak fixes for six-core coexistence). The full
patch-by-patch enumeration matching MD's catalogue style is a TODO
for a future audit pass — see the v1.08 changelog and the commit
messages on `pce_core.c`, `pce_run.c`, and the modified files
under `vendor/huexpress/engine/` for the technical detail.
