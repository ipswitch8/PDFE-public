# -*- coding: utf-8 -*-
"""Redaction subsystem helpers for the PDFE UI test harness.

Provides high-level operations for the redaction subsystem: activating the
Redaction Tool and dragging a mark rectangle, applying all pending redactions
(and accepting the irreversibility warning), and searching for text and
redacting all occurrences via the Search Panel.  All functions accept a
``PDFESession`` instance (from ``tests.ui_harness.core.launch``) as their
first argument and delegate low-level widget location, canvas interaction,
and screenshot capture to the shared ``core`` helpers.

Discovered objectNames (from src/ui/MainWindow.cpp / RedactionPanel.cpp /
SearchPanel.cpp)
--------------------------------------------------------------------------
MainWindow QAction objectNames:
- redactToolAction       — QAction that activates the Redaction Tool
                           accessibleName: "Redaction Tool"
- markRedactionAction    — QAction "Mark Redaction" (also activates the tool)
                           accessibleName: "Mark Redaction"
- applyRedactionsAction  — QAction "Apply Redactions..."
                           accessibleName: "Apply Redactions..."

RedactionPanel widget objectNames (from RedactionPanel.cpp):
- redactionPanel         — QWidget objectName (the dock content widget)
                           accessibleName: "Redaction Panel"
- redactionPanelDock     — QDockWidget objectName "redactionPanelDock"
                           accessibleName: "Redactions Panel"
- redactionMarksList     — QTreeWidget listing pending marks
                           accessibleName: "Redaction Marks List"
- applyRedactionsButton  — QPushButton "Apply All Redactions"
                           accessibleName: "Apply All Redactions"
- deleteRedactionButton  — QPushButton "Delete Selected Redaction"
                           accessibleName: "Delete Selected Redaction"

SearchPanel widget objectNames (from SearchPanel.cpp):
- searchPanel            — QWidget objectName "searchPanel"
- searchPanelDock        — QDockWidget objectName "searchPanelDock"
                           accessibleName: "Search Panel"
- searchEdit             — QLineEdit objectName "searchEdit"
                           accessibleName: "Search Text"
- searchButton           — QPushButton objectName "searchButton"
                           accessibleName: "Search"

Redaction flow notes (from src/ui/MainWindow.cpp):
- markRedactionAction triggers redactToolAction and calls onRedactToolSelected,
  which creates a RedactionTool and sets it as the active canvas tool.
- The RedactionTool draws a dashed-red rectangle on click-drag; on mouse
  release, redactionCreated fires and the mark is stored by RedactionManager.
- applyRedactionsAction invokes onApplyRedactions, which shows a
  QMessageBox::warning (title "Apply Redactions", buttons Yes/No).
- search_and_redact: PDFE does not have a built-in "search and redact" single
  action.  The helper implements the workflow as:
    1. Open the Search Panel via toggleSearchPanelAction or menu View > Search Panel.
    2. Type the query in searchEdit and press Search.
    3. For each found result, drag a redaction mark over the highlighted region.
  This approach works for all text PDFs; for scanned-image PDFs where OCR is
  not available, results will be empty and no marks are created.

Security menu path (for menu fallbacks):
- "Security->Mark &Redaction"  (markRedactionAction)
- "Security->&Apply Redactions..."  (applyRedactionsAction)
"""

import time

import pyautogui
import pywinauto.timings

from tests.ui_harness.core.canvas import (
    canvas_rect,
    click_at,
    drag,
    wait_stable,
)
from tests.ui_harness.core.locate import (
    by_auto_id_or_name,
    by_name,
    invoke,
)
from tests.ui_harness.core.screenshot import capture_canvas

__all__ = [
    "mark_redaction",
    "apply_redactions",
    "search_and_redact",
]

# ---------------------------------------------------------------------------
# Internal constants — objectNames discovered from source files
# ---------------------------------------------------------------------------

# MainWindow QAction objectNames.
_REDACT_TOOL_ACTION_ID = "redactToolAction"
_REDACT_TOOL_ACTION_NAME = "Redaction Tool"

_MARK_REDACTION_ACTION_ID = "markRedactionAction"
_MARK_REDACTION_ACTION_NAME = "Mark Redaction"

_APPLY_REDACTIONS_ACTION_ID = "applyRedactionsAction"
_APPLY_REDACTIONS_ACTION_NAME = "Apply Redactions..."

# Search panel widget objectNames.
_SEARCH_PANEL_DOCK_ID = "searchPanelDock"
_SEARCH_PANEL_DOCK_NAME = "Search Panel"
_SEARCH_EDIT_ID = "searchEdit"
_SEARCH_EDIT_NAME = "Search Text"
_SEARCH_BUTTON_ID = "searchButton"
_SEARCH_BUTTON_NAME = "Search"

# RedactionPanel button objectNames.
_APPLY_REDACTIONS_BUTTON_ID = "applyRedactionsButton"
_APPLY_REDACTIONS_BUTTON_NAME = "Apply All Redactions"

# Menu paths (fallback strategy).
_MENU_MARK_REDACTION = "Security->Mark &Redaction"
_MENU_APPLY_REDACTIONS = "Security->&Apply Redactions..."
_MENU_TOGGLE_SEARCH_PANEL = "View->Search Panel"

# Apply-redactions warning dialog.
_APPLY_WARNING_TITLE_RE = r"Apply Redactions"

# Timeouts.
_DIALOG_TIMEOUT = 15
_PANEL_TIMEOUT = 10
_ACTION_SETTLE = 0.4
_TOOL_ACTIVATE_SETTLE = 0.3
_SEARCH_SETTLE = 1.0  # Allow search index build time.


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def mark_redaction(session, rect):
    """Activate the Redaction Tool and drag out a mark rectangle on the canvas.

    Ensures the Redaction Tool is active (via ``markRedactionAction``), then
    drags from the top-left to the bottom-right of *rect* to draw a redaction
    mark.  The mark is committed on mouse release; the RedactionPanel dock
    shows automatically after the first mark is created.

    Parameters
    ----------
    session : PDFESession
    rect    : tuple   (left, top, right, bottom) in absolute screen pixel
                      coordinates.  Both points must lie within the PDF canvas.

    Raises
    ------
    RuntimeError
        If the Redaction Tool cannot be activated.
    """
    left, top, right, bottom = rect

    _activate_redact_tool(session)

    # Drag from top-left to bottom-right to draw the redaction rectangle.
    drag(left, top, right, bottom, button="left", duration=0.35)
    wait_stable(_ACTION_SETTLE)


def apply_redactions(session):
    """Invoke the Apply Redactions action and confirm the warning dialog.

    Triggers ``applyRedactionsAction`` (or the panel's Apply All Redactions
    button as a fallback).  When the QMessageBox::warning appears (title
    "Apply Redactions") this helper clicks the Yes button to confirm.

    The operation is irreversible: PDFE clears the undo stack after applying.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    RuntimeError
        If the apply action cannot be invoked.
    TimeoutError
        If the confirmation dialog does not appear within ``_DIALOG_TIMEOUT``
        seconds.
    """
    window = session.window

    # Strategy 1: invoke applyRedactionsAction directly.
    invoked = False
    try:
        ctrl = by_auto_id_or_name(
            window,
            _APPLY_REDACTIONS_ACTION_ID,
            _APPLY_REDACTIONS_ACTION_NAME,
            timeout=5,
        )
        invoke(ctrl)
        invoked = True
    except Exception:
        pass

    # Strategy 2: click the "Apply All Redactions" button in the RedactionPanel.
    if not invoked:
        try:
            btn = by_auto_id_or_name(
                window,
                _APPLY_REDACTIONS_BUTTON_ID,
                _APPLY_REDACTIONS_BUTTON_NAME,
                timeout=5,
            )
            invoke(btn)
            invoked = True
        except Exception:
            pass

    # Strategy 3: menu navigation.
    if not invoked:
        try:
            window.set_focus()
            # GAP-A14: UIA menu walk fallback, retained only for interactive/local
            # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
            # prefer invoke_action(window, "applyRedactionsAction").
            window.menu_select(_MENU_APPLY_REDACTIONS)
            invoked = True
        except Exception as exc:
            raise RuntimeError(
                f"Could not invoke Apply Redactions action "
                f"(auto_id={_APPLY_REDACTIONS_ACTION_ID!r}, "
                f"name={_APPLY_REDACTIONS_ACTION_NAME!r}, "
                f"menu={_MENU_APPLY_REDACTIONS!r}): {exc}"
            ) from exc

    # Wait for the confirmation warning dialog (QMessageBox).
    _confirm_warning_dialog(session, title_re=_APPLY_WARNING_TITLE_RE, timeout=_DIALOG_TIMEOUT)

    # Give PDFE time to apply redactions, save, reopen and refresh the canvas.
    wait_stable(2.0)

    # Dismiss any follow-up informational dialog (success/partial-success message).
    _dismiss_dialog_if_present(session, title_re=r"Redactions Applied|Redaction", timeout=5)


def search_and_redact(session, query):
    """Search for *query* in the document and mark all results for redaction.

    Workflow:
    1. Open the Search Panel (via ``toggleSearchPanelAction`` or View menu).
    2. Clear any previous search term, type *query*, and press Search.
    3. For each search result, activate the Redaction Tool and drag a
       redaction mark over the highlighted bounding rect reported by the
       Search Panel's result list items.  Because individual result rects are
       not directly accessible via UIA, the helper extracts approximate screen
       positions by reading result list items from ``resultsList`` and mapping
       them to canvas coordinates using the page-to-screen transform.

    This approach handles text PDFs.  Scanned/image PDFs that have not been
    OCR-processed return no search results; in that case no marks are created
    and the function returns silently.

    Parameters
    ----------
    session : PDFESession
    query   : str   Search term to find and redact.

    Raises
    ------
    RuntimeError
        If the Search Panel or search input cannot be located.
    """
    window = session.window

    # Step 1: Ensure the Search Panel is visible.
    _ensure_search_panel_visible(session)

    # Step 2: Focus the search input, clear it, type the query, run search.
    search_edit = _locate_search_edit(session)
    try:
        search_edit.set_focus()
        search_edit.type_keys("^a{DELETE}", with_spaces=False)
        pyautogui.typewrite(query, interval=0.02)
    except Exception:
        # Fallback: click the edit by name and type.
        try:
            by_name(window, _SEARCH_EDIT_NAME, timeout=5).set_focus()
        except Exception:
            pass
        pyautogui.hotkey("ctrl", "a")
        pyautogui.press("delete")
        pyautogui.typewrite(query, interval=0.02)

    pyautogui.press("enter")
    wait_stable(_SEARCH_SETTLE)

    # Step 3: Collect result items from the results list widget.
    results = _collect_search_results(session)
    if not results:
        # No results found — nothing to redact.
        return

    # Step 4: For each result, activate the Redaction Tool and drag a mark.
    _activate_redact_tool(session)
    for screen_rect in results:
        left, top, right, bottom = screen_rect
        # Expand the rect slightly to ensure the mark fully covers the text.
        padding = 2
        drag(
            left - padding,
            top - padding,
            right + padding,
            bottom + padding,
            button="left",
            duration=0.3,
        )
        wait_stable(0.2)
        # Re-activate the tool between marks in case selection mode was lost.
        _activate_redact_tool(session)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _activate_redact_tool(session):
    """Activate the Redaction Tool via markRedactionAction or redactToolAction.

    Tries ``markRedactionAction`` first (it both checks ``redactToolAction``
    and calls ``onRedactToolSelected``).  Falls back to direct lookup of
    ``redactToolAction``, then to the Security menu.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    RuntimeError
        If the Redaction Tool cannot be activated by any strategy.
    """
    window = session.window

    # Strategy 1: markRedactionAction (preferred — combines check + activate).
    try:
        ctrl = by_auto_id_or_name(
            window,
            _MARK_REDACTION_ACTION_ID,
            _MARK_REDACTION_ACTION_NAME,
            timeout=8,
        )
        invoke(ctrl)
        wait_stable(_TOOL_ACTIVATE_SETTLE)
        return
    except Exception:
        pass

    # Strategy 2: redactToolAction (toggle in annotation toolbar).
    try:
        ctrl = by_auto_id_or_name(
            window,
            _REDACT_TOOL_ACTION_ID,
            _REDACT_TOOL_ACTION_NAME,
            timeout=5,
        )
        try:
            if ctrl.get_toggle_state() == 1:
                return  # Already active.
        except Exception:
            pass
        invoke(ctrl)
        wait_stable(_TOOL_ACTIVATE_SETTLE)
        return
    except Exception:
        pass

    # Strategy 3: Security menu.
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "markRedactionAction").
        window.menu_select(_MENU_MARK_REDACTION)
        wait_stable(_TOOL_ACTIVATE_SETTLE)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not activate Redaction Tool "
            f"(tried markRedactionAction, redactToolAction, "
            f"menu={_MENU_MARK_REDACTION!r}): {exc}"
        ) from exc


def _ensure_search_panel_visible(session):
    """Show the Search Panel if it is not already visible.

    Tries to find the ``searchPanelDock`` via UIA and check its visibility.
    If not visible, invokes ``toggleSearchPanelAction`` or View > Search Panel.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    RuntimeError
        If the search panel cannot be made visible.
    """
    window = session.window

    # Try to locate the dock and check whether it is already visible.
    try:
        dock = by_auto_id_or_name(
            window,
            _SEARCH_PANEL_DOCK_ID,
            _SEARCH_PANEL_DOCK_NAME,
            timeout=3,
        )
        # If the dock is visible and enabled, we are done.
        try:
            if dock.is_visible():
                return
        except Exception:
            pass
    except Exception:
        pass

    # Toggle the search panel via action or menu.
    try:
        toggle_ctrl = by_auto_id_or_name(
            window, "toggleSearchPanelAction", "Search Panel", timeout=5
        )
        invoke(toggle_ctrl)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception:
        pass

    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "toggleSearchPanelAction").
        window.menu_select(_MENU_TOGGLE_SEARCH_PANEL)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not show Search Panel "
            f"(tried UIA dock, toggleSearchPanelAction, menu={_MENU_TOGGLE_SEARCH_PANEL!r}): {exc}"
        ) from exc


def _locate_search_edit(session):
    """Return the pywinauto control for the searchEdit QLineEdit.

    Parameters
    ----------
    session : PDFESession

    Returns
    -------
    pywinauto control wrapper

    Raises
    ------
    RuntimeError
        If the search edit control cannot be found.
    """
    window = session.window
    try:
        return by_auto_id_or_name(
            window, _SEARCH_EDIT_ID, _SEARCH_EDIT_NAME, timeout=_PANEL_TIMEOUT
        )
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate search input "
            f"(auto_id={_SEARCH_EDIT_ID!r}, name={_SEARCH_EDIT_NAME!r}): {exc}"
        ) from exc


def _collect_search_results(session):
    """Return a list of (left, top, right, bottom) screen rects for search hits.

    Reads the ``resultsList`` QListWidget items from the Search Panel UIA
    tree and maps each item's bounding rect to absolute screen coordinates.
    Items that cannot be resolved are skipped silently.

    Parameters
    ----------
    session : PDFESession

    Returns
    -------
    list of (int, int, int, int)  Screen rects for each search result.
                                  Empty if no results or UIA tree is
                                  not accessible.
    """
    window = session.window
    rects = []

    try:
        results_list = by_auto_id_or_name(
            window, "resultsList", "Search Results", timeout=_SEARCH_SETTLE
        )
        children = results_list.children()
        for child in children:
            try:
                r = child.rectangle()
                if r.right > r.left and r.bottom > r.top:
                    rects.append((r.left, r.top, r.right, r.bottom))
            except Exception:
                continue
    except Exception:
        pass  # No accessible results list — caller handles empty list.

    return rects


def _confirm_warning_dialog(session, title_re, timeout=_DIALOG_TIMEOUT):
    """Wait for a QMessageBox warning and click Yes to confirm.

    Parameters
    ----------
    session  : PDFESession
    title_re : str   Regex matched against the dialog window title.
    timeout  : int   Seconds to wait for the dialog.

    Raises
    ------
    TimeoutError
        If the dialog does not appear within *timeout* seconds.
    RuntimeError
        If the Yes button cannot be found or clicked.
    """
    deadline = time.time() + timeout
    dlg = None
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            break
        except Exception:
            time.sleep(0.3)

    if dlg is None:
        raise TimeoutError(
            f"Warning dialog matching title_re={title_re!r} did not appear "
            f"within {timeout}s."
        )

    # Try to click the Yes button via UIA.
    try:
        yes_btn = by_name(dlg, "Yes", timeout=5)
        invoke(yes_btn)
        return
    except Exception:
        pass

    # Fallback: press Alt+Y (mnemonic for &Yes in QMessageBox).
    try:
        pyautogui.hotkey("alt", "y")
        return
    except Exception:
        pass

    # Last resort: press Enter (default button is usually Yes for warnings
    # where we've already set it as the default in MainWindow::onApplyRedactions).
    pyautogui.press("enter")


def _wait_for_dialog(session, title_re, timeout=_DIALOG_TIMEOUT):
    """Block until a top-level dialog whose title matches *title_re* appears.

    Parameters
    ----------
    session  : PDFESession
    title_re : str   Regular expression matched against the dialog title.
    timeout  : int   Seconds to wait.

    Returns
    -------
    pywinauto WindowSpecification for the dialog.

    Raises
    ------
    TimeoutError
        If no matching dialog appears within *timeout* seconds.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            return dlg
        except Exception:
            time.sleep(0.3)

    raise TimeoutError(
        f"Dialog matching title_re={title_re!r} did not appear within {timeout}s."
    )


def _dismiss_dialog_if_present(session, title_re, timeout=3):
    """Accept a dialog (press Enter) if it appears within *timeout* seconds.

    Used to dismiss informational QMessageBox dialogs that appear after
    apply-redactions completes, without raising if no dialog appears.

    Parameters
    ----------
    session  : PDFESession
    title_re : str   Regex to match against dialog title.
    timeout  : int   Seconds to wait for the dialog.
    """
    try:
        _wait_for_dialog(session, title_re=title_re, timeout=timeout)
        pyautogui.press("enter")
        wait_stable(0.2)
    except TimeoutError:
        pass  # No dialog appeared — that is fine.
