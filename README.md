# ThumbyNES

A standalone bare-metal NES emulator firmware for the **TinyCircuits
Thumby Color** (RP2350, 128×128 RGB565 LCD, 4-channel PWM audio,
520 KB SRAM, 16 MB flash).

Drag `.nes` ROMs onto the device over USB, pick from the on-screen
list, play with sound. Modelled on the [ThumbyP8](../ThumbyP8) PICO-8
firmware in the same workspace — same boot flow, same USB MSC
drag-and-drop, same lobby/picker pattern, with the PICO-8 runtime
swapped for a vendored copy of the [Nofrendo](https://github.com/ducalex/retro-go)
NES core.

## Status

| Phase | What | Result |
|---|---|---|
| 0 | Vendor Nofrendo, host build green | ✅ `nesbench` runs |
| 1 | SDL2 host runner with video/audio/input | ✅ Final Fantasy boots, sounds, plays |
| 2 | Bench host → confirm device feasibility | ✅ 17,000 fps headless on Linux → ample headroom |
| 3 | Lift ThumbyP8 device layer, boot to "no ROMs" picker | ✅ LCD/USB/FAT/picker working on hardware |
| 4 | Wire Nofrendo to LCD + PWM audio + buttons | ✅ Final Fantasy plays on real device with sound |
| 5 | Battery saves + fast-forward toggle | ✅ `.sav` round-trips, MENU tap = 4× speed |
| 6 | FPS counter, 60 fps cap, scale modes, persisted config | ✅ FIT/CROP toggle, `/.cfg` survives reboots |
| 7 | Palette cycling, per-ROM config, pannable CROP, picker polish | ✅ 6 palettes, sidecar `.cfg`, mapper/size in list |
| 8 | Pause-in-CROP, volume, quick-resume, autosave, sleep, IRAM hot loops, boot splash | ✅ "handheld basics" bundle |

## Hardware target

- **MCU**: RP2350 dual-core Cortex-M33 @ 250 MHz
- **Display**: 128×128 RGB565, GC9107 LCD over SPI + DMA
- **Audio**: PWM on GP23, 9-bit @ 22050 Hz sample rate, hardware IRQ-driven ring buffer
- **Storage**: 12 MB FAT16 volume on internal QSPI flash, exposed as USB MSC
- **Input**: A, B, D-pad, LB, RB, MENU

## Controls (in-game)

| Thumby button | NES |
|---|---|
| A (right face) | A |
| B (left face) | B |
| D-pad | D-pad |
| LB | Select |
| RB | Start |
| MENU (tap, < 300 ms) | Toggle FIT ↔ CROP scaling |
| MENU + LEFT / RIGHT | Volume −/+ (0..15) |
| MENU + DOWN (chord) | Toggle 4× fast-forward |
| MENU + UP (chord) | Cycle palette (6 built-in) |
| MENU + LB (chord) | Toggle FPS overlay |
| MENU (hold ≥ 600 ms) | Return to picker |

**Scaling modes:**

- **FIT** (default): the entire 256×240 NES frame is downscaled 2:1
  to fit the 128×128 display, centred with 4 px letterbox top and
  bottom. You see the whole screen but small text is often illegible.
- **CROP**: shows a 128×128 native 1:1 viewport of the NES frame.
  Tap MENU to enter; the viewport starts centred and the **D-pad
  pans** it across the full 256×240 picture. **All NES inputs are
  suppressed in CROP mode** — the cart keeps running but receives no
  buttons, so you can read text or inspect HUDs without the game
  reacting. **The cart is fully paused in CROP** — emulation halts
  and audio goes silent so you can step away from the device without
  losing your place. Tap MENU again to return to FIT and resume play.

**Palettes:** Nofrendo ships six built-in NES palettes (`NOFRENDO`,
`COMPOSITE`, `NESCLASSIC`, `NTSC`, `PVM`, `SMOOTH`). Cycle with
MENU + UP. The current choice is persisted per-ROM.

**Per-ROM config:** scale mode, palette, volume, and FPS-overlay state
are persisted to a sidecar `<romname>.cfg` next to the ROM and its
`.sav`. Each game remembers its own preferences across sessions.

**Quick-resume:** the most recently launched ROM is recorded in
`/.last`. On the next boot the device skips the picker and jumps
straight back into that cart — handy for "five minutes of Zelda
before bed" use. **Hold MENU at boot** to bypass quick-resume and
go to the picker instead.

**Auto-save:** battery-backed PRG-RAM is flushed to `.sav` every
30 s of gameplay (plus on exit), so a flat battery never costs you
more than half a minute.

**Idle sleep:** after 90 s of no input the device blanks the LCD
backlight, halts emulation, and writes the battery save. Press any
button to wake.

## Repository layout

```
ThumbyNES/
├── PLAN.md                  ← original design plan + phased build order
├── README.md                ← this file
├── CMakeLists.txt           ← host build (SDL2 runner + bench)
├── vendor/
│   ├── VENDORING.md         ← vendored-source provenance + license notes
│   └── nofrendo/            ← NES core, GPLv2 (retro-go fork)
├── src/                     ← cross-platform glue
│   ├── nes_core.[ch]        ← thin wrapper around Nofrendo's API
│   ├── host_main.c          ← SDL2 host runner (dev iteration)
│   └── bench_main.c         ← headless benchmark harness
└── device/                  ← Thumby Color firmware
    ├── CMakeLists.txt       ← Pico SDK device build
    ├── nes_device_main.c    ← entry, lobby/picker state machine
    ├── nes_run.[ch]         ← ROM runner: input + scaler + audio bridge
    ├── nes_picker.[ch]      ← ROM list UI + file scanner
    ├── nes_font.[ch]        ← 3×5 bitmap font (Pemsa, MIT, transcribed)
    ├── nes_lcd_gc9107.[ch]  ← GC9107 SPI/DMA LCD driver
    ├── nes_buttons.[ch]     ← GPIO button reader
    ├── nes_audio_pwm.[ch]   ← PWM audio output + sample IRQ
    ├── nes_flash_disk.[ch]  ← flash-backed disk + RAM write-back cache
    ├── nes_msc.c            ← TinyUSB MSC class callbacks
    ├── usb_descriptors.c    ← TinyUSB device + composite descriptors
    ├── tusb_config.h        ← TinyUSB compile config
    └── fatfs/               ← vendored FatFs R0.15 (BSD-1, ChaN)
```

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

Output: `build_device/nesrun_device.uf2`.

To flash: power off the Thumby Color, hold the **DOWN** d-pad, power
on. The device mounts as `RPI-RP2350` — drag the `.uf2` onto it. The
device reboots into ThumbyNES automatically.

A prebuilt firmware image is committed to the repo at
[`firmware/nesrun_device.uf2`](firmware/nesrun_device.uf2) so you
don't need to set up the Pico SDK toolchain just to try it.

## Using the device

1. **First boot** wipes the disk to a fresh FAT16 volume labelled
   `THUMBYNES` (yellow splash flashes briefly during format). After
   that, the disk persists across reboots.
2. **Drop ROMs** by plugging the device into a host. It appears as
   a removable drive — copy any number of `.nes` files into the root.
3. **Eject** the drive from the host. The device flushes the cache
   to flash, rescans, and switches from the "no roms" splash to a
   scrollable list.
4. **Pick** with **Up/Down**, launch with **A**.
5. **Hold MENU** for ~500 ms to return to the picker.
6. **Hold MENU at boot** to force-reformat the FAT volume (rescue if
   the disk gets corrupted).

## Architecture notes

### Video pipeline

NES native is 256×240 — the Thumby Color is 128×128. We do a
2:1 nearest-neighbor downscale (every other pixel + every other
line) and centre the resulting 128×120 image with 4 px black bars
top and bottom. The Nofrendo PPU writes 8-bit palette indices into a
single 256×240 buffer; our scaler reads every other column on every
other row and looks up RGB565 from the precomputed palette in one
pass — no intermediate full-resolution colour buffer. Tradeoff:
fine UI text in many games becomes hard to read. This is a physical
limit, not a software one.

### Audio pipeline

Nofrendo's APU emits signed 16-bit mono samples at 22050 Hz, exactly
matching the PWM driver's IRQ rate. Each frame the runner pulls
~368 samples (= 22050/60) from the APU and pushes them into the PWM
ring buffer. The IRQ handler converts s16 → 9-bit unsigned PWM duty
on the way out.

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

### Performance notes

The Nofrendo core compiles to ~55 KB of ARM code. The two hottest
inner loops — `nes6502_execute` (the 6502 dispatch) and
`ppu_renderline` (per-scanline pixel emission) — are placed in SRAM
via `IRAM_ATTR` (mapped to a `.time_critical.nes` section attribute
in the device build). The Pico SDK linker script copies that
section into RAM at boot, so the inner loops dodge XIP flash cache
miss penalties entirely. Everything else still executes from XIP.

Final Fantasy (MMC1, 256 KB PRG) runs full speed at 60 fps with
audio glitch-free. Frame pacing is `sleep_until()`-locked to NTSC
unless the user holds MENU + DOWN to engage 4× fast-forward.

## Vendored sources

| Component | License | Source |
|---|---|---|
| [Nofrendo](https://github.com/ducalex/retro-go) NES core | GPLv2 | retro-go @ commit `4ced120` |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15 | BSD-1-clause (ChaN) | from ThumbyP8 |
| [Pemsa](https://github.com/egordorichev/pemsa) 3×5 font glyphs | MIT | transcribed |

ThumbyNES is itself **GPLv2** to remain compatible with the Nofrendo
core. The vendored copy lives unmodified under `vendor/nofrendo/` —
see `vendor/VENDORING.md` for the pinned commit and any local patches.

## Non-goals

Explicit scope cuts to protect the RAM/CPU budget:

- No save states. Battery-backed PRG-RAM only (per-ROM `.sav` files
  in the FAT root, e.g. `Zelda.nes` ↔ `Zelda.sav`). Written on exit
  to picker, restored on next launch.
- No FDS, no VRC6 / VRC7 / MMC5 expansion audio.
- No NES 2.0 extended-header support.
- No netplay / link cable.
- No on-device cheats / Game Genie.

## Credits

- **Nofrendo** by Matthew Conte (1998–2000), Neil Stevens, and the
  retro-go maintainers.
- **FatFs** by ChaN.
- **Pemsa font** by Egor Dorichev (MIT).
- **ThumbyP8** firmware patterns (LCD driver, PWM audio path, USB MSC
  flow, FAT layout) — reused largely unchanged.

The Thumby Color hardware is by [TinyCircuits](https://tinycircuits.com).
