# ThumbyNES — NES Emulator Firmware for Thumby Color

A standalone bare-metal firmware (mirroring the **ThumbyP8** model) that turns
the Thumby Color into a pocket NES. Flash one `.uf2`, drag `.nes` ROMs onto
the device over USB MSC, pick one from a cart picker, play with sound + LCD +
buttons.

This document is the **plan**. No code yet — decisions, trade-offs, vendor
choices, and a phased build order.

---

## 1. Hardware budget recap (Thumby Color)

| Resource | Amount | NES emulator implication |
|---|---|---|
| MCU | RP2350 dual-core Cortex-M33 @ up to 300 MHz | Core0 = CPU+PPU, Core1 = APU+audio DMA feed (same split as P8/GBEmu) |
| SRAM | 520 KB | Tight but workable. NES needs ~10 KB internal + framebuffer + mapper RAM |
| Flash | 16 MB (≈12 MB usable as FAT after firmware) | Plenty for dozens of ROMs (avg NROM/MMC1/MMC3 = 32–512 KB) |
| Display | 128×128 RGB565, GC9107, DMA SPI | NES is **256×240** — must scale/crop. See §4 |
| Audio | 9-bit PWM + sample IRQ (proven by P8) | Run APU mixer at 22050 Hz mono, same path as P8 |
| Input | A, B, D-pad, LB, RB, MENU | Maps cleanly onto NES (A, B, Start=MENU, Select=LB, RB=fast-fwd or menu) |
| USB | TinyUSB MSC class (proven by P8) | Drag-and-drop `.nes` files onto a FAT volume |

**Headline constraints**:
1. **Screen aspect / resolution mismatch.** NES = 256×240. Thumby = 128×128.
   Horizontally we must downscale 2:1 (every other pixel) or letterbox a
   subwindow. Vertically NES is 240 vs 128 — scaled 2:1 = 120 lines, leaving
   8 px of letterbox or 8 px crop. See §4 for the recommended path.
2. **CPU budget.** RP2350 @ 300 MHz can comfortably run a cycle-accurate 6502
   plus a scanline-accurate PPU for **most NROM/MMC1/MMC3** games at 60 fps.
   Sub-cycle PPU accuracy (sprite-0 hit edge cases, MMC5 scanline IRQ) may
   drop frames on heavy mappers — accept that scope.
3. **RAM.** A second 240×256 working framebuffer would cost 120 KB in RGB565.
   We render **directly into a 128×128 RGB565 buffer** as scanlines come out
   of the PPU, applying the horizontal 2:1 downscale in-line. One scanline
   buffer (256 bytes for the 8bpp NES indices) is enough.

---

## 2. Vendor choice: which NES core?

We are not writing a 6502 from scratch. Candidates, ranked by fitness for
this project:

| Core | Lang | Size | Accuracy | License | Notes |
|---|---|---|---|---|---|
| **InfoNES** | C | ~5k LOC | Medium (NROM/MMC1/MMC3 solid) | "free for non-commercial" — **license-incompatible**, skip |
| **Nofrendo** (Nesticle-derived, used in ESP32 retro-go) | C | ~15k LOC | Medium-High | GPLv2 | Already proven on ESP32 / RP2040 ports — strongest precedent |
| **fceumm** (libretro fork of FCE Ultra) | C | ~80k LOC | High | GPLv2 | Heavy. Many mappers. Likely too big / too slow without surgery |
| **NoftendoNES / smolnes / nesalizer** | C | varies | Low–Medium | various | Hobby cores, fun but not battle-tested |
| **Mesen / puNES** | C++ | huge | Highest | GPLv3 | Not realistic on M33 |

**Recommendation: vendor Nofrendo** (the retro-go variant).
- Already ported to RP2040 in the wild (`pico-infonez`, `pico-rgb-keypad-nes`,
  `MCUME`) so we have prior art for the MCU-side glue.
- Clean separation of CPU / PPU / APU / mapper / I/O — easy to wire to our
  LCD and PWM audio.
- GPLv2 — compatible with our distribution model as long as we ship source.
- Mapper coverage out of the box: 0 (NROM), 1 (MMC1), 2 (UNROM), 3 (CNROM),
  4 (MMC3), 7 (AxROM), 9, 11, 66 — covers the vast majority of the library.

**Fallback**: if Nofrendo proves too RAM-hungry, the second pick is **InfoNES
+ a clean-room rewrite of any non-free header comments**, since it has the
smallest footprint and is known to fit in 64 KB on Cortex-M0+. We will only
fall back if Phase 2 benchmarking forces it.

**License hygiene**: GPLv2 vendored code lives in `vendor/nofrendo/` with the
upstream `LICENSE` and a `VENDORING.md` recording the exact commit. The
firmware glue we write is also GPLv2 to keep the combined work clean.

---

## 3. Repository layout (mirrors ThumbyP8)

```
ThumbyNES/
├── PLAN.md                    ← this file
├── README.md                  ← user-facing once Phase 1 lands
├── CMakeLists.txt             ← host build (SDL2 runner + bench)
├── vendor/
│   └── nofrendo/              ← vendored NES core, GPLv2
├── src/                       ← cross-platform glue
│   ├── nes_core.[ch]          ← thin wrapper around nofrendo entry points
│   ├── nes_video.[ch]         ← NES 256×240 → Thumby 128×128 scaler
│   ├── nes_audio.[ch]         ← APU sample buffer → 22050 Hz mono ring
│   ├── nes_input.[ch]         ← Thumby buttons → NES controller bits
│   ├── nes_rom.[ch]           ← iNES (.nes) header parse + PRG/CHR mapping
│   ├── nes_save.[ch]          ← battery SRAM persistence (per-ROM .sav)
│   ├── host_main.c            ← SDL2 host runner (dev iteration)
│   └── bench_main.c           ← frame-time bench harness
├── device/                    ← device-only firmware glue (lift from P8)
│   ├── CMakeLists.txt         ← Pico SDK build
│   ├── nes_device_main.c      ← entry, lobby/picker/ROM state machine
│   ├── nes_lcd_gc9107.[ch]    ← reuse P8 driver verbatim
│   ├── nes_buttons.[ch]       ← reuse P8 driver
│   ├── nes_audio_pwm.[ch]     ← reuse P8 PWM driver
│   ├── nes_flash_disk.[ch]    ← reuse P8 flash FAT disk
│   ├── nes_msc.c              ← reuse P8 TinyUSB MSC
│   ├── usb_descriptors.c      ← reuse, retag VID/PID strings
│   ├── tusb_config.h          ← reuse
│   ├── nes_picker.[ch]        ← ROM picker (text list — no per-ROM thumbnail)
│   ├── nes_log.[ch]           ← reuse
│   └── fatfs/                 ← reuse
└── tools/
    └── ines_inspect.py        ← optional dev helper to dump iNES headers
```

The strategy is: **steal the entire device/ layer from ThumbyP8 unchanged**,
swap the runtime core, swap the picker contents, retag USB strings. That
halves the work and reuses code that is already known to work on hardware.

---

## 4. Video pipeline — the central design problem

NES native = 256×240. Thumby = 128×128. Options:

### Option A — 2:1 nearest-neighbor downscale, vertical letterbox (chosen)
- X: drop every other pixel → 128 wide.
- Y: drop every other line → 120 tall, center it with 4 px black bars top/bottom.
- Pros: trivially fast (1 mul-free shift per pixel), preserves aspect, every
  NES pixel is sampled.
- Cons: thin 1-px UI elements in some games disappear on the dropped column/line.
- Implementation: PPU emits one 256-byte palette-index scanline; we keep only
  even scanlines, and on those scanlines we pack `dst[x] = palette[src[2*x]]`
  straight into the RGB565 framebuffer. Zero extra buffers.

### Option B — bilinear downscale to 128×120
Better visual fidelity, ~3× the per-pixel work, may push Core0 over budget on
mapper-heavy titles. Reserve as a Phase 6 polish toggle.

### Option C — 1:1 crop to a 128×128 viewport with horizontal pan
Some games (Mario, Mega Man) have important UI on the screen edges; panning
to follow the player is painful. Reject as default, keep as a per-ROM override.

**Decision**: ship Option A. Add Option B as a runtime toggle in Phase 6.

---

## 5. Audio pipeline

- Reuse `p8_audio_pwm.[ch]` verbatim — sample-rate IRQ already feeds 9-bit
  PWM at 22050 Hz from a ring buffer.
- Nofrendo's APU emits signed 16-bit mono samples at a configurable rate.
  Ask it for 22050 Hz, convert s16 → 9-bit unsigned in the IRQ, push into the
  same ring the P8 mixer uses.
- Core1 runs the APU sample generator on a tight loop and refills the ring.
  Core0 stays focused on CPU+PPU+rendering.

---

## 6. Input mapping

| Thumby button | NES |
|---|---|
| A | A |
| B | B |
| D-pad | D-pad |
| LB | Select |
| MENU | Start |
| RB | Hold = 4× fast-forward (uncap frame rate) |
| MENU long-press | Return to ROM picker |

Diagonal coalescing already lives in `p8_buttons.c` — keep it.

---

## 7. ROM loading & storage

- ROMs land on the FAT volume as plain `.nes` files dragged in over USB MSC.
- Picker scans the root for `*.nes`, sorts alphabetically, shows file name +
  size + mapper number (parsed from iNES header on the fly).
- On select: mmap the file from flash via FatFs, hand the pointer + length to
  `nes_rom_load()`, which validates the iNES header, points PRG/CHR banks at
  flash addresses (no copy — execute-in-place style), allocates 8 KB PRG-RAM
  in SRAM if the cart needs battery save.
- Battery saves: on MENU-back-to-picker, write `<romname>.sav` to FAT. Load
  on subsequent boot of the same ROM. Reuse the P8 flash write-back cache.
- ROM size limit: any single ROM ≤ 1 MB (covers everything up to MMC3 maxed
  out and almost all MMC5 carts).

---

## 8. Performance plan

Target: **60 fps for NROM, MMC1, MMC3**, 30 fps acceptable fallback for the
heaviest mappers. Strategy:

1. **Overclock RP2350 to 300 MHz** at boot (P8 already does this).
2. **Place the 6502 dispatch table and PPU hot loop in SRAM**, not XIP flash —
   avoids ~3× slowdown from flash cache misses on the inner loop. Use Pico
   SDK's `__not_in_flash_func` attribute.
3. **Inline the scanline downscaler** into the PPU's end-of-scanline hook so
   we never materialize a 256×240 buffer.
4. **Frame-skip toggle**: if we miss vblank, skip the next frame's PPU
   rendering (CPU still runs) — keeps audio/input smooth under load.
5. **Audio runs on Core1** isolated from rendering jitter.
6. Bench in Phase 2 with Super Mario Bros (NROM), Zelda (MMC1), SMB3 (MMC3),
   Kirby's Adventure (MMC3 heavy).

If Phase 2 shows we can't hit 60 fps on MMC3 with Nofrendo, options are:
- (a) drop to 30 fps for MMC3,
- (b) hand-optimize the 6502 inner loop in ARM asm,
- (c) fall back to InfoNES.

---

## 9. Build system

- **Host build** (SDL2): `cmake -B build && cmake --build build` →
  `nesbench` and `neshost`. Lets us debug the core without flashing.
- **Device build**: `cmake -B build_device -DPICO_BOARD=thumby_color
  -DCMAKE_TOOLCHAIN_FILE=$PICO_SDK_PATH/cmake/preload/toolchains/...` →
  `ThumbyNES.uf2`.
- Identical layout to ThumbyP8 so the existing dev workflow (`./deploy.sh`,
  the `usbipd attach` dance, `mpremote`/Thonny for the FAT volume) all just
  work.

---

## 10. Phased build order

| Phase | Goal | Done when |
|---|---|---|
| **0** | Skeleton + vendor Nofrendo + host build green | `nesbench` runs SMB headlessly for 600 frames on Linux |
| **1** | SDL2 host runner with video + input + audio | SMB playable on Linux at 60 fps with sound |
| **2** | Bench on host + paper math for device budget | Decision: stay on Nofrendo or fall back |
| **3** | Lift P8 device layer wholesale, retag identifiers | Firmware boots to a "no ROMs" picker on real hardware |
| **4** | Wire NES core to LCD + PWM audio + buttons | SMB plays on device with sound, no picker yet |
| **5** | ROM picker + USB MSC drag-and-drop | Drop a .nes onto the volume, pick from list, play |
| **6** | Battery saves + fast-forward + bilinear toggle | Zelda saves, RB works, video toggle in menu |
| **7** | Polish: per-ROM config, palette options, README | Public release-ready |

Rough effort split: Phase 0–2 is core+host work (does not need the device),
Phase 3–5 is the firmware wiring (needs hardware), Phase 6–7 is polish.

---

## 11. Open questions to resolve before Phase 1

1. **Exact Nofrendo source revision** — retro-go's vendored copy vs. the
   original Matthew Conte release. Retro-go's has more mapper fixes; start there.
2. **Palette**: NES has no canonical palette. Ship the FCEUX default and one
   alternate ("Nostalgia") selectable per-ROM.
3. **Dual-core memory model**: confirm Core1 stack lives in SRAM bank 5 (P8
   model) so Core0 doesn't fight it for bandwidth on the PPU hot loop.
4. **Mapper allow-list**: Phase 5 should refuse to load mappers we haven't
   tested, with a friendly "unsupported mapper N" screen, rather than crash.

---

## 12. Non-goals (explicit scope cuts)

- **No NES 2.0 extended header support** (rare, mostly homebrew).
- **No FDS, no VRC6/VRC7 expansion audio, no MMC5 audio** — out of CPU budget.
- **No save states** — battery SRAM only. Save states cost ~10 KB per slot
  and we'd rather spend that RAM on the audio ring + scanline buffers.
- **No netplay / link cable** — the engine's USB link layer is for MicroPython
  games, not bare-metal firmware.
- **No GUI cheats / Game Genie** — could come later, not in v1.

---

## 13. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Nofrendo too slow for MMC3 at 60 fps | Medium | Frame-skip + 30 fps fallback; InfoNES plan B |
| 256×240 → 128×128 looks ugly | Medium | Bilinear toggle in Phase 6 |
| RAM exhaustion with 8 KB PRG-RAM + audio rings + framebuffer + FatFs cache | Medium | Audit in Phase 3 with `arm-none-eabi-size`; trim FatFs cache first |
| GPLv2 viral concern for the firmware | Low | We're fine — entire firmware ships as GPLv2, source published in this repo |
| Some popular ROMs use mappers we don't support | Low-Medium | Allow-list with clear error; document supported mappers in README |

---

## 14. First concrete actions (when we start coding)

1. `git submodule add` (or vendor copy) Nofrendo from retro-go into
   `vendor/nofrendo/`, pin the commit in `VENDORING.md`.
2. Write `src/nes_core.[ch]` — 5 functions: `init`, `load_rom`, `run_frame`,
   `set_buttons`, `audio_pull(samples, n)`.
3. Write `src/host_main.c` — SDL2 window + audio + keyboard, calls the 5
   core functions. This is the dev environment; everything else builds on it.
4. Bench Mario / Zelda / SMB3 on host. Confirm correctness before touching
   the device build.

---

*End of plan. No source files have been created — only this document and the
empty directory layout above. Next step is to walk through this plan
together, decide on Nofrendo vs InfoNES, and then start Phase 0.*
