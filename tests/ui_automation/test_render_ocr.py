# -*- coding: utf-8 -*-
"""Render-verification: OCR language routing on a multilingual page (#3).

Pipeline e7d3c2a1 Phase 3 — category suite for the gap report's #3
(multilingual OCR). Unlike the other suites this one asserts on the OCR *text
output* (via the testOcrTextQueryPipe → testOcrTextResultPipe seam,
MainWindow::currentPageOcrText) rather than a pixel oracle, because the defect
manifests as wrong *recognition* of the Arabic block, not a paint failure.

* **Latin block** — must OCR to Latin letters (working control → PASS).
* **#3 Arabic block** — must OCR to Arabic-script codepoints, not garbled Latin.
  XFAIL until Phase 6: the doc-wide OcrConfig::language is baked to English at
  open and applied to every region, so the Arabic block recognizes as Latin
  noise with zero Arabic-script characters.
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle  # noqa: F401
from tests.ui_automation.harness import render_drive as d


def _has_arabic(text: str) -> bool:
    """True if *text* contains any Arabic-script codepoint (U+0600..U+06FF or
    the U+FB50..U+FEFF presentation forms)."""
    return any(("؀" <= ch <= "ۿ") or ("ﭐ" <= ch <= "﻿")
               for ch in text)


def _has_latin_letters(text: str) -> bool:
    return any(("a" <= ch.lower() <= "z") for ch in text)


@pytest.mark.timeout(180)
def test_latin_block_ocr_recognizes_latin():
    """Control: OCR runs and recognizes the Latin block — proves the OCR seam
    works, so the Arabic xfail below is meaningful (not the pipe failing)."""
    with d.app_session(d.MULTILINGUAL) as win:
        text = d.ocr_text(win)
        assert text, "OCR text pipe returned nothing for the multilingual page"
        assert _has_latin_letters(text), (
            f"OCR found no Latin letters in the Latin block: {text!r}")


@pytest.mark.timeout(180)
# Phase 6 (defect #3) FIXED: currentPageOcrText OCRs with a PER-REGION
# script-aware language (OcrPipeline::detectPageLanguages runs OSD per tile and
# unions the scripts -> "eng+ara") instead of the doc-wide English baked at
# open, so the Arabic block now yields Arabic-script codepoints. xfail removed.
def test_arabic_block_ocr_contains_arabic_script():
    with d.app_session(d.MULTILINGUAL) as win:
        text = d.ocr_text(win)
        assert _has_arabic(text), (
            f"OCR produced no Arabic-script characters for the Arabic block "
            f"(doc-wide English language applied): {text!r}")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
