# -*- coding: utf-8 -*-
"""Tests for the belt-and-suspenders autosave-sidecar cleanup.

Covers both the pure helper (tests/helpers/autosave_cleanup.py) and the
autouse fixture wiring + @pytest.mark.autosave exemption in tests/conftest.py.
"""

from __future__ import annotations

import pytest

from tests.helpers.autosave_cleanup import (
    SIDECAR_SUFFIX,
    discard_recovery_sidecars,
    recovery_dir,
)

pytestmark = pytest.mark.skipif(
    recovery_dir() is None,
    reason="no %APPDATA% (non-Windows) — recovery dir cannot be resolved",
)


def _plant(session_id: str):
    """Create a valid sidecar + companion recovery copy; return the dir."""
    d = recovery_dir()
    d.mkdir(parents=True, exist_ok=True)
    (d / (session_id + SIDECAR_SUFFIX)).write_text(
        '{"sessionId":"%s","recoveryFilePath":"%s"}'
        % (session_id, (d / (session_id + ".pdf")).as_posix()),
        encoding="utf-8",
    )
    (d / (session_id + ".pdf")).write_bytes(b"%PDF-1.4\n%%EOF\n")
    return d


# --------------------------------------------------------------------------
# Pure helper
# --------------------------------------------------------------------------

def test_recovery_dir_matches_appdata_layout():
    d = recovery_dir()
    # Qt AppDataLocation with org == app == "PDFE" (src/main.cpp) + /recovery.
    assert d.parts[-3:] == ("PDFE", "PDFE", "recovery"), d


def test_discard_removes_sidecar_and_copy():
    d = _plant("probe_sessionA")
    _plant("probe_sessionB")
    assert (d / ("probe_sessionA" + SIDECAR_SUFFIX)).exists()

    removed = discard_recovery_sidecars()

    assert removed >= 4, f"two sessions = 4 files, got {removed}"
    # No sidecars survive, and the companion copies are gone.
    assert not list(d.glob("*" + SIDECAR_SUFFIX))
    assert not (d / "probe_sessionA.pdf").exists()
    assert not (d / "probe_sessionB.pdf").exists()


def test_discard_leaves_orphan_pdf_without_sidecar():
    d = recovery_dir()
    d.mkdir(parents=True, exist_ok=True)
    orphan = d / "orphan_no_sidecar.pdf"
    orphan.write_bytes(b"%PDF-1.4\n%%EOF\n")
    try:
        discard_recovery_sidecars()
        assert orphan.exists(), "a .pdf with no sidecar must not be removed"
    finally:
        orphan.unlink(missing_ok=True)


def test_discard_is_noop_on_empty_dir():
    discard_recovery_sidecars()  # clear anything left over
    assert discard_recovery_sidecars() == 0


# --------------------------------------------------------------------------
# Autouse-fixture wiring: plain test -> cleaned; @autosave test -> preserved.
# Ordered pairs (pytest runs in definition order within a file).
# --------------------------------------------------------------------------

def test_fixture_wiring_step1_plant_plain():
    d = _plant("fixtureprobe_plain")
    assert (d / ("fixtureprobe_plain" + SIDECAR_SUFFIX)).exists()
    # The autouse _clean_autosave_sidecars fixture cleans after this test.


def test_fixture_wiring_step2_confirm_cleaned():
    d = recovery_dir()
    assert not (d / ("fixtureprobe_plain" + SIDECAR_SUFFIX)).exists(), (
        "autouse cleanup should have removed the plain test's sidecar"
    )


@pytest.mark.autosave
def test_fixture_exempt_step1_plant_marked():
    d = _plant("fixtureprobe_autosave")
    assert (d / ("fixtureprobe_autosave" + SIDECAR_SUFFIX)).exists()
    # @autosave -> the autouse fixture SKIPS cleanup for this test.


def test_fixture_exempt_step2_confirm_preserved():
    d = recovery_dir()
    p = d / ("fixtureprobe_autosave" + SIDECAR_SUFFIX)
    preserved = p.exists()
    discard_recovery_sidecars()  # clean up regardless of the assertion
    assert preserved, "@pytest.mark.autosave test must be EXEMPT from cleanup"
