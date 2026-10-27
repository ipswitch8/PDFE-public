# -*- coding: utf-8 -*-
"""Render-verification: text editing — the shipped-bug class (Tier A).

Pipeline e7d3c2a1 Phase 3 — the highest-priority category from the gap report
(§6 Tier A). It pins, at the live-canvas pixel level:

* **#1 vector-text edit on an image-dominated page must stay VISIBLE** — editing
  the bold "Invoice" heading currently renders blank (Tr=3 forced via the
  page-wide ImageDominated misclassification).  XFAIL until Phase 5.
* **#1 undo / redo visibility** for that edit.  XFAIL until Phase 5.
* **scanned-page text edit** — routing + a visible result on a pure scan.
* **form-field text edit** — entered value visible on the widget appearance.
* **#4 mixed-content tool routing** — clicking the scanned block on a page that
  also has vector text must reach the region tool, not the per-span tool.
  XFAIL until Phase 6.

Drive is 100% via the deterministic PDFE_TEST_PIPES seams (text edit type /
double-click / tool activate / form-field set), never UIA menu walks (GAP-A14).
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle as ro
from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.conftest import record_matrix_result

# Embedded italic-serif subset fixture (pipeline b91f4e83) — the faithful
# reproduction of the SpecimenA defect span. Used by the phase-7 oracle-primitive
# augmentation cases below.
EMBEDDED_ITALIC_SERIF = d.FIXTURES / "embedded_italic_serif_subset.pdf"
EMBEDDED_HEADING_TEXT = "A Tale of Two Collards"
EMBEDDED_HEADING_CENTER = (231.6, 132.6)   # fitz-space centre of the span
EMBEDDED_ZOOM_STEPS = 3                     # fit (~84%) -> ~165% for a reliable
#                                            serif/sans correlation margin


# The heading-bbox crop also catches the company logo, which sits ABOVE the
# "Invoice" glyphs and is CONSTANT across the edit. Measuring the top half
# therefore reads the (unchanging) logo and is blind to the heading collapse —
# the bug that motivated this whole layer. The heading glyphs occupy the lower
# band of the crop, so we measure there. Verified on PrintWindow captures: the
# logo top-half reads 0.063 before==after (no signal) while the lower text band
# reads 0.187 -> 0.032 when the heading blanks (the real #1 signature).
_HEAD_TEXT_BAND = (0.45, 1.0)
_INK_KEEP = 0.6
# FirstName text field centre on test_form.pdf (fitz coords).
FORM_FIRSTNAME_CENTER = (280.0, 92.0)


def _ink_ratio(img, lum_threshold: int = 110,
               band: tuple = _HEAD_TEXT_BAND) -> float:
    """Fraction of dark 'ink' pixels in the vertical *band* (y0_frac, y1_frac)
    of *img*. Defaults to the heading text band (below the logo)."""
    g = img.convert("L")
    w, h = g.size
    if w == 0 or h == 0:
        return 0.0
    y0 = max(0, int(h * band[0]))
    y1 = max(y0 + 1, int(h * band[1]))
    g = g.crop((0, y0, w, y1))
    data = list(g.getdata())
    if not data:
        return 0.0
    return sum(1 for v in data if v < lum_threshold) / len(data)


# ---------------------------------------------------------------------------
# #1 — vector text edit visibility / undo / redo  (XFAIL until Phase 5)
# ---------------------------------------------------------------------------
@pytest.mark.timeout(150)
# Phase 5 (defects #1+#5) FIXED: per-span TextSpan::isOcrInvisibleLayer() routing
# in applyText() keeps the real heading on the visible-vector path. xfail removed.
def test_vector_text_edit_visible():
    with d.app_session(d.INVOICE) as win:
        d.activate(win, "textEditToolAction")
        before = d.crop_pdf_bbox(win, d.capture(win),
                                 d.INVOICE_HEADING_BBOX, pad=2)
        ink_before = _ink_ratio(before)
        d.double_click_pdf(win, *d.INVOICE_HEADING_CENTER)
        diag = d.selection_diag(win)
        d.type_text(win, "Quote")
        d.commit(win)
        after = d.crop_pdf_bbox(win, d.capture(win),
                                d.INVOICE_HEADING_BBOX, pad=2)
        ink_after = _ink_ratio(after)
        assert ink_after >= _INK_KEEP * ink_before, (
            f"heading collapsed after edit: ink {ink_before:.3f} -> "
            f"{ink_after:.3f}; diag={diag!r}")
        # D8-1 (phase-9): promote the RenderVerify matrix row on a clean pass.
        record_matrix_result(
            "txt_vector_heading_commit_visible", "green",
            f"phase-9 live UI proof: heading ink {ink_before:.3f} -> "
            f"{ink_after:.3f} kept after commit (subset stripped-glyph "
            f"substitution); diag={diag!r}")


@pytest.mark.timeout(150)
# Phase 5 (defect #1) FIXED: EditTextCommand::undo() re-emits the original heading
# via the visible-vector path; in-edit Ctrl-Z restores it. xfail removed.
def test_vector_text_edit_undo_reverts():
    with d.app_session(d.INVOICE) as win:
        d.activate(win, "textEditToolAction")
        before = d.crop_pdf_bbox(win, d.capture(win),
                                 d.INVOICE_HEADING_BBOX, pad=2)
        ink_before = _ink_ratio(before)
        d.double_click_pdf(win, *d.INVOICE_HEADING_CENTER)
        d.type_text(win, "Quote")
        d.undo(win)  # in-edit undo (no commit first) — the broken path
        after = d.crop_pdf_bbox(win, d.capture(win),
                                d.INVOICE_HEADING_BBOX, pad=2)
        ink_after = _ink_ratio(after)
        assert ink_after >= _INK_KEEP * ink_before, (
            f"heading not restored after undo: ink {ink_before:.3f} -> "
            f"{ink_after:.3f}")


@pytest.mark.timeout(150)
# Phase 5 (defect #1) FIXED: redo re-applies the edit through the visible-vector
# path (not Tr=3), so the heading stays visible after redo. xfail removed.
def test_vector_text_edit_redo_stays_visible():
    with d.app_session(d.INVOICE) as win:
        d.activate(win, "textEditToolAction")
        before = d.crop_pdf_bbox(win, d.capture(win),
                                 d.INVOICE_HEADING_BBOX, pad=2)
        ink_before = _ink_ratio(before)
        d.double_click_pdf(win, *d.INVOICE_HEADING_CENTER)
        d.type_text(win, "Quote")
        d.commit(win)
        d.undo(win)
        d.redo(win)
        after = d.crop_pdf_bbox(win, d.capture(win),
                                d.INVOICE_HEADING_BBOX, pad=2)
        ink_after = _ink_ratio(after)
        assert ink_after >= _INK_KEEP * ink_before, (
            f"heading invisible after redo: ink {ink_before:.3f} -> "
            f"{ink_after:.3f}")


# ---------------------------------------------------------------------------
# Scanned-page text edit
# ---------------------------------------------------------------------------
@pytest.mark.timeout(120)
def test_scanned_page_routes_to_region_tool():
    """A pure scanned page (no live text) must route to the TRE region tool —
    the correct per-page decision and the control for the #4 mixed-page xfail."""
    with d.app_session(d.SCANNED) as win:
        d.activate(win, "textEditToolAction")
        d.click_pdf(win, 300, 300)
        active = d.active_tool(win)
        assert active == "treToolAction", (
            f"scanned page activated {active!r}, not the region tool")


@pytest.mark.timeout(180)
# GitHub #218/#250. xfail REMOVED 2026-09-25 -- this must now PASS.
#
# The marker it replaces was xfail(strict=True) on a structural blocker:
# MainWindow gates the recomposite on spanInfo.isOcrInvisibleLayer(), which
# requires a font name containing 'glyphless' (TextEditModel.h:25-39), and
# test_scanned_with_ocr.pdf's only font resource was /Helvetica -- a real font
# merely drawn in Tr=3. Measured 2026-09-17, all 6 probed spans of 32 answered
# RECOMPOSITE_SKIPPED|span-not-ocr-invisible: the recomposite was never reached,
# under any coordinate, at any speed.
#
# The fixture's generator now emits a genuinely glyphless font, and this test
# reported XPASS(strict) on the next run -- the marker going red and demanding
# its own removal is exactly what strict=True was chosen for, and is why the
# earlier strict=False marker let a wrong cause survive for months.
def test_scanned_page_text_edit_changes_canvas():
    """Editing OCR'd text on a scanned page should re-render the region.

    GitHub #218. The body is a REAL assertion now, not a placeholder: it waits
    on the recomposite-complete seam and asserts the outcome status. It fails
    on a measured, named product/fixture gate rather than on a timeout, which
    is the whole difference between this marker and the one it replaces.
    """
    with d.app_session(d.SCANNED_OCR) as win:
        d.activate(win, "textEditToolAction")
        before = d.capture(win)
        mark = d.recomposite_mark()
        d.double_click_pdf(win, 150, 110)
        d.type_text(win, "EDIT")
        status, page, seq, detail = d.commit_and_wait_recomposite(win, mark)
        assert status == "RECOMPOSITE_DONE", (
            f"recomposite did not succeed: {status} page={page} seq={seq} "
            f"detail={detail!r}")
        after = d.capture(win)
        d.assert_changed(before, after, what="scanned edit canvas")


# ---------------------------------------------------------------------------
# Form-field text edit
# ---------------------------------------------------------------------------
@pytest.mark.timeout(120)
def test_form_field_text_edit_changes_canvas():
    """Typing into the FirstName text field must update its on-canvas
    appearance."""
    from tests.ui_automation.harness.pdfe_app_fixture import write_pipe
    with d.app_session(d.FORM) as win:
        d.activate(win, "formToolAction")
        before = d.capture(win)
        d.click_pdf(win, *FORM_FIRSTNAME_CENTER)
        write_pipe(win, "testFormFieldValueSetPipe", "Jane")
        import time
        time.sleep(1.0)
        after = d.capture(win)
        d.assert_changed(before, after, what="form-field canvas")


@pytest.mark.timeout(120)
def test_form_field_edit_undo_reverts_canvas():
    from tests.ui_automation.harness.pdfe_app_fixture import write_pipe
    with d.app_session(d.FORM) as win:
        d.activate(win, "formToolAction")
        reference = d.capture(win)
        d.click_pdf(win, *FORM_FIRSTNAME_CENTER)
        write_pipe(win, "testFormFieldValueSetPipe", "Jane")
        import time
        time.sleep(1.0)
        filled = d.capture(win)
        d.assert_changed(reference, filled, what="form-field pre-undo")
        d.undo(win)
        reverted = d.capture(win)
        assert not ro.images_differ(reference, reverted, threshold=0.02), (
            "form field did not visibly revert after undo")


# ---------------------------------------------------------------------------
# #4 — mixed-content tool routing  (Phase 6 FIXED)
# ---------------------------------------------------------------------------
@pytest.mark.timeout(120)
# Phase 6 (defect #4) FIXED: a left-click on the scanned image block with no live
# text span under it routes to the region tool (PdfCanvas::regionToolRequested ->
# MainWindow switches to treToolAction), instead of the page-wide
# hasTextContent() forcing the whole mixed page onto the per-span tool.
def test_mixed_content_scanned_block_routes_to_region_tool():
    """On a page with BOTH vector text and a scanned block, clicking the
    scanned block must reach the region tool — but page-wide hasTextContent()
    routes the whole page to the per-span tool."""
    with d.app_session(d.MIXED_CONTENT) as win:
        d.activate(win, "textEditToolAction")
        d.click_pdf(win, *d.MIXED_CONTENT_SCANNED_CENTER)
        active = d.active_tool(win)
        assert active == "treToolAction", (
            f"clicking the scanned block activated {active!r}, not the region "
            f"tool 'treToolAction'")


# ---------------------------------------------------------------------------
# Embedded italic-serif overlay — phase-7 oracle-primitive augmentation
# ---------------------------------------------------------------------------
# These two cases integrate the phase-6/7 font-identity oracle primitives
# (assert_overlay_font_class / assert_slant_matches) into the main text-edit
# render suite, on the embedded_italic_serif_subset.pdf fixture. They are the
# pixel-level recurrence guards for the SpecimenA overlay defects (1b: wrong
# family / dropped italic). The dedicated, fuller proof — overlay + new char +
# alignment — lives in test_render_textedit_embedded_font.py; the authoritative
# per-glyph face check is the gtest test_textedit_font_identity.cpp.
def _embedded_overlay_capture(win):
    """Zoom in, activate text-edit, open the heading overlay, return its crop."""
    for _ in range(EMBEDDED_ZOOM_STEPS):
        d.activate(win, "zoomInAction", settle_s=0.4)
    d.activate(win, "textEditToolAction")
    d.double_click_pdf(win, *EMBEDDED_HEADING_CENTER)
    overlay, _ = d.capture_overlay_over_underlay(win)
    return overlay


@pytest.mark.timeout(180)
def test_embedded_italic_serif_overlay_is_serif():
    """The in-place editor overlay on an embedded italic-serif span renders a
    SERIF face, not a sans substitute (defect 1b family) — assert_overlay_font_class."""
    if not EMBEDDED_ITALIC_SERIF.exists():
        pytest.skip(f"fixture missing: {EMBEDDED_ITALIC_SERIF.name}")
    with d.app_session(EMBEDDED_ITALIC_SERIF) as win:
        overlay = _embedded_overlay_capture(win)
        cs, cn = ro.assert_overlay_font_class(
            overlay, "serif", ref_text=EMBEDDED_HEADING_TEXT,
            what="embedded heading overlay")
        assert cs > cn  # explicit: serif correlation must win


@pytest.mark.timeout(180)
def test_embedded_italic_serif_overlay_is_italic():
    """The same overlay renders ITALIC (slanted), not upright (defect 1b slant)
    — directional assert_slant_matches(expected_italic=True)."""
    if not EMBEDDED_ITALIC_SERIF.exists():
        pytest.skip(f"fixture missing: {EMBEDDED_ITALIC_SERIF.name}")
    with d.app_session(EMBEDDED_ITALIC_SERIF) as win:
        overlay = _embedded_overlay_capture(win)
        angle = ro.assert_slant_matches(
            overlay, expected_italic=True, what="embedded heading overlay")
        assert abs(angle) >= ro.ITALIC_SLANT_THRESHOLD_DEG


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
