# -*- coding: utf-8 -*-
"""Render-verification: view / zoom / navigate / rotate / theme.

Pipeline e7d3c2a1 Phase 3 — category suite covering the gap report's G1/G2/G3
(zoom, navigation, rotation) render-asserted paths plus the Tier-C theme repaint
(#20). Each test follows pre-capture → act (via the deterministic action seam,
GAP-A14-safe) → post-capture → assert the canvas pixels actually moved. These
exercise *working* features, so they are expected to PASS — they form the
load-bearing baseline the systemic-defect xfails are measured against.
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle  # noqa: F401  (req'd import)
from tests.ui_automation.harness import render_drive as d


@pytest.mark.timeout(120)
def test_zoom_in_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        before = d.capture(win)
        d.activate(win, "zoomInAction")
        after = d.capture(win)
        d.assert_changed(before, after, threshold=0.02, what="zoom-in canvas")


@pytest.mark.timeout(120)
def test_zoom_out_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "zoomInAction")
        before = d.capture(win)
        d.activate(win, "zoomOutAction")
        after = d.capture(win)
        d.assert_changed(before, after, threshold=0.02, what="zoom-out canvas")


@pytest.mark.timeout(120)
def test_actual_size_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "zoomInAction")
        d.activate(win, "zoomInAction")
        before = d.capture(win)
        d.activate(win, "actualSizeAction")
        after = d.capture(win)
        d.assert_changed(before, after, threshold=0.02,
                         what="actual-size canvas")


@pytest.mark.timeout(120)
def test_fit_page_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "zoomInAction")
        d.activate(win, "zoomInAction")
        before = d.capture(win)
        d.activate(win, "fitPageAction")
        after = d.capture(win)
        d.assert_changed(before, after, threshold=0.02, what="fit-page canvas")


@pytest.mark.timeout(120)
def test_next_page_changes_canvas():
    """Navigation on the high-contrast self-check fixture (78% page delta)."""
    with d.app_session(d.NAV_SELFCHECK) as win:
        page0 = d.capture(win)
        d.go_to_page(win, 1)
        page1 = d.capture(win)
        d.assert_changed(page0, page1, threshold=0.05, what="page-1 canvas")


@pytest.mark.timeout(120)
def test_rotate_page_cw_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        # capture_stable, not capture: a plain capture() can grab a MID-render
        # frame while the machine is busy (its own docstring says so), and the
        # golden below is captured by the same call -- which is how a golden
        # can end up encoding a half-painted frame.
        before = d.capture_stable(win)
        d.activate(win, "rotatePageCWAction")
        after = d.capture_stable(win)
        d.assert_changed(before, after, threshold=0.02, what="rotated canvas")
        # D8-2: crop to the page rect so the golden is geometry-stable across rigs.
        d.assert_matches_golden(d.crop_to_page_content(after),
                                "view_nav_rotate_cw_multispan.png")


@pytest.mark.timeout(180)
def test_theme_toggle_repaints_canvas_gap20():
    """Switching to the Dark theme repaints the PdfCanvas viewport.

    GitHub #217 / Tier-C gap #20. This was an ``xfail(strict=False)`` asserting
    the canvas does NOT repaint, on the strength of a diagnostic
    (``diag_gap20_theme_toggle.py``, 24 sequential launches) that found the
    settled before/after frames PIXEL-IDENTICAL on every clean capture.

    That evidence was real; the conclusion drawn from it was not. The probe
    drove ``themeToggleAction``, whose checked state was seeded from nothing
    and started UNCHECKED whatever the persisted theme was, so on any run that
    launched already in Dark the "toggle" set Dark again and changed nothing.
    Pixel-identical frames were the signature of an action that did not fire,
    not of a canvas that ignores the palette. Pipeline p87 phase-3 fixed the
    toggle's checked state (MainWindow.cpp), gave the dark palette the SHADE
    roles PdfCanvas paints its viewport with (ThemeManager.cpp), and stopped
    ``isHighContrastMode()`` classifying every ordinary palette as a
    High-Contrast theme (AccessibilityHelper.cpp) — which is what had been
    discarding the canvas's shade role before it reached a pixel.

    The theme is PERSISTED, so this drives the explicit Light and Dark actions
    rather than the toggle: starting from a state this test chose is the whole
    difference between measuring the product and measuring the last run.
    Captures stay stability-gated — a plain capture right after launch can grab
    a mid-render frame and manufacture a change that is pure capture noise."""
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "themeLightAction", settle_s=1.5)
        before = d.capture_stable(win)
        d.activate(win, "themeDarkAction", settle_s=1.5)
        after = d.capture_stable(win)
        try:
            d.assert_changed(before, after, threshold=0.02,
                             what="themed canvas")
        finally:
            # Leave the persisted preference where the next suite expects it.
            d.activate(win, "themeLightAction", settle_s=1.0)


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
