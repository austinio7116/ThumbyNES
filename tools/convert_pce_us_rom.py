#!/usr/bin/env python3
"""
pce_convert.py — convert a US TurboGrafx-16 HuCard dump (bit-reversed)
into native PC Engine byte order so ThumbyNES will load it.

Mirrors the device-side logic in ThumbyNES/src/pce_core.c:
  - strips a copier header (any bytes before the last whole 0x2000 block)
  - detects US encoding via the reset-vector heuristic: native carts have
    body[0x1FFF] >= 0xE0; bit-reversed US dumps come out < 0xE0
  - bit-reverses every byte (the encoding is its own inverse)

Usage:
    python3 pce_convert.py INPUT.pce [OUTPUT.pce]
    python3 pce_convert.py INPUT.pce --force     # convert even if it looks native
If OUTPUT is omitted, writes alongside INPUT as "<name> (native).pce".
"""

import sys
import os

# 0..255 byte bit-reversal table
BITREV = bytes(int(f"{b:08b}"[::-1], 2) for b in range(256))


def strip_header(data: bytes):
    """Return (header_len, body). A copier header is any bytes before the
    last whole 0x2000 (8 KB) block — matches `len & 0x1FFF` on device."""
    hdr = len(data) & 0x1FFF
    return hdr, data[hdr:]


def is_us_encoded(body: bytes) -> bool:
    if len(body) < 0x2000:
        return False
    return body[0x1FFF] < 0xE0


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    force = "--force" in argv
    if not args:
        print(__doc__)
        return 2

    inp = args[0]
    if not os.path.isfile(inp):
        print(f"error: no such file: {inp}", file=sys.stderr)
        return 1

    data = open(inp, "rb").read()
    if len(data) < 0x2000:
        print(f"error: file too small ({len(data)} bytes) to be a HuCard", file=sys.stderr)
        return 1

    hdr_len, body = strip_header(data)
    if hdr_len:
        print(f"copier header detected: stripping {hdr_len} bytes")

    if is_us_encoded(body):
        print(f"detected US/TG-16 bit-reversed dump (body[0x1FFF]=0x{body[0x1FFF]:02X} < 0xE0) -> converting")
    else:
        msg = f"looks already native (body[0x1FFF]=0x{body[0x1FFF]:02X} >= 0xE0)"
        if not force:
            print(f"{msg}; nothing to do. Re-run with --force to convert anyway.")
            return 0
        print(f"{msg}; --force given, converting anyway")

    out_body = body.translate(BITREV)

    if len(args) >= 2:
        outp = args[1]
    else:
        root, ext = os.path.splitext(inp)
        outp = f"{root} (native){ext or '.pce'}"

    open(outp, "wb").write(out_body)

    # Verify the result reads as native now.
    ok = not is_us_encoded(out_body)
    print(f"wrote {outp} ({len(out_body)} bytes)")
    print(f"verify: output {'reads as native (good)' if ok else 'STILL flags as US-encoded — header may be off'}; "
          f"body[0x1FFF]=0x{out_body[0x1FFF]:02X}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
