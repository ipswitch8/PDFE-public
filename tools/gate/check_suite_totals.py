#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A live source for the gtest suite's size and skip SET (GitHub #276).

THE GAP THIS CLOSES
-------------------
Nothing in the repository recorded the suite's size or skip count in a form any
script reads, so a rising skip count -- or a shrinking test count -- could not be
caught on the run that caused it. A pipeline criterion of the form

    0 failures / 0 errors, with skips at or below the current 15-skip baseline

is satisfiable indefinitely by fourteen NEW skips appearing in a week, and the
number "15" itself traced only to a project note. #274's census found 635 latent
`GTEST_SKIP` sites in 227 real test sources, dominated by
`if (!fs::exists(<fixture>)) GTEST_SKIP()`; one renamed fixture quietly retires up
to 27 cases in a single file.

WHY THIS RECORDS THE SKIP *SET*, NOT A COUNT
--------------------------------------------
A count conflates two different events. Measured 2026-10-07: CI run 37366537472
reported 15 skipped and this workstation reports 19 for the same commit. Nothing
regressed -- four `CNNFontClassifierTest` cases skip here because the CNN model
file is not present, which is an environment difference, not drift.

A bare threshold cannot tell those apart, so it would either cry wolf on every
developer machine or be set loose enough to miss real drift. Recording the sorted
set of skipped test ids makes the question answerable: the checker reports which
ids APPEARED and which DISAPPEARED, and an environment difference shows up as a
named, explicable group rather than as a number that moved.

EXIT CODES
----------
  0  the suite matches the baseline, or only grew in test count
  1  drift: a skip appeared or vanished, or the suite shrank
  2  usage / IO error
"""
from __future__ import annotations

import argparse
import io
import json
import os
import sys
import xml.etree.ElementTree as ET
from typing import Dict, List, Optional, Tuple

try:  # pragma: no cover
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

DEFAULT_BASELINE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "suite_totals_baseline.json")


def read_xml(path: str) -> Tuple[Dict[str, int], List[str]]:
    """(totals, sorted skipped ids) from a merged gtest JUnit XML.

    The skipped ids are counted from the TESTCASE elements, not taken from the
    root's `skipped=` attribute, and the two are cross-checked by the caller. A
    root attribute that disagrees with the tree is exactly the kind of thing this
    file exists to notice -- GAP-A15 is the same lesson about exit codes.
    """
    root = ET.parse(path).getroot()
    totals = {}
    for key in ("tests", "failures", "errors", "skipped"):
        try:
            totals[key] = int(root.get(key) or 0)
        except ValueError:
            totals[key] = 0

    skipped: List[str] = []
    counted = 0
    for tc in root.iter("testcase"):
        counted += 1
        kinds = [c.tag for c in list(tc)]
        if ("skipped" in kinds or tc.get("status") == "notrun"
                or tc.get("result") == "skipped"):
            skipped.append("%s.%s" % (tc.get("classname") or "",
                                      tc.get("name") or ""))
    totals["testcase_elements"] = counted
    return totals, sorted(skipped)


def load_baseline(path: str) -> Optional[dict]:
    if not os.path.isfile(path):
        return None
    with io.open(path, encoding="utf-8") as fh:
        return json.load(fh)


def write_baseline(path: str, totals: Dict[str, int], skipped: List[str],
                   note: str) -> None:
    payload = {
        "_what": ("Live source for the GoogleTest suite's size and skip set "
                  "(GitHub #276). Updating this file is how a deliberate change "
                  "to either is recorded; check_suite_totals.py fails on any "
                  "drift that has not been recorded here."),
        "_why_a_set": ("A count cannot distinguish a new skip from a different "
                       "machine. CI reported 15 and a workstation 19 for the "
                       "same commit, the difference being four CNN-model tests "
                       "whose model file is absent locally."),
        "tests": totals["tests"],
        "skipped_count": len(skipped),
        "skipped": skipped,
        "note": note,
    }
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(payload, indent=2, ensure_ascii=False) + "\n")


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xml", required=True,
                    help="merged gtest JUnit XML (run_gtest.py's gtest_full.xml)")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--update", action="store_true",
                    help="rewrite the baseline from this run instead of checking")
    ap.add_argument("--note", default="",
                    help="recorded with --update, explaining the change")
    ap.add_argument("--allow-env-skips", default="",
                    help="comma-separated id prefixes whose skips are known to "
                         "depend on the environment; drift in these is reported "
                         "but does not fail. Use sparingly and say why.")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.xml):
        print("no such XML: %s" % args.xml, file=sys.stderr)
        return 2

    totals, skipped = read_xml(args.xml)

    # Vacuity guard: a root attribute that disagrees with the tree means one of
    # the two is lying, and reporting either on its own would be a confident
    # wrong number.
    if totals["skipped"] != len(skipped):
        print("MISMATCH: root says skipped=%d but %d testcase element(s) are "
              "marked skipped. Refusing to report either as the truth."
              % (totals["skipped"], len(skipped)), file=sys.stderr)
        return 1

    print("suite totals (live)")
    print("  tests    : %d" % totals["tests"])
    print("  failures : %d" % totals["failures"])
    print("  errors   : %d" % totals["errors"])
    print("  skipped  : %d" % len(skipped))

    if args.update:
        write_baseline(args.baseline, totals, skipped,
                       args.note or "recorded from %s" % os.path.basename(args.xml))
        print("baseline updated: %s" % args.baseline)
        return 0

    base = load_baseline(args.baseline)
    if base is None:
        print("no baseline at %s -- create one with --update" % args.baseline,
              file=sys.stderr)
        return 2

    base_skipped = set(base.get("skipped") or [])
    now_skipped = set(skipped)
    appeared = sorted(now_skipped - base_skipped)
    vanished = sorted(base_skipped - now_skipped)

    env_prefixes = [p.strip() for p in (args.allow_env_skips or "").split(",")
                    if p.strip()]

    def is_env(tid: str) -> bool:
        return any(tid.startswith(p) for p in env_prefixes)

    hard_appeared = [t for t in appeared if not is_env(t)]
    hard_vanished = [t for t in vanished if not is_env(t)]

    ok = True
    if totals["tests"] < int(base.get("tests") or 0):
        print("SUITE SHRANK: %d tests now, baseline %d. A test that stops being "
              "registered is invisible to a pass/fail count."
              % (totals["tests"], base["tests"]), file=sys.stderr)
        ok = False
    elif totals["tests"] > int(base.get("tests") or 0):
        print("  (suite grew by %d test(s) since the baseline -- not a failure)"
              % (totals["tests"] - int(base["tests"])))

    for tid in appeared:
        print("  SKIP APPEARED%s: %s" % (" [env]" if is_env(tid) else "", tid))
    for tid in vanished:
        print("  skip vanished%s: %s" % (" [env]" if is_env(tid) else "", tid))

    if hard_appeared:
        print("FAIL: %d test(s) started skipping and the baseline does not "
              "record them. A skip is counted as a non-failure by every "
              "consumer, so this is how coverage leaves silently. Fix the test, "
              "or re-record with --update and say why in --note."
              % len(hard_appeared), file=sys.stderr)
        ok = False
    if hard_vanished:
        print("FAIL: %d baseline skip(s) no longer skip. That is usually good "
              "news, but the baseline must say so -- re-record with --update."
              % len(hard_vanished), file=sys.stderr)
        ok = False

    if ok:
        print("OK -- suite size and skip set match the baseline")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
