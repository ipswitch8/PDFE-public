# -*- coding: utf-8 -*-
"""Focus-safe keystroke injection for PDFE UI tests (Pipeline F focus-safety
audit).

Background
----------
``pywinauto.keyboard.send_keys`` and ``pyautogui.{typewrite,press,hotkey}``
deliver keystrokes via the OS input queue (SendInput / keybd_event).  These
APIs route the input to **whichever window currently has foreground focus**,
not to a specific HWND.  If PDFE loses foreground focus mid-test (the user
clicks another window, a system notification appears, an OS focus-steal
fires), the keystrokes are typed into the wrong window — with the user
seeing fragments of test fixture paths appearing in Notepad / their browser /
their chat client.

This module provides a thin, deterministic wrapper that **forces PDFE (or a
specific dialog HWND) to be the foreground window before every key
injection**.  The wrapper retries on Windows' built-in foreground-lock
protection (``LockSetForegroundWindow``) using the
``AttachThreadInput`` workaround that has been the standard idiom since
Win2K.

Public API
----------
    ensure_foreground(hwnd, timeout=2.0) -> bool
        Force a HWND to the foreground.  Returns True on success.

    send_keys_to(hwnd, keys)
        Wraps ``pywinauto.keyboard.send_keys``.  Brings ``hwnd`` to
        foreground first; raises ``FocusError`` if that fails.

    typewrite_to(hwnd, text, interval=0.01)
        Wraps ``pyautogui.typewrite``.  Same focus guarantee.

    press_to(hwnd, key)
        Wraps ``pyautogui.press``.  Same focus guarantee.

    hotkey_to(hwnd, *keys)
        Wraps ``pyautogui.hotkey``.  Same focus guarantee.

    FocusError
        Raised when ``ensure_foreground`` cannot bring a HWND to the
        foreground within the timeout.

Why not just SetForegroundWindow?
---------------------------------
Windows blocks ``SetForegroundWindow`` from a process that is not already
the foreground process — to prevent malicious focus stealing.  The
documented workaround is to attach the calling thread's input queue to
the target thread's input queue (``AttachThreadInput``), which makes
Windows treat them as one input context for the duration of the call.

This module also calls ``AllowSetForegroundWindow(ASFW_ANY)`` and
``BringWindowToTop`` as belt-and-suspenders — none of these calls fail if
the others succeeded; we just want at least one path to win the race.

Native OS dialogs (file picker)
-------------------------------
The Common Item Dialog (file picker) helpers in ``tests/helpers/file_dialog.py``
already use ``WM_SETTEXT`` + ``BM_CLICK`` — direct messages to specific
HWNDs that bypass the OS input queue entirely.  Those paths do NOT need
focus and are not changed by this module.
"""

from __future__ import annotations

import ctypes
import time
from ctypes import wintypes
from typing import Iterable, Optional


# ---------------------------------------------------------------------------
# Win32 bindings
# ---------------------------------------------------------------------------

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

_user32.GetForegroundWindow.argtypes = []
_user32.GetForegroundWindow.restype = wintypes.HWND

_user32.GetWindowThreadProcessId.argtypes = [
    wintypes.HWND, ctypes.POINTER(wintypes.DWORD),
]
_user32.GetWindowThreadProcessId.restype = wintypes.DWORD

_user32.AttachThreadInput.argtypes = [
    wintypes.DWORD, wintypes.DWORD, wintypes.BOOL,
]
_user32.AttachThreadInput.restype = wintypes.BOOL

_user32.SetForegroundWindow.argtypes = [wintypes.HWND]
_user32.SetForegroundWindow.restype = wintypes.BOOL

_user32.SetActiveWindow.argtypes = [wintypes.HWND]
_user32.SetActiveWindow.restype = wintypes.HWND

_user32.SetFocus.argtypes = [wintypes.HWND]
_user32.SetFocus.restype = wintypes.HWND

_user32.BringWindowToTop.argtypes = [wintypes.HWND]
_user32.BringWindowToTop.restype = wintypes.BOOL

_user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.ShowWindow.restype = wintypes.BOOL

_user32.IsIconic.argtypes = [wintypes.HWND]
_user32.IsIconic.restype = wintypes.BOOL

_user32.IsWindowVisible.argtypes = [wintypes.HWND]
_user32.IsWindowVisible.restype = wintypes.BOOL

_user32.IsWindow.argtypes = [wintypes.HWND]
_user32.IsWindow.restype = wintypes.BOOL

_user32.AllowSetForegroundWindow.argtypes = [wintypes.DWORD]
_user32.AllowSetForegroundWindow.restype = wintypes.BOOL

_user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
_user32.GetAncestor.restype = wintypes.HWND

_kernel32.GetCurrentThreadId.argtypes = []
_kernel32.GetCurrentThreadId.restype = wintypes.DWORD

# ShowWindow nCmdShow constants
_SW_RESTORE = 9
_SW_SHOW = 5
# AllowSetForegroundWindow magic
_ASFW_ANY = 0xFFFFFFFF
# GetAncestor flags
_GA_ROOT = 2


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class FocusError(RuntimeError):
    """Raised when a target HWND cannot be brought to the foreground.

    This is intentionally distinct from generic RuntimeError so tests can
    catch focus-related failures specifically and emit a meaningful
    diagnostic ("the test could not reclaim PDFE focus") rather than
    silently typing keystrokes into the wrong window.
    """


# ---------------------------------------------------------------------------
# ensure_foreground — the core primitive
# ---------------------------------------------------------------------------

def _root_hwnd(hwnd: int) -> int:
    """Return the top-level (root) HWND for ``hwnd``.

    Modal dialogs sometimes report a child HWND; the foreground state lives
    on the root, so we promote before doing the dance.  Falls back to the
    input HWND when ``GetAncestor`` is not applicable.
    """
    if not hwnd:
        return 0
    root = _user32.GetAncestor(wintypes.HWND(hwnd), _GA_ROOT)
    return int(root) if root else int(hwnd)


def ensure_foreground(hwnd: int, timeout: float = 2.0,
                      poll: float = 0.05) -> bool:
    """Force ``hwnd`` (or its root ancestor) to be the foreground window.

    Strategy
    --------
    1. Promote ``hwnd`` to the root top-level window.
    2. If the window is minimized (iconic), call ``ShowWindow(SW_RESTORE)``.
    3. Loop until either ``GetForegroundWindow() == target`` or the
       timeout expires.  Each iteration:
         a. ``AllowSetForegroundWindow(ASFW_ANY)`` (giveth permission to
            ourselves and any child process; harmless if it fails).
         b. Attach this thread's input queue to the foreground thread's
            queue via ``AttachThreadInput`` so Windows treats us as the
            same input context.
         c. ``BringWindowToTop`` + ``SetForegroundWindow`` +
            ``SetActiveWindow`` + ``SetFocus`` — redundant on purpose.
         d. Detach the input queues.
         e. Sleep ``poll`` seconds and retest.

    Returns
    -------
    True if the target became foreground within ``timeout``, False
    otherwise.  Returning False is the cue for callers to raise
    ``FocusError`` rather than silently send keys to the wrong window.
    """
    if not hwnd:
        return False
    target = _root_hwnd(hwnd)
    if not target or not _user32.IsWindow(wintypes.HWND(target)):
        return False

    # Restore from minimized state if necessary.  A minimized window can
    # never become foreground without being un-minimized first.
    if _user32.IsIconic(wintypes.HWND(target)):
        _user32.ShowWindow(wintypes.HWND(target), _SW_RESTORE)

    deadline = time.monotonic() + timeout
    current_tid = _kernel32.GetCurrentThreadId()

    while time.monotonic() < deadline:
        # Already foreground? Done.
        cur = _user32.GetForegroundWindow()
        if cur and int(cur) == target:
            return True

        # Determine the foreground window's thread id (may be 0 if there
        # is no foreground window — e.g. lock screen, RDP transition).
        fg_tid = wintypes.DWORD(0)
        if cur:
            _user32.GetWindowThreadProcessId(cur, ctypes.byref(fg_tid))

        attached = False
        try:
            # Attach input queues so SetForegroundWindow doesn't get
            # vetoed by Windows' foreground-lock protection.
            if fg_tid.value and fg_tid.value != current_tid:
                if _user32.AttachThreadInput(
                    current_tid, fg_tid.value, True,
                ):
                    attached = True

            # Best-effort permission grant; harmless on failure.
            try:
                _user32.AllowSetForegroundWindow(_ASFW_ANY)
            except Exception:  # noqa: BLE001
                pass

            _user32.BringWindowToTop(wintypes.HWND(target))
            _user32.ShowWindow(wintypes.HWND(target), _SW_SHOW)
            _user32.SetForegroundWindow(wintypes.HWND(target))
            _user32.SetActiveWindow(wintypes.HWND(target))
            _user32.SetFocus(wintypes.HWND(target))
        finally:
            if attached:
                _user32.AttachThreadInput(
                    current_tid, fg_tid.value, False,
                )

        # Verify.
        cur = _user32.GetForegroundWindow()
        if cur and int(cur) == target:
            return True

        time.sleep(poll)

    # One final check after the loop.
    cur = _user32.GetForegroundWindow()
    return bool(cur and int(cur) == target)


# ---------------------------------------------------------------------------
# Lazy backend imports so this module imports on non-Windows CI.
# ---------------------------------------------------------------------------

def _import_pyautogui():
    import pyautogui  # pylint: disable=import-outside-toplevel
    # Fail-safe is a user-protection feature for interactive scripts; in
    # an automated UI test, the mouse may legitimately end up at a screen
    # corner mid-test (e.g. when a dialog's signature widget is clipped
    # to the screen edge), and PyAutoGUI's FailSafeException then masks
    # whatever the test was actually doing.  Disable it so legitimate
    # cursor moves don't abort the test run.  See P13-INFRA fixes,
    # KNOWN_WEAK_TESTS.txt.
    pyautogui.FAILSAFE = False
    return pyautogui


def _import_pywinauto_keyboard():
    from pywinauto import keyboard  # pylint: disable=import-outside-toplevel
    return keyboard


# ---------------------------------------------------------------------------
# Public focus-safe wrappers
# ---------------------------------------------------------------------------

def _require_focus(hwnd: int, timeout: float, op_label: str) -> None:
    """Raise FocusError with a useful diagnostic if focus can't be
    obtained.  The op_label appears in the message so test failures
    point straight at the offending call site."""
    if hwnd <= 0:
        raise FocusError(
            f"{op_label}: HWND={hwnd!r} is not a valid window handle. "
            "Pass session.hwnd (or the dialog's HWND from "
            "session.wait_for_top_level_window)."
        )
    if not ensure_foreground(hwnd, timeout=timeout):
        cur = _user32.GetForegroundWindow()
        cur_int = int(cur) if cur else 0
        raise FocusError(
            f"{op_label}: could not bring HWND 0x{hwnd:x} to foreground "
            f"within {timeout:.1f}s (current foreground = 0x{cur_int:x}). "
            "Refusing to send keys — they would route to the wrong window."
        )


def send_keys_to(hwnd: int, keys: str,
                 focus_timeout: float = 2.0,
                 with_spaces: bool = True,
                 with_tabs: bool = True,
                 with_newlines: bool = True,
                 pause: Optional[float] = None) -> None:
    """Send a pywinauto SendKeys-syntax string to ``hwnd``.

    Brings ``hwnd`` to the foreground first.  Raises ``FocusError`` if
    that fails — never silently sends keys to whatever has focus.

    The remaining kwargs forward to ``pywinauto.keyboard.send_keys``.
    """
    _require_focus(hwnd, focus_timeout, "send_keys_to")
    keyboard = _import_pywinauto_keyboard()
    keyboard.send_keys(
        keys,
        with_spaces=with_spaces,
        with_tabs=with_tabs,
        with_newlines=with_newlines,
        pause=pause if pause is not None else 0.05,
    )


def _hwnd_pid(hwnd: int) -> int:
    """Return the process ID that owns *hwnd*, or 0 on failure."""
    if not hwnd:
        return 0
    pid = wintypes.DWORD(0)
    _user32.GetWindowThreadProcessId(wintypes.HWND(hwnd), ctypes.byref(pid))
    return int(pid.value)


def verify_foreground_is(expected_hwnd: int) -> bool:
    """Return True if the OS foreground window belongs to the same process as
    ``expected_hwnd``.

    Use this for "I'm about to send a keystroke and I'm relying on the
    foreground window being PDFE because a prior click_input() established
    that" — verify just before the send, so we fail fast (FocusError)
    rather than leak the keystroke into the user's email / browser /
    explorer if focus escaped between the click and the send.

    **PID-based comparison (not HWND-root comparison)**: Qt popup windows
    (sticky-note text editors, free-text inline editors) open as separate
    top-level HWNDs owned by the PDFE process.  Comparing root HWNDs would
    return False even though the popup IS the legitimate foreground and IS
    the correct keystroke target.  Comparing PIDs allows any PDFE-owned
    top-level window to be accepted as foreground — the safety invariant
    (keystrokes must not leak to a different application) is preserved
    because PIDs differ across processes.

    Falls back to root-HWND equality if PID resolution fails (returns 0),
    e.g. on protected system windows.

    Returns False (do not raise) so callers can compose this into
    try/fallback logic; raise FocusError yourself when ``False`` is the
    wrong answer for the call site.
    """
    if not expected_hwnd:
        return False
    cur = _user32.GetForegroundWindow()
    if not cur:
        return False
    cur_pid = _hwnd_pid(int(cur))
    expected_pid = _hwnd_pid(int(expected_hwnd))
    if cur_pid and expected_pid:
        return cur_pid == expected_pid
    # Fallback: root-HWND equality (pre-PID behaviour).
    cur_root = _root_hwnd(int(cur))
    expected_root = _root_hwnd(int(expected_hwnd))
    return cur_root == expected_root


def _foreground_ok_or_recover(expected_hwnd: int, recover: bool) -> bool:
    """True if ``expected_hwnd``'s process owns the foreground — optionally
    attempting ONE bounded recovery first.

    ``recover=True`` handles the "an unrelated window popped up and stole the
    foreground" case (e.g. an app launched by a stray click): it calls
    ``ensure_foreground()``, which fights the target window back to the front
    (un-minimise + AttachThreadInput + SetForegroundWindow, retried for up to
    2 s), then RE-VERIFIES. The keystroke is still only sent once the intended
    window is confirmed foreground, so the no-leak guarantee is preserved — we
    recover instead of failing a test over a transient popup.

    *** WHEN NOT TO USE recover=True ***
    ONLY safe for WINDOW-LEVEL keystrokes that do not depend on a caret sitting
    in a particular widget — Escape/Enter dialog dismissals, Ctrl+Z undo, etc.

    NEVER enable it after a ``click_input()`` that focused a SPECIFIC FIELD (the
    QColorDialog HTML hex box, an annotation-contents edit, a file-dialog
    filename box). ``ensure_foreground()`` re-asserts SetForegroundWindow /
    SetActiveWindow / SetFocus, which resets Qt's focusWidget: the text would
    then land in the window but NOT in the intended field, silently producing a
    WRONG-BUT-PASSING or subtly-invalid test instead of an honest failure.
    Likewise avoid it mid-sequence where a transient UI state matters (an open
    Alt-menu closes when foreground is re-asserted). In those cases keep the
    default fail-fast and let the file-level rerun handle a transient popup.
    """
    if verify_foreground_is(expected_hwnd):
        return True
    if not recover:
        return False
    ensure_foreground(expected_hwnd)
    return verify_foreground_is(expected_hwnd)


def typewrite_assuming_foreground(expected_hwnd: int, text: str,
                                  interval: float = 0.04) -> None:
    """Type ``text`` only if the foreground window belongs to the same process
    as ``expected_hwnd``.

    Use this immediately after a click_input/mouse_event that should have
    brought ``expected_hwnd`` (or a PDFE-owned popup, e.g. a sticky-note
    editor or free-text box) to the foreground. Unlike ``typewrite_to``
    (which re-asserts foreground via SetForegroundWindow + AttachThreadInput),
    this preserves the Qt focusWidget that the prior click established —
    Qt would otherwise reset it on a fresh SetFocus. But it raises
    FocusError if focus escaped to a *different process*, so keystrokes never
    leak to an unrelated application.

    "Same process" semantics: ``verify_foreground_is`` compares PIDs, so any
    PDFE-owned top-level window (main window, popup, dialog) satisfies the
    check.

    NOTE — this helper deliberately has NO ``recover=`` option (unlike
    ``press_/hotkey_assuming_foreground``). Typing is always aimed at a caret in
    a specific field that a prior click_input() established; recovering via
    ``ensure_foreground()`` re-asserts SetFocus and resets Qt's focusWidget, so
    the text would land in the window but NOT the intended field — a silently
    wrong test rather than an honest failure. Always fail fast here and let the
    file-level rerun absorb a transient popup.
    """
    if not verify_foreground_is(expected_hwnd):
        raise FocusError(
            "typewrite_assuming_foreground: foreground window is not the "
            "expected HWND — refusing to send keystrokes that would leak "
            "to an unrelated window."
        )
    pg = _import_pyautogui()
    pg.typewrite(text, interval=interval)


def press_assuming_foreground(expected_hwnd: int, key: str,
                              recover: bool = False) -> None:
    """Press a single key only if the foreground window belongs to the same
    process as ``expected_hwnd``.

    Same rationale as ``typewrite_assuming_foreground`` — preserves Qt's
    focusWidget where a prior click placed it, fails fast (FocusError) if
    focus escaped to a different process. PDFE-owned popup windows (sticky-note
    editors, free-text boxes) satisfy the same-process check and are accepted.

    :param recover: When True, an unexpected foreground steal (a stray popup /
        an app launched by a mis-fired click) is RECOVERED from: PDFE is fought
        back to the foreground and the check re-run, and only a failed recovery
        raises. Enable ONLY for window-level keys (Escape / Enter dismissals) —
        never where a caret in a specific field matters. See
        ``_foreground_ok_or_recover``.
    """
    if not _foreground_ok_or_recover(expected_hwnd, recover):
        raise FocusError(
            "press_assuming_foreground: foreground window is not the "
            "expected HWND — refusing to press a key that would leak to "
            "an unrelated window."
        )
    pg = _import_pyautogui()
    pg.press(key)


def hotkey_assuming_foreground(expected_hwnd: int, *keys: str,
                               recover: bool = False) -> None:
    """Send a chord (e.g. Ctrl+A) only if the foreground window belongs to the
    same process as ``expected_hwnd``.

    Same rationale as ``typewrite_assuming_foreground`` — use immediately after a
    click_input() that should have focused a PDFE-owned window/field. Raises
    FocusError if focus escaped to a different process, so a stray ``ctrl+a``
    (and the text that usually follows it) never leaks to an unrelated window.
    This is the guarded replacement for bare ``pyautogui.hotkey(...)``.

    :param recover: When True, an unexpected foreground steal is RECOVERED from
        (PDFE is fought back to the front, then re-verified). Enable ONLY for
        window-level chords (e.g. Ctrl+Z undo) — NEVER for a chord that acts on
        a caret in a specific field (e.g. the Ctrl+A that precedes typing into a
        clicked-into edit box), because re-asserting foreground resets Qt's
        focusWidget and the chord would act on the wrong widget. See
        ``_foreground_ok_or_recover``.
    """
    if not _foreground_ok_or_recover(expected_hwnd, recover):
        raise FocusError(
            "hotkey_assuming_foreground: foreground window is not the "
            "expected HWND — refusing to send a chord that would leak to "
            "an unrelated window."
        )
    pg = _import_pyautogui()
    pg.hotkey(*keys)


def typewrite_to(hwnd: int, text: str,
                 interval: float = 0.01,
                 focus_timeout: float = 2.0) -> None:
    """Send literal printable text to ``hwnd`` via pyautogui.typewrite.

    Use this for paths / passwords / fixture text — anything that should
    be interpreted as raw characters (no SendKeys-syntax modifiers like
    ``^`` for Ctrl).
    """
    _require_focus(hwnd, focus_timeout, "typewrite_to")
    pg = _import_pyautogui()
    pg.typewrite(text, interval=interval)


def press_to(hwnd: int, key: str,
             focus_timeout: float = 2.0,
             pause: Optional[float] = None) -> None:
    """Press a single key (e.g. 'enter', 'escape', 'tab') against ``hwnd``.

    ``pause`` -- seconds to wait after the key, overriding PyAutoGUI's global
    ``PAUSE``. See :func:`hotkey_to` for why this parameter exists. ``None``
    (the default) keeps the global value, so existing callers are unaffected.
    """
    _require_focus(hwnd, focus_timeout, "press_to")
    pg = _import_pyautogui()
    if pause is None:
        pg.press(key)
    else:
        pg.press(key, _pause=False)
        if pause > 0:
            time.sleep(pause)


def press_to_own_foreground(pid: int, key: str,
                            pause: Optional[float] = None) -> bool:
    """Press `key` at whatever window of process `pid` currently has focus.

    Returns False without sending anything when the foreground window belongs
    to another process -- the same refusal `_require_focus` makes, for the same
    reason: a keystroke sent blind goes to whoever is in front.

    GitHub #238. This exists for dismissing a popup the application just opened
    ITSELF. `press_to(mainWindowHwnd, "escape")` forces the MAIN window to the
    foreground first, which means fighting the popup that legitimately has
    focus: ensure_foreground() polls at 50 ms and calls the whole
    AttachThreadInput / BringWindowToTop / SetForegroundWindow / SetFocus
    sequence each round. Measured at ~170 ms per Escape across 1,000
    creations -- 170 s, 79 % of the create loop, spent taking focus away from
    the very window we are about to close.

    Pressing at the foreground window is also the more faithful gesture: a user
    dismissing a popup presses Escape while the popup has focus, not after
    wrestling the main window in front of it.
    """
    cur = _user32.GetForegroundWindow()
    if not cur:
        return False
    owner = wintypes.DWORD(0)
    _user32.GetWindowThreadProcessId(wintypes.HWND(int(cur)),
                                     ctypes.byref(owner))
    if not owner.value or owner.value != pid:
        return False
    pg = _import_pyautogui()
    if pause is None:
        pg.press(key)
    else:
        pg.press(key, _pause=False)
        if pause > 0:
            time.sleep(pause)
    return True


def hotkey_to(hwnd: int, *keys: str,
              focus_timeout: float = 2.0,
              pause: Optional[float] = None) -> None:
    """Send a key chord (e.g. ``hotkey_to(hwnd, 'ctrl', 'shift', 'e')``).

    ``pause`` -- seconds to wait after the chord, overriding PyAutoGUI's global
    ``PAUSE`` (0.1 s).

    GitHub #238. That global is applied by PyAutoGUI's own decorator after
    every call, so a test driving a few keystrokes never notices it, and a test
    driving thousands pays it as the dominant cost of the run: measured at
    202 s of a 285 s stress scenario, 71 % of the total, for 2,020 chords.
    `_keyDown`/`_keyUp` are SendInput and return immediately, so the
    application processes the keystroke DURING that sleep -- the pause buys no
    settling that the application actually needs, it only serialises the
    sender.

    A caller in that position passes its own value, so the delay it waits for
    is stated in its own source rather than inherited invisibly. ``None`` keeps
    the global, so every existing caller is unchanged.
    """
    if not keys:
        raise ValueError("hotkey_to requires at least one key.")
    _require_focus(hwnd, focus_timeout, "hotkey_to")
    pg = _import_pyautogui()
    if pause is None:
        pg.hotkey(*keys)
    else:
        pg.hotkey(*keys, _pause=False)
        if pause > 0:
            time.sleep(pause)


# ---------------------------------------------------------------------------
# Convenience: re-acquire focus repeatedly during a sequence of inputs.
#
# Some tests issue several keystrokes in succession (open dialog,
# navigate menu, type filename).  A foreign focus-steal between two
# of those keystrokes can only be defended against by re-checking
# foreground state for *each* injection.  These helpers do that.
# ---------------------------------------------------------------------------

def send_sequence_to(hwnd: int,
                     ops: Iterable[tuple],
                     focus_timeout: float = 2.0) -> None:
    """Run a sequence of focus-safe key operations against ``hwnd``.

    Each ``ops`` element is a tuple ``(kind, *args)`` where ``kind`` is
    one of:

        ("send_keys", str)               -> send_keys_to(hwnd, str)
        ("typewrite", str)               -> typewrite_to(hwnd, str)
        ("typewrite", str, interval)     -> typewrite_to(hwnd, str, interval)
        ("press", str)                   -> press_to(hwnd, str)
        ("hotkey", *keys)                -> hotkey_to(hwnd, *keys)
        ("sleep", float)                 -> time.sleep(float)

    Focus is re-verified before every key op (not before the sleep).
    """
    for op in ops:
        if not op:
            continue
        kind = op[0]
        if kind == "send_keys":
            send_keys_to(hwnd, op[1], focus_timeout=focus_timeout)
        elif kind == "typewrite":
            interval = op[2] if len(op) > 2 else 0.01
            typewrite_to(hwnd, op[1], interval=interval,
                         focus_timeout=focus_timeout)
        elif kind == "press":
            press_to(hwnd, op[1], focus_timeout=focus_timeout)
        elif kind == "hotkey":
            hotkey_to(hwnd, *op[1:], focus_timeout=focus_timeout)
        elif kind == "sleep":
            time.sleep(float(op[1]))
        else:
            raise ValueError(f"send_sequence_to: unknown op {kind!r}")
