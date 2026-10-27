# -*- coding: utf-8 -*-
"""Page-operations subsystem helpers for the PDFE UI test harness.

Provides high-level operations for the Pages subsystem: rotating pages,
deleting pages, inserting blank pages, inserting pages from file, extracting
pages, drag-reordering in PagePanel, invoking the context menu, and
multi-selecting pages.  All functions accept a ``PDFESession`` instance (from
``tests.ui_harness.core.launch``) as their first argument and delegate
low-level widget location, canvas clicking, and screenshot capture to the
shared ``core`` helpers.

Discovered objectNames (from src/ui/PagePanel.cpp and src/ui/MainWindow.cpp)
-----------------------------------------------------------------------------
PagePanel widget:
- pagePanel              — QWidget objectName (the dock content widget)
- pagePanelList          — QListWidget objectName (the thumbnail list)
                           accessibleName: "Page Thumbnails"

PagePanel context-menu QAction objectNames (local to the ephemeral QMenu):
- rotatePageCWAction     — "Rotate Clockwise"
- rotatePageCCWAction    — "Rotate Counter-Clockwise"
- rotatePage180Action    — "Rotate 180°"
- insertBlankPageAction  — "Insert Blank Page"  (also in main menu)
- deleteSelectedAction   — "Delete Selected"
- extractSelectedAction  — "Extract Selected..."

MainWindow QAction objectNames (also exposed via Pages menu):
- rotatePageCWAction     — Pages > Rotate Clockwise   (Ctrl+Shift+Right)
- rotatePageCCWAction    — Pages > Rotate Counter-Clockwise (Ctrl+Shift+Left)
- rotatePage180Action    — Pages > Rotate 180°
- deletePageAction       — Pages > Delete Selected Pages  (Delete key)
- insertBlankPageAction  — Pages > Insert Blank Page  (Ctrl+Shift+N)
- insertPagesFromFileAction — Pages > Insert Pages from File...  (Ctrl+Shift+I)
- extractPagesAction     — Pages > Extract Pages...

Pages menu objectName:
- pagesMenu              — QMenu "Pages" in the menu bar

File dialogs triggered by MainWindow:
- "Insert Pages from File"  — QFileDialog::getOpenFileName, title "Insert Pages from File"
- "Extract Pages to File"   — QFileDialog::getSaveFileName, title "Extract Pages to File"
"""

import os
import time

import pyautogui
import pywinauto.timings

from tests.ui_harness.core.canvas import (
    click_at,
    drag,
    right_click_at,
    wait_stable,
)
from tests.ui_harness.core.locate import (
    by_auto_id,
    by_auto_id_or_name,
    by_name,
    invoke,
)
from tests.ui_harness.core.screenshot import capture_canvas

__all__ = [
    "rotate_page",
    "delete_page",
    "insert_blank",
    "insert_from_file",
    "extract_pages",
    "drag_reorder",
    "page_panel_context",
    "select_pages",
]

# ---------------------------------------------------------------------------
# Internal constants — objectNames / accessibleNames
# ---------------------------------------------------------------------------

_PAGE_LIST_AUTO_ID = "pagePanelList"
_PAGE_LIST_NAME = "Page Thumbnails"

# MainWindow QAction objectNames used for Pages-menu fallback.
_ROTATE_CW_ACTION_ID = "rotatePageCWAction"
_ROTATE_CCW_ACTION_ID = "rotatePageCCWAction"
_ROTATE_180_ACTION_ID = "rotatePage180Action"
_DELETE_PAGE_ACTION_ID = "deletePageAction"
_INSERT_BLANK_ACTION_ID = "insertBlankPageAction"
_INSERT_FROM_FILE_ACTION_ID = "insertPagesFromFileAction"
_EXTRACT_PAGES_ACTION_ID = "extractPagesAction"

# Pages menu path (for menu_select fallbacks).
_PAGES_MENU_ROTATE_CW = "Pages->Rotate Clockwise"
_PAGES_MENU_ROTATE_CCW = "Pages->Rotate Counter-Clockwise"
_PAGES_MENU_ROTATE_180 = "Pages->Rotate 180°"
_PAGES_MENU_DELETE = "Pages->Delete Selected Pages"
_PAGES_MENU_INSERT_BLANK = "Pages->Insert Blank Page"
_PAGES_MENU_INSERT_FROM_FILE = "Pages->Insert Pages from File..."
_PAGES_MENU_EXTRACT = "Pages->Extract Pages..."

# Context-menu item text as it appears in the ephemeral QMenu (see PagePanel.cpp).
_CTX_ACTION_MAP = {
    "rotate cw":      "Rotate Clockwise",
    "rotate ccw":     "Rotate Counter-Clockwise",
    "rotate 180":     "Rotate 180°",
    "insert blank":   "Insert Blank Page",
    "delete":         "Delete Selected",
    "extract":        "Extract Selected...",
}

# Timeouts.
_DIALOG_TIMEOUT = 15
_PANEL_TIMEOUT = 10
_ACTION_SETTLE = 0.3


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def rotate_page(session, idx, delta):
    """Select page *idx* in PagePanel and rotate it by *delta* degrees.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based page index to rotate.
    delta   : int   Rotation in degrees: +90 (CW), -90 (CCW), or 180.

    Raises
    ------
    ValueError
        If *delta* is not 90, -90, or 180.
    RuntimeError
        If the rotate action cannot be located or invoked.
    """
    if delta not in (90, -90, 180):
        raise ValueError(
            f"rotate_page: delta must be 90, -90, or 180; got {delta!r}."
        )

    select_pages(session, [idx])

    if delta == 90:
        action_id = _ROTATE_CW_ACTION_ID
        menu_path = _PAGES_MENU_ROTATE_CW
        action_name = "Rotate Clockwise"
    elif delta == -90:
        action_id = _ROTATE_CCW_ACTION_ID
        menu_path = _PAGES_MENU_ROTATE_CCW
        action_name = "Rotate Counter-Clockwise"
    else:
        action_id = _ROTATE_180_ACTION_ID
        menu_path = _PAGES_MENU_ROTATE_180
        action_name = "Rotate 180°"

    _invoke_action_or_menu(session, action_id, action_name, menu_path)
    wait_stable(_ACTION_SETTLE)


def delete_page(session, idx):
    """Select page *idx* in PagePanel and delete it via the Delete Page action.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based page index to delete.

    Raises
    ------
    RuntimeError
        If the delete action cannot be located or invoked.
    """
    select_pages(session, [idx])
    _invoke_action_or_menu(
        session,
        action_id=_DELETE_PAGE_ACTION_ID,
        action_name="Delete Selected Pages",
        menu_path=_PAGES_MENU_DELETE,
    )
    wait_stable(_ACTION_SETTLE)


def insert_blank(session, at_idx):
    """Insert a blank page at *at_idx* via the Insert Blank Page action.

    PDFE inserts the blank page *after* the currently selected page.  This
    helper first selects page ``at_idx - 1`` (or page 0 when at_idx == 0) so
    that the new page lands at the expected position.  When ``at_idx`` equals
    the current page count the last page is selected, which inserts at the end.

    If PDFE opens a dialog requesting page dimensions this helper accepts the
    defaults (pressing Enter).

    Parameters
    ----------
    session : PDFESession
    at_idx  : int   Zero-based index at which the blank page should appear
                    after the operation.

    Raises
    ------
    RuntimeError
        If the insert action cannot be located or invoked.
    """
    # Select the page before the insertion point so PDFE inserts after it.
    anchor = max(0, at_idx - 1)
    select_pages(session, [anchor])

    _invoke_action_or_menu(
        session,
        action_id=_INSERT_BLANK_ACTION_ID,
        action_name="Insert Blank Page",
        menu_path=_PAGES_MENU_INSERT_BLANK,
    )

    # Dismiss any size dialog with defaults.
    _dismiss_dialog_if_present(session, title_re=r"Insert|Blank|Page Size", timeout=3)

    wait_stable(_ACTION_SETTLE)


def insert_from_file(session, path, at_idx):
    """Insert pages from *path* into the document at *at_idx*.

    Selects the anchor page (at_idx - 1 or 0), invokes the Insert Pages from
    File action, types *path* in the open-file dialog, and confirms.

    Parameters
    ----------
    session  : PDFESession
    path     : str   Absolute path to the source PDF file.
    at_idx   : int   Zero-based index at which pages should be inserted.

    Raises
    ------
    TimeoutError
        If the open-file dialog does not appear within ``_DIALOG_TIMEOUT`` s.
    RuntimeError
        If the action cannot be located or invoked.
    """
    path = os.path.abspath(path)
    anchor = max(0, at_idx - 1)
    select_pages(session, [anchor])

    _invoke_action_or_menu(
        session,
        action_id=_INSERT_FROM_FILE_ACTION_ID,
        action_name="Insert Pages from File...",
        menu_path=_PAGES_MENU_INSERT_FROM_FILE,
    )

    _wait_for_dialog(session, title_re=r"Insert Pages from File|Open", timeout=_DIALOG_TIMEOUT)

    # Type path in the file-name field and confirm.
    pyautogui.hotkey("ctrl", "a")
    time.sleep(0.05)
    pyautogui.typewrite(path, interval=0.02)
    pyautogui.press("enter")

    wait_stable(_ACTION_SETTLE)


def extract_pages(session, indices, dest):
    """Select *indices* in PagePanel and extract them to *dest*.

    Parameters
    ----------
    session : PDFESession
    indices : list[int]   Zero-based page indices to extract.  Must not be empty.
    dest    : str         Absolute path for the output PDF file.

    Raises
    ------
    ValueError
        If *indices* is empty.
    TimeoutError
        If the save-file dialog does not appear within ``_DIALOG_TIMEOUT`` s.
    RuntimeError
        If the extract action cannot be located or invoked.
    """
    if not indices:
        raise ValueError("extract_pages: indices must not be empty.")

    dest = os.path.abspath(dest)

    select_pages(session, indices)

    _invoke_action_or_menu(
        session,
        action_id=_EXTRACT_PAGES_ACTION_ID,
        action_name="Extract Pages...",
        menu_path=_PAGES_MENU_EXTRACT,
    )

    _wait_for_dialog(session, title_re=r"Extract Pages|Save", timeout=_DIALOG_TIMEOUT)

    # Type destination path in the save-file dialog and confirm.
    pyautogui.hotkey("ctrl", "a")
    time.sleep(0.05)
    pyautogui.typewrite(dest, interval=0.02)
    pyautogui.press("enter")

    wait_stable(_ACTION_SETTLE)


def drag_reorder(session, src, dst):
    """Drag-drop PagePanel item from index *src* to index *dst*.

    Uses PyAutoGUI for the drag because pywinauto's drag_mouse_input may not
    trigger Qt's InternalMove drop event reliably.  The items are located via
    the UIA tree of ``pagePanelList`` (QListWidget), with a coordinate fallback
    that estimates item positions from the list widget's bounding rect and
    uniform icon size (128 px + 10 px spacing).

    Parameters
    ----------
    session : PDFESession
    src     : int   Zero-based source item index.
    dst     : int   Zero-based destination item index.

    Raises
    ------
    RuntimeError
        If the PagePanel list cannot be located.
    """
    if src == dst:
        return  # Nothing to do.

    src_cx, src_cy = _item_screen_center(session, src)
    dst_cx, dst_cy = _item_screen_center(session, dst)

    # Slow drag to allow Qt's drag-start threshold to be exceeded.
    drag(src_cx, src_cy, dst_cx, dst_cy, button="left", duration=0.5)
    wait_stable(_ACTION_SETTLE)


def page_panel_context(session, idx, action):
    """Right-click PagePanel item *idx* and click the named context-menu action.

    *action* is a case-insensitive key from the supported set:
    ``"rotate cw"``, ``"rotate ccw"``, ``"rotate 180"``,
    ``"insert blank"``, ``"delete"``, ``"extract"``.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based page index to right-click.
    action  : str   Context-menu action name (see module docstring for valid keys).

    Raises
    ------
    ValueError
        If *action* is not a recognised key.
    RuntimeError
        If the item or context-menu entry cannot be located.
    """
    key = action.strip().lower()
    if key not in _CTX_ACTION_MAP:
        raise ValueError(
            f"page_panel_context: unknown action {action!r}. "
            f"Valid keys: {sorted(_CTX_ACTION_MAP)!r}."
        )

    menu_item_text = _CTX_ACTION_MAP[key]

    # Ensure the target item is selected first (right-click alone may not change selection).
    select_pages(session, [idx])

    cx, cy = _item_screen_center(session, idx)
    right_click_at(cx, cy)
    wait_stable(0.2)

    # The ephemeral context menu appears as a top-level popup; find the item by name.
    window = session.window
    try:
        menu_ctrl = by_name(window, menu_item_text, timeout=5)
        invoke(menu_ctrl)
    except Exception:
        # Fallback: type the first few chars and press Enter if UIA can't see the menu item.
        pyautogui.typewrite(menu_item_text[:4].lower(), interval=0.05)
        pyautogui.press("enter")

    wait_stable(_ACTION_SETTLE)


def select_pages(session, indices):
    """Multi-select pages by 0-based index in PagePanel via Ctrl+click.

    The first index in *indices* is clicked normally; subsequent indices are
    Ctrl+clicked to extend the selection.  All indices must exist in the list.

    Parameters
    ----------
    session : PDFESession
    indices : list[int]   Zero-based page indices to select.  Must not be empty.

    Raises
    ------
    ValueError
        If *indices* is empty.
    RuntimeError
        If the PagePanel list cannot be located.
    """
    if not indices:
        raise ValueError("select_pages: indices must not be empty.")

    for i, idx in enumerate(indices):
        cx, cy = _item_screen_center(session, idx)
        if i == 0:
            click_at(cx, cy)
        else:
            pyautogui.keyDown("ctrl")
            click_at(cx, cy)
            pyautogui.keyUp("ctrl")
        wait_stable(0.1)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _get_page_list(session):
    """Return the pywinauto control for the pagePanelList QListWidget.

    Tries auto_id first, falls back to accessibleName.

    Parameters
    ----------
    session : PDFESession

    Returns
    -------
    pywinauto control wrapper

    Raises
    ------
    RuntimeError
        If the list widget cannot be found within the timeout.
    """
    window = session.window
    try:
        return by_auto_id_or_name(
            window,
            _PAGE_LIST_AUTO_ID,
            _PAGE_LIST_NAME,
            timeout=_PANEL_TIMEOUT,
        )
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate PagePanel list widget "
            f"(auto_id={_PAGE_LIST_AUTO_ID!r}, name={_PAGE_LIST_NAME!r}): {exc}"
        ) from exc


def _item_screen_center(session, idx):
    """Return (screen_x, screen_y) for the centre of PagePanel item *idx*.

    Strategy:
    1. Locate the list widget via UIA and try to get its children (list items).
       If child count is sufficient, use the child's bounding rect centre.
    2. Fall back to estimating the item position from the list widget rect and
       the uniform icon grid (128 px icons + 10 px spacing, column-major).

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based item index.

    Returns
    -------
    (int, int)  absolute screen coordinates

    Raises
    ------
    RuntimeError
        If the list widget cannot be located.
    """
    list_ctrl = _get_page_list(session)

    # Strategy 1: UIA children.
    try:
        children = list_ctrl.children()
        if idx < len(children):
            r = children[idx].rectangle()
            cx = (r.left + r.right) // 2
            cy = (r.top + r.bottom) // 2
            return (cx, cy)
    except Exception:
        pass

    # Strategy 2: grid estimation.
    # PagePanel uses IconMode with 128x128 icons + 10px spacing.
    # Items flow left-to-right then top-to-bottom (QListWidget::Adjust).
    try:
        list_rect = list_ctrl.rectangle()
        cell_w = 128 + 10
        cell_h = 128 + 10
        list_width = list_rect.right - list_rect.left

        # Number of columns that fit in the list width (at least 1).
        cols = max(1, list_width // cell_w)

        col = idx % cols
        row = idx // cols

        # Centre of the cell.
        cx = list_rect.left + col * cell_w + cell_w // 2
        cy = list_rect.top + row * cell_h + cell_h // 2
        return (cx, cy)
    except Exception as exc:
        raise RuntimeError(
            f"_item_screen_center: could not compute position for item {idx}: {exc}"
        ) from exc


def _invoke_action_or_menu(session, action_id, action_name, menu_path):
    """Invoke a QAction via UIA auto_id lookup, falling back to menu navigation.

    Parameters
    ----------
    session     : PDFESession
    action_id   : str   objectName of the QAction (UIA AutomationId).
    action_name : str   accessibleName fallback.
    menu_path   : str   pywinauto menu_select path (e.g. "Pages->Delete Selected Pages").

    Raises
    ------
    RuntimeError
        If both strategies fail.
    """
    window = session.window

    # Strategy 1: direct UIA node lookup.
    try:
        ctrl = by_auto_id_or_name(window, action_id, action_name, timeout=5)
        invoke(ctrl)
        return
    except Exception:
        pass

    # Strategy 2: menu navigation.
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, action_id) which fires the QAction via the
        # testToolActivatePipe seam instead of enumerating the UIA menu tree.
        window.menu_select(menu_path)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke action "
            f"(auto_id={action_id!r}, name={action_name!r}, menu={menu_path!r}): {exc}"
        ) from exc


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

    Used to dismiss optional dimension-confirmation dialogs after Insert Blank
    Page without raising if no dialog appears.

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
