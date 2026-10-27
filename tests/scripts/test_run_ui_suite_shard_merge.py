# -*- coding: utf-8 -*-
"""A multi-shard ui-suite summary must MERGE, not overwrite (GitHub #278).

WHY THESE EXIST
===============
Every shard wrote `ui-suite-summary.json` and nothing distinguished it from a
whole-sweep summary. In CI the three shards are separate matrix jobs with their
own workspaces, so nothing is clobbered on disk -- the collision happens when the
three identically-named artifacts are collected, and then ONE SHARD'S NUMBERS
STAND FOR THE RUN.

That is not hypothetical. GitHub #277 records it happening to a reader twice:
"2,500 tests, 16 failures" reported, then noticed that a later shard had replaced
the file with "560 tests, 14 failures".

`merge_shard_summaries` is a pure function over parsed summaries precisely so it
can be exercised without running the suite -- a merge nobody can run is how the
overwrite survived.
"""
from __future__ import annotations

import io
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
SCRIPT = HERE / "run_ui_suite.py"
sys.path.insert(0, str(HERE))

import run_ui_suite as R  # noqa: E402


def shard_summary(shard, shards, *, tests, failures, errors, skipped,
                  files, failing_names=(), problems=()):
    """A summary shaped like the real one, for the fields the merge reads."""
    return {
        "files_run": len(files),
        "wall_clock_s": 100.0 * shard,
        "totals": {"tests": tests, "failures": failures, "errors": errors,
                   "skipped": skipped, "time": 1.5},
        "failure_count": len(failing_names),
        "per_file": [{"file": f, "returncode": 0} for f in files],
        "failures": [{"file": n, "test": n + "::t"} for n in failing_names],
        "files_with_problems_count": len(problems),
        "files_with_problems": [{"file": p, "rc": 1} for p in problems],
        "shard": shard,
        "shards": shards,
        "is_partial": shards > 1,
    }


def test_two_shards_merge_retains_both_shards_results():
    """THE CRITERION. Neither shard's rows may be lost, and totals must be the
    SUM -- not either shard's value, which is what reading one file gave."""
    a = shard_summary(1, 2, tests=2500, failures=16, errors=1, skipped=30,
                      files=["a.py", "b.py"], failing_names=["a.py"])
    b = shard_summary(2, 2, tests=560, failures=14, errors=0, skipped=20,
                      files=["c.py"], failing_names=["c.py"],
                      problems=["c.py"])

    m = R.merge_shard_summaries([a, b])

    assert m["totals"]["tests"] == 3060, m["totals"]
    assert m["totals"]["failures"] == 30, m["totals"]
    assert m["totals"]["errors"] == 1, m["totals"]
    assert m["totals"]["skipped"] == 50, m["totals"]

    # The specific regression: the merged number must not equal EITHER shard's.
    assert m["totals"]["tests"] != a["totals"]["tests"]
    assert m["totals"]["tests"] != b["totals"]["tests"]

    files = {r["file"] for r in m["per_file"]}
    assert files == {"a.py", "b.py", "c.py"}, files
    assert m["files_run"] == 3, m["files_run"]

    failing = {f["file"] for f in m["failures"]}
    assert failing == {"a.py", "c.py"}, failing
    assert m["failure_count"] == 2

    assert m["files_with_problems_count"] == 1
    assert m["shards_merged"] == [1, 2]
    assert m["shards_missing"] == []
    assert m["is_partial"] is False


def test_a_merge_missing_a_shard_declares_itself_partial():
    """Two of three is not a sweep either. Without this the overwrite defect
    simply moves up a level: a confident merged total that silently omits a
    third of the suite."""
    a = shard_summary(1, 3, tests=100, failures=1, errors=0, skipped=0,
                      files=["a.py"])
    c = shard_summary(3, 3, tests=100, failures=0, errors=0, skipped=0,
                      files=["c.py"])

    m = R.merge_shard_summaries([a, c])

    assert m["shards_merged"] == [1, 3]
    assert m["shards_missing"] == [2], m
    assert m["is_partial"] is True
    assert m["totals"]["tests"] == 200


def test_a_single_shard_summary_merges_to_itself_and_is_not_partial():
    """The unsharded case must stay honest: one shard of one is the whole run."""
    only = shard_summary(1, 1, tests=42, failures=2, errors=0, skipped=3,
                         files=["x.py"], failing_names=["x.py"])
    m = R.merge_shard_summaries([only])
    assert m["totals"]["tests"] == 42
    assert m["shards_missing"] == []
    assert m["is_partial"] is False


def test_merging_nothing_is_an_error_not_an_empty_green():
    """An empty merge must not produce a zero-failure summary -- that is the
    vacuous-oracle shape: a clean-looking result derived from no data."""
    with pytest.raises(ValueError):
        R.merge_shard_summaries([])


def test_the_runner_writes_a_shard_qualified_filename(tmp_path):
    """The collision is between identically-named artifacts, so the fix has to
    exist at the FILENAME, not only inside the JSON."""
    src = io.open(SCRIPT, encoding="utf-8").read()
    assert "ui-suite-summary.shard%dof%d.json" in src, (
        "a sharded run must write a name that cannot collide when the three "
        "artifacts are collected into one directory")


def test_merge_cli_reports_partial_and_exits_non_zero(tmp_path):
    """End to end through the CLI: a partial merge must FAIL, not print a
    confident total. Exercises the real entry point, not just the function."""
    src = tmp_path / "collected"
    src.mkdir()
    for shard in (1, 3):
        s = shard_summary(shard, 3, tests=10, failures=0, errors=0, skipped=0,
                          files=["f%d.py" % shard])
        io.open(src / ("ui-suite-summary.shard%dof3.json" % shard),
                "w", encoding="utf-8").write(json.dumps(s))

    out = tmp_path / "out"
    proc = subprocess.run(
        [sys.executable, str(SCRIPT),
         "--repo-root", str(REPO_ROOT),
         "--output-dir", str(out),
         "--pdfe-exe", "unused-in-merge-mode",
         "--merge-summaries", str(src)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    combined = (proc.stdout or "") + (proc.stderr or "")

    assert proc.returncode == 1, combined
    assert "PARTIAL" in combined, combined
    merged = json.loads(io.open(out / "ui-suite-summary-merged.json",
                                encoding="utf-8").read())
    assert merged["shards_missing"] == [2], merged
    assert merged["totals"]["tests"] == 20, merged


def test_merge_cli_succeeds_when_every_shard_is_present(tmp_path):
    """Control for the test above: prove the non-zero exit is caused by the
    MISSING shard and not by merge mode always failing."""
    src = tmp_path / "collected"
    src.mkdir()
    for shard in (1, 2, 3):
        s = shard_summary(shard, 3, tests=10, failures=0, errors=0, skipped=0,
                          files=["f%d.py" % shard])
        io.open(src / ("ui-suite-summary.shard%dof3.json" % shard),
                "w", encoding="utf-8").write(json.dumps(s))

    out = tmp_path / "out"
    proc = subprocess.run(
        [sys.executable, str(SCRIPT),
         "--repo-root", str(REPO_ROOT),
         "--output-dir", str(out),
         "--pdfe-exe", "unused-in-merge-mode",
         "--merge-summaries", str(src)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    combined = (proc.stdout or "") + (proc.stderr or "")

    assert proc.returncode == 0, combined
    assert "PARTIAL" not in combined, combined
    merged = json.loads(io.open(out / "ui-suite-summary-merged.json",
                                encoding="utf-8").read())
    assert merged["totals"]["tests"] == 30, merged
    assert merged["is_partial"] is False


def test_a_crashed_shard_carries_its_app_crash_count_through_the_merge():
    """GitHub #301, SECOND SITE.

    The per-shard summary gained `app_crash_count`. CI runs three shards and
    then merges, so a merge that did not carry the key would leave the run-wide
    summary without it -- and a `.get("app_crash_count", 0)` reader would see
    zero. That is the #278 defect repeated one level up: the shard knew and the
    merge did not.

    The count is derived from the concatenated `per_file` rows rather than
    summed from the shards' own counts, so this also pins that it cannot
    disagree with the rows a reader can see.
    """
    a = shard_summary(1, 2, tests=10, failures=0, errors=0, skipped=0,
                      files=["clean.py"])
    b = shard_summary(2, 2, tests=11, failures=0, errors=0, skipped=0,
                      files=["tests/ui_automation/test_final_phase8.py"])
    # The crash condition: pytest succeeded (rc 0, nothing failing) and the
    # APPLICATION faulted. Exactly the p96 phase-7 artifact.
    b["per_file"][0]["app_crashed"] = True
    b["per_file"][0]["unhealable_app_crash"] = True

    m = R.merge_shard_summaries([a, b])

    assert "app_crash_count" in m, (
        "the merged summary dropped app_crash_count; a sharded run -- which is "
        "how CI runs -- would report no crash at all: %r" % (sorted(m),))
    assert m["app_crash_count"] == 1, m["app_crash_count"]
    # And the clean-shard direction, so this is not an always-true assertion.
    clean = R.merge_shard_summaries([a, shard_summary(
        2, 2, tests=11, failures=0, errors=0, skipped=0, files=["ok.py"])])
    assert clean["app_crash_count"] == 0, clean["app_crash_count"]


def test_the_merge_also_replaces_the_unsuffixed_summary(tmp_path):
    """GitHub #278 names `ui-suite-summary.json`, so that file must aggregate.

    The first attempt at this wrote the aggregate to a NEW name
    (`ui-suite-summary-merged.json`) and left the unsuffixed file holding the
    last shard's numbers. The p97 phase-5 validator gate failed the phase for
    it, correctly: the criterion names a specific artifact, and that artifact
    is the one readers and uploaded CI artifacts actually reach for.

    Safe because of WHEN the merge runs -- after every shard has finished, so
    the in-shard gate that legitimately reads its own shard's unsuffixed
    summary has already read it. The shard-qualified files are left untouched
    for anyone who wants per-shard numbers.
    """
    import subprocess
    collected = tmp_path / "collected"
    collected.mkdir()
    a = shard_summary(1, 2, tests=10, failures=0, errors=0, skipped=0,
                      files=["a.py"])
    b = shard_summary(2, 2, tests=7, failures=1, errors=0, skipped=0,
                      files=["b.py"], failing_names=["b.py"])
    for s in (a, b):
        p = collected / ("ui-suite-summary.shard%dof2.json" % s["shard"])
        io.open(p, "w", encoding="utf-8").write(json.dumps(s))

    # A STALE last-shard summary is already sitting there, exactly as a real
    # sharded run leaves it. If the merge did not replace it, this test would
    # still find shard 2's numbers below.
    stale = collected / "ui-suite-summary.json"
    io.open(stale, "w", encoding="utf-8").write(json.dumps(b))

    r = subprocess.run(
        [sys.executable, str(SCRIPT), "--repo-root", str(REPO_ROOT),
         "--output-dir", str(collected), "--pdfe-exe", "unused",
         "--merge-summaries", str(collected)],
        capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-2000:]

    written = json.loads(io.open(stale, encoding="utf-8").read())
    assert written["files_run"] == 2, (
        "ui-suite-summary.json still holds %d file(s) -- the named artifact "
        "was not replaced with the aggregate, which is GitHub #278 unfixed: %r"
        % (written["files_run"], written))
    assert written["totals"]["tests"] == 17, written["totals"]
    assert written["shards_merged"] == [1, 2], written
    assert written["is_partial"] is False, written
    # ...and the per-shard evidence is still there for anyone who needs it.
    assert (collected / "ui-suite-summary.shard1of2.json").is_file()
    assert (collected / "ui-suite-summary.shard2of2.json").is_file()


def test_a_partial_merge_still_replaces_it_but_says_so(tmp_path):
    """A stale last-shard summary looks complete; a partial merge does not.

    `is_partial` and `shards_missing` travel INSIDE the document, so a reader
    cannot mistake it for a sweep. Leaving the stale file in place would be the
    worse of the two, because nothing in it announces that it describes one
    shard of three.
    """
    import subprocess
    collected = tmp_path / "collected"
    collected.mkdir()
    a = shard_summary(1, 3, tests=10, failures=0, errors=0, skipped=0,
                      files=["a.py"])
    io.open(collected / "ui-suite-summary.shard1of3.json", "w",
            encoding="utf-8").write(json.dumps(a))

    r = subprocess.run(
        [sys.executable, str(SCRIPT), "--repo-root", str(REPO_ROOT),
         "--output-dir", str(collected), "--pdfe-exe", "unused",
         "--merge-summaries", str(collected)],
        capture_output=True, text=True)
    assert r.returncode != 0, (
        "a partial merge must exit non-zero: " + (r.stdout + r.stderr)[-1500:])

    written = json.loads(
        io.open(collected / "ui-suite-summary.json", encoding="utf-8").read())
    assert written["is_partial"] is True, written
    assert written["shards_missing"] == [2, 3], written
