# Dual-boot plan: ThumbyNES + MicroPython Tiny Game Engine

**Goal.** One firmware image that a user flashes once and gets both
ThumbyNES (NES / SMS / GG / GB emulator) *and* MicroPython + the
Tiny Game Engine (the stock Thumby Color experience: pure Python
games built on the engine's C module). No re-flashing to swap
between them. From the user's perspective: drop a `.nes` / `.sms`
/ `.gg` / `.gb` ROM or a MicroPython game folder onto the USB drive,
see them all in the same picker, launch, play, return.

**Not a goal for v1.** Same-millisecond switching between modes. A
~1-second reboot-crossing between tabs is acceptable.


## Why co-residency doesn't work

The Thumby Color is 512 KB SRAM + 16 MB flash, no PSRAM. ThumbyNES's
resident footprint is ~250 KB (core BSS per active emulator + 128×128
RGB565 framebuffer + menu backdrop + IRAM'd hot loops + Pico SDK
runtime + FatFs work area). MicroPython + Tiny Game Engine adds:

- MicroPython interpreter BSS: ~60-100 KB
- Tiny Game Engine C module BSS: ~50-100 KB (framebuffer, depth
  buffer, audio ring, scene graph pools, resource loaders)
- MP GC heap for Python-side state: realistically ≥150 KB to run
  anything interesting

Stacking those on top of ThumbyNES's resident state lands around
500 KB before the MP game itself allocates anything. No room. Cannot
unload ThumbyNES's BSS at runtime on Cortex-M (BSS is statically
placed by the linker).


## Architecture: A/B firmware slots + reboot handoff

Two separate firmware images live in flash at fixed offsets. At any
moment exactly one is running, so each one has the full 512 KB SRAM
to itself. Crossing between them = `watchdog_reboot()` into the
other slot, reading a small handoff struct to decide what to do.

### Flash layout (proposed)

```
0x10000000   ThumbyNES image      ~1.5 MB cap   (slot A)
0x10180000   mp-thumby image      ~1.5 MB cap   (slot B)
0x10300000   handoff sector       4 KB (one flash sector)
0x10301000   FAT16 volume         ~12.5 MB      (same volume both firmwares see)
```

The current ThumbyNES firmware is ~1.25 MB, the stock mp-thumby
firmware is ~1.8 MB but could be trimmed. Both fit in 1.5 MB slots
with breathing room for growth.

### Handoff struct

4-byte-aligned, one flash sector. Written by whichever slot is
about to reboot; read by the other slot on first boot code.

```c
struct dualboot_handoff {
    uint32_t magic;          /* 'DBHO' */
    uint32_t version;        /* = 1    */
    uint32_t target_slot;    /* 0 = ThumbyNES, 1 = mp-thumby */
    uint32_t action;         /* 0 = lobby, 1 = launch named game */
    char     path[96];       /* game path if action == 1 */
    uint32_t crc32;          /* over the fields above */
};
```

- To hand *from* ThumbyNES *to* mp-thumby with a named game: write
  `{target_slot=1, action=1, path="/Games/Foo"}` → `watchdog_reboot()`.
- To hand *from* mp-thumby *to* ThumbyNES after the user exits:
  write `{target_slot=0, action=0}` → `watchdog_reboot()`.
- On boot, each slot's stage-0 reads the handoff. If it's addressed
  at the other slot, the boot ROM can jump there via the partition
  table (RP2350 supports alternate XIP starts); if it's addressed
  here, clear it and proceed normally.
- CRC is belt-and-braces for power-loss during the write.

### Boot-decision flow

Simplest implementation: ThumbyNES remains the default boot target.
Its very first instructions read the handoff. If it says
`target_slot=1`, it sets up the alternate-boot vector and issues a
second watchdog reboot that lands in mp-thumby's slot. mp-thumby
similarly reads the handoff; if it says `target_slot=0` (or the
struct is blank), it clears and reboots back to slot A.

RP2350 has enough boot-ROM / partition-table machinery to support
this cleanly — we don't need to write our own second-stage
bootloader. The Pico SDK docs for RP2350 have a section on "slot
layouts" and `pico_set_binary_type` with alternate start addresses
that covers the build-system side.


## The two decisions worth making up front

### 1. Shared filesystem

mp-thumby stock uses LittleFS for its user-facing FS (that's how
Thonny pushes `.py` files, how the Thumby Color Arcade downloads
games). ThumbyNES uses FAT because USB MSC is cleanest against FAT
and Windows File Explorer tolerates mid-flight FAT BPB better than
LittleFS. For a unified picker + one USB drive the user sees, pick
one filesystem.

**Recommendation: switch mp-thumby to a FatFs VFS.** MicroPython
supports FatFs natively — it's a config change in the `mpconfigport.h`
/ board config, not a rewrite. Then a folder like `/Games/Foo/main.py`
on the FAT volume is visible to both firmwares. Downsides: mp-thumby
users accustomed to the stock LittleFS layout need to adapt;
mp-thumby tooling that assumes LittleFS (if any) needs checking.

Alternative (worse): dual FS, each firmware reads the other's FS
read-only. Doubles the code surface and complicates the picker's
"Thumby tab" enumeration.

### 2. Plain Thumby lobby access

Do we expose the stock Thumby Color experience (Thonny REPL, the
Arcade menu, `main.py` auto-run) as a first-class option? Two
positions:

**A. Yes — add a picker menu item "Boot Thumby firmware"** that
writes `{target_slot=1, action=0}` (no named game) and reboots.
User then has full stock mp-thumby until they reboot the device.
Preserves compatibility with existing Thumby Color workflows.

**B. No — slot B only ever boots with a named game** and returns
directly to ThumbyNES on exit. Simpler mp-thumby patch; ThumbyNES
becomes the canonical resting state. Loses access to stock tooling
like Thonny's REPL unless flashed back to stock mp-thumby.

Recommendation: **A**. The overhead is tiny (one more menu item)
and it avoids cutting users off from the broader Thumby ecosystem.


## Changes needed in each image

### ThumbyNES (slot A)

1. Picker gains a **Thumby tab** alongside NES / SMS / GG / GB:
   enumerate `/Games/*/main.py` (or similar glob) on the FAT volume,
   show in the same picker UI with procedural cartridge icons.
2. Launch path writes the handoff struct and reboots. No emulator
   core involved.
3. Picker menu gets **"Boot Thumby firmware"** as a one-shot action
   (decision 2A).
4. On cold boot: read handoff; if `target_slot == 1`, re-reboot into
   slot B. Clear handoff unconditionally once consumed.

### mp-thumby (slot B)

1. Build with alternate start address at `0x10180000` (linker
   tweak, Pico SDK supports it).
2. Switch to FatFs VFS (decision 1).
3. On Python-side boot: read handoff struct; if `action == 1`,
   exec the named game directly (`exec(open("/Games/Foo/main.py").read())`)
   instead of dropping to the stock lobby.
4. Add an exit mechanism — a MENU-chord that writes
   `{target_slot=0}` and reboots. Could be the same MENU-long-hold
   pattern ThumbyNES uses in its in-game menu for "Quit to picker".
5. On unhandled exceptions during a Python game, optionally still
   reboot to slot A rather than landing in the REPL.


## Suggested work order

Incremental — each step testable in isolation before moving on.

1. **Spike: build mp-thumby at `0x10180000`.** Linker script change
   only; no handoff logic yet. Flash it by hand alongside an
   unmodified ThumbyNES; confirm you can manually boot either by
   picking the appropriate `.uf2`. Answers the "is the SDK
   alternate-boot-start stable?" question with a one-afternoon spike.

2. **Handoff protocol in ThumbyNES.** Write the struct + CRC on
   launch from a dummy "Boot Thumby" picker item, issue
   `watchdog_reboot()`. Verify the contents in the other slot with a
   test build of mp-thumby that just prints the handoff over USB
   serial and blinks.

3. **mp-thumby autorun from handoff.** Add the Python-side handoff
   reader + `exec()` path. Get a real Thumby game launching from
   ThumbyNES's picker via reboot.

4. **Return path.** Mp-thumby's quit-menu chord writes a return
   handoff and reboots back.

5. **Thumby tab in the picker.** UI work on ThumbyNES's side —
   glob `/Games/*/main.py`, cart icons, launch dispatch. Reuses
   everything the existing picker already has.

6. **Shared-FAT switch on mp-thumby.** The biggest unknown. Swap
   the VFS, verify USB MSC exposes the same volume both firmwares
   see, adjust Python-side paths.

7. **Polish.** Error handling (corrupt handoff, missing game,
   slot-B crash → reboot to slot A), screenshot sharing, favorites
   that span tabs, etc.


## Effort estimate

If decisions 1 (switch mp-thumby to FatFs) and 2A (include
"Boot Thumby firmware") land cleanly, realistic weekend-scale
project — maybe two weekends for the step 1-6 spike-through-spec,
another for polish. The partition-table setup and the mp-thumby VFS
swap are the two unknowns; both are Pico SDK / MicroPython config
work rather than new code.

The hard part is not the architecture — it's disciplined scope
management. Resist the temptation to expand "Thumby tab" into "also
the Arcade's download flow" or "also keep LittleFS as a legacy
compat layer"; each of those doubles the work.


## Open questions for before implementation

- **mp-thumby revision.** Which upstream checkout of
  `TinyCircuits-Tiny-Game-Engine` do we pin to? The stock Thumby
  Color firmware's MicroPython version, or a more recent build?
- **Screenshot sidecars for Thumby games.** Do we carry the same
  `.scr32` / `.scr64` convention? Probably yes — mp-thumby gets a
  "save screenshot" chord and writes the sidecars to the FAT volume
  alongside the game folder.
- **Overclock.** mp-thumby has its own system-clock setup. Does
  it honour ThumbyNES's `/.global` overclock value, or keep its own?
  Suggest: mp-thumby reads `/.global` on boot and applies the same
  rule — consistency matters more than a clean abstraction here.
- **Handoff CRC failure mode.** What if the handoff struct is
  corrupt on boot? Safe default: treat as "boot normally into this
  slot's lobby" and log. Never get stuck in a reboot loop.
