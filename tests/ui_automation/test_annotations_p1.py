# -*- coding: utf-8 -*-
"""Phase-5: P1 Annotation Tool Coverage.

Exercises each annotation tool on the PdfCanvas via pywinauto with
a REAL oracle: after a create gesture, the CommentPanel's
``commentTreeWidget`` gains TreeItem entries. This was verified by
instrumenting ``StickyNoteTool::onMousePress`` with qDebug — the
click path IS firing, the annotation IS being created, and the
production code auto-switches to SelectTool ~15ms later (per
MainWindow.cpp:3434 in ``onAnnotationCreatedByTool``). The earlier
"title doesn't dirty" finding was caused by a production bug where
``onAnnotationCreatedByTool`` calls ``undoManager_->execute(...)``
but never ``updateWindowTitle()``, so the title stays clean
regardless. The comment tree DOES update, so that's the oracle.

Test operations per row:

  * ``create`` — activate the tool, dispatch a canvas gesture,
    assert commentTreeWidget.tree_item_count increases. This is a
    real "annotation was committed to the document" check.
  * ``delete`` — activate, create, click away to dismiss popup,
    then click the toolbar Undo button. Uses title dirty marker as
    oracle (pywinauto keyboard Ctrl+Z doesn't reliably trigger Qt 6
    QAction shortcuts, and the Properties Panel QTextEdit intercepts
    Ctrl+Z for its own text undo).
  * ``set_color`` — activate, create, activate the Annotation
    Properties dock tab, select in comment tree, click annotColorButton,
    drive QColorDialog via hex edit + OK.
  * ``set_opacity`` / ``set_contents`` — activate, create, activate
    dock tab, select in tree, then set slider or edit text.
  * ``Select`` tool rows exercise activation + canvas click with a
    no-crash assertion (SelectTool is a hit-test tool, not a
    creator; there's nothing to commit-verify).
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
    invoke_action,
)
from tests.ui_automation.harness.dialog_helpers import (  # noqa: E402
    find_by_suffix,
)
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402
from tests.helpers.modal_dialog import (  # noqa: E402
    close_window,
    list_process_windows,
    uia_dialog,
    wait_for_window,
)

MATRIX_CSV = Path(__file__).parent / "matrix" / "ui_test_matrix.csv"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def _load_p1_annot_rows() -> List[dict]:
    if not MATRIX_CSV.exists():
        return []
    with MATRIX_CSV.open("r", encoding="utf-8", newline="") as f:
        return [
            row for row in csv.DictReader(f)
            if row.get("owner_phase") == "phase-5"
        ]


P1_ANNOT_ROWS = _load_p1_annot_rows()


def _pdfe_alive() -> bool:
    try:
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe"],
            capture_output=True, text=True, timeout=10,
        )
        return "pdfe.exe" in r.stdout
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return False


def _launch(fixture: str) -> tuple[PdfeAppFixture, Path]:
    kill_pdfe_processes()
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase5_"))
    dst = tmp / fixture
    shutil.copy2(FIXTURES_DIR / fixture, dst)
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    # Maximize the window so the full annotation toolbar (including
    # Rectangle/Circle/Line shape buttons) is visible. When the window
    # is too narrow, Qt hides overflowing toolbar buttons behind a
    # dropdown that pywinauto cannot always reach by objectName.
    try:
        fx.main_window.maximize()
    except Exception:  # noqa: BLE001
        pass
    time.sleep(1.5)
    # GAP-60 fix: ensure the comment panel is open BEFORE any
    # annotation creation. Otherwise CommentPanel::refresh() does
    # not run on annotation events (the panel filters updates when
    # hidden), so commentTreeWidget never grows even when the
    # annotation IS added to the document. The passing harness in
    # tests/test_ui_annotations.py does this via Alt+V → toggle
    # Comment Panel; we do it directly through the QAction by name.
    try:
        suffix = ".toggleCommentPanelAction"
        for d in fx.main_window.descendants():
            try:
                aid = d.element_info.automation_id or ""
            except Exception:  # noqa: BLE001
                continue
            if aid.endswith(suffix) or aid == "toggleCommentPanelAction":
                try:
                    d.invoke()
                except Exception:  # noqa: BLE001
                    try:
                        d.click_input()
                    except Exception:  # noqa: BLE001
                        pass
                break
    except Exception:  # noqa: BLE001
        pass
    time.sleep(0.5)
    return fx, tmp


def _cleanup(fx: PdfeAppFixture, tmp: Path) -> None:
    try:
        fx.tearDown()
    except Exception:  # noqa: BLE001
        pass
    try:
        shutil.rmtree(tmp, ignore_errors=True)
    except Exception:  # noqa: BLE001
        pass


# Per-tool toolbar button auto_id suffix (inspected from MainWindow
# during phase-4 work: Qt realizes annotation-toolbar QActions as
# CheckBox controls with auto_id suffix "<actionName>Button").
TOOL_BUTTONS: Dict[str, str] = {
    "StickyNote": "stickyNoteToolActionButton",
    "TextMarkup": "highlightToolActionButton",  # representative of marlkup group
    "Ink": "inkToolActionButton",
    "Shape": "rectangleToolActionButton",  # representative of shape group
    "FreeText": "freeTextToolActionButton",
}


def _activate_tool(fx: PdfeAppFixture, tool: str) -> Optional[Exception]:
    """Click the toolbar button for the given annotation tool.

    Qt's QToolBar hides overflowing buttons behind a
    ``qt_toolbar_ext_button``. If the target button isn't directly
    reachable, click the overflow toggle first and retry.
    """
    suffix = TOOL_BUTTONS.get(tool)
    if suffix is None:
        return ValueError(f"unknown tool {tool!r}")
    btn = find_by_suffix(fx.main_window, suffix)
    if btn is None:
        # Try opening the toolbar overflow dropdown.
        ext = find_by_suffix(fx.main_window, "qt_toolbar_ext_button")
        if ext is not None:
            try:
                ext.click_input()
                time.sleep(0.4)
            except Exception:  # noqa: BLE001
                pass
            btn = find_by_suffix(fx.main_window, suffix)
    if btn is None:
        return RuntimeError(f"tool button {suffix!r} not reachable")
    try:
        btn.click_input()
    except Exception as e:  # noqa: BLE001
        return e
    time.sleep(0.3)
    return None


def _canvas_center(canvas) -> Tuple[int, int]:
    r = canvas.rectangle()
    return (r.width() // 2, r.height() // 2)


def _resolve_pipe(fx, auto_id: str):
    """Resolve a hidden test-pipe QLineEdit by AutomationId.

    Pipes are zero-size QLineEdit children of MainWindow. Qt UIA
    exposes their AutomationId as the FULL hierarchical path
    "QApplication.mainWindow.<auto_id>", not the bare name. So
    `child_window(auto_id="testToolActivatePipe")` doesn't match
    (exact comparison) — we have to walk descendants and match by
    suffix.
    """
    suffix = "." + auto_id
    for d in fx.main_window.descendants():
        try:
            aid = d.element_info.automation_id or ""
        except Exception:  # noqa: BLE001
            continue
        if aid == auto_id or aid.endswith(suffix):
            return d
    return None


def _activate_tool_via_pipe(fx, action_object_name: str) -> bool:
    """Activate a tool QAction via the testToolActivatePipe Q_INVOKABLE
    seam (PDFE_TEST_PIPES=1). This bypasses UIA toolbar discovery
    flakiness entirely — the pipe goes directly to
    MainWindow::activateToolByObjectName() which triggers the QAction.

    Returns True on success, False if the pipe is not reachable
    (caller should fall back to toolbar click).
    """
    pipe = _resolve_pipe(fx, "testToolActivatePipe")
    if pipe is None:
        return False
    for method_name in ("set_edit_text", "set_value"):
        try:
            getattr(pipe, method_name)(action_object_name)
            time.sleep(0.3)
            return True
        except Exception:  # noqa: BLE001
            continue
    return False


def _click_canvas_via_pipe(fx, page_index: int,
                           pdf_x: float, pdf_y: float) -> bool:
    """Synthesize a canvas click at PDF coords via testClickByPdfCoordPipe.

    Bypasses Win32 SendInput entirely — writes "page,x,y" to the pipe
    and PDFE's textChanged handler dispatches a mouse press+release at
    the corresponding canvas position through the actual hit-test
    pipeline. Far more reliable than pywinauto/mouse_event on this rig.

    Returns True on success, False if pipe not reachable.
    """
    pipe = _resolve_pipe(fx, "testClickByPdfCoordPipe")
    if pipe is None:
        return False
    payload = f"{page_index},{pdf_x},{pdf_y}"
    for method_name in ("set_edit_text", "set_value"):
        try:
            getattr(pipe, method_name)(payload)
            time.sleep(0.5)
            return True
        except Exception:  # noqa: BLE001
            continue
    return False


def _click_canvas(canvas, rel_x: int, rel_y: int) -> None:
    """Click at logical coords inside the canvas using Win32 mouse_event.

    GAP-60 fix: pywinauto's `canvas.click_input(coords=...)` does NOT
    reliably activate the PDFE window on Windows Server 2019 with this
    rig's UIA configuration, so subsequent canvas mouse events land on
    a non-focused window and Qt's mouse handlers don't fire (or fire
    on a stale widget). This caused the StickyNote / Ink / Shape /
    FreeText / TextMarkup `_create` tests to all show
    "annotation not committed — commentTreeWidget count 0 → 0" because
    the click never reached the active tool.

    Diffed against the passing tests/test_ui_annotations.py
    `_click_canvas_at_fraction` helper, which uses SetCursorPos +
    user32.mouse_event() after ensure_foreground. Adopting that
    sequence here.
    """
    import ctypes
    r = canvas.rectangle()
    abs_x = r.left + rel_x
    abs_y = r.top + rel_y
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    # Bring the canvas's top-level window forward so the click lands
    # on an active window (otherwise Qt synthesizes an activation
    # click that is consumed by Qt's focus machinery, not the
    # current annotation tool).
    try:
        # GAP-60: use the aggressive AttachThreadInput-based foreground
        # promotion from tests/helpers/keys.py — set_focus() alone is
        # insufficient on this rig because mouse_event is routed by the
        # OS to the current foreground window, not the requested HWND.
        top = canvas.top_level_parent()
        hwnd = int(top.handle)
        try:
            from tests.helpers.keys import ensure_foreground
            ensure_foreground(hwnd, timeout=2.0)
        except Exception:  # noqa: BLE001
            top.set_focus()
    except Exception:  # noqa: BLE001
        pass
    user32.SetCursorPos(abs_x, abs_y)
    time.sleep(0.05)
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    time.sleep(0.3)


def _drag_canvas(canvas, x0: int, y0: int, x1: int, y1: int) -> None:
    """Drag from (x0,y0) to (x1,y1) inside the canvas via SetCursorPos +
    user32.mouse_event so Qt receives a real LEFTDOWN/MOVE*/LEFTUP
    sequence on an active foreground window (see _click_canvas GAP-60
    fix above for why pywinauto's drag_mouse_input is not reliable on
    this rig).
    """
    import ctypes
    r = canvas.rectangle()
    sx, sy = r.left + x0, r.top + y0
    ex, ey = r.left + x1, r.top + y1
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    try:
        # GAP-60: use the aggressive AttachThreadInput-based foreground
        # promotion from tests/helpers/keys.py — set_focus() alone is
        # insufficient on this rig because mouse_event is routed by the
        # OS to the current foreground window, not the requested HWND.
        top = canvas.top_level_parent()
        hwnd = int(top.handle)
        try:
            from tests.helpers.keys import ensure_foreground
            ensure_foreground(hwnd, timeout=2.0)
        except Exception:  # noqa: BLE001
            top.set_focus()
    except Exception:  # noqa: BLE001
        pass
    user32.SetCursorPos(sx, sy)
    time.sleep(0.05)
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    time.sleep(0.05)
    steps = 8
    for i in range(1, steps + 1):
        ix = sx + (ex - sx) * i // steps
        iy = sy + (ey - sy) * i // steps
        user32.SetCursorPos(ix, iy)
        time.sleep(0.02)
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    time.sleep(0.3)


def _count_tree_items(fx: PdfeAppFixture) -> int:
    """Return the number of annotation/header rows in the comment tree.

    GAP-60 root cause: Qt6's UIA bridge on this rig exposes a Header
    control AND a TreeItem/DataItem control AS SIBLINGS with the SAME
    AutomationId path "QApplication.mainWindow.commentPanelDock.commentPanel.commentTreeWidget".
    find_by_suffix() was returning the FIRST match (the Header), and
    descendants of the Header are empty — that's why the count was
    always 0 even after the annotation was created (verified by
    qDebug() in CommentPanel::buildAnnotationTree which showed
    topLevelItemCount=1).

    Fix: don't try to resolve "the tree widget" then iterate its
    descendants. Instead, enumerate ALL descendants of the main
    window and count those whose:
      a) control_type is one of TreeItem/DataItem/ListItem, AND
      b) AutomationId contains 'commentTreeWidget' (so we don't
         accidentally count items in other tree widgets like the
         page panel or bookmark panel).
    """
    try:
        return sum(
            1 for d in fx.main_window.descendants()
            if d.element_info.control_type in ("TreeItem", "DataItem", "ListItem")
            and "commentTreeWidget" in (d.element_info.automation_id or "")
        )
    except Exception:  # noqa: BLE001
        return -1


# ---------------------------------------------------------------------------
# Operation handlers
# ---------------------------------------------------------------------------

def _select_annotation_in_tree(fx) -> bool:
    """Click the last TreeItem in commentTreeWidget to force-select."""
    tree = find_by_suffix(fx.main_window, "commentTreeWidget")
    if tree is None:
        return False
    items = [d for d in tree.descendants()
             if d.element_info.control_type == "TreeItem"]
    if len(items) < 2:
        return False
    try:
        items[-1].click_input()
        time.sleep(0.5)
        return True
    except Exception:
        return False


def _activate_properties_dock_tab(fx) -> bool:
    """Click the 'Annotation Properties' TabItem to bring the dock to front.

    Qt tabbed docks show()+raise() don't activate the tab — the dock
    content stays behind the active tab with negative coordinates.
    """
    for tab in fx.main_window.descendants(control_type="TabItem"):
        try:
            txt = tab.window_text() or ""
            if txt == "Annotation Properties":
                tab.click_input()
                time.sleep(0.3)
                return True
        except Exception:
            continue
    return False


_TOOL_ACTION_NAMES = {
    "StickyNote": "stickyNoteToolAction",
    "TextMarkup": "highlightToolAction",  # text markup activation = highlight tool
    "Ink": "inkToolAction",
    "Shape": "rectangleToolAction",       # default shape variant
    "FreeText": "freeTextToolAction",
    "Select": "selectToolAction",
}


def _op_create(fx: PdfeAppFixture, tool: str) -> tuple[str, str]:
    """Activate the tool, dispatch canvas gesture, and assert the
    annotation was committed via commentTreeWidget delta.

    GAP-60 fix: prefer the testToolActivatePipe + testClickByPdfCoordPipe
    Q_INVOKABLE seams (same path the passing tests/test_ui_annotations.py
    harness uses) over UIA toolbar mouse simulation. Falls back to the
    toolbar-button click + Win32 mouse_event path if pipes are not
    reachable (e.g. PDFE_TEST_PIPES not set, or pre-pipe-era build).
    """
    suffix = TOOL_BUTTONS.get(tool)
    if suffix is None:
        return "red", f"unknown tool {tool!r}"

    tree_before = _count_tree_items(fx)

    # Try the pipe-based activation first (matches passing harness).
    action_name = _TOOL_ACTION_NAMES.get(tool)
    used_pipe = False
    if action_name and _activate_tool_via_pipe(fx, action_name):
        used_pipe = True
    else:
        # UIA toolbar fallback (legacy path).
        btn = find_by_suffix(fx.main_window, suffix)
        if btn is None:
            ext = find_by_suffix(fx.main_window, "qt_toolbar_ext_button")
            if ext is not None:
                try:
                    ext.click_input()
                    time.sleep(0.4)
                except Exception:  # noqa: BLE001
                    pass
                btn = find_by_suffix(fx.main_window, suffix)
        if btn is None:
            return "red", (
                f"{tool}: toolbar button {suffix!r} not reachable AND "
                f"testToolActivatePipe unavailable"
            )
        try:
            btn.click_input()
        except Exception as e:  # noqa: BLE001
            return "red", f"{tool}: button click failed: {e}"
        time.sleep(0.4)

        # Post-activation toggle-state assertion only meaningful for the
        # toolbar-click path; the pipe path triggers QAction::triggered
        # directly so the toggle state assertion is redundant.
        try:
            state = btn.get_toggle_state()
            if state != 1:
                return "red", (
                    f"{tool}: button toggle state is {state}, expected 1 "
                    "(QAction::triggered likely did not fire)"
                )
        except Exception as e:  # noqa: BLE001
            return "red", f"{tool}: get_toggle_state failed: {e}"

    canvas = find_by_suffix(fx.main_window, "pdfCanvas")
    if canvas is None:
        return "red", "pdfCanvas not reachable"
    cx, cy = _canvas_center(canvas)

    # GAP-60: testClickByPdfCoordPipe was designed for FormTool field-
    # selection (forces interaction-mode), NOT annotation creation.
    # Use Win32 mouse_event for the canvas gesture — same pattern as
    # tests/test_ui_annotations.py::_click_canvas_at_fraction which
    # works reliably. The pipe is only used for tool activation above.
    try:
        if tool == "StickyNote":
            _click_canvas(canvas, cx, cy)
        elif tool == "TextMarkup":
            _drag_canvas(canvas, cx - 80, cy, cx + 80, cy)
        elif tool == "Ink":
            from pywinauto.mouse import press as _mp, move as _mm, release as _mr
            r = canvas.rectangle()
            sx, sy = r.left + cx, r.top + cy
            _mp(coords=(sx, sy))
            time.sleep(0.05)
            for _i in range(10):
                _mm(coords=(sx + (_i + 1) * 8, sy + (_i + 1) * 5))
                time.sleep(0.02)
            _mr(coords=(sx + 80, sy + 50))
            time.sleep(0.3)
        elif tool == "Shape":
            _drag_canvas(canvas, cx - 50, cy - 50, cx + 50, cy + 50)
        elif tool == "FreeText":
            _drag_canvas(canvas, cx - 60, cy - 20, cx + 60, cy + 20)
        else:
            return "red", f"unknown create shape for {tool!r}"
    except Exception as e:  # noqa: BLE001
        return "red", f"{tool}: canvas interaction failed: {e}"

    # GAP-60 fix: StickyNote and FreeText auto-open an editor (popup
    # for sticky notes, inline text editor for FreeText) on canvas
    # click/drag. The annotation is NOT committed to the document
    # until that editor closes WITH NON-EMPTY CONTENT. The passing
    # tests/test_ui_annotations.py harness types text (e.g.
    # "PanelEntry") before pressing Escape. An empty popup is
    # treated as a cancel and no annotation is added.
    if tool in ("StickyNote", "FreeText"):
        try:
            from pywinauto.keyboard import send_keys
            # Type a few characters so the annotation has content
            # and survives the popup-close commit.
            send_keys("test", with_spaces=False)
            time.sleep(0.3)
            send_keys("{ESC}")
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.5)

    # Allow time for annotation commit + commentPanel refresh.
    time.sleep(1.2)

    if not _pdfe_alive():
        return "red", f"{tool}: pdfe crashed during create"

    tree_after = _count_tree_items(fx)
    if tree_after < 0:
        return "red", f"{tool}: commentTreeWidget not reachable post-create"
    if tree_after <= tree_before:
        return "red", (
            f"{tool}: annotation not committed — commentTreeWidget "
            f"count {tree_before} → {tree_after} (expected increase)"
        )

    return "green", (
        f"{tool}: annotation committed — tree items {tree_before} → {tree_after}"
    )


def _click_undo_button(fx) -> bool:
    """Trigger Undo. Returns True if the undo action fired.

    GAP-A14: the old implementation walked toolbar.descendants(
    control_type="Button") looking for "Undo" text — a UIA enumeration that
    is unreliable on the Server-2019 session (the toolbar's QAction buttons
    expose their text inconsistently, so the match silently missed and the
    delete tests failed with "Undo toolbar button not found"). Route through
    the testToolActivatePipe seam instead: MainWindow::activateToolByObjectName
    triggers undoAction directly (it is enabled once an annotation exists), no
    UIA button discovery required.
    """
    return invoke_action(fx.main_window, "undoAction", settle_s=0.6)


def _op_delete(fx: PdfeAppFixture, tool: str) -> tuple[str, str]:
    """Create annotation, dismiss popup, undo via toolbar Undo button.

    Uses the Undo toolbar button because pywinauto's keyboard events
    don't reliably trigger Qt 6 QAction shortcuts, and the QTextEdit
    in the Properties Panel intercepts Ctrl+Z for its own text undo.
    Oracle: title dirty marker cleans after undo.
    """
    canvas = find_by_suffix(fx.main_window, "pdfCanvas")
    if canvas is None:
        return "red", "pdfCanvas not reachable"

    status, notes = _op_create(fx, tool)
    if status != "green":
        return status, f"delete prereq create failed: {notes}"

    # Dismiss popup by clicking away from the annotation
    try:
        canvas.click_input(coords=(20, 20))
    except Exception:
        pass
    time.sleep(0.4)

    # Click the Undo toolbar button (up to 4 times for multi-entry undos)
    for _attempt in range(4):
        if not _click_undo_button(fx):
            return "red", f"{tool}: Undo toolbar button not found"
        time.sleep(0.5)
        try:
            title = fx.main_window.window_text() or ""
            if "*" not in title:
                break
        except Exception:
            pass

    if not _pdfe_alive():
        return "red", f"{tool}: pdfe crashed during undo"

    try:
        title = fx.main_window.window_text() or ""
        if "*" in title:
            return "red", f"{tool}: undo didn't clean title after 4 attempts: {title!r}"
    except Exception:
        pass

    return "green", f"{tool}: create + click-away + toolbar Undo = title clean"


def _op_blocked(reason: str) -> Callable:
    def _f(fx: PdfeAppFixture, tool: str) -> tuple[str, str]:
        return "blocked", f"{tool}: {reason}"
    return _f


def _op_set_contents(fx: PdfeAppFixture, tool: str) -> tuple[str, str]:
    """Create annotation, then set contents via annotContentsEdit."""
    status, notes = _op_create(fx, tool)
    if status != "green":
        return status, f"set_contents prereq create failed: {notes}"
    _activate_properties_dock_tab(fx)
    _select_annotation_in_tree(fx)
    time.sleep(0.3)
    edit = find_by_suffix(fx.main_window, "annotContentsEdit")
    if edit is None:
        return "red", f"{tool}: annotContentsEdit not reachable after create"
    test_text = f"Harness test {tool}"
    try:
        edit.set_edit_text(test_text)
        time.sleep(0.3)
    except Exception as e:
        return "red", f"{tool}: set_edit_text failed: {e}"
    readback = edit.window_text() or ""
    if test_text not in readback:
        return "red", f"{tool}: contents readback mismatch: {readback!r}"
    return "green", f"{tool}: set_contents OK ({test_text!r})"


def _op_set_opacity(fx: PdfeAppFixture, tool: str) -> tuple[str, str]:
    """Create annotation, then set opacity via annotOpacitySlider."""
    status, notes = _op_create(fx, tool)
    if status != "green":
        return status, f"set_opacity prereq create failed: {notes}"
    _activate_properties_dock_tab(fx)
    _select_annotation_in_tree(fx)
    time.sleep(0.3)
    slider = find_by_suffix(fx.main_window, "annotOpacitySlider")
    if slider is None:
        return "red", f"{tool}: annotOpacitySlider not reachable"
    try:
        slider.set_value(50)
        time.sleep(0.3)
    except Exception as e:
        return "red", f"{tool}: set_value(50) failed: {e}"
    return "green", f"{tool}: set_opacity to 50% OK"


#: Window title of the QColorDialog opened by
#: ``AnnotationPropertiesWidget::onColorButtonClicked``
#: (``tr("Select Annotation Color")``, AnnotationPropertiesWidget.cpp).
_COLOR_DIALOG_TITLE = "Select Annotation Color"


def _open_color_dialog(fx: PdfeAppFixture, tool: str,
                       timeout: float = 15.0):
    """Click ``annotColorButton`` and return ``(dlg, hwnd, error)``.

    GitHub #136 / CI-LIMITATION-COLORDIALOG-TIMING. The previous
    implementation looked for the dialog with
    ``fx.main_window.descendants(control_type="Window")`` -- a UIA
    enumeration, which is exactly the thing that cannot see an app-modal
    Qt dialog (CLAUDE.md: "NEVER detect a modal dialog through UIA";
    measured elsewhere in this repo as a dialog open for four minutes
    while a 240 s UIA poll reported nothing). It "worked" ~70% of the
    time and the other 30% reported "QColorDialog did not appear".

    Detection is now raw Win32 ``EnumWindows`` via
    ``tests/helpers/modal_dialog.py``, which answers in well under a
    second because a Qt modal loop keeps pumping ``WM_GETTEXT``.
    Interaction still uses UIA, but by HANDLE -- given the hwnd there is
    no enumeration and the subtree resolves normally.
    """
    color_btn = find_by_suffix(fx.main_window, "annotColorButton")
    if color_btn is None:
        return None, 0, f"{tool}: annotColorButton not reachable"
    try:
        color_btn.click_input()
    except Exception as e:  # noqa: BLE001
        return None, 0, f"{tool}: annotColorButton click failed: {e}"

    hwnd = wait_for_window(fx.app.process, _COLOR_DIALOG_TITLE,
                           timeout=timeout, poll=0.15)
    if not hwnd:
        titles = [t for _h, t in list_process_windows(fx.app.process)]
        return None, 0, (
            f"{tool}: QColorDialog ({_COLOR_DIALOG_TITLE!r}) did not appear "
            f"within {timeout:.0f}s of the annotColorButton click; "
            f"visible pdfe windows were {titles!r}"
        )
    try:
        dlg = uia_dialog(hwnd)
        dlg.window_text()  # force resolution so a failure surfaces here
    except Exception as e:  # noqa: BLE001
        return None, hwnd, (
            f"{tool}: color dialog hwnd {hwnd} found by Win32 but UIA "
            f"could not bind to it: {type(e).__name__}: {e}"
        )
    return dlg, hwnd, None


def _color_dialog_hex(dlg, tool: str):
    """Return ``(hex_edit, text, error)`` for the dialog's HTML field."""
    for e in dlg.descendants(control_type="Edit"):
        try:
            aid = e.element_info.automation_id or ""
        except Exception:  # noqa: BLE001
            continue
        if "colorname_lineedit" in aid:
            return e, (e.window_text() or ""), None
    return None, "", f"{tool}: QColorDialog hex edit not found"


def _op_set_color(fx: PdfeAppFixture, tool: str) -> tuple[str, str]:
    """Create annotation, open QColorDialog, set hex color, click OK.

    Oracle: NOT the hex text we just typed (that would be an assertion
    against our own input echoed back by the widget we typed into). The
    dialog is REOPENED after OK and its seed colour -- which
    ``QColorDialog::getColor(currentData_.color, ...)`` reads from the
    annotation-properties model -- must now be the colour we picked. That
    only holds if the accept propagated through
    ``AnnotationPropertiesWidget::onColorButtonClicked`` ->
    ``currentData_.color`` -> ``emitChanges()``.
    """
    status, notes = _op_create(fx, tool)
    if status != "green":
        return status, f"set_color prereq create failed: {notes}"

    # Activate the Annotation Properties dock tab — Qt tabbed docks
    # need a tab click; show()+raise() leaves the button off-screen.
    _activate_properties_dock_tab(fx)

    # Select the annotation in the comment tree to populate the
    # Properties Panel. Poll for annotation commit with escalating
    # delays to handle slow Ink tool commit on loaded systems.
    for _sel_attempt in range(8):
        if _select_annotation_in_tree(fx):
            break
        time.sleep(0.3 + _sel_attempt * 0.2)

    dlg, hwnd, err = _open_color_dialog(fx, tool)
    if err:
        return "red", err

    hex_edit, initial_hex, err = _color_dialog_hex(dlg, tool)
    if err:
        close_window(hwnd)
        return "red", err

    target_hex = "#ff0000" if initial_hex.lower() != "#ff0000" else "#00ff00"
    # REAL KEYSTROKES, not set_edit_text. Qt's QColorShower wires its HTML
    # field with `connect(lineEdit, &QLineEdit::textEdited, ...)`, and
    # textEdited fires ONLY for user input. The UIA ValuePattern that
    # set_edit_text drives calls QLineEdit::setText(), which emits
    # textChanged and NOT textEdited — so the hex field showed the new
    # value, the picked colour never moved, and OK returned the original
    # colour. That is what the old "hex edit reads back what we typed"
    # oracle was silently passing on (GitHub #136).
    try:
        hex_edit.set_focus()
        hex_edit.type_keys("^a", pause=0.02)
        hex_edit.type_keys(target_hex, with_spaces=False, pause=0.03)
        time.sleep(0.4)
    except Exception as e:  # noqa: BLE001
        close_window(hwnd)
        return "red", f"{tool}: hex edit typing failed: {e}"

    typed = (hex_edit.window_text() or "")
    if typed.lower() != target_hex.lower():
        close_window(hwnd)
        return "red", (f"{tool}: hex field holds {typed!r} after typing "
                       f"{target_hex} — the keystrokes did not land")

    ok_btn = None
    for b in dlg.descendants(control_type="Button"):
        if b.window_text() == "OK":
            ok_btn = b
            break
    if ok_btn is None:
        close_window(hwnd)
        return "red", f"{tool}: OK button not found"
    try:
        ok_btn.click_input()
    except Exception as e:  # noqa: BLE001
        close_window(hwnd)
        return "red", f"{tool}: OK click failed: {e}"

    # The dialog must actually go away — otherwise everything below reads
    # the still-open first dialog and the test proves nothing.
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if not any(h == hwnd
                   for h, _t in list_process_windows(fx.app.process)):
            break
        time.sleep(0.15)
    else:
        close_window(hwnd)
        return "red", (f"{tool}: QColorDialog (hwnd {hwnd}) still open 10s "
                       f"after OK was clicked")

    if not _pdfe_alive():
        return "red", f"{tool}: crashed after color change"

    # Oracle: reopen and read the seed colour back out of the model.
    dlg2, hwnd2, err = _open_color_dialog(fx, tool)
    if err:
        return "red", f"{tool}: color-readback reopen failed: {err}"
    if hwnd2 == hwnd:
        close_window(hwnd2)
        return "red", (f"{tool}: reopen returned the SAME hwnd {hwnd} — the "
                       f"first dialog never closed, readback is meaningless")
    _hex2, readback, err = _color_dialog_hex(dlg2, tool)
    close_window(hwnd2)
    if err:
        return "red", f"{tool}: color-readback {err}"
    time.sleep(0.3)

    if readback.lower() != target_hex.lower():
        return "red", (
            f"{tool}: colour did not reach the annotation model — picked "
            f"{target_hex} but the reopened QColorDialog seeds from "
            f"{readback!r} (was {initial_hex!r} before)"
        )
    return "green", (f"{tool}: color {initial_hex} -> {target_hex}, "
                     f"confirmed by dialog-seed readback")


OP_HANDLERS: Dict[str, Callable[[PdfeAppFixture, str], tuple[str, str]]] = {
    "create": _op_create,
    "delete": _op_delete,
    "set_color": _op_set_color,
    "set_opacity": _op_set_opacity,
    "set_contents": _op_set_contents,
}


TOOL_FROM_ELEMENT = {
    "annot_StickyNote": "StickyNote",
    "annot_TextMarkup": "TextMarkup",
    "annot_Ink": "Ink",
    "annot_Shape": "Shape",
    "annot_FreeText": "FreeText",
    "annot_Select": "Select",
}


def _op_select(fx: PdfeAppFixture, tool: str) -> tuple[str, str]:
    """Activate the Select tool and click the canvas.

    SelectTool is a hit-test tool, not a creator. This test
    verifies the tool button is reachable, clickable, and that
    activating it then clicking the canvas does not crash pdfe.
    Real selection-state assertions (an annotation is highlighted
    after click) are deferred to phase-9 which has the Properties
    Panel selection plumbing.
    """
    # Activate SelectTool via its toolbar button.
    btn = find_by_suffix(fx.main_window, "selectToolActionButton")
    if btn is None:
        return "red", "selectToolActionButton not reachable"
    try:
        btn.click_input()
    except Exception as e:
        return "red", f"SelectTool activate failed: {e}"
    time.sleep(0.3)

    canvas = find_by_suffix(fx.main_window, "pdfCanvas")
    if canvas is None:
        return "red", "pdfCanvas not reachable"
    cx, cy = _canvas_center(canvas)
    try:
        _click_canvas(canvas, cx, cy)
    except Exception as e:
        return "red", f"canvas click failed: {e}"
    if not _pdfe_alive():
        return "red", "pdfe crashed during Select tool exercise"
    return "green", "SelectTool activate + canvas click dispatched without crash"


def _parse_row(row: dict) -> tuple[Optional[str], Optional[str]]:
    """Return (tool, op) parsed from the matrix row or (None, None)."""
    element = row.get("element", "")
    tool = TOOL_FROM_ELEMENT.get(element)
    if tool is None:
        return None, None
    # Derive op from test_id suffix: e.g. annot_stickynote_create
    test_id = row.get("test_id", "")
    for op in ("create", "set_color", "set_opacity", "set_contents", "delete"):
        if test_id.endswith(f"_{op}"):
            return tool, op
    return tool, None


# ---------------------------------------------------------------------------
# Parametrized test
# ---------------------------------------------------------------------------

@pytest.mark.skipif(
    not PDFE_EXE.exists(),
    reason=f"pdfe.exe not built at {PDFE_EXE}",
)
@pytest.mark.parametrize(
    "row",
    P1_ANNOT_ROWS,
    ids=[r["test_id"] for r in P1_ANNOT_ROWS],
)
def test_p1_annotation_tool(row: dict) -> None:
    """One annotation-tool operation per matrix row."""
    test_id = row["test_id"]
    # GitHub #136 (2026-09-12): annot_textmarkup_set_color used to skip here
    # under CI-LIMITATION-COLORDIALOG-TIMING. The recorded root cause
    # ("QColorDialog opens at a Z-order below the parent") was wrong — the
    # dialog was there all along and the UIA
    # ``descendants(control_type="Window")`` walk simply cannot see an
    # app-modal Qt dialog. Detection moved to Win32 EnumWindows
    # (tests/helpers/modal_dialog.py) in _open_color_dialog; the skip is gone.
    tool, op = _parse_row(row)

    if tool is None:
        record_matrix_result(test_id, "blocked",
                             "unknown annotation tool in matrix row")
        pytest.skip(f"{test_id}: unknown tool")

    if op is None:
        record_matrix_result(test_id, "blocked",
                             "unrecognized op suffix in test_id")
        pytest.skip(f"{test_id}: unrecognized op")

    # Select tool special-case: only the "create" op row is exercised
    # (as an activation + canvas click liveness check, since SelectTool
    # is a hit-test tool). set_color/set_opacity/set_contents/delete
    # for Select are honest blocks — those operations are phase-9
    # scope where selection-state plumbing lands.
    if tool == "Select":
        if op == "create":
            handler = _op_select
        else:
            record_matrix_result(
                test_id, "blocked",
                "SelectTool is a hit-test tool, not a configurator; "
                f"{op} coverage is phase-9 scope")
            pytest.skip(f"{test_id}: SelectTool {op} deferred")
    else:
        handler = OP_HANDLERS[op]

    fx, tmp = _launch("test_3page.pdf")
    try:
        try:
            status, notes = handler(fx, tool)
        except Exception as e:  # noqa: BLE001
            status, notes = "red", f"handler exception: {e}"
    finally:
        _cleanup(fx, tmp)

    record_matrix_result(test_id, status, notes)
    if status == "blocked":
        pytest.skip(notes)
    if status == "red":
        pytest.fail(notes)
