#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate ``tests/fixtures/test_taggable_rich.pdf`` — a synthetic PDF
with multi-size text, bullet lists, and numbered lists that AutoTagger's
heading-detection heuristics fire on.

AutoTagger (src/compliance/AutoTagger.cpp) classifies a text block as a
heading when its average font size exceeds 1.2x the page average, with
size ratios mapping to H1 (>1.8x), H2 (>1.4x), or H3 (>1.2x or bold).
This fixture lays out:

  Page 1:
    - 24pt Title  (Helvetica-Bold)        → H1
    - 18pt Section heading (Helv-Bold)    → H2
    - 11pt body paragraph                 → P
    - 18pt second section heading         → H2
    - 11pt body paragraph                 → P
    - Bullet list with "•" prefixes       → P-as-list-items (heuristic)

  Page 2:
    - 18pt section heading                → H2
    - Numbered list "1.", "2.", "3."      → P-as-list-items
    - 11pt closing paragraph              → P

Page-average font size will sit around 11.5-12pt (most chars are body),
so any 18+pt run hits >1.4x → H2, and the 24pt Title hits >2x → H1.

Result: AutoTagger::tagPage returns at least 4 non-P top-level elements
across the two pages, so ComplianceDialog::previewTags produces a
populated tagStructureTree — closing P13-INFRA-5.

Deterministic: no RNG; identical bytes every run modulo fitz's
millisecond /CreationDate stamp.  The fixture is checked into
tests/fixtures/ so generation only runs once.
"""
from __future__ import annotations

from pathlib import Path
import sys

try:
    import fitz  # PyMuPDF
except ImportError:
    print("ERROR: pymupdf not installed.  pip install pymupdf", file=sys.stderr)
    sys.exit(1)


FIXTURE_DIR = Path(__file__).parent
OUTPUT = FIXTURE_DIR / "test_taggable_rich.pdf"


# US Letter
PAGE_W = 612.0
PAGE_H = 792.0
MARGIN_X = 72.0
MARGIN_Y = 72.0


def _insert_paragraph(page: fitz.Page, y: float, text: str,
                      *, size: float = 11.0,
                      font: str = "helv", bold: bool = False,
                      indent: float = 0.0) -> float:
    """Place a single line at ``y`` (top-left origin in PDFE coords).

    PyMuPDF's insert_text uses a baseline at ``y``; we treat ``y`` as
    the baseline directly.  Returns the next-baseline y advanced past
    this line by ~1.4× the font size (single-line block).
    """
    fontname = font
    if bold and font == "helv":
        fontname = "hebo"  # Helvetica-Bold
    page.insert_text(
        fitz.Point(MARGIN_X + indent, y),
        text,
        fontname=fontname,
        fontsize=size,
    )
    return y + size * 1.4


def _insert_bullet_line(page: fitz.Page, y: float, text: str,
                        *, bullet: str = "•") -> float:
    return _insert_paragraph(
        page, y, f"{bullet}  {text}", size=11.0, indent=18.0)


def build_page_1(page: fitz.Page) -> None:
    y = MARGIN_Y + 24.0  # baseline for 24pt title
    # 24pt H1 title
    y = _insert_paragraph(
        page, y, "Quarterly Report: Compliance Audit", size=24.0,
        bold=True)
    y += 12.0

    # 18pt H2 section heading
    y = _insert_paragraph(
        page, y, "Executive Summary", size=18.0, bold=True)
    y += 6.0

    # 11pt body paragraph (3 lines of body text)
    body_1 = ("This document summarises the quarterly compliance audit "
              "for the period ending March 31.")
    body_2 = ("Findings are organised by severity, with critical "
              "issues escalated to the audit committee.")
    body_3 = ("All recommendations are tracked through the remediation "
              "portal and reviewed weekly.")
    y = _insert_paragraph(page, y, body_1, size=11.0)
    y = _insert_paragraph(page, y, body_2, size=11.0)
    y = _insert_paragraph(page, y, body_3, size=11.0)
    y += 12.0

    # 18pt H2 second section
    y = _insert_paragraph(
        page, y, "Key Findings", size=18.0, bold=True)
    y += 6.0

    # 11pt body paragraph
    intro = "The audit surfaced three categories of findings:"
    y = _insert_paragraph(page, y, intro, size=11.0)
    y += 4.0

    # Bullet list at 11pt
    y = _insert_bullet_line(page, y, "Access-control gaps in the "
                                     "shared accounting drive")
    y = _insert_bullet_line(page, y, "Outdated encryption on archived "
                                     "customer correspondence")
    y = _insert_bullet_line(page, y, "Missing audit logs for "
                                     "third-party API integrations")


def build_page_2(page: fitz.Page) -> None:
    y = MARGIN_Y + 18.0  # baseline for 18pt heading

    # 18pt H2 section
    y = _insert_paragraph(
        page, y, "Recommended Remediation Steps", size=18.0,
        bold=True)
    y += 6.0

    # 11pt intro
    intro = "We propose the following actions, in priority order:"
    y = _insert_paragraph(page, y, intro, size=11.0)
    y += 4.0

    # Numbered list at 11pt
    y = _insert_paragraph(
        page, y, "1.  Rotate access keys on the shared drive "
                 "within five business days.",
        size=11.0, indent=18.0)
    y = _insert_paragraph(
        page, y, "2.  Re-encrypt archived correspondence with AES-256 "
                 "and rotate KEKs.",
        size=11.0, indent=18.0)
    y = _insert_paragraph(
        page, y, "3.  Enable verbose audit logging on all third-party "
                 "API connectors.",
        size=11.0, indent=18.0)
    y += 12.0

    # 18pt H2 closing section
    y = _insert_paragraph(
        page, y, "Conclusion", size=18.0, bold=True)
    y += 6.0

    # 11pt closing paragraph
    closing_1 = ("The findings are tractable within the current quarter "
                 "given adequate engineering capacity.")
    closing_2 = ("A follow-up audit is scheduled for the end of the "
                 "next quarter to verify remediation.")
    y = _insert_paragraph(page, y, closing_1, size=11.0)
    y = _insert_paragraph(page, y, closing_2, size=11.0)


def main() -> int:
    doc = fitz.open()  # new empty document
    p1 = doc.new_page(width=PAGE_W, height=PAGE_H)
    build_page_1(p1)
    p2 = doc.new_page(width=PAGE_W, height=PAGE_H)
    build_page_2(p2)

    # Deterministic metadata (no timestamps in author/title that vary).
    doc.set_metadata({
        "title": "Quarterly Report: Compliance Audit",
        "author": "PDFE Test Suite",
        "subject": "AutoTagger fixture (multi-size + lists)",
        "keywords": "test, autotagger, heading, list",
        "creator": "tests/fixtures/generate_taggable_rich_pdf.py",
        "producer": "PyMuPDF",
    })

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    doc.save(str(OUTPUT), deflate=True, garbage=3)
    doc.close()
    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
