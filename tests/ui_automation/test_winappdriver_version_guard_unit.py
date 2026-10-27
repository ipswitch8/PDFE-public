# -*- coding: utf-8 -*-
"""Unit tests for the WinAppDriver prerequisite guard (gap A1-64 / UI-SKIP-01).

Headless: every test feeds a synthetic ``/status`` payload or monkeypatches the
reachability probe. No service, no PDFE, no display.

Ground truth established while fixing this gap (this host, Server 2019):

* WinAppDriver **is** installed at
  ``C:\\Program Files (x86)\\Windows Application Driver\\WinAppDriver.exe``.
* Its live ``GET /status`` answers
  ``{"build": {"revision": "2003", "version": "1.2.2009"}, "os": {...}}`` —
  i.e. version **1.2.2009**, comfortably above the ``>= 1.2.1`` floor.
* The service was simply **not running**; nothing listens on 4723 by default.

So the two WAD suites were NOT skipping because of the version floor, as the
audit row assumed. They skipped on reachability — and worse, the version
parser could never have enforced the floor anyway, because it read the wrong
place in the payload (see :func:`parse_status_version`).
"""
from __future__ import annotations

import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests import notrun  # noqa: E402
from tests.ui_automation.helpers import (  # noqa: E402
    winappdriver_dialog_driver as wad,
)

#: The exact body the live 1.2.2009 service returned on this host.
LIVE_STATUS_BODY = {
    "build": {"revision": "2003", "time": "Wed Aug 26 07:56:06 2020",
              "version": "1.2.2009"},
    "os": {"arch": "amd64", "name": "windows", "version": "10.0.17763"},
}


# ---------------------------------------------------------------------------
# The version parser
# ---------------------------------------------------------------------------

def test_parses_top_level_build_version():
    """MUTATION KILLED: reverting to ``body.get("value", {})``-only parsing.
    That returns None for the real 1.2.x payload above, which
    require_winappdriver_version() treats as 'cannot tell — do not skip', so
    the >= 1.2.1 floor becomes unenforceable."""
    assert wad.parse_status_version(LIVE_STATUS_BODY) == "1.2.2009"


def test_parses_value_nested_build_version():
    """MUTATION KILLED: dropping the legacy ``value``-wrapped shape while
    fixing the top-level one."""
    assert wad.parse_status_version(
        {"value": {"build": {"version": "1.1.1"}}}) == "1.1.1"


def test_never_reports_os_version_as_driver_version():
    """MUTATION KILLED: the old fall-through from ``build`` to ``os``, which
    would report Windows' 10.0.17763 as the WinAppDriver version — a value
    above every conceivable floor, i.e. a guaranteed false pass."""
    body = {"os": {"arch": "amd64", "name": "windows",
                   "version": "10.0.17763"}}
    assert wad.parse_status_version(body) is None


def test_returns_none_for_versionless_body():
    assert wad.parse_status_version({}) is None
    assert wad.parse_status_version({"build": {"revision": "2003"}}) is None
    assert wad.parse_status_version("not a dict") is None


def test_live_version_tuple_meets_floor(monkeypatch):
    """The whole point: the real payload must now produce a tuple that can be
    compared against the floor.
    MUTATION KILLED: a parser that 'works' but yields an uncomparable type."""
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: True)
    monkeypatch.setattr(wad, "_http", lambda *a, **k: LIVE_STATUS_BODY)
    ver = wad.get_winappdriver_version()
    assert ver == (1, 2, 2009)
    assert ver >= wad.MIN_WINAPPDRIVER_VERSION


def test_old_beta_version_is_below_floor(monkeypatch):
    """MUTATION KILLED: removing the floor comparison entirely."""
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: True)
    monkeypatch.setattr(wad, "_http",
                        lambda *a, **k: {"build": {"version": "0.9.0"}})
    assert wad.get_winappdriver_version() < wad.MIN_WINAPPDRIVER_VERSION


# ---------------------------------------------------------------------------
# The guard reports NOT-RUN, with an accurate cause
# ---------------------------------------------------------------------------

def _skip_reason(monkeypatch) -> str:
    with pytest.raises(BaseException) as exc:
        wad.require_winappdriver_version()
    return str(getattr(exc.value, "msg", exc.value))


def test_service_down_but_installed_reports_launcher_cause(monkeypatch):
    """MUTATION KILLED: the old single message that told the operator to
    INSTALL WinAppDriver when it was already installed — the misdiagnosis that
    left these suites skipped for months on a host that had 1.2.2009."""
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: False)
    monkeypatch.setattr(wad, "autostart_enabled", lambda: False)
    monkeypatch.setattr(wad, "installed_winappdriver_exe",
                        lambda: r"C:\WAD\WinAppDriver.exe")
    reason = _skip_reason(monkeypatch)
    assert reason.startswith(notrun.NOT_RUN_PREFIX)
    assert notrun.parse_cause(reason) == "winappdriver-service-down"
    assert "IS installed" in reason
    assert wad.AUTOSTART_ENV_VAR in reason


def test_not_installed_reports_provisioning_cause(monkeypatch):
    """MUTATION KILLED: collapsing 'not installed' and 'not started' back into
    one cause — they need different fixes."""
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: False)
    monkeypatch.setattr(wad, "autostart_enabled", lambda: False)
    monkeypatch.setattr(wad, "installed_winappdriver_exe", lambda: None)
    reason = _skip_reason(monkeypatch)
    assert notrun.parse_cause(reason) == "winappdriver-not-installed"


def test_too_old_reports_version_cause(monkeypatch):
    """MUTATION KILLED: silently accepting a below-floor service."""
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: True)
    monkeypatch.setattr(wad, "get_winappdriver_version", lambda: (0, 9, 0))
    reason = _skip_reason(monkeypatch)
    assert notrun.parse_cause(reason) == "winappdriver-too-old"


def test_guard_passes_through_when_service_is_good(monkeypatch):
    """MUTATION KILLED: a guard that skips unconditionally (which would make
    every negative test above pass while blocking the lane forever)."""
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: True)
    monkeypatch.setattr(wad, "get_winappdriver_version", lambda: (1, 2, 2009))
    assert wad.require_winappdriver_version() == (1, 2, 2009)


def test_autostart_is_attempted_only_when_enabled(monkeypatch):
    """MUTATION KILLED: auto-starting a service unconditionally (surprising
    side effect), or never wiring the autostart path at all."""
    calls = []
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: False)
    monkeypatch.setattr(wad, "installed_winappdriver_exe",
                        lambda: r"C:\WAD\WinAppDriver.exe")
    monkeypatch.setattr(wad, "start_winappdriver_service",
                        lambda *a, **k: calls.append("start") or False)

    monkeypatch.setattr(wad, "autostart_enabled", lambda: False)
    _skip_reason(monkeypatch)
    assert calls == []

    monkeypatch.setattr(wad, "autostart_enabled", lambda: True)
    _skip_reason(monkeypatch)
    assert calls == ["start"]


def test_installed_exe_detection_uses_env_override(monkeypatch, tmp_path):
    """MUTATION KILLED: hard-coding only the default install path, so a
    non-default install reads as 'not installed'."""
    fake = tmp_path / "WinAppDriver.exe"
    fake.write_bytes(b"stub")
    monkeypatch.setenv("PDFE_WINAPPDRIVER_EXE", str(fake))
    assert wad.installed_winappdriver_exe() == str(fake)
    monkeypatch.setenv("PDFE_WINAPPDRIVER_EXE", str(tmp_path / "nope.exe"))
    # Falls back to the standard locations rather than trusting a bad override.
    assert wad.installed_winappdriver_exe() != str(tmp_path / "nope.exe")


def test_require_runnable_env_turns_not_run_into_failure(monkeypatch):
    """A lane declared able to run these tests must FAIL, not skip, when the
    prerequisite is missing.
    MUTATION KILLED: making PDFE_REQUIRE_RUNNABLE a no-op — without it there is
    no way for CI to assert the WAD lane actually exercised anything."""
    monkeypatch.setenv(notrun.REQUIRE_ENV_VAR, "1")
    monkeypatch.setattr(wad, "_service_reachable", lambda *a, **k: False)
    monkeypatch.setattr(wad, "autostart_enabled", lambda: False)
    monkeypatch.setattr(wad, "installed_winappdriver_exe", lambda: None)
    with pytest.raises(BaseException) as exc:
        wad.require_winappdriver_version()
    assert exc.typename == "Failed", (
        f"expected a hard failure under {notrun.REQUIRE_ENV_VAR}=1, got "
        f"{exc.typename}")
