# -*- coding: utf-8 -*-
"""Render-oracle ZOOM-ROBUSTNESS proof — pipeline d4e7f921 Phase 4.

The three font/geometry pixel oracles in :mod:`render_oracle`
(``assert_slant_matches`` / ``assert_overlay_font_class`` /
``assert_overlay_alignment``) gained an optional ``scale_factor`` parameter so a
caller can compensate for display-scaling / zoom variation between rigs. This
suite proves, on the live PDFE canvas, that each oracle reaches the CORRECT
verdict at TWO distinct zoom levels — i.e. that the parameterisation did not
break the oracles and that they are genuinely zoom-robust.

What each oracle's robustness actually rests on (see render_oracle.RIG_DISPLAY_SCALE):

* ``assert_slant_matches``      — keys on a shear ANGLE (degrees), dimensionless
  and scale-invariant; ``scale_factor`` is a documented no-op. The proof is that
  the same italic overlay reads italic at both zooms.
* ``assert_overlay_font_class`` — size-normalises live + reference shapes before
  correlating, so zoom cancels; ``scale_factor`` is a documented no-op. The proof
  is that the same serif overlay reads serif at both zooms.
* ``assert_overlay_alignment`` — tolerance is in screen PIXELS, so it genuinely
  multiplies by ``scale_factor``; the same physical registration must pass at
  both zooms once the pixel budget is scaled by the live zoom.

Zoom levels — why 3 and 4 ``zoomInAction`` steps, not fit-zoom:
The b91f4e83 calibration established that the serif/sans template correlation
margin collapses below ~50px glyph height; at the default fit-zoom (~84%) the
heading glyphs are ~23px and the margin is too thin to classify reliably. Using
fit-zoom for the font-class oracle would manufacture a FALSE-NEGATIVE, which this
phase's acceptance criteria explicitly forbid ("no false-negative at either
zoom"). So both levels are above that floor: 3 steps (~165%, the level the
existing embedded-font proof is calibrated at) and 4 steps (one ``zoomInAction``
higher). The two levels are distinct, both reliable, and both reachable purely
via the app's ``zoomInAction`` seam.

Drive is 100% the deterministic ``PDFE_TEST_PIPES`` seams (zoom / tool activate /
double-click), never UIA menu walks (GAP-A14). Live-UI pytest must run via the
Bash tool (the PowerShell tool is a desktop-less RDP session).
"""
from __future__ import annotations

import sys
from dataclasses import dataclass

import pytest

from tests.ui_automation.harness import render_oracle as ro
from tests.ui_automation.harness import render_drive as d

# The faithful SpecimenA reproduction fixture: BaseFont SSTISF+PalatinoLTPro
# BoldItalic — embedded, subsetted, serif, bold, italic.
FIXTURE = d.FIXTURES / "embedded_italic_serif_subset.pdf"
HEADING_TEXT = "A Tale of Two Collards"
HEADING_CENTER = (231.6, 132.6)   # fitz-space centre of the heading span

# Two distinct, both-reliable zoom levels (see module docstring). Both are above
# the ~50px-glyph correlation-reliability floor from the b91f4e83 calibration.
ZOOM_STEPS_LOW = 3                # ~165% — the calibrated reference level
ZOOM_STEPS_HIGH = 4               # one zoomInAction step higher

# Same alignment budget the embedded-font proof uses; it is multiplied by the
# live scale_factor inside assert_overlay_alignment so it tracks the zoom.
MAX_ALIGN_OFFSET_PX = 16


@dataclass
class OverlayCapture:
    """In-memory capture of the open text-edit overlay at a given zoom level."""
    overlay: object
    underlay: object
    scale: float          # render_drive.get_app_zoom_factor (live / reference)
    steps: int
    zoom_raw: str
    diag: str


def _capture_overlay_at(steps: int) -> OverlayCapture:
    """Launch the fixture, zoom *steps* times, open the heading overlay, and
    return its captured overlay/underlay crops + the live scale_factor.

    The whole app session lives and dies inside this helper; the returned data is
    pure in-memory PIL images + floats, so the oracle tests run with no live
    pdfe.exe (and never contend with the autouse process killer)."""
    with d.app_session(FIXTURE) as win:
        for _ in range(steps):
            d.activate(win, "zoomInAction", settle_s=0.4)
        zoom_raw = d.zoom_value(win)
        scale = d.get_app_zoom_factor(win)
        d.activate(win, "textEditToolAction")
        pre_edit = d.capture(win)
        d.double_click_pdf(win, *HEADING_CENTER)
        diag = d.selection_diag(win)
        overlay, underlay = d.capture_overlay_over_underlay(win, pre_edit)
    return OverlayCapture(overlay=overlay, underlay=underlay, scale=scale,
                          steps=steps, zoom_raw=zoom_raw, diag=diag)


@pytest.fixture(scope="module",
                params=[ZOOM_STEPS_LOW, ZOOM_STEPS_HIGH],
                ids=["zoom_low_3steps", "zoom_high_4steps"])
def overlay_capture(request) -> OverlayCapture:
    """Module-scoped per-zoom-level capture — launches pdfe.exe exactly ONCE per
    zoom level (two launches total for the whole module), shared across the three
    oracle tests at that level."""
    if not FIXTURE.exists():
        pytest.skip(f"fixture missing: {FIXTURE.name}")
    cap = _capture_overlay_at(request.param)
    # Sanity: the live zoom readback must be a believable scale (the oracles are
    # only meaningful if zoom actually changed). A broken pipe returns 1.0/empty.
    if cap.scale <= 0.0:
        pytest.fail(f"get_app_zoom_factor returned {cap.scale} at "
                    f"{request.param} steps (zoom_raw={cap.zoom_raw!r}) — the "
                    f"zoom pipe seam is not reporting; cannot prove robustness")
    return cap


# ---------------------------------------------------------------------------
# Oracle 1 — slant (scale-invariant angle): italic reads italic at both zooms
# ---------------------------------------------------------------------------
@pytest.mark.timeout(200)
def test_slant_oracle_robust_across_zoom(overlay_capture):
    cap = overlay_capture
    angle = ro.assert_slant_matches(
        cap.overlay, expected_italic=True,
        what=f"overlay@{cap.steps}steps(zoom={cap.zoom_raw})",
        scale_factor=cap.scale)
    assert abs(angle) >= ro.ITALIC_SLANT_THRESHOLD_DEG, (
        f"slant {angle:.1f} deg below italic threshold at "
        f"{cap.steps} steps (scale={cap.scale:.3f})")


# ---------------------------------------------------------------------------
# Oracle 2 — font class (scale-invariant correlation): serif reads serif at both
# ---------------------------------------------------------------------------
@pytest.mark.timeout(200)
def test_font_class_oracle_robust_across_zoom(overlay_capture):
    cap = overlay_capture
    cs, cn = ro.assert_overlay_font_class(
        cap.overlay, "serif", ref_text=HEADING_TEXT,
        what=f"overlay@{cap.steps}steps(zoom={cap.zoom_raw})",
        scale_factor=cap.scale)
    assert cs > cn, (
        f"serif correlation did not win at {cap.steps} steps "
        f"(corr_serif={cs:.3f} <= corr_sans={cn:.3f}, scale={cap.scale:.3f})")


# ---------------------------------------------------------------------------
# Oracle 3 — alignment (scale-DEPENDENT pixels): aligned at both, budget scaled
# ---------------------------------------------------------------------------
@pytest.mark.timeout(200)
def test_alignment_oracle_robust_across_zoom(overlay_capture):
    cap = overlay_capture
    dx, dy = ro.assert_overlay_alignment(
        cap.overlay, cap.underlay, MAX_ALIGN_OFFSET_PX,
        what=f"overlay@{cap.steps}steps(zoom={cap.zoom_raw})",
        scale_factor=cap.scale)
    # The effective budget tracks the live zoom; both axes must be inside it.
    eff = MAX_ALIGN_OFFSET_PX * cap.scale
    assert dx <= eff and dy <= eff, (
        f"overlay misaligned at {cap.steps} steps: (dx={dx}, dy={dy}) > "
        f"{eff:.1f}px (base {MAX_ALIGN_OFFSET_PX} * scale {cap.scale:.3f})")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v", "-s"]))
