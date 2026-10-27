# -*- coding: utf-8 -*-
"""Render page 3 of both the original and the edited Lean Startup PDF
so we can SEE the symptom on disk and stop relying on speculation."""
from __future__ import annotations

import pathlib
import sys

import pymupdf

ORIG = pathlib.Path(r"C:\Users\dev\Desktop\The Lean Startup - Erick Ries.pdf")
EDITED = pathlib.Path(r"C:\Users\dev\Desktop\Edited - The Lean Startup - Erick Ries.pdf")
OUT = pathlib.Path(__file__).parent


def render(pdf_path: pathlib.Path, page_idx: int, out_name: str, dpi: int = 200):
    doc = pymupdf.open(pdf_path)
    page = doc[page_idx]
    pix = page.get_pixmap(dpi=dpi)
    out_path = OUT / out_name
    pix.save(str(out_path))
    print(f"  rendered {pdf_path.name} page {page_idx+1} ({page.rect}) -> {out_name} ({pix.width}x{pix.height})")
    doc.close()


def main() -> int:
    render(ORIG, 2, "render_orig_page3.png")
    render(EDITED, 2, "render_edited_page3.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
