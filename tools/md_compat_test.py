#!/usr/bin/env python3
"""
Batch compatibility tester for the MD/Genesis core via mdhost.

For every .md ROM in the supplied directory, runs mdhost with a frame
cap, captures stdout/stderr/exit, parses the MDHOST_SUMMARY line that
the host prints at exit, converts the final-frame RGB565 dump to PNG,
and writes a Markdown compatibility report.

Usage:
    python3 md_compat_test.py [--rom-dir DIR] [--out-dir DIR]
                              [--frames N] [--timeout SECS]
                              [--mdhost PATH] [--rom GLOB] [--no-snapshot]

Default ROM dir: /home/maustin/thumby-color/EMUBackup
Default output : /home/maustin/thumby-color/tmp/md_compat
Default frames : 2400 (40s @ 60Hz NTSC, 48s @ 50Hz PAL)
"""

import argparse
import dataclasses
import os
import re
import shlex
import struct
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image

DEFAULT_ROM_DIR = Path("/home/maustin/thumby-color/EMUBackup")
DEFAULT_OUT_DIR = Path("/home/maustin/thumby-color/tmp/md_compat")
DEFAULT_MDHOST  = Path("/home/maustin/thumby-color/ThumbyNES/build/mdhost")

MDC_MAX_W = 320
MDC_MAX_H = 240

SUMMARY_RE = re.compile(
    r"MDHOST_SUMMARY frames=(\d+) viewport=(\d+)x(\d+)@\((\d+),(\d+)\) "
    r"refresh=(\d+) nonblack=(\d+) total=(\d+) pct=([\d.]+)"
    r"(?: peak_pct=([\d.]+) peak_frame=(\d+))?"
)

# MD_SP_GUARD trip — emitted by md_sp_check_fail() in thumby_platform.c.
# Captures the offending 68K PC, A7, and the trailing opcode trace so the
# report can show a "where it died" entry instead of just "signal 6".
SP_GUARD_PC_RE = re.compile(
    r"MD_SP_GUARD: A7 left RAM.*?current 68K PC : 0x([0-9a-fA-F]+)\s+"
    r"A7 \(SP\)\s+: 0x([0-9a-fA-F]+)",
    re.DOTALL,
)
SP_GUARD_TRACE_LINE_RE = re.compile(
    r"^\s*\d+\s+PC=0x([0-9a-fA-F]+)\s+OP=0x([0-9a-fA-F]+)\s+SP=0x([0-9a-fA-F]+)",
    re.MULTILINE,
)


@dataclasses.dataclass
class RomResult:
    rom: str
    exit_code: int
    wall_seconds: float
    timed_out: bool
    frames: int
    viewport: str
    refresh: int
    nonblack_pct: float
    peak_pct: float
    peak_frame: int
    classification: str
    notes: str
    snapshot_path: str
    stderr_tail: str
    sp_guard_pc: str = ""    # 68K PC at SP-guard trip (hex string)
    sp_guard_sp: str = ""    # 68K A7 at SP-guard trip (hex string)
    sp_guard_trace: str = "" # last few opcodes from the ring (multiline)

    def md_row(self) -> str:
        notes = self.notes.replace("|", "\\|")
        snap = (f"![]({self.snapshot_path})"
                if self.snapshot_path else "")
        return (f"| {self.rom} | {self.classification} | {self.frames} | "
                f"{self.viewport} | {self.refresh} Hz | "
                f"final {self.nonblack_pct:.1f}% / "
                f"peak {self.peak_pct:.1f}% @f{self.peak_frame} | "
                f"{self.wall_seconds:.1f}s | "
                f"{notes} | {snap} |")


def rgb565_dump_to_png(raw_path: Path, out_path: Path,
                       crop=None) -> bool:
    """Convert a 320x240 RGB565 dump to PNG. Optionally crop to viewport."""
    if not raw_path.exists():
        return False
    data = raw_path.read_bytes()
    expected = MDC_MAX_W * MDC_MAX_H * 2
    if len(data) != expected:
        print(f"  warn: dump {raw_path} is {len(data)}B, expected {expected}",
              file=sys.stderr)
        return False
    pixels = struct.unpack(f"<{MDC_MAX_W * MDC_MAX_H}H", data)
    img = Image.new("RGB", (MDC_MAX_W, MDC_MAX_H))
    px = []
    for v in pixels:
        r = (v >> 11) & 0x1F
        g = (v >>  5) & 0x3F
        b = (v      ) & 0x1F
        # 5/6-bit -> 8-bit with bit replication.
        r = (r << 3) | (r >> 2)
        g = (g << 2) | (g >> 4)
        b = (b << 3) | (b >> 2)
        px.append((r, g, b))
    img.putdata(px)
    if crop:
        vx, vy, vw, vh = crop
        if vw > 0 and vh > 0:
            img = img.crop((vx, vy, vx + vw, vy + vh))
    img.save(out_path)
    return True


def classify(exit_code: int, timed_out: bool, frames: int,
             nonblack_pct: float, peak_pct: float,
             stderr_tail: str,
             sp_guard_pc: str, sp_guard_sp: str) -> tuple[str, str]:
    """Bucket result. Returns (classification, notes)."""
    notes = []

    # SP-guard trip beats raw signal classification — md_sp_check_fail
    # calls abort() (SIGABRT) after dumping the trace, so the exit code
    # alone would say "signal 6 (SIGABRT)". Surface the captured PC + SP
    # instead so the report shows what we know.
    if sp_guard_pc:
        return "sp_guard", (f"68K PC=0x{sp_guard_pc} A7=0x{sp_guard_sp} "
                            "(stack escaped RAM mirror — see trace)")

    # Hard failures first.
    if exit_code is not None and exit_code < 0:
        return "crash", f"signal {-exit_code} ({signal_name(-exit_code)})"
    if exit_code != 0 and not timed_out:
        # Check stderr for known failure modes
        if "mdc_init failed" in stderr_tail:
            return "init_fail", "mdc_init returned error"
        if "rom load failed" in stderr_tail:
            return "load_fail", "mdc_load_rom rejected the file"
        if "short read" in stderr_tail:
            return "io_fail", "ROM file too short / read error"
        return "fail", f"exit code {exit_code}"

    if timed_out and frames == 0:
        return "hang_early", "process killed by timeout, no MDHOST_SUMMARY"

    if frames == 0:
        return "no_frames", "process exited without printing summary"

    # Visual classification: bucket on PEAK non-black across the run,
    # not the final-frame snapshot. Many games land on a black fade or
    # transition at the cap frame even when they were rendering content
    # earlier — the peak avoids that false-negative.
    visual_pct = max(nonblack_pct, peak_pct)
    if visual_pct < 0.5:
        cls = "black"
    elif visual_pct < 5.0:
        cls = "mostly_black"
    else:
        cls = "ok"

    if timed_out:
        notes.append("timed out (still running)")

    return cls, "; ".join(notes)


def signal_name(sig: int) -> str:
    import signal as _s
    for name in dir(_s):
        if name.startswith("SIG") and not name.startswith("SIG_") \
                and getattr(_s, name, None) == sig:
            return name
    return f"sig{sig}"


def run_one(rom: Path, mdhost: Path, frames: int, timeout: float,
            out_dir: Path, snapshot: bool) -> RomResult:
    stem = rom.stem
    safe_stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", stem)
    raw_path = out_dir / f"{safe_stem}.raw"
    png_path = out_dir / f"{safe_stem}.png"

    env = os.environ.copy()
    env["MDHOST_MAX_FRAMES"] = str(frames)
    env["MDHOST_FB_DUMP"]    = str(raw_path)
    env["MDHOST_AUTO_START"] = "1"
    if "DISPLAY" not in env:
        env["DISPLAY"] = ":0"

    cmd = [str(mdhost), str(rom)]
    print(f"\n=== {rom.name} ===")
    print(f"    {' '.join(shlex.quote(c) for c in cmd)}")

    t0 = time.monotonic()
    timed_out = False
    try:
        proc = subprocess.run(
            cmd, env=env, capture_output=True, text=True,
            timeout=timeout, check=False,
        )
        exit_code = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr
    except subprocess.TimeoutExpired as te:
        timed_out = True
        exit_code = None
        stdout = (te.stdout or b"").decode(errors="replace") \
            if isinstance(te.stdout, (bytes, bytearray)) else (te.stdout or "")
        stderr = (te.stderr or b"").decode(errors="replace") \
            if isinstance(te.stderr, (bytes, bytearray)) else (te.stderr or "")
    wall = time.monotonic() - t0

    summary = SUMMARY_RE.search(stdout) or SUMMARY_RE.search(stderr)
    if summary:
        frames_out = int(summary.group(1))
        vw, vh, vx, vy = (int(summary.group(2)), int(summary.group(3)),
                          int(summary.group(4)), int(summary.group(5)))
        refresh = int(summary.group(6))
        pct = float(summary.group(9))
        peak_pct   = float(summary.group(10)) if summary.group(10) else pct
        peak_frame = int(summary.group(11))   if summary.group(11) else frames_out
    else:
        frames_out = 0
        vw = vh = vx = vy = refresh = 0
        pct = peak_pct = 0.0
        peak_frame = 0

    # Parse SP-guard output if present (combined stdout+stderr — abort()
    # output races flush so check both streams).
    combined = (stdout or "") + (stderr or "")
    sp_pc = sp_sp = ""
    sp_trace_lines = []
    m = SP_GUARD_PC_RE.search(combined)
    if m:
        sp_pc, sp_sp = m.group(1), m.group(2)
        # Grab the last 6 trace lines (most recent opcodes — older ones
        # are usually loop-spam, last one is the culprit).
        all_trace = SP_GUARD_TRACE_LINE_RE.findall(combined)
        for pc, op, sp in all_trace[-6:]:
            sp_trace_lines.append(f"PC=0x{pc} OP=0x{op} SP=0x{sp}")

    cls, notes = classify(exit_code if exit_code is not None else -1,
                          timed_out, frames_out, pct, peak_pct,
                          stderr[-2000:] if stderr else "",
                          sp_pc, sp_sp)

    snapshot_rel = ""
    if snapshot and raw_path.exists():
        crop = (vx, vy, vw, vh) if (vw > 0 and vh > 0) else None
        ok = rgb565_dump_to_png(raw_path, png_path, crop=crop)
        if ok:
            snapshot_rel = png_path.name

    # Cleanup the raw dump — PNG is the keep-around artifact.
    if raw_path.exists():
        try:
            raw_path.unlink()
        except OSError:
            pass

    stderr_tail = "\n".join(stderr.splitlines()[-8:]) if stderr else ""
    print(f"    exit={exit_code} wall={wall:.1f}s frames={frames_out} "
          f"final={pct:.1f}% peak={peak_pct:.1f}%@f{peak_frame} -> {cls}")
    if cls == "sp_guard":
        print(f"    SP_GUARD: 68K PC=0x{sp_pc} A7=0x{sp_sp}")
        if sp_trace_lines:
            print("    last opcodes:")
            for ln in sp_trace_lines:
                print(f"      {ln}")
    elif cls in {"crash", "init_fail", "load_fail", "fail", "hang_early",
                 "no_frames", "black"}:
        if stderr_tail:
            print(f"    stderr tail:\n      " + stderr_tail.replace("\n", "\n      "))

    return RomResult(
        rom=rom.name,
        exit_code=exit_code if exit_code is not None else -1,
        wall_seconds=wall,
        timed_out=timed_out,
        frames=frames_out,
        viewport=f"{vw}x{vh}@({vx},{vy})" if vw else "—",
        refresh=refresh,
        nonblack_pct=pct,
        peak_pct=peak_pct,
        peak_frame=peak_frame,
        classification=cls,
        notes=notes,
        snapshot_path=snapshot_rel,
        stderr_tail=stderr_tail,
        sp_guard_pc=sp_pc,
        sp_guard_sp=sp_sp,
        sp_guard_trace="\n".join(sp_trace_lines),
    )


def write_report(results: list[RomResult], out_dir: Path,
                 frames: int, timeout: float, mdhost: Path) -> Path:
    bucket_order = ["crash", "sp_guard", "fail", "init_fail", "load_fail",
                    "io_fail", "hang_early", "no_frames",
                    "black", "mostly_black", "ok"]
    by_bucket: dict[str, list[RomResult]] = {b: [] for b in bucket_order}
    for r in results:
        by_bucket.setdefault(r.classification, []).append(r)

    lines = []
    lines.append(f"# MD core compat report")
    lines.append("")
    lines.append(f"- mdhost: `{mdhost}`")
    lines.append(f"- frame cap per ROM: **{frames}**")
    lines.append(f"- wall timeout per ROM: **{timeout:.0f}s**")
    lines.append(f"- ROMs tested: **{len(results)}**")
    lines.append("")

    lines.append("## Summary by classification")
    lines.append("")
    lines.append("| Class | Count | Meaning |")
    lines.append("|---|---|---|")
    meanings = {
        "ok":           "boots, viewport >5% non-black at end of run",
        "mostly_black": "ran but final frame had little non-black content (likely menus/intros)",
        "black":        "ran but final frame is fully black (probably stuck)",
        "no_frames":    "process exited without printing summary",
        "hang_early":   "killed by wall-clock timeout, never reached summary",
        "io_fail":      "ROM file read error",
        "load_fail":    "mdc_load_rom rejected the ROM",
        "init_fail":    "mdc_init returned error",
        "fail":         "non-zero exit, not otherwise classified",
        "sp_guard":     "MD_SP_GUARD trip — 68K stack escaped RAM mirror; trace captured",
        "crash":        "killed by signal (segfault, abort, ...)",
    }
    for b in bucket_order:
        n = len(by_bucket.get(b, []))
        if n == 0 and b not in ("ok", "mostly_black", "black", "crash"):
            continue
        lines.append(f"| `{b}` | {n} | {meanings.get(b, '')} |")
    lines.append("")

    lines.append("## Per-ROM results")
    lines.append("")
    lines.append("| ROM | Class | Frames | Viewport | Refresh | "
                 "Non-black | Wall | Notes | Snapshot |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for b in bucket_order:
        for r in sorted(by_bucket.get(b, []), key=lambda x: x.rom.lower()):
            lines.append(r.md_row())
    # Anything in an unexpected bucket
    other = [r for r in results
             if r.classification not in bucket_order]
    for r in sorted(other, key=lambda x: x.rom.lower()):
        lines.append(r.md_row())
    lines.append("")

    sp_guard_fails = [r for r in results if r.classification == "sp_guard"]
    if sp_guard_fails:
        lines.append("## SP-guard traces")
        lines.append("")
        lines.append("Each entry shows the 68K state captured by "
                     "`md_sp_check_fail` when A7 left the RAM mirror "
                     "(0xE00000-0xFFFFFF). Last opcode in the trace is the "
                     "instruction that broke SP — usually a wild RTS/JMP "
                     "into bad data.")
        lines.append("")
        for r in sp_guard_fails:
            lines.append(f"### {r.rom}")
            lines.append("")
            lines.append(f"- **68K PC at trip**: `0x{r.sp_guard_pc}`")
            lines.append(f"- **A7 (SP) at trip**: `0x{r.sp_guard_sp}`")
            if r.sp_guard_trace:
                lines.append("- **Last 6 dispatched opcodes** (newest at bottom):")
                lines.append("```")
                lines.append(r.sp_guard_trace)
                lines.append("```")
            lines.append("")

    fails = [r for r in results
             if r.classification not in {"ok", "mostly_black", "sp_guard"}
             and r.stderr_tail]
    if fails:
        lines.append("## Failure stderr tails")
        lines.append("")
        for r in fails:
            lines.append(f"### {r.rom} — `{r.classification}`")
            lines.append("```")
            lines.append(r.stderr_tail)
            lines.append("```")
            lines.append("")

    report = out_dir / "compat.md"
    report.write_text("\n".join(lines))
    return report


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rom-dir", type=Path, default=DEFAULT_ROM_DIR)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    ap.add_argument("--mdhost",  type=Path, default=DEFAULT_MDHOST)
    ap.add_argument("--frames",  type=int,  default=2400)
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="Wall-clock kill timeout per ROM (seconds)")
    ap.add_argument("--rom", type=str, default="*.md",
                    help="Glob (or comma-separated globs) for ROM filenames "
                         "inside --rom-dir")
    ap.add_argument("--no-snapshot", action="store_true",
                    help="Skip writing PNG snapshots of the final frame")
    args = ap.parse_args(argv)

    if not args.mdhost.exists():
        print(f"mdhost not found: {args.mdhost}", file=sys.stderr)
        return 2
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rom_set = set()
    for pattern in args.rom.split(","):
        pattern = pattern.strip()
        if not pattern:
            continue
        rom_set.update(args.rom_dir.glob(pattern))
    roms = sorted(r for r in rom_set if r.is_file())
    if not roms:
        print(f"no ROMs match {args.rom_dir}/{args.rom}", file=sys.stderr)
        return 2
    print(f"Found {len(roms)} ROM(s) in {args.rom_dir}")
    print(f"Output dir: {args.out_dir}")
    print(f"Frame cap: {args.frames}, wall timeout: {args.timeout}s")

    results = []
    for rom in roms:
        results.append(run_one(rom, args.mdhost, args.frames, args.timeout,
                               args.out_dir, snapshot=not args.no_snapshot))

    report = write_report(results, args.out_dir, args.frames, args.timeout,
                          args.mdhost)
    print(f"\nReport: {report}")
    print(f"Snapshots: {args.out_dir}")
    n_ok = sum(1 for r in results if r.classification == "ok")
    print(f"OK: {n_ok}/{len(results)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
