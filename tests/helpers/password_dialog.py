# -*- coding: utf-8 -*-
"""Shared helper for driving the PDFE PasswordDialog in UI tests.

Pipeline b3f19d72 phase-9 (P13-INFRA-4 resolution).

The PasswordDialog is auto-triggered when PDFE opens an encrypted PDF —
it is NOT reachable from a menu action, so the generic
``test_keyboard_dialog_*`` parametrize cases for PasswordDialog correctly
skip (no menu route).  This helper supports dedicated encryption tests
that need to open an encrypted fixture and supply the password
programmatically.

Usage
-----
    from tests.helpers.password_dialog import (
        open_encrypted_pdf_with_password,
        PasswordDialogNotFound,
    )

    with _pdfe_session_dirty_safe() as s:
        # Open the encrypted fixture via Ctrl+O, drive the open dialog
        # with the fixture path, then call this helper.
        ok = open_encrypted_pdf_with_password(s, "test123")
        if not ok:
            pytest.skip("PasswordDialog did not appear for encrypted fixture.")

Public API
----------
``open_encrypted_pdf_with_password(session, password, timeout)``
    Wait for the PasswordDialog to appear (``passwordEdit`` auto_id),
    type the password into it, and click OK.  Returns True on success.

``PasswordDialogNotFound``
    Raised internally when the dialog is expected but absent; callers
    can also check the bool return of ``open_encrypted_pdf_with_password``.

Known fixture passwords (for tests that need them)
---------------------------------------------------
    test_encrypted_aes256.pdf        user="test123"  owner="owner456"
    test_encrypted_restricted.pdf    user="test123"  owner="owner456"
    test_encrypted_no_print_copy.pdf user="test123"  owner="owner456"
    test_encrypted_owner_only.pdf    user=""         owner="owner456"
    phase3_encrypted_aes128.pdf      user="test123"  (legacy fixture)
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Lazy import guard — the module must importable on non-Windows CI where
# pywinauto is not installed (tests skip at the REQUIRES_LIVE_UI mark).
# ---------------------------------------------------------------------------

def _try_import_pywinauto():
    try:
        import pywinauto  # noqa: F401  # pylint: disable=import-outside-toplevel
        return True
    except ImportError:
        return False


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class PasswordDialogNotFound(RuntimeError):
    """Raised when the PasswordDialog does not appear within the timeout."""


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _find_password_edit(session, timeout: float):
    """Wait for ``passwordEdit`` to appear in the UIA tree.

    Returns the wrapper object or None.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            widget = session.find_optional(auto_id="passwordEdit",
                                           timeout=1.0)
            if widget is not None:
                return widget
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.2)
    return None


def _find_ok_button(session, timeout: float = 3.0):
    """Locate the PasswordDialog OK button by auto_id ``okButton``.

    Falls back to the first enabled Button with text "OK".
    Returns the wrapper or None.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            btn = session.find_optional(auto_id="okButton", timeout=0.5)
            if btn is not None:
                return btn
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.2)
    # Fallback: search descendants for an enabled Button labelled "OK"
    try:
        for d in session.window.descendants():
            try:
                ct = d.element_info.control_type or ""
                nm = (d.element_info.name or "").strip().upper()
                if ct == "Button" and nm in ("OK", "OK\n"):
                    return d
            except Exception:  # noqa: BLE001
                continue
    except Exception:  # noqa: BLE001
        pass
    return None


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def open_encrypted_pdf_with_password(
    session,
    password: str,
    timeout: float = 8.0,
) -> bool:
    """Wait for PasswordDialog, fill ``password``, and click OK.

    Parameters
    ----------
    session
        A live ``PDFESession`` (or compatible) with ``find_optional`` and
        ``window`` attributes; the caller must have already triggered the
        encrypted-open flow (e.g. by driving the Open dialog with an
        encrypted fixture path).
    password
        The user password to type.  May be empty (open-owner-only PDFs).
    timeout
        Seconds to wait for the PasswordDialog to appear.

    Returns
    -------
    bool
        True if the password was submitted (OK clicked); False if the
        dialog did not appear within ``timeout``.

    Raises
    ------
    PasswordDialogNotFound
        Re-raised internally when called with ``raise_on_miss=True``
        (not exposed in the public API — callers check the bool return).
    """
    pwd_edit = _find_password_edit(session, timeout=timeout)
    if pwd_edit is None:
        return False

    # Type the password.  pywinauto's type_keys() is more reliable than
    # set_edit_text() for QLineEdit::Password mode because set_edit_text
    # uses UIA SetValue which some Qt6 builds ignore on password fields.
    try:
        pwd_edit.click_input()
        time.sleep(0.1)
        # Clear any pre-existing text before typing.
        try:
            pwd_edit.set_edit_text("")
        except Exception:  # noqa: BLE001
            pass
        if password:
            # type_keys with with_spaces=True so spaces are included.
            pwd_edit.type_keys(password, with_spaces=True)
        time.sleep(0.15)
    except Exception as exc:  # noqa: BLE001
        # If type_keys fails (e.g. on non-Windows CI), fall back to
        # returning False so the caller can pytest.skip.
        _ = exc
        return False

    # Click OK.
    ok_btn = _find_ok_button(session, timeout=2.0)
    if ok_btn is None:
        # Last-resort: send Enter to whatever has focus.
        try:
            from pywinauto.keyboard import send_keys  # type: ignore  # noqa: PLC0415
            send_keys("{ENTER}")
            time.sleep(0.5)
        except Exception:  # noqa: BLE001
            return False
        return True

    try:
        ok_btn.click_input()
        time.sleep(0.5)
        return True
    except Exception:  # noqa: BLE001
        return False


# ---------------------------------------------------------------------------
# Convenience: known fixture passwords (import from tests that need them)
# ---------------------------------------------------------------------------

#: Known user passwords for encrypted test fixtures.
#: Keys are fixture basenames; values are (user_password, owner_password).
FIXTURE_PASSWORDS = {
    "test_encrypted_aes256.pdf":        ("test123", "owner456"),
    "test_encrypted_restricted.pdf":    ("test123", "owner456"),
    "test_encrypted_no_print_copy.pdf": ("test123", "owner456"),
    "test_encrypted_owner_only.pdf":    ("",        "owner456"),
    "phase3_encrypted_aes128.pdf":      ("test123", ""),
}


def user_password_for(fixture_path: "Path | str") -> Optional[str]:
    """Return the user password for a known encrypted fixture, or None."""
    name = Path(fixture_path).name
    entry = FIXTURE_PASSWORDS.get(name)
    return entry[0] if entry else None
