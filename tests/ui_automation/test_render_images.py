# -*- coding: utf-8 -*-
"""Render-verification: image operations + per-region DPI (#2).

Pipeline e7d3c2a1 Phase 3 — category suite for the gap report's image paths
(#15 image transform visible on canvas) and the systemic mixed-DPI defect:

* **convert-to-grayscale** must visibly re-render the page (working feature →
  PASS).
* **#2 mixed-DPI region not smeared** — replacing the 150-DPI region must not
  upsample it to the page-max 300 DPI.  XFAIL (strict=False) until Phase 6; the
  authoritative per-region-DPI assertion is the gtest
  tests/test_image_replacer_dpi.cpp.
"""
from __future__ import annotations

import pathlib
import sys
import tempfile

import pytest

from tests.ui_automation.harness import render_oracle as ro
from tests.ui_automation.harness import render_drive as d


def _sharpness(img) -> float:
    """Mean absolute edge energy (proxy for crispness). Pure Pillow."""
    g = img.convert("L")
    w, h = g.size
    if w < 2 or h < 2:
        return 0.0
    px = g.load()
    sx = max(1, w // 200)
    sy = max(1, h // 200)
    total = 0
    n = 0
    for y in range(0, h - 1, sy):
        for x in range(0, w - 1, sx):
            c = px[x, y]
            total += abs(c - px[x + 1, y]) + abs(c - px[x, y + 1])
            n += 1
    return (total / n) if n else 0.0


@pytest.mark.timeout(150)
def test_convert_to_grayscale_changes_canvas():
    # NAV_SELFCHECK has a full-page saturated colour field (blue page 0); the
    # MIXED_DPI fixture is rasterised black-on-white (already grayscale), so
    # converting it is a visual no-op — a fixture bug, not a product one.
    with d.app_session(d.NAV_SELFCHECK) as win:
        before = d.capture(win)
        # convertToGrayscaleAction raises a "Convert to Grayscale" QMessageBox
        # (Ok|Cancel); the recolor only runs on Ok, so accept the modal (Enter =
        # default Ok button) before capturing the result.
        d.activate(win, "convertToGrayscaleAction", settle_s=0.4)
        assert d.accept_dialog(win, "Convert to Grayscale", accept_key="enter"), (
            "Convert to Grayscale confirmation dialog never appeared")
        after = d.capture(win)
        d.assert_changed(before, after, threshold=0.02,
                         what="grayscale canvas")


@pytest.mark.timeout(180)
# xfail removed (GitHub #262 fixed). The three blockers this probe carried in
# turn are all resolved, and it is worth recording which were fixed and which
# merely stopped reproducing, because they are not the same thing:
#
#   (1) FIXED earlier — mixed_dpi_page.pdf had no font resources at all, so
#       there was no OCR span to edit and the recomposite never engaged. The
#       fixture now carries an invisible glyphless OCR layer; 'THUMBNAIL' sits
#       at (84.8, 479.6, 171.2, 498.8), inside THUMB_REGION_BBOX.
#   (2) NOT REPRODUCING, never fixed — the "0.0 baseline sharpness from the
#       capture/crop pipeline" recorded 2026-09-22 is not observable on this
#       rig: sharp_before measures 2.18 with the new fixture AND with the old
#       one, so it was never fixture-dependent and nothing addressed it. The
#       `sharp_before > 0.0` assertion below is what catches it if it returns,
#       and it stays for that reason.
#   (3) FIXED here, GitHub #262 — the recomposite reported a terminal
#       RECOMPOSITE_DONE while the region's pixels did not change at all
#       (changed=False, sharpness 2.18 -> 2.18). Cause: the write-back target
#       was chosen as the LARGEST image XObject on the page, so an edit over
#       the 150-DPI thumbnail was committed into the 300-DPI photo underneath
#       and painted over by the thumbnail. It was NOT defect #2 — #2 predicts a
#       changed-but-smeared region, and per-region DPI was already correct.
#       Full analysis: docs/audit/dca/0105-a-success-reported-without-checking-the-thing-it-claims.md
#
# Deterministic oracles for the fix, which do not need a live UI:
# tests/test_recomposite_writes_visible_pixels.cpp.
#
#   (4) FIXED, GitHub #266 — with (3) fixed the probe reached its real oracle
#       for the first time and failed on what the recomposite DRAWS: editing
#       'THUMBNAIL' to 'EDIT' produced 'EDI' plus a solid grey block. Two
#       independent causes. First, atlas glyphs were harvested from a page-wide
#       300-DPI render and composited into a 150-DPI region at native size, so
#       they arrived at exactly 2x. Second, and the reason a correctly-sized
#       glyph was still a block: GlyphAtlas stored a crop whose peak alpha was
#       127. isolateAlpha gives 255 where a pixel IS the text colour and 128
#       where it is equidistant, so peak 127 means NOTHING in that crop was more
#       text-like than background — it was a featureless grey bar, and being the
#       highest-fidelity source it outranked the fallbacks that render E, D and
#       I correctly. The harvest now refuses such a crop and lets the working
#       fallback supply the glyph.
def test_mixed_dpi_region_not_smeared():
    """Editing the 150-DPI region must not re-render it at the page-max 300 DPI.

    The ``sharp_before > 0.0`` guard is the assertion that exposed blocker (2)
    above: a vacuous comparison of two blank crops would have passed forever. It
    stays, because a silent zero is the failure mode worth keeping visible.
    """
    with d.app_session(d.MIXED_DPI) as win:
        d.activate(win, "textEditToolAction")
        before = d.crop_pdf_bbox(win, d.capture(win),
                                 d.THUMB_REGION_BBOX, pad=2)
        sharp_before = _sharpness(before)
        assert sharp_before > 0.0, (
            "baseline sharpness is 0 -- the crop captured nothing, so the "
            "comparison below would pass on two blank images")
        mark = d.recomposite_mark()
        d.double_click_pdf(win, *d.THUMB_SPAN_CENTER)
        # SAME LENGTH as the original "THUMBNAIL", deliberately -- see the
        # sharpness note below.
        d.type_text(win, "BOOKMARKS")
        status, page, seq, detail = d.commit_and_wait_recomposite(win, mark)
        assert status == "RECOMPOSITE_DONE", (
            f"recomposite did not succeed: {status} page={page} seq={seq} "
            f"detail={detail!r}")
        after = d.crop_pdf_bbox(win, d.capture(win),
                                d.THUMB_REGION_BBOX, pad=2)
        sharp_after = _sharpness(after)
        changed = ro.images_differ(before, after, threshold=0.01)
        # Printed, not asserted. GitHub #268 asks for the end-to-end number and
        # the comments below explain at length why it cannot be a bar: the ratio
        # moves ~0.2 on word length alone. Printing it means a future run
        # reports the measurement instead of someone having to re-derive it from
        # a failure message that only appears when something else breaks.
        print(f"[i268] end-to-end sharpness {sharp_before:.3f} -> "
              f"{sharp_after:.3f} (ratio "
              f"{(sharp_after / sharp_before if sharp_before else float('nan')):.3f}"
              f"), same-length 9-char replacement")
        assert changed, (
            f"the edit produced no visible change in the 150-DPI region "
            f"(sharpness {sharp_before:.2f} -> {sharp_after:.2f})")

        # THE PROPERTY THIS TEST IS NAMED FOR, MEASURED DIRECTLY.
        #
        # "No upsample to the page-max 300 DPI" is an exact, checkable fact
        # about what was WRITTEN, so it is now checked instead of inferred from
        # edge energy in a screenshot.
        #
        # WHY THE OLD `sharp_after >= 0.85 * sharp_before` ASSERTION IS GONE,
        # and why this is not a threshold quietly loosened to go green:
        #
        #   * It was never once evaluated against the region it names. Until
        #     this pipeline, crop_pdf_bbox mapped PDF coordinates through a
        #     Python re-derivation of the page's position and returned 196x196
        #     px of GREY MARGIN. The "2.18" both probes reported for months was
        #     the contrast step between margin and paper. A bar applied to the
        #     wrong pixels was never calibrated against the right ones.
        #   * It is confounded by TEXT LENGTH, which has nothing to do with
        #     crispness. Measured 2026-09-30 over a fixed crop: replacing the
        #     9-character "THUMBNAIL" with the 9-character "BOOKMARKS" gives a
        #     ratio of 0.809; replacing it with the 4-character "EDIT" gives
        #     0.605. Five characters' worth of edges simply left the crop. A
        #     metric that moves 0.2 on word length cannot adjudicate a 0.15
        #     blur budget.
        #   * The mechanism it was built to catch is provably absent here: the
        #     image under the region keeps its native 300x300 pixels in a 144pt
        #     box, which is what the assertion below states exactly.
        #
        # The residual softness IS real and is not dismissed: the FreeType
        # fallback renders glyphs at the SCAN DPI (300) and the compositor then
        # scales them to the composite DPI (150), so replacement text is drawn
        # at 2x and downsampled. Rendering at the target DPI needs per-DPI glyph
        # caching in EnrichedAtlas; recorded on GitHub #266 rather than bodged.
        saved = pathlib.Path(tempfile.gettempdir()) / "pdfe_mixed_dpi_probe.pdf"
        assert d.save_as(win, saved), (
            "testSaveAsPipe did not produce a file, so what the recomposite "
            "actually wrote cannot be inspected")
        measured = d.image_dpi_under_region(saved, d.THUMB_REGION_BBOX)
        assert measured is not None, (
            "no image is drawn under the thumbnail region in the saved "
            "document -- the recomposite removed it rather than replacing it")
        dpi, px_w, width_pt = measured
        assert 140.0 <= dpi <= 160.0, (
            f"the 150-DPI region was re-rendered at {dpi:.1f} DPI "
            f"({px_w}px across {width_pt:.1f}pt). Defect #2 is exactly this: "
            f"ImageReplacer taking the page-MAXIMUM DPI (300 here, from the "
            f"photo) and re-rendering a 150-DPI region at it, upsampling and "
            f"blurring. Observed sharpness {sharp_before:.2f} -> "
            f"{sharp_after:.2f}.")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
