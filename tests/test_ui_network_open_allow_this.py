# -*- coding: utf-8 -*-
"""GitHub #240 (pipeline ui-report-netopen, fix #3): the network-open REFUSAL
dialog offers "Allow this", and clicking it flips the Preferences setting and
proceeds — end-to-end, in the real process.

WHAT THIS COVERS THAT THE GOOGLETEST DOES NOT
---------------------------------------------
``tests/test_network_open_allow_this.cpp`` drives ``MainWindow::openFile`` and
the dialog-action seam IN-PROCESS. This test exercises the same path through a
REAL launched ``pdfe.exe``: the real ``openAction``, the real modal dialog, the
real "Allow this" button, and — the end-to-end half the gtest cannot see — the
setting actually written to a persisted INI that a FRESH process would read.

HOW THE SETTING STARTS OFF
--------------------------
``allowUserNetworkPaths`` defaults TRUE, which is the whole point of #240, so
the Refused branch never fires with stock settings. We point PDFE at a private
INI via ``PDFE_SETTINGS_PATH`` (the documented per-process settings override)
pre-seeded with ``Network/allowUserPaths=false``. After the click we read that
same INI back: a fresh ``AppSettings`` reader in production would see exactly
this, which is the persistence the Preferences checkbox promises.

WHY THE DIALOG IS DRIVEN THROUGH THE SEAM, NOT UIA
--------------------------------------------------
UIA cannot enumerate a Qt app-modal ``exec()`` on the Server-2019 session (see
tests/helpers/modal_dialog.py). So the Allow click is queued through the
dialog-action pipe BEFORE the open (``registerDialogIntrospection`` replays it
on the dialog's own event loop), exactly as the gtest does. The unreachable UNC
means the re-attempt after the flip ends in the Unreachable branch (unchanged by
this fix); the seam's auto-dismisser closes that box so nothing hangs.

Run:  python -m pytest tests/test_ui_network_open_allow_this.py -v -s
"""
from __future__ import annotations

import configparser
import os
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

# A host that cannot exist, so the case needs no share. The assertion is about
# the setting being flipped, not about a successful open.
UNREACHABLE_UNC = r"\\pdfe-no-such-host.invalid\shareone\nope.pdf"

INI_SECTION = "Network"
INI_KEY = "allowUserPaths"


def _has(mod: str) -> bool:
    try:
        __import__(mod)
        return True
    except Exception:  # noqa: BLE001
        return False


def _has_pdfe() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:  # noqa: BLE001
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    if sys.platform != "win32":
        return False
    return bool(os.environ.get("SESSIONNAME"))


pytestmark = [
    pytest.mark.skipif(not _has_pdfe(), reason="pdfe.exe not built"),
    pytest.mark.skipif(not _is_interactive(),
                       reason="UIA requires an interactive Windows desktop"),
    pytest.mark.skipif(not _has("pywinauto"), reason="pywinauto not installed"),
    pytest.mark.skipif(not _has("psutil"), reason="psutil not installed"),
]


def _write_settings_ini(path: Path, allow: bool) -> None:
    """Seed a QSettings IniFormat file with the network-open policy value.

    QSettings writes ``true``/``false`` (lower-case) under ``[Network]``; that
    is the exact spelling ``AppSettings::allowUserNetworkPaths`` reads back.
    """
    path.write_text(
        "[%s]\n%s=%s\n" % (INI_SECTION, INI_KEY, "true" if allow else "false"),
        encoding="utf-8",
    )


def _read_allow_user_paths(path: Path) -> str | None:
    """Return the stored value as a lower-case string, or None if unset.

    A string (not a bool) so "absent" is distinguishable from "false": an
    absent key would let ``allowUserNetworkPaths`` fall back to its TRUE
    default, which must never be mistaken for a successful flip.
    """
    cp = configparser.ConfigParser()
    # QSettings INIs are case-sensitive in their keys; preserve them.
    cp.optionxform = str  # type: ignore[assignment]
    try:
        cp.read(path, encoding="utf-8")
    except Exception:  # noqa: BLE001
        return None
    if not cp.has_section(INI_SECTION) or not cp.has_option(INI_SECTION, INI_KEY):
        return None
    return cp.get(INI_SECTION, INI_KEY).strip().strip('"').lower()


def test_allow_this_button_flips_the_setting_and_proceeds(tmp_path):
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers import qinvoke

    ini = tmp_path / "pdfe_netopen_settings.ini"
    _write_settings_ini(ini, allow=False)
    assert _read_allow_user_paths(ini) == "false", "seed INI did not take"

    prior = os.environ.get("PDFE_SETTINGS_PATH")
    os.environ["PDFE_SETTINGS_PATH"] = str(ini)
    try:
        with PDFESession(pdf_path=None) as s:
            assert s.wait_for_window_title("PDFE"), "PDFE window never appeared"
            s.wait_for_idle()
            time.sleep(1.0)

            # Queue the REAL Allow-button click, keyed by the dialog objectName,
            # BEFORE the dialog opens (MainWindow is still non-modal, so the
            # pipe receives the UIA value event).
            assert qinvoke.enqueue_dialog_action(
                s, "networkOpenBlockedDialog|click:networkOpenAllowButton"), \
                "could not queue the Allow-this dialog action"

            # Pre-seat the path File > Open will yield, then drive the same
            # openAction the menu drives.
            assert qinvoke.inject_file_dialog_path(s, UNREACHABLE_UNC), \
                "could not pre-seat the file-dialog path"
            assert qinvoke.activate_tool_by_name(s, "openAction"), \
                "failed to trigger the openAction QAction"

            # Give the dialog time to open, the queued click to replay, the flip
            # + sync to run, and the re-attempt's Unreachable box to be dismissed
            # by the seam's auto-dismisser.
            time.sleep(4.0)
            s.wait_for_idle()

        # The session has exited (process gone), so the INI is fully flushed.
        value = _read_allow_user_paths(ini)
        assert value == "true", (
            "clicking \"Allow this\" did not persist allowUserPaths=true; the "
            "INI still reads %r. The setting the Preferences checkbox writes was "
            "not written, so a formerly-refused open could not proceed." % value
        )
    finally:
        if prior is None:
            os.environ.pop("PDFE_SETTINGS_PATH", None)
        else:
            os.environ["PDFE_SETTINGS_PATH"] = prior
