# -*- coding: utf-8 -*-
"""GitHub #279 -- the page crop must contain the PAGE, not the toolbar.

THE MEASUREMENT THAT PRODUCED THIS TEST
=======================================
Ten ``phase5_*_canvas`` oracles fail at SSIM 0.9349-0.9471 with
``pixel_fraction`` 0.0081-0.0126. Splitting the three-panel failure composites
(``save_diff_visualization`` pastes actual / baseline / diff at x = 0 / w / 2w,
y = 24, losslessly when the sizes match -- and ``_baseline_check`` crop-matches
the pair before calling it) and dumping per-row statistics showed, for all ten:

  * every differing pixel lies in rows 0..19 of the page crop;
  * rows 0..19 are a flat ``(212, 208, 200)`` -- Qt's window/button colour;
  * rows 20..21 are a ``(106, 104, 100)`` rule;
  * rows 22.. are the page, and they are byte-identical between the two sides
    (rows 100, 400 and 844 of one oracle have the same mean to three decimals
    and the same colour histogram).

A vertical shift was ruled out arithmetically, not by eye: correlating the two
row-mean series over a fixed span scored k=0 at 0.066 mean absolute error
against 0.841 for the next-best offset, so nothing moved.

Magnifying rows 0..21 of both panels showed what they are. The actual reads
"Fit Width | Fit Page | Select | Sticky Note | Highlight | Underline"; the
baseline reads "Strike Out | Text Box | Rectangle | Circle | Line | Ink |
Image Tool". It is the TOOLBAR, at two different overflow positions.

So the renderer is not at fault and neither is the palette: the region the
oracle asserts begins roughly 22 px above the page, inside the toolbar, and the
toolbar's overflow state is the only thing that differs between the recorded
baseline and now. Every one of those ten failures is the oracle measuring
chrome -- the same class as the crop that measured the grey margin, and the
reason the repo's rule is to ask where the page is rather than guess.

WHAT THIS TEST PINS
===================
The geometric property directly, at the seam, with no pixels and no baseline:
the page rectangle reported for the canvas must lie inside the canvas, and the
pixels at its top must not be toolbar. Expressed as two independent assertions
so a failure says WHICH it is:

  1. ``page_rect_on_canvas`` is inside the canvas rect  -- a pure geometry
     check that needs no rendering.
  2. The top rows of the page crop are not a flat chrome-coloured band above a
     rule, which is the toolbar's signature and cannot occur inside a rendered
     page.

Both are properties of the INPUT geometry rather than agreement between two
captures, deliberately: comparing one capture against another is what let this
hide behind a "session drift" story, because two wrong crops agree with each
other perfectly well.

Run:  python -m pytest tests/visual/test_page_crop_excludes_the_toolbar.py -q
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

pytest.importorskip("PIL")
from PIL import Image  # noqa: E402

from tests.helpers.canvas_geometry import page_rect_on_canvas  # noqa: E402

#: Qt's default window/button colour, and the rule drawn under the toolbar.
#: Measured from the failing composites rather than taken from a palette
#: constant, because the point is to recognise what IS on screen.
CHROME_RGB = (212, 208, 200)
RULE_RGB = (106, 104, 100)


@pytest.fixture(scope="module")
def pdfe_canvas_session():
    """A live session with a document open, plus the canvas rect.

    ``PDFE_TEST_DETERMINISTIC_LAYOUT`` is set for the same reason
    ``test_ui_file_operations`` sets it: MainWindow's constructor calls
    ``restoreState(appSettings_->windowState())`` and otherwise adopts whatever
    dock widths this profile last had, which is per-machine state.

    Skipped rather than failed when the binary or an interactive desktop is
    absent -- this file asserts a geometry property of a running UI, and a
    headless runner has nothing to say about it. The skip names the missing
    precondition so it cannot be read as a pass.
    """
    # tests.helpers.pdfe_session, NOT tests.ui_harness.core.launch: two classes
    # share the name and only this one exposes `hwnd`, which capture_hwnd needs.
    # It is also the one test_ui_file_operations imports, so the session this
    # fixture builds is the same object those ten oracles measure.
    try:
        from tests.helpers.pdfe_session import PDFESession
    except Exception as exc:                                  # pragma: no cover
        pytest.skip(f"UI harness unavailable: {exc}")
    if not os.environ.get("SESSIONNAME"):
        pytest.skip("no interactive desktop session")

    from tests.helpers.canvas_geometry import canvas_region_client_px
    from tests.test_ui_file_operations import FIX_SINGLE

    key = "PDFE_TEST_DETERMINISTIC_LAYOUT"
    previous = os.environ.get(key)
    os.environ[key] = "1"
    try:
        with PDFESession(pdf_path=str(FIX_SINGLE)) as s:
            region = canvas_region_client_px(s, settle=1.5)
            if region is None:
                pytest.fail(
                    "testCanvasRect seam did not answer, so the canvas "
                    "rectangle is unknown. Refusing to guess it. Is pdfe.exe "
                    "built with PDFE_TESTING=ON and launched with "
                    "PDFE_TEST_PIPES=1?")
            yield s, region
    finally:
        if previous is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = previous


def _chrome_band_height(img: "Image.Image", max_rows: int = 64,
                        dominance: float = 0.40) -> int:
    """Rows from the top in which ``CHROME_RGB`` covers >= ``dominance``.

    DOMINANCE, NOT EQUALITY. The first version of this required every pixel in
    the row to equal ``CHROME_RGB``, and it passed on a capture that visibly
    contains the toolbar: a toolbar row is only ~70% background, the rest being
    icons and label text, so the check returned 0 and the test returned early
    without asserting anything. That is a vacuous oracle -- it reported the
    absence of its own sensitivity as the absence of the defect.

    0.40 is deliberately below the ~0.70 measured on the real captures, so the
    detector still fires on a denser toolbar row, while staying far above what
    a rendered page can reach: a page would have to be 40% covered in exactly
    ``(212, 208, 200)`` to trip it.
    """
    w, h = img.size
    px = img.load()
    n = 0
    for y in range(min(max_rows, h)):
        hits = sum(1 for x in range(w) if px[x, y] == CHROME_RGB)
        if hits >= dominance * w:
            n += 1
        else:
            break
    return n


def _rule_row_fraction(img: "Image.Image", y: int) -> float:
    """Fraction of row ``y`` that is the toolbar separator colour."""
    w, _h = img.size
    px = img.load()
    return sum(1 for x in range(w) if px[x, y] == RULE_RGB) / float(w)


def toolbar_band_rows(img: "Image.Image") -> int:
    """Rows of toolbar at the top of ``img``; 0 if the top is page.

    The signature is a chrome-dominated run terminated by a near-full-width
    separator rule. Both halves are required: a page may be chrome-coloured by
    coincidence, and a rule may appear inside page content, but a page cannot
    begin with the pair.
    """
    band = _chrome_band_height(img)
    if band == 0 or band >= img.size[1]:
        return 0
    if _rule_row_fraction(img, band) < 0.90:
        return 0
    return band


def test_the_reported_page_rect_lies_inside_the_canvas(pdfe_canvas_session):
    """Geometry only -- no pixels, so it cannot be confused by rendering.

    If the page rect escapes the canvas rect, the crop is reading pixels the
    canvas does not own, which is how toolbar rows ended up inside a region
    named "the page".
    """
    session, canvas_rect = pdfe_canvas_session
    rect = page_rect_on_canvas(session, 0)
    assert rect is not None, (
        "page_rect_on_canvas() did not answer. Failing rather than guessing a "
        "crop -- a guessed region is the defect GitHub #136 removed.")
    x, y, w, h = rect
    cw, ch = canvas_rect[2], canvas_rect[3]
    assert x >= 0 and y >= 0 and x + w <= cw and y + h <= ch, (
        f"the page rect {rect} is not inside the canvas {(cw, ch)}, so the "
        f"page crop reads pixels outside the canvas widget (GitHub #279)")


def test_the_page_crop_does_not_begin_with_toolbar_chrome(pdfe_canvas_session):
    """The top of the page crop must be page, not a chrome band above a rule.

    A rendered PDF page can legitimately begin with flat white, or flat any
    colour. What it cannot do is begin with a run of Qt's window colour
    terminated by the toolbar's separator rule -- that pair is chrome, and it
    is exactly what all ten failing oracles contain.
    """
    session, _canvas_rect = pdfe_canvas_session
    from tests.test_ui_file_operations import _capture_canvas  # local: heavy

    rect = page_rect_on_canvas(session, 0)
    assert rect is not None, "page_rect_on_canvas() did not answer"
    x, y, w, h = rect
    canvas = _capture_canvas(session)
    crop = canvas.crop((x, y, x + w, y + h)).convert("RGB")

    band = toolbar_band_rows(crop)
    assert band == 0, (
        f"the page crop begins with {band} row(s) of toolbar: a "
        f"{CHROME_RGB}-dominated run terminated by a full-width {RULE_RGB} "
        f"rule. That is chrome, not the page. The oracle therefore fails "
        f"whenever the toolbar's overflow state differs from the day the "
        f"baseline was recorded, which is exactly what the ten failing "
        f"phase5_* oracles are measuring (GitHub #279).")


# ---------------------------------------------------------------------------
# Detector sensitivity -- both directions, against a REAL contaminated capture
# ---------------------------------------------------------------------------
#
# The live test above can only ever report "clean" on a healthy rig, so on its
# own it is indistinguishable from a detector that cannot fire at all. These
# two pin the detector against the actual pixels that produced the failure, and
# against a page that must not trip it.

CONTAMINATED = (REPO_ROOT / "tests" / "fixtures" / "visual_regressions"
                / "toolbar_contaminated_page_crop.png")


def test_the_detector_fires_on_the_real_contaminated_capture():
    """The top 40 rows of a genuinely failing oracle, committed as evidence.

    Extracted from the left (actual) panel of
    ``phase5_open_test_single_canvas_diff.png``. A synthetic band would only
    prove the detector matches my idea of a toolbar; this proves it matches the
    one the product actually drew.
    """
    assert CONTAMINATED.exists(), (
        f"the contaminated-capture fixture is missing at {CONTAMINATED}. "
        "Without it this detector has no positive case and the live test "
        "above is unfalsifiable.")
    img = Image.open(str(CONTAMINATED)).convert("RGB")
    rows = toolbar_band_rows(img)
    assert rows > 0, (
        "the detector did not fire on a capture that visibly contains the "
        "toolbar ('Fit Width | Fit Page | Select | Sticky Note | ...'), so it "
        "cannot be trusted to fire in the live test either")
    assert 15 <= rows <= 25, (
        f"detected a {rows}-row band; the measured toolbar remnant is 20 rows "
        f"(rows 0..19, with the separator rule at 20). A wildly different "
        f"answer means the detector is keying on something else.")


def test_the_two_capture_paths_agree_on_where_the_client_area_starts(
        pdfe_canvas_session):
    """PrintWindow and the BitBlt fallback must return the SAME pixels.

    ``capture_hwnd`` has two implementations. The primary uses PrintWindow into
    a client-sized bitmap; the fallback computes ``ClientToScreen(hwnd, (rx,
    ry))`` and grabs the screen there. The fallback was always client-correct.
    The primary was not, because it omitted ``PW_CLIENTONLY`` -- so the two
    disagreed by the non-client offset (measured dx=8 dy=31 here) and which one
    you got depended on whether PrintWindow happened to succeed.

    That is the property worth pinning: not "the primary is right" in the
    abstract, but that the two agree. A future flag change that re-breaks the
    alignment reddens this even if every baseline has since been regenerated
    around it, because the fallback is an INDEPENDENT answer rather than
    another output of the same path.

    Compared on the top-left corner only, and loosely. The fallback grabs real
    screen pixels, so anything overlapping the window, a cursor, or subpixel AA
    can differ; the non-client offset being tested is 31 px, which no amount of
    that noise resembles.
    """
    session, _region = pdfe_canvas_session
    from tests.visual.capture import capture_hwnd, capture_screen_region
    import win32gui

    hwnd = session.hwnd
    probe = (0, 0, 240, 64)
    primary = capture_hwnd(hwnd, region=probe).convert("RGB")

    cx, cy = win32gui.ClientToScreen(hwnd, (0, 0))
    fallback = capture_screen_region(cx, cy, 240, 64).convert("RGB")

    assert primary.size == fallback.size
    pp, pf = primary.load(), fallback.load()
    close = sum(1 for y in range(64) for x in range(240)
                if all(abs(pp[x, y][c] - pf[x, y][c]) <= 24 for c in range(3)))
    frac = close / float(240 * 64)
    assert frac >= 0.90, (
        f"only {frac:.1%} of the top-left 240x64 agrees between PrintWindow "
        f"and the ClientToScreen+BitBlt fallback. The two capture paths are "
        f"reading different origins, so which pixels a test asserts depends "
        f"on whether PrintWindow succeeded (GitHub #279: the primary path "
        f"omitted PW_CLIENTONLY and was offset by the non-client frame).")


def test_the_detector_does_not_fire_on_a_plain_page():
    """A white page with ordinary dark content must read as clean.

    Without this the detector could simply return a positive constant and the
    test above would still pass.
    """
    img = Image.new("RGB", (597, 40), (255, 255, 255))
    px = img.load()
    for y in range(5, 12):                 # a line of "text"
        for x in range(30, 400, 3):
            px[x, y] = (20, 20, 20)
    assert toolbar_band_rows(img) == 0, (
        "the detector fired on a plain white page with text, so a passing "
        "live run would say nothing")
