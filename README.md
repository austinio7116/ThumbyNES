# ThumbyNES

A bare-metal **NES + Sega Master System + Game Gear + Game Boy (DMG)**
emulator firmware for the **TinyCircuits Thumby Color** (RP2350,
128×128 RGB565 LCD, PWM audio, 520 KB SRAM, 16 MB flash).

Drop `.nes`, `.sms`, `.gg`, or `.gb` ROMs onto the device over USB,
browse them in a tabbed picker with thumbnail screenshots, play with
sound. Per-ROM saves and **save states**, per-ROM and global settings,
in-game pause menu, idle sleep, fast-forward, palettes, in-game
screenshot capture, live-pan read mode for the handhelds, automatic
FAT defragmenter, configurable system clock — all in a single
~1.1 MB firmware image.

[`firmware/nesrun_device.uf2`](firmware/nesrun_device.uf2) is committed
to the repo if you want to flash without setting up the toolchain.

---

## Quickstart

1. **Flash the firmware.** Power off the Thumby Color, hold the
   **DOWN** d-pad, power back on. The device mounts as `RPI-RP2350`.
   Drag `firmware/nesrun_device.uf2` onto it. The device auto-reboots
   into ThumbyNES.

2. **First boot** wipes the disk to a fresh FAT16 volume labelled
   `THUMBYNES` (a yellow splash flashes briefly).

3. **Drop ROMs.** Plug the device into a host. It enumerates as a
   removable drive — copy any number of `.nes`, `.sms`, `.gg`, or
   `.gb` files into the root. Eject from the host. The device flushes
   the cache and the picker rescans.

4. **Pick + play.** Browse with the **D-pad**, shoulder buttons
   switch tabs, **A** to launch.

5. **Hold MENU at boot** to force-reformat the FAT volume (only takes
   effect if the volume can't otherwise be mounted).

6. **Hold B at boot** to force the FAT defragmenter to run (normally
   it auto-runs only when needed).

---

## The picker

The browser is the resting state of the device — there's no separate
"main menu". When you exit a game (Quit from the in-game menu) you
land back here. Pressing MENU in the picker opens the **picker menu**
overlay; the picker itself only ever exits via launching a ROM.

### Layout: tabs + two views

A **tab strip** runs across the top of the picker:

```
[★ FAV] [NES] [SMS] [GB] [GG]
```

Each tab shows a procedurally-drawn platform icon (controller for
NES, cartridge for SMS, Game Boy silhouette for GB, handheld pill for
GG, star for favorites) and the ROM count for that tab. Empty tabs
are skipped automatically when stepping with the shoulder buttons.

Two views, swapped from the picker menu (`Display: HERO / LIST`):

- **Hero view** (default) — one ROM per screen, 64×64 thumbnail
  centred under the tab strip, large 2×-scaled ROM title (auto-marquees
  if it doesn't fit), meta line, favorite indication via yellow title
  text, sort badge in the title row, position counter and prev/next
  arrows along the bottom.
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
| **B tap** (< 300 ms) | toggle favorite (highlighted ROM goes yellow) |
| **B hold** (≥ 5 s) | red `DELETE ROM?` confirmation overlay with countdown |
| **B hold** (≥ 10 s) | actually delete the ROM + all its sidecars |
| **MENU tap** | open the **picker menu** |

The picker is the resting state — there is no exit-to-lobby chord.
Once a ROM is on disk you stay in the picker until you launch
something.

### Picker menu

Tap **MENU** in the picker to open a full-screen overlay listing
system-wide settings, current device status, and one-shot actions:

| Item | Kind | Notes |
|---|---|---|
| Resume | Action | close the overlay |
| Volume | Slider 0..30 | global master volume — see [Audio](#audio) below |
| Overclock | Choice | global system clock: 125 / 150 / 200 / 250 MHz, takes effect on next launch |
| Display | Choice HERO / LIST | swaps the picker layout |
| Sort | Choice ALPHA / FAVS / SIZE | favs-first puts your starred carts on top, size sorts descending |
| Battery | Info row | live percent + voltage; flips to `CHRG` when external power is detected; bar strip shows level |
| Storage | Info row | `<used>.<dec>/<total>.<dec> MB`; bar strip shows used fraction |
| Defragment now | Action | manual trigger of the same FAT defragmenter that runs at boot |
| About | Info row | firmware identifier |

D-pad UP/DOWN walks the items, LEFT/RIGHT changes values for
sliders / choices, A activates Action items, B or MENU closes the
menu without further action. The frozen picker frame stays visible
behind a darkened overlay so you have context.

### Per-tab selection memory

The picker remembers which ROM you had highlighted on every tab.
Switching tabs preserves the highlight on the tab you left and
restores the highlight on the tab you arrive at. Launching a ROM
also remembers the same selection so when you exit the game you
come back to the cart you just played, in the tab you launched it
from. Persisted across reboots in `/.picker_view`.

### Deleting ROMs in the picker

Holding **B** on the highlighted ROM is the explicit-confirmation
delete path: short taps still toggle the favorite, and you have to
sit on the button for a full 10 seconds for anything to actually
happen.

- **0 .. 300 ms** — released here → toggle favorite (no overlay).
- **300 ms .. 5 s** — released here → cancelled, no effect.
- **5 s mark** — a full-screen red `DELETE ROM?` overlay drops in
  with a `release B to cancel` hint and a `in N` countdown updated
  every frame. Releasing B at any point before the 10 s mark aborts
  with no side effects.
- **10 s mark** — the highlighted ROM file is unlinked along with
  its `.sav`, `.cfg`, `.scr32`, `.scr64` and `.sta` sidecars; the
  favorite entry is removed if present; the picker re-scans the
  volume, reseats the selection on the closest surviving entry, and
  flashes a brief `deleted` OSD. Deleting the last ROM bounces the
  picker back to the lobby's no-roms splash.

### Live USB rescan

The picker watches USB MSC activity and rescans the FAT volume
whenever host writes go quiet for ≥ 400 ms. Files added or deleted
via the host filesystem appear / disappear without having to reboot
the device — including the per-tab count badges, the active selection
if it points at a now-deleted entry, and the no-roms splash fallback
when the volume is emptied.

### Persistence

| File | Purpose |
|---|---|
| `/.picker_view` | Active view, tab, sort mode, **plus the per-tab last-selected ROM names**. |
| `/.global` | Global master volume + global overclock value. Changed from any in-game menu or the picker menu. |
| `/.favs` | Plain newline-separated list of favorited base ROM names (system-agnostic). Survives reboots, editable from a host over USB. |

### Long filenames

The picker stores up to **96-character** ROM names so full no-intro
/ GoodNES tags like `Super Mario Land 2 - 6 Golden Coins (USA, Europe).gb`
fit without truncation. The path buffers in every runner are sized
for `name + 16` so the leading `/` and any sidecar suffix
(`.scr32`, `.scr64`, `.cfg`, `.sav`, `.sta`) round-trip intact.

---

## Controls in-game

### Cart inputs

| Thumby button | NES | SMS | Game Gear | Game Boy |
|---|---|---|---|---|
| **A** (right face) | A | Button 2 | Button 2 | A |
| **B** (left face)  | B | Button 1 | Button 1 | B |
| **D-pad**          | D-pad | D-pad | D-pad | D-pad |
| **LB**             | Select | — | — | Select |
| **RB**             | Start | Pause | Start | Start |

### MENU chords during play

The in-game chord set is now small — most settings live in the
in-game menu (open with MENU long-hold) instead.

| Gesture | Action |
|---|---|
| **MENU tap** (< 300 ms, no chord) | Toggle FIT ↔ CROP scaling |
| **MENU long hold** (≥ 500 ms, no chord) | Open the **in-game menu** |
| **MENU + A** | Save a screenshot (32×32 + 64×64 sidecars) |
| **MENU + dpad** *(GG / GB only)* | Pan the live CROP viewport while the cart keeps running |

### At boot

| Gesture | Action |
|---|---|
| **MENU held** | Force a FAT reformat if the volume can't be mounted. |
| **B held**    | Force the FAT defragmenter pass even if the pre-flight thinks nothing's fragmented. |

---

## In-game menu

Hold MENU for ≥ 500 ms to open the in-game menu in any runner. The
cart freezes, audio stops, and the menu draws over a darkened copy
of the last frame. Items vary slightly per system but the core set
is shared:

| Item | Available on | Notes |
|---|---|---|
| Resume | all | close the menu, unfreeze the cart |
| Save state | all | write `<rom>.sta` next to the ROM |
| Load state | all (greyed when no `.sta`) | restore from `<rom>.sta` |
| Display | all | FIT ↔ CROP — the same toggle as the MENU short tap |
| Volume | all | Slider 0..30 — **global** value, also in the picker menu |
| Fast-fwd | all | toggle 4× speed |
| Show FPS | all | toggle the on-screen FPS overlay |
| BLEND | NES, SMS | toggle 2×2 box-average smoothing |
| Palette | NES (6 choices), GB (6 choices) | per-cart |
| Region | NES | NTSC ↔ PAL — takes effect on the next launch |
| Overclock | all | per-cart override: `global` or 125/150/200/250 MHz, takes effect on the next launch |
| Quit to picker | all | exit the cart |

The menu controls (inside the overlay):

| Key | Action |
|---|---|
| **UP / DOWN** | Move cursor (skips disabled and Info rows) |
| **LEFT / RIGHT** | Adjust slider / choice / toggle value |
| **A** | Activate Action items, also toggles bool items |
| **B** or **MENU** | Close menu without further action |

Volume changes write directly to `/.global` so the new value is
immediately visible across runners and to the picker menu. Overclock
changes write to the cart's `.cfg` (the per-cart override) and take
effect on the **next launch** of any cart — the runner doesn't
reinit the system clock mid-cart because audio and LCD timing are
configured against it.

---

## Audio

The PWM driver runs at 22050 Hz, 9-bit. The runners apply software
volume scaling per-frame:

- `volume == 0` → silence
- `volume == 15` (default) → unity passthrough
- `volume == 30` (max) → 2× with hard clipping at the int16 boundary

Volume is **global** — one value across every cart, stored in
`/.global` as `volume`. The in-game menu and the picker menu both
adjust the same value. (Per-cart `.cfg` files keep a `volume` byte
for binary compatibility, but the load path always overwrites it
with the global value.)

The 2× ceiling has plenty of headroom on most carts because the
nofrendo / smsplus / minigb_apu cores leave their output well below
±32767. Particularly heavy SMS chiptunes can clip at the top of the
range; back the slider off if you hear distortion.

---

## Overclock

The system clock is configurable across four values:

| Value | Notes |
|---|---|
| **125 MHz** | Lowest power; may struggle to hit 60 fps on heavier NES / SMS carts |
| **150 MHz** | RP2350 stock-ish |
| **200 MHz** | |
| **250 MHz** | Default; the original fixed clock |

Two scopes:

- **Global** (picker menu → Overclock) — written to `/.global`,
  applies to any cart that doesn't have a per-cart override.
- **Per-cart** (in-game menu → Overclock) — written to the cart's
  `.cfg` as a `clock_mhz` field. The first choice is `global` which
  clears the override; the others (125/150/200/250) pin that
  specific cart to that exact clock.

Resolution order at launch time: **per-cart override → global →
250 MHz default.**

Both kinds of change take effect on the **next ROM launch**. The
runner doesn't reinit the system clock mid-cart because that would
require tearing down the LCD SPI dividers and audio PWM IRQ rate
mid-stream. The dispatcher (`nes_device_main` between picker and
runner) reads the saved value, calls `set_sys_clock_khz()`, and
re-runs `nes_lcd_init()` + `nes_audio_pwm_init()` so the new
peripherals come up against the new system clock.

---

## Save state

Two persistence layers:

### Battery save (cart RAM)

Battery-backed cart RAM is persisted to `<romname>.sav` next to the
ROM in the FAT root. Loaded on launch, written on exit to picker,
and **auto-saved every 30 seconds** of gameplay so a flat battery
never costs you more than half a minute.

- **NES**: nofrendo's PRG-RAM (8 KB typical)
- **SMS / GG**: smsplus's `cart.sram` (32 KB)
- **Game Boy**: peanut_gb's `cart_ram`, sized via `gb_get_save_size_s`
  per cart (0..32 KB depending on MBC)

ROMs without a battery flag in their header are unaffected.

### Save states

Independent of the battery save: each runner can serialize its full
core state to a single `<rom>.sta` sidecar via the in-game menu's
**Save state** / **Load state** items. One slot per cart.

| Core | Surface | Mechanism |
|---|---|---|
| **nofrendo** | full machine state via its `state_save` / `state_load` | uses an `SNSS`-format file written through the FatFs bridge |
| **smsplus** | full machine state via its `system_save_state` / `system_load_state` | same FatFs bridge |
| **peanut_gb** | `struct gb_s` + `minigb_apu_ctx` (~17 KB) | direct memcpy with a `'GBCS'` header (magic / version / size); function pointers re-attached on load |

Both retro-go cores use libc stdio internally for the save/load
calls. On the device build the relevant `fopen` / `fwrite` / `fread`
/ `fseek` / `fclose` calls in `state.c` are remapped to a tiny FatFs
shim (`device/thumby_state_bridge.[ch]`) via a top-of-file `#ifdef
THUMBY_STATE_BRIDGE` block. The host build still uses real stdio
without any change.

When the menu is open the runner is fully paused — frame, audio and
input are all suspended — so saves and loads are atomic relative to
the cart's view of the world.

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
you know the capture landed.

---

## Display modes

Every cart launches in **FIT** mode regardless of any saved
preference. CROP is a transient mid-session toggle — the runner
intentionally does not persist scale mode in the `.cfg` sidecar, so
launching a cart never traps you in a stale CROP setting.

### FIT (default)

The entire native frame is downscaled to fit the 128×128 display.

| System | Native | FIT scaled to | Notes |
|---|---|---|---|
| **NES** | 256×240 | 128×120 | 4 px black bars top + bottom |
| **SMS** | 256×192 | 128×96  | 16 px black bars top + bottom |
| **Game Gear** | 160×144 | 128×128 | asymmetric 5:4 / 9:8 nearest, **fills the whole screen** |
| **Game Boy** | 160×144 | 128×128 | asymmetric 5:4 / 9:8 nearest, **fills the whole screen** |

With **BLEND** on (the NES/SMS default) each output pixel is a
2×2 box average of four source pixels — softer image, no
nearest-neighbor shimmer. Toggle BLEND from the in-game menu.

### CROP

A 1:1 native viewport into the cart frame, pannable across the full
picture. Two flavours:

- **Pause-on-CROP** (NES + SMS): tap MENU to enter CROP. The cart
  pauses entirely, audio mutes, and the **D-pad pans the viewport**
  across the full picture. Tap MENU again to return to FIT. Designed
  for reading text or stepping away.
- **Live-pan CROP** (GB + GG): tap MENU to enter CROP. The cart keeps
  running, audio keeps playing, and the **D-pad still goes to the
  game**. **MENU + dpad** pans the viewport while held. Designed
  for the handhelds where reading menus while the cart breathes is
  the point.

The CROP pan range is whatever the source frame allows: NES has 128
horizontal × 112 vertical of slack, SMS has 128 × 64, GB and GG both
have 32 × 16.

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

Override from the in-game menu via the Region row — flips between
NTSC (60 Hz, 1.79 MHz CPU) and PAL (50 Hz, 1.66 MHz CPU). Persisted
per-ROM and **takes effect on the next launch**.

### SMS / GG

smsplus does its own region work from the ROM header (byte at 0x7FFF)
and an internal CRC database, so the runner just trusts whatever it
reports — no manual override needed in practice.

### Game Boy

DMG runs at exactly 59.7275 Hz everywhere — no region switch.

---

## Palettes

### NES

Six palettes from nofrendo: `NOFRENDO`, `COMPOSITE` (default),
`NESCLASSIC`, `NTSC`, `PVM`, `SMOOTH`. Cycle from the in-game menu's
Palette row. Persisted per-ROM in `.cfg`.

### Game Boy

Six 4-shade palettes: `GREEN` (the classic Game Boy LCD green,
default), `GREY`, `POCKET`, `CREAM`, `BLUE`, `RED`. Cycle from the
in-game menu's Palette row. Persisted per-ROM.

### SMS / GG

No palette toggle — those cores manage their own VDP palette directly.

---

## Idle sleep

After 90 s of no input the device commits a battery save, blanks the
LCD backlight, and drops to a tight sleep loop. Press any button to
wake. Frame pacing is re-anchored on wake so the cart picks up at
the correct refresh rate (instead of running flat-out to "catch up"
with the time spent asleep).

---

## Fast-forward

Toggle from the in-game menu's Fast-fwd row. The frame-rate cap is
bypassed and four cart frames are stepped per outer iteration; the
renderer and audio still only present the most recent frame so the
audio ring doesn't overflow.

---

## Battery monitor

The Thumby Color exposes the battery through a 1:2 resistor divider
into GPIO 29 / ADC channel 3. The picker menu's Battery row shows
live percent and voltage (e.g. `87% 3.85V`); when the device is
plugged in and the ADC reads above the cell's max threshold the row
flips to `CHRG <voltage>`. A thin green strip below the row visualises
the percent.

Thresholds match the engine's reference implementation so behaviour
on hardware is identical between ThumbyNES and the engine builds.

---

## In-firmware defragmenter

Large ROMs (≳ 256 KB) need to be loaded via the **XIP mmap** path
because the malloc-into-RAM fallback can't fit them. XIP mmap requires
the file to be physically contiguous on flash, but after a few rounds
of dropping ROMs over USB and saving screenshots the FatFs free-list
can fragment, producing a red `load err -35` splash on bigger carts.

The firmware ships with an in-place defragmenter that uses FatFs's
`f_expand` to allocate contiguous cluster chains.

- **Auto pre-flight** at every cold boot — walks `/`, checks each
  non-system file > 64 KB with the same `chain_is_contiguous()` probe
  the XIP mmap path uses, and if any are fragmented runs the rewrite
  pass. The pre-flight shows a 3-line diagnostic for ~0.8 s on every
  boot:

  ```
  checking files
  47 files / 23 big
  0 need defrag        ← yellow if non-zero, green if all clear
  ```

- **Rewrite pass** for each victim:

  ```
  f_open(src, READ)
  f_open(/.defrag.tmp, WRITE | CREATE_NEW)
  f_expand(dst, size, 1)              -- pre-allocate contiguous chain
  stream src -> dst, 4 KB at a time   -- with progress overlay
  f_unlink(src)
  f_rename(/.defrag.tmp, src)
  nes_flash_disk_flush()
  ```

  A full-screen `DEFRAGMENTING / do not unplug / N/total / filename /
  progress bar` overlay redraws every 64 KB so the user sees movement
  on the bigger ROMs. tud_task() pumps between files so USB
  enumeration stays alive. A previously interrupted defrag leaves
  `/.defrag.tmp` behind; the per-file step `f_unlink`s any leftover
  before allocating a new one.

- **Manual triggers**:
  - Hold **B at boot** — forces the pass to run even if the
    pre-flight thinks no large file is fragmented.
  - Pick **Defragment now** in the picker menu — same pass, no
    reboot required.

- **Skipped files**: anything < 64 KB and any system bookkeeping file
  (`/.favs`, `/.picker_view`, `/.global`, `/.defrag.tmp`, sidecars).

---

## Hardware target

| | |
|---|---|
| **MCU** | RP2350 dual-core Cortex-M33 @ 125 / 150 / 200 / 250 MHz |
| **Display** | 128×128 RGB565, GC9107 LCD over SPI + DMA |
| **Audio** | 9-bit PWM on GP23 @ 22050 Hz sample rate, hardware IRQ-driven ring buffer |
| **Storage** | 12 MB FAT16 volume on internal QSPI flash, exposed as USB MSC |
| **Input** | A, B, D-pad, LB, RB, MENU |
| **Battery** | 1:2 divider on GPIO 29, ADC channel 3 |

---

## Architecture

### Multi-core dispatch

The picker tags each ROM entry with a `system` field at scan time
(extension based: `.nes` → NES, `.sms` → SMS, `.gg` → GG, `.gb` → GB).
When the user launches a ROM the device main loop dispatches to one
of `nes_run_rom` (nofrendo), `sms_run_rom` (smsplus) or `gb_run_rom`
(peanut_gb). Three parallel runners share every hardware driver —
LCD, audio PWM, buttons, FAT sidecars, idle-sleep timer — and only
differ in the per-frame core calls and the scaling routines.

The cores never run concurrently. Static per-core BSS state coexists
in SRAM but only one runner is active at a time, so the heap budget
only has to satisfy whichever cart is currently loaded.

Between picker and runner, the dispatcher applies the per-cart
overclock override (or the global default), re-running
`nes_lcd_init()` + `nes_audio_pwm_init()` if the system clock changed
so the LCD SPI dividers and audio IRQ rate pick up the new sys_clock.

### In-game menu

`device/nes_menu.[ch]` is a small reusable modal UI module. Each
runner builds an item list with pointers into its own state, hands
the list to `nes_menu_run()`, and gets back either `NES_MENU_RESUME`
or `NES_MENU_ACTION` with a per-item `action_id`. Item kinds:

- `ACTION` — buttons (Resume, Save state, Load state, Quit, ...)
- `TOGGLE` — bool flipped by LEFT/RIGHT or A
- `SLIDER` — int with min/max and a horizontal bar
- `CHOICE` — int index into a named array
- `INFO` — non-interactive label + value text, optional thin bar strip
  along the bottom of the row (used for Battery and Storage rows)

The frozen game frame is darkened to ~25 % brightness in place
(channel-correct extract / `>> 2` / repack) so the menu has contrast
without losing context. Items > 9 scroll with up / down chevrons in
the title bar.

### State save bridge

`device/thumby_state_bridge.[ch]` exposes
`thumby_state_open / read / write / seek / close` against FatFs's
`FIL` handle. The vendored `nofrendo/nes/state.c` and
`smsplus/state.c` are minimally patched to switch their stdio calls
through `STATE_OPEN` / `STATE_WRITE` / etc. macros that expand to
either the bridge functions (when compiled with `-DTHUMBY_STATE_BRIDGE`)
or libc stdio (host build). A single static `FIL` instance is enough
because save and load are mutually exclusive.

### Video pipeline

The Thumby Color is 128×128. Each runner has its own scaler set:

**NES** (`nes_run.c`)
- **`blit_fit`** — 256×240 → 128×120 nearest, 4 px letterbox.
- **`blit_blend`** — 256×240 → 128×120 with 2×2 box average.
- **`blit_crop`** — 1:1 native 128×128 window with D-pad pan.

**SMS / GG** (`sms_run.c`)
- **`blit_fit_sms`** — 256×192 → 128×96 nearest, 16 px letterbox.
- **`blit_blend_sms`** — 256×192 → 128×96 with 2×2 box average.
- **`blit_crop_sms`** — 1:1 native crop with pan (SMS only).
- **`blit_fit_gg`** — 160×144 → 128×128 with asymmetric 5:4 / 9:8
  nearest. Fills the whole screen with no letterbox.
- **`blit_crop_gg`** — 1:1 native crop into a 128×128 window of the
  160×144 viewport with live pan.

**Game Boy** (`gb_run.c`)
- **`blit_fit_gb`** — 160×144 → 128×128 with asymmetric 5:4 / 9:8
  nearest. Same trick as the GG fit blitter.
- **`blit_crop_gb`** — 1:1 native crop with live pan.

### Audio pipeline

All three cores produce signed 16-bit mono samples at 22050 Hz,
matching the PWM driver's IRQ rate. Each runner applies the global
volume scale around `VOL_UNITY = 15` (1.0×) up to `VOL_MAX = 30`
(2.0× with hard clipping at the int16 boundary).

- **nofrendo** emits mono natively.
- **smsplus** produces stereo (PSG L + R); the wrapper averages
  L and R per sample.
- **peanut_gb + minigb_apu** produce stereo (4-channel DMG APU);
  the wrapper averages L and R per sample.

In NES + SMS CROP mode the audio path is skipped entirely so the
speaker goes silent rather than holding the last frame's samples.
GG + GB live-pan CROP keeps audio flowing.

### Memory budget

| Region | Bytes |
|---|---|
| nofrendo internal state (CPU/PPU/APU/mappers) | ~30 KB BSS |
| smsplus internal state (CPU/VDP/PSG + LUTs)   | ~80 KB BSS |
| peanut_gb gb_s (WRAM/VRAM/regs) + minigb_apu state | ~30 KB BSS |
| Per-session vidbuf (NES 65 KB / SMS 49 KB / GB 23 KB) | malloc'd in init |
| GB cart_ram (32 KB max)                        | malloc'd in init |
| SMS cart.sram (32 KB) + vram (16 KB) + wram (8 KB) | smsplus heap |
| Thumby Color framebuffer (128×128 RGB565)      | 32 KB BSS |
| Menu backdrop snapshot (32 KB)                  | BSS, only used while menu is open |
| Audio ring (4096 samples × 2 bytes)            | 8 KB BSS |
| FatFs work area + flash disk write cache       | ~12 KB BSS |
| Picker / favorites / cfg / view pref / defrag snapshot | ~10 KB BSS |
| ROM (XIP-mapped from flash for everything ≥ 256 KB) | ≤ 8 MB |
| **Free heap (typical session)** | **~330 KB** |

Per-core source framebuffers were originally in BSS but are now
malloc'd in each wrapper's `init()` and freed in `shutdown()`. Net
effect: the SRAM cost of the inactive cores' framebuffers (~137 KB)
is freed up as heap during the active session.

### Performance

The nofrendo and smsplus hot inner loops are placed in SRAM via
`IRAM_ATTR` / `__not_in_flash_func`, mapped to `.time_critical.*`
section attributes in the device build. The Pico SDK linker copies
those into RAM at boot, so the hot dispatch loops dodge XIP flash
cache miss penalties entirely. Everything else still executes from
XIP.

NES Final Fantasy (MMC1, 256 KB PRG), SMS Sonic the Hedgehog and
Game Boy Tetris all run full speed at the default 250 MHz with audio
glitch-free. Lighter carts can be clocked down via the per-cart or
global Overclock setting to save power.

---

## Building

### Host (SDL2 runners — for development on Linux/macOS)

Prereqs: `cmake`, a C compiler, `libsdl2-dev`.

```bash
cmake -B build
cmake --build build -j8

./build/neshost /path/to/rom.nes        # NES interactive SDL window
./build/smshost /path/to/rom.sms        # SMS / GG interactive SDL window
./build/gbhost  /path/to/rom.gb         # Game Boy interactive SDL window

./build/nesbench /path/to/rom.nes       # headless NES benchmark
./build/smsbench /path/to/rom.sms       # headless SMS / GG benchmark
./build/gbbench  /path/to/rom.gb        # headless Game Boy benchmark
```

Host controls (all runners): `Z`=A/B1, `X`=B/B2, `Enter`=Start,
`RShift`=Select/Pause, arrows=D-pad, `Esc`=quit. The GB host runner
also accepts `1`..`6` to cycle palettes.

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
│   ├── smsplus/             ← SMS / GG core, GPLv2 (retro-go fork, patched)
│   └── peanut_gb/           ← Game Boy core + minigb_apu, MIT (no patches)
├── src/                     ← cross-platform glue
│   ├── nes_core.[ch]        ← thin wrapper around nofrendo's API + state save
│   ├── sms_core.[ch]        ← thin wrapper around smsplus's API + state save
│   ├── gb_core.[ch]         ← thin wrapper around peanut_gb's API + state save
│   ├── host_main.c          ← NES SDL2 host runner
│   ├── sms_host_main.c      ← SMS / GG SDL2 host runner
│   ├── gb_host_main.c       ← Game Boy SDL2 host runner
│   ├── bench_main.c         ← NES headless benchmark
│   ├── sms_bench_main.c     ← SMS / GG headless benchmark
│   └── gb_bench_main.c      ← Game Boy headless benchmark
└── device/                  ← Thumby Color firmware
    ├── CMakeLists.txt       ← Pico SDK device build
    ├── nes_device_main.c    ← entry, lobby, defrag pre-flight, picker dispatch, clock apply
    ├── nes_run.[ch]         ← NES runner (input + scaler + audio + autosave + sleep + menu)
    ├── sms_run.[ch]         ← SMS / GG runner (mirror of nes_run for smsplus)
    ├── gb_run.[ch]          ← Game Boy runner (mirror of nes_run for peanut_gb)
    ├── nes_picker.[ch]      ← tabbed picker UI + extension scanner + defragmenter + picker menu
    ├── nes_menu.[ch]        ← reusable in-game / picker menu module
    ├── nes_thumb.[ch]       ← procedural icons + screenshot sidecar I/O
    ├── nes_battery.[ch]     ← ADC battery monitor (voltage / percent / charging)
    ├── nes_font.[ch]        ← 3×5 bitmap font + 2× scaled draw helper
    ├── nes_lcd_gc9107.[ch]  ← GC9107 SPI/DMA LCD driver + backlight
    ├── nes_buttons.[ch]     ← GPIO button reader
    ├── nes_audio_pwm.[ch]   ← PWM audio output + sample IRQ
    ├── nes_flash_disk.[ch]  ← flash-backed disk + RAM write-back cache
    ├── thumby_state_bridge.[ch]  ← FatFs-backed stdio shim for vendored state.c
    ├── nes_msc.c            ← TinyUSB MSC class callbacks
    ├── usb_descriptors.c    ← TinyUSB device + composite descriptors
    ├── tusb_config.h        ← TinyUSB compile config
    └── fatfs/               ← vendored FatFs R0.15 (BSD-1, ChaN)
```

### Files written to the FAT volume

| File | Purpose |
|---|---|
| `<rom>.nes` / `.sms` / `.gg` / `.gb` | The ROM image you dropped via USB. |
| `<rom>.sav`     | Battery-backed cart RAM. Auto-saved every 30 s. |
| `<rom>.cfg`     | Per-ROM BLEND / palette / FPS / region / **per-cart overclock** state (system-tagged magic). |
| `<rom>.sta`     | Save state — full serialized core state (NES/SMS use the core's native format, GB uses a `'GBCS'`-tagged memcpy blob). |
| `<rom>.scr32`   | 32×32 RGB565 thumbnail (list view). |
| `<rom>.scr64`   | 64×64 RGB565 thumbnail (hero view). |
| `/.favs`        | Newline-separated list of favorited ROM names. |
| `/.picker_view` | Persisted picker view + active tab + sort mode + per-tab last-selected ROM names. |
| `/.global`      | Global master volume + global overclock value. |
| `/.defrag.tmp`  | Transient — only present mid-defrag if the device was unplugged during a rewrite. |

---

## Vendored sources

| Component | License | Source |
|---|---|---|
| [nofrendo](https://github.com/ducalex/retro-go) NES core | GPLv2 | retro-go @ commit `4ced120`, with the IRAM_ATTR + state-bridge patches |
| [smsplus](https://github.com/ducalex/retro-go) SMS / GG core | GPLv2 | retro-go @ commit `4ced120`, with the LUT decomposition + state-bridge patches |
| [Peanut-GB](https://github.com/deltabeard/Peanut-GB) DMG core | MIT | via TinyCircuits Tiny Game Engine `gbemu/`, no patches |
| [minigb_apu](https://github.com/baines/MiniGBS) Game Boy APU | MIT | via TinyCircuits Tiny Game Engine `gbemu/`, no patches |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15 | BSD-1-clause (ChaN) | from ThumbyP8 |
| [Pemsa](https://github.com/egordorichev/pemsa) 3×5 font glyphs | MIT | transcribed |

ThumbyNES is itself **GPLv2** to remain compatible with the nofrendo
+ smsplus cores. Both retro-go cores live under `vendor/` with
patches recorded in `vendor/VENDORING.md`. The peanut_gb + minigb_apu
sources are vendored verbatim from the engine's GBEmu user-module
(which itself wraps the upstream MIT projects).

---

## Non-goals

Explicit scope cuts to protect the RAM/CPU budget:

- **No multiple save state slots.** One `.sta` per cart.
- **No FDS, no VRC6 / VRC7 / MMC5 expansion audio (NES).**
- **No NES 2.0 extended-header support.**
- **No ColecoVision / SG-1000.** smsplus supports them but we don't expose them.
- **No YM2413 FM (SMS Japanese carts).** smsplus has it; off for now.
- **No Game Boy Color.** peanut_gb is DMG only — GBC-only carts will
  fail to load with `load err -11` (cartridge type unsupported).
- **No netplay / link cable.**
- **No on-device cheats or Game Genie.**
- **No PWM backlight dimming** (single-GPIO BL line on the Thumby Color).

---

## Credits

- **nofrendo** by Matthew Conte (1998–2000), Neil Stevens, and the
  retro-go maintainers.
- **smsplus** by Charles MacDonald (1998–2007), with additional code
  by Eke-Eke (SMS Plus GX) and the retro-go maintainers.
- **Peanut-GB** by Mahyar Koshkouei (deltabeard).
- **minigb_apu / MiniGBS** by Alex Baines.
- **FatFs** by ChaN.
- **Pemsa font** by Egor Dorichev (MIT).
- **TinyCircuits Tiny Game Engine GBEmu** — source-of-truth for the
  Peanut-GB + minigb_apu vendoring + many of the wrapper patterns.
- **ThumbyP8** firmware patterns (LCD driver, PWM audio path, USB MSC
  flow, FAT layout, lobby/picker state machine) — reused and renamed.

The Thumby Color hardware is by [TinyCircuits](https://tinycircuits.com).

---

*ThumbyNES — a pocket NES + SMS + Game Gear + Game Boy with battery
saves, save states, an in-game pause menu, screenshots, in-firmware
defragmenter, configurable system clock, and a tabbed browser that
boots back to where you left off.*
