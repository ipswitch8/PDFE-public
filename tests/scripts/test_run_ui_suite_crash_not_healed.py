#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A UI-suite retry must not heal a crashed application (GitHub #265).

`run_ui_suite.py` re-runs a problem file so a transient UIA flake self-heals on
a fresh launch. That inference is sound for order-independent flakiness and
inverted for a crash — isolation is what makes both disappear — and it is worse
here than in the gtest runner (#264) for two reasons:

  * the thing that dies is a SEPARATE PROCESS. `pdfe.exe` faulting reaches this
    runner only as a failing or timed-out pytest file, which is exactly the
    shape the retry exists to absorb;
  * the retry used to re-run "into the same junit/log, overwriting", so the
    passing attempt REPLACED the failing artifact and afterwards nothing
    recorded that anything had gone wrong — not the reason, not the fact.

The signal is a minidump: `run_one` points `PDFE_CRASH_DUMP_DIR` at a
per-attempt directory, and `src/main.cpp` installs the handler that writes
there.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "run_ui_suite.py"
sys.path.insert(0, str(HERE))

_spec = importlib.util.spec_from_file_location("_run_ui_suite_under_test", SCRIPT)
ui = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ui)


class _Proc:
    def __init__(self, rc: int = 0) -> None:
        self.returncode = rc


def _stub_subprocess(monkeypatch, rc: int, on_run=None):
    """Make run_one's pytest subprocess a no-op returning `rc`."""
    def fake_run(cmd, **kw):
        if on_run is not None:
            on_run(cmd, kw)
        logf = kw.get("stdout")
        if hasattr(logf, "write"):
            logf.write("stub\n")
        return _Proc(rc)
    monkeypatch.setattr(ui.subprocess, "run", fake_run)
    monkeypatch.setattr(ui, "scrub_log_file", lambda *a, **k: None)
    monkeypatch.setattr(ui, "scrub_junit_file", lambda *a, **k: None)


def test_the_dump_dir_is_passed_to_the_child(tmp_path, monkeypatch):
    """Without PDFE_CRASH_DUMP_DIR in the child's env there is no signal."""
    seen = {}
    _stub_subprocess(monkeypatch, 0,
                     on_run=lambda cmd, kw: seen.update(kw.get("env") or {}))
    dumps = tmp_path / "d"
    ui.run_one(tmp_path / "t.py", tmp_path / "j.xml", tmp_path / "l.log",
               60, tmp_path, "pdfe.exe", reap=False, crash_dump_dir=dumps)
    assert seen.get("PDFE_CRASH_DUMP_DIR") == str(dumps)


def test_a_dump_marks_the_attempt_as_an_app_crash(tmp_path, monkeypatch):
    dumps = tmp_path / "d"

    def drop_a_dump(cmd, kw):
        dumps.mkdir(parents=True, exist_ok=True)
        (dumps / "pdfe_1234.dmp").write_bytes(b"MDMP")

    _stub_subprocess(monkeypatch, 1, on_run=drop_a_dump)
    r = ui.run_one(tmp_path / "t.py", tmp_path / "j.xml", tmp_path / "l.log",
                   60, tmp_path, "pdfe.exe", reap=False, crash_dump_dir=dumps)
    assert r["app_crashed"] is True
    assert r["app_crash_dumps"] == ["pdfe_1234.dmp"]


def test_no_dump_means_no_app_crash(tmp_path, monkeypatch):
    """The discriminator. A failing test that did NOT crash the app must stay
    healable, or the retry mitigation this suite needs is gone."""
    _stub_subprocess(monkeypatch, 1)
    r = ui.run_one(tmp_path / "t.py", tmp_path / "j.xml", tmp_path / "l.log",
                   60, tmp_path, "pdfe.exe", reap=False,
                   crash_dump_dir=tmp_path / "d")
    assert r["app_crashed"] is False
    assert r["app_crash_dumps"] == []


def test_the_dump_dir_is_optional_for_the_other_importers(tmp_path, monkeypatch):
    """run_render_suite.py and f48_producer_probe.py call run_one without it."""
    _stub_subprocess(monkeypatch, 0)
    r = ui.run_one(tmp_path / "t.py", tmp_path / "j.xml", tmp_path / "l.log",
                   60, tmp_path, "pdfe.exe", reap=False)
    assert "app_crashed" not in r
    assert "app_crash_dumps" not in r


# ---------------------------------------------------------------------------
# The summary must keep a crashed file red even when the last attempt passed.
# ---------------------------------------------------------------------------
def _problems(results):
    """The REAL predicate, not a copy of it.

    An earlier version of this file mirrored the expression from main(). A
    mirror passes while the original drifts, which is the shape DCA 0015
    records -- so the predicate was extracted and both callers use it.
    """
    return [r for r in results if ui.file_has_problem(r)]


def test_a_healed_file_whose_app_crashed_stays_a_problem():
    healed_but_crashed = {"timed_out": False, "returncode": 0,
                          "healed_on_retry": True, "unhealable_app_crash": True}
    assert _problems([healed_but_crashed]) == [healed_but_crashed]


def test_a_genuinely_healed_file_is_not_a_problem():
    """The control: the rule must not redden every retry."""
    healed = {"timed_out": False, "returncode": 0, "healed_on_retry": True}
    assert _problems([healed]) == []


def test_a_still_failing_file_is_a_problem():
    assert _problems([{"timed_out": False, "returncode": 1}])


@pytest.mark.parametrize("field", ["first_attempt_outcome", "healed_on_retry"])
def test_the_retry_loop_records_what_it_superseded(field):
    """A heal must name the outcome it replaced, or the artifact cannot say
    what healed -- the #264/DCA-0101 lesson applied here."""
    src = SCRIPT.read_text(encoding="utf-8")
    assert field in src, (
        "run_ui_suite.py no longer records %r, so a healed file's original "
        "outcome is unrecoverable from the summary" % field)


def test_each_attempt_is_preserved_rather_than_overwritten():
    """The artifact-destruction half of #265."""
    src = SCRIPT.read_text(encoding="utf-8")
    assert ".attempt{attempt}." in src or "attempt{attempt}" in src, (
        "the retry no longer copies the superseded attempt aside; a passing "
        "retry would again replace the failing artifact outright")
