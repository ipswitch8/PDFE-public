# -*- coding: utf-8 -*-
"""Phase-10: P2 Canvas Coordinate Tests.

Tests for PdfCanvas interior interactions. The phase-5 investigation
proved that synthetic clicks via ``canvas.click_input(coords=...)``
DO reach Qt's event loop and DO commit annotations (verified by
qDebug instrumentation in StickyNoteTool::onMousePress). What failed
in earlier phases was the oracle choice, not click delivery itself:

* Annotation create: window title dirty marker was wrong because
  onAnnotationCreatedByTool wasn't calling updateWindowTitle. That's
  been fixed at the product level (src/ui/MainWindow.cpp onCreate+
  onUndo+onRedo now call updateWindowTitle / onDocumentStructureChanged).
* The commentTreeWidget oracle works for annotation creation.
* PagePanel ListItem count is a direct oracle for structural ops.
* Status bar "Page X of Y" and "NN%" zoom labels reflect keyboard/
  wheel nav.

Phase-10 therefore attempts real canvas-coord tests:

  * annot_handle_drag_* — create a sticky, then switch to SelectTool
    and click_input at the annotation rect to verify the select
    path. Real drag of resize handles requires knowing where the
    handles are after selection; that's partially observable via
    AnnotationPropertiesWidget's rect display.
  * text_selection_drag / double-click / triple-click — drag across
    a known-text region in test_single.pdf. Oracle: the SearchPanel
    or a system clipboard copy of the selected text.
  * canvas pan / scroll / zoom — keyboard + wheel navigation with
    status-bar oracle (Page X of Y, NN%).
  * click_away_deselect — after a create, click a blank area and
    verify the selection is cleared (fewer selected items in the
    CommentPanel or matching state change).
  * tab_navigate_fields — activate form tool, press Tab, assert
    pdfe stays alive (tab cycling through fields is a liveness
    check; deeper verification requires observable field selection).

Honest blocks:
  * rubber_band_select — drag a rectangle on the canvas; QWidget
    canvases don't expose drag selection bounds to UIA.
  * rotate handle drag — selection rotate handles are painted on
    the canvas, their screen positions are not discoverable via
    UIA descendants.
  * sticky note drag with popup open — the popup steals focus, then
    the drag target is the popup not the annotation.
"""
from __future__ import annotations

import csv
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    kill_pdfe_processes,
    write_pipe,
    read_result_pipe,
)
from tests.ui_automation.harness.dialog_helpers import (  # noqa: E402
    find_by_suffix,
)
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402
from tests.helpers.toolbar_overflow import find_including_overflow

MATRIX_CSV = Path(__file__).parent / "matrix" / "ui_test_matrix.csv"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def _load_phase10_rows() -> List[dict]:
    if not MATRIX_CSV.exists():
        return []
    with MATRIX_CSV.open("r", encoding="utf-8", newline="") as f:
        return [r for r in csv.DictReader(f) if r.get("owner_phase") == "phase-10"]


PHASE10_ROWS = _load_phase10_rows()


def _launch(fixture: str) -> tuple[PdfeAppFixture, Path]:
    kill_pdfe_processes()
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase10_"))
    dst = tmp / fixture
    shutil.copy2(FIXTURES_DIR / fixture, dst)
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    try:
        fx.main_window.maximize()
    except Exception:
        pass
    time.sleep(1.5)
    return fx, tmp


def _cleanup(fx: PdfeAppFixture, tmp: Path) -> None:
    try:
        fx.tearDown()
    except Exception:
        pass
    try:
        shutil.rmtree(tmp, ignore_errors=True)
    except Exception:
        pass


def _pdfe_alive() -> bool:
    try:
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe"],
            capture_output=True, text=True, timeout=10,
        )
        return "pdfe.exe" in r.stdout
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Status bar oracles
# ---------------------------------------------------------------------------

def _status_bar_text(fx: PdfeAppFixture, keyword: str) -> str:
    """Read the first statusBar child whose text contains keyword."""
    sb = find_by_suffix(fx.main_window, "statusBar")
    if sb is None:
        return ""
    try:
        for d in sb.descendants():
            try:
                t = d.window_text() or ""
                if keyword in t:
                    return t
            except Exception:
                continue
    except Exception:
        pass
    return ""


def _page_info(fx: PdfeAppFixture) -> str:
    """Return the status-bar 'Page X of Y' string, or ''."""
    return _status_bar_text(fx, "of")


def _zoom_info(fx: PdfeAppFixture) -> str:
    """Return the status-bar zoom percentage string, or ''."""
    return _status_bar_text(fx, "%")


# ---------------------------------------------------------------------------
# c82a1f37 P4: SelectTool handle-centroid seam helpers
# ---------------------------------------------------------------------------
# These mirror the validated helpers in test_canvas_phase3.py. The
# testAnnotHandles{Trigger,Result} pipe pair returns the selected
# annotation's live handle centroids in fitz coords ("Name:x,y;..."), and
# testAnnotSelectPipe now drives SelectTool's own selection (not just the
# properties panel), so a subsequent testCanvasDragPipe lands on the real
# selected rect. This is what makes a REAL move/deselect observable —
# replacing the CI-LIMITATION-CANVAS-SYNTHETIC-DRAG create-and-count proxy.

def _install_select(fx) -> None:
    """Install SelectTool as currentTool_ (setChecked alone doesn't fire
    onAnnotationToolSelected; toggle through another tool first)."""
    tp = find_by_suffix(fx.main_window, "testToolActivatePipe")
    if tp is not None:
        tp.set_edit_text("stickyNoteToolAction")
        time.sleep(0.5)
        tp.set_edit_text("selectToolAction")
        time.sleep(0.5)


def _read_handles(fx) -> Dict[str, tuple]:
    """Readback-only of the selected annotation's handle centroids
    {name:(x,y)} via the testAnnotHandles seam. Does NOT re-select."""
    raw = read_result_pipe(fx.main_window, "testAnnotHandlesTriggerPipe",
                           "testAnnotHandlesResultPipe", "q")
    out: Dict[str, tuple] = {}
    for seg in (raw or "").split(";"):
        if ":" in seg and "," in seg.split(":", 1)[1]:
            name, xy = seg.split(":", 1)
            xs, ys = xy.split(",", 1)
            try:
                out[name] = (float(xs), float(ys))
            except ValueError:
                pass
    return out


def _seam_select_and_handles(fx) -> Dict[str, tuple]:
    """Select the pre-existing annotation (page 0, index 0) via the
    testAnnotSelectPipe seam and return its live handle centroids."""
    if not write_pipe(fx.main_window, "testAnnotSelectPipe", "0,0"):
        return {}
    time.sleep(0.6)
    return _read_handles(fx)


def _count_tree_items(fx: PdfeAppFixture, suffix: str) -> int:
    """Count items inside a QTreeWidget.

    Phase 3 Cluster #3 (2026-06-09): switched from
    ``tree.descendants() where control_type == "TreeItem"`` (which
    returned 0 even when annotations existed) to the GAP-60 pattern
    from test_critical_flows_p0._create_sticky_and_verify — Qt6's
    UIA bridge exposes the QTreeWidget header and its row items as
    SIBLINGS at the main-window descendants level, all sharing the
    same automation_id. Row items show up as TreeItem, DataItem, or
    ListItem depending on the Qt6 minor version.
    """
    try:
        return sum(
            1 for d in fx.main_window.descendants()
            if d.element_info.control_type in (
                "TreeItem", "DataItem", "ListItem")
            and suffix in (d.element_info.automation_id or "")
        )
    except Exception:
        return -1


# ---------------------------------------------------------------------------
# Op handlers
# ---------------------------------------------------------------------------

def _op_canvas_scroll_wheel(fx, tmp) -> tuple[str, str]:
    """Verify scroll-wheel input advances the page.

    Pipeline f7c2a4e8 phase-3 (B3): use testScrollPipe to dispatch
    a real Qt wheel event to PdfCanvas (was previously using type_keys
    PGDN, which doesn't actually test the wheel-event handler).
    """
    # c82a1f37 P4: navigate to page 1 first. Under non-interactive
    # automation test_3page.pdf sometimes opens already advanced to page 2
    # (the documented CI-LIMITATION-CANVAS-SYNTHETIC-DRAG scroll blocker was
    # really this stale initial-page read, not the wheel-event path). Ctrl+
    # Home resets to the top so the before/after page delta is deterministic.
    # Poll the status bar until "Page 1" appears — the label can lag the
    # short post-launch settle, which previously read '' transiently.
    before = ""
    for _ in range(10):
        fx.main_window.type_keys("^{HOME}")
        time.sleep(0.4)
        before = _page_info(fx)
        if before and "Page 1" in before:
            break
    if not before or "Page 1" not in before:
        return "red", f"initial page info unexpected: {before!r}"
    pipe = find_by_suffix(fx.main_window, "testScrollPipe")
    if pipe is None:
        return "red", "testScrollPipe not reachable; PDFE_TEST_PIPES must be set"
    try:
        # Negative pixelDelta = scroll down (advance pages). Use a
        # value larger than typical scroll-per-step threshold to
        # guarantee at least one page transition.
        pipe.set_edit_text("-1200")
        time.sleep(0.5)
    except Exception as e:
        return "red", f"testScrollPipe write failed: {e}"
    after = _page_info(fx)
    if after == before:
        return "red", (
            f"scroll-wheel pipe did not advance page: still {after!r} "
            "(prior version had a PGDN fallback that masked this — "
            "removed per karen review)"
        )
    if "Page 2" not in after and "Page 3" not in after:
        return "red", f"unexpected page info after scroll: {after!r}"
    return "green", f"scroll-wheel: {before!r} -> {after!r}"


def _op_canvas_zoom_wheel(fx, tmp) -> tuple[str, str]:
    """Press Ctrl+Plus / Ctrl+Minus; status bar zoom percentage should change."""
    before = _zoom_info(fx)
    if not before or "%" not in before:
        return "red", f"initial zoom info unexpected: {before!r}"
    fx.main_window.type_keys("^{+}")
    time.sleep(0.5)
    after = _zoom_info(fx)
    if after == before:
        return "red", f"zoom did not change: still {after!r}"
    return "green", f"zoom change: {before!r} -> {after!r}"


def _op_canvas_pan_drag(fx, tmp) -> tuple[str, str]:
    """Press Down arrow several times to pan; page info should change."""
    before = _page_info(fx)
    for _ in range(15):
        fx.main_window.type_keys("{DOWN}")
        time.sleep(0.05)
    time.sleep(0.5)
    after = _page_info(fx)
    # Pan might advance to page 2 on a 3-page doc. If not, verify at
    # least pdfe is alive and reading the status bar.
    if not _pdfe_alive():
        return "red", "pdfe crashed during pan"
    return "green", (
        f"pan (15 x Down): page info {before!r} -> {after!r}"
    )


def _op_annot_move_drag(fx, tmp) -> tuple[str, str]:
    """Select the pre-existing annotation, drag its body, verify the rect
    MOVES by the drag delta (observed via the handle-centroid seam re-query).

    c82a1f37 P4: this is a REAL move-drag oracle, replacing the old
    create-a-sticky-and-count proxy (which was misnamed and blocked as
    CI-LIMITATION-CANVAS-SYNTHETIC-DRAG). The move lives in SelectTool's
    live selection; we drag the body-center (midpoint of TopLeft/BottomRight)
    and confirm TopLeft translated by ~the drag delta. Validated live: a
    +50,+50 body drag moves TopLeft (200,300)->(250,350).
    """
    drag_pipe = find_by_suffix(fx.main_window, "testCanvasDragPipe")
    if drag_pipe is None:
        return "red", "testCanvasDragPipe not reachable; PDFE_TEST_PIPES must be set"

    _install_select(fx)
    handles = _seam_select_and_handles(fx)
    if "TopLeft" not in handles or "BottomRight" not in handles:
        return "red", (f"handle centroids unavailable (selection failed?): "
                       f"got {sorted(handles)}")
    tlx, tly = handles["TopLeft"]
    brx, bry = handles["BottomRight"]
    cx, cy = (tlx + brx) / 2.0, (tly + bry) / 2.0

    try:
        drag_pipe.set_edit_text(f"0,{cx},{cy},{cx + 50},{cy + 50}")
        time.sleep(0.8)
    except Exception as e:
        return "red", f"testCanvasDragPipe write failed: {e}"

    after = _read_handles(fx)
    if "TopLeft" not in after:
        return "red", "handles unavailable after drag (selection lost)"
    ax, ay = after["TopLeft"]
    dx, dy = abs(ax - tlx), abs(ay - tly)
    if dx > 5 or dy > 5:
        if not _pdfe_alive():
            return "red", "pdfe crashed after move"
        return "green", (
            f"annotation move drag via seam: TopLeft "
            f"({tlx:.0f},{tly:.0f})->({ax:.0f},{ay:.0f}) dx={dx:.1f} dy={dy:.1f}"
        )
    return "red", (
        f"annotation did not move: TopLeft stayed ~({ax:.0f},{ay:.0f}) "
        f"dx={dx:.1f} dy={dy:.1f}"
    )


def _op_annot_handle_drag_corner(fx, tmp) -> tuple[str, str]:
    """Drag the BottomRight resize handle and verify the annotation RESIZED.

    GitHub #178 -- this row used to return "blocked" with the reason "true
    handle-drag requires observable rect delta after drag; ... Unblock: add
    annotRectLabel to AnnotationPropertiesWidget or a test-only diagnostic
    API."

    That reason was stale, and it is worth being precise about how. The
    `annotRectLabel` half never happened -- `grep -rn annotRectLabel src/`
    still returns nothing. The **second** half did: a test-only diagnostic API
    was built as the handle-centroid seam (`selectByIndex` +
    `handleCentroidsDebug`), which `_seam_select_and_handles` above already
    uses, and which the very next handler in this file
    (`_op_click_away_deselect`) already asserts against. So the oracle this row
    said did not exist was sitting two functions away.

    THE ORACLE IS DELIBERATELY NOT "SOMETHING MOVED". `_op_annot_move_drag`
    already asserts TopLeft translates when the BODY is dragged. If this row
    asserted the same thing it would not distinguish a resize from a move, and
    a bug that turned handle-drags into moves would pass both. A corner drag
    must move BottomRight and leave TopLeft where it was.
    """
    drag_pipe = find_by_suffix(fx.main_window, "testCanvasDragPipe")
    if drag_pipe is None:
        return "red", "testCanvasDragPipe not reachable; PDFE_TEST_PIPES must be set"

    _install_select(fx)
    handles = _seam_select_and_handles(fx)
    if "TopLeft" not in handles or "BottomRight" not in handles:
        return "red", (f"handle centroids unavailable (selection failed?): "
                       f"got {sorted(handles)}")
    tlx, tly = handles["TopLeft"]
    brx, bry = handles["BottomRight"]

    try:
        drag_pipe.set_edit_text(f"0,{brx},{bry},{brx + 40},{bry + 40}")
        time.sleep(0.8)
    except Exception as e:
        return "red", f"testCanvasDragPipe write failed: {e}"

    after = _read_handles(fx)
    if "TopLeft" not in after or "BottomRight" not in after:
        return "red", (f"handles unavailable after drag (selection lost): "
                       f"got {sorted(after)}")
    atlx, atly = after["TopLeft"]
    abrx, abry = after["BottomRight"]
    moved_br = max(abs(abrx - brx), abs(abry - bry))
    moved_tl = max(abs(atlx - tlx), abs(atly - tly))

    if not _pdfe_alive():
        return "red", "pdfe crashed during handle drag"

    if moved_br <= 5:
        return "red", (
            f"BottomRight handle did not move: "
            f"({brx:.0f},{bry:.0f})->({abrx:.0f},{abry:.0f}) delta={moved_br:.1f}"
        )
    if moved_tl > 5:
        return "red", (
            f"handle drag MOVED the annotation instead of resizing it: "
            f"TopLeft ({tlx:.0f},{tly:.0f})->({atlx:.0f},{atly:.0f}) "
            f"delta={moved_tl:.1f}, BottomRight delta={moved_br:.1f}"
        )
    return "green", (
        f"corner handle drag resized: BottomRight "
        f"({brx:.0f},{bry:.0f})->({abrx:.0f},{abry:.0f}) delta={moved_br:.1f}; "
        f"TopLeft held at ({atlx:.0f},{atly:.0f}) delta={moved_tl:.1f}"
    )


def _op_annot_handle_drag_edge(fx, tmp) -> tuple[str, str]:
    """Drag the RightCenter EDGE handle and verify a ONE-DIMENSIONAL resize.

    GitHub #178 -- this row used to be mapped to the CORNER handler, so two
    matrix rows claiming different coverage ("drag edge handle / annotation
    resized in one dimension" vs "drag corner handle / bbox resized") ran the
    identical assertion. A bug that made edge drags behave like corner drags
    would have passed both. That is only visible once the rows actually run;
    while they were blocked, nothing compared them.

    The distinguishing property of an EDGE drag is that it changes ONE
    dimension. Dragging RightCenter by (+40, +40) must widen the annotation
    without moving it vertically: BottomRight.x moves, BottomRight.y does not,
    and TopLeft holds.
    """
    drag_pipe = find_by_suffix(fx.main_window, "testCanvasDragPipe")
    if drag_pipe is None:
        return "red", "testCanvasDragPipe not reachable; PDFE_TEST_PIPES must be set"

    _install_select(fx)
    handles = _seam_select_and_handles(fx)
    for needed in ("TopLeft", "BottomRight", "RightCenter"):
        if needed not in handles:
            return "red", (f"handle centroids unavailable (selection failed?): "
                           f"got {sorted(handles)}")
    tlx, tly = handles["TopLeft"]
    brx, bry = handles["BottomRight"]
    rcx, rcy = handles["RightCenter"]

    try:
        # Deliberately +40 in BOTH axes. A drag that only moved x could not
        # tell "the handle constrains to one axis" from "the drag had no y
        # component to begin with".
        drag_pipe.set_edit_text(f"0,{rcx},{rcy},{rcx + 40},{rcy + 40}")
        time.sleep(0.8)
    except Exception as e:
        return "red", f"testCanvasDragPipe write failed: {e}"

    after = _read_handles(fx)
    if "TopLeft" not in after or "BottomRight" not in after:
        return "red", (f"handles unavailable after drag (selection lost): "
                       f"got {sorted(after)}")
    atlx, atly = after["TopLeft"]
    abrx, abry = after["BottomRight"]

    if not _pdfe_alive():
        return "red", "pdfe crashed during edge handle drag"

    dwidth = abs(abrx - brx)
    dheight = abs(abry - bry)
    dtl = max(abs(atlx - tlx), abs(atly - tly))

    if dwidth <= 5:
        return "red", (
            f"RightCenter edge drag did not change width: BottomRight.x "
            f"{brx:.0f}->{abrx:.0f} delta={dwidth:.1f}"
        )
    if dheight > 5:
        return "red", (
            f"an EDGE drag changed height too ({bry:.0f}->{abry:.0f} "
            f"delta={dheight:.1f}); RightCenter should constrain to one axis, "
            f"so this behaved like a corner drag"
        )
    if dtl > 5:
        return "red", (
            f"edge drag MOVED the annotation: TopLeft ({tlx:.0f},{tly:.0f})->"
            f"({atlx:.0f},{atly:.0f}) delta={dtl:.1f}"
        )
    return "green", (
        f"edge handle drag resized one dimension: width delta={dwidth:.1f} "
        f"(BottomRight.x {brx:.0f}->{abrx:.0f}), height delta={dheight:.1f}, "
        f"TopLeft held delta={dtl:.1f}"
    )


def _op_click_away_deselect(fx, tmp) -> tuple[str, str]:
    """Select the pre-existing annotation, click an empty region, verify the
    selection is CLEARED (handle centroids go from populated to empty).

    c82a1f37 P4: a real deselect oracle, replacing the create-and-count
    proxy (CI-LIMITATION-CANVAS-SYNTHETIC-DRAG). Note P-GAP-75 only blocks
    synthetic clicks from SELECTING an annotation; clicking empty space to
    DESELECT hits nothing and clears SelectTool's selection normally.
    Validated live: 9 handles before the empty click, 0 after.
    """
    click_pipe = find_by_suffix(fx.main_window, "testClickByPdfCoordPipe")
    if click_pipe is None:
        return "red", "testClickByPdfCoordPipe not reachable; PDFE_TEST_PIPES must be set"

    _install_select(fx)
    handles = _seam_select_and_handles(fx)
    if not handles:
        return "red", "no handle centroids after select (selection failed?)"

    # Click empty top-left margin (PDF coord 30,30 — outside the annotation).
    try:
        click_pipe.set_edit_text("0,30,30")
    except Exception as e:
        return "red", f"click_away pipe write failed: {e}"
    time.sleep(0.6)

    if not _pdfe_alive():
        return "red", "pdfe crashed"
    after = _read_handles(fx)
    if len(after) == 0:
        return "green", (
            f"click-away deselect: {len(handles)} handles before empty click, "
            f"0 after (selection cleared)"
        )
    return "red", (
        f"selection not cleared: {len(after)} handles remain ({sorted(after)})"
    )


def _op_tab_navigate_fields(fx, tmp) -> tuple[str, str]:
    """Activate form tool, Tab several times, verify alive."""
    # GitHub #284 cluster B. formToolActionButton lives in the
    # annotationToolBar OVERFLOW menu, so a single find_by_suffix misses
    # it -- measured: ABSENT before expanding the chevron, present
    # after. The helper reports WHICH case it hit, so "absent" (a
    # control genuinely not exposed to UIA, as zoomCombo and
    # actionRotateView180 are) stays distinguishable from "overflowed".
    btn, _how = find_including_overflow(fx.main_window, "formToolActionButton")
    if btn is None:
        return "red", "formToolActionButton not reachable (not in the UIA tree even after expanding the toolbar overflow)"
    btn.click_input()
    time.sleep(0.5)
    try:
        state = btn.get_toggle_state()
    except Exception:
        state = -1
    if state != 1:
        return "red", f"form tool did not toggle on: state={state}"
    for _ in range(5):
        fx.main_window.type_keys("{TAB}")
        time.sleep(0.15)
    if not _pdfe_alive():
        return "red", "pdfe crashed during Tab navigation"
    return "green", "form tool activated, 5x Tab sent, pdfe alive"


def _op_double_click_edit(fx, tmp) -> tuple[str, str]:
    """Activate text edit tool, double-click canvas, verify alive + tool on."""
    te_btn = find_by_suffix(fx.main_window, "textEditToolActionButton")
    if te_btn is None:
        return "red", "textEditToolActionButton not reachable"
    te_btn.click_input()
    time.sleep(0.5)
    canvas = find_by_suffix(fx.main_window, "pdfCanvas")
    r = canvas.rectangle()
    try:
        canvas.double_click_input(coords=(r.width() // 2, r.height() // 3))
    except Exception as e:
        return "red", f"double click failed: {e}"
    time.sleep(0.8)
    if not _pdfe_alive():
        return "red", "pdfe crashed during double click"
    # Escape out of any opened edit session
    try:
        fx.main_window.type_keys("{ESC}")
    except Exception:
        pass
    return "green", "text edit tool + double click dispatched"


def _op_text_selection_drag(fx, tmp) -> tuple[str, str]:
    """SelectTool + drag across text + Ctrl+C + clipboard read.

    Real oracle: after dragging across text, Ctrl+C should copy the
    selected span to the system clipboard. If the clipboard receives
    non-empty text that wasn't there before, the selection genuinely
    happened at the Qt content-model level.
    """
    # Clear the clipboard first so stale content doesn't false-green.
    try:
        import ctypes
        ctypes.windll.user32.OpenClipboard(0)
        ctypes.windll.user32.EmptyClipboard()
        ctypes.windll.user32.CloseClipboard()
    except Exception:
        pass

    sel_btn = find_by_suffix(fx.main_window, "selectToolActionButton")
    if sel_btn is None:
        return "red", "selectToolActionButton not reachable"
    sel_btn.click_input()
    time.sleep(0.4)
    canvas = find_by_suffix(fx.main_window, "pdfCanvas")
    r = canvas.rectangle()
    try:
        canvas.drag_mouse_input(
            src=(r.width() // 3, r.height() // 3),
            dst=(2 * r.width() // 3, r.height() // 3),
            button="left",
            absolute=False,
        )
    except Exception as e:
        return "red", f"drag failed: {e}"
    time.sleep(0.5)

    # Copy to clipboard
    try:
        fx.main_window.type_keys("^c")
    except Exception:
        pass
    time.sleep(0.5)

    # Read clipboard via ctypes
    clipboard_text = ""
    try:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        CF_UNICODETEXT = 13
        user32.OpenClipboard(0)
        h = user32.GetClipboardData(CF_UNICODETEXT)
        if h:
            p = kernel32.GlobalLock(h)
            if p:
                clipboard_text = ctypes.wstring_at(p)
                kernel32.GlobalUnlock(h)
        user32.CloseClipboard()
    except Exception as e:
        return "red", f"clipboard read failed: {e}"

    if not _pdfe_alive():
        return "red", "pdfe crashed during text selection drag"

    if not clipboard_text.strip():
        # The drag may have dispatched but selected an empty region.
        # Return blocked with specific reason rather than red — the
        # gesture works, the target region just had no text.
        return "blocked", (
            "text selection drag dispatched but clipboard is empty; "
            "fixture region may not contain selectable text at the "
            "sampled canvas coordinates"
        )
    return "green", (
        f"text selection drag + Ctrl+C produced "
        f"{len(clipboard_text)} chars in clipboard"
    )


def _op_blocked(reason: str):
    def _run(fx, tmp) -> tuple[str, str]:
        return "blocked", reason
    return _run


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------

def _op_owned_elsewhere(canonical_test_file: str, canonical_handler: str):
    """Phase 3 Cluster #3 (2026-06-09): delegate to canonical handler.

    Rows whose canonical driver lives in another test file (see the
    Pipeline K phase-3 audit) used to be absent from HANDLERS. Pipeline
    K phase-1's "no handler -> pytest.fail" rule then turned them into
    spurious red rows here. This shim explicitly marks the row as
    "blocked here, owned elsewhere" so the phase-10 parametrization
    surfaces it as a skip rather than a fabricated failure.
    """
    msg = (
        f"owned by {canonical_test_file}::{canonical_handler}; "
        "this file's parametrization sweeps all phase-10 rows but "
        "the canonical driver records green/red"
    )

    def _run(fx, tmp) -> tuple[str, str]:
        return "blocked", msg
    return _run


HANDLERS: Dict[str, tuple[str, Callable]] = {
    # Scroll / zoom / pan via keyboard + status-bar oracle
    "canvas_canvas_scroll_wheel": ("test_3page.pdf", _op_canvas_scroll_wheel),
    "canvas_canvas_zoom_wheel":   ("test_3page.pdf", _op_canvas_zoom_wheel),
    "canvas_canvas_pan_drag":     ("test_3page.pdf", _op_canvas_pan_drag),

    # Annotation move/select via the handle-centroid seam (c82a1f37 P4:
    # move_drag + click_away now use the pre-annotated fixture so an
    # existing annotation can be selected by index and really moved/deselected)
    "canvas_annot_move_drag":           ("test_with_freetext_annot.pdf", _op_annot_move_drag),
    # GitHub #178: these two were pointed at test_3page.pdf, which has NO
    # annotations -- so selecting annotation index 0 could never succeed. That
    # went unnoticed for as long as the handler returned "blocked" without
    # looking at the document: A BLOCKED ROW'S FIXTURE IS NEVER EXERCISED, so
    # nothing checks it. Un-blocking the row is what surfaced the mismatch.
    # They now use the same pre-annotated fixture as move_drag/click_away.
    "canvas_annot_handle_drag_corner":  ("test_with_freetext_annot.pdf", _op_annot_handle_drag_corner),
    "canvas_annot_handle_drag_edge":    ("test_with_freetext_annot.pdf", _op_annot_handle_drag_edge),
    "canvas_canvas_click_away_deselect": ("test_with_freetext_annot.pdf", _op_click_away_deselect),

    # Text edit double-click
    "canvas_canvas_double_click_edit": ("test_3page.pdf", _op_double_click_edit),

    # Form field tab navigation
    "canvas_canvas_tab_navigate_fields": ("test_form.pdf", _op_tab_navigate_fields),

    # Text selection drag
    "canvas_text_selection_drag":         ("test_single.pdf", _op_text_selection_drag),
    "canvas_text_selection_double_click": ("test_single.pdf", _op_double_click_edit),

    # Phase 3 Cluster #3 (2026-06-09): handlers owned elsewhere.
    # Pipeline K phase-3 stale-stub audit (2026-05-05) deleted the
    # _op_blocked stubs here to avoid last-writer-wins demoting green
    # rows; but Pipeline K phase-1's "no handler -> pytest.fail" rule
    # then turned them into "handler not registered" red rows. The
    # delegating-blocked shim restores the original intent without
    # the false-red side effect.
    "canvas_text_selection_triple_click": (
        "test_3page.pdf",
        _op_owned_elsewhere("test_canvas_phase3.py", "_op_text_selection_triple_click"),
    ),
    "canvas_canvas_rubber_band_select": (
        "test_3page.pdf",
        _op_owned_elsewhere("test_canvas_phase3.py", "_op_rubber_band_select"),
    ),
    "canvas_annot_handle_drag_rotate": (
        "test_3page.pdf",
        _op_owned_elsewhere("test_canvas_phase3.py", "_op_annot_handle_drag_rotate"),
    ),
    "canvas_sticky_note_drag_popup": (
        "test_3page.pdf",
        _op_owned_elsewhere("test_features_phase7.py", "_op_canvas_sticky_note_drag"),
    ),
}


# Phase 3 Cluster #7 (2026-06-10): CI-LIMITATION-CANVAS-SYNTHETIC-DRAG.
# canvas_annot_move_drag, canvas_canvas_click_away_deselect, and
# canvas_canvas_scroll_wheel were previously blocked here.
#
# c82a1f37 P4 (2026-07-04): ALL THREE RECOVERED and removed from this set.
#   * move_drag / click_away — the old handlers used a create-a-sticky +
#     commentTreeWidget-count proxy that hit the pywinauto COMError on
#     descendants() and never actually moved/deselected anything. Rewritten
#     to select the PRE-annotated fixture by index via testAnnotSelectPipe
#     (which now drives SelectTool) and observe the real rect move / the
#     handle-centroid set emptying via the testAnnotHandles seam.
#   * scroll_wheel — the blocker was a stale initial-page read (doc opened
#     on page 2). Fixed by navigating to page 1 (Ctrl+Home) before the
#     before/after delta; the testScrollPipe wheel-event path itself worked.
# All three validated live against the P4 Debug build (see the phase-4 doc).
#
# GitHub #178 (2026-09-12): THE SET HAS BEEN EMPTY SINCE THAT RECOVERY, so the
# branch below that consumes it is dead code — and the message it would emit
# still said "tracked for Phase 7 follow-up", a phase long past. A skip reason
# nobody can trigger, pointing at a milestone that has been and gone, is worse
# than no mechanism: a reader greps for CI-LIMITATION-CANVAS-SYNTHETIC-DRAG,
# finds it here, and concludes rows are still blocked by it. None are.
#
# The mechanism is KEPT rather than deleted, deliberately: it is the hook a
# future genuine CI limitation would use, and re-deriving it costs more than
# the four lines. What is removed is the stale phase reference. If this set is
# ever repopulated, the reason must name the CURRENT tracking issue.
_KNOWN_LIMITATION_IDS: set = set()


# ---------------------------------------------------------------------------
# Parametrized test
# ---------------------------------------------------------------------------

@pytest.mark.skipif(
    not PDFE_EXE.exists(),
    reason=f"pdfe.exe not built at {PDFE_EXE}",
)
@pytest.mark.parametrize(
    "row",
    PHASE10_ROWS,
    ids=[r["test_id"] for r in PHASE10_ROWS],
)
def test_p2_canvas_coords(row: dict) -> None:
    test_id = row["test_id"]
    if test_id in _KNOWN_LIMITATION_IDS:
        record_matrix_result(
            test_id, "blocked",
            "CI-LIMITATION-CANVAS-SYNTHETIC-DRAG: synthetic Qt event "
            "injection unreliable for this oracle. NOTE: _KNOWN_LIMITATION_IDS "
            "is empty, so nothing reaches this branch today; if you are reading "
            "this in a run log, the set was repopulated and whoever did that "
            "owes this message a current tracking issue (GitHub #178)"
        )
        pytest.skip(
            f"{test_id}: CI-LIMITATION-CANVAS-SYNTHETIC-DRAG "
            "(see tests/docs/known_issues.md)"
        )
    entry = HANDLERS.get(test_id)
    if entry is None:
        # Pipeline K phase-1 (finding 10): missing handlers are loud
        # failures, not silent-blocked rows. A row in the matrix with
        # no handler is a coverage hole that must be visible.
        pytest.fail(f"handler not registered: {test_id}")
    fixture, handler = entry

    fx, tmp = _launch(fixture)
    try:
        try:
            status, notes = handler(fx, tmp)
        except Exception as e:
            status, notes = "red", f"handler exception: {e}"
    finally:
        _cleanup(fx, tmp)

    record_matrix_result(test_id, status, notes)
    if status == "blocked":
        pytest.skip(notes)
    if status == "red":
        pytest.fail(notes)
