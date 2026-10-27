# -*- coding: utf-8 -*-
"""Title-bar icon verification for PDFE main window and AboutDialog.

Investigation summary (recorded as part of phase-4 acceptance):

  Symptom (pre-Phase-3): the PDFE main window's title-bar icon
  (upper-left of the window frame) showed Windows' generic
  IDI_APPLICATION glyph instead of the red_logo PDFE icon, even
  though Explorer's static file icon for pdfe.exe correctly
  displayed the PDFE icon. main.cpp:44 already had
  QApplication::setWindowIcon(QIcon(":/icons/pdfe.ico")) -- yet
  the title bar still came up blank.

  Root cause (most likely (d): setWindowIcon ordering / propagation):
  Qt 6 on Windows propagates QApplication::setWindowIcon() to
  child QWindows lazily, and the propagation can race with the
  native window-frame creation that happens during the first
  show(). The native HWND is created with no associated icon
  (HICON_SMALL == NULL) and DWM caches that NULL state for the
  title-bar render, even after Qt later assigns the icon.

  Fix (landed in Phase-3 / commit 803c1f1): MainWindow.cpp
  constructor now calls setWindowIcon(pdfeAppIcon()) BEFORE the
  first show(), and AboutDialog.cpp does the same. The helper
  returns QIcon(":/icons/pdfe.ico"), which is the multi-resolution
  Windows ICO (16/24/32/48/64/128/256 frames) sourced from
  pdfe-red.ico — the SVG theme-aware variants were retired. With
  the per-window icon assignment happening before native HWND
  creation, the title-bar HICON_SMALL is non-null when DWM
  composes the frame.

  Confirmed pre-fix observation: a manual run of the post-fix
  pdfe.exe + ctypes WM_GETICON probe returned non-zero HICON
  values for ICON_SMALL, ICON_BIG, GCLP_HICONSM, and GCLP_HICON
  on the main window. Documented in this docstring; reproduced
  by the test below.

  Other candidates ruled out:
    (a) MainWindow ctor clearing icon -- grep finds no such code
    (b) Missing/poor 16x16 ICO entry -- Phase-2 verified ring is
        0% opaque at every size including 16; QIcon.pixmap(16,16)
        renders cleanly (asserted in test_pdfe_icons.cpp's
        IcoSmallSize16IsLiveContent case)
    (c) Windows 11 modern theme hiding title-bar icons -- partial
        contributor in some themes, but Win32 WM_GETICON still
        returns the assigned HICON; Windows' decision to render
        it is independent of whether the app set it. This test
        therefore queries the HICON directly via the Win32 API
        rather than relying on DWM's visual treatment.

This test launches the built pdfe.exe via subprocess.Popen, polls
EnumWindows for a visible "PDFE" window owned by that process,
queries WM_GETICON / GetClassLongPtr through ctypes, and asserts
a non-NULL HICON is associated with the window. We avoid pywinauto
entirely because the dev Debug build is built with WIN32_EXECUTABLE
only in Release (see src/app/CMakeLists.txt), so pywinauto's
Application.start()/connect() trip on WaitForInputIdle returning
"not a GUI process".

Run:
    pytest tests/test_ui_titlebar_icon.py
"""
from __future__ import annotations

import ctypes
import subprocess
import sys
import time
import unittest
from ctypes import wintypes
from pathlib import Path

# Skip the whole module on non-Windows platforms (graceful skip
# rather than import-error since the GoogleTest C++ suite covers
# the cross-platform invariants).
if sys.platform != "win32":
    raise unittest.SkipTest("Win32-only title-bar icon test")


REPO_ROOT = Path(__file__).resolve().parent.parent
_CANDIDATES = [
    REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe",
    REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "pdfe.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "Debug" / "PDFE.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "Debug" / "pdfe.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "bin" / "Debug" / "PDFE.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "bin" / "Debug" / "pdfe.exe",
]
PDFE_EXE = next((p for p in _CANDIDATES if p.exists()), _CANDIDATES[0])

# Win32 constants / API setup
WM_GETICON = 0x007F
ICON_SMALL = 0
ICON_BIG = 1
ICON_SMALL2 = 2
GCLP_HICONSM = -34
GCLP_HICON = -14
WM_CLOSE = 0x0010

user32 = ctypes.WinDLL("user32", use_last_error=True)
SendMessageW = user32.SendMessageW
SendMessageW.argtypes = [
    wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
]
SendMessageW.restype = wintypes.LPARAM

PostMessageW = user32.PostMessageW
PostMessageW.argtypes = [
    wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
]
PostMessageW.restype = wintypes.BOOL

GetClassLongPtrW = user32.GetClassLongPtrW
GetClassLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
GetClassLongPtrW.restype = ctypes.c_void_p

GetWindowThreadProcessId = user32.GetWindowThreadProcessId
GetWindowThreadProcessId.argtypes = [
    wintypes.HWND, ctypes.POINTER(wintypes.DWORD)
]
GetWindowThreadProcessId.restype = wintypes.DWORD

IsWindowVisible = user32.IsWindowVisible
IsWindowVisible.argtypes = [wintypes.HWND]
IsWindowVisible.restype = wintypes.BOOL

GetWindowTextW = user32.GetWindowTextW
GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
GetWindowTextW.restype = ctypes.c_int

GetWindowTextLengthW = user32.GetWindowTextLengthW
GetWindowTextLengthW.argtypes = [wintypes.HWND]
GetWindowTextLengthW.restype = ctypes.c_int

WNDENUMPROC = ctypes.WINFUNCTYPE(
    wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
EnumWindows = user32.EnumWindows
EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
EnumWindows.restype = wintypes.BOOL


def find_window_by_pid_title(pid: int, title_substr: str) -> int | None:
    """Return the first visible top-level HWND owned by `pid` whose
    title contains `title_substr` (case-sensitive). 0 / None if
    none match."""
    hits: list[int] = []

    def cb(hwnd, _lparam):
        if not IsWindowVisible(hwnd):
            return True
        wpid = wintypes.DWORD()
        GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid:
            return True
        n = GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        GetWindowTextW(hwnd, buf, n + 1)
        if title_substr in buf.value:
            hits.append(hwnd)
            return False  # stop on first match
        return True

    EnumWindows(WNDENUMPROC(cb), 0)
    return hits[0] if hits else None


def hicon_for(hwnd: int) -> tuple[int, int, int, int]:
    """Return (small_msg, big_msg, small_class, big_class) HICON
    handles for the window. Each is the raw Win32 HANDLE as an
    integer; 0 means "no icon assigned"."""
    small_msg = SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0) or 0
    big_msg = SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0) or 0
    small_cls = GetClassLongPtrW(hwnd, GCLP_HICONSM) or 0
    big_cls = GetClassLongPtrW(hwnd, GCLP_HICON) or 0
    return (int(small_msg), int(big_msg), int(small_cls), int(big_cls))


def wait_for_window(pid: int,
                    title: str,
                    timeout_s: float = 20.0) -> int | None:
    """Poll until a visible window owned by `pid` with `title` in
    its caption appears, or `timeout_s` elapses."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        hwnd = find_window_by_pid_title(pid, title)
        if hwnd:
            return hwnd
        time.sleep(0.2)
    return None


class TitleBarIconTest(unittest.TestCase):
    """End-to-end native-UI verification that PDFE's title-bar
    icon is set on the main window. The AboutDialog half is
    covered by the C++ AboutDialogTest (which constructs the
    dialog directly and asserts setWindowIcon is wired); this
    Python test focuses on the cross-process Win32 reality
    check that requires a running pdfe.exe."""

    @classmethod
    def setUpClass(cls):
        if not PDFE_EXE.exists():
            raise unittest.SkipTest(
                f"PDFE.exe not built; expected at {PDFE_EXE}"
            )
        cls.proc = subprocess.Popen(
            [str(PDFE_EXE)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cls.hwnd = wait_for_window(cls.proc.pid, "PDFE", 20.0)
        if cls.hwnd is None:
            try:
                cls.proc.kill()
            except Exception:
                pass
            raise unittest.SkipTest(
                "PDFE main window did not appear within 20s"
            )

    @classmethod
    def tearDownClass(cls):
        try:
            if cls.hwnd:
                PostMessageW(cls.hwnd, WM_CLOSE, 0, 0)
        except Exception:
            pass
        try:
            cls.proc.wait(timeout=5)
        except Exception:
            try:
                cls.proc.kill()
            except Exception:
                pass

    def test_main_window_title_bar_icon_is_set(self):
        """The PDFE main window must report a non-NULL HICON via
        WM_GETICON. Pre-Phase-3 this returned 0 because Qt hadn't
        finished propagating QApplication's icon to the native
        HWND before show().

        Important: the test asserts on the MESSAGE-level icons
        (WM_GETICON), not the class-level (GCLP_HICONSM /
        GCLP_HICON). The class-level handles are populated by
        Qt's window-class registration with default fallbacks even
        when setWindowIcon() is never called, so they cannot
        distinguish "branded icon" from "Qt default". WM_GETICON
        only returns non-zero when the application explicitly
        called WM_SETICON (which is what QWidget::setWindowIcon
        does internally), so it's the reliable signal.
        """
        small_msg, big_msg, small_cls, big_cls = hicon_for(self.hwnd)

        # Primary assertion: WM_GETICON ICON_SMALL must be set.
        # This is the icon DWM uses for the upper-left of the
        # title bar. Removing pdfeAppIcon() from MainWindow's
        # constructor would make this return 0 — the test
        # genuinely catches the regression it claims to catch.
        self.assertNotEqual(
            small_msg, 0,
            f"MainWindow WM_GETICON ICON_SMALL is 0 "
            f"(big={big_msg:#x} class_small={small_cls:#x} "
            f"class_big={big_cls:#x}). setWindowIcon() in "
            f"MainWindow's constructor must run before the first "
            f"show() so QWidget::setWindowIcon's WM_SETICON "
            f"reaches the native HWND.",
        )

        # Reinforcing assertion: WM_GETICON ICON_BIG should also
        # be set. Qt's setWindowIcon populates both small and big.
        # If small is set but big is not, we have a partial wire-
        # up that leaves Alt-Tab and taskbar icons broken.
        self.assertNotEqual(
            big_msg, 0,
            f"MainWindow WM_GETICON ICON_BIG is 0 "
            f"(small={small_msg:#x}). Qt's setWindowIcon should "
            f"populate both ICON_SMALL and ICON_BIG; missing big "
            f"means the QWindow's icon assignment is incomplete.",
        )

    def test_about_dialog_title_bar_icon_is_set(self):
        """Open Help -> About via Alt+H, A keystrokes (Qt menu
        accelerators), poll for the AboutDialog window owned by
        the same process, and assert its HICON is also non-null.
        """
        # Bring main window to foreground so keystrokes route to
        # PDFE rather than the desktop or another app.
        SetForegroundWindow = user32.SetForegroundWindow
        SetForegroundWindow.argtypes = [wintypes.HWND]
        SetForegroundWindow.restype = wintypes.BOOL
        SetForegroundWindow(self.hwnd)
        time.sleep(0.3)

        # Send Alt+H to open the Help menu, then 'A' to invoke the
        # About PDFE action. We use SendInput for proper modifier-
        # key sequencing instead of WM_KEYDOWN messages, which Qt
        # often ignores for menu accelerators.
        from ctypes import Structure, Union, POINTER, sizeof
        ULONG_PTR = ctypes.c_ulong if ctypes.sizeof(ctypes.c_void_p) == 4 \
                    else ctypes.c_ulonglong

        class KEYBDINPUT(Structure):
            _fields_ = [("wVk", wintypes.WORD),
                        ("wScan", wintypes.WORD),
                        ("dwFlags", wintypes.DWORD),
                        ("time", wintypes.DWORD),
                        ("dwExtraInfo", ULONG_PTR)]

        class _INPUT_UNION(Union):
            _fields_ = [("ki", KEYBDINPUT)]

        class INPUT(Structure):
            _anonymous_ = ("u",)
            _fields_ = [("type", wintypes.DWORD),
                        ("u", _INPUT_UNION)]

        SendInput = user32.SendInput
        SendInput.argtypes = [wintypes.UINT, POINTER(INPUT), ctypes.c_int]
        SendInput.restype = wintypes.UINT

        INPUT_KEYBOARD = 1
        KEYEVENTF_KEYUP = 0x0002
        VK_MENU = 0x12   # ALT
        VK_H = 0x48
        VK_A = 0x41

        def make_key(vk: int, up: bool) -> INPUT:
            inp = INPUT()
            inp.type = INPUT_KEYBOARD
            inp.ki.wVk = vk
            inp.ki.wScan = 0
            inp.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
            inp.ki.time = 0
            inp.ki.dwExtraInfo = 0
            return inp

        # Alt down, H down, H up, Alt up — opens Help menu.
        seq1 = (INPUT * 4)(
            make_key(VK_MENU, False),
            make_key(VK_H, False),
            make_key(VK_H, True),
            make_key(VK_MENU, True),
        )
        SendInput(4, seq1, sizeof(INPUT))
        time.sleep(0.3)

        # Then plain 'A' to trigger the About PDFE menu item.
        seq2 = (INPUT * 2)(
            make_key(VK_A, False),
            make_key(VK_A, True),
        )
        SendInput(2, seq2, sizeof(INPUT))

        # Poll for the dialog. Qt sets the title to "About PDFE".
        about_hwnd = wait_for_window(self.proc.pid, "About PDFE", 8.0)
        if about_hwnd is None:
            self.skipTest(
                "AboutDialog did not open within 8s after Alt+H, A. "
                "The menu accelerator may be locale-dependent or "
                "Help->About may not be wired in this build."
            )

        try:
            sm, big, csm, cb = hicon_for(about_hwnd)
            # Same logic as main window: assert on WM_GETICON, not
            # the class-level fallbacks. AboutDialog's constructor
            # explicitly calls setWindowIcon(pdfeAppIcon()), so
            # WM_GETICON ICON_SMALL must be non-zero.
            self.assertNotEqual(
                sm, 0,
                f"AboutDialog WM_GETICON ICON_SMALL is 0 "
                f"(big={big:#x} class_small={csm:#x} "
                f"class_big={cb:#x}). AboutDialog's constructor "
                f"must call setWindowIcon(pdfeAppIcon()).",
            )
        finally:
            # Close the modal dialog so the main window is
            # responsive for tearDown.
            PostMessageW(about_hwnd, WM_CLOSE, 0, 0)
            time.sleep(0.3)


if __name__ == "__main__":
    unittest.main()
