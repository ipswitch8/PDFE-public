# -*- coding: utf-8 -*-
"""
Karen-style spot validation: read each .expected.json, open the
corresponding PDF, and confirm the structural claims are true.

Reports any mismatch with file path + field + expected vs actual.
Exits non-zero if any mismatch found.

Usage: python validate_sidecars.py [--limit N]
"""
from __future__ import annotations

import argparse
import re
import json
import sys
from pathlib import Path

import fitz


THIS_DIR = Path(__file__).resolve().parent



# WIN-87 phase-57 -- the two names this used to filter on NO LONGER EXIST, and
# the filter that named them was ALREADY UNREACHABLE.
#
# RedactionManager staged its bake and its QPDF output at "<doc>.redact_tmp" /
# "<doc>.redact_out"; both are gone, replaced by StagingFile handles at
# unpredictable names.  Two things follow, and the second is the one worth
# writing down:
#
#   1. The recogniser must move to the new shapes, or it recognises nothing.
#   2. It never recognised anything HERE in the first place.  The caller
#      iterates ``rglob("*.pdf")``, so a name ending in ".redact_tmp" could not
#      be yielded to it -- the check was dead on arrival.  It is kept and
#      corrected rather than deleted because the collectors also feed
#      directory-listing consumers, and a recogniser that is right is worth
#      more than one that is merely unreachable.  Mirrors
#      ``StagingFile::isStagingResidueName`` (src/core/AtomicFileReplace.h).
_TRANSIENT_SUFFIXES = (
    ".redact_out",   # legacy: removed by WIN-87 phase-57
    ".redact_tmp",   # legacy: removed by WIN-87 phase-57
    ".pdfe_tmp",     # StagingFile::createFullRewriteTempFor
)
_STAGING_RESIDUE_RE = re.compile(r"\.pdfe_part\.[0-9a-fA-F]{32}$")


def _is_transient_artifact(name: str) -> bool:
    """True for a PDFE staging/scratch artifact that is never a fixture."""
    if name.endswith(_TRANSIENT_SUFFIXES):
        return True
    return bool(_STAGING_RESIDUE_RE.search(name))


def collect_pdfs() -> list[Path]:
    out: list[Path] = []
    for sub in [THIS_DIR, THIS_DIR / "realworld", THIS_DIR / "image_pdf"]:
        if not sub.exists():
            continue
        for p in sub.rglob("*.pdf"):
            if "batch_output" in p.parts:
                continue
            if _is_transient_artifact(p.name):
                continue
            out.append(p)
    return sorted(set(out))


def page_count_actual(path: Path) -> int | None:
    try:
        doc = fitz.open(str(path))
        n = doc.page_count
        doc.close()
        return n
    except Exception:
        return None


def is_encrypted_actual(path: Path) -> bool | None:
    try:
        doc = fitz.open(str(path))
        v = bool(doc.is_encrypted) or bool(doc.needs_pass)
        doc.close()
        return v
    except Exception:
        return None


def has_form_actual(path: Path) -> bool | None:
    try:
        doc = fitz.open(str(path))
        v = bool(doc.is_form_pdf)
        doc.close()
        return v
    except Exception:
        return None


def has_annotations_actual(path: Path) -> bool | None:
    try:
        doc = fitz.open(str(path))
        if doc.needs_pass:
            doc.close()
            return None  # cannot inspect
        for i in range(min(10, doc.page_count)):
            page = doc.load_page(i)
            for _ in page.annots() or []:
                doc.close()
                return True
        doc.close()
        return False
    except Exception:
        return None


def has_images_actual(path: Path) -> bool | None:
    try:
        doc = fitz.open(str(path))
        if doc.needs_pass:
            doc.close()
            return None
        for i in range(min(5, doc.page_count)):
            page = doc.load_page(i)
            if page.get_images(full=False):
                doc.close()
                return True
        doc.close()
        return False
    except Exception:
        return None


def header_pdf_version(path: Path) -> str | None:
    try:
        with open(path, "rb") as f:
            head = f.read(16)
        if head.startswith(b"%PDF-"):
            v = head[5:8].decode("ascii", errors="ignore")
            if len(v) == 3 and v[1] == "." and v[0].isdigit() and v[2].isdigit():
                return v
    except Exception:
        return None
    return None


CHECKS = [
    ("pageCount", page_count_actual),
    ("isEncrypted", is_encrypted_actual),
    ("hasForm", has_form_actual),
    ("hasAnnotations", has_annotations_actual),
    ("hasImages", has_images_actual),
    ("pdfVersion", lambda p: header_pdf_version(p)),
]


def validate_one(pdf: Path) -> list[str]:
    """Return list of mismatch error strings (empty = passing)."""
    sidecar = pdf.with_suffix(".expected.json")
    if not sidecar.exists():
        return [f"{pdf.name}: sidecar missing"]
    try:
        with open(sidecar, "r", encoding="utf-8") as f:
            sc = json.load(f)
    except Exception as e:
        return [f"{pdf.name}: cannot parse sidecar ({e})"]

    errs: list[str] = []
    for field, getter in CHECKS:
        if field not in sc:
            continue
        expected = sc[field]
        actual = getter(pdf)
        if actual is None:
            continue  # cannot inspect (e.g. encrypted)
        if expected is None:
            continue
        if expected != actual:
            errs.append(
                f"{pdf.name}::{field} expected={expected!r} actual={actual!r}"
            )
    return errs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0,
                    help="Limit to N first fixtures (0 = all)")
    ap.add_argument("--phase3-only", action="store_true",
                    help="Only validate phase3_* fixtures")
    args = ap.parse_args()

    pdfs = collect_pdfs()
    if args.phase3_only:
        pdfs = [p for p in pdfs if p.name.startswith("phase3_")]
    if args.limit > 0:
        pdfs = pdfs[: args.limit]

    print(f"Validating {len(pdfs)} fixtures against their sidecars")
    total_errors: list[str] = []
    pass_count = 0
    for p in pdfs:
        errs = validate_one(p)
        if errs:
            total_errors.extend(errs)
        else:
            pass_count += 1

    print(f"\nPASS: {pass_count}/{len(pdfs)}")
    if total_errors:
        print("\nMISMATCHES:")
        for e in total_errors:
            print(f"  {e}")
        return 1
    print("All sidecars match their fixtures.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
