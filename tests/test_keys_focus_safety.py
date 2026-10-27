# -*- coding: utf-8 -*-
"""Unit lock for ``tests/helpers/keys.py`` focus-safe assumed-foreground wrappers.

History
-------
During pipeline b3f19d72 phase-11, a color-dialog driver in
``tests/test_ui_annotations.py`` used bare ``pyautogui.hotkey('ctrl','a')``
followed by ``pyautogui.typewrite('0000ff')``.  When PDFE lost
foreground between the priming ``click_input()`` and the keystrokes, the
``Ctrl+A`` selected everything in the user's foreground window (email,
browser, file explorer) and ``0000ff`` typed over it.

This module locks two invariants:

1. ``verify_foreground_is(hwnd)`` returns False when the OS foreground
   window is not the given hwnd's root.  ``True`` when it is.
2. ``typewrite_assuming_foreground`` and ``press_assuming_foreground``
   raise ``FocusError`` (do NOT silently send keystrokes) when foreground
   is not the expected hwnd.

The wrappers exist exactly to prevent the phase-11 incident class.  If
either invariant regresses, the test suite catches it without involving
the user's desktop.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.keys import (  # noqa: E402
    FocusError,
    press_assuming_foreground,
    typewrite_assuming_foreground,
    verify_foreground_is,
)


class VerifyForegroundIsTests(unittest.TestCase):
    """Locks for ``verify_foreground_is``."""

    def test_zero_hwnd_returns_false(self) -> None:
        self.assertFalse(verify_foreground_is(0))

    def test_invalid_hwnd_returns_false(self) -> None:
        # 0x7FFFFFFF is not a valid HWND on any sane process; should
        # report False rather than crash.
        self.assertFalse(verify_foreground_is(0x7FFFFFFF))


class AssumingForegroundRaisesOnEscapeTests(unittest.TestCase):
    """Locks the safety contract: never silently send keystrokes."""

    def test_typewrite_assuming_foreground_raises_when_hwnd_invalid(
            self) -> None:
        """If foreground is not the expected hwnd, FocusError fires
        BEFORE any keystroke is sent.  An invalid hwnd is guaranteed not
        to match foreground, so this is a deterministic check."""
        with self.assertRaises(FocusError):
            typewrite_assuming_foreground(0x7FFFFFFF, "should-not-type")

    def test_typewrite_assuming_foreground_raises_on_zero_hwnd(
            self) -> None:
        with self.assertRaises(FocusError):
            typewrite_assuming_foreground(0, "should-not-type")

    def test_press_assuming_foreground_raises_when_hwnd_invalid(
            self) -> None:
        with self.assertRaises(FocusError):
            press_assuming_foreground(0x7FFFFFFF, "enter")

    def test_press_assuming_foreground_raises_on_zero_hwnd(self) -> None:
        with self.assertRaises(FocusError):
            press_assuming_foreground(0, "enter")


if __name__ == "__main__":
    unittest.main()
