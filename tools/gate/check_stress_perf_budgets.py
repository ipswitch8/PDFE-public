# -*- coding: utf-8 -*-
"""A stress scenario over its performance budget must FAIL something (#238).

WHAT WAS MEASURED, AND WHY THIS EXISTS
======================================
GitHub #238 reported `test_stress_1000_annotation_undo_redo` at **277.8 s
against a 300 s budget — 92.6%** — and warned that "one more slowdown of any
size breaches it".

Re-measured 2026-10-15 on an idle machine, one scenario alone:

    wall_clock_s_avg = 600.581   budget_s = 300.0     -> 200.2% of budget

So the breach already happened, and it is not a marginal one: the scenario is
**twice** its budget. The test still reported **PASS**.

It passed because nothing compares the two numbers. `tests/test_ui_stress.py`
writes `budget_s` and `wall_clock_s_avg` into its measurement JSON and never
asserts on them -- there is no comparison anywhere in that module. The only
reader of `budget_s` in the whole tree is
`tests/scripts/remediation_summary.py`, which computes an
`over_budget_scenarios` list into a summary dict; and nothing consumes THAT to
fail a build either. The budget was reporting-only, so a scenario at 200% of
budget was green, and #238's "one more slowdown breaches it" turned out to
understate the position: the slowdown had already landed and no mechanism
noticed.

A budget that nothing enforces is not a budget. That is the same shape as
#238's own observation about its baseline -- "a baseline file that a test run
can overwrite is not a baseline".

WHAT THIS DOES, AND THE RATCHET
===============================
Reads the measurement JSON a stress run produces and fails when a scenario
exceeds its budget. Scenarios already known to be over budget are listed in
`KNOWN_OVER_BUDGET` with their measured ratio and an issue number, so:

  * the EXISTING breach is recorded in code rather than sitting silently in an
    untracked JSON nobody reads;
  * a NEW breach, in any other scenario, fails immediately;
  * a known breach getting materially WORSE fails too -- the allowance is a
    ceiling, not an amnesty, which is what stops this becoming the
    re-baselining #238 nearly suffered;
  * a known breach that comes GOOD also fails, demanding the entry be removed.
    A list that only ever grows reports on its own contents instead of on the
    property.

Deliberately NOT done: turning the live breach into a red test today. #238 is
open, the cause is substantially harness overhead rather than product time
(measured: of the 326.7 s `create 1000` phase, 262.0 s is tool re-arm at
262 ms/iteration, and undo/redo are ~108 ms per operation through the #271
pipe seam), and a permanently-red test teaches people to ignore it. The breach
is recorded, bounded, and attributed instead.

Usage:
    python tools/gate/check_stress_perf_budgets.py [--measured PATH]
    python tools/gate/check_stress_perf_budgets.py --self-test
Exit 0 clean, 1 on a finding, 2 on a usage/input error.
"""
from __future__ import annotations

import argparse
import io
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
assert (REPO / ".github" / "workflows").is_dir(), (
    "repo root misresolved to %r" % (REPO,))

DEFAULT_MEASURED = REPO / "tests" / "_runtime" / "stress_perf_measured.json"
COMMITTED_BASELINE = REPO / "tests" / "docs" / "stress_perf_baseline.json"

#: scenario -> (allowed_ratio_ceiling, issue, note)
#:
#: `allowed_ratio_ceiling` is measured/budget. 1.0 means "must be within
#: budget". An entry above 1.0 is an ADMITTED, TRACKED breach with headroom
#: over the measurement that justified it -- enough that ordinary machine noise
#: does not flip it, and far too little to absorb another regression.
KNOWN_OVER_BUDGET = {
    "test_stress_1000_annotation_undo_redo": (
        2.20, 238,
        "measured 600.581 s against a 300 s budget = 200.2% on 2026-10-15, up "
        "from the 277.8 s / 92.6% this issue was filed on. Dominated by "
        "harness overhead: 262.0 ms per iteration of tool re-arm across the "
        "1000 creations (261.98 s of a 326.73 s phase), plus ~108 ms per "
        "undo and per redo through the #271 pipe seam. Ceiling set just above "
        "the measurement so noise does not flip it and a further regression "
        "does."),
}


def ratio(scenario: dict) -> float:
    budget = float(scenario.get("budget_s") or 0.0)
    measured = float(scenario.get("wall_clock_s_max")
                     or scenario.get("wall_clock_s_avg") or 0.0)
    if budget <= 0:
        return 0.0
    return measured / budget


def evaluate(scenarios: dict) -> list:
    """Return a list of problem strings. Pure, so it is unit-testable."""
    problems = []
    seen = set()

    for name, rec in sorted(scenarios.items()):
        budget = float(rec.get("budget_s") or 0.0)
        measured = float(rec.get("wall_clock_s_max")
                         or rec.get("wall_clock_s_avg") or 0.0)
        if budget <= 0:
            problems.append(
                "%s has no budget (budget_s=%r), so its runtime is "
                "unconstrained and this guard cannot speak for it"
                % (name, rec.get("budget_s")))
            continue
        if measured <= 0:
            problems.append(
                "%s has no measurement (wall_clock=%r); a scenario that "
                "recorded no time is not evidence of a fast scenario"
                % (name, measured))
            continue

        r = measured / budget
        allowed, issue, _note = KNOWN_OVER_BUDGET.get(name, (1.0, None, ""))
        seen.add(name)

        if r > allowed:
            if issue:
                problems.append(
                    "%s is at %.1f%% of budget (%.3f s / %.1f s), above its "
                    "admitted ceiling of %.0f%% for GitHub #%d. A known breach "
                    "is allowed to stay; it is not allowed to grow."
                    % (name, r * 100, measured, budget, allowed * 100, issue))
            else:
                problems.append(
                    "%s is OVER BUDGET at %.1f%% (%.3f s / %.1f s) and is not "
                    "an admitted breach. Either bring it under budget or add "
                    "it to KNOWN_OVER_BUDGET with an issue number."
                    % (name, r * 100, measured, budget))
        elif issue and r <= 1.0:
            problems.append(
                "%s is now WITHIN budget at %.1f%% (%.3f s / %.1f s) but is "
                "still listed in KNOWN_OVER_BUDGET for GitHub #%d. Remove the "
                "entry -- a list that only grows reports on itself rather than "
                "on the property." % (name, r * 100, measured, budget, issue))

    for name in sorted(set(KNOWN_OVER_BUDGET) - seen):
        problems.append(
            "%s is listed in KNOWN_OVER_BUDGET but was not measured in this "
            "run, so its allowance was neither used nor checked. Remove it if "
            "the scenario is gone." % name)

    return problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--measured", default=str(DEFAULT_MEASURED))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    path = pathlib.Path(args.measured)
    if not path.is_file():
        print("ERROR: no measurement at %s. A stress run writes it; this guard "
              "refuses to report 'clean' having examined nothing." % path,
              file=sys.stderr)
        return 2
    try:
        data = json.load(io.open(path, encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print("ERROR: cannot read %s: %s" % (path, exc), file=sys.stderr)
        return 2

    scenarios = data.get("scenarios") or {}
    if not scenarios:
        print("ERROR: %s records no scenarios; nothing was measured."
              % path, file=sys.stderr)
        return 2

    print("stress perf budgets: %d scenario(s) measured in %s"
          % (len(scenarios), path.name))
    for name, rec in sorted(scenarios.items()):
        r = ratio(rec)
        allowed = KNOWN_OVER_BUDGET.get(name, (1.0, None, ""))[0]
        print("  %-52s %7.3f s / %6.1f s = %6.1f%%  (ceiling %.0f%%)"
              % (name, float(rec.get("wall_clock_s_max")
                             or rec.get("wall_clock_s_avg") or 0.0),
                 float(rec.get("budget_s") or 0.0), r * 100, allowed * 100))

    problems = evaluate(scenarios)
    if problems:
        print("\nFINDINGS (%d):" % len(problems))
        for p in problems:
            print("  * %s" % p)
        return 1
    print("\nfindings: 0")
    return 0


def _self_test() -> int:
    cases = [
        ("within budget is clean",
         {"a": {"budget_s": 100.0, "wall_clock_s_max": 50.0}}, 0),
        ("over budget and unlisted is a finding",
         {"a": {"budget_s": 100.0, "wall_clock_s_max": 150.0}}, 1),
        ("exactly at budget is clean",
         {"a": {"budget_s": 100.0, "wall_clock_s_max": 100.0}}, 0),
        ("missing budget is a finding, not a pass",
         {"a": {"budget_s": 0.0, "wall_clock_s_max": 50.0}}, 1),
        ("missing measurement is a finding, not a fast scenario",
         {"a": {"budget_s": 100.0, "wall_clock_s_max": 0.0}}, 1),
        # The admitted breach, and its ceiling behaviour.
        ("admitted breach within its ceiling is clean",
         {"test_stress_1000_annotation_undo_redo":
          {"budget_s": 300.0, "wall_clock_s_max": 600.581}}, 0),
        ("admitted breach ABOVE its ceiling is a finding",
         {"test_stress_1000_annotation_undo_redo":
          {"budget_s": 300.0, "wall_clock_s_max": 700.0}}, 1),
        ("admitted breach that came GOOD must be delisted",
         {"test_stress_1000_annotation_undo_redo":
          {"budget_s": 300.0, "wall_clock_s_max": 250.0}}, 1),
        # wall_clock_s_max is preferred over avg: a single slow sample matters.
        ("max is used in preference to avg",
         {"a": {"budget_s": 100.0, "wall_clock_s_avg": 50.0,
                "wall_clock_s_max": 150.0}}, 1),
    ]
    failures = 0
    for label, scenarios, expected in cases:
        got = len(evaluate(scenarios))
        # Every case above also triggers the "listed but not measured" problem
        # for the admitted entry unless that entry is the one being measured.
        if "test_stress_1000_annotation_undo_redo" not in scenarios:
            got -= 1  # discount the unmeasured-allowance notice
        if got != expected:
            failures += 1
            print("FAIL %-52s expected %d finding(s), got %d"
                  % (label, expected, got))
        else:
            print("ok   %-52s %d" % (label, expected))

    # The unmeasured-allowance notice itself must exist.
    probs = evaluate({"a": {"budget_s": 100.0, "wall_clock_s_max": 10.0}})
    if not any("not measured in this run" in p for p in probs):
        failures += 1
        print("FAIL an unmeasured KNOWN_OVER_BUDGET entry must be reported")
    else:
        print("ok   an unmeasured allowance is reported")

    # Every admitted entry must cite an issue.
    for name, (allowed, issue, note) in KNOWN_OVER_BUDGET.items():
        if not issue or not note:
            failures += 1
            print("FAIL %s has no issue number or no note" % name)
        elif allowed <= 1.0:
            failures += 1
            print("FAIL %s is admitted with a ceiling of %.2f, which is not a "
                  "breach at all" % (name, allowed))
        else:
            print("ok   %s cites GitHub #%d with ceiling %.0f%%"
                  % (name, issue, allowed * 100))

    print("\nself-test: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
