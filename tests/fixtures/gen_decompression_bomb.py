#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate tests/fixtures/flaw_corpus/corrupt/decompression-bomb.pdf.

Meta-audit follow-up F16, gap PDF-109 (merges SEC-37).

WHY THIS FILE EXISTS
--------------------
`tests/fixtures/flaw_corpus/COVERAGE.md` §8 excluded the upstream `bomb-giant`
fixture "(needs a resource-limited sandbox harness)". No such harness existed,
so the exclusion's stated reason was false: nothing was waiting on anything.
Meanwhile the two tests that carry "bomb" in their names are not bombs at all --
`PerfStressTest.ExtremeCompressionRatioBounded` compresses the 5-byte string
"Hello" (expansion ratio 0.4:1) and
`MaliciousPdfTest.DecompressionBombHandledGracefully` uses ten literal 'x'
characters that are not even valid zlib.

F16 built the harness (`tests/test_resource_bomb_sandbox.cpp`) and this is the
input it needs: a REAL decompression bomb, committed, deterministic, and
non-malicious.

THE BOMB
--------
One page whose content stream is `EXPANSION_BYTES` of PDF whitespace compressed
with zlib at level 9. Whitespace is the highest-ratio payload zlib can produce
(~1000:1) and is completely inert once decompressed -- it lexes to zero
operators, so the file cannot draw anything, cannot reference a font, and cannot
carry a payload. What it CAN do is force a decompressor that materialises its
output instead of streaming it to allocate half a gigabyte from a file a
thousand times smaller. That is the property under test.

The header is `%PDF-1.4` with a classic (non-stream) xref table so the bomb is
in the CONTENT path only -- structural parsing stays trivial and any resource
blow-up observed by the harness is attributable to the compressed stream.

DETERMINISM
-----------
Pure zlib + byte assembly: no PyMuPDF, no timestamps, no document ID, no
randomness. Re-running this script byte-for-byte reproduces the fixture, so the
committed file can be verified with a hash.

Usage:  python tests/fixtures/gen_decompression_bomb.py [--check]
        --check recomputes the bytes and compares without writing.
"""
from __future__ import annotations

import argparse
import hashlib
import sys
import zlib
from pathlib import Path

# 512 MiB of decompressed output. Large enough that materialising it is
# unmistakable against the harness's 512 MiB job cap, small enough that a
# streaming decompressor walks it in seconds. The harness restates this number;
# they are cross-checked by ResourceBombSandbox.FixtureIsARealDecompressionBomb.
EXPANSION_BYTES = 512 * 1024 * 1024

OUT_PATH = (Path(__file__).resolve().parent
            / "flaw_corpus" / "corrupt" / "decompression-bomb.pdf")


def build_pdf() -> bytes:
    # Inert payload: PDF whitespace only, one run, which is what lets zlib reach
    # its ~1000:1 ceiling. Once decompressed it lexes to zero operators.
    payload = b" " * (EXPANSION_BYTES - 1) + b"\n"

    comp = zlib.compressobj(9, zlib.DEFLATED, 15, 9)
    stream = comp.compress(payload) + comp.flush()

    objects: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << >> /Contents 4 0 R >>",
        (b"<< /Length " + str(len(stream)).encode() + b" /Filter /FlateDecode >>\n"
         b"stream\n" + stream + b"\nendstream"),
    ]

    out = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += str(i).encode() + b" 0 obj\n" + body + b"\nendobj\n"

    xref_at = len(out)
    out += b"xref\n0 " + str(len(objects) + 1).encode() + b"\n"
    out += b"0000000000 65535 f \n"
    for off in offsets[1:]:
        out += ("%010d 00000 n \n" % off).encode()
    out += (b"trailer\n<< /Size " + str(len(objects) + 1).encode()
            + b" /Root 1 0 R >>\nstartxref\n"
            + str(xref_at).encode() + b"\n%%EOF\n")
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify the committed fixture without writing")
    args = ap.parse_args()

    data = build_pdf()
    digest = hashlib.sha256(data).hexdigest()
    ratio = EXPANSION_BYTES / len(data)

    if args.check:
        if not OUT_PATH.exists():
            print("[ERROR] missing: %s" % OUT_PATH)
            return 1
        on_disk = OUT_PATH.read_bytes()
        if on_disk != data:
            print("[ERROR] %s differs from the generator output "
                  "(on disk %d bytes, generated %d bytes)"
                  % (OUT_PATH, len(on_disk), len(data)))
            return 1
        print("[OK] %s matches (sha256 %s)" % (OUT_PATH.name, digest))
        return 0

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_bytes(data)
    print("[OK] wrote %s" % OUT_PATH)
    print("     compressed   : %d bytes" % len(data))
    print("     decompressed : %d bytes" % EXPANSION_BYTES)
    print("     ratio        : %.0f:1" % ratio)
    print("     sha256       : %s" % digest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
