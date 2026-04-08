# Adding SMS / Game Gear to ThumbyNES

Plan for integrating Sega Master System and Game Gear support into the
existing ThumbyNES firmware as a second emulator core. Picker auto-routes
by file extension; everything else (LCD driver, audio, USB MSC, FAT,
favorites, picker, sleep, autosave, scale modes, palette toggles, region
auto-detect, per-ROM cfg, battery saves) is reused as-is.

This is a planning document, not committed code. Tomorrow's starting point.

---

## 1. Decision: vendor smsplus from retro-go

Looked at three candidates. **smsplus from retro-go** wins on every axis:

| Criterion | smsplus (retro-go) | original SMS Plus | PicoDrive |
|---|---|---|---|
| License | GPLv2 | BSD | LGPLv2 |
| Code size (LOC) | ~11k | ~8k | huge |
| Build pattern | identical to nofrendo (`#ifdef RETRO_GO` w/ printf fallback) | needs porting | needs porting |
| SMS + GG + Coleco | yes | yes | SMS only (well-supported), GG also |
| YM2413 FM (Japanese SMS) | yes | partial | no |
| Maintenance | active | unmaintained since 2004 | active |
| MCU prior art | retro-go ESP32 + RP2040 ports | older | well-known |

The retro-go variant is the right pick for the same reason nofrendo from
retro-go was the right pick for the NES core: same upstream repo, same
clean standalone fallback in `shared.h` (`#define IRAM_ATTR`,
`LOG_PRINTF` → `printf`), same pinning workflow, same patch surface.
Zero patches needed to start — verified by reading `shared.h`.

GPLv2 is already our license (matches nofrendo) so no compatibility friction.

**Vendor at:** `vendor/smsplus/` from retro-go commit `4ced120…` (the commit
already pinned in `vendor/VENDORING.md` for nofrendo). Pin the same revision
for consistency.

---

## 2. smsplus API surface

Mapped to the same five operations our `nes_core` exposes for nofrendo.
The names are different but the semantics are identical.

| Our wrapper (`sms_core.h`) | smsplus call |
|---|---|
| `smsc_init(int system, int sample_rate)` | set `option.console`, `option.sndrate`, allocate `bitmap.data`, set `bitmap.{width,height,pitch}` |
| `smsc_load_rom(const uint8_t *data, size_t len)` | `load_rom((void*)data, max(0x4000, len), len)` then `system_poweron()` |
| `smsc_run_frame()` | `system_frame(0)` |
| `smsc_framebuffer()` | returns `bitmap.data` (8-bit palette indices, `bitmap.pitch` stride) |
| `smsc_palette_rgb565()` | calls `render_copy_palette(s_palette)` and returns `s_palette` |
| `smsc_set_buttons(uint8_t mask)` | writes `input.pad[0]` and `input.system` |
| `smsc_audio_pull(int16_t *out, int n)` | reads `snd.stream[0]` for `snd.sample_count` samples (mono mix from L+R if needed) |
| `smsc_shutdown()` | `system_shutdown()` |
| `smsc_battery_ram()` / `smsc_battery_size()` | returns `cart.sram` / `0x8000` if `cart.sram` is non-NULL |

**Display dimensions:**
- SMS: 256×192 (`SMS_WIDTH` × `SMS_HEIGHT`)
- GG : 160×144 (`GG_WIDTH` × `GG_HEIGHT`)

**Console enum** from `sms.h`:
- `option.console = 0` → SMS (auto-detect SMS1 vs SMS2 vs export vs Japan)
- `option.console = CONSOLE_GG` (`0x40`) → Game Gear
- `option.console = CONSOLE_GGMS` (`0x41`) → Game Gear in SMS-compat mode (rare)
- We won't expose Coleco/SG-1000 — out of scope.

**Bitmap allocation:** `bitmap.data` must be allocated by us before
`system_poweron`. Size = `width * height` bytes (8-bit indices). For SMS
that's 256 × 192 = **49,152 bytes**. We'll declare a static buffer.

**Audio:** smsplus runs at a configurable rate (`option.sndrate`) and
produces `snd.sample_count = sndrate / fps + 1` samples per frame into
two int16 streams `snd.stream[0]` (L) and `snd.stream[1]` (R). Internal
NTSC fps = 60, PAL fps = 50 — same auto-pacing logic as nofrendo.

We want **22050 Hz mono** to match the existing PWM driver. Mono mix is
`(L + R) / 2` per sample. Trivial.

**Input bits** (from `system.h`):
- `INPUT_UP/DOWN/LEFT/RIGHT/BUTTON1/BUTTON2` go in `input.pad[0]`
- SMS Pause goes in `input.system` (`INPUT_PAUSE`)
- GG Start goes in `input.system` (`INPUT_START`)

---

## 3. Memory budget revisited

Current ThumbyNES static + new SMS additions:

| Region | Bytes | Notes |
|---|---|---|
| Nofrendo PPU vidbuf | 65 K | unchanged |
| LCD framebuffer (128×128×16) | 33 K | shared between cores |
| Nofrendo CPU/PPU/APU/mapper state | ~30 K | unchanged |
| Audio ring | 8 K | shared |
| FatFs work area + flash disk cache | 12 K | shared |
| Picker / favorites / cfg | 6 K | shared (favorites RAM grows by 0) |
| **NEW:** smsplus bitmap.data | **49 K** | 256×192 |
| **NEW:** smsplus VRAM (`vdp.vram`) | 16 K |  |
| **NEW:** smsplus work RAM (`sms.wram`) | 8 K |  |
| **NEW:** smsplus other internal state | ~12 K | tile cache, palette, line buf |
| **NEW:** smsplus audio streams (`snd.stream[0..1]`) | ~2 K | calloc'd at sound_init |
| **Total static (combined)** | **~241 K** | up from ~155 K |

Free heap drops from ~340 K → ~270 K. Still well above the working set
either core needs (peak heap usage in ThumbyNES is ~10 K).

**No game in either system should fail to load.** ROMs are XIP-mapped via
the existing `nes_picker_mmap_rom()` path so a 1 MB SMS ROM doesn't touch
heap. The XIP loader is system-agnostic — it's just "find this file's
contiguous flash address" — and reuses verbatim.

Flash text: smsplus is ~80–120 KB compiled. UF2 grows from ~470 KB to
~600–650 KB. Well within the 1 MB firmware budget.

**Strategy A (both static, always allocated)** is simpler and stays under
budget. Skip the BSS-union approach until it's actually tight.

---

## 4. File layout changes

Mostly additive — almost nothing in existing files needs to move.

```
ThumbyNES/
├── vendor/
│   └── smsplus/             ← NEW vendored core (GPLv2, retro-go @ 4ced120)
├── src/
│   ├── nes_core.[ch]        ← unchanged
│   ├── sms_core.[ch]        ← NEW thin wrapper, same shape as nes_core
│   ├── host_main.c          ← extend to dispatch on .nes vs .sms vs .gg
│   └── bench_main.c         ← unchanged (NES bench)
└── device/
    ├── nes_run.[ch]         ← rename to runner.[ch], dispatch internally
    │                          OR add a sibling sms_run.[ch]
    ├── nes_picker.[ch]      ← extend extension list to include .sms / .gg
    └── nes_device_main.c    ← unchanged
```

**Picker changes** (small):

1. Extend `nes_picker_scan` to accept `.sms` and `.gg` in addition to `.nes`.
2. Add `system` field to `nes_rom_entry`: `SYS_NES`, `SYS_SMS`, `SYS_GG`.
   Detected from extension at scan time.
3. Show system tag in the picker's meta row alongside mapper/size/region:
   - NES → `m4  384K  NTSC`
   - SMS → `SMS  256K  NTSC`
   - GG  → `GG   128K  NTSC`
   Mapper number doesn't really apply to SMS (it has a single dominant
   bank-switch scheme), so we replace it with the system tag.
4. iNES 1.0 / 2.0 region detection only fires for `.nes` files. SMS region
   detection: filename heuristic only (`(E)`, `(Eu)`, `(Europe)` etc.) —
   smsplus has its own internal region auto-detect from ROM CRC (`set_rom_config()`)
   that we'll let do its own thing on the smsplus side.

**Runner changes:**

The cleanest split is to keep `nes_run.[ch]` as-is and add
`sms_run.[ch]` as a sibling, then have a thin top-level dispatcher in
`nes_device_main.c` look at `roms[sel].system` and call the right runner.
Both runners share the same `fb`, the same control polling, the same
sleep/autosave/cfg infrastructure — but the per-frame loop is core-specific.

Alternative: write a single `runner.c` that uses a small vtable
(`init/load/run/audio/buttons/teardown` function pointers) to abstract
over both cores. Cleaner abstraction but more refactoring up front. **For
the first pass, ship two parallel runners and refactor once both work.**

---

## 5. Display + scaling

The existing `blit_fit` / `blit_blend` / `blit_crop` paths in `nes_run.c`
are NES-specific only because they hardcode 256×240 → 128×120 with a
4 px letterbox top/bottom. Refactor each to take source `(w, h)` so we
can reuse them for SMS and GG.

| Source | Output (FIT 2:1) | Letterbox |
|---|---|---|
| NES 256×240 | 128×120 | 4 px top + 4 px bottom |
| SMS 256×192 | 128×96  | 16 px top + 16 px bottom |
| GG  160×144 | 80×72   | 24 px each side + 28 px top + 28 px bottom (or 1.6:1 native scale) |

For Game Gear's 160×144 we have a real choice: pure 2:1 wastes most of the
screen. Two better options:
1. **Asymmetric scale** — 160→128 horizontal (5:4), 144→128 vertical (9:8).
   Almost a native fill. Looks great.
2. **CROP at 1:1** — already pannable. Best for reading text.

I'd default GG to the asymmetric scale path. Add it as `blit_fit_gg` (or
parameterize the existing one with a per-source scale ratio table).

CROP for SMS: 128×128 native window into 256×192, pan range
`[0..128] × [0..64]`. Same gesture, same logic.

---

## 6. Picker dispatch + cfg

The cfg sidecar is already system-agnostic (scale, palette, volume,
fps, region, blend) — no changes needed. The same `<rom>.cfg` file
serves NES or SMS or GG; the runner that reads it just ignores fields
that don't apply (e.g. SMS uses smsplus's internal `option.tms_pal`
selection rather than nofrendo's six-palette picker, so the `palette`
byte is unused on the SMS side; that's fine).

Save sidecars: SMS battery is `cart.sram[0x8000]` when present (32 KB).
The existing `<rom>.sav` filename works, just with a different size.
`battery_load`/`battery_save` need to ask the active core for its
PRG-RAM pointer/size — refactor to a function pointer or two parallel
implementations.

Favorites file `/.favs` is already content-agnostic (just stores file
names) — works as-is.

---

## 7. Region / region auto-detect

smsplus has its own `set_rom_config()` that maps known ROM CRCs to
display mode (NTSC/PAL), region (export/Japan), and TMS-mode flag.
**We let smsplus do its own region work** — no parallel header
parsing needed for SMS.

For the cfg-driven manual override, the existing MENU + B chord still
works; we just route the new value into smsplus differently. Setting
`option.tv_mode = TV_PAL` or `TV_NTSC` before `system_poweron()` does
the right thing.

The picker's auto-detect for the metadata column can still use the
filename heuristic (`(E)` etc.) for display purposes, even if smsplus
will refine it on actual launch. If they disagree, the launch wins.

---

## 8. Phased plan

Mirror the original ThumbyNES phases — one feature per phase, get to
green at the end of each.

| Phase | Goal | Done when |
|---|---|---|
| **0** | Vendor smsplus, host build green | `smsbench` runs Sonic on Linux for 600 frames |
| **1** | SDL2 host runner with video / input / audio | Sonic playable on Linux at 60 fps with sound |
| **2** | Bench on host, decide go/no-go | Decision: ship, or fall back to original SMS Plus |
| **3** | Add `sms_run.[ch]` to device build, hardcoded ROM | A single hardcoded `Sonic.sms` plays on hardware |
| **4** | Picker dispatch by extension (.nes / .sms / .gg) | Mixed library: pick either system from one list |
| **5** | Battery saves + cfg + region + scale modes for SMS/GG | Feature parity with NES side |
| **6** | Picker polish (system tag in meta row, GG asymmetric scale) | Public-ready |

Effort estimate: Phases 0–2 are ~half a day on host. Phases 3–5 are ~one
day on device. Phase 6 is polish. Roughly **half the work of the original
ThumbyNES build** because all the scaffolding already exists.

---

## 9. Open questions

1. **Audio mono mix** — smsplus's L/R streams are mostly identical for
   SMS (PSG is mono), but Game Gear has a stereo PSG. Mono mix
   `(L + R) / 2` is fine for both, just slightly lossy on GG. Acceptable.
2. **Save state format** — smsplus has `state.c` with its own format,
   different from nofrendo's. We don't ship save states (battery only),
   so this only matters if Phase 5 wants to add them later.
3. **YM2413** — Japanese SMS games can use the YM2413 FM chip. smsplus
   supports it (`option.fm = 1`). Doesn't cost much to enable. Default off
   for safety, expose as a per-ROM cfg toggle if anyone asks.
4. **ColecoVision** — smsplus supports it. Not in scope for v1, but the
   `option.console = 6` path is there if we ever want it.
5. **Where to dispatch?** Two parallel runners (`nes_run.c` + `sms_run.c`)
   vs one with a vtable. Default to two; refactor only if duplication gets
   ugly.

---

## 10. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| smsplus doesn't standalone-build cleanly | Low | Already verified `shared.h` has `#ifndef RETRO_GO` fallback |
| Memory pressure under combined load | Low | Worst case 290 KB resident, leaves 230 KB heap |
| smsplus is too slow on RP2350 | Very Low | Z80+SMS VDP is much lighter than nofrendo and that runs full-speed |
| Picker UI gets messy with two file types | Low | System tag in meta row, no other changes |
| Save format mismatch confuses users | Low | `.sav` size differs but filename's the same; the runner reads what its core wants |
| Combined firmware bug crashes everything | Medium | Phase 3 keeps SMS path completely separate from NES path until proven |

---

## 11. First concrete actions tomorrow

1. `cp -r /tmp/retro-go/retro-core/components/smsplus vendor/smsplus`
2. Update `vendor/VENDORING.md` with the new component, same commit hash.
3. Write `src/sms_core.[ch]` (mirror `nes_core` exactly).
4. Update host `CMakeLists.txt` to add `libsmsplus.a` and a `smshost` /
   `smsbench` target.
5. `smsbench /path/to/sonic.sms` should print frames-per-second. That's
   Phase 0.
6. SDL host runner — extend `host_main.c` to dispatch by file extension,
   reusing the existing window/audio/input plumbing. That's Phase 1.

---

*One last note: if Phases 0–2 reveal that smsplus is in fact too coupled
to make standalone-friendly (it shouldn't be — `shared.h` is clean), the
fallback is the original [SMS Plus by Charles MacDonald](https://www.smspower.org/maxim/smsmusic/charles-mcdonald/sms_plus.zip)
which is BSD and even cleaner but unmaintained. Both work.*

*End of plan. Picking this up tomorrow with a fresh session.*
