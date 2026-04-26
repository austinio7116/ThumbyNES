# Adding PC Engine / TurboGrafx-16 to ThumbyNES

Plan for integrating a PC Engine core into the existing ThumbyNES firmware
as the fifth emulator core alongside NES (nofrendo), SMS/GG (smsplus),
GB/GBC (peanut_gb), and MD (PicoDrive). Picker auto-routes by extension;
LCD driver, audio, USB MSC, FAT, favorites, picker, sleep, autosave,
scale modes, battery saves, save-states, per-ROM cfg — all reused.

This is a planning document. Companion scaffolding lives on branch
`pce-slot` and is not yet wired into the device build.

---

## 1. Decision: vendor HuExpress (odroid-go-pcengine-huexpress)

Evaluated five candidates. HuExpress from the ODROID-GO port is the only
one already proven on an MCU without PSRAM.

| Criterion | **HuExpress (ODROID-GO)** | Mednafen pce_fast | NitroGrafx | Geargrafx |
|---|---|---|---|---|
| License | GPLv2 | GPLv2 | unstated | GPL-3.0 |
| Language | C | C++ | C + ARM7 asm | C++ |
| Prior MCU port | ESP32 (ODROID-GO) | ESP32 via MCUME + PSRAM | DS/3DS ARM7 | none |
| HuCard-only trim | trivial | mid effort | mid effort | hard |
| CD / Arcade Card | present but isolable | present | partial | accurate |
| Maintenance | quiet but stable fork | active | quiet | active |
| License compat w/ our stack | ✓ (matches nofrendo/smsplus) | ✓ | unknown → risk | ✗ GPL-3 vs our GPLv2 |

### Why not the alternatives

- **Mednafen pce_fast**: fine core but designed for the libretro frontend.
  Extracting a bare C core is more work than starting from HuExpress,
  which is already bare-metal-shaped.
- **NitroGrafx**: likely the smallest in memory terms but mostly ARM7
  assembly — it does not port to Cortex-M33 without a rewrite.
- **Geargrafx**: GPL-3.0 infects the combined binary — can't mix with
  Nofrendo/smsplus/PicoDrive without relicensing the whole firmware.
- **Temper**: closed source.

### Measured footprint (compiled with arm-none-eabi-gcc -Os -mcpu=cortex-m33)

Compile-only (unlinked), HuCard-only configuration:

- **Flash (text + data): ~70 KB**
  - h6280 CPU core: 43 KB text + 3 KB data (dominates)
  - pce.c main hub: 9.4 KB
  - sprite, hard_pce, gfx, format: ~11 KB combined
  - romdb, hucrc, small helpers: < 1 KB
- **Static BSS: ~5 KB** after stripping `CD_track[]` (74 KB, CD-only)
- **Heap, HuCard-only, no shadow-VRAM: ~97 KB** — 32 KB RAM + 64 KB VRAM +
  palette/SPRAM/small

Compared to existing cores: flash is dramatically smaller (~70 KB vs
~630 KB for NES). Heap is in the same ballpark as SMS (~90 KB).

**Vendor at** `vendor/huexpress/` from
`github.com/pelle7/odroid-go-pcengine-huexpress`, pinned by commit.
Drop the ESP-IDF frontend, SDL OSD, Haiku code, netplay, zip, iniconfig,
keyboard mapping, CD support. Keep only the `pcengine-go/components/
huexpress/engine/` tree plus `hucrc.c`.

---

## 2. Feasibility — CPU budget

Biggest unknown. PC Engine CPU is a 6502-derivative (HuC6280) at 7.16 MHz
with 263 scanlines at 60 Hz. The `h6280.c` compiled with
`MY_INLINE_h6280_opcodes` + `USE_INSTR_SWITCH` — a 43 KB text switch
dispatcher — is the hot loop.

ODROID-GO (ESP32 @ 240 MHz LX6, 520 KB SRAM): reported ~50–60 FPS for
most HuCard titles.

RP2350 @ 150 MHz (or 250 MHz OC) Cortex-M33: expected slower, because
LX6 has more ops-per-clock on the 6502-style dispatcher. Order-of-
magnitude guess:

| Title class | FPS @ 150 MHz | FPS @ 250 MHz OC |
|---|---|---|
| Puzzle / static-BG (Puyo Puyo, Devil's Crush) | 60 | 60 |
| Side-scrollers (Bonk, Ninja Spirit) | 35–50 | 55–60 |
| Sprite-heavy shmups (Soldier Blade, R-Type) | 25–35 | 45–55 |
| FM-heavy with PSG (Lords of Thunder) | 25–35 | 35–45 |

Mitigations available, in order of effort:
1. `-O3` and `.time_critical` placement of the h6280 dispatcher
2. Per-scanline rendering directly into the 128×128 LCD framebuffer
   (MD-style `MD_LINE_SCRATCH`) — saves the 64 KB frame buffer and
   eliminates the second copy pass
3. Move the PSG mixer to core 1 (the NES/MD pattern)
4. OC to 250 MHz via `pico_set_overclock`

Final verdict on playability: defer until we have a host bench + a
device bench. Same gate we applied to MD.

---

## 3. HuExpress API surface

Mapped to the same wrapper shape as `sms_core`, `gb_core`, `md_core`.

| Our wrapper (`pce_core.h`) | HuExpress call |
|---|---|
| `pcec_init(int sample_rate)` | initialize `hard_pce`, set `option`, allocate `RAM`, `VRAM`, `Pal`, `SPRAM` |
| `pcec_load_rom(const uint8_t *data, size_t len)` | copy ROM to `ROM` pointer, populate `ROM_size`, call `LoadRom` path |
| `pcec_run_frame()` | `exe_go()` until end-of-frame (one complete `PCE_hardware_periodical()` cycle) |
| `pcec_framebuffer()` | returns 8-bit palette indices from the partial scanline renderer (we will skip the native `osd_gfx_put_image` which draws to SDL) |
| `pcec_palette_rgb565()` | palette expanded from 9-bit VCE → RGB565 on our side |
| `pcec_set_buttons(uint16_t mask)` | write `io.JOY[0]` bits |
| `pcec_audio_pull(int16_t *out, int n)` | drive the PSG mixer via `MixBuffer` and return mono int16 |
| `pcec_battery_ram()` | returns pointer to the 2 KB backup-RAM area (BRAM) |
| `pcec_save_state(path)` / `pcec_load_state(path)` | custom serialisation — HuExpress has none standardised; write our own covering hard_pce + VRAM + VRAM-derived state |
| `pcec_shutdown()` | free heap allocs |

**Display dimensions**: PCE VDC is programmable. Native modes range
from 256×224 (common, e.g. Bonk) to 352×240. We render to a
**256×240** palette-indexed bitmap and expose a viewport via
`pcec_viewport()` — same contract as smsplus Game Gear.

**Controller**: PCE pad is 8 buttons — D-pad + I + II + SELECT + RUN.

```c
#define PCEC_BTN_I       0x01
#define PCEC_BTN_II      0x02
#define PCEC_BTN_SELECT  0x04
#define PCEC_BTN_RUN     0x08
#define PCEC_BTN_UP      0x10
#define PCEC_BTN_RIGHT   0x20
#define PCEC_BTN_DOWN    0x40
#define PCEC_BTN_LEFT    0x80
```

Six-button pad (Avenue Pad 6) is rare enough for launch that we defer.

**Audio**: HuExpress's PSG mixer (`MixBuffer` in `mix.c`) produces
signed 16-bit at a configurable rate. We use 22050 Hz mono to match
`nes_audio_pwm`. CD audio (`lsmp3.c`) stripped; PCM disabled.

---

## 4. Memory layout

Device SRAM budget today (with NES + SMS + GB + MD selected):

```
Cores static ............ ~460 KB   (MD_PLAN Phase 5 target)
LCD framebuffer ........ ~33 KB   (128 × 128 × 2)
Audio rings ............ ~6 KB
FatFs buffers .......... ~10 KB
Picker state ........... ~5 KB
BSS / stacks / misc .... ~6 KB
─────────────────────────────────
Subtotal ................ ~520 KB  (full)
```

Adding PCE:

| Block | Size | Placement | Notes |
|---|---|---|---|
| `RAM` (PCE main RAM) | 32 KB | heap | required |
| `VRAM` | 64 KB | heap | required |
| `osd_gfx_buffer` (XBUF) | **220 KB** | heap | **see below** |
| `VRAM2` (tile cache) | 64 KB | heap | render optimisation |
| `VRAMS` (sprite cache) | 64 KB | heap | render optimisation |
| `vchange`, `vchanges` | 2.5 KB | heap | dirty markers |
| `Pal` | 512 B | heap | required |
| `SPRAM` | 256 B | heap | required |
| palette RGB565 LUT | 1 KB | static | ours |
| static core BSS | ~5 KB | .bss | core |
| **Full-fat working set** | **~453 KB** | | |
| **HuCard minimum (if XBUF eliminated)** | **~100 KB** | | requires rewrite |

### The XBUF problem (primary memory risk)

The renderer expects a **600 × 368 = 220 KB** oversize bitmap. The
600 × 368 size is not the PCE display — it's an overscan-padded
buffer that absorbs sprite writes that would otherwise overrun the
native 256 × 240 area. The upstream author's comment in `gfx.h`:

> "A sharper way of doing would probably reduce the amount of needed
> data from 220kb to 128kb (eventually smaller if restricting games
> with hi res to be launched)."

This does not fit within the RP2350's SRAM budget alongside the other
cores. Mitigation path, in order of effort:

1. **Enable `MY_VIDEO_MODE_SCANLINES`** — already scaffolded in
   `gfx.h`, `gfx_render_lines.h`, `gfx_Loop6502.h` behind the macro.
   The per-scanline renderer writes into a tiny line buffer + callback
   we provide. This is the **intended** path — same shape as
   `MD_LINE_SCRATCH`. Estimated RAM savings: ~220 KB → ~2 KB line
   scratch. Unverified on the device; correctness risk for games
   that rely on mid-line VDC register writes (e.g. Lords of Thunder
   mid-frame palette swaps).

2. **Fall back to trimmed XBUF + bounds-checked sprite writes** if (1)
   breaks too many titles. Reduces 220 KB → 128 KB per the upstream
   comment but still substantial.

3. **Host build keeps full XBUF** — desktop has plenty of RAM, so
   `pce_host_main.c` can run without scanline mode. Easier to debug
   the core against a known-good render path before layering
   optimisations.

Phase 1 scaffold allocates the full XBUF on host; device build is
gated behind `PCE_SCANLINE_RENDER` which initially refuses to build
with a `#error` until the line renderer is plumbed through — no
point shipping a device variant that crashes on first frame.

**Strategy**: PCE cannot coexist in-memory with MD. This is already
the operating model — MD overlays SMS+NES heap regions via the
session-switch pattern. PCE plugs into the same mechanism: the
picker free's the previous system before allocating the new one.

Flash cost: ~70 KB compiled + ~16 KB of palette/state = **~90 KB**.
Fits inside the existing ThumbyOne `THUMBYNES_WITH_MD=OFF` 1 MB slot
comfortably; with MD=ON, we already used 2 MB. Either way, PCE doesn't
change the slot sizing.

---

## 5. Video pipeline

HuExpress's stock renderer writes 256-wide palette-indexed scanlines
into a full-framebuffer bitmap. We mirror the SMS approach (not MD's
per-scanline) at first because it's simpler:

1. `pcec_run_frame()` fills an internal 256×240 palette-indexed buffer
2. `blit_fit_pce()` in `device/pce_run.c` downsamples 2:1 horizontally
   and 2:1 vertically → 128×112 centred inside 128×128 with 8 px
   letterbox top/bottom (for 256×224 titles)
3. Palette expansion uses a precomputed RGB565 LUT (512 entries)

If perf falls short, we upgrade to line-based rendering in phase 2
following the `MD_LINE_SCRATCH` template — the HuExpress scanline
renderer already runs per-line, so the refactor is mostly plumbing.

---

## 6. Input mapping

Thumby has 8 buttons; PCE needs 8 — a clean 1:1 mapping:

| Thumby | PCE |
|---|---|
| A | I |
| B | II |
| LB | SELECT |
| RB | (unused) |
| MENU | RUN |
| D-pad | D-pad |

Rapid-fire toggles (PCE Turbo switches) can be added later as a long-press
on LB/RB. Out of scope for v1.

---

## 7. Save states

HuExpress upstream has no portable save-state format. We'll write one:

- Magic `THPE` + version byte
- `hard_pce` struct dump
- `RAM` (32 KB), `VRAM` (64 KB), `Pal` (512 B), `SPRAM` (256 B)
- CPU registers from `h6280` (extracted from the per-opcode state)
- IO snapshot from `shared_memory.h::IO`

Total ~100 KB per state. Compressed optionally with the same LZ4
helper SMS uses. Route all I/O through `thumby_state_bridge.h` so the
FatFs shim is shared.

Battery RAM: PCE cartridges with backup RAM use a 2 KB BRAM region
at `0x1EE000`. Very few HuCards actually save (Dungeon Explorer,
Neutopia II) but we plug into the same `nes_battery.c` path that SMS
and MD use.

---

## 8. Integration phases

Conservative ordering — no phase requires committing PCE ROMs to the
device until phase 4.

### Phase 1 — Core drops in and builds (host)

- [ ] `vendor/huexpress/` populated with engine/ tree only
- [ ] `thumby_platform.h` stubs out ESP-IDF (`DRAM_ATTR`,
      `IRAM_ATTR`, queue handles, task-as-queue flags)
- [ ] `PCE_HUCARD_ONLY` define strips CD_track, cd_extra_mem,
      ac_extra_mem, WRAM (backup), shadow VRAMs, vchange tracking
- [ ] `src/pce_core.{c,h}` wrapper compiles against the stripped core
- [ ] `src/pce_bench_main.c` — headless frame-rate bench
- [ ] `src/pce_host_main.c` — SDL2 runner (optional; falls out if SDL2
      is found by CMake the same way as mdhost)
- [ ] Plays a HuCard ROM on host at > real-time

### Phase 2 — Device slot scaffolded

- [ ] `device/pce_run.{c,h}` modelled on `gb_run.c`
- [ ] `blit_fit_pce` 2:1 downscale into 128×128 RGB565
- [ ] 22050 Hz mono audio through nes_audio_pwm
- [ ] Input mapping via `pcec_set_buttons`
- [ ] Picker extension: scan `.pce` files, system icon, dispatch route
- [ ] `option(THUMBYNES_WITH_PCE)` in device CMakeLists, default OFF

### Phase 3 — Benchmarks inform optimisation

- [ ] Host bench across a representative ROM set (pce_compat_test.py)
- [ ] Device bench at 150 MHz baseline
- [ ] `.time_critical` pass on the h6280 dispatcher
- [ ] OC trial at 250 MHz, measure vs PSU budget
- [ ] Decide whether to invest in line-based rendering

### Phase 4 — User-visible

- [ ] README gallery entry, screenshot
- [ ] VENDORING.md patch list finalised
- [ ] Release notes
- [ ] Open PR only after user approval per repo norms

---

## 9. Open questions

1. **Six-button Avenue Pad**: skip for v1, or wire behind a
   hold-select chord? Unlikely to affect game compatibility for
   the core library.
2. **CD-ROM BIOS detection**: if a user drops a CD game `.bin` into
   `/roms/`, do we reject with a message or silently skip? Reject
   is nicer and matches how MD handles 32X files.
3. **Region selection**: PCE (Japan) vs TurboGrafx-16 (USA) differ
   only in header byte and a couple of region-locked carts. Auto-
   detect from ROM header at load time; no UI.
4. **Saturn PC Engine games**: out of scope forever.
5. **Audio: PSG LFO accuracy**: HuExpress LFO is simplified. Affects
   ~5 titles. Acceptable for a handheld port.

---

## 10. What this plan does NOT commit to

- CD-ROM² / Super CD-ROM² games
- SuperGrafx exclusives (Ghouls'n Ghosts SGX, Daimakaimura)
- Arcade Card titles (Garou Densetsu, Art of Fighting)
- Six-button peripheral
- Multi-tap (5-player)
- Achievement hash / cheat engine

All of these can be revisited post-v1 without structural changes.
