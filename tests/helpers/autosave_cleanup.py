# -*- coding: utf-8 -*-
"""Autosave / crash-recovery sidecar cleanup for the test harness.

Belt-and-suspenders companion to the in-app safeguards:

- PDFE discards stale recovery sidecars at startup under the UI-test seam
  (MainWindow::checkForRecoverableSessions -> AutosaveManager::discardAllRecoverable
  when PDFE_TEST_SKIP_UNSAVED_PROMPT=1).

This module cleans the SAME location from the OUTSIDE, at the END of every test
(wired via the ``_clean_autosave_sidecars`` autouse fixture in tests/conftest.py),
so a test that dirtied a document and was then force-killed cannot leave a
sidecar that fires the "Recover unsaved changes" modal on a later test's launch.

Kept dependency-free (os / pathlib only) so it imports cleanly in headless /
non-UI environments where pywinauto/pyautogui are absent.

The recovery directory mirrors PDFE's Qt QStandardPaths::AppDataLocation:
organizationName == applicationName == "PDFE" (see src/main.cpp), so on Windows
it resolves to ``%APPDATA%\\PDFE\\PDFE\\recovery`` and the sidecar suffix is
``.pdfe_recovery.json`` (see src/core/AutosaveManager.cpp kSidecarSuffix).
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Optional

# Must match AutosaveManager.cpp kSidecarSuffix.
SIDECAR_SUFFIX = ".pdfe_recovery.json"


def recovery_dir() -> Optional[Path]:
    """Return PDFE's per-user recovery directory, or None if it can't be
    resolved on this platform (e.g. %APPDATA% unset on non-Windows CI).

    Does NOT create the directory — callers only ever need to clean it.
    """
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return None
    return Path(appdata) / "PDFE" / "PDFE" / "recovery"


def discard_recovery_sidecars() -> int:
    """Remove every recovery sidecar and its companion recovery copy from the
    per-user recovery directory. Best-effort: never raises. Returns the number
    of files removed.

    A sidecar is ``<sessionId>.pdfe_recovery.json``; its recovery copy is
    ``<sessionId>.pdf`` in the same directory. Both are removed. Any orphan
    ``*.pdf`` that has no sidecar is left alone (it is not ours to assume).
    """
    d = recovery_dir()
    if d is None:
        return 0
    try:
        if not d.is_dir():
            return 0
    except OSError:
        return 0

    removed = 0
    try:
        sidecars = list(d.glob("*" + SIDECAR_SUFFIX))
    except OSError:
        return 0

    for sidecar in sidecars:
        session_id = sidecar.name[: -len(SIDECAR_SUFFIX)]
        for target in (sidecar, d / (session_id + ".pdf")):
            try:
                if target.exists():
                    target.unlink()
                    removed += 1
            except OSError:
                # Locked (a live PDFE still holds it) or already gone — skip.
                pass
    return removed
