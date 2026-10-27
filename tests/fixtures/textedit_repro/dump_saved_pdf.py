# -*- coding: utf-8 -*-
"""Phase-1 helper: dump page-2 content stream and font resources from
the user's saved 'Edited - The Lean Startup - Erick Ries.pdf' so we can
verify (a) /Fsub1 subtype + encoding, (b) q...Q scope around BT...ET,
(c) the actual Tm/Tfs/Tj bytes that hit disk."""
from __future__ import annotations

import pathlib
import sys

import pikepdf

SAVED = pathlib.Path(r"C:\Users\dev\Desktop\Edited - The Lean Startup - Erick Ries.pdf")
OUT_DIR = pathlib.Path(__file__).parent
PAGE_INDEX = 1  # 0-based; "page 2" in the user's UI


def main() -> int:
    if not SAVED.exists():
        print(f"FAIL: saved PDF not found at {SAVED}", file=sys.stderr)
        return 2

    pdf = pikepdf.open(SAVED)
    if PAGE_INDEX >= len(pdf.pages):
        print(f"FAIL: PDF has {len(pdf.pages)} pages, index {PAGE_INDEX} out of range", file=sys.stderr)
        return 2

    page = pdf.pages[PAGE_INDEX]
    print("=" * 70)
    print(f"PAGE {PAGE_INDEX + 1} OBJECT")
    print("=" * 70)
    print(repr(page.obj))
    print()

    res = page.obj.get("/Resources", None)
    if res is None:
        print("(page has no /Resources)")
    else:
        print("=" * 70)
        print("PAGE RESOURCES")
        print("=" * 70)
        print(repr(res))
        print()

        fonts = res.get("/Font", None)
        if fonts is not None:
            print("=" * 70)
            print("FONT RESOURCE DICTS")
            print("=" * 70)
            for key in fonts.keys():
                fobj = fonts[key]
                print(f"--- {key} ---")
                # Dereference
                try:
                    print(repr(fobj))
                except Exception as e:  # pragma: no cover - defensive
                    print(f"<repr failed: {e}>")
                # Try to extract subtype, encoding, descendant
                try:
                    subtype = fobj.get("/Subtype", None)
                    encoding = fobj.get("/Encoding", None)
                    print(f"  Subtype: {subtype}")
                    print(f"  Encoding: {encoding}")
                    descf = fobj.get("/DescendantFonts", None)
                    if descf is not None:
                        print(f"  DescendantFonts: {descf}")
                        for df in descf:
                            print(f"    descendant Subtype: {df.get('/Subtype', None)}")
                            print(f"    descendant CIDSystemInfo: {df.get('/CIDSystemInfo', None)}")
                except Exception as e:
                    print(f"  <introspection failed: {e}>")
                print()

    # Dump the page content stream(s) to a single bytes object so we can
    # search for Tm/Tj/q/Q operators verbatim.
    contents = page.obj.get("/Contents", None)
    if contents is None:
        print("(page has no /Contents)")
        return 0
    streams = contents if isinstance(contents, pikepdf.Array) else [contents]
    raw = b""
    for s in streams:
        raw += s.read_bytes()

    out_bin = OUT_DIR / f"page{PAGE_INDEX + 1}_contentstream.bin"
    out_bin.write_bytes(raw)
    print(f"WROTE content stream ({len(raw)} bytes) -> {out_bin.name}")

    # Print last ~4KB (where rewritten BT/ET typically lands)
    print("=" * 70)
    print("LAST 4KB OF CONTENT STREAM")
    print("=" * 70)
    tail = raw[-4096:] if len(raw) > 4096 else raw
    # Print as hex+ASCII style with offset references
    line_width = 32
    for off in range(0, len(tail), line_width):
        chunk = tail[off : off + line_width]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        abs_off = (len(raw) - len(tail)) + off
        print(f"  {abs_off:08x}  {hex_part:<{line_width * 3}}  {ascii_part}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
