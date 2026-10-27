# -*- coding: utf-8 -*-
"""Verify each generated fixture: open it, extract the image XObject,
compute implied DPI from pixel dims vs MediaBox, and print."""
from __future__ import annotations
import sys
from pathlib import Path
import fitz

HERE = Path(__file__).resolve().parent

FIXTURES = sorted(HERE.glob("*.pdf"))

def inspect(path: Path) -> int:
    doc = fitz.open(str(path))
    failures = 0
    for pno in range(doc.page_count):
        page = doc[pno]
        mb = page.rect  # points
        page_w_in = mb.width / 72.0
        page_h_in = mb.height / 72.0
        imgs = page.get_images(full=True)
        if not imgs:
            print(f"  FAIL {path.name} p{pno}: no image XObject")
            failures += 1
            continue
        # Pick the largest one
        biggest = max(imgs, key=lambda t: t[2] * t[3])
        xref = biggest[0]
        px_w = biggest[2]
        px_h = biggest[3]
        dpi_w = px_w / page_w_in
        dpi_h = px_h / page_h_in
        name = "." if pno == 0 else f"p{pno}"
        print(
            f"  {path.name:46s} {name} "
            f"page={mb.width:.1f}x{mb.height:.1f}pt "
            f"img={px_w}x{px_h}px "
            f"dpi={dpi_w:.0f}x{dpi_h:.0f}"
        )
    doc.close()
    return failures


def main() -> int:
    total_fail = 0
    for f in FIXTURES:
        total_fail += inspect(f)
    if total_fail:
        print(f"\n{total_fail} failures")
        return 1
    print(f"\nAll {len(FIXTURES)} fixtures valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
