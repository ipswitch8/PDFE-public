# -*- coding: utf-8 -*-
"""Live-UI coverage for GitHub #216 and #217 — pipeline p87 phase-3.

Both defects are things a user sees and no headless test could have caught on
its own, so both are driven here against a REAL pdfe.exe: a real menu bar, a
real tabified dock, real rendered canvas pixels.

#216 — ``View > Search Panel`` must RAISE a tabified dock
--------------------------------------------------------
The shipped layout tabifies Search with Properties in the right-hand dock
area. A dock that is behind a sibling tab is not hidden — it is a live, mapped
child that simply is not the current tab — so the old
``dock->setVisible(!dock->isVisible())`` HID the panel the user had just asked
to see, and the menu item read as inert.

The oracle is ``testDockStateQueryPipe``, whose ``raised`` field is exactly
``!isHidden() && !visibleRegion().isEmpty()``. That distinction is the whole
defect and **UIA cannot make it**: a tabbed-behind QDockWidget is still a
present, "visible" UIA pane either way — the seam's own comment in
MainWindow.cpp says so. The panel is driven through the ACTUAL View menu
(``menu_nav.click_menu_path``) rather than the tool-activate pipe, because
``activateToolByObjectName`` returns early for a checkable action that is
already checked and so cannot exercise the menu item at all.

#217 — the dark theme must repaint the PdfCanvas viewport
---------------------------------------------------------
``PdfCanvas::paintEvent`` fills its rect with
``palette().color(QPalette::Dark)``, and ``ThemeManager::paletteFor(Dark)``
never set that role — so a theme switch moved the chrome and left the canvas
backdrop on its light-mode grey. A previous investigation captured 24
sequential before/after pairs and found them PIXEL-IDENTICAL on every clean
capture (see ``diag_gap20_theme_toggle.py``), which is what makes this a
product defect rather than the capture flake it was once filed as.

NO GUESSED COORDINATES. The measurement here is position-dependent — the
backdrop is the part of the canvas the PAGE IS NOT ON — so the page rectangle
comes from ``canvas_geometry.page_rect_on_canvas`` (the
``testPageRectQueryPipe`` seam, i.e. ``PdfCanvas::getPageScreenRect``). The
seam's answer and the canvas rect are both in canvas-widget logical pixels, so
the test converts them to a FRACTION of the canvas before applying them to the
captured image; that ratio is DPI-independent, which a raw pixel offset is not
(a screenshot measurement once disagreed with the UIA canvas rect by ~145 px
for exactly this reason). If the seam does not answer, the test FAILS. There
is no fallback.

Teardown: every session runs under ``_session``, which closes the window and
then asserts no ``pdfe.exe`` survives — a stray one holds a MACHINE-GLOBAL
single-instance mutex and wedges the next run. The theme test additionally
restores the Light theme before closing, because the choice is PERSISTED and a
dark-left-behind setting would change what the next suite launches into.
"""
from __future__ import annotations

import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Dict, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.pdfe_session import (  # noqa: E402
    PDFE_EXE,
    PDFESession,
    kill_pdfe_orphans,
    no_orphans,
)
from tests.helpers.canvas_geometry import (  # noqa: E402
    canvas_rect,
    page_rect_on_canvas,
)
from tests.helpers.qinvoke import _find_pipe, _write_to_pipe  # noqa: E402
from tests.ui_automation.harness import render_oracle as ro  # noqa: E402
from tests.ui_automation.harness.menu_nav import click_menu_path  # noqa: E402

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_3PAGE = FIXTURES / "test_3page.pdf"

pytestmark = pytest.mark.skipif(
    not Path(PDFE_EXE).exists(),
    reason=f"PDFE build artifact not present at {PDFE_EXE}",
)


# ---------------------------------------------------------------------------
# Session plumbing
# ---------------------------------------------------------------------------

@contextmanager
def _session(fixture: Path, settle: float = 2.0):
    """Launch PDFE on *fixture*, yield the session, prove clean teardown."""
    assert fixture.exists(), f"fixture missing: {fixture}"
    kill_pdfe_orphans()
    s = PDFESession(pdf_path=str(fixture))
    s.__enter__()
    try:
        time.sleep(settle)
        yield s
    finally:
        try:
            s.close()
        finally:
            kill_pdfe_orphans()
    assert no_orphans(), (
        "a pdfe.exe survived teardown; it holds the machine-global "
        "single-instance mutex and will wedge the next run")


# ---------------------------------------------------------------------------
# #216 — dock state oracle
# ---------------------------------------------------------------------------

def _dock_state(session, settle: float = 0.5) -> Dict[str, Dict[str, int]]:
    """``{objectName: {"shown": int, "area": int, "raised": int}}``.

    Reads ``testDockStateQueryPipe`` → ``testDockStateResultPipe``. Returns an
    empty dict when the seam does not answer; callers must FAIL on that rather
    than guess at dock state from the UIA tree, which cannot tell a raised tab
    from a tab sitting behind one.
    """
    if not _write_to_pipe(session, "testDockStateQueryPipe", "?"):
        return {}
    time.sleep(settle)
    el = _find_pipe(session, "testDockStateResultPipe")
    if el is None:
        return {}
    raw = (el.get_value() or "").strip()
    if not raw:
        return {}
    out: Dict[str, Dict[str, int]] = {}
    for record in raw.split(";"):
        parts = record.split(":")
        if len(parts) != 4:
            continue
        try:
            out[parts[0]] = {"shown": int(parts[1]),
                             "area": int(parts[2]),
                             "raised": int(parts[3])}
        except ValueError:
            continue
    return out


def _set_dock(session, name: str, visible: bool) -> None:
    assert _write_to_pipe(session, "testDockSetPipe",
                          f"{name}:{1 if visible else 0}"), (
        f"testDockSetPipe unreachable (dock {name!r})")
    time.sleep(0.5)


@pytest.mark.timeout(240)
def test_view_search_panel_raises_a_tabified_dock_216():
    """``View > Search Panel`` raises Search out from behind Properties."""
    with _session(FIX_3PAGE) as s:
        # Arrange: put Properties in front so Search is SHOWN BUT BEHIND. The
        # setter raises whatever it shows, which is precisely how a user gets
        # into this state by clicking the Properties tab.
        _set_dock(s, "searchPanelDock", True)
        _set_dock(s, "propertiesPanelDock", True)

        before = _dock_state(s)
        assert before, (
            "testDockStateQueryPipe did not answer — the dock-state seam is "
            "the only thing that can tell 'raised' from 'tabbed behind', so "
            "there is nothing to assert on. Was pdfe launched with "
            "PDFE_TEST_PIPES=1?")
        assert "searchPanelDock" in before, sorted(before)
        assert "propertiesPanelDock" in before, sorted(before)

        # PRECONDITION, asserted not assumed. Without it this case could pass
        # against a merely HIDDEN dock, which the old code already handled.
        assert before["searchPanelDock"]["shown"] == 1, (
            "Search dock is hidden; #216 is about the SHOWN-but-behind-a-tab "
            f"state. state={before['searchPanelDock']}")
        assert before["searchPanelDock"]["raised"] == 0, (
            "Search is already the raised tab, so the arrangement did not "
            "take and the toggle would be measured in the wrong state. "
            f"state={before['searchPanelDock']}")
        assert before["propertiesPanelDock"]["raised"] == 1, (
            "Properties should be the raised tab after the arrangement; "
            f"state={before['propertiesPanelDock']}")

        # Act: the REAL menu item, which is the surface #216 is reported
        # against ("the menu item reads as inert").
        click_menu_path(s.window, "View -> Search Panel")
        time.sleep(1.0)

        after = _dock_state(s)
        assert after, "dock-state seam went silent after the menu click"
        state = after["searchPanelDock"]

        assert state["shown"] == 1, (
            "View > Search Panel HID the panel the user asked to see — the "
            "isVisible() toggle counting 'tabbed behind' as showing (#216). "
            f"state={state}")
        assert state["raised"] == 1, (
            "View > Search Panel left the Search dock behind the Properties "
            f"tab; the menu item is still inert (#216). state={state}")
        assert after["propertiesPanelDock"]["raised"] == 0, (
            "Search was raised without Properties yielding the tab group — "
            "the two cannot both be the current tab, so the oracle is "
            f"reporting something impossible. state={after}")


# ---------------------------------------------------------------------------
# #217 — canvas repaint oracle
# ---------------------------------------------------------------------------

def _capture_stable(session, tries: int = 6, interval: float = 0.4,
                    threshold: float = 0.005):
    """Capture the canvas once two consecutive frames agree.

    A plain capture taken right after launch can grab a mid-render frame; a
    later capture then catches the settled one and a "did it change?" oracle
    sees a change that is pure capture noise. This is the same stability gate
    ``render_drive.capture_stable`` uses, and it is why this test can make a
    POSITIVE claim about a pixel change.
    """
    prev = ro.capture_canvas(session.window)
    for _ in range(max(1, tries - 1)):
        time.sleep(interval)
        cur = ro.capture_canvas(session.window)
        if not ro.images_differ(prev, cur, threshold=threshold):
            return cur
        prev = cur
    return prev


def _page_fraction_on_canvas(session) -> Tuple[float, float, float, float]:
    """The page rect as a FRACTION ``(fx, fy, fw, fh)`` of the canvas.

    Both inputs come from the application (``testCanvasRect``-space UIA canvas
    rect and the ``testPageRectQueryPipe`` page rect, both canvas-widget
    logical pixels), so their ratio is DPI-independent and can be applied to a
    captured image of any scale. FAILS if either seam is silent — never guesses.
    """
    canvas = canvas_rect(session)
    assert canvas is not None, (
        "the PdfCanvas rect could not be resolved from the UIA tree; without "
        "it the page cannot be located and a backdrop sample would be a guess")
    page = page_rect_on_canvas(session, 0)
    assert page is not None, (
        "testPageRectQueryPipe did not answer. CLAUDE.md is explicit: when the "
        "page-rect seam does not answer, FAIL — do not fall back to a "
        "fraction-of-the-canvas guess, which has put a measurement at "
        "PDF x = -30.7 before now.")
    cw = canvas.right - canvas.left
    ch = canvas.bottom - canvas.top
    assert cw > 0 and ch > 0, f"degenerate canvas rect {canvas}"
    px, py, pw, ph = page
    return (px / cw, py / ch, pw / cw, ph / ch)


def _backdrop_stats(img, page_fraction) -> Tuple[float, int, Tuple[int, int, int]]:
    """``(mean luminance, pixel count, dominant RGB)`` of the canvas BACKDROP.

    The backdrop is the part of the canvas the page is not on — the grey
    margin. That is the surface ``PdfCanvas::paintEvent`` fills with
    ``QPalette::Dark``, so measuring it (rather than the whole canvas) means
    the assertion cannot be satisfied by the PAGE re-rendering for some
    unrelated reason.

    The DOMINANT colour is returned as well as the mean because "it got
    darker" is a weak claim: it is satisfied by any mechanism that happens to
    put a darker colour there, including the one #217 turned out to be hiding
    behind (``adaptColor`` substituting ``QPalette::Window`` because the
    High-Contrast detector was permanently true). Naming the exact expected
    shade is what makes this test able to tell those apart.
    """
    w, h = img.size
    fx, fy, fw, fh = page_fraction
    x0, y0 = int(fx * w), int(fy * h)
    x1, y1 = int((fx + fw) * w), int((fy + fh) * h)
    px = img.load()
    total = 0.0
    count = 0
    histogram: Dict[Tuple[int, int, int], int] = {}
    for y in range(0, h, 2):
        inside_rows = y0 <= y < y1
        for x in range(0, w, 2):
            if inside_rows and x0 <= x < x1:
                continue
            r, g, b = px[x, y][:3]
            total += 0.2126 * r + 0.7152 * g + 0.0722 * b
            histogram[(r, g, b)] = histogram.get((r, g, b), 0) + 1
            count += 1
    dominant = max(histogram.items(), key=lambda kv: kv[1])[0] if histogram \
        else (0, 0, 0)
    return (total / count if count else 0.0), count, dominant


def _distance(a, b) -> float:
    return sum((int(p) - int(q)) ** 2 for p, q in zip(a, b)) ** 0.5


#: ``ThemeManager::paletteFor(Theme::Dark).color(QPalette::Dark)`` — the shade
#: ``PdfCanvas::paintEvent`` fills its viewport with under the dark theme. Kept
#: as a literal rather than derived from the app, because a test that asks the
#: product what colour it should be cannot catch the product being wrong.
#: Mirrored in src/theme/ThemeManager.cpp.
DARK_CANVAS_SHADE = (30, 30, 30)

#: NOTE on the light theme: its canvas shade is NOT pinned to a literal here.
#: ``paletteFor(Light)`` returns ``QApplication::style()->standardPalette()``,
#: so the value depends on the active Qt style — the live app measures
#: ``(106, 104, 100)`` where a Fusion-styled headless process measures
#: ``(160, 160, 160)``. Pinning it would make this test assert the runner's
#: style rather than the product's behaviour. The light side is asserted
#: structurally instead (it must be far from the dark shade).

#: ``QPalette::Window`` under the dark theme. Asserted AGAINST: if the backdrop
#: lands here instead of on DARK_CANVAS_SHADE, ``adaptColor`` is overriding the
#: canvas's role again — the High-Contrast-detector half of #217 has regressed.
DARK_WINDOW_COLOUR = (53, 53, 53)


@pytest.mark.timeout(300)
def test_dark_theme_repaints_the_canvas_viewport_217():
    """Switching to the Dark theme changes the PdfCanvas backdrop pixels."""
    with _session(FIX_3PAGE) as s:
        # Start from a KNOWN theme. The choice is persisted, so a previous run
        # could otherwise leave this process already dark and the "switch"
        # would be a no-op that reads as a product failure.
        assert _write_to_pipe(s, "testToolActivatePipe", "themeLightAction"), (
            "testToolActivatePipe unreachable")
        time.sleep(1.5)

        page_fraction = _page_fraction_on_canvas(s)
        fx, fy, fw, fh = page_fraction
        assert fw < 0.98 or fh < 0.98, (
            "the page fills the canvas, so there is no backdrop to measure. "
            f"page fraction={page_fraction}. Zoom out, or the oracle below "
            "would be measuring rendered page content instead of the viewport "
            "background this defect is about.")

        before = _capture_stable(s)
        lum_before, sampled, dom_before = _backdrop_stats(before, page_fraction)
        assert sampled > 2000, (
            f"only {sampled} backdrop pixels were sampled — too few to make a "
            "claim about the viewport background")

        assert _write_to_pipe(s, "testToolActivatePipe", "themeDarkAction"), (
            "testToolActivatePipe unreachable for the dark switch")
        time.sleep(2.0)

        after = _capture_stable(s)
        lum_after, sampled_after, dom_after = _backdrop_stats(after,
                                                              page_fraction)
        assert sampled_after == sampled, (
            "the canvas changed size across the toggle, so before/after are "
            "not comparable")

        try:
            assert before.size == after.size
            assert ro.images_differ(before, after, threshold=0.0001), (
                "the PdfCanvas rasterised IDENTICALLY before and after the "
                "switch to the Dark theme — the canvas viewport does not "
                "follow the theme (#217)")

            # The headline claim, measured where the defect lives: the grey
            # margin around the page got materially darker.
            assert lum_after < lum_before - 40.0, (
                "the canvas BACKDROP did not darken on the theme switch "
                f"(mean luminance {lum_before:.1f} -> {lum_after:.1f}). The "
                "chrome may have gone dark while the viewport stayed on its "
                "light-mode QPalette::Dark shade — that is exactly #217.")

            # And it landed on the theme's OWN canvas shade. "Darker" alone is
            # too weak: before the AccessibilityHelper half of #217 was fixed,
            # the backdrop went dark via adaptColor substituting
            # QPalette::Window, with the canvas's shade role never consulted —
            # and a darker-than-before assertion passed against exactly that.
            assert _distance(dom_before, DARK_CANVAS_SHADE) > 40.0, (
                f"the LIGHT-theme canvas backdrop is already {dom_before}, "
                "indistinguishable from the dark shade — the 'before' state "
                "is not light, so the comparison proves nothing")
            assert _distance(dom_after, DARK_WINDOW_COLOUR) > 12.0, (
                f"the dark-theme canvas backdrop is {dom_after}, which is "
                f"QPalette::Window {DARK_WINDOW_COLOUR}, not the canvas shade. "
                "AccessibilityHelper::adaptColor is overriding the role again "
                "— the High-Contrast detector half of #217 has regressed.")
            assert _distance(dom_after, DARK_CANVAS_SHADE) < 12.0, (
                f"the dark-theme canvas backdrop is {dom_after}, expected the "
                f"dark palette's QPalette::Dark shade {DARK_CANVAS_SHADE}")
        finally:
            # Restore the persisted preference for whatever runs next.
            _write_to_pipe(s, "testToolActivatePipe", "themeLightAction")
            time.sleep(1.0)


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
