# PCE heap-corruption bug — open investigation

## TL;DR

PCE init produces a **wild write** somewhere that can corrupt newlib
heap metadata. Symptom: a `malloc(32 KB) + free` cycle anywhere later
in the same firmware-image session leaves newlib's free-list in a
state where the next `malloc(64 KB)` walks corrupted bin pointers and
hangs. Because nes_menu_run mallocs a 32 KB fb_dim, opening the
in-game menu hung SMS later in the same session (PCE → SMS at
`render_init`'s `malloc(0x10000)`).

We have a **stable workaround** in place but the actual write source
has not been identified. Re-read this if symptoms come back or if
new PCE features ever land. See `src/pce_core.c::my_special_alloc`
and the surrounding comment for the live workaround.

## The workaround

`my_special_alloc` allocates every PCE buffer as `[256 B pad][user
data][256 B pad]`, and the small node-tracker struct (`pce_alloc_node_t`)
is also allocated with 256 B of pad on each side. Total overhead:
~15 KB heap per PCE session. The pad shifts the heap layout enough
that the wild write lands on harmless pad bytes instead of metadata.

Two related, smaller permanent fixes are also in:

  * **Path-string globals** (`cart_name`, `rom_file_name`, the 11
    PATH_MAX-sized strings, and `spr_init_pos`) are now NULL'd in
    `pcec_shutdown`. Without that, a second PCE session in the same
    firmware image saw stale non-NULL pointers, skipped re-allocation
    in `pcec_init`'s `if (!p)` checks, and `strcpy(cart_name, …)` in
    `CartLoad` wrote to freed heap. This was a real, separate
    use-after-free root-caused and fixed.
  * **PSG channel-select clamp** (`io.psg_ch = V & 7` → also `if (>5)
    clamp to 0`). Real HuC6280 ignores writes to PSG channels 6/7;
    upstream's `& 7` would have indexed `io.psg_da_data[6]` 1-of-3
    bytes worth past the 6-pointer array, producing a wild `uint8_t*`
    that the next DDA write would deref. Probably benign on most
    games (none we tested wrote 6/7) but a real OOB nonetheless.

## What we tried and ruled out

| Hypothesis                                         | Result               |
|----------------------------------------------------|----------------------|
| OOB write past a `my_special_alloc`'d data buffer  | trail+lead canary clean (256 B each) |
| OOB write past `pce_fb_back`                       | trail+lead canary clean |
| OOB write past direct mallocs (palette, bram, sbuf, spread8, fb_dim) | trail (and lead where added) clean |
| OOB write past node-tracker structs                | lead+trail canary clean |
| Stack overflow corrupting heap globals             | reducing stack pressure didn't help |
| `pce_render_scanline` BG/sprite/emit writes        | disabling each in turn — none of them are it |
| `pce_render_frame_begin`'s 32 KB memset            | disabling didn't fix |
| Audio path (`pcec_audio_pull` → `WriteBuffer`)     | disabling didn't fix |
| Trailing frame after menu close                    | `continue;` instead of `break;` didn't fix |
| Stale `pce_fb_back_raw` between sessions           | not it (sessions free properly) |
| FAT/`f_open` sidecar reads                         | not it |
| `thumbyone_settings_load_brightness`               | not it |
| `nes_menu_run` body: malloc / sleep / free         | none of these in isolation cause it |

## Strongest remaining hypotheses for the wild write

1. **Newlib chunk header between two of our allocations**. Each
   newlib allocation has an 8-byte header just before the user pointer.
   With our padding, between `alloc_A`'s trail canary and `alloc_B`'s
   lead canary sits exactly that 8-byte chunk header for `alloc_B`.
   Our canaries don't cover those bytes. A write at offset 257..264
   past `alloc_A`'s data lands on `alloc_B`'s chunk header.

2. **A write to a fixed absolute address** (not buffer-relative). The
   workaround works because shifting the heap moves what's *at* that
   address. If we ever found the address it'd point straight at the
   bug. Try writing a tracer that detects which heap byte first
   changes between "post-init" and "after first malloc/free".

3. **`ROMMapW[V]` for some bank `V` pointing at a buffer smaller than
   8 KB**, allowing CPU writes via `PageW[memreg][addr]` to escape
   the allocation. Our 256 B stub buffers (`PCM`, `VRAM2`, `VRAMS`,
   `vchange`, `vchanges`, `cd_extra_*`, `ac_extra_mem`,
   `cd_sector_buffer`) were exactly this risk — we bumped them to
   256 B but if any *bank* still maps to one of these, an 8 KB
   bank-relative write stomps 7.75 KB of adjacent heap. **Audit
   `pce.c::CartLoad`'s `ROMMapW` setup** to confirm none of these
   stubs are ever pointed at by a `PageW`-routed bank.

4. **Newlib BSS globals (`__malloc_av_`, fastbins array)**. A wild
   pointer landing on these breaks the allocator without touching
   anything we canary. Hard to detect without instrumenting newlib.

## Suggested next investigation step

Add a per-frame heap-walker that calls `mallinfo()` and checks the
delta from baseline. If a specific frame number causes a sudden
ordblks/fordblks change, we know within-1-frame when the corruption
happens. A more invasive version walks the actual free-list and
verifies forward/back pointers are consistent.

Also worth: bump the my_special_alloc pad from 256 B to 512 B and
see if the workaround still holds. If it does, the wild write is
within ±256 B of some allocation. If it stops working, the address
is more specific.

## Files / lines to remember

  * `src/pce_core.c::my_special_alloc` — the workaround. Comment
    block above it explains what to check.
  * `src/pce_core.c::pcec_shutdown` — the NULL'd path-string list.
  * `vendor/huexpress/engine/IO_write.h::case 0x0800/case 0` — PSG
    channel clamp.
  * `vendor/huexpress/engine/hard_pce.c` — 256 B stubs and trap_ram
    routed through my_special_alloc.

## How we know the workaround is stable

Tested on device:
  * PCE → SMS (the original failure)
  * PCE → PCE (re-entry with stale-pointer use-after-free)
  * PCE → SMS → GB → GG → MD → PCE (the long chain that surfaced
    the path-string bug as a canary fire on the seq=39 size=32 PageR)

All paths boot cleanly with the workaround in place.
