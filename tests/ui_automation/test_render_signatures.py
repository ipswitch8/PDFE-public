# -*- coding: utf-8 -*-
"""Render-verification: signature field appearance on the canvas.

Pipeline e7d3c2a1 Phase 3 — category suite for the gap report's #13 (signature
appearance rendered on canvas; previously byte/model-asserted only). Places a
signature form field via the deterministic form-tool drag seam and asserts its
appearance paints, and that undo removes it. Full cryptographic signing opens a
modal dialog and is covered by the byte/model signature suites; this layer pins
the *rendered* appearance of the signature widget.
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle as ro  # noqa: F401
from tests.ui_automation.harness import render_drive as d

SIG_RECT = (160.0, 300.0, 420.0, 360.0)  # fitz x0,y0,x1,y1 for the drag


@pytest.mark.timeout(120)
def test_signature_field_placement_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "formToolAction")
        before = d.capture(win)
        d.activate(win, "formSignatureAction")
        d.drag_pdf(win, SIG_RECT[0], SIG_RECT[1], SIG_RECT[2], SIG_RECT[3])
        after = d.capture(win)
        d.assert_changed(before, after, what="signature-field canvas")


@pytest.mark.timeout(120)
def test_signature_field_undo_reverts_canvas():
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "formToolAction")
        reference = d.capture(win)
        d.activate(win, "formSignatureAction")
        d.drag_pdf(win, SIG_RECT[0], SIG_RECT[1], SIG_RECT[2], SIG_RECT[3])
        placed = d.capture(win)
        d.assert_changed(reference, placed, what="signature field pre-undo")
        d.undo(win)
        reverted = d.capture(win)
        assert not ro.images_differ(reference, reverted, threshold=0.02), (
            "signature field did not visibly revert after undo")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
