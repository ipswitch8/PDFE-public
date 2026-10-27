# -*- coding: utf-8 -*-
"""A baseline a test run can overwrite is not a baseline (GitHub #238).

`tests/test_ui_stress.py` used to write its measured wall-clock times straight
into `tests/docs/stress_perf_baseline.json`, the committed baseline. GitHub #238
records what that cost: the 2026-09-19 sweep replaced the committed 111.9 s for
`test_stress_1000_annotation_undo_redo` with 277.8 s, and had that been
committed, the 2.48x step would have disappeared into the file's own history and
the next real slowdown would have breached a budget nobody knew was already
92.6 % consumed.

It then happened a second time, on 2026-09-30, to the person investigating
#238 -- a run overwrote 111.913 with 318.182, and it was found only by reading
the diff before committing. A trap that catches the reader of its own warning is
a property of the code, not of the reader. Hence this guard rather than another
warning.

THE PROPERTY: a stress run writes its timings to an untracked run-local file.
The committed baseline changes only when a human asks for it, by setting
PDFE_STRESS_WRITE_BASELINE=1.

Source-level rather than behavioural, deliberately: the behavioural version
would have to run a live-UI stress scenario, which needs a desktop and five
minutes, so it could only ever live in the UI suite -- and the UI suite is
exactly the thing whose runs did the overwriting. This runs in fast-checks,
offline, in milliseconds, on the property that matters.
"""
from __future__ import annotations

import io
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
STRESS = REPO_ROOT / "tests" / "test_ui_stress.py"
BASELINE = REPO_ROOT / "tests" / "docs" / "stress_perf_baseline.json"

OPT_IN_ENV = "PDFE_STRESS_WRITE_BASELINE"


def _source() -> str:
    assert STRESS.is_file(), f"{STRESS} is missing; this guard has no subject"
    return io.open(STRESS, encoding="utf-8").read()


def unguarded_baseline_writes(src: str) -> list:
    """Lines that write PERF_BASELINE_PATH directly.

    A write through the indirection (`out_path`, chosen by
    `_perf_output_path()`) is fine; naming the committed baseline as the write
    target is not.
    """
    hits = []
    for n, line in enumerate(src.splitlines(), 1):
        if re.search(r"PERF_BASELINE_PATH\s*\.\s*(write_text|open|write_bytes)",
                     line):
            hits.append((n, line.strip()))
        if re.search(r"(json\.dump|write_text|write_bytes)\s*\([^)]*PERF_BASELINE_PATH",
                     line):
            hits.append((n, line.strip()))
    return hits


def test_no_code_path_writes_the_committed_baseline_directly():
    hits = unguarded_baseline_writes(_source())
    assert not hits, (
        "tests/test_ui_stress.py writes the COMMITTED perf baseline directly:\n"
        + "\n".join(f"  line {n}: {t}" for n, t in hits)
        + "\n\nGitHub #238: a baseline a test run can overwrite is not a "
          "baseline. Route the write through _perf_output_path(), which "
          f"returns the committed file only when {OPT_IN_ENV}=1."
    )


def test_the_predicate_detects_an_unguarded_write():
    """Non-vacuity. A pattern that matched nothing would pass forever."""
    synthetic = (
        "    payload = {}\n"
        "    PERF_BASELINE_PATH.write_text(json.dumps(payload))\n"
    )
    assert unguarded_baseline_writes(synthetic), (
        "the predicate did not flag a direct PERF_BASELINE_PATH.write_text, so "
        "its green above is evidence about the predicate, not about the tree"
    )
    also = "    json.dump(payload, PERF_BASELINE_PATH.open('w'))\n"
    assert unguarded_baseline_writes(also), (
        "the predicate missed the json.dump-into-the-baseline shape"
    )
    assert not unguarded_baseline_writes(
        "    out_path = _perf_output_path()\n"
        "    out_path.write_text(json.dumps(payload))\n"
    ), "a write through the indirection must NOT be flagged"


def test_the_opt_in_is_the_only_route_to_the_committed_file():
    src = _source()
    assert "_perf_output_path" in src, (
        "tests/test_ui_stress.py has no _perf_output_path(); the indirection "
        "this guard depends on is gone, so the guard is checking nothing"
    )
    # The env var must be consulted, and PERF_BASELINE_PATH must be returned
    # only from inside that function.
    m = re.search(r"def _perf_output_path\(\).*?(?=\ndef |\n@|\nclass )", src,
                  re.S)
    assert m, "could not isolate _perf_output_path()'s body"
    body = m.group()
    assert OPT_IN_ENV in body, (
        f"_perf_output_path() does not consult {OPT_IN_ENV}, so a run can "
        "still reach the committed baseline without anyone asking for it"
    )
    assert "PERF_BASELINE_PATH" in body, (
        "_perf_output_path() never returns the committed baseline, so the "
        "opt-in cannot work at all"
    )


def test_the_committed_baseline_still_holds_the_pre_regression_figure():
    """The 111.9 s figure is the evidence GitHub #238 rests on.

    If a run has overwritten it again, the comparison the issue is built on is
    gone and no amount of later measurement recovers it. Asserted by value
    because that is the thing at risk -- a structural check would pass on a
    file whose numbers had been replaced.
    """
    import json
    assert BASELINE.is_file(), f"{BASELINE} is missing"
    data = json.loads(io.open(BASELINE, encoding="utf-8").read())
    scen = data.get("scenarios", {}).get(
        "test_stress_1000_annotation_undo_redo")
    assert scen, ("the baseline has no entry for "
                  "test_stress_1000_annotation_undo_redo")
    assert scen.get("wall_clock_s_avg") == 111.913, (
        "the committed baseline no longer holds 111.913 s for "
        "test_stress_1000_annotation_undo_redo (found "
        f"{scen.get('wall_clock_s_avg')!r}). GitHub #238 compares against that "
        "figure; a run has overwritten it. Restore with:\n"
        "  git show HEAD:tests/docs/stress_perf_baseline.json "
        "> tests/docs/stress_perf_baseline.json\n"
        "and see #238 before replacing it deliberately -- the figure describes "
        "a run that created ONE annotation, so it must be re-derived from a "
        "run that does the work, which needs GitHub #269 fixed first."
    )
