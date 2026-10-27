# -*- coding: utf-8 -*-
"""
Generate the extraction-fidelity sidecars for the i18n / RTL / CJK fixtures.

meta-audit follow-up F14 -- gaps PDF-70 (RTL logical order), PDF-74 (CID/CMap
correctness), PDF-78 (combining marks), PDF-79 (sidecars nothing read).

HOW THESE SIDECARS ARE VALIDATED -- read this before regenerating.

A sidecar that merely records whatever extraction currently returns enshrines
whatever bug extraction currently has. These sidecars are therefore anchored to
the **authored** source strings, which are literals in
``gen_i18n_render_corpus.py`` -- the script that CREATED the fixtures -- not to
extraction output:

  i18n_rtl.pdf        Hebrew  "שלום"    U+05E9 05DC 05D5 05DD
                      Arabic  "مرحبا"   U+0645 0631 062D 0628 0627
                      Latin   "LTR"
  i18n_cjk.pdf        Chinese "中文测试"  U+4E2D 6587 6D4B 8BD5
  i18n_combining.pdf  "x" U+0301 "N"   (combining acute -- no precomposed form)
                      "x" U+00B4 "N"   (spacing acute -- has its own advance)

This script asserts the equivalence in BOTH directions and refuses to write a
sidecar when they disagree:

  * ``expected_codepoints`` is what extraction returns, and
  * folding it through ``presentation_form_folding`` must reproduce
    ``authored_codepoints`` exactly, IN ORDER.

The folding table exists for Arabic only. Arabic shaping legitimately converts
nominal letters to Arabic Presentation Forms-B (U+FE70..U+FEFF) during
extraction -- the caveat the pre-existing ``phase3_rtl_arabic.expected.json``
documents in prose but enforces nowhere. Folding is the Unicode canonical
decomposition of each presentation form, so the mapping is not hand-authored
guesswork: it is read out of the Unicode character database at generation time.

So the sidecar pins BOTH facts:
  1. extraction returns exactly these codepoints (a stability oracle), and
  2. those codepoints ARE the authored text in LOGICAL order (a correctness
     oracle -- the one that would catch a reversed or mis-segmented RTL string).

Usage:  python gen_extraction_sidecars.py [--check]
        --check verifies the committed sidecars still match without writing.
"""
from __future__ import annotations

import argparse
import json
import sys
import unicodedata
from pathlib import Path

import fitz

THIS_DIR = Path(__file__).resolve().parent
OUT_DIR = THIS_DIR / "extraction"

# The authored source strings, copied from gen_i18n_render_corpus.py. Each entry
# is (fixture, [authored text per band, in the order the generator lays them out]).
AUTHORED: dict[str, list[str]] = {
    "i18n_rtl.pdf": ["שלום",              # Hebrew "shalom"
                     "مرحبا",        # Arabic "marhaba"
                     "LTR"],
    "i18n_cjk.pdf": ["中文测试"],             # "zhong wen ce shi"
    "i18n_combining.pdf": ["x́N", "x´N"],
}

DESCRIPTIONS = {
    "i18n_rtl.pdf":
        "RTL logical-order extraction (PDF-70). Hebrew is extracted as nominal "
        "letters; Arabic is extracted as Presentation Forms-B, which fold back "
        "to the authored nominal letters in the SAME order. A reversed or "
        "mis-segmented RTL run fails the order comparison.",
    "i18n_cjk.pdf":
        "Identity-H CID -> Unicode correctness (PDF-74). A CMap regression "
        "yields garbage or U+FFFD here while the file still opens and renders.",
    "i18n_combining.pdf":
        "Combining-mark stability (PDF-78). U+0301 has no precomposed form with "
        "'x', so NFC must leave the pair decomposed; the U+00B4 band is the "
        "spacing-accent control that must NOT be folded into the base.",
}


def fold_presentation_forms(text: str) -> str:
    """Map Arabic Presentation Forms-B back to their nominal letters.

    Uses each character's Unicode compatibility decomposition (``<initial>``,
    ``<medial>``, ``<final>``, ``<isolated>``), so the table is derived from the
    UCD rather than hand-written.
    """
    out = []
    for ch in text:
        decomp = unicodedata.decomposition(ch)
        if decomp and decomp.startswith("<"):
            tag, _, rest = decomp.partition("> ")
            if tag.lstrip("<") in ("initial", "medial", "final", "isolated"):
                parts = rest.split()
                if len(parts) == 1:
                    out.append(chr(int(parts[0], 16)))
                    continue
        out.append(ch)
    return "".join(out)


def folding_table(text: str) -> dict[str, str]:
    """The subset of the folding actually exercised by @p text, as hex->hex."""
    table: dict[str, str] = {}
    for ch in text:
        folded = fold_presentation_forms(ch)
        if folded != ch:
            table[f"U+{ord(ch):04X}"] = "".join(f"U+{ord(c):04X}" for c in folded)
    return table


def extract(pdf: Path) -> str:
    doc = fitz.open(str(pdf))
    try:
        return doc[0].get_text()
    finally:
        doc.close()


def codepoints(text: str) -> list[str]:
    return [f"U+{ord(c):04X}" for c in text]


def build(fixture: str) -> tuple[dict, list[str]]:
    """Return (sidecar dict, problems). Problems non-empty => do not write."""
    pdf = THIS_DIR / fixture
    problems: list[str] = []
    if not pdf.exists():
        return {}, [f"missing fixture {pdf}"]

    raw = extract(pdf)
    # Extraction returns one line per laid-out band; strip the trailing newline
    # and split so each band is compared to its own authored string.
    got_lines = [ln for ln in raw.splitlines() if ln.strip()]
    authored = AUTHORED[fixture]

    if len(got_lines) != len(authored):
        problems.append(
            f"{fixture}: extraction produced {len(got_lines)} non-blank lines, "
            f"authored has {len(authored)}: {got_lines!r}"
        )
        return {}, problems

    lines = []
    for got, want in zip(got_lines, authored):
        folded = fold_presentation_forms(got)
        # NFC both sides so a purely-normalisation-form difference is not
        # reported as a content difference. (x + U+0301 has no precomposed
        # form, so NFC leaves it alone -- that band still compares literally.)
        if unicodedata.normalize("NFC", folded) != unicodedata.normalize("NFC", want):
            problems.append(
                f"{fixture}: extracted {codepoints(got)} folds to "
                f"{codepoints(folded)}, which is NOT the authored "
                f"{codepoints(want)}. Refusing to write a sidecar that would "
                f"enshrine this."
            )
            continue
        if "�" in got:
            problems.append(f"{fixture}: extraction contains U+FFFD: {got!r}")
            continue
        lines.append({
            "authored": want,
            "authored_codepoints": codepoints(want),
            "expected_extracted": got,
            "expected_codepoints": codepoints(got),
            "presentation_form_folding": folding_table(got),
        })

    if problems:
        return {}, problems

    return {
        "schema": "pdfe.extraction-fidelity/1",
        "fixture": fixture,
        "page": 0,
        "gap_ids": {
            "i18n_rtl.pdf": ["PDF-70", "PDF-79"],
            "i18n_cjk.pdf": ["PDF-74", "PDF-79"],
            "i18n_combining.pdf": ["PDF-78", "PDF-79"],
        }[fixture],
        "description": DESCRIPTIONS[fixture],
        "validation": (
            "expected_codepoints is what extraction returns; folding it through "
            "presentation_form_folding reproduces authored_codepoints exactly "
            "and in order. Both directions are re-checked by "
            "gen_extraction_sidecars.py --check."
        ),
        "authored_source": "tests/fixtures/gen_i18n_render_corpus.py",
        "forbidden_codepoints": ["U+FFFD"],
        "lines": lines,
    }, []


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify committed sidecars without writing")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    failures = 0
    for fixture in AUTHORED:
        sidecar, problems = build(fixture)
        if problems:
            for p in problems:
                print("FAIL:", p, file=sys.stderr)
            failures += 1
            continue

        out = OUT_DIR / (Path(fixture).stem + ".expected.json")
        text = json.dumps(sidecar, indent=2, ensure_ascii=False) + "\n"
        if args.check:
            if not out.exists():
                print(f"FAIL: {out} missing", file=sys.stderr)
                failures += 1
            elif out.read_text(encoding="utf-8") != text:
                print(f"FAIL: {out} is stale", file=sys.stderr)
                failures += 1
            else:
                print(f"OK   {out.name}")
        else:
            out.write_text(text, encoding="utf-8")
            print(f"wrote {out.name}  ({len(sidecar['lines'])} lines)")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
