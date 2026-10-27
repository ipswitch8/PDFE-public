# -*- coding: utf-8 -*-
"""Unit tests for NOT-RUN reporting (gaps A1-64 / A1-66 / A1-83).

The hole: pytest renders a skipped test as a green session, so a suite that
skips 100% of its assertions is visually indistinguishable from one that ran
and passed them. These tests lock in the machinery that separates "ran and
asserted" from "skipped clean".
"""
from __future__ import annotations

import json
import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests import notrun  # noqa: E402


class _FakeReport:
    def __init__(self, nodeid, reason, when="setup", skipped=True):
        self.nodeid = nodeid
        self.when = when
        self.skipped = skipped
        self.longrepr = ("file.py", 10, f"Skipped: {reason}")


class _FakeTerminal:
    def __init__(self):
        self.lines = []
        self.seps = []

    def write_line(self, line):
        self.lines.append(line)

    def write_sep(self, ch, title):
        self.seps.append(title)


@pytest.fixture(autouse=True)
def _isolate_collector(monkeypatch, tmp_path):
    monkeypatch.setattr(notrun, "_COLLECTED", [])
    monkeypatch.setattr(notrun, "REPORT_PATH", tmp_path / "not_run.json")
    monkeypatch.delenv(notrun.REQUIRE_ENV_VAR, raising=False)


# ---------------------------------------------------------------------------
# Reason formatting / parsing
# ---------------------------------------------------------------------------

def test_reason_is_prefixed_and_cause_tagged():
    """MUTATION KILLED: emitting a plain skip reason, which no log grep or CI
    step can distinguish from any other skip."""
    r = notrun.format_reason("winappdriver-service-down", "nothing listening",
                             "start it")
    assert r.startswith(notrun.NOT_RUN_PREFIX)
    assert notrun.parse_cause(r) == "winappdriver-service-down"
    assert "REMEDY" in r
    assert notrun.is_not_run(r) is True


def test_ordinary_skip_reason_is_not_classified_as_not_run():
    """MUTATION KILLED: classifying EVERY skip as NOT-RUN, which floods the
    section and gets it ignored."""
    assert notrun.is_not_run("fixture missing") is False


# ---------------------------------------------------------------------------
# Collection
# ---------------------------------------------------------------------------

def test_collector_records_not_run_skips():
    """MUTATION KILLED: a no-op ``record_report`` (the reporting hook wired
    but doing nothing)."""
    notrun.record_report(_FakeReport(
        "tests/ui_automation/test_a.py::test_x",
        notrun.format_reason("no-live-ui", "headless")))
    assert len(notrun._COLLECTED) == 1
    e = notrun._COLLECTED[0]
    assert e["cause"] == "no-live-ui"
    assert e["file"] == "tests/ui_automation/test_a.py"


def test_collector_ignores_passes_and_plain_skips():
    """MUTATION KILLED: counting passing tests as NOT-RUN (which would make
    the report meaningless in the other direction)."""
    notrun.record_report(_FakeReport("t.py::a", "boring skip"))
    r = _FakeReport("t.py::b", notrun.format_reason("x", "y"))
    r.skipped = False
    notrun.record_report(r)
    assert notrun._COLLECTED == []


def test_collector_ignores_teardown_phase_duplicates():
    """MUTATION KILLED: counting the same skip once per phase, inflating the
    NOT-RUN total."""
    rep = _FakeReport("t.py::a", notrun.format_reason("c", "d"),
                      when="teardown")
    notrun.record_report(rep)
    assert notrun._COLLECTED == []


# ---------------------------------------------------------------------------
# Summary + artifact
# ---------------------------------------------------------------------------

def test_summarise_aggregates_by_cause_and_file():
    entries = [
        {"nodeid": "a::1", "file": "a", "cause": "c1", "reason": "r"},
        {"nodeid": "a::2", "file": "a", "cause": "c1", "reason": "r"},
        {"nodeid": "b::1", "file": "b", "cause": "c2", "reason": "r"},
    ]
    s = notrun.summarise(entries)
    assert s["not_run_total"] == 3
    assert s["by_cause"] == {"c1": 2, "c2": 1}
    assert s["by_file"] == {"a": 2, "b": 1}


def test_terminal_summary_is_loud_and_writes_artifact():
    """MUTATION KILLED: writing the artifact but printing nothing (invisible
    to a human reading the run), or printing but writing nothing (invisible to
    CI). A1-83 needs BOTH."""
    notrun.record_report(_FakeReport(
        "tests/ui_automation/test_ui_encryption_flows_wad.py::test_a",
        notrun.format_reason("winappdriver-service-down", "down")))
    term = _FakeTerminal()
    notrun.terminal_summary(term)

    blob = "\n".join(term.lines)
    assert any("NOT RUN" in s for s in term.seps)
    assert "NOT passes" in blob
    assert "winappdriver-service-down" in blob

    payload = json.loads(notrun.REPORT_PATH.read_text(encoding="utf-8"))
    assert payload["not_run_total"] == 1
    assert payload["by_cause"] == {"winappdriver-service-down": 1}
    assert (payload["entries"][0]["file"]
            == "tests/ui_automation/test_ui_encryption_flows_wad.py")


def test_artifact_is_written_even_with_zero_not_run():
    """A green board must be able to PROVE it had no NOT-RUN tests, so the
    artifact exists either way.
    MUTATION KILLED: writing the file only when the list is non-empty — then a
    missing file is ambiguous between 'clean run' and 'reporting broken'."""
    notrun.terminal_summary(_FakeTerminal())
    payload = json.loads(notrun.REPORT_PATH.read_text(encoding="utf-8"))
    assert payload["not_run_total"] == 0


# ---------------------------------------------------------------------------
# skip_not_run behaviour
# ---------------------------------------------------------------------------

def test_skip_not_run_raises_skip_by_default():
    with pytest.raises(BaseException) as exc:
        notrun.skip_not_run("cause", "detail")
    assert exc.typename == "Skipped"
    assert notrun.NOT_RUN_PREFIX in str(getattr(exc.value, "msg", exc.value))


def test_skip_not_run_fails_hard_when_lane_requires_runnable(monkeypatch):
    """MUTATION KILLED: making PDFE_REQUIRE_RUNNABLE a no-op. Without it, a
    lane that is supposed to exercise these tests cannot detect that its
    prerequisites silently vanished."""
    monkeypatch.setenv(notrun.REQUIRE_ENV_VAR, "1")
    with pytest.raises(BaseException) as exc:
        notrun.skip_not_run("cause", "detail")
    assert exc.typename == "Failed"


def test_conftest_hooks_are_wired():
    """The plugin is worthless if nothing calls it.
    MUTATION KILLED: deleting the hook wiring from tests/conftest.py."""
    text = (REPO_ROOT / "tests" / "conftest.py").read_text(encoding="utf-8")
    assert "def pytest_runtest_logreport" in text
    assert "def pytest_terminal_summary" in text
    assert "notrun" in text
