# -*- coding: utf-8 -*-
"""Render-verification: text-edit FONT IDENTITY on an embedded italic-serif
subset (pipeline b91f4e83, phase 6).

This is the live-canvas, pixel-level proof of the three SpecimenA text-edit
defects fixed in phase 5, on the faithful fixture
``embedded_italic_serif_subset.pdf`` (BaseFont ``SSTISF+PalatinoLTPro
BoldItalic`` — embedded, subsetted, serif, bold, italic; ``z`` is genuinely
absent from the subset):

* **defect 1b — overlay drops italic.** Double-clicking the serif-italic
  heading must open an overlay whose glyphs are SERIF (not a sans substitute)
  and ITALIC (slanted, not upright).
* **defect 2 — overlay/underlay geometry mismatch.** The overlay glyph band
  must sit in register with the page text underneath it.
* **defect 3 — new char renders sans.** Typing ``z`` (absent from the subset,
  so routed through AutoMissingGlyphResolver to a system fallback) must render
  SERIF and ITALIC, matching the surrounding run — italic is CORRECT here (the
  underlying text is italic), only the wrong FAMILY was the bug.

Drive is 100% the deterministic ``PDFE_TEST_PIPES`` seams (tool activate /
double-click / type / zoom), never UIA menu walks (GAP-A14). Pixels are read via
the phase-6 oracle primitives in :mod:`render_oracle`
(``assert_overlay_font_class`` / ``assert_slant_matches`` /
``assert_overlay_alignment``), each calibrated against live serif AND live sans
captures. The AUTHORITATIVE per-glyph face-identity oracle is the gtest
``test_textedit_font_identity.cpp`` (FreeType on the resolved font file); this
suite proves the result reaches the screen.

The heading is edited at ~165% zoom (three ``zoomInAction`` steps) so the glyphs
are large enough (~50px) for a reliable serif/sans template correlation; at the
default fit-zoom the glyphs are ~23px and the margin is too thin.
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle as ro
from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.conftest import record_matrix_result

FIXTURE = d.FIXTURES / "embedded_italic_serif_subset.pdf"
PAGE = (594.0, 360.0)
HEADING_TEXT = "A Tale of Two Collards"
HEADING_CENTER = (231.6, 132.6)   # fitz-space centre of the heading span
ZOOM_STEPS = 3                    # fit (~84%) -> ~165%

# Generous offset budget: the alignment check catches a GROSS overlay/underlay
# mismatch (defect 2), not sub-pixel jitter; the in-place editor adds a few px
# of internal padding around the glyphs vs the page render.
MAX_ALIGN_OFFSET_PX = 16


def _open_zoomed_editing():
    """Context-managed session: open the fixture, zoom in, activate text-edit.
    Returns the ``app_session`` context manager (use with ``with``)."""
    return d.app_session(FIXTURE)


def _zoom_and_activate(win):
    for _ in range(ZOOM_STEPS):
        d.activate(win, "zoomInAction", settle_s=0.4)
    d.activate(win, "textEditToolAction")


@pytest.mark.timeout(180)
def test_overlay_italic_serif_is_serif_italic_and_aligned():
    """Overlay on the embedded italic-serif heading is serif + italic + aligned.

    Covers phase-6 AC (a) serif font class, (b) italic slant, (c) overlay /
    underlay alignment.
    """
    if not FIXTURE.exists():
        pytest.skip(f"fixture missing: {FIXTURE.name}")
    with _open_zoomed_editing() as win:
        _zoom_and_activate(win)
        # Underlay = the page render BEFORE the edit (serif-italic heading).
        pre_edit = d.capture(win)
        # Open the in-place editor on the heading span.
        d.double_click_pdf(win, *HEADING_CENTER)
        diag = d.selection_diag(win)
        overlay, underlay = d.capture_overlay_over_underlay(win, pre_edit)

        # (a) serif, not a sans substitute (defect 1b family).
        cs, cn = ro.assert_overlay_font_class(
            overlay, "serif", ref_text=HEADING_TEXT, what="heading overlay")
        # (b) italic slant preserved (defect 1b slant).
        angle = ro.assert_slant_matches(
            overlay, expected_italic=True, what="heading overlay")
        # (c) overlay sits in register with the underlying page text (defect 2).
        dx, dy = ro.assert_overlay_alignment(
            overlay, underlay, MAX_ALIGN_OFFSET_PX, what="heading overlay")

        record_matrix_result(
            "txt_embedded_italic_serif_overlay", "green",
            f"phase-6 live UI: overlay serif (corr_serif={cs:.3f} > "
            f"corr_sans={cn:.3f}), italic (slant={angle:.1f} deg), aligned "
            f"(dx={dx},dy={dy} <= {MAX_ALIGN_OFFSET_PX}px); diag={diag!r}")


@pytest.mark.timeout(180)
def test_new_char_absent_from_subset_renders_serif_italic():
    """Typing ``z`` (absent from the embedded subset) renders serif + italic.

    Covers phase-6 AC (d): the new char is SERIF (family lock — defect 3) AND
    ITALIC (the correctly-preserved slant). Asserted on the whole post-edit line
    (strong template-correlation margin) and on the isolated ``z`` glyph
    (directional sign check; the gtest is the authoritative per-glyph oracle).
    """
    if not FIXTURE.exists():
        pytest.skip(f"fixture missing: {FIXTURE.name}")
    with _open_zoomed_editing() as win:
        _zoom_and_activate(win)
        # Double-click selects the word "Two"; typing replaces it with "z" so
        # the line becomes "A Tale of z Collards" with z as the 4th word.
        d.double_click_pdf(win, *HEADING_CENTER)
        d.type_text(win, "z")
        diag = d.selection_diag(win)
        new_text = "A Tale of z Collards"
        overlay, _ = d.capture_overlay_over_underlay(win)

        # Line stays serif + italic after inserting the fallback glyph.
        cs, cn = ro.assert_overlay_font_class(
            overlay, "serif", ref_text=new_text, what="post-z line")
        angle = ro.assert_slant_matches(
            overlay, expected_italic=True, what="post-z line")

        # Isolate the new 'z' (4th word of the 5-word line) and prove IT
        # specifically is serif. The inter-word gap a renderer produces is
        # rig-sensitive, so the default word_segments() gap threshold can MERGE
        # words on some rigs (observed: 3 segments instead of 5, segs[3] out of
        # range — CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #4). Sweep
        # the gap threshold DOWNWARD until the line separates into exactly its 5
        # words, then take the 4th. Stopping at the first threshold that yields
        # 5 avoids over-splitting a word into letters (which a too-small gap
        # would do). desl is recomputed per call but is identical (deterministic
        # de-slant of the same overlay).
        expected_words = len(new_text.split())  # "A Tale of z Collards" -> 5
        segs, desl = [], None
        for gap_frac in (0.28, 0.24, 0.20, 0.17, 0.14, 0.11, 0.09):
            segs, desl = ro.word_segments(overlay, gap_frac=gap_frac)
            if desl is not None and len(segs) == expected_words:
                break
        assert desl is not None and len(segs) == expected_words, (
            f"could not separate {new_text!r} into {expected_words} words by "
            f"sweeping the word_segments gap threshold; best got {len(segs)}: "
            f"{segs}")
        zx0, zx1 = segs[3]  # the new 'z' is the 4th word
        gs, gn = ro.assert_glyph_class(
            desl, zx0, zx1, ref_glyph="z", expected_class="serif",
            what="new char 'z'")

        record_matrix_result(
            "txt_embedded_new_char_serif_italic", "green",
            f"phase-6 live UI: post-z line serif (corr_serif={cs:.3f} > "
            f"corr_sans={cn:.3f}) italic (slant={angle:.1f} deg); isolated 'z' "
            f"serif (corr_serif={gs:.3f} > corr_sans={gn:.3f}); diag={diag!r}")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v", "-s"]))
