#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Live canvas-gesture oracles for the MAT-2 matrix rows -- GitHub #221.

WHAT WAS WRONG WITH THESE ROWS

Three rows in ``tests/ui_automation/matrix/ui_test_matrix.csv`` carried
``status=xfail`` with the note "P-GAP-92 blocked":

    canvas_rubber_band          canvas.rubber_band_select
    canvas_scroll_wheel         canvas.scroll_wheel
    canvas_gesture_multiselect  canvas.gesture_multiselect

Two things were wrong with that, and they need separating.

**The citation was stale.** P-GAP-92 -- the Qt6 synthetic-event spontaneity
barrier -- records RESOLVED; it was closed by moving to Win32 ``SendInput``. A
reader chasing the citation lands on a closed item and concludes the rows are
ready to flip green.

**The residual cause is a different mechanism.** A rubber band, a scroll and a
gesture are CONTINUOUS drags, and Qt coalesces the intermediate motion events
``SendInput`` delivers, so the gesture arrives as a click. ``send_drag`` in
``tests/helpers/win32_input.py`` mitigates that by issuing each step as its own
``SendInput`` call with real elapsed time between them, but the project's
settled answer for this class -- the one that closed the archived P-GAP-71 and
P-GAP-91 -- is to drive the PRODUCTION path through a seam instead of
synthesising OS input at all. That is what these tests do.

WHY THE THIRD ROW IS NOT HERE

``canvas_gesture_multiselect`` asserts "Ctrl+click on 2 annotations -> both
selected". **PDFE does not implement that.** ``SelectTool::onMousePress``
(src/annotations/SelectTool.cpp:293-294) clears ``selectedAnnotIndices_``
unconditionally on a hit and never reads ``event->modifiers()``; the only
producer of a multi-selection is the rubber band
(SelectTool.cpp:384-396). So the row asserts an unimplemented PRODUCT
behaviour, not a harness limitation -- no seam can make it pass, and writing a
test that expects two selections would simply be writing a failing test.
Recorded as a blocker in ``tests/docs/known_issues.md``
(PDFE-LIMITATION-NO-CTRL-CLICK-MULTISELECT) and cross-referenced on GitHub
#221 rather than left as a stale xfail.

NOTE ON FIXTURES. Both surviving rows cited fixtures that do not fit:
``test_annotations.pdf`` does not exist in ``tests/fixtures/`` at all, and a
scroll oracle needs a page taller than the viewport. The annotations are
therefore CREATED through the production sticky-note path at the start of the
rubber-band test, and the scroll test zooms in first so there is scroll range
to observe. Both facts are reflected in the CSV's fixture column.
"""
from __future__ import annotations

import pathlib
import sys
import time

import pytest

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    kill_pdfe_processes,
)
from tests.ui_automation.harness.dialog_helpers import find_by_suffix  # noqa: E402

FIXTURES = REPO_ROOT / "tests" / "fixtures"


def _view_state(fx) -> dict:
    """Read the view-state seam as a dict.

    The handler answers ``page=..,scrollY=..,annots=..,selected=..,scrollX=..,
    selCount=..,rect=..,viewRot=..,pageRot=..``. Parsed into a dict rather
    than index-read so a future field added to the middle of that string
    cannot silently shift what a test believes it is asserting on.
    """
    q = find_by_suffix(fx.main_window, "testViewStateQueryPipe")
    r = find_by_suffix(fx.main_window, "testViewStateResultPipe")
    assert q is not None and r is not None, (
        "the view-state query/result pipes must be reachable; without them "
        "these gestures have no oracle and the test would degenerate into a "
        "liveness check")
    q.set_edit_text("1")
    time.sleep(0.4)
    from tests.ui_automation.harness.pdfe_app_fixture import _read_pipe_value
    raw = (_read_pipe_value(r) or "").strip()
    out = {}
    for part in raw.split(","):
        if "=" in part:
            k, _, v = part.partition("=")
            out[k.strip()] = v.strip()
    assert out, "view-state seam returned %r" % raw
    return out


@pytest.fixture()
def pdfe_3page():
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    src = FIXTURES / "test_3page.pdf"
    if not src.exists():
        pytest.skip(f"fixture missing: {src}")
    import shutil
    import tempfile
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="pdfe_mat2_"))
    dst = tmp / "test_3page.pdf"
    shutil.copy2(src, dst)
    kill_pdfe_processes()
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    time.sleep(2.0)
    yield fx
    try:
        fx.tearDown()
    except Exception:
        pass
    shutil.rmtree(tmp, ignore_errors=True)
    # No orphaned pdfe.exe: a survivor holds the machine-global single-instance
    # mutex and wedges the next run.
    kill_pdfe_processes()


@pytest.mark.timeout(180)
def test_canvas_rubber_band(pdfe_3page):
    """canvas_rubber_band -- a rubber-band drag multi-selects the annotations
    it encloses, observed through ``selCount``.

    The pre-drag count is the CONTROL. Without it, "2 are selected afterwards"
    is satisfied by a document that already had 2 selected and by a drag that
    did nothing at all -- which is how this row's whole family previously
    passed on ``proc.poll() is None``.
    """
    fx = pdfe_3page
    tool = find_by_suffix(fx.main_window, "testToolActivatePipe")
    click = find_by_suffix(fx.main_window, "testClickByPdfCoordPipe")
    drag = find_by_suffix(fx.main_window, "testCanvasDragPipe")
    assert tool is not None and click is not None and drag is not None, (
        "testToolActivatePipe / testClickByPdfCoordPipe / testCanvasDragPipe "
        "must all be reachable (PDFE_TEST_PIPES)")

    before_annots = int(_view_state(fx).get("annots", "-1"))
    assert before_annots >= 0, "annotation count seam did not answer"

    # Two sticky notes through the production creation path, in top-left PDF
    # coords (clickByPdfCoord uses canvasRectFromPdf, which is top-left origin).
    for x in (178, 357):
        tool.set_edit_text("stickyNoteToolAction")
        time.sleep(0.5)
        click.set_edit_text(f"0,{x},252")
        time.sleep(1.5)
        fx.main_window.type_keys("{ESC}")
        time.sleep(0.5)

    after_create = int(_view_state(fx).get("annots", "-1"))
    assert after_create >= before_annots + 2, (
        f"could not create the 2 annotations the drag is supposed to enclose "
        f"({before_annots} -> {after_create}); the selection assertion below "
        f"would then be measuring nothing")

    # Install SelectTool. activateToolByObjectName returns early when the
    # action is already checked, so toggle through another tool first.
    tool.set_edit_text("stickyNoteToolAction")
    time.sleep(0.5)
    tool.set_edit_text("selectToolAction")
    time.sleep(0.6)

    sel_before = int(_view_state(fx).get("selCount", "-1"))
    assert sel_before >= 0, "selCount seam did not answer before the drag"
    assert sel_before < 2, (
        f"{sel_before} annotations were already multi-selected before the "
        f"drag, so a post-drag count of 2 would prove nothing about the drag")

    drag.set_edit_text("0,89,127,446,380")
    time.sleep(1.0)

    sel_after = int(_view_state(fx).get("selCount", "-1"))
    assert sel_after >= 2, (
        f"rubber-band drag (89,127)->(446,380) over {after_create - before_annots} "
        f"annotations selected {sel_after}, expected >= 2 (control: "
        f"{sel_before} before the drag)")


@pytest.mark.timeout(180)
def test_canvas_scroll_wheel(pdfe_3page):
    """canvas_scroll_wheel -- a wheel event scrolls the viewport vertically.

    Zoom in first. At the default fit-page zoom a page can occupy the whole
    viewport with no scroll range, in which case "scrollY did not change" is
    the CORRECT behaviour and a test asserting otherwise would be asserting a
    bug. Establishing the range is part of the precondition, not a workaround.
    """
    fx = pdfe_3page
    zoom = find_by_suffix(fx.main_window, "testSetZoomPipe")
    scroll = find_by_suffix(fx.main_window, "testScrollPipe")
    assert zoom is not None and scroll is not None, (
        "testSetZoomPipe / testScrollPipe must be reachable (PDFE_TEST_PIPES)")

    zoom.set_edit_text("200")
    time.sleep(1.2)

    before = _view_state(fx)
    y_before = int(before.get("scrollY", "-1"))
    assert y_before >= 0, "scrollY seam did not answer before the wheel event"

    # Negative pixelDelta = scroll down, matching a physical wheel.
    scroll.set_edit_text("-600")
    time.sleep(1.0)

    after = _view_state(fx)
    y_after = int(after.get("scrollY", "-1"))
    assert y_after >= 0, "scrollY seam stopped answering after the wheel event"
    assert y_after != y_before, (
        f"wheel event did not scroll the viewport: scrollY stayed at "
        f"{y_before} (page before={before.get('page')} after={after.get('page')}). "
        f"At 200% zoom on a 3-page document there is scroll range, so an "
        f"unchanged offset means the event was not delivered or not consumed.")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
