# -*- coding: utf-8 -*-
"""Inspect /Fsub1 on edited page 3 to see whether its embedded TrueType
subset, /W array, /CIDToGIDMap and /ToUnicode actually map the CIDs we
wrote (0x0094 0x0008 0x0051 ...) to the correct glyphs.

If we wrote CID 0x0008 expecting it to render as 'E' but the subset's
GID 8 is some random Devanagari glyph, the output looks garbled — that
matches symptom B.
"""
from __future__ import annotations

import pathlib
import sys

import pikepdf

EDITED = pathlib.Path(r"C:\Users\dev\Desktop\Edited - The Lean Startup - Erick Ries.pdf")


def main() -> int:
    pdf = pikepdf.open(EDITED)
    page = pdf.pages[2]  # 0-based; "page 3" in 1-based UI
    fonts = page.obj["/Resources"]["/Font"]
    fsub1 = fonts["/Fsub1"]

    print("=== /Fsub1 top-level ===")
    print(repr(fsub1))
    print()

    desc = fsub1["/DescendantFonts"][0]
    print("=== descendant ===")
    print(repr(desc))
    print()

    fd = desc["/FontDescriptor"]
    print("=== FontDescriptor ===")
    print(f"  FontName: {fd.get('/FontName', None)}")
    if "/FontFile2" in fd:
        ff = fd["/FontFile2"]
        print(f"  FontFile2 length: {ff.get('/Length', None)}")
        print(f"  FontFile2 Length1: {ff.get('/Length1', None)}")
        # Try to read the embedded TrueType
        try:
            tt_bytes = ff.read_bytes()
            print(f"  FontFile2 decompressed bytes: {len(tt_bytes)}")
            print(f"  First 16 bytes (hex): {tt_bytes[:16].hex()}")
            print(f"  Magic (should be 00010000 for TT or OTTO for CFF): {tt_bytes[:4].hex()}")
            # Save it for offline analysis
            out = pathlib.Path(__file__).parent / "fsub1_embedded.ttf"
            out.write_bytes(tt_bytes)
            print(f"  WROTE embedded font -> {out.name}")
        except Exception as e:
            print(f"  read_bytes failed: {e}")

    print()
    print("=== /W array ===")
    w = desc.get("/W", None)
    if w is not None:
        # Convert to plain list
        wlist = list(w)
        print(f"  /W (len {len(wlist)}): {wlist}")

    print()
    print("=== /CIDToGIDMap ===")
    print(f"  {desc.get('/CIDToGIDMap', None)}")

    print()
    print("=== /ToUnicode CMap ===")
    tu = fsub1.get("/ToUnicode", None)
    if tu is not None:
        try:
            tu_text = tu.read_bytes().decode("latin-1")
            print(tu_text)
            # Save for analysis
            out = pathlib.Path(__file__).parent / "fsub1_tounicode.txt"
            out.write_text(tu_text, encoding="utf-8")
            print(f"WROTE -> {out.name}")
        except Exception as e:
            print(f"  read failed: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
