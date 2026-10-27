# -*- coding: utf-8 -*-
"""Phase-25 criterion: a MERGED per-shard UI aggregate scores like a single run.

    "check_matrix_health.py can consume a MERGED ui_test_matrix_aggregate.csv
     assembled from per-shard aggregates -- verified by a test that a merged
     aggregate scores identically to a single-run aggregate. Without this,
     sharding the UI leg makes every shard look like it dropped the
     committed-green rows it did not run."

That last sentence is the hazard, and it is not hypothetical arithmetic: each
shard reports every row it did not run as `no-run`, and the gate FAILS a
committed-green row that lands on `no-run` under --require-ran. So a naive merge
(or scoring one shard alone) turns a perfectly healthy sharded sweep into a
mass-regression report, and the natural "fix" for that noise is to stop running
the gate -- which is how a control dies.

Run:
    python -m pytest tests/scripts/test_matrix_shard_merge.py
    python tests/scripts/test_matrix_shard_merge.py
"""
from __future__ import annotations

import io
import sys
import csv
import shutil
import tempfile
import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "check_matrix_health", HERE / "check_matrix_health.py")
mh = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mh)

# A committed matrix: 9 green, 1 planned.
COMMITTED = [(f"ui.test_{i}", "green") for i in range(9)] + [("ui.test_9", "planned")]
# The single-run truth: 8 green, 1 red (test_3), plus the planned row.
SINGLE = {f"ui.test_{i}": "green" for i in range(9)}
SINGLE["ui.test_3"] = "red"
SINGLE["ui.test_9"] = "planned"


def _write(path: Path, rows) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with io.open(path, "w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["test_id", "status"])
        for tid, status in rows:
            w.writerow([tid, status])
    return path


def _shard_rows(shard: int, shards: int):
    """Shard `shard` reports its own rows truthfully and the rest as no-run.

    This mirrors what the real sweep writes: the runner emits the full matrix
    with out-of-scope rows marked no-run, which is precisely why merging cannot
    be last-writer-wins.
    """
    out = []
    for i, (tid, _committed) in enumerate(COMMITTED):
        out.append((tid, SINGLE[tid] if i % shards == shard else "no-run"))
    return out


def _score(committed: Path, runtime_status):
    return mh.evaluate(committed, None, floor_delta=5, require_ran=True,
                       runtime_status=runtime_status)


class _Sandbox:
    def __enter__(self):
        self.root = Path(tempfile.mkdtemp(prefix="pdfe-matrix-merge-"))
        self.committed = _write(self.root / "committed.csv", COMMITTED)
        return self

    def __exit__(self, *a):
        shutil.rmtree(self.root, ignore_errors=True)


# ---------------------------------------------------------------------------

def test_merged_scores_identically_to_a_single_run():
    """THE criterion."""
    with _Sandbox() as sb:
        single_ok, single_msgs = _score(sb.committed, dict(SINGLE))

        d = sb.root / "shards"
        for s in range(4):
            _write(d / f"ui_test_matrix_2026_shard{s}.csv", _shard_rows(s, 4))
        merged, conflicts = mh.merge_shard_status(mh._shard_csvs(d))
        merged_ok, merged_msgs = _score(sb.committed, merged)

    assert not conflicts, conflicts
    assert merged == SINGLE, (
        "the merged status map differs from the single-run truth:\n  merged=%r\n"
        "  single=%r" % (merged, SINGLE))
    assert merged_ok == single_ok, (
        "merged verdict %r != single-run verdict %r" % (merged_ok, single_ok))
    assert sorted(merged_msgs) == sorted(single_msgs), (
        "merged messages differ from single-run messages:\n  %r\n  %r"
        % (merged_msgs, single_msgs))


def test_the_comparison_is_not_vacuous():
    """Both sides must actually be scoring something, and finding the red row."""
    with _Sandbox() as sb:
        ok, msgs = _score(sb.committed, dict(SINGLE))
    assert ok is False, "the single-run reference is HEALTHY, so 'merged == single' " \
                        "would be satisfied by two clean sheets"
    assert any("ui.test_3" in m for m in msgs), (
        "the reference scoring did not name the red row; the comparison would "
        "not notice a merge that lost it: %r" % msgs)


def test_a_single_shard_alone_looks_like_mass_regression():
    """Pins WHY merging is required -- the failure this criterion describes."""
    with _Sandbox() as sb:
        d = sb.root / "one"
        _write(d / "ui_test_matrix_2026_shard0.csv", _shard_rows(0, 4))
        one, _ = mh.merge_shard_status(mh._shard_csvs(d))
        ok, msgs = _score(sb.committed, one)
    assert ok is False and len(msgs) > 2, (
        "scoring ONE shard did not look like a regression, so this test is not "
        "pinning the hazard it claims to: %r" % msgs)


def test_a_no_run_never_overwrites_a_real_result():
    """The specific merge bug: a later shard's no-run erasing an earlier green."""
    with _Sandbox() as sb:
        d = sb.root / "order"
        # shard0 ran test_0 (green); shard1 reports test_0 as no-run.
        _write(d / "ui_test_matrix_a.csv", [("ui.test_0", "green")])
        _write(d / "ui_test_matrix_b.csv", [("ui.test_0", "no-run")])
        merged, conflicts = mh.merge_shard_status(mh._shard_csvs(d))
    assert not conflicts
    assert merged["ui.test_0"] == "green", (
        "a no-run overwrote a real green: %r" % merged)


def test_a_no_run_never_hides_a_red():
    """The dangerous direction of the same bug."""
    with _Sandbox() as sb:
        d = sb.root / "hide"
        _write(d / "ui_test_matrix_a.csv", [("ui.test_0", "red")])
        _write(d / "ui_test_matrix_b.csv", [("ui.test_0", "no-run")])
        merged, _ = mh.merge_shard_status(mh._shard_csvs(d))
    assert merged["ui.test_0"] == "red", (
        "a no-run erased a RED result, which would turn a failing sweep green: "
        "%r" % merged)


def test_two_shards_disagreeing_is_a_conflict_not_a_coin_toss():
    with _Sandbox() as sb:
        d = sb.root / "conflict"
        _write(d / "ui_test_matrix_a.csv", [("ui.test_0", "green")])
        _write(d / "ui_test_matrix_b.csv", [("ui.test_0", "red")])
        _merged, conflicts = mh.merge_shard_status(mh._shard_csvs(d))
    assert conflicts, "two shards disagreeing was silently resolved"
    assert "not disjoint" in conflicts[0]


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print("  %-52s PASS" % t.__name__)
        except AssertionError as exc:
            failed += 1
            print("  %-52s FAIL" % t.__name__)
            print("      " + str(exc).replace("\n", "\n      "))
    print("")
    if failed:
        print("RESULT: %d of %d FAILED" % (failed, len(tests)))
        return 1
    print("RESULT: all %d checks pass" % len(tests))
    return 0


if __name__ == "__main__":
    sys.exit(main())
