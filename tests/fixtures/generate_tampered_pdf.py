#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate test_tampered.pdf from test_signed.pdf by mutating a byte
outside the embedded ByteRange hash coverage. The resulting file should
open normally but fail signature verification.

Why a byte OUTSIDE the ByteRange? Because a byte-swap inside the
signed range would typically produce a parseable-but-invalid-hash
state, which is exactly what "tampered" means. The PDF's /ByteRange
covers [0, sigOffset] and [sigOffset+sigLen, EOF], leaving a gap for
the signature contents itself. Flipping a byte there wouldn't affect
the signature's hash — we want to flip a byte INSIDE the signed range
so the hash check fails. The /Contents area between the two ByteRange
segments is one of the few places that's NOT covered, so we must
choose a byte outside it.

This script reads the /ByteRange from the source signed PDF, then
chooses a byte offset inside the first ByteRange segment (well after
the file's %PDF header but before the signature dictionary) and flips
its low bit.
"""

from pathlib import Path
import re
import sys


def main() -> int:
    here = Path(__file__).parent
    src = here / "test_signed.pdf"
    dst = here / "test_tampered.pdf"

    if not src.exists():
        print(f"[skip] {src.name} missing — run generate_signed_pdf.py first")
        return 0

    data = bytearray(src.read_bytes())

    # Try to locate a /ByteRange to flip a byte inside the signed region
    # — the "proper" tampering a signature-verification test should see.
    m = re.search(
        rb"/ByteRange\s*\[\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*\]", data)
    target = -1
    if m:
        a, b, _c, _d = (int(x) for x in m.groups())
        if b > 200:
            target = a + 100
        elif b > 1:
            target = a + b // 2

    # Fallback for fixtures that don't carry a real signature yet
    # (current test_signed.pdf): flip a byte well past the %PDF header
    # but before the xref table. Enough to break content-hashing
    # verification logic and keep the document parseable.
    if target < 0 and len(data) > 512:
        target = min(500, len(data) // 3)

    if target < 0:
        print(f"[skip] cannot pick tamper offset in {src.name}")
        return 0

    original = data[target]
    data[target] = original ^ 0x01   # flip the low bit
    dst.write_bytes(bytes(data))

    print(
        f"[ok] {dst.name}: flipped bit at offset {target} "
        f"(was 0x{original:02x}, now 0x{data[target]:02x})"
    )
    print("   This file should open but fail signature verification.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
