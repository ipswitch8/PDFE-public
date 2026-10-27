#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Bisect the NATIVE-stack failure depth for nested Form XObjects (GitHub #176).

WHY THIS EXISTS
---------------
GitHub #159 was an untrappable process kill: a deeply nested Form XObject chain
exhausts the *native* stack inside ``fz_load_page`` -- MuPDF walks the resource
tree at page LOAD, not at render -- and ``fz_try`` is setjmp-based, so nothing
in-process can catch it. The fix added ``pdfe::core::FormXObjectDepthGuard``,
which refuses such a document before the load.

The limit it enforces (256) was **argued, not measured**. The prior pipeline
established that MuPDF's own catchable guard copes from depth 64 through 1024
and that 4096 kills the process, then chose 256 with reasoning about margin.
Nobody bisected the actual failure point, so "256 has ample margin" was a
judgement about an unmeasured quantity -- and a margin that is ample on this
platform's 1 MiB default thread stack is not known to be ample on a smaller
stack, a different MuPDF build, or a document shape that consumes more stack per
level than the fixture does.

This script measures it.

HOW
---
Binary search over depth. At each probe it spawns ``pdfe_tests.exe`` running the
existing child role ``NestedFormXObjectBoundChild.RunRole`` on a generated
nested-XObject document, and classifies the outcome by the child's EXIT CODE --
not by anything the child says, because a child that dies of stack exhaustion
says nothing at all.

  survived : the child exited normally (0 or 1) and wrote a result file
  killed   : the child was terminated by the OS -- STATUS_STACK_OVERFLOW
             (0xC00000FD) is the one we are looking for

The guard MUST be raised out of the way for this to measure anything. Without
the override every probe above 256 is answered by the guard, and the bisect
would converge on 256 -- i.e. it would rediscover its own input and report it as
a measurement. ``PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE`` (compiled in only under
``-DPDFE_TESTING=ON``) raises it; see src/core/FormXObjectDepthGuard.cpp.

A CONTROL RUN IS MANDATORY and is performed first: a shallow document must
SURVIVE with the override in force. Without it, a bisect in which every probe
dies for an unrelated reason (a missing DLL, a bad fixture, an exe that will not
start) reports a failure depth of "the lowest probe" and looks like a
measurement. The control is what separates "the stack ran out" from "nothing
ever ran".

THE FAILURE DEPTH IS A BAND, NOT A NUMBER -- AND THAT IS WHY EVERY PROBE IS
REPEATED
-----------------------------------------------------------------------------
The first version of this script probed each depth ONCE and binary-searched for
the boundary. It converged on "3999 survives, 4000 kills" -- a sharp, confident,
and false answer. Re-running the two endpoints three times each produced: 3999
killed on one trial, and 4000 SURVIVED on another. The transition is stochastic,
because the thread's stack base is randomised and per-level stack consumption is
not perfectly constant.

A single-shot bisect over a non-monotonic predicate does not fail loudly. It
converges on an arbitrary point inside the transition band and reports it to
four significant figures. The measured band, at 7 trials per depth, is:

    <= 3968   0/7 killed      always survives
       4000   4/7 killed      STOCHASTIC -- neither safe nor reliably fatal
    >= 4032   7/7 killed      always dies

So each probe now runs ``--trials`` times and is classified three ways, and the
search reports BOTH edges: the deepest depth that survived every trial, and the
shallowest that died in every trial. A margin must be computed against the
FORMER. Quoting the midpoint of a band as though it were a threshold is the
precise error this script was rewritten to stop making.

USAGE
-----
    python tests/scripts/bisect_xobject_native_depth.py \
        --exe build/vs2022/bin/Debug/pdfe_tests.exe \
        --out docs/audit/p83-outstanding/phase-6/native-stack-depth.json

Windows-only: the kill-code classification is Win32-specific, as is the child
role itself.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# STATUS_STACK_OVERFLOW. The child does not get to run a handler, so this is the
# only evidence the failure produces.
STATUS_STACK_OVERFLOW = 0xC00000FD

# Anything at or above this is an NTSTATUS-shaped abnormal termination rather
# than a gtest exit code. gtest uses 0 (all passed) and 1 (a test failed); the
# project's own run_gtest.py treats "rc not in (0, 1)" as abnormal for the same
# reason.
NORMAL_EXIT_CODES = (0, 1)


def nested_xobject_pdf(depth: int) -> bytes:
    """Byte-for-byte the same document shape as ``nestedXObjectPdf(int)`` in
    tests/test_malicious_pdf_advanced.cpp.

    Kept deliberately identical so this script measures the SAME input the
    regression sweep uses. If the C++ generator changes, this must change with
    it or the recorded depth stops describing the tested document -- the
    round-trip test asserts they agree.

    No xref table: MuPDF reconstructs. Computing exact offsets for thousands of
    objects would make the generator the thing under test.
    """
    out = "%PDF-1.7\n"
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    out += ("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            "/Resources << /XObject << /X0 10 0 R >> >> /Contents 4 0 R >>\n"
            "endobj\n")

    page_stream = "q /X0 Do Q\n"
    out += ("4 0 obj\n<< /Length " + str(len(page_stream)) + " >>\nstream\n"
            + page_stream + "endstream\nendobj\n")

    for i in range(depth):
        obj_num = 10 + i
        innermost = (i == depth - 1)
        body = "0 0 100 100 re f\n" if innermost else "q /X" + str(i + 1) + " Do Q\n"
        dict_str = ("<< /Type /XObject /Subtype /Form "
                    "/BBox [0 0 612 792] /Length " + str(len(body)))
        if not innermost:
            dict_str += (" /Resources << /XObject << /X" + str(i + 1) + " "
                         + str(obj_num + 1) + " 0 R >> >>")
        dict_str += " >>"
        out += (str(obj_num) + " 0 obj\n" + dict_str + "\nstream\n" + body
                + "endstream\nendobj\n")

    out += ("trailer\n<< /Size " + str(10 + depth)
            + " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n")
    return out.encode("utf-8")


def probe(exe: Path, workdir: Path, depth: int, op: str, override: int,
          timeout_s: int) -> dict:
    """Run one depth in a child process. Returns a classification dict."""
    pdf_path = workdir / ("bisect_%d_%s.pdf" % (depth, op))
    res_path = workdir / ("bisect_%d_%s.txt" % (depth, op))
    with io.open(pdf_path, "wb") as f:
        f.write(nested_xobject_pdf(depth))
    if res_path.exists():
        res_path.unlink()

    env = dict(os.environ)
    env["PDFE_XOBJ_BOUND_FILE"] = str(pdf_path)
    env["PDFE_XOBJ_BOUND_RESULT"] = str(res_path)
    env["PDFE_XOBJ_BOUND_OP"] = op
    env["PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE"] = str(override)
    # Without this the seam is not compiled in -- but if the build was
    # configured without PDFE_TESTING the override is statically dead and the
    # control run below will show the guard refusing at 256 regardless. That is
    # a LOUD failure, which is the intent.
    env["PDFE_TEST_PIPES"] = "1"

    try:
        cp = subprocess.run(
            [str(exe), "--gtest_filter=NestedFormXObjectBoundChild.RunRole",
             "--gtest_brief=1"],
            env=env, timeout=timeout_s,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        rc = cp.returncode
        timed_out = False
    except subprocess.TimeoutExpired:
        rc = None
        timed_out = True

    stage, outcome = "", ""
    if res_path.exists():
        with io.open(res_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k == "stage":
                    stage = v
                elif k == "outcome":
                    outcome = v

    # Windows reports the NTSTATUS as an unsigned value through some paths and
    # as a negative signed int through others. Normalise before comparing.
    rc_u = None if rc is None else (rc & 0xFFFFFFFF)

    if timed_out:
        cls = "timeout"
    elif rc_u in NORMAL_EXIT_CODES:
        cls = "survived"
    elif rc_u == STATUS_STACK_OVERFLOW:
        cls = "killed_stack_overflow"
    else:
        cls = "killed_other"

    return {"depth": depth, "op": op, "classification": cls,
            "exit_code": rc_u,
            "exit_code_hex": None if rc_u is None else ("0x%08X" % rc_u),
            "child_stage": stage, "child_outcome": outcome}


def probe_repeated(exe: Path, workdir: Path, depth: int, op: str, override: int,
                   timeout_s: int, trials: int) -> dict:
    """Run one depth ``trials`` times and classify the depth, not the run.

    Three outcomes, because a two-way classification is exactly what produced
    the false sharp boundary described in the module docstring:

      always_survived : 0 kills out of ``trials``
      stochastic      : some kills, some survivals -- inside the transition band
      always_killed   : ``trials`` kills out of ``trials``
    """
    runs = [probe(exe, workdir, depth, op, override, timeout_s)
            for _ in range(trials)]
    kills = sum(1 for r in runs if r["classification"] != "survived")
    if kills == 0:
        cls = "always_survived"
    elif kills == trials:
        cls = "always_killed"
    else:
        cls = "stochastic"
    kill_kinds = sorted({r["classification"] for r in runs
                         if r["classification"] != "survived"})
    return {"depth": depth, "op": op, "trials": trials, "kills": kills,
            "classification": cls, "kill_classifications": kill_kinds,
            "runs": runs}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path,
                    help="path to pdfe_tests.exe")
    ap.add_argument("--out", required=True, type=Path,
                    help="path to write the measurement JSON")
    ap.add_argument("--op", default="pageinfo",
                    choices=["pageinfo", "page", "displaylist"],
                    help="entry point to probe; pageinfo is the one reachable "
                         "by merely opening a document")
    ap.add_argument("--lo", type=int, default=64,
                    help="depth known to survive (lower bound)")
    ap.add_argument("--hi", type=int, default=8192,
                    help="depth expected to kill (upper bound)")
    ap.add_argument("--override", type=int, default=1000000,
                    help="value for PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE; must be far "
                         "above --hi or the guard answers instead of the stack")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--trials", type=int, default=7,
                    help="runs per probed depth. MUST be > 1: the predicate is "
                         "stochastic near the boundary, and a single-shot "
                         "search reports a random point in the band as a "
                         "threshold. See the module docstring.")
    args = ap.parse_args()

    if args.trials < 2:
        print("bisect: --trials must be at least 2. A single trial per depth "
              "cannot distinguish 'survives' from 'survived this once', which "
              "is how this script previously reported a false sharp boundary.",
              file=sys.stderr)
        return 2

    if not args.exe.is_file():
        print("bisect: exe not found: %s" % args.exe, file=sys.stderr)
        return 2
    if args.override <= args.hi:
        print("bisect: --override (%d) must exceed --hi (%d), or the guard "
              "refuses before the stack is reached and the bisect converges on "
              "the guard's own limit." % (args.override, args.hi),
              file=sys.stderr)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="pdfe_xobj_bisect_"))
    probes = []

    # ---- CONTROL. Non-negotiable: see the module docstring. ----
    control = probe(args.exe, tmp, 8, args.op, args.override, args.timeout)
    probes.append(dict(control, role="control_shallow"))
    if control["classification"] != "survived":
        print("bisect: CONTROL FAILED -- a depth-8 document did not survive "
              "(%s, exit %s, stage '%s'). Nothing below is a measurement of "
              "stack depth; fix the harness first."
              % (control["classification"], control["exit_code_hex"],
                 control["child_stage"]), file=sys.stderr)
        return 3
    if control["child_outcome"] == "refused":
        print("bisect: CONTROL REFUSED -- the guard answered at depth 8 with "
              "an override of %d in force. The override is not reaching the "
              "guard: either the build was configured without -DPDFE_TESTING "
              "(the seam is then statically dead) or the seam is not consulted "
              "at the deciding site. Not a measurement."
              % args.override, file=sys.stderr)
        return 3

    # ---- Confirm the upper bound actually kills, EVERY time. ----
    top = probe_repeated(args.exe, tmp, args.hi, args.op, args.override,
                         args.timeout, args.trials)
    probes.append(dict(top, role="upper_bound"))
    print("  depth %6d -> %s (%d/%d killed)"
          % (args.hi, top["classification"], top["kills"], top["trials"]))
    if top["classification"] != "always_killed":
        print("bisect: --hi (%d) did not die on every trial (%d/%d). The "
              "always-fatal depth is above the search range; raise --hi and "
              "re-run. Reporting an edge the search never bracketed would be a "
              "fabricated number."
              % (args.hi, top["kills"], top["trials"]), file=sys.stderr)
        return 4

    # ---- Two searches, because there are two edges. ----
    #
    # safe_hi : deepest depth observed to survive ALL trials
    # fatal_lo: shallowest depth observed to die on ALL trials
    #
    # Between them lies the stochastic band. Collapsing the two into one number
    # is what the first version of this script did wrong.
    # `not_safe_hi` is the binary-search upper bound: the shallowest depth known
    # NOT to survive every trial. A stochastic depth qualifies -- it is not safe
    # -- so it narrows this bound. It is deliberately NOT the same quantity as
    # `fatal_lo`, which tracks only depths that died on EVERY trial. Conflating
    # them made the previous run report 3995 as "always fatal" when 3995 had in
    # fact killed 1 of 7. Two different claims need two different variables.
    safe_hi = args.lo
    not_safe_hi = args.hi
    fatal_lo = args.hi

    while not_safe_hi - safe_hi > 1:
        mid = (safe_hi + not_safe_hi) // 2
        r = probe_repeated(args.exe, tmp, mid, args.op, args.override,
                           args.timeout, args.trials)
        probes.append(dict(r, role="search"))
        print("  depth %6d -> %s (%d/%d killed)"
              % (mid, r["classification"], r["kills"], r["trials"]))
        if r["classification"] == "always_survived":
            safe_hi = mid
        else:
            not_safe_hi = mid
            if r["classification"] == "always_killed":
                fatal_lo = min(fatal_lo, mid)

    band = [p for p in probes if p.get("classification") == "stochastic"]

    shipped_limit = 256
    result = {
        "issue": "GitHub #176",
        "what_was_measured": ("the depth at which a nested Form XObject chain "
                              "terminates the process natively, with the "
                              "FormXObjectDepthGuard raised out of the way"),
        "entry_point": args.op,
        "trials_per_depth": args.trials,
        "deepest_always_surviving_depth": safe_hi,
        "shallowest_not_always_surviving_depth": not_safe_hi,
        "shallowest_always_fatal_depth": fatal_lo,
        "shallowest_always_fatal_depth_caveat": (
            "This is the shallowest PROBED depth that died on every trial, not "
            "a located edge. The search narrows the SAFE edge, so once it is "
            "inside the stochastic band it stops probing deeper. The true "
            "always-fatal edge lies somewhere at or below this value and above "
            "the deepest stochastic observation. Nothing depends on it: the "
            "margin is computed against the safe edge."),
        "stochastic_band_observed": [
            {"depth": p["depth"], "kills": p["kills"], "trials": p["trials"]}
            for p in band
        ],
        "not_a_single_threshold": (
            "The failure depth is a BAND, not a number. Depths inside it kill "
            "on some runs and survive on others, because the thread stack base "
            "is randomised and per-level consumption is not constant. Any "
            "margin must be computed against deepest_always_surviving_depth, "
            "the conservative edge."),
        "guard_override_in_force": args.override,
        "shipped_limit": shipped_limit,
        "margin_factor_vs_conservative_edge":
            round(float(safe_hi) / float(shipped_limit), 2),
        "platform_note": ("Windows, default 1 MiB thread stack, MuPDF 1.26.10, "
                          "Debug build. A Debug build spends more stack per "
                          "level than Release, so this is the PESSIMISTIC "
                          "direction -- which is the right direction for a "
                          "safety margin. It remains a measurement of THIS "
                          "configuration, not a universal constant."),
        "probes": probes,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with io.open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(result, f, indent=2, sort_keys=True)
        f.write("\n")

    print("\nbisect: deepest ALWAYS-surviving        = %d" % safe_hi)
    print("bisect: shallowest NOT-always-surviving = %d" % not_safe_hi)
    print("bisect: shallowest ALWAYS-fatal         = %d" % fatal_lo)
    if band:
        print("bisect: stochastic band observed at: %s"
              % ", ".join("%d (%d/%d killed)"
                          % (p["depth"], p["kills"], p["trials"])
                          for p in band))
    print("bisect: shipped limit %d -> margin %.2fx against the conservative "
          "edge" % (shipped_limit,
                    result["margin_factor_vs_conservative_edge"]))
    print("bisect: wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
