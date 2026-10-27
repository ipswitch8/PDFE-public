# -*- coding: utf-8 -*-
"""
Coverage inventory for PDFE test fixtures.

Scans every PDF under tests/fixtures/ (root + realworld/ + image_pdf/) and emits
per-fixture attribute records used to build the phase-3 coverage matrix.

Run from tests/fixtures/ or repo root:
    python coverage_inventory.py [--out coverage_inventory.json]
"""
from __future__ import annotations

import argparse
import re
import json
import os
import sys
from pathlib import Path

try:
    import fitz  # PyMuPDF
except ImportError:  # pragma: no cover - exercised only where PyMuPDF is absent
    # Deferred rather than fatal so the pure-string helpers below
    # (scrub_repo_paths, repo_relative) can be imported and TESTED in
    # environments without a PDF engine -- notably the fast-checks CI lane,
    # which installs only pytest and pyyaml. main() still refuses to run
    # without it; see the guard there.
    fitz = None


THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent  # tests/fixtures/ -> tests/ -> repo root


# Heuristics ------------------------------------------------------------------
def categorize_size(size_bytes: int) -> str:
    if size_bytes < 1024:
        return "tiny"          # <1 KB
    if size_bytes < 5 * 1024 * 1024:
        return "small"         # <5 MB
    if size_bytes < 50 * 1024 * 1024:
        return "medium"        # <50 MB
    return "large"             # >=50 MB


def categorize_pages(n: int) -> str:
    if n <= 1:
        return "single"
    if n <= 9:
        return "few"
    if n <= 99:
        return "ten_to_99"
    return "hundred_plus"


def detect_text_kind(text: str) -> list[str]:
    """Best-effort detection of script categories present in extracted text."""
    if not text:
        return []
    kinds: list[str] = []
    has_ascii = any(0x20 <= ord(c) < 0x7F and c.isprintable() for c in text)
    has_latin1_extended = any(0x80 <= ord(c) <= 0x024F for c in text)
    has_cjk = any(
        (0x3040 <= ord(c) <= 0x30FF) or  # Hiragana/Katakana
        (0x4E00 <= ord(c) <= 0x9FFF) or  # CJK Unified
        (0xAC00 <= ord(c) <= 0xD7AF)     # Hangul
        for c in text
    )
    has_arabic = any(
        (0x0600 <= ord(c) <= 0x06FF) or       # Arabic
        (0xFB50 <= ord(c) <= 0xFDFF) or       # Arabic Presentation Forms-A
        (0xFE70 <= ord(c) <= 0xFEFF)          # Arabic Presentation Forms-B
        for c in text
    )
    has_hebrew = any(0x0590 <= ord(c) <= 0x05FF for c in text)
    has_cyrillic = any(0x0400 <= ord(c) <= 0x04FF for c in text)
    has_greek = any(0x0370 <= ord(c) <= 0x03FF for c in text)
    if has_ascii:
        kinds.append("ascii")
    if has_latin1_extended:
        kinds.append("latin-extended")
    if has_cjk:
        kinds.append("cjk")
    if has_arabic or has_hebrew:
        kinds.append("rtl")
    if has_arabic:
        kinds.append("arabic")
    if has_hebrew:
        kinds.append("hebrew")
    if has_cyrillic:
        kinds.append("cyrillic")
    if has_greek:
        kinds.append("greek")
    if not kinds and text.strip():
        kinds.append("non-ascii-other")
    return kinds


def first_pages_text(doc: fitz.Document, max_pages: int = 3, char_cap: int = 4000) -> str:
    out: list[str] = []
    for i in range(min(max_pages, doc.page_count)):
        try:
            t = doc.load_page(i).get_text("text") or ""
        except Exception:
            t = ""
        out.append(t)
        if sum(len(s) for s in out) >= char_cap:
            break
    return "".join(out)[:char_cap]


def has_images(doc: fitz.Document, max_pages: int = 5) -> bool:
    try:
        for i in range(min(max_pages, doc.page_count)):
            page = doc.load_page(i)
            if page.get_images(full=False):
                return True
    except Exception:
        return False
    return False


def has_annotations(doc: fitz.Document, max_pages: int = 10) -> bool:
    try:
        for i in range(min(max_pages, doc.page_count)):
            page = doc.load_page(i)
            for _ in page.annots() or []:
                return True
    except Exception:
        return False
    return False


def has_xref_object_streams(doc: fitz.Document) -> bool:
    """Detect whether the file uses /XRef streams (PDF 1.5+) by checking
    presence of /Type /XRef in any stream object."""
    try:
        for xref in range(1, doc.xref_length()):
            try:
                t = doc.xref_get_key(xref, "Type")
                if t and t[1] == "/XRef":
                    return True
            except Exception:
                continue
    except Exception:
        return False
    return False


def has_acroform(doc: fitz.Document) -> bool:
    try:
        return bool(doc.is_form_pdf)
    except Exception:
        return False


def has_outputintent(doc: fitz.Document) -> bool:
    """Cheap detection: scan trailer-rooted catalog for /OutputIntents."""
    try:
        cat_xref = doc.pdf_catalog()
        keys = doc.xref_get_keys(cat_xref) or ()
        return "OutputIntents" in keys
    except Exception:
        return False


def has_marked_content(doc: fitz.Document) -> bool:
    """Detect Tagged PDF (StructTreeRoot)."""
    try:
        cat_xref = doc.pdf_catalog()
        keys = doc.xref_get_keys(cat_xref) or ()
        return "StructTreeRoot" in keys
    except Exception:
        return False


def detect_pdfa_claim(doc: fitz.Document) -> str | None:
    """Inspect XMP for a pdfaid:part / pdfaid:conformance pair."""
    try:
        xmp = doc.get_xml_metadata() or ""
        if "pdfaid:part" in xmp:
            # cheap parse
            import re
            part = re.search(r"pdfaid:part>\s*([0-9])", xmp)
            conf = re.search(r"pdfaid:conformance>\s*([A-Za-z])", xmp)
            if part and conf:
                return f"PDF/A-{part.group(1)}{conf.group(1).lower()}"
            if part:
                return f"PDF/A-{part.group(1)}"
        if "pdfuaid:part" in xmp:
            return "PDF/UA-1"
    except Exception:
        return None
    return None


def is_linearized(path: Path) -> bool:
    """Inspect raw bytes for /Linearized in the first 1024 bytes after %PDF marker."""
    try:
        with open(path, "rb") as f:
            blob = f.read(4096)
        return b"/Linearized" in blob
    except Exception:
        return False


def header_pdf_version(path: Path) -> str | None:
    """Read the PDF header (%PDF-X.Y) directly. Some PyMuPDF builds return None
    from doc.pdf_version() so this is the reliable fallback."""
    try:
        with open(path, "rb") as f:
            head = f.read(16)
        # %PDF-1.4 ...
        if head.startswith(b"%PDF-"):
            ver = head[5:8].decode("ascii", errors="ignore")
            # validate "X.Y" shape
            if len(ver) == 3 and ver[1] == "." and ver[0].isdigit() and ver[2].isdigit():
                return ver
    except Exception:
        return None
    return None


def encryption_detail(doc: fitz.Document) -> dict:
    """Return {algorithm, keyLength, V, R} or {} for unencrypted."""
    out: dict = {}
    try:
        if not doc.is_encrypted and not doc.needs_pass:
            return out
        # MuPDF exposes V/R via metadata->permissions only loosely.
        # Easiest: scan trailer dict for /Encrypt -> /V /R /Length.
        trail = doc.pdf_trailer() or ""
        import re
        m_v = re.search(r"/V\s+(\d+)", trail)
        m_r = re.search(r"/R\s+(\d+)", trail)
        m_len = re.search(r"/Length\s+(\d+)", trail)
        if m_v:
            out["V"] = int(m_v.group(1))
        if m_r:
            out["R"] = int(m_r.group(1))
        if m_len:
            out["keyLength"] = int(m_len.group(1))
        # Map V to algo
        v = out.get("V")
        if v in (1, 2):
            out["algorithm"] = "RC4"
        elif v == 4:
            out["algorithm"] = "AES-128"
        elif v == 5:
            out["algorithm"] = "AES-256"
    except Exception:
        pass
    return out


# Inventory -------------------------------------------------------------------
def repo_relative(path: Path) -> str:
    """Path as written into the inventory: repo-relative, never absolute.

    This used to be `str(path)`, i.e. the absolute path of whichever machine
    ran the script, which embeds the build account name in a TRACKED file.
    This repository had its history rewritten across 1,290 commits to remove
    that string and is intended to become public, so the committed inventory
    was being hand-scrubbed to `C:/Users/dev/...` after every regeneration --
    a manual step that only has to be forgotten once.

    Nothing reads this field: `generate_existing_sidecars.py` and
    `test_extraction_sidecar_conformance.cpp` both key on `relPath`. The key
    is kept for schema stability and now carries a value that is meaningful
    on any machine.

    See tests/test_ui_suite_path_hygiene.cpp, which fails on any tracked file
    carrying the account name -- it is what caught this.
    """
    try:
        return str(path.relative_to(REPO_ROOT)).replace("\\", "/")
    except ValueError:
        # Outside the repo entirely: emit the bare filename rather than an
        # absolute path, so no machine-specific prefix can leak.
        return path.name


def scrub_repo_paths(text: str) -> str:
    """Replace any spelling of the repo root inside `text` with `<repo>`.

    A separator-agnostic match is required, not a literal replace. MuPDF's
    open-failure message quotes the filename with DOUBLED backslashes
    (`C:\\\\Users\\\\...`) once it has been through JSON escaping, so a
    replace of the plain or forward-slashed root silently misses it -- which
    is exactly how one account-name occurrence survived the first attempt at
    this fix and was caught by the path-hygiene test rather than by me.
    """
    parts = [re.escape(p) for p in str(REPO_ROOT).replace("\\", "/").split("/") if p]
    pattern = re.compile(r"[\\/]+".join(parts), re.IGNORECASE)
    return pattern.sub("<repo>", text)


def inspect_pdf(path: Path) -> dict:
    rec: dict = {
        "path": repo_relative(path),
        "relPath": str(path.relative_to(THIS_DIR)).replace("\\", "/"),
        "fileSize": path.stat().st_size,
    }
    rec["sizeCategory"] = categorize_size(rec["fileSize"])

    try:
        doc = fitz.open(str(path))
    except Exception as e:
        # Scrubbed for the same reason as the outer handler below: MuPDF's
        # open-failure text quotes the absolute filename it was handed.
        rec["error"] = scrub_repo_paths(f"open failed: {e}")
        return rec

    try:
        rec["pageCount"] = doc.page_count
        rec["pagesCategory"] = categorize_pages(doc.page_count)
        rec["isEncrypted"] = bool(doc.is_encrypted) or bool(doc.needs_pass)
        rec["needsPass"] = bool(doc.needs_pass)
        try:
            v = doc.pdf_version() if hasattr(doc, "pdf_version") else None
            if not v:
                v = header_pdf_version(path)
            rec["pdfVersion"] = v
        except Exception:
            rec["pdfVersion"] = header_pdf_version(path)
        rec["hasForm"] = has_acroform(doc)
        rec["hasAnnotations"] = has_annotations(doc)
        rec["hasImages"] = has_images(doc)
        rec["hasXrefStreams"] = has_xref_object_streams(doc)
        rec["isLinearized"] = is_linearized(path)
        rec["hasOutputIntent"] = has_outputintent(doc)
        rec["hasStructTree"] = has_marked_content(doc)
        rec["pdfaClaim"] = detect_pdfa_claim(doc)
        rec["encryption"] = encryption_detail(doc)

        # Text + scripts (skip if we can't decrypt)
        if not rec["needsPass"]:
            text = first_pages_text(doc)
            rec["scripts"] = detect_text_kind(text)
            rec["hasText"] = bool(text.strip())
        else:
            rec["scripts"] = []
            rec["hasText"] = None

        # Content kind: vector-text vs scanned-image-only vs mixed
        if rec.get("hasImages") and not rec.get("hasText"):
            rec["contentKind"] = "scanned-image-only"
        elif rec.get("hasImages") and rec.get("hasText"):
            rec["contentKind"] = "mixed"
        elif rec.get("hasText"):
            rec["contentKind"] = "vector-text"
        else:
            rec["contentKind"] = "unknown"
    finally:
        doc.close()

    return rec



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
        # immediate children + one level of subdirs (realworld/<cat>/<file>.pdf)
        for p in sub.rglob("*.pdf"):
            # Skip transient outputs and batch_output
            if "batch_output" in p.parts:
                continue
            if _is_transient_artifact(p.name):
                continue
            out.append(p)
    return sorted(set(out))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(THIS_DIR / "coverage_inventory.json"))
    args = ap.parse_args()

    if fitz is None:
        print("ERROR: PyMuPDF (fitz) is required to build the inventory.",
              file=sys.stderr)
        return 2

    pdfs = collect_pdfs()
    print(f"Found {len(pdfs)} PDF fixtures to inspect")

    records: list[dict] = []
    for p in pdfs:
        try:
            rec = inspect_pdf(p)
        except Exception as e:
            # Same reason as inspect_pdf: never an absolute path in a tracked
            # artifact. The exception text is scrubbed too -- a MuPDF or
            # Python error message routinely quotes the filename it was
            # handed, which is how an absolute path gets in through the back
            # door of an error string.
            rec = {
                "path": repo_relative(p),
                "error": scrub_repo_paths(f"{type(e).__name__}: {e}"),
            }
        records.append(rec)

    out_path = Path(args.out)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({"fixtures": records, "count": len(records)}, f, indent=2)

    print(f"Wrote {out_path}")
    # Quick stats
    enc = sum(1 for r in records if r.get("isEncrypted"))
    forms = sum(1 for r in records if r.get("hasForm"))
    annots = sum(1 for r in records if r.get("hasAnnotations"))
    images = sum(1 for r in records if r.get("hasImages"))
    pdfa = sum(1 for r in records if r.get("pdfaClaim"))
    print(f"Encrypted: {enc}, Forms: {forms}, Annotations: {annots}, Images: {images}, PDF/A claims: {pdfa}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
