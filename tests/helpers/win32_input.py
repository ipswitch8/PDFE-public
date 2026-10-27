# -*- coding: utf-8 -*-
"""Win32 SendInput wrapper for crossing Qt6's event-spontaneity barrier.

Pipeline e4c71f3a phase-5 (GAP-92/98).

Qt6 ignores synthetic key/mouse events that lack the OS-level spontaneous
flag. pywinauto's click_input and pyautogui's keyboard functions don't
always set this flag. Win32 SendInput sends events through the OS input
queue, making them indistinguishable from real user input.

Usage:
    from tests.helpers.win32_input import send_key, send_click, send_modified_click

    send_key(VK_RETURN)
    send_click(x=100, y=200)
    send_modified_click(x=100, y=200, ctrl=True)
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes
import time
from typing import Optional

# Every absolute-coordinate click/drag MUST pass the shared safety net. This
# module drives the mouse with raw SendInput (SetCursorPos + mouse events), which
# is INVISIBLE to click_safety's pyautogui monkey-patch — so without this explicit
# check a mis-computed coordinate (stale/gone window rect) fires unguarded onto
# the desktop or taskbar. assert_safe_click raises UnsafeClickError for a loud
# failure instead of a silent mis-fire; it honors allow_unsafe_clicks() /
# PDFE_ALLOW_UNSAFE_CLICKS for the rare legitimate outside-PDFE click.
from tests.helpers.click_safety import assert_safe_click

# Win32 constants
INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_WHEEL = 0x0800

#: One notch of a standard wheel, in the units Windows reports and Qt
#: forwards verbatim as ``QWheelEvent::angleDelta().y()``.
WHEEL_DELTA = 120

KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001

VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12  # Alt
VK_RETURN = 0x0D
VK_TAB = 0x09
VK_ESCAPE = 0x1B
VK_DELETE = 0x2E
VK_BACK = 0x08

# Structures for SendInput
class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.wintypes.LONG),
        ("dy", ctypes.wintypes.LONG),
        ("mouseData", ctypes.wintypes.DWORD),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("time", ctypes.wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", ctypes.wintypes.WORD),
        ("wScan", ctypes.wintypes.WORD),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("time", ctypes.wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT_UNION(ctypes.Union):
    _fields_ = [
        ("mi", MOUSEINPUT),
        ("ki", KEYBDINPUT),
    ]


class INPUT(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.wintypes.DWORD),
        ("union", INPUT_UNION),
    ]


SendInput = ctypes.windll.user32.SendInput
SendInput.argtypes = [ctypes.c_uint, ctypes.POINTER(INPUT), ctypes.c_int]
SendInput.restype = ctypes.c_uint

SetCursorPos = ctypes.windll.user32.SetCursorPos
GetSystemMetrics = ctypes.windll.user32.GetSystemMetrics

SM_CXSCREEN = 0
SM_CYSCREEN = 1


def _screen_width() -> int:
    return GetSystemMetrics(SM_CXSCREEN)


def _screen_height() -> int:
    return GetSystemMetrics(SM_CYSCREEN)


def _abs_coords(x: int, y: int) -> tuple[int, int]:
    """Convert screen pixels to absolute coordinates (0-65535 range)."""
    sw = _screen_width()
    sh = _screen_height()
    ax = int(x * 65536 / sw) + 1
    ay = int(y * 65536 / sh) + 1
    return ax, ay


def send_key(vk: int, *, extended: bool = False) -> None:
    """Send a single key press + release via SendInput."""
    flags_down = KEYEVENTF_EXTENDEDKEY if extended else 0
    flags_up = flags_down | KEYEVENTF_KEYUP

    inputs = (INPUT * 2)()
    inputs[0].type = INPUT_KEYBOARD
    inputs[0].union.ki.wVk = vk
    inputs[0].union.ki.dwFlags = flags_down

    inputs[1].type = INPUT_KEYBOARD
    inputs[1].union.ki.wVk = vk
    inputs[1].union.ki.dwFlags = flags_up

    SendInput(2, inputs, ctypes.sizeof(INPUT))


def send_char(char: str) -> None:
    """Send a single character keystroke (ASCII A-Z, 0-9)."""
    vk = ord(char.upper())
    send_key(vk)


def send_click(x: int, y: int, *, button: str = "left") -> None:
    """Send a mouse click at screen coordinates (x, y)."""
    assert_safe_click(x, y, op="click")
    ax, ay = _abs_coords(x, y)

    if button == "left":
        down_flag = MOUSEEVENTF_LEFTDOWN
        up_flag = MOUSEEVENTF_LEFTUP
    elif button == "right":
        down_flag = MOUSEEVENTF_RIGHTDOWN
        up_flag = MOUSEEVENTF_RIGHTUP
    else:
        raise ValueError(f"Unknown button: {button}")

    inputs = (INPUT * 3)()
    # Move
    inputs[0].type = INPUT_MOUSE
    inputs[0].union.mi.dx = ax
    inputs[0].union.mi.dy = ay
    inputs[0].union.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE

    # Down
    inputs[1].type = INPUT_MOUSE
    inputs[1].union.mi.dx = ax
    inputs[1].union.mi.dy = ay
    inputs[1].union.mi.dwFlags = down_flag | MOUSEEVENTF_ABSOLUTE

    # Up
    inputs[2].type = INPUT_MOUSE
    inputs[2].union.mi.dx = ax
    inputs[2].union.mi.dy = ay
    inputs[2].union.mi.dwFlags = up_flag | MOUSEEVENTF_ABSOLUTE

    SendInput(3, inputs, ctypes.sizeof(INPUT))


def send_wheel(x: int, y: int, notches: int, *, ctrl: bool = False) -> None:
    """Scroll the wheel ``notches`` steps with the cursor at ``(x, y)``.

    Positive ``notches`` scrolls AWAY from the user (wheel-up), which is what
    ``PdfCanvas::wheelEvent`` reads as ``angleDelta().y() > 0`` and, under
    Ctrl, routes to ``zoomIn()``. Negative scrolls toward the user.

    Exists because the ``canvas_zoom_pinch`` matrix row names Ctrl+scroll as
    the gesture under test and there was no wheel primitive here -- only
    clicks and drags. Driving zoom through the ``zoomInAction_`` shortcut
    instead would exercise the QAction, NOT ``PdfCanvas::wheelEvent``, so it
    could not distinguish a working canvas wheel handler from a dead one.

    The cursor is moved to ``(x, y)`` first because a wheel event is delivered
    to the window under the cursor, not to the focused window.
    """
    assert_safe_click(x, y, op="wheel")
    ax, ay = _abs_coords(x, y)

    n = abs(int(notches))
    if n == 0:
        raise ValueError("send_wheel: notches must be non-zero")
    # mouseData is declared DWORD (unsigned) but Windows reads the wheel
    # delta as a SIGNED short, so a wheel-down must be passed as the
    # two's-complement pattern rather than a negative Python int.
    delta = (WHEEL_DELTA if notches > 0 else -WHEEL_DELTA) & 0xFFFFFFFF

    # Move first, as its own SendInput batch, so the wheel events that follow
    # unambiguously hit the widget under the new cursor position.
    move = (INPUT * 1)()
    move[0].type = INPUT_MOUSE
    move[0].union.mi.dx = ax
    move[0].union.mi.dy = ay
    move[0].union.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
    SendInput(1, move, ctypes.sizeof(INPUT))
    time.sleep(0.05)

    if ctrl:
        kd = (INPUT * 1)()
        kd[0].type = INPUT_KEYBOARD
        kd[0].union.ki.wVk = VK_CONTROL
        kd[0].union.ki.dwFlags = 0
        SendInput(1, kd, ctypes.sizeof(INPUT))
        time.sleep(0.03)

    try:
        for _ in range(n):
            wheel = (INPUT * 1)()
            wheel[0].type = INPUT_MOUSE
            wheel[0].union.mi.dx = ax
            wheel[0].union.mi.dy = ay
            wheel[0].union.mi.mouseData = delta
            wheel[0].union.mi.dwFlags = MOUSEEVENTF_WHEEL | MOUSEEVENTF_ABSOLUTE
            SendInput(1, wheel, ctypes.sizeof(INPUT))
            time.sleep(0.08)
    finally:
        if ctrl:
            ku = (INPUT * 1)()
            ku[0].type = INPUT_KEYBOARD
            ku[0].union.ki.wVk = VK_CONTROL
            ku[0].union.ki.dwFlags = KEYEVENTF_KEYUP
            SendInput(1, ku, ctypes.sizeof(INPUT))


def send_modified_click(
    x: int, y: int,
    *, ctrl: bool = False, shift: bool = False, alt: bool = False,
    button: str = "left",
) -> None:
    """Send a modifier+click (e.g., Ctrl+Click for multi-select)."""
    assert_safe_click(x, y, op="modified-click")
    modifiers = []
    if ctrl:
        modifiers.append(VK_CONTROL)
    if shift:
        modifiers.append(VK_SHIFT)
    if alt:
        modifiers.append(VK_MENU)

    ax, ay = _abs_coords(x, y)

    if button == "left":
        down_flag = MOUSEEVENTF_LEFTDOWN
        up_flag = MOUSEEVENTF_LEFTUP
    else:
        down_flag = MOUSEEVENTF_RIGHTDOWN
        up_flag = MOUSEEVENTF_RIGHTUP

    # Build input array: mod-downs, move, click-down, click-up, mod-ups
    count = len(modifiers) * 2 + 3
    inputs = (INPUT * count)()
    idx = 0

    for vk in modifiers:
        inputs[idx].type = INPUT_KEYBOARD
        inputs[idx].union.ki.wVk = vk
        inputs[idx].union.ki.dwFlags = 0
        idx += 1

    inputs[idx].type = INPUT_MOUSE
    inputs[idx].union.mi.dx = ax
    inputs[idx].union.mi.dy = ay
    inputs[idx].union.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
    idx += 1

    inputs[idx].type = INPUT_MOUSE
    inputs[idx].union.mi.dx = ax
    inputs[idx].union.mi.dy = ay
    inputs[idx].union.mi.dwFlags = down_flag | MOUSEEVENTF_ABSOLUTE
    idx += 1

    inputs[idx].type = INPUT_MOUSE
    inputs[idx].union.mi.dx = ax
    inputs[idx].union.mi.dy = ay
    inputs[idx].union.mi.dwFlags = up_flag | MOUSEEVENTF_ABSOLUTE
    idx += 1

    for vk in reversed(modifiers):
        inputs[idx].type = INPUT_KEYBOARD
        inputs[idx].union.ki.wVk = vk
        inputs[idx].union.ki.dwFlags = KEYEVENTF_KEYUP
        idx += 1

    SendInput(count, inputs, ctypes.sizeof(INPUT))


def send_drag(
    x1: int, y1: int, x2: int, y2: int,
    *, duration: float = 0.3, steps: int = 10, button: str = "left",
) -> None:
    """Send a mouse drag from (x1,y1) to (x2,y2).

    ``button`` selects which physical button is held for the drag. "middle"
    exists for GitHub #119's drag-to-pan gesture, which is deliberately NOT
    the left button — see the block comment at the head of
    PdfCanvas::mousePressEvent.
    """
    if button == "middle":
        down_flag, up_flag = MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP
    elif button == "right":
        down_flag, up_flag = MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP
    elif button == "left":
        down_flag, up_flag = MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP
    else:
        raise ValueError(f"unsupported drag button {button!r}")
    # Guard BOTH endpoints: a drag that starts or ends off PDFE is as dangerous
    # as a stray click (it can rubber-band-select the desktop or drop on another app).
    assert_safe_click(x1, y1, op="drag-start")
    assert_safe_click(x2, y2, op="drag-end")
    ax1, ay1 = _abs_coords(x1, y1)
    ax2, ay2 = _abs_coords(x2, y2)

    # Move to start
    inp = (INPUT * 1)()
    inp[0].type = INPUT_MOUSE
    inp[0].union.mi.dx = ax1
    inp[0].union.mi.dy = ay1
    inp[0].union.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
    SendInput(1, inp, ctypes.sizeof(INPUT))
    time.sleep(0.05)

    # Press
    inp[0].union.mi.dwFlags = down_flag | MOUSEEVENTF_ABSOLUTE
    SendInput(1, inp, ctypes.sizeof(INPUT))
    time.sleep(0.05)

    # Drag in steps
    dt = duration / steps
    for i in range(1, steps + 1):
        t = i / steps
        cx = int(ax1 + (ax2 - ax1) * t)
        cy = int(ay1 + (ay2 - ay1) * t)
        inp[0].union.mi.dx = cx
        inp[0].union.mi.dy = cy
        inp[0].union.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
        SendInput(1, inp, ctypes.sizeof(INPUT))
        time.sleep(dt)

    # Release
    inp[0].union.mi.dx = ax2
    inp[0].union.mi.dy = ay2
    inp[0].union.mi.dwFlags = up_flag | MOUSEEVENTF_ABSOLUTE
    SendInput(1, inp, ctypes.sizeof(INPUT))
