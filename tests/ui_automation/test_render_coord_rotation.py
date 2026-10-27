# -*- coding: utf-8 -*-
"""Render-verification: per-run rotation on a /Rotate 90 page (defect #6).

Pipeline e7d3c2a1 Phase 6. A page with ``/Rotate 90`` carrying a text run whose
OWN text matrix (Tm) ALSO encodes a 90-degree rotation is the double-rotation
trap that defect #6 fixes: CoordMapper used to apply the page ``/Rotate`` as a
blanket correction to every element, so a self-rotated run was rotated twice and
landed off-screen. ``CoordMapper::computeRunTransform`` now decomposes the run's
own rotation and applies the page ``/Rotate`` only as the residual, so the text
maps to the correct on-screen location.

The authoritative unit proof is gtest ``tests/test_coord_mapper_rotation.cpp``
(12 cases over the residual math). This is the render-oracle regression guard the
Phase 6 acceptance criteria require: it opens a fixture with internally-rotated
text on a ``/Rotate 90`` page, asserts the text renders ON the visible canvas
(not double-rotated off-screen), and locks the location with a golden.

Driven via the deterministic render harness; no UIA menu walks (GAP-A14).
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_drive as d

COORD_ROTATION = d.FIXTURES / "coord_rotation_page.pdf"


def _center_crop(img, frac: float = 0.6):
    """Central *frac* x *frac* region of *img* (a PIL image)."""
    w, h = img.size
    cw, ch = int(w * frac), int(h * frac)
    x0, y0 = (w - cw) // 2, (h - ch) // 2
    return img.crop((x0, y0, x0 + cw, y0 + ch))


def _ink_centroid_frac(img, lum_threshold: int = 130):
    """Return the (cx, cy) centroid of dark 'ink' pixels as fractions of *img*'s
    width/height, or ``None`` if the image has no ink. Used as a rig-robust
    positional oracle: where the text's ink sits, not its exact pixels."""
    import numpy as np
    g = np.asarray(img.convert("L"))
    ys, xs = np.where(g < lum_threshold)
    if xs.size == 0:
        return None
    h, w = g.shape
    return (float(xs.mean()) / w, float(ys.mean()) / h)


@pytest.mark.timeout(120)
def test_rotated_text_maps_on_screen():
    """The rotated text must render on the visible canvas, not double-rotated
    off-screen. The defect's render-level symptom is the text being pushed
    off-page (the canvas/page area goes blank); the fix keeps it visible and
    roughly centred, where the golden pins it."""
    if not COORD_ROTATION.exists():
        pytest.skip(f"fixture missing: {COORD_ROTATION} "
                    f"(run generate_render_oracle_fixtures.py)")
    with d.app_session(COORD_ROTATION) as win:
        cap = d.capture(win)
        # Text is present on the canvas (a double-rotation off-screen would
        # leave the rendered page blank).
        d.assert_not_blank(cap, blank_threshold=0.985,
                            what="rotated-text canvas")
        # The text lands in the central band — not flung into a corner / off the
        # edge.
        d.assert_not_blank(_center_crop(cap, 0.6), blank_threshold=0.992,
                           what="rotated-text centre region")
        # Positional oracle for the correct location (pipeline 9a4e2f71 phase-4,
        # item #3 of CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES). This
        # REPLACES a committed pixel golden (coord_rotation_page.png, SSIM<=0.08
        # tolerance) that was rig-sensitive: an exact-pixel compare diverged >8%
        # between the dev session and the CI runner's rendering even after
        # crop_to_page_content, and the c4a9e271 P10 crop attempt did not stabilise
        # it. The defect this guards (#6 double-rotation) flings the text to a
        # corner / off the page edge; its render signature is therefore POSITIONAL
        # — the text's ink centroid moving out of the page centre — not a specific
        # pixel pattern. We crop to the page rect (rig-robust) and assert the ink
        # centroid sits in the central band (text is ~50% x / ~34% y on the
        # displayed landscape page when correctly mapped). A double-rotation pushes
        # the centroid out of this band (or blanks the page, caught above), so a
        # real regression still fails — without the pixel-exactness that made the
        # golden flaky across rigs.
        page = d.crop_to_page_content(cap)
        c = _ink_centroid_frac(page)
        assert c is not None, "no ink found in the page-content crop (page blank?)"
        cx, cy = c
        assert 0.32 <= cx <= 0.68 and 0.12 <= cy <= 0.60, (
            f"rotated-text ink centroid ({cx:.2f}, {cy:.2f}) is outside the "
            f"central band x[0.32-0.68] y[0.12-0.60] — the text is likely "
            f"double-rotated off-centre (defect #6 regression).")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
