#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_junit_xml.py -- machine-check a GoogleTest JUnit XML report.

Single source of truth for "did the suite actually pass", shared by:
  * the repo-local gate hook (.claude/hooks/phase-gate.sh)  [conceptually --
    the hook embeds the same rules for zero-dependency execution], and
  * CI workflows (.github/workflows/*.yml), which MUST NOT trust the process
    exit code -- pdfe_tests.exe can exit 0 while the XML records failures
    (GAP-A15). This script parses the XML and is the CI pass/fail authority.

A report PASSES only when ALL of:
  * the file exists and parses,
  * sum(failures) == 0 across all <testsuite> elements,
  * sum(errors)   == 0,
  * tests > 0 (an empty run is not a pass),
  * at least one <testcase> ran (status="run"),
  * if --require-test NAME is given, that test is present AND ran
    (matches bare name or "ClassName.name"),
  * if --min-tests N is given, tests >= N (guards silent suite shrinkage).

Exit 0 = pass, 1 = fail (assertion(s) not met), 2 = usage/parse error.
Prints a one-line human summary to stdout either way.
"""
import argparse
import sys
import xml.etree.ElementTree as ET


def _int(el, name):
    try:
        return int((el.get(name) or "0"))
    except (TypeError, ValueError):
        return 0


def _is_run(tc):
    # GoogleTest emits status="run" for executed cases; some versions omit
    # the attribute entirely for run cases -> treat missing as run.
    return tc.get("status", "run") == "run"


def check(path, require_test="", min_tests=0):
    try:
        root = ET.parse(path).getroot()
    except FileNotFoundError:
        return False, "report not found: %s" % path
    except ET.ParseError as exc:
        return False, "unparseable XML (%s): %s" % (path, exc)

    suites = list(root.iter("testsuite"))
    cases = list(root.iter("testcase"))
    failures = sum(_int(ts, "failures") for ts in suites)
    errors = sum(_int(ts, "errors") for ts in suites)
    tests = sum(_int(ts, "tests") for ts in suites)
    if tests == 0:  # testsuites-only attrs (single flat report)
        failures += _int(root, "failures")
        errors += _int(root, "errors")
        tests += _int(root, "tests")

    run_cases = [tc for tc in cases if _is_run(tc)]

    problems = []
    if failures != 0:
        problems.append("failures=%d" % failures)
    if errors != 0:
        problems.append("errors=%d" % errors)
    if tests <= 0:
        problems.append("tests=%d (empty run)" % tests)
    if not run_cases:
        problems.append("no testcase ran (status!=run)")
    if min_tests and tests < min_tests:
        problems.append("tests=%d < required min %d" % (tests, min_tests))
    if require_test:
        matches = [tc for tc in cases
                   if tc.get("name") == require_test
                   or (tc.get("classname", "") + "." + tc.get("name", "")) == require_test]
        if not any(_is_run(tc) for tc in matches):
            problems.append("required test %r absent or not run" % require_test)

    summary = "tests=%d failures=%d errors=%d run=%d" % (
        tests, failures, errors, len(run_cases))
    if problems:
        return False, "FAIL " + summary + " :: " + "; ".join(problems)
    return True, "PASS " + summary


def main(argv):
    ap = argparse.ArgumentParser(description="Machine-check a GoogleTest JUnit XML report.")
    ap.add_argument("xml", help="path to gtest --gtest_output=xml report")
    ap.add_argument("--require-test", default="", help="test name that must be present and run")
    ap.add_argument("--min-tests", type=int, default=0, help="minimum total tests expected")
    args = ap.parse_args(argv)
    ok, msg = check(args.xml, args.require_test, args.min_tests)
    print(msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
