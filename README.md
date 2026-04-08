# ThumbyNES

A bare-metal NES emulator firmware for the **TinyCircuits Thumby Color**
(RP2350, 128×128 RGB565 LCD, PWM audio, 520 KB SRAM, 16 MB flash).

Drop a `.nes` ROM onto the device over USB, pick it from the on-screen
list, play with sound. Per-ROM saves, per-ROM settings, idle sleep,
quick-resume, fast-forward, six built-in palettes, and a pannable
read-mode for menu-heavy games — all in a 470 KB firmware image.

[`firmware/nesrun_device.uf2`](firmware/nesrun_device.uf2) is committed
to the repo if you want to flash without setting up the toolchain.

---

## Quickstart

1. **Flash the firmware.** Power off the Thumby Color, hold the **DOWN**
   d-pad, power back on. The device mounts as `RPI-RP2350`. Drag
   `firmware/nesrun_device.uf2` onto it. The device auto-reboots into
   ThumbyNES.

2. **First boot** wipes the disk to a fresh FAT16 volume labelled
   `THUMBYNES` (a yellow splash flashes briefly). After that, the
   filesystem persists across reboots.

3. **Drop ROMs.** Plug the device into a host. It enumerates as a
   removable drive — copy any number of `.nes` files into the root.
   Eject from the host. The device flushes the cache to flash and
   the picker rescans.

4. **Pick + play.** Up/Down to navigate, **A** to launch.

5. **Hold MENU at boot** to bypass quick-resume / force-reformat
   (the latter only takes effect if the FAT volume can't be mounted).

---

## Controls

### In-game

| Thumby button | NES |
|---|---|
| **A** (right face) | A |
| **B** (left face)  | B |
| **D-pad**          | D-pad |
| **LB**             | Select |
| **RB**             | Start |

### Menu chords (held during play)

| Gesture | Action |
|---|---|
| **MENU tap** (< 300 ms) | Toggle FIT ↔ CROP scaling (CROP also pauses) |
| **MENU + LEFT / RIGHT** | Volume −/+ (0..15, OSD popup) |
| **MENU + DOWN**         | Toggle 4× fast-forward |
| **MENU + UP**           | Cycle through six built-in palettes |
| **MENU + LB**           | Toggle on-screen FPS counter |
| **MENU hold** (≥ 600 ms) | Return to picker |

### In the picker

| Button | Action |
|---|---|
| **Up / Down** | Navigate the ROM list |
| **A**         | Launch the highlighted ROM |
| **MENU**      | Cancel back to lobby (also force-reformats on next boot if held there) |

### At boot

| Gesture | Action |
|---|---|
| **MENU held** | Skip quick-resume → go directly to the picker. Also forces a FAT reformat if the volume can't be mounted. |

---

## Features

### Display modes

- **FIT** (default): the entire 256×240 NES frame is downscaled 2:1
  to fit the 128×128 display, centred with 4 px letterbox top and
  bottom. You see the whole screen but small text is often hard to read.
- **CROP**: a 128×128 native 1:1 viewport into the NES frame. **Tap
  MENU to enter; the D-pad pans the viewport** across the full 256×240
  picture. **All NES inputs are suppressed and emulation is paused**
  while CROP is active, so you can read text or step away from the
  device without the cart eating frames. Tap MENU again to return to
  FIT and resume play.

### Palettes

Six built-in NES palettes from Nofrendo: `NOFRENDO`, `COMPOSITE`,
`NESCLASSIC`, `NTSC`, `PVM`, `SMOOTH`. Cycle in-game with **MENU + UP**.
The current choice is persisted per-ROM.

### Save state

- **Battery-backed PRG-RAM** is persisted to a sidecar `<romname>.sav`
  next to the ROM in the FAT root. Loaded on launch, written on exit
  to picker, **and** auto-saved every 30 s of gameplay so a flat
  battery never costs you more than half a minute.
- ROMs without a battery flag in their iNES header are unaffected.
- No save-state slots — battery only. See [Non-goals](#non-goals).

### Per-ROM config

Scale mode, palette, volume, and FPS-overlay state are persisted to
a sidecar `<romname>.cfg` next to the ROM and its `.sav`. Each game
remembers its own preferences across sessions.

### Quick-resume

The most recently launched ROM is recorded in `/.last`. On the next
boot the device skips the picker and jumps straight back into that
cart — handy for "five minutes of Zelda before bed" use. **Hold MENU
at boot** to bypass quick-resume and reach the picker instead.

### Idle sleep

After 90 s of no input the device commits a battery save, blanks the
LCD backlight, and drops to a tight sleep loop. Press any button to
wake. (The Thumby Color drives the backlight from a single GPIO so
sleep is on/off — no PWM dimming.)

### Fast-forward

**MENU + DOWN** toggles 4× speed. The frame-rate cap is bypassed and
four NTSC frames are stepped per outer iteration; the renderer and
audio still only present the most recent frame so the audio ring
doesn't overflow.

### FPS counter

**MENU + LB** toggles a yellow on-screen FPS readout in the top-left
corner. Shows ` FF` when fast-forward is engaged. Off by default.

### Picker

Two-line rows: ROM name on top, `m<mapper>  <size>K` below (mapper
number parsed from the iNES header at scan time). Footer shows
`<sel>/<n>  pg <p>/<P>`. The `.nes` extension is stripped from
displayed names.

### Boot splash

A short text panel (`ThumbyNES v0.8 · nofrendo · GPLv2`) displays
between the hardware-init green splash and the first ROM hand-off,
just so the version is always visible after a flash.

---

## Hardware target

| | |
|---|---|
| **MCU** | RP2350 dual-core Cortex-M33 @ 250 MHz |
| **Display** | 128×128 RGB565, GC9107 LCD over SPI + DMA |
| **Audio** | 9-bit PWM on GP23 @ 22050 Hz sample rate, hardware IRQ-driven ring buffer |
| **Storage** | 12 MB FAT16 volume on internal QSPI flash, exposed as USB MSC |
| **Input** | A, B, D-pad, LB, RB, MENU |

---

## Architecture

### Video pipeline

NES native is 256×240 — the Thumby Color is 128×128. There are two
blit paths:

- **`blit_fit`** does a 2:1 nearest-neighbor downscale (every other
  pixel + every other line) and centres the resulting 128×120 image
  with 4 px black bars top and bottom. The Nofrendo PPU writes 8-bit
  palette indices into a single 256×240 buffer; the scaler reads
  every other column on every other row and looks up RGB565 from the
  precomputed palette in one pass — no intermediate full-resolution
  colour buffer.

- **`blit_crop`** is a 1:1 native copy of a 128×128 window starting
  at NES `(pan_x, pan_y)`, clamped to `[0, 128] × [0, 112]`. Default
  pan is `(64, 56)` — the centred view. While CROP is active the
  D-pad shifts `pan_x / pan_y` and the cart receives no inputs.

### Audio pipeline

Nofrendo's APU emits signed 16-bit mono samples at 22050 Hz, exactly
matching the PWM driver's IRQ rate. Each frame the runner pulls
`samples_per_frame` (≈ 368 at 60 fps) from `nes->apu->buffer`,
applies the linear volume scale, and pushes them into the PWM ring
buffer. The IRQ handler converts s16 → 9-bit unsigned PWM duty on
the way out.

In CROP mode the audio path is skipped entirely so the speaker goes
silent rather than holding the last frame's samples.

### Memory budget

| Region | Bytes |
|---|---|
| Nofrendo internal state (CPU/PPU/APU/mappers) | ~30 KB |
| Nofrendo PPU framebuffer (272×240 8-bit indices) | 65 KB |
| Thumby Color framebuffer (128×128 RGB565) | 32 KB |
| Audio ring (4096 samples × 2 bytes) | 8 KB |
| FatFs work area + flash disk write cache | ~12 KB |
| ROM (PRG slurped to malloc'd RAM at load) | ≤ 1 MB |
| **Free heap** | **~350 KB** |

### Performance

The Nofrendo core compiles to ~55 KB of ARM code. The two hottest
inner loops — `nes6502_execute` (the 6502 dispatch) and
`ppu_renderline` (per-scanline pixel emission) — are placed in SRAM
via `IRAM_ATTR`, mapped to a `.time_critical.nes` section attribute
in the device build. The Pico SDK linker script copies that section
into RAM at boot, so the inner loops dodge XIP flash cache miss
penalties entirely. Everything else still executes from XIP.

Final Fantasy (MMC1, 256 KB PRG) runs full speed at 60 fps with
audio glitch-free. Frame pacing is `sleep_until()`-locked to NTSC
unless the user holds MENU + DOWN to engage 4× fast-forward.

### Frame loop (per iteration)

1. Sample input + idle sleep tracking.
2. MENU edge / chord / hold detection.
3. If asleep → 50 ms idle, continue.
4. If `scale_mode == SCALE_CROP` → pan only, **no emulation step**.
5. Else → run 1 (or 4) NTSC frames via `nesc_run_frame()`.
6. Blit (fit or crop) into the 128×128 RGB565 framebuffer.
7. Draw FPS / OSD overlays if active.
8. Push framebuffer over SPI/DMA to the LCD.
9. Pull APU samples → scale by volume → push to PWM ring.
10. Maybe auto-save battery (30 s wall-clock interval).
11. Cap to 60 fps via `sleep_until()` (skipped in fast-forward).

---

## Building

### Host (SDL2 runner — for development on Linux/macOS)

Prereqs: `cmake`, a C compiler, `libsdl2-dev`.

```bash
cmake -B build
cmake --build build -j8
./build/neshost /path/to/rom.nes        # interactive SDL window
./build/nesbench /path/to/rom.nes       # headless benchmark
```

Host controls: `Z`=A, `X`=B, `Enter`=Start, `RShift`=Select, arrows=D-pad, `Esc`=quit.

### Device (Thumby Color firmware)

Prereqs: `gcc-arm-none-eabi`, `cmake`, and a checkout of the Pico SDK
(any RP2350-capable revision).

```bash
cmake -B build_device -S device \
      -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build_device -j8
```

Output: `build_device/nesrun_device.uf2`. To flash, see the
[Quickstart](#quickstart).

---

## Repository layout

```
ThumbyNES/
├── README.md                ← this file
├── PLAN.md                  ← original design plan + phased build order
├── CMakeLists.txt           ← host build (SDL2 runner + bench)
├── firmware/
│   └── nesrun_device.uf2    ← prebuilt device firmware (committed)
├── vendor/
│   ├── VENDORING.md         ← vendored-source provenance + license + patches
│   └── nofrendo/            ← NES core, GPLv2 (retro-go fork, patched)
├── src/                     ← cross-platform glue
│   ├── nes_core.[ch]        ← thin wrapper around Nofrendo's API
│   ├── host_main.c          ← SDL2 host runner (dev iteration)
│   └── bench_main.c         ← headless benchmark harness
└── device/                  ← Thumby Color firmware
    ├── CMakeLists.txt       ← Pico SDK device build
    ├── nes_device_main.c    ← entry, lobby, picker, quick-resume
    ├── nes_run.[ch]         ← ROM runner: input + scaler + audio + autosave + sleep
    ├── nes_picker.[ch]      ← ROM list UI + iNES header scanner
    ├── nes_font.[ch]        ← 3×5 bitmap font (Pemsa, MIT, transcribed)
    ├── nes_lcd_gc9107.[ch]  ← GC9107 SPI/DMA LCD driver + backlight
    ├── nes_buttons.[ch]     ← GPIO button reader
    ├── nes_audio_pwm.[ch]   ← PWM audio output + sample IRQ
    ├── nes_flash_disk.[ch]  ← flash-backed disk + RAM write-back cache
    ├── nes_msc.c            ← TinyUSB MSC class callbacks
    ├── usb_descriptors.c    ← TinyUSB device + composite descriptors
    ├── tusb_config.h        ← TinyUSB compile config
    └── fatfs/               ← vendored FatFs R0.15 (BSD-1, ChaN)
```

### Files written to the FAT volume

| File | Purpose |
|---|---|
| `<rom>.nes`     | The ROM image you dropped via USB. |
| `<rom>.sav`     | Battery-backed PRG-RAM, 8 KB. Auto-saved every 30 s. |
| `<rom>.cfg`     | Per-ROM scale / palette / volume / FPS-overlay state. |
| `/.last`        | Base name of the most recently launched ROM (quick-resume). |

---

## Vendored sources

| Component | License | Source |
|---|---|---|
| [Nofrendo](https://github.com/ducalex/retro-go) NES core | GPLv2 | retro-go @ commit `4ced120`, two local patches (see `vendor/VENDORING.md`) |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15 | BSD-1-clause (ChaN) | from ThumbyP8 |
| [Pemsa](https://github.com/egordorichev/pemsa) 3×5 font glyphs | MIT | transcribed |

ThumbyNES is itself **GPLv2** to remain compatible with the Nofrendo
core. The vendored copy lives under `vendor/nofrendo/` with patches
recorded in `vendor/VENDORING.md`.

---

## Non-goals

Explicit scope cuts to protect the RAM/CPU budget:

- **No save states.** Battery-backed PRG-RAM only — see above.
- **No FDS, no VRC6 / VRC7 / MMC5 expansion audio.**
- **No NES 2.0 extended-header support.**
- **No netplay / link cable.**
- **No on-device cheats or Game Genie.**
- **No PWM backlight dimming** (single-GPIO BL line on the Thumby Color).

---

## Credits

- **Nofrendo** by Matthew Conte (1998–2000), Neil Stevens, and the
  retro-go maintainers.
- **FatFs** by ChaN.
- **Pemsa font** by Egor Dorichev (MIT).
- **ThumbyP8** firmware patterns (LCD driver, PWM audio path, USB MSC
  flow, FAT layout, lobby/picker state machine) — reused and renamed.

The Thumby Color hardware is by [TinyCircuits](https://tinycircuits.com).

---

*ThumbyNES v0.8 — five sessions from "what if" to a pocket NES with
saves, sleep, and a quick-resume habit.*
