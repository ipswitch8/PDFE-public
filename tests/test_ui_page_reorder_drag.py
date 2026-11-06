# -*- coding: utf-8 -*-
"""GitHub #326 — the REAL within-file page-reorder drag must change the
DOCUMENT order, not merely move a thumbnail icon.

The defect: PagePanel's QListWidget was configured with a non-Static
``movement()`` (Snap). QListView then handled the drag gesture itself —
repositioning the icon's rect — and never fell through to the drag-drop
subsystem, so ``PageListWidget::dropEvent`` → ``onPageDropped`` →
``pageMoved`` → ``MovePageCommand`` → ``Document::reorderPages`` never ran.
The thumbnail moved; the document order did not.

THIS TEST DRIVES THE REAL GESTURE. It performs a genuine Win32 SendInput
mouse drag of one thumbnail onto another within the Pages panel (NOT the
``movePageProgrammatic`` test bypass — see PagePanel.h:48-54 / KNOWN_WEAK
P10-INFRA-1), then asserts the END STATE: the Document's page order (read
through the live view-state seam, which reports each page's /Rotate) has
actually changed into a permutation of the original, and the document is
dirty. Before the fix those two facts stay false after the drag; that is the
defect signature this pins.

Project rules honoured (see C:/src/PDFE/CLAUDE.md "Writing a UI test"):
  * No UIA modal-dialog detection — this flow opens no modal.
  * No guessed canvas coordinates — the drag endpoints come from the
    QListWidget items' own screen rectangles, not a canvas fraction; page
    identities are read via the view-state seam, never measured off pixels.
  * Runs under a Bash shell; UTF-8 declared; explicit encoding on file I/O.
  * The app is closed at teardown (orphan pdfe.exe wedges the next run).
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import List, Optional

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.pdfe_session import (  # noqa: E402
    PDFESession,
    PDFE_EXE,
    kill_pdfe_orphans,
    no_orphans,
)

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_3PAGE = FIXTURES / "test_3page.pdf"


# ---------------------------------------------------------------------------
# Skip-conditions (mirror test_ui_page_operations.py)
# ---------------------------------------------------------------------------

def _has_pdfe_binary() -> bool:
    return Path(PDFE_EXE).exists()


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401
        return True
    except Exception:  # noqa: BLE001
        return False


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pdfe_binary() and _has_pywinauto() and _is_interactive_session()),
    reason="Live pdfe.exe + interactive Windows desktop session required.",
)


# ---------------------------------------------------------------------------
# Process isolation
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _isolate_pdfe_processes():
    kill_pdfe_orphans()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if no_orphans():
            break
        kill_pdfe_orphans()
        time.sleep(0.3)
    time.sleep(1.0)
    yield
    kill_pdfe_orphans()
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        if no_orphans():
            break
        kill_pdfe_orphans()
        time.sleep(0.3)
    assert no_orphans(), "pdfe.exe still running at teardown — orphan leak."


def _settle(seconds: float = 0.6) -> None:
    time.sleep(seconds)


# ---------------------------------------------------------------------------
# Page-identity oracle via the live view-state seam
# ---------------------------------------------------------------------------

def _goto_page(session: PDFESession, index: int) -> None:
    from tests.helpers.qinvoke import _write_to_pipe
    assert _write_to_pipe(session, "testGoToPagePipe", str(index)), (
        "testGoToPagePipe did not answer — cannot navigate to establish the "
        "page-order oracle (is PDFE_TEST_PIPES=1 set?)."
    )
    _settle(0.5)


def _page_rotation(session: PDFESession) -> Optional[int]:
    from tests.helpers.canvas_geometry import view_state
    st = view_state(session)
    if st is None or "pageRot" not in st:
        return None
    return int(st["pageRot"])


def _order_signature(session: PDFESession, n: int) -> List[int]:
    """Each page's /Rotate, in display order — the order fingerprint."""
    sig: List[int] = []
    for p in range(n):
        _goto_page(session, p)
        rot = _page_rotation(session)
        if rot is None:
            pytest.fail(
                "view-state seam did not report pageRot — the reorder oracle "
                "is unavailable (PDFE built without the #79 seam?)."
            )
        sig.append(rot)
    return sig


def _page_drag_starts(session: PDFESession) -> Optional[int]:
    """GitHub #335 residual: how many times PageListWidget::startDrag() has been
    entered, read from the view-state seam (field ``pageDragStarts``). Lets this
    test distinguish a synthetic drag that REACHED startDrag from one that died
    upstream at Qt's drag-initiation threshold."""
    from tests.helpers.canvas_geometry import view_state
    st = view_state(session)
    if st is None or "pageDragStarts" not in st:
        return None
    return int(st["pageDragStarts"])


def _refocus(session: PDFESession) -> None:
    from tests.helpers.canvas_geometry import refocus_canvas
    refocus_canvas(session)
    _settle(0.3)


def _rotate_cw(session: PDFESession) -> None:
    from tests.helpers.keys import send_keys_to
    send_keys_to(session.hwnd, "^+{RIGHT}")
    _settle(0.5)


def _find_page_panel_list(session: PDFESession):
    lst = session.find_optional(name="Page Thumbnails",
                                control_type="List", timeout=4)
    if lst is not None:
        return lst
    pp = session.find_optional(auto_id="pagePanel", timeout=2)
    if pp is None:
        return None
    try:
        for d in pp.descendants():
            try:
                if d.element_info.control_type == "List":
                    return d
            except Exception:  # noqa: BLE001
                continue
    except Exception:  # noqa: BLE001
        pass
    return None


def _item_rect(lst, index: int):
    children = lst.children()
    if index < 0 or index >= len(children):
        return None
    return children[index].rectangle()


def _item_center(lst, index: int):
    r = _item_rect(lst, index)
    if r is None:
        return None
    return ((r.left + r.right) // 2, (r.top + r.bottom) // 2)


def _hand_drag(x1: int, y1: int, x2: int, y2: int) -> None:
    """A deliberately slow, dwelling Win32 drag.

    Qt6's item views only begin an InternalMove QDrag after the pointer has
    moved past ``startDragDistance`` with the button held AND the event loop
    has processed those moves. pyautogui / a fast send_drag often jumps the
    pointer in one or two batched moves that Qt collapses, so the drag never
    starts (KNOWN_WEAK P10-INFRA-1). This issues the press, then creeps the
    pointer a few pixels at a time with dwells so Qt sees a genuine press-move
    sequence and enters startDrag(), then walks to the target and dwells on it
    so the drop indicator settles before release.
    """
    import ctypes
    import time as _t
    from tests.helpers.win32_input import (
        INPUT, INPUT_MOUSE, SendInput, _abs_coords, assert_safe_click,
        MOUSEEVENTF_MOVE, MOUSEEVENTF_ABSOLUTE,
        MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
    )
    assert_safe_click(x1, y1, op="drag-start")
    assert_safe_click(x2, y2, op="drag-end")

    def _move_abs(x, y, flags):
        ax, ay = _abs_coords(x, y)
        inp = (INPUT * 1)()
        inp[0].type = INPUT_MOUSE
        inp[0].union.mi.dx = ax
        inp[0].union.mi.dy = ay
        inp[0].union.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE
        SendInput(1, inp, ctypes.sizeof(INPUT))

    _move_abs(x1, y1, MOUSEEVENTF_MOVE)
    _t.sleep(0.15)
    _move_abs(x1, y1, MOUSEEVENTF_LEFTDOWN)
    _t.sleep(0.25)
    # Creep to cross the start-drag distance (default ~10px) a few px at a time.
    for dy in (4, 8, 12, 18, 26):
        _move_abs(x1, y1 + dy, MOUSEEVENTF_MOVE)
        _t.sleep(0.12)
    # Walk to the target in small steps with dwells.
    steps = 24
    for i in range(1, steps + 1):
        t = i / steps
        cx = int(x1 + (x2 - x1) * t)
        cy = int((y1 + 26) + (y2 - (y1 + 26)) * t)
        _move_abs(cx, cy, MOUSEEVENTF_MOVE)
        _t.sleep(0.05)
    # Dwell on the drop point so the indicator settles, then release.
    for _ in range(4):
        _move_abs(x2, y2, MOUSEEVENTF_MOVE)
        _t.sleep(0.08)
    _move_abs(x2, y2, MOUSEEVENTF_LEFTUP)
    _t.sleep(0.2)


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

@pytest.mark.xfail(
    reason=(
        "KNOWN_WEAK P10-INFRA-1, re-measured 2026-11-05 against the GitHub #335 "
        "startDrag() override — and the measurement OVERTURNED the prior "
        "assumption. The old reason claimed SendInput could not make Qt even "
        "BEGIN the drag. MEASURED this run via the new pageDragStarts view-state "
        "seam: after a 71s dwelling SendInput drag, PageListWidget::startDrag() "
        "was entered exactly ONCE (pageDragStarts 0 -> 1) — so the synthetic "
        "gesture DOES now cross Qt's drag-initiation threshold and reach the "
        "#335 override. What still fails is DOWNSTREAM: the real QDrag::exec() "
        "modal loop, driven by synthetic SendInput, does not deliver a drop that "
        "dropEvent turns into a reorder, so the document order is unchanged after "
        "the gesture. The failure point has therefore moved from drag-INITIATION "
        "to drop/EXEC under synthetic input; #335's startDrag override runs, but "
        "the un-driveable half is now the OS modal drag/drop loop, which a "
        "headless/synthetic harness cannot complete. The real startDrag()+exec "
        "path (with a deterministic drop delivery) is covered by "
        "test_page_panel_real_drag_reorder.cpp; the routing/config half by "
        "test_page_panel_q_invokable.py::DragReorderConfiguration...; the "
        "model/command half by test_page_commands.py. This test drives the REAL "
        "drag (never the movePageProgrammatic bypass) and asserts the true end "
        "state; it will XPASS the day the synthetic drop becomes drivable. "
        "strict=False so an XPASS is reported, not failed."
    ),
    strict=False,
)
@REQUIRES_LIVE_UI
def test_within_file_reorder_drag_changes_document_order():
    if not FIX_3PAGE.exists():
        pytest.skip(f"Fixture missing: {FIX_3PAGE}")

    from tests.helpers.win32_input import send_click

    with PDFESession(pdf_path=str(FIX_3PAGE)) as s:
        _settle(1.2)

        # 1) Stamp distinct, observable per-page identities so a reorder is
        #    distinguishable from a no-op: page 0 -> 0°, page 1 -> 90°,
        #    page 2 -> 180°. Rotate acts on the current page, so navigate first,
        #    then refocus the canvas (the pipe write stole keyboard focus).
        _goto_page(s, 1)
        _refocus(s)
        _rotate_cw(s)                 # page 1 -> 90
        _goto_page(s, 2)
        _refocus(s)
        _rotate_cw(s)
        _rotate_cw(s)                 # page 2 -> 180

        before = _order_signature(s, 3)
        assert len(set(before)) == len(before), (
            f"page identities not distinct ({before}); no permutation could "
            f"be distinguished. Rotation stamping failed."
        )

        # 2) Drive the REAL reorder gesture: drag thumbnail 0 downward onto /
        #    past thumbnail 2. Two drop targets are attempted (end of list, then
        #    onto the last item) because the exact drop position that Qt treats
        #    as a row-move in IconMode is build-dependent; both are REAL drags,
        #    neither is the movePageProgrammatic bypass.
        lst = _find_page_panel_list(s)
        assert lst is not None, "could not locate the Pages panel QListWidget."
        assert len(lst.children()) == 3, (
            f"expected 3 thumbnails, found {len(lst.children())}."
        )
        src = _item_center(lst, 0)
        last_rect = _item_rect(lst, 2)
        assert src and last_rect, "could not resolve thumbnail item rectangles."

        # GitHub #335 residual observability: snapshot the startDrag() entry
        # count so, if the reorder does not happen, we can say WHERE it died
        # rather than restate the pre-#335 assumption.
        starts_before = _page_drag_starts(s)
        print(f"[#335-residual] pageDragStarts before drag = {starts_before}")

        after = before
        drop_points = [
            # Just below the last thumbnail (end-of-list insert).
            ((last_rect.left + last_rect.right) // 2, last_rect.bottom - 4),
            # Onto the last thumbnail's lower half.
            ((last_rect.left + last_rect.right) // 2,
             (last_rect.top + 3 * last_rect.bottom) // 4),
        ]
        for dx, dy in drop_points:
            # Select + focus the source item, then perform the dwelling drag.
            send_click(src[0], src[1])
            _settle(0.4)
            _hand_drag(src[0], src[1], dx, dy)
            _settle(1.2)
            after = _order_signature(s, 3)
            if after != before:
                break
            # Re-resolve the source center (a partial move could have shifted
            # layout) before the next attempt.
            lst = _find_page_panel_list(s) or lst
            src = _item_center(lst, 0) or src

        # GitHub #335 residual: report where the gesture died. If startDrag()
        # never incremented, Qt did not even begin the InternalMove QDrag —
        # the synthetic SendInput moves did not cross the drag-initiation
        # threshold — so the failure is UPSTREAM of the startDrag() override
        # #335 added, not in the clearOrRemove tail it fixed.
        starts_after = _page_drag_starts(s)
        print(f"[#335-residual] pageDragStarts after drag  = {starts_after}")
        if starts_before is not None and starts_after is not None:
            if starts_after == starts_before:
                print("[#335-residual] MEASURED: startDrag() was NOT entered — "
                      "the synthetic drag died at Qt's drag-INITIATION "
                      "threshold, upstream of the #335 override.")
            else:
                print("[#335-residual] MEASURED: startDrag() WAS entered "
                      f"({starts_after - starts_before}x) — the gesture reached "
                      "the override; any remaining failure is at drop/exec.")

        # 3) END-STATE assertions — the real path, no bypass.
        assert after != before, (
            f"the Document page order did not change after the drag "
            f"(before={before}, after={after}). This is exactly the #326 "
            f"defect: only the thumbnail icon moved, the document order was "
            f"untouched."
        )
        assert sorted(after) == sorted(before), (
            f"the page set changed (before={before}, after={after}); a reorder "
            f"must be a permutation — no page gained, lost or duplicated."
        )

        # The reorder must have marked the document dirty, so close/save sees
        # the unsaved edit.
        title = ""
        try:
            title = s.window.window_text() or ""
        except Exception:  # noqa: BLE001
            title = ""
        assert "*" in title, (
            f"the document is not marked dirty after a real reorder "
            f"(title={title!r}); a persisted reorder must set the modified "
            f"flag."
        )
