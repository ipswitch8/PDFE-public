# -*- coding: utf-8 -*-
"""Deterministic Windows Common Item Dialog driver for UI tests.

Replaces pyautogui keystroke-based dialog driving (pyautogui.typewrite +
pyautogui.press("enter")) with direct Win32 SendMessage to the filename
Edit control inside the dialog, plus a button click on OK / Open / Save.

Background
----------
Tests that drive the native Windows Common Item Dialog via pyautogui
flake under load.  Keystrokes go through the OS shell input queue and
can be lost when 40+ pywinauto sessions run back-to-back in the full
suite.  Empirically:

    * In isolation (3 dialog tests):   2 pass, 1 skip
    * In full suite (47 tests):        same 3 fail with "typed path
                                       didn't reach picker"

The fix is in HOW the dialog input is driven, not the assertion: send
the text directly to the Edit control HWND via WM_SETTEXT, then click
the OK button via WM_COMMAND / BM_CLICK.  Both messages are synchronous
within the dialog's message loop, so there is no input-queue race.

Public API
----------
    set_open_dialog_filename(dialog_hwnd, filename, button_label="Open")
    set_save_dialog_filename(dialog_hwnd, filename, button_label="Save")

Both return ``True`` on success and raise ``DialogDriveError`` on
failure (with a diagnostic listing the dialog's child controls).

Implementation
--------------
Approach A (primary): Win32 EnumChildWindows + WM_SETTEXT
    Walks every descendant of the dialog HWND looking for an Edit
    control inside a ComboBoxEx32 / ComboBox parent (the "File name:"
    combo of IFileDialog).  Falls back to the first Edit child.

Approach B (fallback): pywinauto UIA child_window
    If the SendMessage path can't locate the Edit control, fall back
    to ``Application(backend="uia").connect(handle=hwnd).window(...)``
    and ``set_text`` / ``click_input``.  AutomationId 1148 is the
    standard IFileDialog filename Edit on Win10 / Win11.
"""

from __future__ import annotations

import ctypes
import datetime as _dt
import os
import signal
import threading
import time
from ctypes import wintypes
from pathlib import Path
from typing import List, Optional, Tuple


# ---------------------------------------------------------------------------
# Win32 bindings
# ---------------------------------------------------------------------------

_user32 = ctypes.WinDLL("user32", use_last_error=True)

WM_SETTEXT = 0x000C
WM_COMMAND = 0x0111
WM_GETTEXTLENGTH = 0x000E
WM_GETTEXT = 0x000D
BM_CLICK = 0x00F5
EM_SETSEL = 0x00B1
EM_REPLACESEL = 0x00C2
BN_CLICKED = 0
IDOK = 1
IDCANCEL = 2

_EnumChildWindowsProto = ctypes.WINFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_long,
)

_user32.EnumChildWindows.argtypes = [
    wintypes.HWND, _EnumChildWindowsProto, ctypes.c_long,
]
_user32.EnumChildWindows.restype = wintypes.BOOL

_user32.GetClassNameW.argtypes = [
    wintypes.HWND, wintypes.LPWSTR, ctypes.c_int,
]
_user32.GetClassNameW.restype = ctypes.c_int

_user32.GetWindowTextW.argtypes = [
    wintypes.HWND, wintypes.LPWSTR, ctypes.c_int,
]
_user32.GetWindowTextW.restype = ctypes.c_int

_user32.GetDlgCtrlID.argtypes = [wintypes.HWND]
_user32.GetDlgCtrlID.restype = ctypes.c_int

_user32.GetParent.argtypes = [wintypes.HWND]
_user32.GetParent.restype = wintypes.HWND

_user32.IsWindowVisible.argtypes = [wintypes.HWND]
_user32.IsWindowVisible.restype = wintypes.BOOL

_user32.IsWindowEnabled.argtypes = [wintypes.HWND]
_user32.IsWindowEnabled.restype = wintypes.BOOL

_user32.SendMessageW.argtypes = [
    wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM,
]
_user32.SendMessageW.restype = ctypes.c_ssize_t

_user32.SetForegroundWindow.argtypes = [wintypes.HWND]
_user32.SetForegroundWindow.restype = wintypes.BOOL

_user32.SetFocus.argtypes = [wintypes.HWND]
_user32.SetFocus.restype = wintypes.HWND

# EnumWindows / IsWindow for the overwrite-prompt watchdog.
_EnumWindowsProto = ctypes.WINFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_long,
)
_user32.EnumWindows.argtypes = [_EnumWindowsProto, ctypes.c_long]
_user32.EnumWindows.restype = wintypes.BOOL

_user32.IsWindow.argtypes = [wintypes.HWND]
_user32.IsWindow.restype = wintypes.BOOL


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class DialogDriveError(RuntimeError):
    """Raised when the helper cannot locate a control or click a button.

    The message includes a snapshot of the child-control tree so test
    output is self-diagnostic.
    """


class OverwritePromptError(RuntimeError):
    """Raised when an overwrite confirmation dialog appears after a save.

    With unique-per-run output naming (``tests/helpers/output_paths``)
    this prompt should NEVER appear.  If it does, a test is using a
    hardcoded path instead of ``gen_out(tmp_path, ...)`` — fail
    immediately so the regression is obvious instead of letting the
    test silently accept and overwrite something.

    The watchdog screenshots the prompt to
    ``tests/visual/_failure_artifacts/`` before clicking No and
    raising, so the offending test's name + the target path can be
    diagnosed from the artifact filename + this exception's message.
    """


# ---------------------------------------------------------------------------
# Win32 child enumeration
# ---------------------------------------------------------------------------

def _class_name(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(128)
    _user32.GetClassNameW(hwnd, buf, 128)
    return buf.value or ""


def _window_text(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(512)
    _user32.GetWindowTextW(hwnd, buf, 512)
    return buf.value or ""


def _enum_descendants(root_hwnd: int) -> List[int]:
    """Return every descendant HWND of ``root_hwnd`` (depth-first order).

    EnumChildWindows already recurses, so a single call returns every
    descendant.  We keep the result list in the order the OS returns
    so callers see top-of-Z-order controls first.
    """
    out: List[int] = []

    def _cb(hwnd, _lp):
        out.append(int(hwnd))
        return True  # continue

    _user32.EnumChildWindows(
        wintypes.HWND(root_hwnd), _EnumChildWindowsProto(_cb), 0,
    )
    return out


def _wait_for_dialog_children(
    dialog_hwnd: int,
    min_count: int = 3,
    timeout: float = 3.0,
    interval: float = 0.1,
) -> int:
    """Poll until the dialog has at least ``min_count`` descendant HWNDs.

    GAP-29 fix: on Windows Server 2019 the IFileDialog often returns
    HWND before its inner child controls (file-name combo, Save/Cancel
    buttons, browse pane) have been instantiated.  Enumerating once
    immediately can yield (no children) which then fails both the
    EnumChildWindows path and the UIA fallback in a hard-to-diagnose
    way.  Poll up to ``timeout`` seconds for the child tree to populate
    before the caller proceeds with control lookups.

    Returns the final child count (which may be < min_count if the
    poll timed out — caller decides whether to fail or continue with
    the partial tree).
    """
    deadline = time.monotonic() + max(timeout, 0.0)
    count = 0
    while True:
        count = len(_enum_descendants(dialog_hwnd))
        if count >= min_count:
            return count
        if time.monotonic() >= deadline:
            return count
        time.sleep(interval)


def _describe_children(root_hwnd: int, limit: int = 40) -> str:
    """Return a multi-line diagnostic listing of the dialog's children."""
    lines: List[str] = []
    for hwnd in _enum_descendants(root_hwnd)[:limit]:
        cls = _class_name(hwnd)
        text = _window_text(hwnd)
        cid = _user32.GetDlgCtrlID(hwnd)
        vis = "vis" if _user32.IsWindowVisible(hwnd) else "hid"
        lines.append(
            f"  hwnd=0x{hwnd:x} id={cid:>5} {vis} class={cls!r:<22} "
            f"text={text!r}"
        )
    return "\n".join(lines) if lines else "  (no children)"


# ---------------------------------------------------------------------------
# Edit-control lookup (Approach A)
# ---------------------------------------------------------------------------

def _find_filename_edit(dialog_hwnd: int) -> int:
    """Locate the filename Edit control inside a Common Item Dialog.

    Strategy (in priority order):

    1. Edit whose immediate parent class is ``ComboBox`` whose
       grandparent class is ``ComboBoxEx32`` and whose dialog control
       id is 1148 — this is the standard IFileDialog filename combo on
       Win10 / Win11.
    2. Any visible+enabled Edit whose dialog control id is 1148 (in
       case the combo wrapping changes between Windows builds).
    3. Any visible+enabled Edit whose immediate parent is a ComboBox
       (covers the older XP-style File-Name combo).
    4. The first visible+enabled Edit control found (last-resort).

    Returns the HWND or 0 if nothing matched.
    """
    candidates = _enum_descendants(dialog_hwnd)
    edits: List[Tuple[int, int, str, str]] = []  # hwnd, ctrl_id, parent_cls, gp_cls
    for hwnd in candidates:
        if _class_name(hwnd) != "Edit":
            continue
        if not _user32.IsWindowVisible(hwnd):
            continue
        if not _user32.IsWindowEnabled(hwnd):
            continue
        cid = _user32.GetDlgCtrlID(hwnd)
        parent = _user32.GetParent(hwnd)
        gp = _user32.GetParent(parent) if parent else None
        parent_cls = _class_name(parent) if parent else ""
        gp_cls = _class_name(gp) if gp else ""
        edits.append((hwnd, cid, parent_cls, gp_cls))

    # Priority 1: the canonical IFileDialog filename combo.
    for hwnd, cid, pcls, gpcls in edits:
        if cid == 1148 and pcls == "ComboBox" and gpcls == "ComboBoxEx32":
            return hwnd

    # Priority 2: control id 1148 anywhere.
    for hwnd, cid, _pcls, _gpcls in edits:
        if cid == 1148:
            return hwnd

    # Priority 3: any Edit inside a ComboBox.
    for hwnd, _cid, pcls, _gpcls in edits:
        if pcls == "ComboBox":
            return hwnd

    # Priority 4: first visible+enabled Edit.
    if edits:
        return edits[0][0]

    return 0


def _find_button_by_label(dialog_hwnd: int, label: str) -> int:
    """Locate a Button control matching ``label`` (case-insensitive)."""
    label_norm = label.lower().lstrip("&").strip()
    for hwnd in _enum_descendants(dialog_hwnd):
        if _class_name(hwnd) != "Button":
            continue
        if not _user32.IsWindowVisible(hwnd):
            continue
        if not _user32.IsWindowEnabled(hwnd):
            continue
        text = _window_text(hwnd).lower().lstrip("&").strip()
        # Exact or prefix match — Windows sometimes appends "(O)" or
        # mnemonics depending on locale.
        if text == label_norm or text.startswith(label_norm):
            return hwnd
    return 0


def _find_button_idok(dialog_hwnd: int) -> int:
    """Locate the IDOK (control id 1) Button as a safe fallback."""
    for hwnd in _enum_descendants(dialog_hwnd):
        if _class_name(hwnd) != "Button":
            continue
        if not _user32.IsWindowVisible(hwnd):
            continue
        if _user32.GetDlgCtrlID(hwnd) == IDOK:
            return hwnd
    return 0


# ---------------------------------------------------------------------------
# UIA fallback (Approach B)
# ---------------------------------------------------------------------------

def _drive_via_uia(
    dialog_hwnd: int,
    filename: str,
    button_label: str,
) -> bool:
    """Pywinauto/UIA fallback for when SendMessage cannot find the Edit.

    This is slower (UIA round-trips) and slightly less deterministic
    (the button click goes through the input queue), but it succeeds
    on edge cases where the file dialog control IDs differ.
    """
    try:
        from pywinauto import Application
    except ImportError:
        return False

    try:
        app = Application(backend="uia").connect(handle=dialog_hwnd, timeout=5)
        dlg = app.window(handle=dialog_hwnd)

        # Try the canonical AutomationId for the filename Edit.
        edit = None
        for auto_id in ("1148",):
            try:
                edit = dlg.child_window(
                    auto_id=auto_id, control_type="Edit",
                ).wrapper_object()
                if edit is not None:
                    break
            except Exception:  # noqa: BLE001
                pass
        if edit is None:
            # Last-resort: any Edit descendant.
            try:
                edit = dlg.child_window(
                    control_type="Edit",
                ).wrapper_object()
            except Exception:  # noqa: BLE001
                return False
        if edit is None:
            return False

        edit.set_edit_text(filename)
        # Click OK / Save / Open by label.
        try:
            btn = dlg.child_window(
                title=button_label, control_type="Button",
            ).wrapper_object()
            btn.click_input()
            return True
        except Exception:  # noqa: BLE001
            # Try IDOK by AutomationId "1".
            try:
                btn = dlg.child_window(
                    auto_id="1", control_type="Button",
                ).wrapper_object()
                btn.click_input()
                return True
            except Exception:  # noqa: BLE001
                return False
    except Exception:  # noqa: BLE001
        return False


# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

def _read_edit_text(edit_hwnd: int) -> str:
    """Read the text out of an Edit control via WM_GETTEXT."""
    length = _user32.SendMessageW(edit_hwnd, WM_GETTEXTLENGTH, 0, 0)
    buf = ctypes.create_unicode_buffer(int(length) + 2)
    _user32.SendMessageW(
        edit_hwnd, WM_GETTEXT, len(buf), ctypes.addressof(buf),
    )
    return buf.value or ""


# ---------------------------------------------------------------------------
# Overwrite-prompt watchdog
# ---------------------------------------------------------------------------

# Substrings (case-insensitive) on a top-level window's title that
# identify an overwrite-confirmation prompt.  The native Common Item
# Dialog uses "Confirm Save As" on en-US Windows; Qt's QFileDialog
# (when used with custom widget mode) uses "File already exists" or
# "Save".  We match leniently because the prompt is the failure
# itself — the localization of the title is incidental.
_OVERWRITE_TITLES = (
    "confirm save as",
    "replace existing file?",
    "file already exists",
    "confirm save",
    "overwrite",
)


def _list_top_level_windows() -> List[Tuple[int, str]]:
    """Return [(hwnd, title), ...] for every visible top-level window."""
    out: List[Tuple[int, str]] = []

    def _cb(hwnd, _lp):
        if _user32.IsWindowVisible(hwnd):
            text = _window_text(hwnd)
            if text:
                out.append((int(hwnd), text))
        return True

    _user32.EnumWindows(_EnumWindowsProto(_cb), 0)
    return out


def _find_overwrite_prompt(deadline: float) -> Tuple[int, str]:
    """Poll for an overwrite-confirmation dialog.  Returns (hwnd, title)
    on success, (0, "") on timeout.
    """
    while time.monotonic() < deadline:
        for hwnd, title in _list_top_level_windows():
            low = title.lower()
            for needle in _OVERWRITE_TITLES:
                if needle in low:
                    return hwnd, title
        time.sleep(0.1)
    return 0, ""


def _take_overwrite_screenshot(target_path: str, title: str) -> str:
    """Capture a screenshot of the overwrite-prompt situation.

    Lands the file under ``tests/visual/_failure_artifacts/`` so the
    janitor's 20-file cap will eventually evict it.  Returns the path
    written, or an empty string if capture failed (we never want the
    screenshot path itself to mask the original OverwritePromptError).
    """
    try:
        # Lazy import — pyautogui pulls in pillow + tk; not all CI hosts
        # have these and we don't want to require them for non-UI tests.
        import pyautogui  # type: ignore  # noqa: PLC0415
    except ImportError:
        return ""

    try:
        run_id = os.environ.get("PDFE_TEST_RUN_ID", "norunid")
        ts = _dt.datetime.now().strftime("%Y%m%dT%H%M%Sp%f")
        artifacts = (
            Path(__file__).resolve().parent.parent
            / "visual" / "_failure_artifacts"
        )
        artifacts.mkdir(parents=True, exist_ok=True)
        out = artifacts / f"overwrite_prompt_{run_id}_{ts}.png"
        img = pyautogui.screenshot()
        img.save(str(out))
        return str(out)
    except Exception:  # noqa: BLE001
        return ""


def _click_overwrite_no(dialog_hwnd: int) -> bool:
    """Click "No" / "Cancel" on an overwrite-confirmation dialog.

    Returns True if a click was dispatched.  We never auto-accept — the
    test must fail loudly.
    """
    # Try the standard No / Cancel labels first.
    for label in ("No", "Cancel", "&No", "&Cancel"):
        btn = _find_button_by_label(dialog_hwnd, label)
        if btn:
            _user32.SendMessageW(btn, BM_CLICK, 0, 0)
            return True
    # Fall back to IDCANCEL.
    for hwnd in _enum_descendants(dialog_hwnd):
        if _class_name(hwnd) != "Button":
            continue
        if not _user32.IsWindowVisible(hwnd):
            continue
        if _user32.GetDlgCtrlID(hwnd) == IDCANCEL:
            _user32.SendMessageW(hwnd, BM_CLICK, 0, 0)
            return True
    return False


def accept_overwrite_prompt(
    dialog_hwnd: int = 0,
    timeout: float = 5.0,
) -> bool:
    """Click "Yes" / "OK" on an overwrite-confirmation dialog.

    P-G-INFRA-11 fix.  The previous approach sent ``pyautogui.press('y')``
    + ``press('enter')`` to the dialog's HWND via the focus-safe wrappers,
    which on this rig wedged for >70 min: the keystrokes were never
    consumed by the Confirm Save As prompt — either because focus
    contention prevented delivery, or because the OS shell consumes the
    'y' mnemonic only when the prompt explicitly has keyboard input.

    This helper bypasses the OS input queue entirely: it locates the
    Yes / OK button by label or by IDOK control id and sends BM_CLICK
    via SendMessageW.  BM_CLICK is synchronous in the dialog's own
    message loop, identical to a real mouse click, and cannot be lost
    to focus stealing.

    Parameters
    ----------
    dialog_hwnd
        HWND of the confirmation prompt.  When 0, the helper polls for
        a top-level window matching ``_OVERWRITE_TITLES`` (case-
        insensitive substring) for up to ``timeout`` seconds.
    timeout
        Seconds to wait for the prompt to appear when ``dialog_hwnd``
        is 0.  Ignored when an explicit HWND is provided.

    Returns
    -------
    True if a click was dispatched on a Yes / OK button.  False if
    no such button was found (caller may then fall back to keyboard
    input or fail the test).
    """
    if dialog_hwnd == 0:
        deadline = time.monotonic() + max(timeout, 0.0)
        dialog_hwnd, _title = _find_overwrite_prompt(deadline)
        if dialog_hwnd == 0:
            return False

    # Try the standard accept labels.  Order matters: prefer "Yes"
    # over "OK" because the Confirm Save As prompt always uses Yes/No.
    for label in ("Yes", "&Yes", "OK", "&OK"):
        btn = _find_button_by_label(dialog_hwnd, label)
        if btn:
            _user32.SendMessageW(btn, BM_CLICK, 0, 0)
            return True
    # Fall back to IDOK by control-id (button 1).
    btn = _find_button_idok(dialog_hwnd)
    if btn:
        _user32.SendMessageW(btn, BM_CLICK, 0, 0)
        return True
    return False


def _watchdog_for_overwrite_prompt(
    target_path: str,
    timeout: float = 1.5,
) -> None:
    """Poll for an overwrite-confirmation dialog and fail loudly if seen.

    With per-run output naming every save path is unique, so this prompt
    should never appear.  If it does, the test is using a hardcoded
    path — raise so the offender is obvious.

    Caller must invoke this AFTER ``_drive_dialog`` has clicked Save but
    BEFORE returning True, so the test sees the exception synchronously.
    """
    deadline = time.monotonic() + timeout
    hwnd, title = _find_overwrite_prompt(deadline)
    if not hwnd:
        return
    # Found it.  Screenshot, log, click No, raise.
    shot = _take_overwrite_screenshot(target_path, title)
    _click_overwrite_no(hwnd)
    raise OverwritePromptError(
        f"Overwrite-confirmation dialog appeared after Save: "
        f"title={title!r}, target_path={target_path!r}. "
        f"This indicates the test wrote to a path that already exists "
        f"— it MUST use tests.helpers.output_paths.gen_out(tmp_path, ...) "
        f"so output names are unique per run.  "
        f"Screenshot: {shot or '(capture failed)'}"
    )


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def _normalize_path(path: str) -> str:
    return path.replace("/", "\\")


def _drive_dialog(
    dialog_hwnd: int,
    filename: str,
    button_label: str,
    settle_after_set: float,
    settle_after_click: float,
) -> bool:
    """Internal — set filename and click OK button.

    Returns True on confirmed success (Edit text matches filename and
    a click was dispatched), raises DialogDriveError otherwise.
    """
    if not dialog_hwnd:
        raise DialogDriveError(
            "drive_dialog called with HWND=0 (dialog never appeared).",
        )

    filename = _normalize_path(filename)

    # GAP-29: wait for the dialog's child tree to populate before trying
    # to locate the filename Edit. On Windows Server 2019 the IFileDialog
    # HWND is returned by ShowDialog() before its inner controls are
    # created; enumerating immediately yields (no children).
    _wait_for_dialog_children(dialog_hwnd, min_count=3, timeout=3.0)

    # --- Approach A: SendMessage to Edit control ------------------------
    edit_hwnd = _find_filename_edit(dialog_hwnd)
    if edit_hwnd:
        # Bring the dialog forward so its message loop is responsive.
        _user32.SetForegroundWindow(wintypes.HWND(dialog_hwnd))
        _user32.SetFocus(wintypes.HWND(edit_hwnd))

        # Clear any existing text (IFileDialog pre-fills the suggested
        # name).  EM_SETSEL(0,-1) selects all, then WM_SETTEXT replaces.
        _user32.SendMessageW(edit_hwnd, EM_SETSEL, 0, -1)
        # WM_SETTEXT with a wide-string pointer.
        text_buf = ctypes.create_unicode_buffer(filename)
        rc = _user32.SendMessageW(
            edit_hwnd, WM_SETTEXT, 0, ctypes.addressof(text_buf),
        )
        if rc != 1:
            raise DialogDriveError(
                f"WM_SETTEXT to Edit hwnd=0x{edit_hwnd:x} returned {rc}; "
                f"dialog children:\n{_describe_children(dialog_hwnd)}"
            )

        # Verify the text actually landed.  If the Edit is inside a
        # ComboBoxEx32 the read-back can come from the combo, not the
        # raw Edit; treat a substring match (case-insensitive) as OK.
        if settle_after_set > 0:
            time.sleep(settle_after_set)
        actual = _read_edit_text(edit_hwnd)
        if filename.lower() not in actual.lower():
            raise DialogDriveError(
                f"WM_SETTEXT did not stick: wrote {filename!r} but read "
                f"back {actual!r} from hwnd=0x{edit_hwnd:x}.\n"
                f"Dialog children:\n{_describe_children(dialog_hwnd)}"
            )

        # --- Click the OK / Open / Save button -------------------------
        btn_hwnd = _find_button_by_label(dialog_hwnd, button_label)
        if not btn_hwnd:
            btn_hwnd = _find_button_idok(dialog_hwnd)
        if not btn_hwnd:
            raise DialogDriveError(
                f"Could not locate OK button (label={button_label!r} or "
                f"IDOK).\nDialog children:\n"
                f"{_describe_children(dialog_hwnd)}"
            )

        # Send BM_CLICK.  This is synchronous in the dialog's queue and
        # gives the same result as a real mouse click (sends WM_COMMAND
        # to the parent), without going through the OS input queue.
        _user32.SendMessageW(btn_hwnd, BM_CLICK, 0, 0)
        if settle_after_click > 0:
            time.sleep(settle_after_click)
        return True

    # --- Approach B: UIA fallback ---------------------------------------
    if _drive_via_uia(dialog_hwnd, filename, button_label):
        if settle_after_click > 0:
            time.sleep(settle_after_click)
        return True

    raise DialogDriveError(
        "Could not locate filename Edit via SendMessage or UIA.\n"
        f"Dialog children:\n{_describe_children(dialog_hwnd)}"
    )


def set_open_dialog_filename(
    dialog_hwnd: int,
    filename: str,
    button_label: str = "Open",
    settle_after_set: float = 0.05,
    settle_after_click: float = 1.5,
) -> bool:
    """Drive an Open File dialog: set filename and click Open.

    ``dialog_hwnd``
        HWND of the file dialog (use
        ``PDFESession.wait_for_top_level_window("Open PDF File", ...)``
        to obtain it after sending Ctrl+O).

    ``filename``
        Absolute path to the file to open.  Forward slashes are
        normalized to backslashes.

    ``button_label``
        Label of the OK button.  Defaults to "Open" but accepts the
        localized variant for non-English Windows.

    ``settle_after_click``
        Seconds to wait after dispatching BM_CLICK so the dialog has
        time to dismiss and the parent can process the file.  Default
        1.5s matches the previous pyautogui-based ``_settle(1.2)``
        behavior with extra headroom for slow disks.

    Returns True on success.  Raises DialogDriveError on failure.
    """
    return _drive_dialog(
        dialog_hwnd, filename, button_label,
        settle_after_set=settle_after_set,
        settle_after_click=settle_after_click,
    )


def set_save_dialog_filename(
    dialog_hwnd: int,
    filename: str,
    button_label: str = "Save",
    settle_after_set: float = 0.05,
    settle_after_click: float = 2.0,
    overwrite_watchdog_timeout: float = 1.5,
) -> bool:
    """Drive a Save File dialog: set filename and click Save.

    Same semantics as ``set_open_dialog_filename`` with a longer
    post-click settle (saves involve a write + redraw round-trip).

    Overwrite-prompt watchdog
    -------------------------
    After clicking Save, we poll up to ``overwrite_watchdog_timeout``
    seconds for a top-level "Confirm Save As" / "File already exists" /
    "Replace existing file?" dialog.  If one appears, the watchdog
    screenshots it to ``tests/visual/_failure_artifacts/``, clicks "No",
    and raises ``OverwritePromptError``.

    This is a safety net for tests that bypass
    ``tests.helpers.output_paths.gen_out`` and write to a non-unique
    path.  Pass ``overwrite_watchdog_timeout=0`` to disable (e.g. when
    intentionally testing the overwrite codepath itself).
    """
    rc = _drive_dialog(
        dialog_hwnd, filename, button_label,
        settle_after_set=settle_after_set,
        settle_after_click=settle_after_click,
    )
    if rc and overwrite_watchdog_timeout > 0:
        _watchdog_for_overwrite_prompt(
            filename,
            timeout=overwrite_watchdog_timeout,
        )
    return rc


# ---------------------------------------------------------------------------
# Filename-only setter (no button click) — used by the overwrite dispatcher
# ---------------------------------------------------------------------------

def set_save_dialog_filename_only(
    dialog_hwnd: int,
    filename: str,
    settle_after_set: float = 0.05,
) -> int:
    """Set the filename text in a Save dialog WITHOUT clicking Save.

    Returns the HWND of the Save / OK button so the caller can dispatch
    BM_CLICK via ``click_save_with_overwrite_dispatcher``.  Raises
    ``DialogDriveError`` if the Edit control or button cannot be found.

    This is split from ``set_save_dialog_filename`` so the caller can
    interpose a daemon thread between setting the filename and clicking
    the Save button — necessary when clicking Save shows a secondary
    modal (Confirm Save As) that must be dismissed from a different
    thread before the blocking SendMessageW can return.
    """
    if not dialog_hwnd:
        raise DialogDriveError(
            "set_save_dialog_filename_only called with HWND=0 "
            "(dialog never appeared)."
        )

    filename = _normalize_path(filename)

    # GAP-29: wait for child controls to populate (see _drive_dialog).
    _wait_for_dialog_children(dialog_hwnd, min_count=3, timeout=3.0)

    edit_hwnd = _find_filename_edit(dialog_hwnd)
    if not edit_hwnd:
        raise DialogDriveError(
            "Could not locate filename Edit control in Save dialog.\n"
            f"Dialog children:\n{_describe_children(dialog_hwnd)}"
        )

    _user32.SetForegroundWindow(wintypes.HWND(dialog_hwnd))
    _user32.SetFocus(wintypes.HWND(edit_hwnd))
    _user32.SendMessageW(edit_hwnd, EM_SETSEL, 0, -1)

    text_buf = ctypes.create_unicode_buffer(filename)
    rc = _user32.SendMessageW(
        edit_hwnd, WM_SETTEXT, 0, ctypes.addressof(text_buf),
    )
    if rc != 1:
        raise DialogDriveError(
            f"WM_SETTEXT to Edit hwnd=0x{edit_hwnd:x} returned {rc}; "
            f"dialog children:\n{_describe_children(dialog_hwnd)}"
        )

    if settle_after_set > 0:
        time.sleep(settle_after_set)

    actual = _read_edit_text(edit_hwnd)
    if filename.lower() not in actual.lower():
        raise DialogDriveError(
            f"WM_SETTEXT did not stick: wrote {filename!r} but read back "
            f"{actual!r} from hwnd=0x{edit_hwnd:x}.\n"
            f"Dialog children:\n{_describe_children(dialog_hwnd)}"
        )

    btn_hwnd = _find_button_by_label(dialog_hwnd, "Save")
    if not btn_hwnd:
        btn_hwnd = _find_button_idok(dialog_hwnd)
    if not btn_hwnd:
        raise DialogDriveError(
            "Could not locate Save / OK button in Save dialog.\n"
            f"Dialog children:\n{_describe_children(dialog_hwnd)}"
        )

    return btn_hwnd


# ---------------------------------------------------------------------------
# Daemon-thread dispatcher for overwrite-accepting Save clicks
# ---------------------------------------------------------------------------

def _kill_pdfe_process(pid: int) -> None:
    """Kill the PDFE process by PID.  First SIGTERM, then SIGKILL after 2 s.

    On Windows ``os.kill(pid, signal.SIGTERM)`` calls TerminateProcess
    immediately (SIGTERM and SIGKILL are both translated to TerminateProcess
    by the Windows CRT).  We send SIGTERM first for symmetry with the
    documented contract, then double-check and send SIGKILL if the
    process is still alive after the grace period.
    """
    try:
        os.kill(pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError, OSError):
        return

    # Grace period — on Windows TerminateProcess is effectively
    # instantaneous, but give the OS a moment to update the process table.
    time.sleep(2.0)

    # Check whether the process is still alive and escalate.
    try:
        result = subprocess_check_pid(pid)
        if result:
            os.kill(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        pass


def subprocess_check_pid(pid: int) -> bool:
    """Return True if a process with ``pid`` is currently running."""
    import subprocess  # noqa: PLC0415
    try:
        out = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        # tasklist outputs the PID in the CSV row if the process exists.
        return str(pid) in out.stdout
    except Exception:  # noqa: BLE001
        # If tasklist itself fails, fall back to os.kill(pid, 0).
        try:
            os.kill(pid, 0)
            return True
        except (ProcessLookupError, PermissionError, OSError):
            return False


def click_save_with_overwrite_dispatcher(
    save_button_hwnd: int,
    pdfe_pid: int,
    overall_timeout: float = 30.0,
    overwrite_poll_timeout: float = 15.0,
) -> bool:
    """Click the Save button on a native Save dialog while a daemon
    thread waits for and dismisses a Confirm Save As modal.

    Returns True if Save completed and (if it appeared) the overwrite
    prompt was dismissed.  Returns False on timeout.

    Architecture
    ------------
    1. Start a daemon thread that polls for the overwrite prompt for up
       to ``overwrite_poll_timeout`` seconds.  When it finds one it
       dispatches BM_CLICK on the Yes button via SendMessageW (safe —
       that send DOES return because the overwrite dialog is the
       topmost modal handler, not the original Save-As dialog).
    2. Start a watchdog ``threading.Timer`` that, if it fires before
       the test thread is unblocked, calls ``_kill_pdfe_process(pdfe_pid)``
       so PDFE shuts down and the blocking ``SendMessageW(Save)`` returns
       even if the daemon thread failed to find the prompt.  The Timer
       is cancelled on normal completion.
    3. Call ``SendMessageW(save_button_hwnd, BM_CLICK, 0, 0)``.  This
       blocks while Qt runs the Confirm Save As modal event loop.  The
       daemon thread (step 1) handles the modal; once Yes is clicked
       Qt unwinds the modal and SendMessageW returns.
    4. Cancel the watchdog timer.  Wait briefly for the daemon to finish.

    The OS-level kill guard exists because pytest-timeout cannot preempt
    a thread blocked in SendMessageW on Windows.  If the daemon thread
    fails to find / dismiss the prompt, the watchdog ensures the test
    does NOT hang indefinitely and does NOT leave PDFE.exe wedged for
    subsequent tests.

    Parameters
    ----------
    save_button_hwnd
        HWND of the Save button inside the native Save-As file dialog.
        Obtain via ``_find_button_by_label(dialog_hwnd, "Save")``.
    pdfe_pid
        Process ID of the running PDFE.exe instance.  Used by the
        watchdog kill fallback.
    overall_timeout
        How long (seconds) the watchdog waits before force-killing
        PDFE.  Should be comfortably larger than ``overwrite_poll_timeout``
        so the daemon has time to succeed first.
    overwrite_poll_timeout
        How long (seconds) the daemon thread polls for the overwrite
        prompt before giving up.  The watchdog fires ``overall_timeout``
        seconds after the BM_CLICK, so set this < overall_timeout.

    Returns
    -------
    bool
        True if SendMessageW returned (prompt dismissed by daemon OR no
        prompt appeared AND save succeeded).  False if the watchdog
        killed PDFE before SendMessageW could return.
    """
    # Shared result flag written by the daemon thread.
    daemon_result: List[bool] = [False]
    # Event set by the main thread to tell the daemon to stop early.
    stop_event = threading.Event()

    def _daemon_body() -> None:
        """Poll for the overwrite prompt and click Yes when it appears."""
        deadline = time.monotonic() + overwrite_poll_timeout
        while time.monotonic() < deadline and not stop_event.is_set():
            hwnd, _title = _find_overwrite_prompt(
                deadline=time.monotonic() + 0.25,
            )
            if hwnd:
                clicked = accept_overwrite_prompt(dialog_hwnd=hwnd, timeout=0)
                daemon_result[0] = clicked
                return
        # Prompt never appeared within poll timeout — that is fine;
        # the save may have succeeded without an overwrite prompt
        # (e.g. the file did not exist after all, or the dialog closed
        # for another reason).  Don't set daemon_result — caller checks
        # SendMessageW return instead.

    daemon = threading.Thread(target=_daemon_body, daemon=True, name="overwrite-dispatcher")

    # The watchdog kills PDFE if the main thread is still blocked after
    # overall_timeout seconds.  We arm it BEFORE starting the daemon and
    # BEFORE calling SendMessageW so the guard is always active.
    watchdog_fired: List[bool] = [False]

    def _watchdog_body() -> None:
        watchdog_fired[0] = True
        _kill_pdfe_process(pdfe_pid)

    watchdog = threading.Timer(overall_timeout, _watchdog_body)
    watchdog.daemon = True

    # --- Start daemon and watchdog, then issue the blocking BM_CLICK ---
    daemon.start()
    watchdog.start()

    try:
        _user32.SendMessageW(save_button_hwnd, BM_CLICK, 0, 0)
    finally:
        # Always cancel the watchdog and signal the daemon to stop.
        watchdog.cancel()
        stop_event.set()

    # Give the daemon thread a moment to finish cleanly.
    daemon.join(timeout=2.0)

    # If the watchdog fired, SendMessageW returned because PDFE was killed
    # — treat that as a non-successful save (caller decides how to proceed).
    if watchdog_fired[0]:
        return False

    return True
