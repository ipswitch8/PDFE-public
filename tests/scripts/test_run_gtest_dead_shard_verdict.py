# -*- coding: utf-8 -*-
"""GitHub #288 — a shard that dies MID-RUN must not be reported as a clean run.

THE DEFECT, measured on a 16-shard local run 2026-10-07 when
`TileRendererTest.CancelPage` tripped a fitz assertion and killed shard 7
(GitHub #287). Four outputs, three honest and one not:

| output | said |
|---|---|
| merged `gtest_full.xml` | `tests=6527 failures=0 errors=408` — RED |
| `healed_flakes.json` | `"healed": []`, 408 names under `not_run_due_to_dead_shard` |
| final console line | `all 408 failure(s)/error(s) healed on a fresh rerun. CLEAN.` |
| process exit code | **0** |

The data layer was correct. Only the verdict was wrong, and the verdict is what
a caller reads.

WHY IT FELL THROUGH. `run_sharded`'s verdict had three exits, and a mid-run
crash matched none of them:

* `unresolved` was empty — every one of the 408 PASSED when re-run alone,
  because they never ran in the first place;
* `teardown_crashed_shards` was empty — the shard died DURING the run, not on
  the way out;
* so control reached the unconditional `CLEAN` / `return 0`.

`never_ran` was already computed for the healed ledger and for the aggregate
rewrite, and simply never consulted by the verdict. F70(b) stopped those tests
being *called* healed; nothing stopped the run being *called* clean.

WHY A PASSING RERUN IS NOT EVIDENCE HERE. These ids did not run in situ. GitHub
#264 already establishes that an order-dependent crash passes when re-run alone
— so "it passes by itself" is the signature of the defect, not an argument
against it.

WHAT THESE CASES PIN. The sensitivity direction (a dead shard is RED) AND the
specificity direction (an ordinary clean sharded run is still GREEN, and a
genuine heal is still a heal). Without the second, "return 1 if anything looks
odd" would satisfy the first and break every passing run — which is the shape
that gets a guard switched off.

Note the single-process path was fixed earlier and is covered by
`test_run_gtest_single_process_crash.py`; this file is the SHARDED path, which
that fix did not reach.
"""
from __future__ import annotations

import importlib.util
import io
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RUN_GTEST = ROOT / "tests" / "scripts" / "run_gtest.py"

#: A complete, green, parseable shard report for two tests.
GREEN_TWO = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="2" failures="0" disabled="0" errors="0" name="AllTests">
  <testsuite name="S" tests="2" failures="0" disabled="0" errors="0">
    <testcase name="A" status="run" result="completed" classname="S"/>
    <testcase name="B" status="run" result="completed" classname="S"/>
  </testsuite>
</testsuites>
"""


def _load():
    spec = importlib.util.spec_from_file_location("run_gtest_dsv", RUN_GTEST)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["run_gtest_dsv"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mod():
    return _load()


ALL_IDS = ["S.A", "S.B", "S.C", "S.D"]


def _harness(mod, monkeypatch, *, dead_shard: int | None, rerun_passes: bool):
    """Drive the REAL `run_sharded` verdict path with a controlled process seam.

    Everything that touches a child process is replaced; the partitioning,
    merging, synthesis, rerun bookkeeping and verdict are the production code.

    `dead_shard` writes NO xml for that shard index, which is what a mid-run
    crash looks like to the harness. `rerun_passes` controls the isolated
    rerun, because the defect depends on those reruns passing.
    """
    monkeypatch.setattr(mod, "_list_tests",
                        lambda *a, **k: list(ALL_IDS))
    monkeypatch.setattr(mod, "flagfile_is_honoured",
                        lambda *a, **k: (False, "stubbed"))
    monkeypatch.setattr(mod, "report_external_corpus_skips",
                        lambda *a, **k: None)
    monkeypatch.setattr(mod, "shard_filter_budget",
                        lambda *a, **k: (True, 0, 1))

    calls = {"shards": 0, "reruns": 0}

    def fake_run(exe, out_xml, log, gtest_filter, flagfile=None):
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        io.open(log, "w", encoding="utf-8").write("stub\n")
        name = Path(out_xml).name
        if name.startswith("gtest_shard_"):
            idx = int(name.split("_")[-1].split(".")[0])
            calls["shards"] += 1
            if dead_shard is not None and idx == dead_shard:
                return 0xC0000005 - (1 << 32)   # access violation, no xml
            io.open(out_xml, "w", encoding="utf-8").write(GREEN_TWO)
            return 0
        # An isolated rerun of one test.
        calls["reruns"] += 1
        if rerun_passes:
            io.open(out_xml, "w", encoding="utf-8").write(GREEN_TWO)
            return 0
        io.open(out_xml, "w", encoding="utf-8").write(
            GREEN_TWO.replace('failures="0"', 'failures="1"', 1))
        return 1

    monkeypatch.setattr(mod, "_run", fake_run)
    return calls


def test_a_shard_that_dies_mid_run_is_NOT_clean(mod, tmp_path, monkeypatch):
    """THE DEFECT. Dead shard, every orphaned test 'heals', verdict must be RED."""
    _harness(mod, monkeypatch, dead_shard=1, rerun_passes=True)
    rc = mod.run_sharded("fake.exe", tmp_path, 2, None, 1, None)
    assert rc != 0, (
        "a shard died mid-run, its tests never executed, and the run was "
        "still reported CLEAN with exit 0 -- this is GitHub #288 exactly. The "
        "orphaned tests pass when re-run ALONE, which is the signature of an "
        "order-dependent crash (GitHub #264), not evidence of health.")


def test_the_dead_shard_is_recorded_as_never_run_not_as_healed(mod, tmp_path,
                                                               monkeypatch):
    """The data layer was always right; this asserts it stays right.

    If a future change 'fixed' the verdict by reclassifying these as genuine
    heals, the exit code would go red for the wrong reason and the ledger that
    FEEDS check_flaky_quarantine.py would be contaminated again (F70b).
    """
    _harness(mod, monkeypatch, dead_shard=1, rerun_passes=True)
    mod.run_sharded("fake.exe", tmp_path, 2, None, 1, None)
    ledger = json.loads(
        (tmp_path / "healed_flakes.json").read_text(encoding="utf-8"))
    assert ledger["not_run_due_to_dead_shard"], (
        "the dead shard's tests are missing from not_run_due_to_dead_shard")
    assert not ledger["healed"], (
        "a never-executed test was recorded as healed-on-rerun; F70b exists to "
        "stop exactly that, and it feeds the flaky-quarantine gate")


def test_an_ordinary_clean_sharded_run_is_still_GREEN(mod, tmp_path, monkeypatch):
    """THE SPECIFICITY DIRECTION, and it is not ceremonial.

    Both cases above expect RED, so alone they prove only that the path is
    SENSITIVE. A fix of the form 'return 1 whenever a shard looks unusual'
    would satisfy them and redden every passing run -- and a verdict that is
    always red gets ignored, which is how this defect class survives.
    """
    _harness(mod, monkeypatch, dead_shard=None, rerun_passes=True)
    rc = mod.run_sharded("fake.exe", tmp_path, 2, None, 1, None)
    assert rc == 0, (
        "a fully clean two-shard run was reported as a failure; the #288 fix "
        "must key on a DEAD SHARD, not on anything that merely looks odd")


def test_a_genuine_failure_is_still_red_without_any_dead_shard(mod, tmp_path,
                                                               monkeypatch):
    """The other half of specificity: red for the ordinary reason, too.

    A run with no crashed shard but a test that fails every rerun must still
    exit non-zero, and must do so via the `unresolved` branch rather than the
    new dead-shard one -- otherwise the fix has swallowed the normal path.
    """
    _harness(mod, monkeypatch, dead_shard=None, rerun_passes=False)
    # Force a failure into the aggregate so there is something to rerun.
    monkeypatch.setattr(
        mod, "_failed_ids_from_root", lambda root: ["S.A"])
    rc = mod.run_sharded("fake.exe", tmp_path, 2, None, 1, None)
    assert rc != 0, "a test failing every rerun must still be reported RED"
    ledger = json.loads(
        (tmp_path / "healed_flakes.json").read_text(encoding="utf-8"))
    assert not ledger["not_run_due_to_dead_shard"], (
        "no shard died, so nothing should be attributed to a dead shard -- if "
        "this is non-empty the new branch is firing on the wrong population")
