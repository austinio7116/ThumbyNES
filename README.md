# ThumbyNES

A bare-metal NES + Sega Master System + Game Gear emulator firmware
for the **TinyCircuits Thumby Color** (RP2350, 128×128 RGB565 LCD,
PWM audio, 520 KB SRAM, 16 MB flash).

Drop `.nes`, `.sms`, or `.gg` ROMs onto the device over USB, browse
them in a tabbed picker with thumbnail screenshots, play with sound.
Per-ROM saves, per-ROM settings, idle sleep, fast-forward, palettes,
in-game screenshot capture, pannable read-mode for menu-heavy games
— all in a ~1.1 MB firmware image.

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
   removable drive — copy any number of `.nes`, `.sms`, or `.gg`
   files into the root. Eject from the host. The device flushes the
   cache to flash and the picker rescans.

4. **Pick + play.** Browse with **D-pad** (any axis), shoulder
   buttons switch tabs, **A** to launch.

5. **Hold MENU at boot** to force-reformat (only takes effect if
   the FAT volume can't otherwise be mounted).

---

## The picker

The browser is the resting state of the device — there's no separate
"main menu". When you exit a game (long-hold MENU in-game) you land
back here.

### Layout: tabs + two views

A **tab strip** runs across the top of the picker:

```
[★ FAV] [NES] [SMS] [GG]
```

Each tab shows a procedurally-drawn platform icon (controller for
NES, cartridge for SMS, handheld for GG, star for favorites) and the
ROM count for that tab. Empty tabs are skipped automatically when
stepping with the shoulder buttons. The active tab is highlighted in
orange.

Two views, toggled with **MENU**:

- **Hero view** (default) — one ROM per screen, 64×64 thumbnail
  centred under the tab strip, large 2×-scaled ROM title underneath
  (auto-marquees if it doesn't fit), meta line (system / mapper /
  size / region), favorite indication via yellow title text, sort
  badge in the title row, position counter and prev/next arrows
  along the bottom.
- **List view** — three rows per screen, each row a 32×32 thumbnail
  next to the ROM name and meta. Highlighted row glows green; the
  selected row's position-in-tab appears as a third line.

The thumbnails are screenshots you've captured in-game (see below).
ROMs without a screenshot fall back to a procedurally-drawn
placeholder: the platform icon centred on a tinted panel.

### Picker controls

| Key | Action |
|---|---|
| **LEFT / RIGHT / UP / DOWN** | prev / next ROM (any D-pad direction) |
| **LB / RB** | prev / next tab (skips empty tabs) |
| **A** | launch the highlighted ROM |
| **B** | toggle favorite (highlighted ROM goes yellow) |
| **MENU tap** | toggle hero ↔ list view |
| **MENU hold** (≥ 500 ms) | cycle sort mode (alpha → favs first → size desc) |

The current sort mode is shown briefly as an OSD overlay when changed
and permanently as a small badge above the title in hero view.

There is no "back to lobby" path from the picker — once a ROM is on
disk you stay in the picker until you launch something. The lobby's
"no ROMs found" splash only kicks in when the volume is genuinely
empty.

### Persistence

A small `/.picker_view` sidecar remembers the active **view**, **tab**,
and **sort** across reboots, so the picker comes back where you left
it. Sort is intentionally session-persistent now too — first boot
defaults to alpha.

`/.favs` is a plain newline-separated list of favorited base ROM
names (system-agnostic). Survives reboots and is editable from a host
over USB if you want to bulk-manage them.

---

## Controls in-game

### Cart inputs

| Thumby button | NES | SMS | Game Gear |
|---|---|---|---|
| **A** (right face) | A | Button 2 | Button 2 |
| **B** (left face)  | B | Button 1 | Button 1 |
| **D-pad**          | D-pad | D-pad | D-pad |
| **LB**             | Select | — | — |
| **RB**             | Start | Pause | Start |

### MENU chords (held during play)

These work in both NES and SMS/GG runners.

| Gesture | Action |
|---|---|
| **MENU tap** (< 300 ms) | Toggle FIT ↔ CROP scaling (CROP pauses + pans the SMS/NES view) |
| **MENU + A**            | **Save a screenshot** (32×32 + 64×64 sidecars) |
| **MENU + LEFT / RIGHT** | Volume −/+ (0..15, OSD popup) |
| **MENU + DOWN**         | Toggle 4× fast-forward |
| **MENU + UP** *(NES)*   | Cycle through six built-in palettes |
| **MENU + LB**           | Toggle on-screen FPS counter |
| **MENU + RB**           | Toggle BLEND smoothing (on by default) |
| **MENU + B** *(NES)*    | Toggle NTSC 60 Hz ↔ PAL 50 Hz (next launch) |
| **MENU hold** (≥ 600 ms) | Return to picker |

### At boot

| Gesture | Action |
|---|---|
| **MENU held** | Force a FAT reformat if the volume can't be mounted. (No effect on a healthy volume.) |

---

## Screenshots

Pressing **MENU + A** in any running game grabs the live 128×128
framebuffer and writes two raw RGB565 sidecars next to the ROM:

| File | Size | Use |
|---|---|---|
| `<rom>.scr32` | 32×32 (2 KB)  | inline thumbnail in the list view |
| `<rom>.scr64` | 64×64 (8 KB)  | hero thumbnail in the default view |

Both are produced from the same screen capture in a single step:
the 32×32 is a 4×4 box-average of the framebuffer, the 64×64 is a
2×2 average. The picker reads them on demand for the visible ROM(s)
and falls back to a procedural placeholder when missing.

A short OSD reads `shot saved` (or `shot fail` on a write error) so
you know the capture landed. ROMs without screenshots show a tinted
panel with the platform icon centred — distinct enough that you can
tell at a glance which carts you've yet to capture.

---

## Display modes

- **FIT** (default): the entire native frame is downscaled to fit
  the 128×128 display.
  - **NES** 256×240 → 128×120 with 4 px letterbox top/bottom.
  - **SMS** 256×192 → 128×96 with 16 px letterbox top/bottom.
  - **GG** 160×144 → 128×128 via asymmetric 5:4 / 9:8 nearest, **fills the whole screen**.

  With **BLEND** on (the default for NES/SMS) each output pixel is a
  2×2 box average of four source pixels — softer image, no
  nearest-neighbor shimmer. With BLEND off you get crisp drop-pixel
  output instead. Toggle BLEND with **MENU + RB**.

- **CROP** (NES + SMS only): a 128×128 native 1:1 viewport into the
  source frame. **Tap MENU to switch to it; the D-pad then pans the
  viewport** across the full picture. **All cart inputs are suppressed
  and emulation is paused** while CROP is active, so you can read text
  or step away from the device without the cart eating frames. Tap
  MENU again to return to FIT. Game Gear has no CROP — the asymmetric
  fill already covers the whole 160×144.

The scale mode and the BLEND toggle are persisted independently per
ROM in the `.cfg` sidecar.

---

## Region (NTSC / PAL)

### NES

ThumbyNES auto-detects region per ROM at picker scan time using
three signals (any one fires → PAL):

1. **iNES 2.0 byte 12 bits 0..1** == 1 (most reliable, rarely set)
2. **iNES 1.0 byte 9 bit 0** == 1 (rarely set in real-world dumps)
3. **Filename heuristic** — case-insensitive substring match for
   `(E)`, `(Eu)`, `(Europe)`, `(PAL)`, `(A)`, `(Australia)`. This
   covers no-intro / GoodNES naming and is in practice the most
   useful of the three.

Tap **MENU + B** in-game to override the detection — it flips between
NTSC (60 Hz, 1.79 MHz CPU) and PAL (50 Hz, 1.66 MHz CPU). Persisted
per-ROM and **takes effect on the next launch** — the OSD shows
`NTSC next launch` / `PAL  next launch`.

### SMS / GG

smsplus does its own region work from the ROM header (byte at 0x7FFF)
and an internal CRC database, so the runner just trusts whatever it
reports — no manual override needed in practice. PAL games run at
50 Hz and NTSC at 60 Hz automatically; the picker uses the same
filename heuristic as NES for the meta column display only.

---

## Palettes (NES)

Six built-in NES palettes from Nofrendo: `NOFRENDO`, `COMPOSITE`,
`NESCLASSIC`, `NTSC`, `PVM`, `SMOOTH`. Cycle in-game with **MENU + UP**.
**Default is COMPOSITE** — warmer and closer to a CRT than the
brighter `NOFRENDO`. The current choice is persisted per-ROM. SMS and
GG have no palette toggle — those cores manage their own VDP palette.

---

## Save state

- **Battery-backed cart RAM** is persisted to a sidecar `<romname>.sav`
  next to the ROM in the FAT root. Loaded on launch, written on exit
  to picker, **and** auto-saved every 30 s of gameplay so a flat
  battery never costs you more than half a minute.
  - NES: nofrendo's PRG-RAM (8 KB typical).
  - SMS / GG: smsplus's `cart.sram` (32 KB).
- ROMs without battery support in their header are unaffected.
- No save-state slots — battery only. See [Non-goals](#non-goals).

---

## Per-ROM config

Scale mode, BLEND, palette (NES), volume, FPS overlay, region (NES)
are persisted to a sidecar `<romname>.cfg`. The NES and SMS runners
use independent magic numbers in the cfg file so a `.cfg` written by
one core is silently ignored by the other.

---

## Idle sleep

After 90 s of no input the device commits a battery save, blanks the
LCD backlight, and drops to a tight sleep loop. Press any button to
wake. (The Thumby Color drives the backlight from a single GPIO so
sleep is on/off — no PWM dimming.)

---

## Fast-forward

**MENU + DOWN** toggles 4× speed. The frame-rate cap is bypassed and
four cart frames are stepped per outer iteration; the renderer and
audio still only present the most recent frame so the audio ring
doesn't overflow.

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

### Multi-core dispatch

The picker tags each ROM entry with a `system` field at scan time
(extension based: `.nes` → NES, `.sms` → SMS, `.gg` → GG). When the
user launches a ROM the device main loop dispatches to either
`nes_run_rom` (nofrendo) or `sms_run_rom` (smsplus). Two parallel
runners share every hardware driver — LCD, audio PWM, buttons, FAT
sidecars, idle-sleep timer — and only differ in the per-frame core
calls and the scaling routines.

### Video pipeline

The Thumby Color is 128×128. Each runner has its own scaler set:

**NES** (`nes_run.c`)
- **`blit_fit`** — 256×240 → 128×120 nearest, 4 px letterbox.
- **`blit_blend`** — 256×240 → 128×120 with 2×2 box average, same
  letterbox. Soft, anti-shimmer.
- **`blit_crop`** — 1:1 native 128×128 window with D-pad pan.

**SMS / GG** (`sms_run.c`)
- **`blit_fit_sms`** — 256×192 → 128×96 nearest, 16 px letterbox.
- **`blit_blend_sms`** — 256×192 → 128×96 with 2×2 box average.
- **`blit_crop_sms`** — 1:1 native crop with pan (SMS only).
- **`blit_fit_gg`** — 160×144 → 128×128 with asymmetric 5:4 / 9:8
  nearest. Fills the whole screen with no letterbox.

### Audio pipeline

Both cores produce signed 16-bit mono samples at 22050 Hz, exactly
matching the PWM driver's IRQ rate. smsplus produces `(sndrate / fps + 1)`
stereo samples per frame; the wrapper mixes L and R to mono with a
clipping `(L + R) / 2`. nofrendo emits mono natively.

In CROP mode the audio path is skipped entirely so the speaker goes
silent rather than holding the last frame's samples.

### Memory budget

| Region | Bytes |
|---|---|
| nofrendo internal state (CPU/PPU/APU/mappers) | ~30 KB |
| nofrendo PPU framebuffer | 65 KB |
| smsplus internal state (CPU/VDP/PSG + LUTs) | ~80 KB BSS |
| smsplus VDP/Z80 dynamic (vram, wram, sram, audio streams) | ~60 KB heap during SMS |
| Thumby Color framebuffer (128×128 RGB565) | 32 KB |
| Audio ring (4096 samples × 2 bytes) | 8 KB |
| FatFs work area + flash disk write cache | ~12 KB |
| Picker / favorites / cfg / view pref | ~6 KB |
| ROM (NES: PRG slurped to RAM; SMS/GG: XIP-mapped) | ≤ 1 MB |
| **Free heap (typical)** | **~150 KB** |

The smsplus core was vendored as-is from retro-go and ports cleanly
once a few upstream issues are patched out — see `vendor/VENDORING.md`
for the patch list (separable bp_lut decomposition, Z80 SZHVC tables
moved out of the runtime malloc path, fixed-width uint32 typedefs for
host LP64 builds, etc.).

### Performance

The nofrendo and smsplus hot inner loops are placed in SRAM via
`IRAM_ATTR` / `__not_in_flash_func`, mapped to `.time_critical.*`
section attributes in the device build. The Pico SDK linker copies
those into RAM at boot, so the hot dispatch loops dodge XIP flash
cache miss penalties entirely. Everything else still executes from
XIP.

NES Final Fantasy (MMC1, 256 KB PRG) and SMS Sonic the Hedgehog both
run full speed with audio glitch-free. Frame pacing is
`sleep_until()`-locked to native refresh unless the user holds
MENU + DOWN to engage 4× fast-forward.

---

## Building

### Host (SDL2 runner — for development on Linux/macOS)

Prereqs: `cmake`, a C compiler, `libsdl2-dev`.

```bash
cmake -B build
cmake --build build -j8
./build/neshost /path/to/rom.nes        # NES interactive SDL window
./build/smshost /path/to/rom.sms        # SMS / GG interactive SDL window
./build/nesbench /path/to/rom.nes       # headless NES benchmark
./build/smsbench /path/to/rom.sms       # headless SMS / GG benchmark
```

Host controls (both runners): `Z`=A/B1, `X`=B/B2, `Enter`=Start, `RShift`=Select/Pause, arrows=D-pad, `Esc`=quit.

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
├── PLAN.md                  ← original NES design plan
├── SMS_PLAN.md              ← SMS / GG integration plan
├── CMakeLists.txt           ← host build (SDL2 runners + benches)
├── firmware/
│   └── nesrun_device.uf2    ← prebuilt device firmware (committed)
├── tools/
│   └── gen_sms_z80_szhvc.c  ← build-time generator for the smsplus Z80 flag tables
├── vendor/
│   ├── VENDORING.md         ← vendored-source provenance, license, patches
│   ├── nofrendo/            ← NES core, GPLv2 (retro-go fork, patched)
│   └── smsplus/             ← SMS / GG core, GPLv2 (retro-go fork, patched)
├── src/                     ← cross-platform glue
│   ├── nes_core.[ch]        ← thin wrapper around nofrendo's API
│   ├── sms_core.[ch]        ← thin wrapper around smsplus's API
│   ├── host_main.c          ← NES SDL2 host runner
│   ├── sms_host_main.c      ← SMS / GG SDL2 host runner
│   ├── bench_main.c         ← NES headless benchmark
│   └── sms_bench_main.c     ← SMS / GG headless benchmark
└── device/                  ← Thumby Color firmware
    ├── CMakeLists.txt       ← Pico SDK device build
    ├── nes_device_main.c    ← entry, lobby, picker dispatch
    ├── nes_run.[ch]         ← NES runner (input + scaler + audio + autosave + sleep)
    ├── sms_run.[ch]         ← SMS / GG runner (mirror of nes_run for smsplus)
    ├── nes_picker.[ch]      ← tabbed picker UI + extension scanner
    ├── nes_thumb.[ch]       ← procedural icons + screenshot sidecar I/O
    ├── nes_font.[ch]        ← 3×5 bitmap font + 2× scaled draw helper
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
| `<rom>.nes` / `.sms` / `.gg` | The ROM image you dropped via USB. |
| `<rom>.sav`     | Battery-backed cart RAM. Auto-saved every 30 s. |
| `<rom>.cfg`     | Per-ROM scale / palette / volume / FPS / region state (system-tagged magic). |
| `<rom>.scr32`   | 32×32 RGB565 thumbnail (list view). |
| `<rom>.scr64`   | 64×64 RGB565 thumbnail (hero view). |
| `/.favs`        | Newline-separated list of favorited ROM names. |
| `/.picker_view` | Persisted picker view + active tab + sort mode. |

---

## Vendored sources

| Component | License | Source |
|---|---|---|
| [nofrendo](https://github.com/ducalex/retro-go) NES core | GPLv2 | retro-go @ commit `4ced120`, two local patches |
| [smsplus](https://github.com/ducalex/retro-go) SMS / GG core | GPLv2 | retro-go @ commit `4ced120`, several local patches |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15 | BSD-1-clause (ChaN) | from ThumbyP8 |
| [Pemsa](https://github.com/egordorichev/pemsa) 3×5 font glyphs | MIT | transcribed |

ThumbyNES is itself **GPLv2** to remain compatible with the cores.
Both vendored cores live under `vendor/` with patches recorded in
`vendor/VENDORING.md`.

---

## Non-goals

Explicit scope cuts to protect the RAM/CPU budget:

- **No save states.** Battery-backed cart RAM only — see above.
- **No FDS, no VRC6 / VRC7 / MMC5 expansion audio (NES).**
- **No NES 2.0 extended-header support.**
- **No ColecoVision / SG-1000.** smsplus supports them but we don't expose them.
- **No YM2413 FM (SMS Japanese carts).** smsplus has it; off for now.
- **No netplay / link cable.**
- **No on-device cheats or Game Genie.**
- **No PWM backlight dimming** (single-GPIO BL line on the Thumby Color).

---

## Credits

- **nofrendo** by Matthew Conte (1998–2000), Neil Stevens, and the
  retro-go maintainers.
- **smsplus** by Charles MacDonald (1998–2007), with additional code
  by Eke-Eke (SMS Plus GX) and the retro-go maintainers.
- **FatFs** by ChaN.
- **Pemsa font** by Egor Dorichev (MIT).
- **ThumbyP8** firmware patterns (LCD driver, PWM audio path, USB MSC
  flow, FAT layout, lobby/picker state machine) — reused and renamed.

The Thumby Color hardware is by [TinyCircuits](https://tinycircuits.com).

---

*ThumbyNES — a pocket NES + SMS + Game Gear with saves, sleep,
screenshots, and a tabbed browser that boots back to where you left
off.*
