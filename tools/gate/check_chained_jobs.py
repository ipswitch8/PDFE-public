# -*- coding: utf-8 -*-
"""Guard the chained-jobs structure of windows-validation.yml.

WHY THIS EXISTS. The lane was one 720-minute job; it is now a DAG of six jobs so
a failed leg can be re-run alone and each leg gets its own timeout budget. That
split introduces four ways to silently lose the whole point of it, none of which
is visible in a green run:

  1. `steps.<id>.outcome` referenced from ANOTHER job. Actions does not error on
     this -- it evaluates to EMPTY, which reads as "not success", so the step
     silently SKIPS. Every downstream control skipping while the lane reports
     green is the F44/F46 masking class arriving through the refactor meant to
     help. This is the one that would actually happen.
  2. `actions/checkout` defaulting to `clean: true`, i.e. `git clean -ffdx`. The
     `-x` deletes gitignored files -- which is `build/`, the entire artifact the
     chaining exists to carry between jobs.
  3. A job with no `timeout-minutes`, which silently inherits the 6-hour default
     and re-creates the single-ceiling problem the split was for.
  4. The `report` job losing `if: always()`. It carries the F49 step-level
     unmasking, so without `always()` the control that reveals step-level red is
     itself skipped by the failure it exists to reveal.

Run:
    python tools/gate/check_chained_jobs.py [--self-test]
"""
from __future__ import annotations

import io
import os
import re
import sys

import yaml  # type: ignore

WORKFLOW = os.path.join(".github", "workflows", "windows-validation.yml")
REPORT_JOB = "report"


def check_doc(doc: dict) -> list[str]:
    problems: list[str] = []
    jobs = doc.get("jobs") or {}
    if len(jobs) < 2:
        problems.append(
            f"{WORKFLOW}: only {len(jobs)} job(s). The lane was restructured into "
            f"a chained DAG so a failed leg can be re-run alone; collapsing it "
            f"back to one job restores the single 720-minute ceiling.")
        return problems

    first = next(iter(jobs))
    for name, job in jobs.items():
        steps = job.get("steps") or []

        # (3) every job budgets its own time
        if not job.get("timeout-minutes"):
            problems.append(
                f"{WORKFLOW}: job {name!r} has no `timeout-minutes`; it inherits "
                f"the 6-hour default, which re-creates the single-ceiling "
                f"problem the split exists to remove.")

        # (2) follow-on jobs must not wipe build/
        for s in steps:
            if "actions/checkout" not in str(s.get("uses", "")):
                continue
            clean = (s.get("with") or {}).get("clean")
            if name != first and clean is not False:
                problems.append(
                    f"{WORKFLOW}: job {name!r} checks out with clean={clean!r}. "
                    f"The default runs `git clean -ffdx`, and -x deletes "
                    f"gitignored files -- i.e. the build/ tree this job depends "
                    f"on the build job having produced. Set `clean: false`.")

        # (1) THE dangerous one: cross-job step references
        local_ids = {s.get("id") for s in steps if s.get("id")}
        for s in steps:
            cond = str(s.get("if", ""))
            for sid in set(re.findall(r"steps\s*\.\s*([A-Za-z_][\w-]*)\s*\.\s*outcome",
                                      cond)):
                if sid not in local_ids:
                    problems.append(
                        f"{WORKFLOW}: job {name!r} step "
                        f"{str(s.get('name', ''))[:50]!r} references "
                        f"`steps.{sid}.outcome`, but no step in THIS job declares "
                        f"that id. Across a job boundary that evaluates to empty, "
                        f"reads as 'not success', and the step SILENTLY SKIPS. "
                        f"Use a job output (`needs.<job>.outputs.<name>`).")

    # (4) the reporting leg must survive an upstream failure
    if REPORT_JOB in jobs:
        cond = str(jobs[REPORT_JOB].get("if", ""))
        if "always()" not in cond:
            problems.append(
                f"{WORKFLOW}: job {REPORT_JOB!r} does not use `if: always()`. It "
                f"carries the F49 step-level unmasking, so without it the control "
                f"that reveals step-level red is skipped by the very failure it "
                f"exists to reveal.")
    return problems


def self_test() -> int:
    checks = 0
    failures: list[str] = []

    def check(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append(f"{label}: got {got!r}, want {want!r}")

    good = {"jobs": {
        "build": {"timeout-minutes": 180, "steps": [
            {"uses": "actions/checkout@v4"},
            {"name": "Build", "id": "build_debug",
             "if": "${{ !cancelled() }}"}]},
        "core-tests": {"timeout-minutes": 300, "needs": ["build"], "steps": [
            {"uses": "actions/checkout@v4", "with": {"clean": False}},
            {"name": "Test",
             "if": "${{ !cancelled() && needs.build.outputs.built == 'success' }}"}]},
        "report": {"timeout-minutes": 30, "if": "${{ always() }}", "steps": [
            {"uses": "actions/checkout@v4", "with": {"clean": False}},
            {"name": "F49"}]},
    }}
    check("a sound DAG passes", check_doc(good), [])

    import copy
    # THE defect: a consumer left on the step-level spelling across jobs.
    bad = copy.deepcopy(good)
    bad["jobs"]["core-tests"]["steps"][1]["if"] = \
        "${{ !cancelled() && steps.build_debug.outcome == 'success' }}"
    check("a cross-job steps.<id> reference is caught",
          any("SILENTLY SKIPS" in p for p in check_doc(bad)), True)

    bad2 = copy.deepcopy(good)
    bad2["jobs"]["core-tests"]["steps"][0]["with"] = {"clean": True}
    check("a follow-on job that cleans is caught",
          any("git clean -ffdx" in p for p in check_doc(bad2)), True)

    bad3 = copy.deepcopy(good)
    del bad3["jobs"]["core-tests"]["timeout-minutes"]
    check("a job with no timeout is caught",
          any("timeout-minutes" in p for p in check_doc(bad3)), True)

    bad4 = copy.deepcopy(good)
    bad4["jobs"]["report"]["if"] = "${{ success() }}"
    check("a report job without always() is caught",
          any("always()" in p for p in check_doc(bad4)), True)

    bad5 = {"jobs": {"only": good["jobs"]["build"]}}
    check("collapsing back to a single job is caught",
          any("chained DAG" in p for p in check_doc(bad5)), True)

    if failures:
        print("[chained-jobs] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1
    print(f"[chained-jobs] self-test: {checks} checks passed")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    if not os.path.isfile(WORKFLOW):
        print(f"[chained-jobs] {WORKFLOW} not found", file=sys.stderr)
        return 1
    with io.open(WORKFLOW, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh.read())
    problems = check_doc(doc)
    if problems:
        print("[chained-jobs] FAILED — the job DAG can silently lose coverage:")
        for p in problems:
            print("  * " + p)
        return 1
    n = len(doc.get("jobs") or {})
    # WORDING CORRECTION (multi-runner phase-2). This line used to end "every
    # follow-on job preserves build/", full stop. That was true of `clean:
    # false` on ONE machine and reads as a cross-machine guarantee it never
    # made: preservation only preserves what THIS machine built. The
    # cross-machine half is a different mechanism with its own guard, and a
    # reassurance that overstates its own scope is how a single-runner
    # assumption survives a review.
    print(f"[chained-jobs] {n} jobs: no cross-job step reference, every follow-on "
          f"job preserves build/ ON THE MACHINE THAT BUILT IT, every job budgets "
          f"its own time, report runs always(). Cross-MACHINE availability is "
          f"NOT checked here — see tools/gate/check_ensure_build_tree.py.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
