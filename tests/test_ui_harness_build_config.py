#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #270 — PDFE_BUILD_CONFIG must reach the harness that launches PDFE.

Two UI harnesses resolve the application binary independently:

  * ``tests/ui_automation/harness/pdfe_app_fixture.py`` honours
    ``PDFE_BUILD_CONFIG``, and three of its callers (``render_drive.py``,
    ``batch2_drive.py``, ``diag_gap20_theme_toggle.py``) set it to ``Release``.
  * ``tests/ui_harness/core/launch.py`` hardcoded ``Debug`` and consulted no
    environment variable at all.

``tests/helpers/pdfe_session.py`` re-exports the second one's constant and
``tests/test_ui_stress.py`` imports it from there, so the whole stress suite
launched the Debug binary regardless of what the variable said.

That is a SILENT no-op, and the failure mode is a stale binary with nothing
reporting that the requested configuration was not the one under test — the
shape this project has already paid for in #245 and in two `docs/audit/` entries
on stale objects surviving a relink.

These tests need no application launch and no build: they exercise the
RESOLUTION, which is where the defect was.
"""

from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_harness.core import launch  # noqa: E402


@pytest.fixture(autouse=True)
def _restore_env():
    """Leave PDFE_BUILD_CONFIG exactly as it was found."""
    had = "PDFE_BUILD_CONFIG" in os.environ
    old = os.environ.get("PDFE_BUILD_CONFIG")
    yield
    if had:
        os.environ["PDFE_BUILD_CONFIG"] = old
    else:
        os.environ.pop("PDFE_BUILD_CONFIG", None)


def test_defaults_to_debug_when_unset():
    os.environ.pop("PDFE_BUILD_CONFIG", None)
    p = launch.pdfe_exe()
    assert Path(p).parts[-2] == "Debug", (
        f"with the variable unset the harness must still choose Debug; got {p}")


@pytest.mark.parametrize("config", ["Release", "Debug", "RelWithDebInfo"])
def test_the_requested_config_reaches_the_resolver(config):
    """The actual #270 assertion: the value REACHES the harness."""
    os.environ["PDFE_BUILD_CONFIG"] = config
    p = launch.pdfe_exe()
    assert Path(p).parts[-2] == config, (
        f"PDFE_BUILD_CONFIG={config!r} did not reach the resolver; it chose "
        f"{Path(p).parts[-2]!r}. This is the silent no-op #270 reports.")
    assert Path(p).name == "pdfe.exe"


def test_resolution_happens_at_call_time_not_import_time():
    """The subtler half, and the reason pdfe_exe() is a function.

    A module-level constant is evaluated ONCE. A test that sets the variable
    after this module was first imported — directly or transitively, and pytest
    import order follows collection, not intent — would get the value from
    whenever that first import happened. That is a second silent no-op of
    exactly the same shape as #270 itself.

    Re-importing the module here would NOT prove this: it would re-run the
    module body and pick up the new value, which is precisely the behaviour a
    cached constant does not have. So this asserts the resolver responds to a
    change made with the module already imported and untouched.
    """
    os.environ["PDFE_BUILD_CONFIG"] = "Release"
    first = launch.pdfe_exe()
    os.environ["PDFE_BUILD_CONFIG"] = "Debug"
    second = launch.pdfe_exe()

    assert first != second, (
        "the resolver returned the same path for Release and Debug without any "
        "re-import, so it is caching — which is the import-time-freeze defect")
    assert Path(first).parts[-2] == "Release"
    assert Path(second).parts[-2] == "Debug"


def test_the_launcher_uses_the_resolver_not_the_cached_constant():
    """`launch_pdfe` must call `pdfe_exe()`, not read `PDFE_EXE`.

    `PDFE_EXE` is deliberately kept — roughly 274 references across ~91 files
    read it, nearly all to display a path or check existence, and renaming them
    is a far larger change than this defect warrants. But the ONE place that
    decides which binary actually runs must not use it.

    Checked against the source rather than by launching, so this test needs no
    build and no desktop. A grep-shaped assertion is weak evidence in general,
    but here the claim IS about which identifier appears at the launch site.
    """
    src = Path(launch.__file__).read_text(encoding="utf-8")
    marker = "cmd = [exe]"
    assert marker in src, (
        "launch_pdfe no longer builds its command from the resolved `exe` "
        "local; if that was renamed, update this test deliberately rather than "
        "letting it pass vacuously")
    assert "cmd = [PDFE_EXE]" not in src, (
        "the launch site reads the cached PDFE_EXE constant again — that is the "
        "#270 regression, and it is invisible at runtime because the wrong "
        "binary still launches successfully")


def test_a_missing_binary_names_the_config():
    """The diagnosis, not just the failure.

    Pre-fix, a Release run on a Debug-only tree produced a missing-file error
    that never mentioned PDFE_BUILD_CONFIG, so it read as a broken checkout.
    """
    os.environ["PDFE_BUILD_CONFIG"] = "NoSuchConfigXYZ"
    with pytest.raises(FileNotFoundError) as ei:
        launch.launch_pdfe()
    msg = str(ei.value)
    assert "NoSuchConfigXYZ" in msg, (
        "the error must name the configuration that was requested")
    assert "PDFE_BUILD_CONFIG" in msg, (
        "...and the variable that selected it, so the reader knows where to look")
