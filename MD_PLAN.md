# Adding Mega Drive / Genesis to ThumbyNES

Plan for integrating a Sega Mega Drive / Genesis core into the existing
ThumbyNES firmware as the fourth emulator core alongside NES (nofrendo),
SMS/GG (smsplus), and GB (peanut_gb). Picker auto-routes by extension;
everything else (LCD driver, audio, USB MSC, FAT, favorites, picker,
sleep, autosave, scale modes, battery saves, save-states, per-ROM cfg)
is reused as-is.

This is a planning document, not committed code.

---

## 1. Decision: vendor PicoDrive (notaz)

PicoDrive is the only realistic MD core for RP2350-class SRAM. Anything
targeting desktop accuracy (Genesis Plus GX, BlastEm) blows the memory
and CPU budgets by an order of magnitude.

| Criterion | **PicoDrive** | Genesis Plus GX | BlastEm |
|---|---|---|---|
| License | LGPLv2 | GPLv3 | MPL2 |
| 68K core | FAME/C (+ optional Cyclone ARM-asm) | Musashi interp | tables + micro-ops |
| Static state footprint | **~160 KB** | ~400 KB | ~1 MB |
| Proven CPU floor for 60 fps | **~100 MHz ARM9** (GP2X, Dingoo, Caanoo) | ~500 MHz+ | ~1 GHz+ |
| Prior MCU port | RP2040 community forks, PSP, Dingoo | ESP32-S3 (struggles) | none |
| Support beyond MD | SMS, GG, Pico, stubs for 32X/MCD | MD/SMS/GG | MD only |
| Maintenance | Active (notaz, libretro) | Active | Active |

**Vendor at** `vendor/picodrive/` pinned to notaz's standalone tree
(NOT the libretro packaging) so we don't import the libretro frontend
API layer. 32X and MegaCD features disabled at compile time
(`-DNO_32X -DNO_CD`). SMS support in PicoDrive is also disabled — we
already have smsplus and don't want two SMS cores linked in.

LGPLv2 mixes fine with our GPLv2 (we distribute as GPLv2 combined). No
license friction.

**Don't port Cyclone.** Cyclone is ARM7/ARM9/ARM11 hand-asm and does
not target Cortex-M33 Thumb-2. Stick with the FAME/C core. Perf
headroom comes from `.time_critical` placement + dual-core + OC, not
from asm.

---

## 2. Feasibility — the native-FPS reality check

This is the primary risk. RP2350 M33 at 150 MHz (OC 250 MHz viable)
vs PicoDrive's proven ~100 MHz ARM9 floor:

| Title class | FPS @ 150 MHz | FPS @ 250 MHz OC |
|---|---|---|
| Puzzle / static-BG RPG (Columns, PS IV dialog) | 60 | 60 |
| Mid (Sonic 1, Castle of Illusion, Ecco) | 40–55 | 55–60 |
| Sprite-heavy (Sonic 2/3, SOR 2, Gunstar) | 28–40 | 45–55 |
| FM-heavy / scaler tricks (SF2, Thunder Force IV) | 25–35 | 35–45 |
| SVP / SegaCD / 32X | drop |

The "native FPS" requirement is achievable for **simpler titles at
250 MHz OC**; for the Sonic-tier sprite-heavy mainline expect 50–60
at frameskip 0, 60 solid at frameskip 1. Frameskip must be a
first-class cfg toggle (0 / 1 / auto), same sidecar mechanism SMS/NES use.

Levers to hit 60 fps on more of the library:

1. **Dual-core split** — core1 runs 68K execution + cart bus, core0
   runs VDP line render + YM2612/SN76489 + display blit + input.
   Mirrors P8's dual-core precedent.
2. `.time_critical.md` placement for: FAME dispatch loop, 68K work
   RAM, VRAM, Z80 RAM, CRAM, VSRAM, YM2612 per-sample step.
3. Line-based rendering direct into the 128×128 RGB565 framebuffer —
   no intermediate 320×224 palette buffer.
4. YM2612 at 22050 Hz mono, no per-sample interpolation, linear
   channel mixing only.
5. OC to 250 MHz default (ThumbyNES already runs at 150; GBEmu runs
   at 200+).

---

## 3. Memory budget

Worst-case resident state with all four cores linked in (exactly one
active at a time):

| Region | Bytes | Notes |
|---|---|---|
| Nofrendo state (already) | 95 K | |
| smsplus state (already) | 87 K | |
| peanut_gb state (already) | 40 K | |
| LCD framebuffer 128×128×16 | 33 K | shared |
| FatFs + flash disk cache | 12 K | shared |
| Picker / favorites / cfg / audio ring | 14 K | shared |
| **NEW:** MD 68K work RAM | **64 K** | `.time_critical.md`, hot |
| **NEW:** MD VRAM | **64 K** | `.time_critical.md`, hot |
| **NEW:** MD Z80 RAM | 8 K | |
| **NEW:** FAME 68K dispatch + state | 32 K | tables can live in flash |
| **NEW:** YM2612 + SN76489 state | 8 K | |
| **NEW:** line render scratch + CRAM/VSRAM | 2 K | 320×u8 + small |
| **NEW:** cart SRAM (BRAM) | up to 64 K | lazy — only if header declares it |
| **Total static (combined)** | **~460 K** | of 520 K SRAM |

This is tight. Two levers:

- **BSS-union across cores** — we only run one at a time; union the
  four per-core state blobs. Saves ~100 K, drops us to ~360 K used
  and ~160 K free heap. The SMS plan deferred this as unneeded; for
  MD we should do it **at the point of MD integration, not after**.
  One `union { nes_state_t; sms_state_t; gb_state_t; md_state_t; }`
  in BSS, per-core `init()` zeroes the region it owns.
- **FAME opcode tables in flash** — marked `__in_flash()`, accept
  ~5% slowdown on rare opcodes. Keep the hot dispatch path in SRAM.

**Cart SRAM (BRAM)** is lazy-allocated only when the ROM header's
`SRAM_START`/`SRAM_END` region is declared (Phantasy Star IV, NBA Jam,
Shining Force, etc.). Most carts don't declare it and pay zero.

---

## 4. ROM access via XIP — reuse what exists

The existing `nes_picker_mmap_rom()` / `nes_picker_mmap_rom_chain()`
path in `device/nes_picker.c` already solves zero-copy ROM loading
from QSPI XIP. It's system-agnostic ("give me the XIP pointer for this
FAT file") and has been battle-tested by the NES and SMS runners.

PicoDrive's 68K cart-space read goes through `Pico.rom` /
`Pico.romsize`. Three cases:

1. **Contiguous ROM** (common): `Pico.rom = xip_ptr`, zero copy,
   direct XIP fetches at ~60 MB/s after cache warmup.
2. **Fragmented ROM**: PicoDrive already has a bus-level function-
   pointer dispatch used for MCD and SSF2 bank-swap. Hook our chained-
   XIP table into it — one indirect load per cart fetch, ~10% perf
   hit on fragmented carts only.
3. **ROM > 2 MB flash partition**: v1 scope caps at 2 MB. 90% of the
   interesting MD library fits: Sonic 1/2/3, SOR 1/2/3, Shinobi,
   Thunder Force III/IV, Phantasy Star II–IV, Gunstar Heroes, Castle
   of Illusion, Ecco, Streets, Shining Force 1/2. The oversize corner
   cases (Super Street Fighter 2 = 5 MB, Pier Solar = 8 MB, Super
   Mario Bros 32X repro) are post-v1.

**Byte-swap**: MD ROMs store 68K code big-endian. FAME reads with
`READ_WORD_SWAPPED` macros — the swap happens on fetch, not on store.
XIP flash stays untouched; we never have to rewrite ROM bytes.

**SMD interleaved headers** (the 512-byte interleave variant from old
copier dumps): detect at load time via magic bytes, deinterleave once
into a small scratch or, better, reject `.smd` and document `.md`/
`.bin`/`.gen` only. Deinterleaving in-place in XIP is not possible
(read-only); deinterleaving into heap defeats the zero-copy goal.
**Decision: support `.md`/`.bin`/`.gen`, skip `.smd`** for v1.

---

## 5. Rendering — 320×224 → 128×128

PicoDrive has a line-based renderer that emits one scanline of
palette indices at a time. Perfect match:

```c
for (int scan = 0; scan < 224; scan++) {
    PicoFrameLine(line_buf);                  // 320 × u8 palette idx
    if (scan >= crop_top && scan < crop_bot)
        downscale_line_to_rgb565(line_buf,
                                 fb + y_out * 128 * 2,
                                 palette_rgb565);
}
```

Downscale modes (cfg toggle, same sidecar bit layout as SMS):

| Mode | Behavior |
|---|---|
| **FIT 2.5:1 H, 1.75:1 V** (default) | 128×128 letterboxed |
| **CROP 1:1** | 128×128 pannable window into 320×224 |
| **FILL asymmetric** | 320→128 × 224→128, stretched, shows full screen |

No intermediate 320×224 palette framebuffer — saves ~70 KB vs the
naive approach. Palette: MD CRAM has 64 9-bit entries (32 per palette
line × 2); convert once per frame to 64 RGB565 words in a small
lookup table.

---

## 6. Audio — 22050 Hz mono

PicoDrive's sound core emits L/R int16 at configurable rate. We want
**22050 Hz mono**: `(L + R) / 2`, matches the existing PWM path.

YM2612 at 22050 Hz mono is the heaviest single component of MD
emulation. Pin the YM2612 step to core1 with 68K to keep the VDP line
render uncontended on core0. Per-frame budget at 60 fps @ 22050 Hz is
367 samples — feed them in one batch at end-of-frame, buffered into
the shared audio ring.

SN76489 PSG (identical chip to SMS) — reuse the existing smsplus
SN76489 implementation if it's cleanly separable, otherwise link
PicoDrive's copy; either works, it's a tiny chip.

---

## 7. File layout — additive, same shape as SMS addition

```
ThumbyNES/
├── vendor/picodrive/            NEW (LGPLv2) — notaz standalone @ pinned commit
│   ├── Pico/                    (68K, VDP, memory map)
│   ├── cpu/fame/                (68K interpreter — NOT cyclone)
│   ├── cpu/cz80/                (Z80 for PCM driver)
│   ├── sound/ym2612.*           FM synthesis
│   ├── sound/sn76496.*          PSG
│   └── platform/                (stub — we provide our own glue)
├── src/
│   ├── md_core.[ch]             NEW — mirrors nes_core / sms_core / gb_core
│   ├── md_bench_main.c          NEW
│   └── md_host_main.c           NEW — SDL2
└── device/
    ├── md_run.[ch]              NEW — mirrors sms_run
    ├── nes_picker.[ch]          extend ext list to .md .bin .gen, add SYS_MD
    ├── nes_device_main.c        extend dispatch
    └── CMakeLists.txt           add libpicodrive, .time_critical.md section
```

Picker meta-row tag: `MD  512K  NTSC` (where size is ROM size in KB
and region comes from header byte at offset 0x1F0 — `J`/`U`/`E`).

## 8. md_core.[ch] API surface

Mirrors nes_core / sms_core / gb_core exactly. Six operations:

| Our wrapper | PicoDrive call |
|---|---|
| `mdc_init(int region, int sample_rate)` | `PicoInit()`, set `PicoIn.sndRate`, set region |
| `mdc_load_rom(const uint8_t *xip, size_t len)` | `PicoCartInsert(xip, len, NULL)` then `PicoReset()` |
| `mdc_run_frame()` | `PicoFrame()` |
| `mdc_framebuffer()` | internal line-callback writes direct to our RGB565 FB; `mdc_framebuffer()` returns it |
| `mdc_palette_rgb565()` | convert CRAM once/frame |
| `mdc_set_buttons(uint8_t mask)` | writes `PicoIn.pad[0]` |
| `mdc_audio_pull(int16_t *out, int n)` | drains PicoDrive's sound ring, mono-mixes |
| `mdc_shutdown()` | `PicoShutdown()` |
| `mdc_battery_ram()` / `mdc_battery_size()` | returns `Pico.sv.data` / `Pico.sv.size` if present |

Button map:

| Thumby | MD pad |
|---|---|
| D-pad | Up/Down/Left/Right |
| A | B (the common "jump" button in most MD games) |
| B | C |
| LB | A |
| RB | X (or unmapped if using 3-button mode only) |
| MENU | Start / pause |

**Default to 3-button pad**; offer 6-button as per-ROM cfg toggle for
SF2 / Comix Zone / fighters. Almost all MD games work fine with
3-button; 6-button enables the extra three face buttons only.

---

## 9. Phased plan

| Phase | Goal | Done when |
|---|---|---|
| **0** | Vendor PicoDrive stripped to MD-only, host build green | `mdbench sonic.md` runs 600 frames on Linux without crash |
| **1** | SDL2 host runner: video, input, audio | Sonic 1 playable at 60 fps with FM sound on Linux |
| **2** | Host perf bench, throttled to simulate ~150 MHz single-core | Go/no-go list: which titles hit 60 fps on device |
| **3** | Device build, hardcoded ROM, single-core | Sonic 1 boots + renders + sound on hardware, any FPS |
| **4** | `.time_critical.md` placement, dual-core split (core1 = 68K+FM) | Frameskip-0 native for simpler titles at 250 MHz OC |
| **5** | Picker dispatch (.md/.bin/.gen), BRAM saves, cfg (frameskip, pad mode, region, scale, palette blend) | Mixed library works, Phantasy Star IV saves |
| **6** | BSS-union four cores, save-states via `thumby_state_bridge`, polish | Ship-ready |

Effort estimate (with my 10-50× overestimate correction applied):

- Phases 0–1: **one evening** on host.
- Phase 2: **one evening** benching.
- Phases 3–4: **2–3 evenings** on device — the perf tuning is the
  risk-carrying stretch.
- Phases 5–6: **2 evenings** — the scaffolding already exists.

~1 week of focused evenings total, front-loaded on host where
iteration is fast.

---

## 10. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| 60 fps not achievable even with OC + dual-core on mid-tier titles | Medium | Default to frameskip 1 for Sonic-tier games; document expected FPS in the picker meta row |
| PicoDrive's standalone tree still assumes libretro frontend macros | Low-Medium | Phase 0 shakedown; fallback is vendoring only `Pico/` + `cpu/` + `sound/`, stubbing `platform/` ourselves |
| FAME opcode tables too big for flash budget | Low | Trim opcodes MD never exercises (MOVEP on sound latch is real, keep that; TAS on IO we can stub) |
| YM2612 eats core0 during render | Medium | Pin YM2612 sample step to core1 alongside 68K; measure in Phase 2 |
| 2 MB flash blocks big-cart library | Accepted | Document supported list; SSF2/Pier Solar deferred |
| Combined firmware too close to 520 KB SRAM | Medium | BSS-union at Phase 4, not Phase 6 — bring it forward if Phase 3 resident usage > 400 K |
| Save-state format churn across four cores | Low | Existing `thumby_state_bridge` already handles three; PicoDrive has its own `state.c` we patch the same way |

---

## 11. Open questions (answered — 2026-04-21)

1. **"Drive" = Mega Drive / Genesis** ✓
2. **22050 Hz mono audio downmix** ✓
3. **OC to 250 MHz acceptable** ✓
4. **32X / MegaCD / SVP out of scope** ✓

---

## 12. First concrete actions

1. `git clone https://github.com/notaz/picodrive vendor/picodrive` at
   pinned commit; trim `cpu/cyclone/`, `platform/`, non-MD sound.
   Update `vendor/VENDORING.md` with the new component and hash.
2. Write `src/md_core.[ch]` mirroring `sms_core` exactly.
3. Extend `CMakeLists.txt`: add `libpicodrive` static target, hook it
   into `thumbynes`, add `mdbench` executable.
4. `mdbench sonic.md` should print frames-per-second. That's Phase 0.
5. Extend `host_main.c` dispatch to route `.md`/`.bin`/`.gen` to a
   new `md_host_main.c`. SDL2 audio path already exists — reuse it.
   That's Phase 1.
6. Before touching device code, bench on host throttled to one core
   and estimate MHz-per-frame. That's Phase 2 — the go/no-go gate.

---

*One last note: if Phases 0–2 reveal that PicoDrive's FAME core is
too slow on M33 even with dual-core + OC, the escape hatch is to
accept frameskip 1 as the default and market ThumbyDrive as "30 fps
MD" for sprite-heavy titles. The Mega Drive library is playable at
30 fps — most emulation handhelds shipped exactly that profile for a
decade. Native 60 is the stretch goal, not a hard gate.*

*End of plan.*
