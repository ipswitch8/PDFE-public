#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_perf_floor.py -- performance-regression floor gate for PDFE (C-07).

The perf tests (test_performance_stress.cpp / test_search_performance.cpp /
test_xfa_save_benchmark.cpp) MEASURE hot-path timings but enforce no floor: a
regression is timed and printed, never asserted. The XFA-save 317s-class
regression is exactly the failure mode this leaves unguarded.

This script turns a set of measured hot-path timings into a GATE, modelled on
the Phase-5 mutation-kill floor (tools/gate/mutation_kill.py): a committed
baseline file records a per-metric baseline (in ms) and a regression
threshold; a measured run is compared against it; the script EXITS NON-ZERO
when any metric regresses beyond ``baseline * threshold``.

  * baselines JSON (committed, tools/gate/perf_baselines.json):
    {
      "threshold_multiplier": 3.0,          # default per-metric multiplier
      "metrics": {
        "document_open_ms": {"baseline_ms": 12.0},
        "save_routing_ms":  {"baseline_ms": 30.0, "threshold_multiplier": 2.5},
        ...
      }
    }
  * measured JSON (emitted by the PerfBenchmark gtest -> perf-measured.json):
    { "document_open_ms": 11.4, "save_routing_ms": 28.9, ... }

A metric FAILS iff measured_ms > baseline_ms * threshold_multiplier (the
per-metric multiplier overrides the file-level default). A metric present in
the baseline but ABSENT from the measured run is a HARD error (exit 2) -- a
silently-dropped benchmark must never pass. Missing/unparseable files are a
HARD error (exit 2), never a silent pass.

MACHINE-VARIANCE NOTE (honest limitation): absolute wall-clock varies by
machine, background load, Debug-vs-Release, and CPU thermal state. This gate
is therefore RELATIVE to a SAME-MACHINE baseline and uses a GENEROUS default
threshold (3x) so ordinary run-to-run noise (typically <2x on a dev box /
self-hosted runner) does not trip it -- only a real, large hot-path regression
(the 10x-100x XFA-save class) does. The baseline MUST be regenerated on the
machine that runs the gate; a baseline captured on a faster box will produce
false regressions on a slower one. See docs/coverage-expansion/phase-4-report.md.

Usage:
  python tools/gate/check_perf_floor.py \
      --baselines tools/gate/perf_baselines.json \
      --measured  perf-measured.json \
      [--json-out perf-gate-report.json]

Exit 0 = all metrics within threshold; 1 = one or more regressions;
2 = missing/unparseable input or a baseline metric absent from the measured run.
"""
from __future__ import annotations

import argparse
import json
import sys


def _load(path):
    """Return (obj, error_message). obj is None on failure."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh), ""
    except FileNotFoundError:
        return None, "file not found: %s" % path
    except (OSError, ValueError) as exc:
        return None, "unreadable/unparseable JSON (%s): %s" % (path, exc)


def evaluate(baselines, measured):
    """Compare measured timings against baselines.

    Returns (code, lines, results):
      code    = 0 pass / 1 regression / 2 hard error
      lines   = human-readable per-metric report lines
      results = list of per-metric dicts (for the JSON report artifact)
    """
    default_mult = float(baselines.get("threshold_multiplier", 3.0))
    metrics = baselines.get("metrics", {})
    if not metrics:
        return 2, ["HARD ERROR: baselines file declares no 'metrics'"], []

    lines = []
    results = []
    regressed = False
    hard_error = False

    for name in sorted(metrics):
        spec = metrics[name] or {}
        base = spec.get("baseline_ms")
        mult = float(spec.get("threshold_multiplier", default_mult))
        if base is None:
            lines.append("  %-24s HARD ERROR: no baseline_ms" % name)
            hard_error = True
            results.append({"metric": name, "status": "no_baseline"})
            continue
        base = float(base)
        limit = base * mult

        if name not in measured:
            lines.append("  %-24s HARD ERROR: absent from measured run "
                         "(baseline=%.2fms)" % (name, base))
            hard_error = True
            results.append({"metric": name, "status": "missing_measurement",
                            "baseline_ms": base})
            continue

        meas = float(measured[name])
        ok = meas <= limit
        status = "PASS" if ok else "REGRESSED"
        ratio = (meas / base) if base > 0 else float("inf")
        lines.append(
            "  %-24s %-9s measured=%.2fms baseline=%.2fms "
            "limit=%.2fms (%.2fx of baseline, threshold %.1fx)"
            % (name, status, meas, base, limit, ratio, mult))
        if not ok:
            regressed = True
        results.append({"metric": name, "status": status.lower(),
                        "measured_ms": meas, "baseline_ms": base,
                        "limit_ms": limit, "ratio": ratio,
                        "threshold_multiplier": mult})

    if hard_error:
        return 2, lines, results
    if regressed:
        return 1, lines, results
    return 0, lines, results


def main(argv):
    ap = argparse.ArgumentParser(
        description="Performance-regression floor gate for PDFE (C-07).")
    ap.add_argument("--baselines", required=True,
                    help="committed per-metric baseline JSON")
    ap.add_argument("--measured", required=True,
                    help="measured-run JSON emitted by the PerfBenchmark gtest")
    ap.add_argument("--json-out", default="",
                    help="optional path for a machine-readable gate report")
    args = ap.parse_args(argv)

    baselines, err_b = _load(args.baselines)
    if baselines is None:
        print("[perf-floor] HARD ERROR: baselines %s" % err_b)
        return 2
    measured, err_m = _load(args.measured)
    if measured is None:
        print("[perf-floor] HARD ERROR: measured %s" % err_m)
        return 2

    code, lines, results = evaluate(baselines, measured)
    print("[perf-floor] baselines=%s measured=%s" % (args.baselines, args.measured))
    for ln in lines:
        print(ln)
    verdict = {0: "PASS (all metrics within threshold)",
               1: "FAIL (one or more regressions beyond threshold)",
               2: "HARD ERROR (missing metric / bad input)"}[code]
    print("[perf-floor] %s" % verdict)

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8", newline="") as fh:
            json.dump({"exit_code": code, "verdict": verdict,
                       "results": results}, fh, indent=2)

    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
