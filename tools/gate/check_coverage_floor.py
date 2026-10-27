#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_coverage_floor.py — enforce a line-coverage floor on a Cobertura report.

Remediation Phase 5, target R-32. The windows-validation.yml coverage step was
best-effort (``continue-on-error: true``): OpenCppCoverage produced a Cobertura
XML that was uploaded as an artifact but never gated, and a runner WITHOUT
OpenCppCoverage silently skipped coverage entirely. Either way a coverage
collapse could ship green. This script turns the report into a GATE:

  * the Cobertura XML MUST exist and parse (a missing/corrupt report is a HARD
    error — exit 2 — never a silent pass);
  * the overall line-rate MUST be >= ``--floor`` (default 0.60), else exit 1.

Cobertura encodes overall coverage as ``line-rate`` (0..1) on the root
``<coverage>`` element. When absent we derive it from the aggregate
``lines-covered`` / ``lines-valid`` attributes, and failing that from the
per-<line> ``hits`` across all classes — so the check is robust to the exact
OpenCppCoverage export shape.

WHERE THIS RUNS, AND WHAT THE PUSH LANE DOES *NOT* PROVE (GitHub #258).

Two different things are wired, and conflating them would overstate the gate:

  * THE REAL GATE runs in `windows-validation.yml` (line ~1673), against the
    Cobertura XML OpenCppCoverage produces there. That is the only place a real
    coverage number is checked against a real floor.
  * `--self-test` runs on every push from `fast-checks.yml`. It synthesises
    report shapes and drives all three exit codes through `main()`. It proves the
    GATE WORKS. It says nothing whatsoever about the project's coverage.

This file was previously exempted in `guard_reachability_baseline.json` as an
admitted gap, on the correct reasoning that pointing it at a nonexistent report
on every push would be red for a reason unrelated to coverage. The self-test is
how it earns a push-lane presence without that: no build, no coverage run, and a
failure means the gate itself is broken. The exemption was then removed because
the reachability ratchet fails on a baselined guard that has become reached --
a list that only ever grows reports "closed" about itself, not about the
property.

The residual gap is real and unchanged: **coverage is still not gated on an
automatic trigger**, because no automatic trigger produces a coverage artifact.
Do not read the removed exemption as that having been fixed.

Usage:
  python tools/gate/check_coverage_floor.py coverage-cobertura.xml --floor 0.60
  python tools/gate/check_coverage_floor.py --self-test
Exit 0 = at/above floor; 1 = below floor; 2 = missing/unparseable report.
"""
from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from typing import Optional, Tuple


def _f(el, name) -> Optional[float]:
    v = el.get(name)
    if v is None or v == "":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def line_rate(path: str) -> Tuple[Optional[float], str]:
    """Return (line_rate_0_to_1 or None, human message). None == unparseable."""
    try:
        root = ET.parse(path).getroot()
    except FileNotFoundError:
        return None, "coverage report not found: %s" % path
    except ET.ParseError as exc:
        return None, "unparseable coverage XML (%s): %s" % (path, exc)

    # 1. Direct root line-rate attribute (canonical Cobertura).
    lr = _f(root, "line-rate")
    if lr is not None:
        return lr, "root line-rate=%.4f" % lr

    # 2. Aggregate lines-covered / lines-valid on the root.
    covered = _f(root, "lines-covered")
    valid = _f(root, "lines-valid")
    if covered is not None and valid and valid > 0:
        return covered / valid, "lines-covered=%d lines-valid=%d" % (int(covered), int(valid))

    # 3. Derive from per-<line> hits across all classes (fallback).
    total = 0
    hit = 0
    for ln in root.iter("line"):
        total += 1
        h = _f(ln, "hits")
        if h is not None and h > 0:
            hit += 1
    if total > 0:
        return hit / total, "derived from <line> hits: %d/%d" % (hit, total)

    return None, "no line-rate / lines-valid / <line> data in %s" % path


def check(path: str, floor: float) -> Tuple[int, str]:
    lr, msg = line_rate(path)
    if lr is None:
        return 2, "HARD ERROR: %s" % msg
    pct = lr * 100.0
    if lr + 1e-9 < floor:
        return 1, "FAIL line-rate=%.2f%% < floor=%.2f%% (%s)" % (pct, floor * 100.0, msg)
    return 0, "PASS line-rate=%.2f%% >= floor=%.2f%% (%s)" % (pct, floor * 100.0, msg)


def self_test() -> int:
    """Prove all three exit codes, WITHOUT needing a real coverage run.

    GitHub #258. This guard is invoked only from windows-validation.yml, because
    it needs the Cobertura XML that OpenCppCoverage produces there. That made it
    the one of #258's three scripts with no way to be exercised locally at all:
    its companion test called `check()` directly, which proves the predicate
    computes but not that the PROCESS reports it. Those are different claims, and
    this corpus has an instance of the gap between them -- a watchdog that failed
    every heartbeat write and still exited 0.

    So the shapes are synthesised here. A floor gate is exactly the kind of check
    whose failure path nobody runs until the day it matters, and on that day the
    answer must not be "it printed FAIL and exited 0".
    """
    import tempfile, os, io as _io   # noqa: E401

    cases = [
        # (label, xml body, floor, expected exit)
        ("below floor via root line-rate",
         '<?xml version="1.0"?><coverage line-rate="0.42"></coverage>', 0.60, 1),
        ("at/above floor via root line-rate",
         '<?xml version="1.0"?><coverage line-rate="0.73"></coverage>', 0.60, 0),
        # Exactly AT the floor must PASS, not fail. An off-by-one here silently
        # moves the gate, and `>=` vs `>` is the easiest line in the file to get
        # wrong -- note check() carries a 1e-9 epsilon for precisely this.
        ("exactly at the floor",
         '<?xml version="1.0"?><coverage line-rate="0.60"></coverage>', 0.60, 0),
        ("derived from lines-covered/lines-valid",
         '<?xml version="1.0"?><coverage lines-covered="80" lines-valid="100">'
         '</coverage>', 0.60, 0),
        ("derived from lines-covered/lines-valid, below",
         '<?xml version="1.0"?><coverage lines-covered="10" lines-valid="100">'
         '</coverage>', 0.60, 1),
        # A report with no coverage data at all is a HARD error, never a pass.
        # This is the shape that let coverage collapse ship green before R-32:
        # a runner without OpenCppCoverage produced nothing and nothing complained.
        ("no coverage data anywhere",
         '<?xml version="1.0"?><coverage></coverage>', 0.60, 2),
        ("unparseable",
         '<coverage line-rate=', 0.60, 2),
    ]

    failures = []
    with tempfile.TemporaryDirectory() as td:
        for i, (label, body, floor, want) in enumerate(cases):
            p = os.path.join(td, "cov%d.xml" % i)
            with _io.open(p, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(body)
            got = main([p, "--floor", str(floor)])
            if got != want:
                failures.append("%s: expected exit %d, got %d" % (label, want, got))

        # And the missing-file path, which has no file to write.
        got = main([os.path.join(td, "definitely-absent.xml")])
        if got != 2:
            failures.append("missing report: expected exit 2, got %d" % got)

    if failures:
        for f in failures:
            print("[coverage-floor] SELF-TEST FAIL: " + f, file=sys.stderr)
        return 1
    print("[coverage-floor] self-test: %d case(s) passed, all three exit codes "
          "(0 pass / 1 below floor / 2 missing-or-unusable report) observed "
          "through main()" % (len(cases) + 1))
    return 0


def main(argv) -> int:
    ap = argparse.ArgumentParser(description="Enforce a Cobertura line-coverage floor.")
    ap.add_argument("xml", nargs="?", help="path to a Cobertura coverage XML")
    ap.add_argument("--floor", type=float, default=0.60,
                    help="minimum acceptable line-rate 0..1 (default 0.60)")
    ap.add_argument("--self-test", action="store_true",
                    help="synthesise Cobertura reports and prove every exit code")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.xml:
        ap.error("an XML path is required unless --self-test is given")
    code, msg = check(args.xml, args.floor)
    print("[coverage-floor] " + msg)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
