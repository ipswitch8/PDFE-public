# -*- coding: utf-8 -*-
"""End-to-end smoke test for PDFE.exe launch + clean exit.

Phase-5 criterion 5: "Build artifact build/windows-vs2022/Debug/
PDFE.exe launches cleanly (no startup crash) and exits cleanly
when closed via pywinauto; confirmed by a smoke test under
tests/ that closes the window and asserts no orphan PDFE.exe
process remains."

The C++ GoogleTest suite (pdfe_tests.exe) constructs MainWindow
inside the test process and so verifies the Qt object lifecycle.
This Python test verifies the cross-process lifecycle: spawn
PDFE.exe, confirm the main window appears, post WM_CLOSE,
confirm the process exits within a reasonable deadline, and
double-check via tasklist that no orphan pdfe.exe processes
remain.

Run:
    pytest tests/test_ui_pdfe_smoke.py
"""
from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import time
import unittest
from ctypes import wintypes
from pathlib import Path

if sys.platform != "win32":
    raise unittest.SkipTest("Win32-only smoke test")


REPO_ROOT = Path(__file__).resolve().parent.parent

# CLAUDE.md documents build/windows-vs2022/Debug/PDFE.exe but the
# actual CMake output (preset windows-vs2022) lands at
# build/vs2022/bin/Debug/PDFE.exe. Probe both, plus the lowercase
# pdfe.exe variant some toolchains emit, so the test is robust
# against either preset's binary directory layout.
_CANDIDATES = [
    REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe",
    REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "pdfe.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "Debug" / "PDFE.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "Debug" / "pdfe.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "bin" / "Debug" / "PDFE.exe",
    REPO_ROOT / "build" / "windows-vs2022" / "bin" / "Debug" / "pdfe.exe",
]
PDFE_EXE = next((p for p in _CANDIDATES if p.exists()), _CANDIDATES[0])

WM_CLOSE = 0x0010

user32 = ctypes.WinDLL("user32", use_last_error=True)
PostMessageW = user32.PostMessageW
PostMessageW.argtypes = [
    wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
]
PostMessageW.restype = wintypes.BOOL

GetWindowThreadProcessId = user32.GetWindowThreadProcessId
GetWindowThreadProcessId.argtypes = [
    wintypes.HWND, ctypes.POINTER(wintypes.DWORD)
]
GetWindowThreadProcessId.restype = wintypes.DWORD

IsWindowVisible = user32.IsWindowVisible
IsWindowVisible.argtypes = [wintypes.HWND]
IsWindowVisible.restype = wintypes.BOOL

GetWindowTextLengthW = user32.GetWindowTextLengthW
GetWindowTextLengthW.argtypes = [wintypes.HWND]
GetWindowTextLengthW.restype = ctypes.c_int

GetWindowTextW = user32.GetWindowTextW
GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
GetWindowTextW.restype = ctypes.c_int

WNDENUMPROC = ctypes.WINFUNCTYPE(
    wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
EnumWindows = user32.EnumWindows
EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
EnumWindows.restype = wintypes.BOOL


def find_window_by_pid_title(pid: int, title_substr: str) -> int | None:
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
            return False
        return True

    EnumWindows(WNDENUMPROC(cb), 0)
    return hits[0] if hits else None


def count_pdfe_processes() -> int:
    """Return how many pdfe.exe / PDFE.exe processes exist."""
    out = subprocess.run(
        ["tasklist", "/NH", "/FI", "IMAGENAME eq pdfe.exe"],
        capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    return sum(
        1 for line in out.stdout.splitlines()
        if "pdfe.exe" in line.lower()
    )


class PdfeSmokeTest(unittest.TestCase):
    def setUp(self):
        # Pre-condition: no orphan from prior tests.
        if count_pdfe_processes() > 0:
            subprocess.run(
                ["taskkill", "/F", "/IM", "pdfe.exe"],
                capture_output=True
            )
            time.sleep(0.5)

    def test_launch_and_clean_exit(self):
        if not PDFE_EXE.exists():
            self.skipTest(f"PDFE.exe not built; expected at {PDFE_EXE}")

        before = count_pdfe_processes()
        self.assertEqual(
            before, 0,
            f"Test environment dirty: {before} pdfe.exe processes "
            f"already running before launch.")

        # Launch.
        proc = subprocess.Popen(
            [str(PDFE_EXE)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            # Suppress the "Recover unsaved changes" modal so a sidecar left by a
            # prior killed-dirty UI test cannot wedge this clean launch/close.
            env={**os.environ, "PDFE_TEST_SKIP_UNSAVED_PROMPT": "1"},
        )

        try:
            # Wait for the main window (criterion: "launches cleanly").
            deadline = time.monotonic() + 20.0
            hwnd = None
            while time.monotonic() < deadline:
                hwnd = find_window_by_pid_title(proc.pid, "PDFE")
                if hwnd:
                    break
                # Also bail early if the process died on launch.
                if proc.poll() is not None:
                    self.fail(
                        f"PDFE.exe exited with code {proc.returncode} "
                        f"before its window appeared — startup crash."
                    )
                time.sleep(0.2)
            self.assertIsNotNone(
                hwnd,
                "PDFE main window did not appear within 20s of launch."
            )

            # Close (criterion: "exits cleanly when closed").
            PostMessageW(hwnd, WM_CLOSE, 0, 0)
            try:
                rc = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.fail(
                    "PDFE.exe did not exit within 10s after WM_CLOSE — "
                    "the close path is hanging."
                )

            # Exit code 0 is the clean-shutdown signal; non-zero
            # often indicates an exception in the close path.
            self.assertEqual(
                rc, 0,
                f"PDFE.exe exited with code {rc} after WM_CLOSE — "
                f"non-zero indicates an unclean shutdown.",
            )

            # Final guard (criterion: "no orphan PDFE.exe process
            # remains"). Give Windows a moment to reap the handle.
            time.sleep(0.5)
            after = count_pdfe_processes()
            self.assertEqual(
                after, 0,
                f"{after} orphan PDFE.exe process(es) survived "
                f"after WM_CLOSE + proc.wait — close path is "
                f"leaking processes."
            )
        finally:
            if proc.poll() is None:
                try:
                    proc.kill()
                except Exception:
                    pass


if __name__ == "__main__":
    unittest.main()
