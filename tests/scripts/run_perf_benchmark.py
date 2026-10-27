# -*- coding: utf-8 -*-
"""Run the PerfBenchmark gtest suite and emit the measured-timings JSON.

GitHub #258. `tools/gate/check_perf_floor.py` compares measured hot-path
timings against a committed baseline, and was reachable only from a
`workflow_dispatch` workflow because nothing else produced its input. A floor
compared against a file that does not exist is either a hard error on every run
or a step structurally unable to fail; neither is a guard.

This produces that input, so the floor can be wired into the local fast-checks
mirror and actually mean something. It is deliberately a separate, tiny script
rather than logic inside the mirror: the mirror runs COMMANDS, and a command is
something a person can also run by hand when the floor reddens and they want to
see the timings.

Exit codes: 0 = the JSON was produced, 1 = the suite failed or produced nothing.
A missing test binary is exit 1 and NOT a pass -- an input that could not be
produced is not evidence of a passing check.

    python tests/scripts/run_perf_benchmark.py [--exe PATH] [--out PATH]
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

REPO = Path(__file__).resolve().parents[2]
DEFAULT_EXE = REPO / "build" / "vs2022" / "bin" / "Debug" / "pdfe_tests.exe"
DEFAULT_OUT = REPO / "tests" / "_runtime" / "perf-measured.json"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(DEFAULT_EXE),
                    help="gtest binary (default: the Debug build)")
    ap.add_argument("--out", default=str(DEFAULT_OUT),
                    help="where to write perf-measured.json")
    args = ap.parse_args()

    exe = Path(args.exe)
    out = Path(args.out)

    if not exe.exists():
        sys.stderr.write(
            f"run_perf_benchmark: no test binary at {exe}\n"
            f"  Build it first:  cmake --build build/vs2022 "
            f"--target pdfe_tests --config Debug\n"
            f"  Reporting FAILURE rather than success: a floor that never ran "
            f"is not a floor that passed.\n")
        return 1

    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        out.unlink()
    except OSError:
        pass

    env = dict(os.environ)
    # The suite honours this; without it the JSON lands in a temp directory the
    # checker would have to guess at.
    env["PDFE_PERF_JSON"] = str(out)

    print(f"[perf-benchmark] {exe.name} --gtest_filter=PerfBenchmark.*")
    r = subprocess.run(
        [str(exe), "--gtest_filter=PerfBenchmark.*"],
        cwd=str(REPO), env=env, capture_output=True, text=True,
        encoding="utf-8", errors="replace")
    tail = (r.stdout or "").strip().splitlines()[-3:]
    for line in tail:
        print(f"    {line}")

    if r.returncode != 0:
        sys.stderr.write(
            f"run_perf_benchmark: the PerfBenchmark suite exited "
            f"{r.returncode}; timings are not trustworthy and no floor is "
            f"claimed.\n")
        return 1

    if not out.exists() or out.stat().st_size == 0:
        sys.stderr.write(
            f"run_perf_benchmark: the suite passed but wrote no JSON at {out}. "
            f"Refusing to report success -- the checker downstream would then "
            f"compare against a stale or absent file.\n")
        return 1

    print(f"[perf-benchmark] wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
