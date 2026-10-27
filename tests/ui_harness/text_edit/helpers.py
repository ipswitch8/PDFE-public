# -*- coding: utf-8 -*-
"""Text-edit subsystem helpers for the PDFE UI test harness.

Provides high-level operations for the text-edit tool: opening a document in
text-edit mode, clicking into spans, typing, capturing overlays, saving and
reopening, word selection, and formatting toolbar interactions.

All functions accept a ``PDFESession`` instance (from
``tests.ui_harness.core.launch``) as their first argument and delegate
low-level widget location, canvas clicking, and screenshot capture to the
shared ``core`` helpers.

Discovered objectNames (from src/ui/TextEditToolBar.cpp and MainWindow.cpp)
---------------------------------------------------------------------------
- textEditToolAction  — QAction that activates the text-edit tool
- boldButton          — QToolButton (checkable) for bold; accessibleName "Bold"
- italicButton        — QToolButton (checkable) for italic; accessibleName "Italic"
- fontSizeSpinBox     — QDoubleSpinBox for font size; accessibleName "Font Size"
- saveAsAction        — QAction for File > Save As (shortcut Ctrl+Shift+S)
- closeAction         — QAction for File > Close (shortcut Ctrl+W)

Page navigation on the canvas is driven by PageDown / PageUp key presses;
the canvas must have focus for these to register.

Focus-safety
------------
All keyboard injection in this module uses the focus-safe wrappers from
``tests.helpers.keys`` (hotkey_to, typewrite_to, press_to) or the Win32
SendMessage-based file-dialog driver (set_open_dialog_filename,
set_save_dialog_filename).  ZERO bare pyautogui.{typewrite,press,hotkey}
calls exist in this file — each injection goes through a helper that
verifies or forces the target HWND to the foreground before delivery.
"""

import os
import time

import pywinauto.timings

from tests.helpers.keys import (
    FocusError,
    hotkey_to,
    press_to,
    typewrite_to,
)
from tests.helpers.file_dialog import (
    set_open_dialog_filename,
    set_save_dialog_filename,
)
from tests.ui_harness.core.canvas import (
    canvas_rect,
    click_at,
    double_click_at,
    page_to_screen,
    wait_stable,
)
from tests.ui_harness.core.locate import (
    by_auto_id,
    by_auto_id_or_name,
    by_name,
    invoke,
    type_into,
)
from tests.ui_harness.core.screenshot import capture_canvas

__all__ = [
    "open_in_text_edit_mode",
    "click_into_span",
    "type_keys",
    "capture_overlay",
    "save_and_reopen",
    "select_word_at",
    "toggle_bold",
    "toggle_italic",
    "set_font_size",
]

# ---------------------------------------------------------------------------
# Internal constants
# ---------------------------------------------------------------------------

_TEXT_EDIT_ACTION_ID = "textEditToolAction"
_BOLD_BUTTON_ID = "boldButton"
_BOLD_BUTTON_NAME = "Bold"
_ITALIC_BUTTON_ID = "italicButton"
_ITALIC_BUTTON_NAME = "Italic"
_FONT_SIZE_SPIN_ID = "fontSizeSpinBox"
_FONT_SIZE_SPIN_NAME = "Font Size"
_SAVE_AS_ACTION_ID = "saveAsAction"
_CLOSE_ACTION_ID = "closeAction"
_CANVAS_ACCESSIBLE_NAME = "PDF Canvas"

# Seconds to wait for the text-edit toolbar to become visible after activation.
_TOOLBAR_APPEAR_TIMEOUT = 10

# Seconds to wait for a save-file dialog to appear.
_DIALOG_TIMEOUT = 15

# Seconds to wait for the app to reopen a file after close.
_REOPEN_TIMEOUT = 20


# ---------------------------------------------------------------------------
# Internal: resolve main-window HWND from a session object
# ---------------------------------------------------------------------------

def _session_hwnd(session) -> int:
    """Return the Win32 HWND of the main PDFE window.

    Works with both ``tests.ui_harness.core.launch.PDFESession`` (which
    exposes ``session.window.handle``) and the phase-5 wrapper
    ``tests.helpers.pdfe_session.PDFESession`` (which exposes
    ``session.hwnd`` directly).
    """
    # Phase-5 wrapper exposes .hwnd directly.
    if hasattr(session, "hwnd"):
        return int(session.hwnd)
    # Base harness session: derive from pywinauto window spec.
    try:
        return int(session.window.handle)
    except Exception:  # noqa: BLE001
        return 0


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def open_in_text_edit_mode(session, pdf_path, page_idx=0):
    """Open *pdf_path*, navigate to *page_idx*, and activate the text-edit tool.

    Parameters
    ----------
    session   : PDFESession
    pdf_path  : str  Absolute or relative path to the PDF file.
    page_idx  : int  Zero-based page index.  Page 0 is the default.

    Raises
    ------
    RuntimeError
        If the text-edit tool action cannot be found or invoked.
    TimeoutError
        If the document does not load or the toolbar does not appear.
    """
    pdf_path = os.path.abspath(pdf_path)

    # Open the file via File > Open (Ctrl+O).  Use focus-safe hotkey_to so
    # the keystroke is guaranteed to reach the PDFE main window.
    hwnd = _session_hwnd(session)
    window = session.window
    window.set_focus()
    hotkey_to(hwnd, "ctrl", "o")

    # Wait for the open-file dialog, then drive it via Win32 SendMessage
    # (focus-immune; no input-queue race).  Mirror the title-fallback
    # chain used by test_ui_file_operations.py: try the specific Qt-set
    # title first, then the generic "Open" substring.  Using the
    # specific title first prevents matching against the PDFE main
    # window itself (which can contain "Open" in transient menu states
    # or recent-files hints).
    dlg_hwnd = _wait_for_top_level_window_hwnd(
        session, "Open PDF File", timeout=_DIALOG_TIMEOUT - 2)
    if dlg_hwnd == 0:
        dlg_hwnd = _wait_for_top_level_window_hwnd(
            session, "Open", timeout=2)
    if dlg_hwnd:
        set_open_dialog_filename(dlg_hwnd, pdf_path)
    else:
        # Fallback: dialog appeared but HWND lookup failed; type the path
        # with focus-safe wrapper targeting the main window (the dialog
        # should have inherited foreground from the Ctrl+O dispatch).
        typewrite_to(hwnd, pdf_path, interval=0.02)
        press_to(hwnd, "enter")

    # Wait for the document to load: canvas must become enabled.
    _wait_for_canvas(session, timeout=_REOPEN_TIMEOUT)

    # Navigate to the requested page (PageDown from page 0).
    if page_idx > 0:
        _navigate_to_page(session, page_idx)

    # Activate the text-edit tool.
    _activate_text_edit_tool(session)


def click_into_span(session, x, y):
    """Click at canvas-relative pixel coordinates to place the caret in a span.

    Parameters
    ----------
    session : PDFESession
    x       : int  Pixels from the left edge of the PDF canvas.
    y       : int  Pixels from the top edge of the PDF canvas.
    """
    sx, sy = page_to_screen(session.app, x, y)
    click_at(sx, sy)
    wait_stable(0.15)


def type_keys(session, s):
    """Type a literal string into the active text-edit span.

    Uses ``typewrite_to`` with the session's main-window HWND for reliable,
    focus-safe delivery.  The canvas must already have focus (call
    ``click_into_span`` first).

    Parameters
    ----------
    session : PDFESession
    s       : str  The string to type.  Stick to printable ASCII for
                   reliable cross-platform key delivery.
    """
    hwnd = _session_hwnd(session)
    typewrite_to(hwnd, s, interval=0.02)


def capture_overlay(session, out_path):
    """Capture the canvas area to *out_path* as a PNG.

    Derives the subsystem and test-name tokens from *out_path* so the
    screenshot manifest is updated correctly.

    Parameters
    ----------
    session  : PDFESession
    out_path : str  Destination PNG path (created / overwritten).

    Returns
    -------
    str  Absolute path to the saved file (same as *out_path* resolved).
    """
    out_path = os.path.abspath(out_path)
    out_dir = os.path.dirname(out_path)
    os.makedirs(out_dir, exist_ok=True)

    # Derive subsystem / test tokens from the path so the manifest
    # records them usefully.  Fall back to generic tokens if the path
    # structure does not conform to the expected pattern.
    parts = out_path.replace("\\", "/").split("/")
    test_name = os.path.splitext(parts[-1])[0] if parts else "capture"
    subsystem = parts[-2] if len(parts) >= 2 else "text_edit"

    saved = capture_canvas(session.app, subsystem=subsystem, test=test_name, wait=0.2)

    # Move to the caller-specified path if it differs from where
    # capture_canvas saved it (capture_canvas chooses its own directory).
    saved = os.path.abspath(saved)
    if saved != out_path:
        import shutil
        shutil.copy2(saved, out_path)

    return out_path


def save_and_reopen(session, save_path):
    """Save the current document to *save_path* via Save As, then reopen it.

    Sequence
    --------
    1. Invoke File > Save As via the ``saveAsAction`` UIA control.
    2. Wait for the native save-file dialog; set the path via Win32
       SendMessage (focus-immune); accept.
    3. Close the document with Ctrl+W (focus-safe).
    4. Reopen the saved file via Ctrl+O + Win32 dialog driver.

    Parameters
    ----------
    session   : PDFESession
    save_path : str  Absolute path for the saved PDF.

    Raises
    ------
    TimeoutError
        If the save dialog does not appear within ``_DIALOG_TIMEOUT`` seconds.
    """
    save_path = os.path.abspath(save_path)
    hwnd = _session_hwnd(session)
    window = session.window

    # Invoke Save As through the menu to avoid the Ctrl+Shift+S ambiguity
    # (signDocumentAction also uses Ctrl+Shift+S on this build).
    try:
        save_as_ctrl = by_auto_id(window, _SAVE_AS_ACTION_ID, timeout=5)
        invoke(save_as_ctrl)
    except Exception:
        # Fall back to menu navigation if the action is not directly findable
        # as a UIA node (QActions may not surface as standalone controls).
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where the menu_select walk is reliable. On Server-2019 CI it can
        # deadlock — prefer invoke_action(window, "saveAsAction") (or the
        # testSaveAsPipe seam) which never enumerates the UIA menu tree.
        window.menu_select("File->Save As...")

    # Wait for the save-file dialog, then drive it via Win32 SendMessage.
    save_dlg_hwnd = _wait_for_top_level_window_hwnd(session, "Save", timeout=_DIALOG_TIMEOUT)
    if save_dlg_hwnd:
        set_save_dialog_filename(save_dlg_hwnd, save_path)
    else:
        # Fallback: use focus-safe keyboard injection targeting the main window.
        hotkey_to(hwnd, "ctrl", "a")
        time.sleep(0.05)
        typewrite_to(hwnd, save_path, interval=0.02)
        press_to(hwnd, "enter")

    # Allow the write to complete.
    wait_stable(0.5)

    # Close the document with a focus-safe hotkey.
    window.set_focus()
    hotkey_to(hwnd, "ctrl", "w")
    wait_stable(0.4)

    # Handle any "unsaved changes?" prompt that might appear after close.
    _dismiss_save_prompt_if_present(session)

    # Reopen the saved file.
    window = session.window
    window.set_focus()
    hotkey_to(hwnd, "ctrl", "o")
    open_dlg_hwnd = _wait_for_top_level_window_hwnd(session, "Open", timeout=_DIALOG_TIMEOUT)
    if open_dlg_hwnd:
        set_open_dialog_filename(open_dlg_hwnd, save_path)
    else:
        typewrite_to(hwnd, save_path, interval=0.02)
        press_to(hwnd, "enter")

    _wait_for_canvas(session, timeout=_REOPEN_TIMEOUT)


def select_word_at(session, x, y):
    """Double-click at canvas-relative coordinates to select a word.

    Parameters
    ----------
    session : PDFESession
    x       : int  Pixels from canvas left edge.
    y       : int  Pixels from canvas top edge.
    """
    sx, sy = page_to_screen(session.app, x, y)
    double_click_at(sx, sy)
    wait_stable(0.15)


def toggle_bold(session):
    """Click the Bold toolbar button in the TextEditToolBar.

    Locates the button by objectName ``boldButton`` (UIA AutomationId),
    falling back to accessibleName ``"Bold"`` if the automation ID is
    not directly reachable.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    TimeoutError
        If the bold button cannot be found within the default timeout.
    """
    _click_toolbar_button(session, _BOLD_BUTTON_ID, _BOLD_BUTTON_NAME)


def toggle_italic(session):
    """Click the Italic toolbar button in the TextEditToolBar.

    Locates the button by objectName ``italicButton`` (UIA AutomationId),
    falling back to accessibleName ``"Italic"``.

    Parameters
    ----------
    session : PDFESession

    Raises
    ------
    TimeoutError
        If the italic button cannot be found within the default timeout.
    """
    _click_toolbar_button(session, _ITALIC_BUTTON_ID, _ITALIC_BUTTON_NAME)


def set_font_size(session, pt):
    """Set the font-size spinbox in the TextEditToolBar to *pt* points.

    Locates the spinbox by objectName ``fontSizeSpinBox``, clears its current
    value, and types the new value followed by Enter to commit.

    Parameters
    ----------
    session : PDFESession
    pt      : float  Font size in points.

    Raises
    ------
    TimeoutError
        If the spinbox cannot be found within the default timeout.
    """
    hwnd = _session_hwnd(session)
    window = session.window
    try:
        spin = by_auto_id_or_name(
            window,
            _FONT_SIZE_SPIN_ID,
            _FONT_SIZE_SPIN_NAME,
            timeout=10,
        )
    except TimeoutError:
        raise TimeoutError(
            f"Font-size spinbox not found "
            f"(auto_id={_FONT_SIZE_SPIN_ID!r}, name={_FONT_SIZE_SPIN_NAME!r}). "
            "Ensure the text-edit tool is active and a span is selected."
        )

    # Use set_value if available (UIA RangeValue pattern), else type.
    try:
        spin.set_focus()
        spin.set_value(float(pt))
    except Exception:
        type_into(spin, str(pt), clear_first=True)
        press_to(hwnd, "enter")

    wait_stable(0.1)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _activate_text_edit_tool(session):
    """Invoke the textEditToolAction and wait for the toolbar to appear.

    QActions are NOT directly exposed as UIA elements.  The toolbar button
    wrapping textEditToolAction has auto_id "textEditToolActionButton"
    (the Qt UIA bridge appends "Button" to the QToolButton's objectName,
    which is derived from the QAction's objectName).  We try the button
    first, then fall back to the bare action ID for builds that may expose
    it differently, then to the accessible name "Edit Text".
    """
    window = session.window
    action_ctrl = None

    # Try the button wrapper first (most reliable on this rig).
    for candidate_id in (_TEXT_EDIT_ACTION_ID + "Button", _TEXT_EDIT_ACTION_ID):
        try:
            action_ctrl = by_auto_id(window, candidate_id, timeout=5)
            break
        except Exception:
            pass

    # Fallback: search by accessible name "Edit Text".
    if action_ctrl is None:
        try:
            action_ctrl = by_name(window, "Edit Text", timeout=5)
        except Exception:
            pass

    if action_ctrl is None:
        raise RuntimeError(
            f"Could not locate the text-edit tool action "
            f"(tried auto_id={_TEXT_EDIT_ACTION_ID!r}, "
            f"{_TEXT_EDIT_ACTION_ID!r}Button, name='Edit Text'). "
            "Ensure PDFE is running with a document loaded."
        )

    try:
        invoke(action_ctrl)
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke the text-edit tool action: {exc}"
        ) from exc

    # Wait for the TextEditToolBar to become visible.  The toolbar's
    # objectName is "textEditToolBar"; wait until any of its children
    # (e.g. boldButton) appears in the UIA tree, confirming the toolbar
    # is shown.
    deadline = time.time() + _TOOLBAR_APPEAR_TIMEOUT
    while time.time() < deadline:
        try:
            by_auto_id(window, _BOLD_BUTTON_ID, timeout=1)
            return  # toolbar is up
        except Exception:
            time.sleep(0.2)

    raise TimeoutError(
        f"TextEditToolBar did not become visible within {_TOOLBAR_APPEAR_TIMEOUT}s "
        f"after activating {_TEXT_EDIT_ACTION_ID!r}."
    )


def _navigate_to_page(session, page_idx):
    """Navigate the canvas to the given zero-based page index via PageDown.

    Focuses the canvas first so key events are delivered to PdfCanvas.
    Uses the focus-safe press_to wrapper with the canvas HWND (falling back
    to the main-window HWND) so PageDown keystrokes cannot leak to other apps.
    """
    hwnd = _session_hwnd(session)
    window = session.window
    canvas_hwnd = hwnd

    # Focus the canvas widget.
    try:
        canvas_ctrl = by_name(window, _CANVAS_ACCESSIBLE_NAME, timeout=10)
        canvas_ctrl.set_focus()
        # Prefer targeting the canvas's own HWND for the PageDown presses.
        try:
            canvas_hwnd = int(canvas_ctrl.handle)
        except Exception:  # noqa: BLE001
            canvas_hwnd = hwnd
    except Exception:
        # If UIA cannot find the canvas by name, fall back to clicking the centre.
        r = canvas_rect(session.app)
        cx = (r.left + r.right) // 2
        cy = (r.top + r.bottom) // 2
        click_at(cx, cy)

    # Page 0 is where we start; send PageDown (page_idx) times.
    for _ in range(page_idx):
        press_to(canvas_hwnd, "pagedown")
        time.sleep(0.1)

    wait_stable(0.3)


def _wait_for_canvas(session, timeout=20):
    """Block until the PDF canvas widget is visible and enabled.

    P-H-INFRA-1 fix: the canvas accessible name (UIA Name property) is NOT
    a static "PDF Canvas" string — PdfCanvasAccessible::text(Name) returns the
    currently-loaded document filename (see src/ui/PdfCanvasAccessible.cpp).
    The original by_name() lookup therefore never resolves after a document is
    loaded.  The correct locator is the canvas objectName ("pdfCanvas"), which
    is stable and exposed as the UIA AutomationId.

    Two-stage lookup for robustness after a modal dialog dismisses:
    Strategy A: poll using by_auto_id on the existing session.window spec.
    Strategy B: fresh pywinauto.Application connect (bypasses stale UIA cache).
    Total timeout budget is split equally between the two stages.
    """
    import pywinauto as _pwa  # noqa: PLC0415

    _CANVAS_AUTO_ID = "pdfCanvas"
    half = max(5, timeout // 2)

    # --- Strategy A: poll using the existing session.window UIA cache ---
    def _canvas_ready_a():
        try:
            window = session.window  # property — fresh spec each call
            by_auto_id(window, _CANVAS_AUTO_ID, timeout=1)
            return True
        except Exception:
            return False

    found_a = False
    try:
        pywinauto.timings.wait_until(
            timeout=half,
            retry_interval=0.3,
            func=_canvas_ready_a,
        )
        found_a = True
    except pywinauto.timings.TimeoutError:
        pass

    # --- Strategy B: fresh Application connect, bypassing stale UIA cache ---
    # Always run Strategy B when we have a PID, even if Strategy A found the
    # canvas.  Reason: Strategy A may succeed on the canvas element (which the
    # stale UIA client can still see as a Pane) while other elements in the
    # same application window (QActions, toolbar buttons) are invisible to the
    # same stale client.  Replacing session.app with a fresh IUIAutomation
    # client guarantees subsequent lookups (textEditToolAction, boldButton, etc.)
    # also succeed.
    pid = None
    try:
        pid = session.pid if hasattr(session, "pid") else None
        if pid is None:
            pid = int(session.window.process_id())
    except Exception:
        pass

    if pid is not None:
        b_timeout = max(5, timeout - half) if not found_a else 10
        deadline = time.time() + b_timeout
        while time.time() < deadline:
            try:
                fresh_app = _pwa.Application(backend="uia")
                fresh_app.connect(process=pid, timeout=5)
                fresh_win = fresh_app.window(title_re=r".*PDFE.*")
                fresh_win.wait("visible", timeout=3)
                ctrl = fresh_win.child_window(auto_id=_CANVAS_AUTO_ID, found_index=0)
                ctrl.wait("exists", timeout=2)
                # Replace session.app so all subsequent window/widget lookups
                # in this test use the fresh UIA client.  PDFESession.window is
                # a property that delegates to self.app, so this is sufficient.
                try:
                    session.app = fresh_app
                except Exception:
                    pass
                return
            except Exception:
                time.sleep(0.4)

    if found_a:
        # Canvas was found via Strategy A but we couldn't refresh session.app.
        # Return success anyway — some subsequent lookups may still work.
        return

    raise TimeoutError(
        f"PDF canvas (auto_id={_CANVAS_AUTO_ID!r}) "
        f"did not become ready within {timeout}s (Strategy A + Strategy B)."
    )


def _wait_for_dialog(session, title_re, timeout=15):
    """Block until a top-level dialog whose title matches *title_re* appears.

    Returns the dialog WindowSpecification.
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


def _wait_for_top_level_window_hwnd(session, title_substr, timeout=15):
    """Wait for a top-level window whose title contains *title_substr*.

    Returns the HWND (int) of the first matching visible window, or 0 on
    timeout.  Uses ``session.wait_for_top_level_window`` when available
    (phase-5 PDFESession wrapper), otherwise falls back to a PID-
    independent EnumWindows scan via the file_dialog helper.

    Pipeline b7e2d04a phase-1 fix: the prior pywinauto fallback used
    ``session.app.window(title_re=)`` which filters by the Application's
    connected PID — but native file dialogs (Windows Common Item Dialog
    via IFileOpenDialog/IFileSaveDialog) live in a different message
    thread that may not surface through the PID-filtered window list on
    the base harness Application.  EnumWindows across all visible
    top-level windows finds the dialog regardless.
    """
    if hasattr(session, "wait_for_top_level_window"):
        return session.wait_for_top_level_window(title_substr, timeout=timeout)

    # Fallback for base harness sessions: PID-independent enumeration.
    from tests.helpers.file_dialog import _list_top_level_windows  # type: ignore
    needle = title_substr.lower()
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, title in _list_top_level_windows():
            if needle in title.lower():
                return hwnd
        time.sleep(0.3)
    return 0


def _dismiss_save_prompt_if_present(session):
    """If a 'save changes?' modal appears after Ctrl+W, click 'Don't Save'."""
    try:
        # Qt shows a QMessageBox titled "PDFE" or similar asking about unsaved changes.
        dlg = session.app.window(title_re=r"PDFE|Save")
        dlg.wait("visible", timeout=2)
        # Look for a 'Don't Save' or 'Discard' button.
        for label in ("Don't Save", "Discard", "No"):
            try:
                btn = dlg.child_window(title=label, control_type="Button")
                btn.wait("exists visible enabled", timeout=1)
                btn.click_input()
                return
            except Exception:
                continue
    except Exception:
        pass  # No dialog appeared — document was already clean.


def _click_toolbar_button(session, auto_id, accessible_name):
    """Locate a TextEditToolBar button and click it.

    Parameters
    ----------
    session         : PDFESession
    auto_id         : str  objectName / UIA AutomationId of the button.
    accessible_name : str  Fallback UIA Name (accessibleName).

    Raises
    ------
    TimeoutError
        If neither lookup succeeds within the default timeout.
    """
    window = session.window
    try:
        btn = by_auto_id_or_name(window, auto_id, accessible_name, timeout=10)
    except TimeoutError:
        raise TimeoutError(
            f"Toolbar button not found "
            f"(auto_id={auto_id!r}, name={accessible_name!r}). "
            "Ensure the text-edit tool is active."
        )
    invoke(btn)
    wait_stable(0.1)
