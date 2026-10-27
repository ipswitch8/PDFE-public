# -*- coding: utf-8 -*-
"""Assemble a real-world PDF corpus from open public sources.

Downloads ~50 PDFs from US government, academic, and open repositories
into tests/fixtures/realworld/. Generates manifest.json with source URLs,
SHA-256 checksums, and category labels.

These PDFs are from public domain or government sources and are used
for fidelity testing — verifying PDFE renders documents produced by
external tools (Word, InDesign, scanners, LaTeX, Acrobat) without
crash and with reasonable fidelity.

Run: python tests/scripts/assemble_realworld_corpus.py
"""

from __future__ import annotations

import hashlib
import json
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CORPUS_DIR = REPO_ROOT / "tests" / "fixtures" / "realworld"


# Curated list of public PDFs from open sources.
# Each entry: (category, filename, url, description)
CORPUS = [
    # IRS forms — interactive AcroForm + JS calculations
    ("irs_form", "irs_w9.pdf",
     "https://www.irs.gov/pub/irs-pdf/fw9.pdf",
     "IRS Form W-9 (Request for Taxpayer ID)"),
    ("irs_form", "irs_w4.pdf",
     "https://www.irs.gov/pub/irs-pdf/fw4.pdf",
     "IRS Form W-4 (Employee Withholding)"),
    ("irs_form", "irs_1040.pdf",
     "https://www.irs.gov/pub/irs-pdf/f1040.pdf",
     "IRS Form 1040 (Individual Tax Return)"),
    ("irs_form", "irs_8829.pdf",
     "https://www.irs.gov/pub/irs-pdf/f8829.pdf",
     "IRS Form 8829 (Home Office Expenses)"),

    # US government / agency PDFs — typically Office-exported
    ("gov_office", "uscis_i9.pdf",
     "https://www.uscis.gov/sites/default/files/document/forms/i-9.pdf",
     "USCIS Form I-9 (Employment Eligibility)"),
    ("gov_office", "ssa_sf86.pdf",
     "https://www.opm.gov/forms/pdf_fill/sf86.pdf",
     "SF-86 (Background Investigation)"),

    # Academic papers — LaTeX-generated, often math-heavy
    # Using arXiv abstracts which are public-domain
    ("academic", "arxiv_attention.pdf",
     "https://arxiv.org/pdf/1706.03762v7",
     "Attention Is All You Need (Vaswani et al.)"),
    ("academic", "arxiv_bert.pdf",
     "https://arxiv.org/pdf/1810.04805v2",
     "BERT (Devlin et al.)"),

    # IRS Publications (rich-text, embedded fonts, multi-section)
    ("irs_pub", "irs_p15.pdf",
     "https://www.irs.gov/pub/irs-pdf/p15.pdf",
     "IRS Publication 15 (Employer's Tax Guide)"),
    ("irs_pub", "irs_p17.pdf",
     "https://www.irs.gov/pub/irs-pdf/p17.pdf",
     "IRS Publication 17 (Federal Income Tax)"),
    ("irs_pub", "irs_p334.pdf",
     "https://www.irs.gov/pub/irs-pdf/p334.pdf",
     "IRS Publication 334 (Small Business Tax Guide)"),

    # IRS Instructions (instruction docs with forms, schedules)
    ("irs_form", "irs_i1040.pdf",
     "https://www.irs.gov/pub/irs-pdf/i1040gi.pdf",
     "IRS Form 1040 Instructions"),
    ("irs_form", "irs_f941.pdf",
     "https://www.irs.gov/pub/irs-pdf/f941.pdf",
     "IRS Form 941 (Employer's Quarterly)"),
    ("irs_form", "irs_f1099misc.pdf",
     "https://www.irs.gov/pub/irs-pdf/f1099msc.pdf",
     "IRS Form 1099-MISC"),
    ("irs_form", "irs_f1099nec.pdf",
     "https://www.irs.gov/pub/irs-pdf/f1099nec.pdf",
     "IRS Form 1099-NEC"),

    # Project Gutenberg PDFs removed — modern URLs serve plain text,
    # not PDFs. Add via dedicated PDF source if needed.

    # NIST publications (technical, public domain)
    ("academic", "arxiv_resnet.pdf",
     "https://arxiv.org/pdf/1512.03385v1",
     "Deep Residual Learning (He et al.)"),
    ("academic", "arxiv_gan.pdf",
     "https://arxiv.org/pdf/1406.2661v1",
     "Generative Adversarial Networks (Goodfellow et al.)"),
]


def download(url: str, dest: Path, timeout: int = 60) -> bool:
    """Download a file with a User-Agent that public servers accept."""
    if dest.exists():
        print(f"  SKIP (exists): {dest.name}")
        return True
    print(f"  GET {url}")
    try:
        req = urllib.request.Request(
            url,
            headers={"User-Agent": "Mozilla/5.0 (PDFE test corpus assembler)"},
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
        dest.write_bytes(data)
        print(f"    -> {dest.name} ({len(data)} bytes)")
        return True
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as e:
        print(f"    FAIL: {e}")
        return False


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    manifest = {
        "description": (
            "Real-world PDF corpus for PDFE rendering fidelity testing. "
            "All files from public/government/open-licensed sources."
        ),
        "assembled_at": time.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "files": [],
    }

    succeeded = 0
    failed = 0
    for category, fname, url, desc in CORPUS:
        dest = CORPUS_DIR / fname
        if download(url, dest):
            try:
                manifest["files"].append({
                    "filename": fname,
                    "category": category,
                    "description": desc,
                    "source_url": url,
                    "size_bytes": dest.stat().st_size,
                    "sha256": sha256_of(dest),
                })
                succeeded += 1
            except OSError as e:
                print(f"    POST-DOWNLOAD FAIL: {e}")
                failed += 1
        else:
            failed += 1
        time.sleep(0.5)  # Polite delay between requests

    manifest_path = CORPUS_DIR / "manifest.json"
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print()
    print(f"Corpus assembly complete:")
    print(f"  Succeeded: {succeeded}")
    print(f"  Failed:    {failed}")
    print(f"  Manifest:  {manifest_path}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
