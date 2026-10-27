# -*- coding: utf-8 -*-
"""Diagnostic tool: inspect every font on a PDF page and report whether
the PDF's `/BaseFont` label agrees with the embedded font binary's
`name`-table family.

For each font on the page (or every page with --all) prints:

  * Resource name (the page's `/Resources/Font` dict key, e.g. /F1).
  * PDF `/BaseFont` (the dict-level label).
  * `/FontDescriptor /FontName` (often duplicates BaseFont but can
    differ for Type0 wrappers).
  * Font format (TrueType, CFF/Type1C, Type 0, Type 3, etc.) and
    whether a binary is embedded.
  * Binary `name`-table family (the inside-the-binary truth — read via
    PyMuPDF's `fitz.Font(fontbuffer=...).name`).
  * Mismatch flag if the dict label and the binary disagree.
  * The first few text runs that use this font (so you can correlate
    the visible glyphs to the named font).

This is the answer to the question "is the font detection in the
edit pipeline correct against the SpecimenA Green PDF". Run it on the
PDF in question; if the binary's name disagrees with the
`/BaseFont`, the PDF is mis-labeled and the pipeline (which trusts
`/BaseFont`) is identifying the font incorrectly. Use the output to
decide whether to switch the pipeline to prefer the binary's name.

Requirements:
    pip install pymupdf

Usage:
    python tests/diagnose_pdf_fonts.py path/to/file.pdf [--page N] [--all]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import fitz  # PyMuPDF
except ImportError:
    print("ERROR: this tool requires PyMuPDF. Install with:\n"
          "  pip install pymupdf", file=sys.stderr)
    sys.exit(2)


def _binary_name_from_buffer(font_bytes: bytes) -> str:
    """Return the family name embedded inside the font binary's `name`
    table. Falls back to an empty string if PyMuPDF can't parse the
    buffer (e.g. for Type 3 fonts which have no binary).
    """
    if not font_bytes:
        return ""
    try:
        f = fitz.Font(fontbuffer=font_bytes)
        return f.name or ""
    except Exception as ex:
        return f"<parse error: {ex!r}>"


def _font_details(doc: fitz.Document, font_entry: tuple) -> dict:
    """Resolve a single `page.get_fonts(full=True)` entry into the
    diagnostic record we print.
    """
    # PyMuPDF tuple shape: (xref, ext, type, basefont, refname, encoding[, refxref])
    xref = font_entry[0]
    ext = font_entry[1]
    ftype = font_entry[2]
    basefont = font_entry[3]
    refname = font_entry[4]
    encoding = font_entry[5] if len(font_entry) > 5 else ""

    info: dict = {
        "refname": refname,
        "basefont": basefont,
        "type": ftype,
        "ext": ext,
        "encoding": encoding,
        "xref": xref,
    }

    # Pull the embedded font binary if present (returns empty bytes for
    # Standard 14 / Type 3 / non-embedded fonts).
    try:
        result = doc.extract_font(xref)
        # Newer PyMuPDF returns (basefont, ext, type, buffer); older may
        # have a different arity. Defensive unpacking.
        if len(result) >= 4:
            font_bytes = result[3]
        else:
            font_bytes = b""
    except Exception:
        font_bytes = b""

    info["has_binary"] = bool(font_bytes)
    info["binary_name"] = _binary_name_from_buffer(font_bytes) if font_bytes else ""

    # FontDescriptor / FontName lookup (best-effort; PyMuPDF doesn't
    # expose this directly, so reach into the xref via xref_get_key).
    try:
        # Get the font dict directly so we can probe FontDescriptor.
        # `xref_get_key(xref, "FontDescriptor")` returns a tuple
        # (kind, value). For dict refs the value is e.g. "<<...>>" or
        # an indirect reference string like "12 0 R".
        # This is best-effort — if it fails, we just skip.
        kind, fd_ref = doc.xref_get_key(xref, "FontDescriptor")
        if kind == "xref":
            # fd_ref is "12 0 R" — split off the object number
            fd_xref = int(fd_ref.split()[0])
            kind2, fname = doc.xref_get_key(fd_xref, "FontName")
            if kind2 == "name":
                info["fontname_in_descriptor"] = fname
    except Exception:
        pass

    return info


def _strip_subset_prefix(name: str) -> str:
    """Strip the standard PDF subset prefix (six uppercase letters + '+')."""
    if len(name) >= 8 and name[6] == "+" and name[:6].isalpha() and name[:6].isupper():
        return name[7:]
    return name


def _format_font_record(info: dict, runs: list[dict]) -> str:
    """Pretty-print one font's diagnostic record."""
    lines = []
    lines.append(f"  Resource /{info['refname']}  (xref {info['xref']})")
    lines.append(f"    /BaseFont           : {info['basefont']!r}")
    if info.get("fontname_in_descriptor"):
        lines.append(f"    /FontDescriptor /FontName: "
                     f"{info['fontname_in_descriptor']!r}")
    lines.append(f"    Type / Format       : {info['type']} (.{info['ext']})")
    lines.append(f"    Encoding            : {info['encoding']!r}")
    lines.append(f"    Embedded binary?    : {'yes' if info['has_binary'] else 'NO'}")

    if info["has_binary"]:
        bn = info["binary_name"] or "<empty>"
        lines.append(f"    Binary `name` family: {bn!r}")

        # Compare dict's BaseFont (with subset prefix stripped) against
        # the binary's name-table family. They normally agree; a
        # mismatch is the smoking gun for "the PDF is mislabeled".
        bf_stripped = _strip_subset_prefix(info["basefont"])
        bn_norm = info["binary_name"].strip()
        if bn_norm and bf_stripped and bn_norm.lower() != bf_stripped.lower():
            lines.append(f"    *** MISMATCH ***")
            lines.append(f"        PDF says     : {bf_stripped!r}")
            lines.append(f"        Binary says  : {bn_norm!r}")
            lines.append(f"        The PDF is labeling this font as "
                         f"'{bf_stripped}' but the embedded binary is "
                         f"actually '{bn_norm}'.")

    if runs:
        lines.append(f"    Text runs using this font ({len(runs)} total):")
        for r in runs[:5]:
            sample = r["text"][:60].replace("\n", "⏎")
            lines.append(f"      {r['size']:.1f} pt: {sample!r}")
        if len(runs) > 5:
            lines.append(f"      … and {len(runs) - 5} more")

    return "\n".join(lines)


def _collect_text_runs_per_font(page: fitz.Page) -> dict:
    """Walk the page's text dict and group runs by font name (the same
    name MuPDF reports as `TextSpan.fontName` to our pipeline)."""
    by_font: dict = {}
    for block in page.get_text("dict")["blocks"]:
        for line in block.get("lines", []):
            for span in line.get("spans", []):
                fname = span.get("font", "")
                by_font.setdefault(fname, []).append({
                    "text": span.get("text", ""),
                    "size": span.get("size", 0.0),
                    "color": span.get("color", 0),
                })
    return by_font


def diagnose_page(doc: fitz.Document, page_idx: int) -> None:
    page = doc[page_idx]
    print(f"\n{'=' * 72}")
    print(f"PAGE {page_idx + 1} / {doc.page_count}")
    print(f"{'=' * 72}")
    print(f"Page size: {page.rect.width:.1f} x {page.rect.height:.1f} pt")

    fonts = page.get_fonts(full=True)
    if not fonts:
        print("\n(no fonts on this page)")
        return

    runs_by_font = _collect_text_runs_per_font(page)

    print(f"\n{len(fonts)} font(s) on this page:\n")
    mismatches = 0
    for entry in fonts:
        info = _font_details(doc, entry)
        # Match runs by stripped basefont (page.get_text reports the
        # basefont, possibly with subset prefix).
        runs = []
        bf_stripped = _strip_subset_prefix(info["basefont"])
        for fname, run_list in runs_by_font.items():
            if _strip_subset_prefix(fname) == bf_stripped:
                runs.extend(run_list)
        print(_format_font_record(info, runs))
        print()
        if info["has_binary"]:
            bn_norm = info["binary_name"].strip().lower()
            bf_norm = bf_stripped.lower()
            if bn_norm and bn_norm != bf_norm:
                mismatches += 1

    print(f"Page {page_idx + 1} summary: {len(fonts)} fonts, "
          f"{mismatches} dict<->binary mismatch(es).")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Diagnose font label vs embedded-binary name on a PDF."
    )
    ap.add_argument("pdf", type=Path, help="Path to the PDF file")
    ap.add_argument("--page", type=int, default=0,
                    help="Page index to diagnose (0-based; default 0)")
    ap.add_argument("--all", action="store_true",
                    help="Diagnose every page, not just --page")
    args = ap.parse_args()

    if not args.pdf.exists():
        print(f"ERROR: {args.pdf} does not exist", file=sys.stderr)
        return 1

    doc = fitz.open(str(args.pdf))
    try:
        if args.all:
            for i in range(doc.page_count):
                diagnose_page(doc, i)
        else:
            if args.page < 0 or args.page >= doc.page_count:
                print(f"ERROR: page {args.page} out of range "
                      f"(document has {doc.page_count} pages)",
                      file=sys.stderr)
                return 1
            diagnose_page(doc, args.page)
    finally:
        doc.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
