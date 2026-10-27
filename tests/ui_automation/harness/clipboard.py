# -*- coding: utf-8 -*-
"""RDP-safe clipboard helpers.

Under Windows RDP sessions the clipboard ownership flips transiently
to the session manager, and naive ``QClipboard::text()`` calls often
return an empty string even though the prior ``setText`` succeeded.
This mirrors the same problem the production TextEditTool worked
around in commit 8371c81 (``readClipboardTextWithRetry`` /
``writeClipboardTextWithVerify``).

These helpers use pywinauto's ``Application.clipboard()`` — which is
just a pywinauto view onto the same Windows clipboard — so the same
race applies. Retry with ``QCoreApplication::processEvents``
equivalent (Python's ``time.sleep`` after a small poll) until the
value round-trips.
"""
from __future__ import annotations

import time
from typing import Optional


def set_clipboard_text(text: str, max_attempts: int = 5,
                        delay_s: float = 0.02) -> bool:
    """Set clipboard text with verify-then-retry.

    :returns: True iff a subsequent read confirms the set value.
    """
    try:
        import pyperclip  # Preferred: cross-platform, reliable
    except ImportError:
        pyperclip = None

    if pyperclip is not None:
        for _ in range(max_attempts):
            try:
                pyperclip.copy(text)
                time.sleep(delay_s)
                if pyperclip.paste() == text:
                    return True
            except Exception:  # noqa: BLE001
                pass
            time.sleep(delay_s)
        return False

    # Fallback: Win32 via ctypes (rarely needed — pyperclip installs
    # as a harness dependency).
    import ctypes
    from ctypes import wintypes

    CF_UNICODETEXT = 13
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32

    for _ in range(max_attempts):
        if not user32.OpenClipboard(0):
            time.sleep(delay_s)
            continue
        try:
            user32.EmptyClipboard()
            # Allocate a global moveable buffer for the text
            data = text.encode("utf-16-le") + b"\x00\x00"
            GMEM_MOVEABLE = 0x0002
            h_mem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
            lp = kernel32.GlobalLock(h_mem)
            ctypes.memmove(lp, data, len(data))
            kernel32.GlobalUnlock(h_mem)
            user32.SetClipboardData(CF_UNICODETEXT, h_mem)
        finally:
            user32.CloseClipboard()
        time.sleep(delay_s)
        if get_clipboard_text(max_attempts=1) == text:
            return True
    return False


def get_clipboard_text(max_attempts: int = 5,
                        delay_s: float = 0.02,
                        allow_empty: bool = False) -> Optional[str]:
    """Read clipboard text with retries.

    Karen phase-1 review finding #2: tests that legitimately cleared
    the clipboard and expect ``""`` back must pass
    ``allow_empty=True`` or they will see ``None`` after the retry
    loop exhausts. By default the empty string is treated as
    "not-yet-set" (the RDP-race signature where another process
    has grabbed the clipboard), which is the common case.

    :param max_attempts: How many times to poll before giving up.
    :param delay_s: Sleep between attempts.
    :param allow_empty: When True, an empty string is a valid
                        result and returns immediately. When False
                        (default), keep polling until non-empty or
                        the retry budget runs out.
    :returns: The clipboard text. ``None`` on total failure. May
              be ``""`` when ``allow_empty=True``.
    """
    try:
        import pyperclip
    except ImportError:
        pyperclip = None

    if pyperclip is not None:
        last: Optional[str] = None
        for _ in range(max_attempts):
            try:
                last = pyperclip.paste()
                if last or (allow_empty and last == ""):
                    return last
            except Exception:  # noqa: BLE001
                pass
            time.sleep(delay_s)
        return last

    # Ctypes fallback
    import ctypes

    CF_UNICODETEXT = 13
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32

    for _ in range(max_attempts):
        if not user32.OpenClipboard(0):
            time.sleep(delay_s)
            continue
        try:
            h_data = user32.GetClipboardData(CF_UNICODETEXT)
            if not h_data:
                continue
            lp = kernel32.GlobalLock(h_data)
            try:
                return ctypes.c_wchar_p(lp).value
            finally:
                kernel32.GlobalUnlock(h_data)
        finally:
            user32.CloseClipboard()
        time.sleep(delay_s)
    return None


def clear_clipboard() -> None:
    """Clear the clipboard entirely. Best-effort."""
    try:
        import pyperclip
        pyperclip.copy("")
    except Exception:  # noqa: BLE001
        pass
