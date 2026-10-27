# -*- coding: utf-8 -*-
"""Locking tests for green-wins matrix aggregation (pipeline c82a1f37 P1).

Root cause these tests pin down (the "false-block" bug):
    A row such as ``act_zoomInAction`` is OWNED by ``test_ui_batch1_actions``
    (records ``green`` when the QAction fires) but is ALSO iterated by a
    non-owning phase-loading suite (``test_annotations_p1`` filters
    ``owner_phase == phase-5``) which cannot parse it and records ``blocked``.
    Under the old precedence (green lost every tie) the non-owning ``blocked``
    clobbered the owning ``green``, so ~131 genuinely-passing ``act_`` rows read
    as blocked in the aggregate.

These tests lock the fix at BOTH layers:
  * ``run_ui_suite.aggregate_matrix`` — the cross-file (per-subprocess) merge.
  * ``conftest.record_matrix_result`` — the in-process (same-subprocess) merge.

No PDFE.exe / UI is launched: this is pure aggregation logic.
"""
from __future__ import annotations

import csv
import importlib.util
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.conftest import (  # noqa: E402
    MATRIX_RESULTS,
    record_matrix_result,
)


def _load_run_ui_suite():
    """Import tests/scripts/run_ui_suite.py by path (not a package)."""
    mod_path = REPO_ROOT / "tests" / "scripts" / "run_ui_suite.py"
    spec = importlib.util.spec_from_file_location("run_ui_suite_under_test", mod_path)
    mod = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(mod)
    return mod


_RUS = _load_run_ui_suite()

_FIELDNAMES = ["test_id", "status", "notes"]


def _write_matrix(path: Path, statuses: dict) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=_FIELDNAMES)
        w.writeheader()
        for tid, st in statuses.items():
            w.writerow({"test_id": tid, "status": st, "notes": ""})


def _aggregate(tmp_path: Path, committed: dict, per_file: list) -> dict:
    """Run aggregate_matrix over synthetic CSVs; return {test_id: status}.

    Models the pipeline e5b9c231 Phase 5 emission: each per-file CSV carries ALL
    rows, but a row this subprocess did NOT record is ``no-run`` (the honest
    sentinel) rather than a carry-forward copy of the committed status. Only rows
    listed in ``pf`` carry a real recorded status.
    """
    committed_csv = tmp_path / "committed.csv"
    _write_matrix(committed_csv, committed)
    runtime_csvs = []
    for i, pf in enumerate(per_file):
        merged = {tid: "no-run" for tid in committed}
        merged.update(pf)
        p = tmp_path / f"runtime_{i}.csv"
        _write_matrix(p, merged)
        runtime_csvs.append(p)
    out = tmp_path / "aggregate.csv"
    _RUS.aggregate_matrix(committed_csv, runtime_csvs, out)
    with out.open(encoding="utf-8", newline="") as f:
        return {r["test_id"]: r["status"] for r in csv.DictReader(f)}


# ---------------------------------------------------------------------------
# Cross-file aggregate (the dominant false-block path)
# ---------------------------------------------------------------------------

def test_aggregate_green_beats_blocked(tmp_path):
    """Owning suite green + non-owning suite blocked -> green (the real bug)."""
    committed = {"act_zoomInAction": "blocked"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[
            {"act_zoomInAction": "green"},    # owning batch suite
            {"act_zoomInAction": "blocked"},  # non-owning phase suite
        ],
    )
    assert result["act_zoomInAction"] == "green"


def test_aggregate_green_beats_skip(tmp_path):
    committed = {"row_x": "blocked"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[{"row_x": "green"}, {"row_x": "skip"}],
    )
    assert result["row_x"] == "green"


def test_aggregate_red_beats_blocked(tmp_path):
    """A genuine red must dominate (false-green defense preserved)."""
    committed = {"row_x": "blocked"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[{"row_x": "red"}, {"row_x": "blocked"}],
    )
    assert result["row_x"] == "red"


def test_aggregate_red_beats_skip(tmp_path):
    committed = {"row_x": "blocked"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[{"row_x": "red"}, {"row_x": "skip"}],
    )
    assert result["row_x"] == "red"


def test_aggregate_red_dominates_green(tmp_path):
    """Both recorded this run: red wins so a real failure is never hidden
    behind another suite's green (green->red regression detection)."""
    committed = {"row_x": "blocked"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[{"row_x": "green"}, {"row_x": "red"}],
    )
    assert result["row_x"] == "red"


def test_aggregate_owner_green_beats_nonowning_blocked_committed_green(tmp_path):
    """False-block fix under the honest emission: committed == green, the OWNING
    suite records green (now a visible real result, not dropped), a non-owning
    suite records blocked. Green-wins keeps the row green."""
    committed = {"row_x": "green"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[
            {"row_x": "green"},    # owning suite records green this run
            {"row_x": "blocked"},  # non-owning suite
        ],
    )
    assert result["row_x"] == "green"


def test_aggregate_committed_green_not_run_becomes_no_run(tmp_path):
    """H4 honesty (criterion 1/2 at the aggregate layer): a committed-green row
    that NO suite exercises this sweep must NOT be resurrected as green — it
    resolves to no-run so --require-ran can flag lost coverage."""
    committed = {"row_x": "green"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[{}, {}],  # two suites ran, neither recorded row_x
    )
    assert result["row_x"] == "no-run", (
        "stale committed green was resurrected in the aggregate — the runtime "
        "matrix is still 'rebuilt from committed green'")


def test_aggregate_committed_green_regresses_to_red(tmp_path):
    """A real green->red regression must still be surfaced by the aggregate."""
    committed = {"row_x": "green"}
    result = _aggregate(tmp_path, committed, per_file=[{"row_x": "red"}])
    assert result["row_x"] == "red"


def test_aggregate_wont_fix_is_preserved(tmp_path):
    """A committed wont-fix row (emitted verbatim by every subprocess) stays
    wont-fix — it is a permanent disposition, not a per-run result."""
    committed = {"row_x": "wont-fix"}
    result = _aggregate(
        tmp_path, committed,
        per_file=[{"row_x": "wont-fix"}, {"row_x": "wont-fix"}],
    )
    assert result["row_x"] == "wont-fix"


# ---------------------------------------------------------------------------
# In-process record_matrix_result (same-subprocess collision guard)
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _clean_matrix_results():
    saved = dict(MATRIX_RESULTS)
    MATRIX_RESULTS.clear()
    yield
    MATRIX_RESULTS.clear()
    MATRIX_RESULTS.update(saved)


def test_inproc_blocked_never_overwrites_green():
    record_matrix_result("row_x", "green", "owning test passed")
    record_matrix_result("row_x", "blocked", "non-owning test skipped")
    assert MATRIX_RESULTS["row_x"]["status"] == "green"


def test_inproc_green_upgrades_blocked():
    record_matrix_result("row_x", "blocked", "non-owning test recorded first")
    record_matrix_result("row_x", "green", "owning test passed after")
    assert MATRIX_RESULTS["row_x"]["status"] == "green"


def test_inproc_red_overwrites_green():
    record_matrix_result("row_x", "green")
    record_matrix_result("row_x", "red", "later real failure")
    assert MATRIX_RESULTS["row_x"]["status"] == "red"


def test_inproc_green_does_not_overwrite_red():
    record_matrix_result("row_x", "red", "real failure seen first")
    record_matrix_result("row_x", "green")
    assert MATRIX_RESULTS["row_x"]["status"] == "red"
