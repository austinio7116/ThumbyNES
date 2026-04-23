# ThumbyNES

> 🎮 **ThumbyNES is now part of [ThumbyOne](https://github.com/austinio7116/ThumbyOne)** — a unified multi-boot firmware that ships ThumbyNES, ThumbyP8 (PICO-8), ThumbyDOOM, and MicroPython + Tiny Game Engine in a single UF2 with one shared USB drive for ROMs, carts, and Python games. Most users should flash ThumbyOne instead of the standalone ThumbyNES firmware below.
>
> This repo remains the standalone build of ThumbyNES and the source of truth for the emulator itself — the code here is what ThumbyOne's NES slot compiles. Use this repo if you specifically want NES-only firmware, or to hack on the emulator code.

A bare-metal **NES + Sega Master System + Game Gear + Game Boy (DMG + Color)
+ Sega Mega Drive / Genesis** emulator firmware for the **TinyCircuits
Thumby Color** (RP2350, 128×128 RGB565 LCD, PWM audio, 520 KB SRAM, 16 MB
flash).

Drop `.nes`, `.sms`, `.gg`, `.gb`, `.gbc`, `.md`, `.gen`, or `.bin` ROMs
onto the device over USB, browse them in a tabbed picker with thumbnail
screenshots, play with sound. Per-ROM saves and **save states**, per-ROM and global
settings, in-game pause menu, idle sleep, fast-forward, palettes,
in-game screenshot capture, live-pan read mode for the handhelds,
**cluster-level FAT defragmenter with live cluster map**,
**chained-XIP fallback** so fragmented carts still load at full
speed, configurable system clock — all in a single ~1.2 MB
firmware image.

<p align="center">
<img src="images/nes-smb3.jpg" width="360" alt="NES — Super Mario Bros. 3 title screen">
<img src="images/sms-alex-kidd.jpg" width="360" alt="SMS — Alex Kidd in Miracle World">
</p>
<p align="center">
<img src="images/gb-super-mario-land.jpg" width="360" alt="Game Boy — Super Mario Land">
<img src="images/sms-sonic-fit.jpg" width="360" alt="SMS — Sonic the Hedgehog (FIT mode)">
</p>

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
   removable drive — copy any number of `.nes`, `.sms`, `.gg`, `.gb`,
   `.gbc`, `.md`, `.gen`, or `.bin` files into the root. Eject from the
   host. The device flushes the cache and the picker rescans.

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
[★ FAV] [NES] [SMS] [GB] [GG] [MD]
```

Each tab shows a procedurally-drawn platform icon (controller for
NES, cartridge for SMS, Game Boy silhouette for GB, handheld pill for
GG, 6-button pad silhouette for MD, star for favorites) and the ROM
count for that tab. Empty tabs are skipped automatically when stepping
with the shoulder buttons.

Two views, swapped from the picker menu (`Display: HERO / LIST`):

<p align="center">
<img src="images/picker-hero.jpg" width="360" alt="Picker — Hero view">
<img src="images/picker-list.jpg" width="360" alt="Picker — List view">
</p>

- **Hero view** (default) — one ROM per screen, 64×64 thumbnail
  centred under the tab strip, large 2×-scaled ROM title (auto-marquees
  if it doesn't fit), small dim meta line, **the active tab name in
  the larger 2× font** (`NES`, `MASTER SYSTEM`, `GAME BOY`, `GAME GEAR`,
  `FAVORITES`), favorite indication via yellow title text, sort badge
  in the title row, position counter with prev/next arrows along the
  very bottom.
- **List view** — three rows per screen, each row a 32×32 thumbnail
  next to the ROM name and meta. Highlighted row glows green; the
  selected row's position-in-tab appears as a third line.

The thumbnails are screenshots you've captured in-game (see below).
ROMs without a screenshot fall back to a procedurally-drawn
placeholder: the platform icon centred on a tinted panel.

### Picker controls

| Key | Action |
|---|---|
| **LEFT / RIGHT / UP / DOWN** | prev / next ROM (any D-pad direction; wraps at both ends) |
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

<p align="center">
<img src="images/menu-overlay.jpg" width="360" alt="Picker menu overlay">
</p>

| Item | Kind | Notes |
|---|---|---|
| Resume | Action | close the overlay |
| Volume | Slider 0..30 | global master volume — see [Audio](#audio) below |
| Overclock | Choice | global system clock: 125 / 150 / 200 / 250 / 300 MHz, takes effect on next launch |
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

| Thumby button | NES | SMS | Game Gear | Game Boy | Mega Drive |
|---|---|---|---|---|---|
| **A** (right face) | A | Button 2 | Button 2 | A | B (jump/light attack) |
| **B** (left face)  | B | Button 1 | Button 1 | B | A |
| **D-pad**          | D-pad | D-pad | D-pad | D-pad | D-pad |
| **LB**             | Select | — | — | Select | Mode |
| **RB**             | Start | Pause | Start | Start | C (heavy attack) |
| **LB + RB** (chord) | — | — | — | — | Start (pulse) |

MD's **Start** is an LB+RB chord because MENU tap is reserved for
scale-mode cycling (FIT / FILL / CROP). MENU+A still saves a screenshot,
and MENU long-hold opens the in-game menu. The Mode button (mapped to
LB) only does anything with 6-button ROMs and can be ignored for
3-button titles.

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
| Overclock | all | per-cart override: `global` or 125/150/200/250/300 MHz, takes effect on the next launch |
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

The **MD runner's Audio menu item** has three settings that trade
synthesis cost for quality:

- **FULL** (default) — YM2612 + PSG + Z80 all at 22050 Hz.
  Reference quality, ~5 ms/frame of FM synthesis cost. Locks 50 PAL
  with some help from adaptive VDP skip-render; NTSC carts typically
  run at ~45 FPS.
- **HALF** — YM2612 at 11025 Hz; samples upsampled via zero-order
  hold in `mdc_audio_pull` for the 22050 Hz PWM path. Halves FM
  cost (~2.5 ms reclaimed). Audible HF roll-off but musical — worth
  trying on NTSC carts where the full path falls short.
- **OFF** — all Z80 + FM + PSG disabled (`POPT_EN_*` stripped from
  `PicoIn.opt` at `mdc_init` time). Completely silent, locks 50 PAL
  or 60 NTSC with zero audio path cost. For twitchy gameplay where
  maximum refresh beats hearing the music.

Choice is per-cart; takes effect on **next launch** because the
sample rate is baked into PicoDrive's `PsndRerate` resampler tables
and the opt flags are checked at `PicoCartInsert` time.

---

## Overclock

The system clock is configurable across five values:

| Value | Notes |
|---|---|
| **125 MHz** | Lowest power; may struggle to hit 60 fps on heavier NES / SMS carts |
| **150 MHz** | RP2350 stock-ish |
| **200 MHz** | |
| **250 MHz** | Default; matches the v1.0–1.03 fixed clock |
| **300 MHz** | Extra headroom for the v1.04 GB / GBC / GG coverage-blend scaler on dense carts. Uses the default core voltage — if a specific cart shows instability at 300, step down to 250. |

Two scopes:

- **Global** (picker menu → Overclock) — written to `/.global`,
  applies to any cart that doesn't have a per-cart override.
- **Per-cart** (in-game menu → Overclock) — written to the cart's
  `.cfg` as a `clock_mhz` field. The first choice is `global` which
  clears the override; the others (125/150/200/250/300) pin that
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

<p align="center">
<img src="images/sms-sonic-fit.jpg" width="360" alt="Sonic — FIT mode (downscaled to 128×128)">
<img src="images/sms-sonic-crop.jpg" width="360" alt="Sonic — CROP mode (1:1 native pixels)">
</p>

The entire native frame is downscaled to fit the 128×128 display.

| System | Native | FIT scaled to | Notes |
|---|---|---|---|
| **NES** | 256×240 | 128×120 | 4 px black bars top + bottom |
| **SMS** | 256×192 | 128×96  | 16 px black bars top + bottom |
| **Game Gear** | 160×144 | 128×128 | asymmetric 5:4 × 9:8, **fills the whole screen** |
| **Game Boy** | 160×144 | 128×128 | asymmetric 5:4 × 9:8, **fills the whole screen** |
| **Mega Drive (H40)** | 320×224 | 128×90 | 19 px black bars top + bottom, 2.5:1 aspect preserved |
| **Mega Drive (H32)** | 256×224 | 128×90 | same letterbox — sprites appear slightly wider |

With **BLEND** on (the default on every system) each output pixel
blends the source pixels that cover its footprint — softer image,
no nearest-neighbour shimmer, readable text in dialogue and menus.
Specifics per system:

- **NES / SMS** — 2×2 box average of four source pixels (integer
  2:1 scaling downstairs).
- **Game Gear / Game Boy (CGB)** — coverage-weighted 2×2 blend in
  RGB565 using the packed-lerp trick, matching the exact 1.25 ×
  1.125 source footprint of each output pixel. No columns or rows
  get dropped.
- **Game Boy (DMG)** — palette-aware blend: fractional shade index
  (0..3) from the four source pixels, then linear interpolation
  between the two bracketing palette entries. Keeps output on the
  chosen DMG palette's own gradient so the classic Nintendo greens
  don't acquire a teal tint in intermediate blends.

Toggle BLEND from the in-game menu — the row is enabled in FIT
mode and greyed in CROP / SMS FILL where it has no meaning.

### FILL (SMS + Mega Drive)

A third MENU-tap cycle position between FIT and CROP, available on
the SMS and MD runners. Fills the full 128×128 display with an
aspect-preserving blit: the native frame is scaled so height=128,
then the sides are cropped to 128 px. No letterbox, sprites stay
proportional — the trade is that action or HUD at the far edges
gets cropped off the visible area.

- **SMS** — 1.5× uniform area-weighted blit of the middle 192×192 of
  the 256×192 source (crops 32 src cols / side). The cart keeps
  playing (unlike SMS CROP which pauses).
- **Mega Drive** — scale factor 128/224 applied to both axes; for
  H40 (320-wide) that shows a centred 224-col window (crops 48 src
  cols / side); for H32 (256-wide) it crops 16 cols / side. Uses
  the same `sx_lut` table as FIT with a different column mapping,
  so the cost is identical.

The trade is visible — for games with action or HUD at the far
edges (scoreboards, end-of-stage indicators, lives counters) you'll
lose some of it — so FIT / BLEND remain the defaults; FILL is
there when you want the maximum playable screen area.

FILL is always area-weighted blended; the BLEND toggle doesn't
apply (there's no nearest-neighbour alternative offered). Game Gear
doesn't expose FILL in the cycle (its existing FIT already fills
the screen via asymmetric scaling).

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

### Mega Drive / Genesis

PicoDrive detects region from the ROM header's J/U/E/A string at
offset `0x1F0`. The auto-detect preference order is **EU → US → JP**
(`PicoIn.autoRgnOrder = 0x148`). PAL carts run at 50 Hz / 224-line
V28 or 240-line V30; NTSC at 60 Hz. Header-less dumps fall back to
the auto-order. No manual override is wired into the MD menu today
— in practice `.md` files from no-intro include the correct region
byte.

**FPS note**: NTSC's 60 Hz target is tight — the per-frame budget is
16.67 ms vs PAL's 20 ms, and the MD core generally sits at ~21-24 ms
of work on busy scenes. PAL carts lock 50 FPS with adaptive VDP skip
(see "Adaptive skip-render" below); NTSC carts typically run at
~44-49 FPS in FULL audio. Try HALF or OFF audio mode, or overclock
to 300 MHz, to close the gap.

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

**GBC carts** ignore the palette setting — they supply their own
per-tile CGB palette from the cart, which peanut_gb resolves
per-pixel at line time into RGB565 for the framebuffer. The Palette
row in the in-game menu stays visible but is inert for `.gbc` files.

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

## Defragmenter

### Why you might (or might not) want to defragment

ThumbyNES runs carts straight out of flash via **XIP mmap** rather
than copying them to RAM. That's how a 1 MB GBC cart fits on a
device with a ~330 KB heap budget: the core reads each byte directly
from its memory-mapped address in flash.

There are two XIP paths:

1. **Contiguous mmap** — the fast, simple path. The file's FAT chain
   is a straight cluster run (e.g. clusters 500→501→502→…→1523) so
   the entire ROM maps to a single flat address range in the XIP
   region. No per-byte indirection; the core has a plain `const
   uint8_t *` pointer into flash.
2. **Chained XIP** — the fallback when the file is fragmented.
   The loader walks the FAT chain, records the XIP address of each
   cluster into a `cluster_ptrs[]` table, and every ROM byte access
   does a shift + mask + table lookup to find the right cluster
   before hitting flash. Still zero-copy (no RAM copy of the cart),
   still backed by XIP, but the per-byte indirection costs CPU on
   every single read the core performs. The table itself is ~4 KB
   of heap for a 1 MB cart.

**Fragmented doesn't mean broken.** Most games stay locked to their
native refresh rate on chained XIP — early NES, Game Boy, most SMS /
GG carts have plenty of CPU headroom to absorb the lookup overhead.
The ones that can drop frames when fragmented are the busy ones:
dense NES mapper carts (MMC3/MMC5 games with big tilemap bandwidth),
some GBC titles with heavy per-scanline palette work, a handful of
SMS titles hammering the PSG. If a fragmented cart feels sluggish,
the defragmenter is the fix.

**What defragmenting actually buys you:**

- **Existing fragmented carts get moved onto the fast path.**
  Every cart on the volume ends up as a contiguous cluster chain,
  so every cart runs through direct mmap with no per-byte
  indirection. If a cart was dropping frames because it was
  fragmented, defrag puts it right.
- **New uploads stay on the fast path too.** Defragging
  consolidates all free space into one run at the end of the
  volume. The next ROM you drop over USB lands in that contiguous
  run, so it starts life as a contiguous file — no fragmentation,
  no chained XIP, no re-defrag needed. Writing a file to a
  fragmented free list *works* (the host's FAT driver is happy to
  chain scattered clusters into a fragmented file), but the
  resulting file is then fragmented itself and needs chained XIP
  to run.
- **Cleaner cluster map.** Satisfying to look at.

**What it doesn't do:** ROMs still load and play correctly whether
the volume is defragmented or not. Chained XIP is the safety net —
you can drop a ROM, play it immediately, and defragment later (or
never) if the cart runs fine either way.

### The cluster-level defragmenter

The defragmenter does an **in-place cluster-level cycle sort** over
the entire FAT volume. This is the same strategy Norton SpeedDisk
and ext4's `e4defrag` use: plan a target layout, then cycle each
cluster into its destination using only two cluster-sized RAM
buffers — regardless of how full the disk is. A file-level approach
(copy-to-scratch + rename) can't operate on a near-full volume
because it needs 2× the file size free at once; cluster-level doesn't.

### Flash wear — don't run it habitually

A full defrag rewrites every moved cluster to flash. The RP2350's
internal QSPI flash is a consumer-grade chip with a finite
erase/program endurance (spec is ~100k cycles per sector), so
treat defrag as an **occasional cleanup / optimisation**, not
something to fire off after every USB session. One pass after a
big batch of uploads is fine; running it for its own sake every
boot is wasteful.

The preview-and-confirm step partly exists to make accidental
runs hard. Repeat runs on an already-clean volume are also a true
no-op — preview shows `0 mv` and zero writes land on flash.

### Preview + confirm

<p align="center">
<img src="images/defrag-preview.png" width="360" alt="Defrag preview — cluster map before and after, frag/free/file stats, A=apply/B=cancel">
</p>

The preview screen shows the **current cluster layout** (top) and the
**planned layout** (bottom), colour-coded per-file. The summary line
tells you:

- `frag: X → Y` — files currently fragmented vs files that would
  remain unplaceable after the pass
- `free: XK → YK` — largest contiguous free run before vs after
- `N files  TOTAL_K  MOVES mv` — total file count, total bytes, and
  number of cluster moves required

**A** applies, **B** or **MENU** cancels. Nothing touches the FAT
until you explicitly confirm, so you can look at the preview and
back out if the numbers don't make sense.

### Live execution view

<p align="center">
<img src="images/defrag-moving.png" width="360" alt="Defrag running — MOVING CLUSTERS overlay with live per-file colour map and progress counter">
</p>

During execution the cluster map redraws as clusters move, so you
can watch files physically reorder. Cells are coloured per-file
(a 15-hue palette, cycled mod-15 for big volumes). A **red "DO NOT
POWER OFF" banner** across the top doubles as a hardware indicator
(the front LED stays solid red for the same duration).

Phase breakdown during a pass:

1. **Cycle sort** — moves cluster data to match the target layout,
   using two 1 KB buffers (carry + swap) without ever needing more
   free clusters than the size of one cluster. Cluster moves are
   serialised through the flash disk's write-back cache; USB MSC
   stays alive via `tud_task()` between moves.
2. **FAT rebuild** — writes a fresh FAT image with every placed
   file getting a straight cluster chain (`n → n+1 → … → EOC`).
   Unplaceable files (rare; only if the volume is over-committed
   from a prior bad state) keep their existing chain via a
   preservation pass.
3. **Directory patch** — updates every moved file's SFN slot bytes
   26/27 (first-cluster-low) in its parent directory, plus the
   `.` and `..` entries inside each moved subdirectory.
4. **Remount** — `f_unmount` + `f_mount` so FatFs drops every
   cached FAT / dir-entry sector and re-reads fresh state.

The pass is idempotent: running it on an already-clean volume is a
no-op (preview shows `0 mv`, zero cluster writes).

### How it's triggered

- **Auto pre-flight** at every cold boot — walks `/`, checks each
  non-trivial file with `chain_is_contiguous()`, and if any are
  fragmented runs the pass with a brief confirm.
- **Pick "Defragment now"** in the picker menu — same pass, no
  reboot required.
- **Hold B at boot** — forces the pass to run even if the pre-flight
  thinks nothing is fragmented.
- **Per-ROM fallback** — when a runner tries to mmap a cart and the
  contiguous path returns `-5` (fragmented), it first attempts
  chained XIP. If the cart is small enough to fall back to a RAM
  load it can also trigger a targeted rewrite of just that one
  file.

### Safety belts

- **Preview is mandatory.** No cluster writes until you press A.
- **Enumeration cap of 2000 files.** Covers worst-case MicroPython
  game trees with many small asset files alongside ROMs + sidecars.
  Dynamic heap only — 0 permanent SRAM cost.
- **Orphan FAT scan.** Before building the target layout, the
  analyser walks the raw FAT and pins any cluster the FAT says is
  in-use but that the directory walk didn't account for. Even if
  enumeration misses a file (corrupt parent dir, truncated path,
  etc.) its cluster chain is preserved rather than zeroed in the
  FAT rebuild. Defence in depth, not the main correctness story.
- **Red LED + banner** while the FAT is mid-write so the user
  doesn't power-cycle in the worst possible window.

---

## Hardware target

| | |
|---|---|
| **MCU** | RP2350 dual-core Cortex-M33 @ 125 / 150 / 200 / 250 / 300 MHz |
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
- **`blit_fit_gg_nearest`** — 160×144 → 128×128 with asymmetric
  5:4 × 9:8 nearest. Fills the whole screen with no letterbox.
  Used when BLEND is off.
- **`blit_fit_gg_blend`** — same framing, coverage-weighted 2×2
  blend via the packed-RGB565 lerp (one 32-bit multiply per lerp).
  Default on GG.
- **`blit_crop_gg`** — 1:1 native crop into a 128×128 window of the
  160×144 viewport with live pan.

**Game Boy** (`gb_run.c`)
- **`blit_fit_gb_nearest`** — 160×144 → 128×128 with asymmetric
  5:4 × 9:8 nearest. Used when BLEND is off.
- **`blit_fit_gb_cgb_blend`** — coverage-weighted 2×2 blend in
  RGB565 (packed lerp). Default on CGB carts.
- **`blit_fit_gb_dmg_blend`** — palette-index-space blend. Consumes
  the DMG shade-index buffer that `gb_core` populates alongside the
  RGB565 framebuffer, blends shade indices with coverage weights,
  then lerps between bracketing palette entries. Default on DMG
  carts; preserves palette gradient (no hue shift on classic
  Nintendo green).
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
| picodrive residual BSS (after heap-reloc patches — see VENDORING) | ~26 KB BSS |
| PicoDrive `PicoMem` (VRAM/CRAM/VSRAM/zram/ioports) | 140 KB heap (MD session) |
| Cz80_Exec IRAM pool (memcpy'd from flash) | 17 KB heap (MD session) |
| Per-session vidbuf (NES 65 KB / SMS 49 KB / GB 23 KB) | malloc'd in init |
| MD line-scratch buffer (640 B) + LCD fb shared | 640 B BSS |
| GB cart_ram (32 KB max)                        | malloc'd in init |
| SMS cart.sram (32 KB) + vram (16 KB) + wram (8 KB) | smsplus heap |
| MD cart SRAM (when present — Sonic 3, SOR2, etc.) | malloc'd in init |
| Thumby Color framebuffer (128×128 RGB565)      | 32 KB BSS |
| Menu backdrop snapshot (32 KB)                  | BSS, only used while menu is open |
| Audio ring (4096 samples × 2 bytes)            | 8 KB BSS |
| FatFs work area + flash disk write cache       | ~12 KB BSS |
| Picker / favorites / cfg / view pref / defrag snapshot | ~10 KB BSS |
| ROM (XIP-mapped from flash for everything ≥ 256 KB) | ≤ 8 MB |
| **Free heap (typical NES/SMS/GB session)** | **~330 KB** |
| **Free heap (MD session — PicoMem + IRAM)** | **~170 KB** |

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

**MD performance** (measured on Sonic 2, PAL, 300 MHz):

| Audio mode | E (emul us) | FPS | Skipped/s |
|---|---|---|---|
| FULL      | 21–24 k | 50–51 (locked) | 0–25 |
| HALF      | 19–22 k | 50–51 (locked) | 0–10 |
| OFF       | ~14 k   | 50 (locked)     | 0 |

MD's per-frame work is dominated by FAME 68K emulation + VDP
rendering (~13.5 ms) and cz80 Z80 dispatch (~8.5 ms). Critical
optimisations that got us to PAL lock:

- **Cz80_Exec in dynamic IRAM** via `--wrap=Cz80_Exec` + the
  `.md_iram_pool` flash section (see `vendor/VENDORING.md` patches
  16-18). +2-3 ms reclaimed.
- **Adaptive VDP skip-render** — if last frame's emulation time
  overran the refresh budget, set `PicoIn.skipFrame=1` on the next
  frame to let 68K + Z80 + audio emulate normally while the VDP
  line composite + `FinalizeLine` bail early. Held at ≤ 2
  consecutive skips so the display never freezes. +2-3 ms average.
  Overlay shows `k<n>` = skips-per-second.
- **sx/sx2 source-column LUT** in `md_core_scan_end` — replaces
  ~57 k integer divs/frame with table reads, rebuilt only on
  H32 ↔ H40 viewport changes.
- **Packed-RGB565 2×2 blend** on the 320→128 downsample — GB-core
  trick with the 0x07E0F81F expand-pack mask for one-add
  channel-wise averaging.
- **Mono audio** (`POPT_EN_STEREO` dropped) — speaker is mono
  anyway, saves ~0.5 ms / frame.

NTSC 60 Hz lock stays aspirational for now — the per-frame budget
is only 16.67 ms vs our ~21-24 ms typical. Dual-core (Z80 + YM2612
on core1) would get us there; attempted on the `dual-core-wip`
branch but left unfinished (sound-routing issue we didn't crack
before pivoting to the easier single-core wins above).

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
| `<rom>.nes` / `.sms` / `.gg` / `.gb` / `.gbc` | The ROM image you dropped via USB. |
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
| [Peanut-GB](https://github.com/deltabeard/Peanut-GB) DMG + CGB core | MIT | [fhoedemakers fork](https://github.com/fhoedemakers/Peanut-GB) with `PEANUT_FULL_GBC_SUPPORT`, vendored verbatim |
| [minigb_apu](https://github.com/baines/MiniGBS) Game Boy APU | MIT | via TinyCircuits Tiny Game Engine `gbemu/`, no patches |
| [PicoDrive](https://github.com/notaz/picodrive) MD / Genesis core | LGPLv2 | notaz master @ `dd762b8`, heavily patched for Cortex-M33 + XIP flash + heap-allocated statics + IRAM dispatch loop. See `vendor/VENDORING.md` §picodrive for 18 individual patches. |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15 | BSD-1-clause (ChaN) | from ThumbyP8 |
| [Pemsa](https://github.com/egordorichev/pemsa) 3×5 font glyphs | MIT | transcribed |

ThumbyNES is itself **GPLv2** to remain compatible with the nofrendo
+ smsplus cores. All three major vendored cores (nofrendo, smsplus,
picodrive) live under `vendor/` with patches recorded in
`vendor/VENDORING.md`. The peanut_gb + minigb_apu sources are vendored
verbatim from the engine's GBEmu user-module (which itself wraps the
upstream MIT projects). PicoDrive (LGPLv2) combines with our GPLv2
main binary under the LGPL's static-linking carve-out.

The MD core can be disabled at build time with `-DTHUMBYNES_WITH_MD=OFF`
— nesrun_device drops to 643 KB (fits the backward-compatible 1 MB
ThumbyOne NES partition). Default builds include MD and need the 2 MB
partition layout (`ThumbyOne/common/pt_with_md.json`).

---

## Non-goals

Explicit scope cuts to protect the RAM/CPU budget:

- **No multiple save state slots.** One `.sta` per cart.
- **No FDS, no VRC6 / VRC7 / MMC5 expansion audio (NES).**
- **No NES 2.0 extended-header support.**
- **No ColecoVision / SG-1000.** smsplus supports them but we don't expose them.
- **No YM2413 FM (SMS Japanese carts).** smsplus has it; off for now.
- **No Mega-CD / 32X / SVP** (MD). The PicoDrive source is built with
  `-DNO_32X`; CD sources are excluded from the build + stubbed;
  Virtua Racing's SVP co-processor would need its own ARM-less
  interpreter port. SVP sources compile as empty stubs.
- **No YM2413 on MD** (the rare Japanese SMS-on-MD FM adapter —
  excluded via `-DTHUMBY_YM2413_EXCLUDED`).
- **No `draw2.c` alt renderer on MD** (the full-frame 320×240 renderer;
  the default per-line `draw.c` path + our 128×128 line-scratch
  downsample is what ships. Saves ~103 KB of BSS).
- **No netplay / link cable.**
- **No on-device cheats or Game Genie.**
- **No PWM backlight dimming** (single-GPIO BL line on the Thumby Color).

---

## Changelog

### v1.05 — Mega Drive / Genesis

- **Mega Drive emulation** via vendored [PicoDrive](https://github.com/notaz/picodrive)
  (LGPLv2, notaz master @ `dd762b8`). Drops `.md` / `.gen` / `.bin`
  into the picker alongside NES/SMS/GB. Boots and plays most 1990-
  era 3-button carts — Sonic 2, Streets of Rage 2, many more —
  locked at 50 PAL on a 300 MHz overclock with full audio.
- **New "Audio" in-game menu**: FULL / HALF / OFF per cart. HALF
  halves YM2612 synthesis cost (11025 Hz + ZOH upsample); OFF
  strips Z80 + FM + PSG entirely for maximum refresh. Replaces the
  old per-cart Frameskip option (was universally worse).
- **Adaptive VDP skip-render** — MD runner auto-skips rendering on
  frames following a budget overrun, locking 50 FPS on heavy scenes
  without visible stutter.
- **Aspect-preserving FILL for MD** — matches SMS FILL's approach
  (scale by height, crop sides) instead of the stretched default.
  Sprites stay proportional.
- **Dynamic IRAM for Cz80_Exec** — 17 KB Z80 dispatch loop memcpy'd
  from flash to heap SRAM at `mdc_init`, freed at shutdown. Zero
  BSS cost across sibling cores; ~2-3 ms / frame reclaimed.
- **ThumbyOne integration** behind a new `THUMBYONE_WITH_MD` flag.
  Default ON; grows the NES partition from 1 MB to 2 MB to hold
  PicoDrive's ~850 KB of precomputed flash tables (FAME jumptable,
  YM2612 log-sine, cz80 SZHVC). Shifts P8/DOOM/MPY partitions and
  the shared FAT up by 1 MB. Backward-compatible build with
  `-DTHUMBYONE_WITH_MD=OFF` retains the original 1 MB partition +
  9.6 MB FAT and skips picodrive entirely.
- **Known gaps** — Sonic 3 level load and Gunstar Heroes boot hang
  on both device and host (PicoDrive core bug, not a ThumbyNES
  regression). No Mega-CD, no 32X, no Virtua Racing SVP.
- **Vendored PicoDrive patches**: 18 individual patches totalling
  ~4.4 MB of BSS relocated out of static storage, Cortex-M33 Thumb-
  bit fixes in cz80's function-map dispatcher, XIP-flash safety-op
  suppression, FAME/YM2612/cz80 table generation pushed to build
  time. Full catalogue in [`vendor/VENDORING.md`](vendor/VENDORING.md).

### v1.04

- **Better Game Boy / Game Gear / Game Boy Color FIT scaling.** The
  old asymmetric 5:4 × 9:8 nearest dropped every 5th source column
  and every 9th row — thin text strokes vanished in menus,
  dialogue, HUDs. The FIT path now has a **coverage-weighted
  blend**: every source pixel contributes to the output proportional
  to its exact 1.25 × 1.125 footprint, nothing drops. Selected
  per-cart via a new **BLEND toggle** in the in-game menu (default
  on for GB / GBC / GG); flip it off to go back to the pure
  nearest look on pixel-art carts.
- **Palette-aware DMG blend.** On DMG carts the blend runs in
  palette-index space and then interpolates between bracketing
  palette entries — preserves hue on the classic 4-shade Nintendo
  greens (where a naive RGB565 blend was introducing a visible teal
  shift between the lighter and darker greens).
- **Packed-RGB565 lerp** for the fast path. One 32-bit multiply per
  pixel-lerp (all three channels in parallel via the `0x07E0F81F`
  mask trick) instead of four channel-separated multiplies. ~3×
  less arithmetic per output pixel; full 128×128 blend stays well
  under 1 ms at 250 MHz when placed in SRAM.
- **New 300 MHz overclock option** alongside the existing
  125/150/200/250 MHz choices. Both the global Overclock row in the
  picker menu and the per-cart Overclock in each in-game menu now
  offer it. Default stays at 250 MHz.

### v1.03

- **Game Boy Color support.** Swapped the vendored peanut_gb for
  fhoedemakers' CGB-capable fork (MIT). `.gbc` ROMs now load and
  run with their native 15-bit palette converted to RGB565 at line
  time. DMG carts still work with the six built-in shade palettes;
  CGB carts use the cart's own palette and ignore that setting.
- **Chained-XIP fallback for fragmented carts.** When a cart's
  cluster chain isn't contiguous the runners fall back to a
  per-cluster pointer table so the file still maps straight out of
  flash — no RAM load. Most games run at full speed either way;
  heavier carts (dense NES mappers, some GBC titles) can drop
  frames on chained XIP when they wouldn't on contiguous mmap.
  Defragment if a fragmented cart feels sluggish; otherwise it's
  optional. See [Defragmenter](#defragmenter) for the full
  explanation.
- **New cluster-level defragmenter** with live cluster-map
  visualisation. Replaces the old file-level `f_expand` approach.
  Works on near-full volumes (the file-level path couldn't — it
  needed 2× the largest file free). Preview-then-confirm UX with
  A = apply / B = cancel; moves render live per-file with a red
  `DO NOT POWER OFF` banner + front LED while the FAT is
  mid-write. See the [Defragmenter](#defragmenter) section for
  why you might want (or not need) to run it.

### v1.02

- **New SMS display option: FILL.** A third MENU-tap cycle position
  between FIT and CROP on Sega Master System carts. Fills the full
  screen at uniform 1.5× scale (no letterbox, square pixels) by
  cropping ~25% of the horizontal source — you lose a strip off
  each side but gain meaningfully more vertical pixels for the
  action. Area-weighted blended for image quality, and the cart
  keeps playing (unlike SMS CROP which pauses). Game Gear doesn't
  need it — the existing FIT already fills the screen via
  asymmetric scaling.

### v1.01

- **Super Mario Bros save states now work.** Previously the game
  would hang when a save was loaded after a power cycle. A handful
  of other NES carts were affected in the same way. (Saves created
  on v1.0 still won't load — just re-save once after updating.)
- **Cleaner boot.** Straight to the ThumbyNES logo instead of
  flashing through a sequence of coloured startup screens.
- **Faster SMS and Game Gear emulation.** A build-setting oversight
  had the SMS/GG CPU emulator running from slow flash memory
  instead of fast RAM. Fixed.
- **SDL host runner pacing** (only relevant if you build the Linux /
  macOS development runners). The NES runner now runs at the cart's
  native refresh rate on systems that ignore vsync, matching the
  SMS and GB runners.

### v1.0

First release. NES, Sega Master System, Game Gear, and Game Boy
cores with save states, an in-game pause menu, screenshots, a
tabbed ROM picker that remembers where you left off, in-firmware
FAT defragmenter, and configurable overclock.

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
