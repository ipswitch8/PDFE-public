#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""The render suite's JUnit artifact must carry its tests (GitHub #141).

THE DEFECT THIS CLOSES
----------------------
`tests/docs/known_issues.md` §CI-LIMITATION-RENDER-JUNIT-EMPTY-ARTIFACT records
a windows-validation run whose render step went GREEN and whose
`render-suite.log` showed `46 passed, 2 xfailed, 2 xpassed`, while the uploaded
`render-junit.xml` -- the artifact gates and reviewers actually parse --
contained zero testcases. This project's own standing practice is "parse the
JUnit XML, never trust the exit code" (GAP-A15 / gtest_exit_code_masking), so
an empty artifact makes every render test invisible to every consumer that
follows the rule, and is indistinguishable from a suite that never ran.

WHAT WAS MEASURED, AND WHY THE ISSUE STAYED OPEN
------------------------------------------------
p82 phase-9 measured every link and refuted the suspected cause: pytest's
--junitxml does populate the file (62 testcases), scrub_artifacts.py preserves
them, and check_junit_xml.py sums the child <testsuite> correctly. Re-measured
independently on 2026-09-14 with the workflow's exact command shape: 62
testcases across all 13 test_render_*.py files.

But the original cited run (28615963046) is HTTP 404 and past retention, so the
observation could never be re-examined -- three positive measurements against
one unverifiable negative is not grounds to call a signal-integrity row stale.
Nothing in the workflow would have NOTICED an empty artifact either way: the
step's own exit code was the only evidence, which is precisely the practice the
project rejects.

So the resolution is not another measurement. It is a check that makes the
artifact assert its own contents, so the symptom cannot recur unobserved.

THE GENERALISABLE HALF
----------------------
pytest writes the counts on the CHILD <testsuite> and leaves the root
<testsuites> bare::

    <testsuites name="pytest tests">
      <testsuite name="pytest" tests="62" failures="40" ...>

A consumer -- or a human -- reading `testsuites/@tests` sees nothing, which is
exactly what "contains 0 testcases" looks like. PDFE's own tools sum the
children and are fine; an external tool or a reviewer eyeballing the root is
not. `--backfill-root` writes the aggregate onto the root element so both
readings agree, which removes the misreading rather than documenting it.

Usage:
    python tools/gate/check_render_junit.py render-junit.xml --tests-dir tests/ui_automation
    python tools/gate/check_render_junit.py render-junit.xml --backfill-root
    python tools/gate/check_render_junit.py --self-test

Exit 0 = the artifact carries its tests, 1 = it does not, 2 = usage error.
"""
from __future__ import annotations

import argparse
import glob
import io
import os
import sys
import tempfile
import xml.etree.ElementTree as ET

#: The classname pytest gives a test in tests/ui_automation/test_render_x.py is
#: "tests.ui_automation.test_render_x".
RENDER_GLOB = "test_render_*.py"

_COUNT_ATTRS = ("tests", "failures", "errors", "skipped")


def testcases(root):
    return root.findall(".//testcase")


def classnames(root):
    """Distinct module classnames present in the artifact."""
    return sorted({c.get("classname", "") for c in testcases(root) if c.get("classname")})


def expected_modules(tests_dir):
    """Module classnames the render suite SHOULD have produced, from disk.

    Derived from the tree, not from a hardcoded number: the issue was filed
    when there were 48 render tests and there are now 62, and a check pinned to
    a stale constant would either have to be edited on every addition or would
    quietly stop meaning anything.
    """
    out = []
    for path in sorted(glob.glob(os.path.join(tests_dir, RENDER_GLOB))):
        stem = os.path.splitext(os.path.basename(path))[0]
        out.append(stem)
    return out


def aggregate(root):
    """Sum the count attributes across child <testsuite> elements."""
    totals = {k: 0 for k in _COUNT_ATTRS}
    time_total = 0.0
    for ts in root.findall("testsuite"):
        for k in _COUNT_ATTRS:
            try:
                totals[k] += int(ts.get(k, "0") or 0)
            except ValueError:
                pass
        try:
            time_total += float(ts.get("time", "0") or 0)
        except ValueError:
            pass
    return totals, time_total


def backfill_root(tree, path):
    """Write the child aggregate onto the root element. Returns what was written."""
    root = tree.getroot()
    totals, time_total = aggregate(root)
    # If pytest wrote no child <testsuite> at all, fall back to counting the
    # elements themselves -- the root must never end up claiming zero while
    # testcases are present, which is the exact misreading this exists to end.
    if totals["tests"] == 0:
        totals["tests"] = len(testcases(root))
    for k in _COUNT_ATTRS:
        root.set(k, str(totals[k]))
    root.set("time", "%.3f" % time_total)
    tree.write(path, encoding="utf-8", xml_declaration=True)
    return totals


def check(path, tests_dir=None, backfill=False, out=sys.stdout):
    """Returns (ok, messages)."""
    msgs = []
    if not os.path.exists(path):
        return False, ["%s does not exist. The render step produced no JUnit "
                       "artifact at all, so nothing downstream can parse it."
                       % path]
    try:
        tree = ET.parse(path)
    except ET.ParseError as exc:
        return False, ["%s is not parseable XML: %s" % (path, exc)]

    root = tree.getroot()
    cases = testcases(root)
    msgs.append("%s: %d <testcase> element(s)" % (path, len(cases)))

    ok = True
    if not cases:
        ok = False
        msgs.append(
            "FAIL: ZERO testcases. This is GitHub #141's symptom exactly. The "
            "render step's exit code is NOT evidence the tests ran -- this "
            "project's standing rule is to parse the XML -- so an empty "
            "artifact means every render test is invisible to every consumer "
            "that follows the rule.")

    present = classnames(root)
    msgs.append("distinct test modules present: %d" % len(present))

    if tests_dir:
        want = expected_modules(tests_dir)
        msgs.append("test_render_*.py on disk        : %d" % len(want))
        missing = [m for m in want
                   if not any(cn.endswith("." + m) or cn == m for cn in present)]
        if missing:
            ok = False
            msgs.append(
                "FAIL: %d render module(s) on disk contributed NO testcase to "
                "the artifact: %s. A partial artifact is the same defect as an "
                "empty one for the modules it omits -- they read as 'did not "
                "run' and nothing else would say otherwise."
                % (len(missing), ", ".join(missing)))
        if not want:
            ok = False
            msgs.append(
                "FAIL: no %s found under %s. This check would then pass "
                "vacuously against an empty expectation, so it fails instead."
                % (RENDER_GLOB, tests_dir))

    totals, _t = aggregate(root)
    if root.get("tests") is None:
        msgs.append(
            "NOTE: the root <testsuites> carries no counts (pytest puts them on "
            "the child <testsuite>). A consumer reading testsuites/@tests sees "
            "nothing, which is what '0 testcases' looked like in #141.")
    if backfill:
        written = backfill_root(tree, path)
        msgs.append("backfilled root <testsuites> with %s" % written)

    for m in msgs:
        print(m, file=out)
    return ok, msgs


# --------------------------------------------------------------------------
# self-test -- the check must be able to go red AND to stay green
# --------------------------------------------------------------------------

_PYTEST_SHAPE = """<?xml version="1.0" encoding="utf-8"?>
<testsuites name="pytest tests"><testsuite name="pytest" errors="0" failures="0" skipped="0" tests="2" time="1.5">
<testcase classname="tests.ui_automation.test_render_alpha" name="t1" time="0.5"/>
<testcase classname="tests.ui_automation.test_render_beta" name="t2" time="1.0"/>
</testsuite></testsuites>
"""

_EMPTY_SHAPE = """<?xml version="1.0" encoding="utf-8"?>
<testsuites name="pytest tests"><testsuite name="pytest" errors="0" failures="0" skipped="0" tests="0" time="0.0">
</testsuite></testsuites>
"""


def self_test():
    failures = 0
    devnull = io.StringIO()

    def check_case(label, got, want):
        nonlocal failures
        if got != want:
            failures += 1
        print("  [%s] %-52s got=%r" % ("ok  " if got == want else "FAIL", label, got))

    tmp = tempfile.mkdtemp(prefix="render_junit_selftest_")
    tests_dir = os.path.join(tmp, "ui_automation")
    os.makedirs(tests_dir)
    for name in ("test_render_alpha.py", "test_render_beta.py"):
        with io.open(os.path.join(tests_dir, name), "w", encoding="utf-8") as fh:
            fh.write("# fixture\n")

    populated = os.path.join(tmp, "populated.xml")
    with io.open(populated, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_PYTEST_SHAPE)
    empty = os.path.join(tmp, "empty.xml")
    with io.open(empty, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_EMPTY_SHAPE)

    ok, _ = check(populated, tests_dir, out=devnull)
    check_case("a populated artifact PASSES", ok, True)

    ok, msgs = check(empty, tests_dir, out=devnull)
    check_case("an EMPTY artifact FAILS", ok, False)
    check_case("and the failure says ZERO testcases",
               any("ZERO testcases" in m for m in msgs), True)

    # ISOLATE THE ZERO-TESTCASE RULE. Caught by mutation: with --tests-dir
    # supplied, an empty artifact also trips the missing-module rule, so
    # deleting the zero-testcase rule outright left "an EMPTY artifact FAILS"
    # still green. Two rules sharing one verdict means neither is pinned. This
    # case removes the other rule from the picture.
    ok, msgs = check(empty, None, out=devnull)
    check_case("an EMPTY artifact FAILS on its own, with no module expectation",
               ok, False)
    ok, msgs = check(populated, None, out=devnull)
    check_case("a populated artifact PASSES with no module expectation",
               ok, True)

    ok, msgs = check(os.path.join(tmp, "absent.xml"), tests_dir, out=devnull)
    check_case("a MISSING artifact FAILS", ok, False)

    # A partial artifact: beta ran, alpha did not. Reads as "alpha did not run"
    # to every consumer, which is #141 scoped to one module.
    partial = os.path.join(tmp, "partial.xml")
    with io.open(partial, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_PYTEST_SHAPE.replace(
            '<testcase classname="tests.ui_automation.test_render_alpha" name="t1" time="0.5"/>\n', ""))
    ok, msgs = check(partial, tests_dir, out=devnull)
    check_case("a PARTIAL artifact FAILS", ok, False)
    check_case("and it names the missing module",
               any("test_render_alpha" in m for m in msgs), True)

    # No render tests on disk: the module check would otherwise pass against an
    # empty expectation.
    bare = os.path.join(tmp, "bare")
    os.makedirs(bare)
    ok, msgs = check(populated, bare, out=devnull)
    check_case("an EMPTY EXPECTATION fails rather than passing vacuously",
               ok, False)

    # Backfill, both directions.
    root = ET.parse(populated).getroot()
    check_case("root carries no counts before backfill", root.get("tests"), None)
    check(populated, tests_dir, backfill=True, out=devnull)
    root = ET.parse(populated).getroot()
    check_case("root carries the aggregate after backfill", root.get("tests"), "2")
    check_case("and the testcases survived the rewrite",
               len(testcases(root)), 2)

    # ------------------------------------------------------------------
    # THE GUARD MUST STILL BE INVOKED (2026-09-18, GitHub #141).
    #
    # Everything above proves the check CAN redden. None of it proves it is
    # wired to anything. This checker's only real invocation is one line inside
    # the render step of windows-validation.yml -- a dispatch-only workflow on
    # an often-offline runner -- so deleting that line would remove the guard
    # with nothing going red anywhere, and `--self-test` would keep passing in
    # the fast-checks lane while guarding an artifact nobody validates. That is
    # the "never invoked" shape of an inert guard, and it is the one this
    # project keeps re-finding. Pin the call site.
    # ------------------------------------------------------------------
    wf = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), ".github", "workflows",
        "windows-validation.yml")
    if not os.path.exists(wf):
        check_case("windows-validation.yml is where this expects it", False, True)
    else:
        with io.open(wf, encoding="utf-8", errors="replace") as fh:
            wf_text = fh.read().replace("\r\n", "\n")
        check_case("the render step still INVOKES this checker",
                   "check_render_junit.py" in wf_text, True)
        check_case("...with --tests-dir, so a missing module is caught",
                   "--tests-dir" in wf_text, True)
        check_case("...and --backfill-root, so the root stops reading as zero",
                   "--backfill-root" in wf_text, True)
        # A non-zero exit must actually fail the step. `python foo.py` inside a
        # pwsh block does NOT propagate its exit code on its own.
        check_case("...and its non-zero exit is acted on",
                   "$LASTEXITCODE -ne 0" in wf_text, True)

    print("self-test: %s" % ("PASS" if failures == 0 else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("artifact", nargs="?", help="path to render-junit.xml")
    ap.add_argument("--tests-dir", default="",
                    help="directory holding test_render_*.py; when given, every "
                         "module on disk must have contributed a testcase")
    ap.add_argument("--backfill-root", action="store_true",
                    help="write the child aggregate onto the root <testsuites>")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.artifact:
        ap.error("an artifact path is required (or --self-test)")

    ok, _ = check(args.artifact, args.tests_dir or None, args.backfill_root)
    if not ok:
        print("check_render_junit: FAIL", file=sys.stderr)
        return 1
    print("check_render_junit: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
