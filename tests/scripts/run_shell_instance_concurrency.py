# -*- coding: utf-8 -*-
"""F63 — does `ShellAndInstanceLaunchTest` survive CONCURRENT execution?

Why this exists as a committed control rather than a throwaway
---------------------------------------------------------------
The F63 backlog row asserted a 2-of-5 failure split under concurrent sharded
execution. That split had never been re-measured, and by the time it was, the
harness it described had changed: `run_gtest.py::run_sharded` is explicitly
SEQUENTIAL ("N sequential fresh-process shards"), contains no
Popen/ThreadPoolExecutor/ProcessPoolExecutor at all, and CI invokes it with no
`--shards` whatsoever. So nothing in the normal path could still produce the
topology the row described, and a reader could reasonably have concluded the row
was stale.

It was not stale. It was unobserved, because nothing ran the topology any more.
This script runs it deliberately, so the question has an answer that does not
depend on someone remembering a claim.

The lesson is the one DCA 0045 records about `os.utime`: a fix -- or a
measurement -- that lives inside one throwaway script is not a control. The
hazard is a property of the product (a process-global single-instance domain
shared by every process running as one account), not of any particular runner,
so the probe for it is committed and re-runnable.

What it does
------------
Two groups, both from a cold start, neither killing anything:

  serial      each of the five cases alone, one process at a time. The CONTROL.
              If these fail, the hazard is not about concurrency at all and the
              diagnosis is wrong.
  concurrent  all five launched simultaneously, one process each. The topology
              the row describes.

Nothing is ever killed. A foreign `pdfe.exe` on this host may be a developer's
open editor with unsaved work, or the self-hosted runner's own child; see the
same reasoning at `run_ui_suite.py`'s reap guard. The count of running `pdfe.exe`
images is RECORDED before and after each group instead, which is the fact that
separates an environment collision from a product regression -- and, after the
fix, is also how a leaked orphan would show up.

Verdicts are read from the JUnit XML, never from the exit code: gtest can exit 0
with failures recorded in the XML (GAP-A15).

Measured with this script, on the fix commit's parent (pre-fix):

    serial      x4    0 of 5 failed, every time
    concurrent  x4    1-2 of 5 failed, every time, and ALWAYS from the same
                      pair -- SecondLaunchForwardsPathAndExitsWithoutSecondWindow
                      and ForwardedOpenRestoresTheMinimisedPrimaryWindow, which
                      are exactly the two cases that rely on the DEFAULT
                      single-instance behaviour. The other three either opt out
                      via PDFE_DISABLE_SINGLE_INSTANCE or never need to be
                      primary, and never failed.

and after the fix, `concurrent` is not merely green but ~22s against ~127s,
because the failures had been sitting out the full window timeout.

Usage:
    python tests/scripts/run_shell_instance_concurrency.py \
        --exe build/vs2022/bin/Debug/pdfe_tests.exe \
        --out build/f63-concurrency --repeat 3
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

SUITE = "ShellAndInstanceLaunchTest"

#: All five cases. Deliberately spelled out rather than discovered by
#: `--gtest_list_tests`: if someone adds a sixth case that also depends on the
#: default single-instance behaviour, this list not matching is a prompt to
#: think about it, where silent auto-discovery would just absorb it.
CASES = [
    "SecondLaunchForwardsPathAndExitsWithoutSecondWindow",
    "ForwardedOpenRestoresTheMinimisedPrimaryWindow",
    "DisableSingleInstanceEnvYieldsTwoIndependentEditors",
    "ShellOpenCommandFromPackagingOpensPathWithSpaces",
    "UnquotedPercentOneFailsToOpenPathWithSpaces",
]

#: The two that depend on the DEFAULT single-instance domain, and therefore the
#: two that F63 predicts will fail when the domain is shared across processes.
DEFAULT_DOMAIN_CASES = {
    "SecondLaunchForwardsPathAndExitsWithoutSecondWindow",
    "ForwardedOpenRestoresTheMinimisedPrimaryWindow",
}


def count_pdfe() -> int:
    """How many `pdfe.exe` images are running right now. Never kills anything.

    Returns -1 if the count could not be taken, which is reported as such rather
    than folded into 0 -- "could not measure" and "measured zero" are different
    facts and must not share a representation (DCA 0044).
    """
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe", "/NH"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        ).stdout
    except Exception:
        return -1
    return sum(1 for line in out.splitlines() if "pdfe.exe" in line.lower())


def verdict(xml_path: Path) -> str:
    """`pass` / `FAIL` / `NO-XML`, read from the JUnit XML.

    NO-XML is its own verdict and never silently a pass: a process that produced
    no result did not pass, and conflating the two is the exact defect DCA 0044
    exists for.
    """
    if not xml_path.exists():
        return "NO-XML"
    try:
        root = ET.parse(xml_path).getroot()
    except Exception:
        return "NO-XML"
    seen = False
    for tc in root.iter("testcase"):
        if tc.get("classname", "") != SUITE:
            continue
        seen = True
        if tc.find("failure") is not None or tc.find("error") is not None:
            return "FAIL"
    return "pass" if seen else "NO-XML"


def properties(xml_path: Path) -> dict:
    """SetUp-recorded environment facts, when the binary records them.

    `pdfe_processes_running_at_setup` / `foreign_pdfe_processes_at_setup` were
    added precisely so a failed run's XML answers "was this an environment
    collision?" directly. Historic XMLs predate them, which is why the original
    F63 evidence could not settle the question.
    """
    got = {}
    if not xml_path.exists():
        return got
    try:
        root = ET.parse(xml_path).getroot()
    except Exception:
        return got
    for prop in root.iter("property"):
        name = prop.get("name") or ""
        if "pdfe" in name:
            got[name] = prop.get("value")
    return got


def spawn(exe: str, case: str, out: Path, tag: str):
    xml = out / ("%s_%s.xml" % (tag, case))
    log = open(out / ("%s_%s.log" % (tag, case)), "w", encoding="utf-8")
    proc = subprocess.Popen(
        [exe,
         "--gtest_filter=%s.%s" % (SUITE, case),
         "--gtest_output=xml:%s" % xml],
        stdout=log, stderr=subprocess.STDOUT,
    )
    return proc, log, xml


def run_group(exe: str, out: Path, tag: str, concurrent: bool) -> dict:
    before = count_pdfe()
    started = time.time()
    results = {}
    if concurrent:
        live = [spawn(exe, c, out, tag) for c in CASES]
        for proc, log, _ in live:
            proc.wait()
            log.close()
        for (proc, _, xml), case in zip(live, CASES):
            results[case] = {"verdict": verdict(xml), "rc": proc.returncode,
                             "properties": properties(xml)}
    else:
        for case in CASES:
            proc, log, xml = spawn(exe, case, out, tag)
            proc.wait()
            log.close()
            results[case] = {"verdict": verdict(xml), "rc": proc.returncode,
                             "properties": properties(xml)}
    after = count_pdfe()
    return {
        "mode": tag,
        "concurrent": concurrent,
        "elapsed_s": round(time.time() - started, 1),
        "pdfe_processes_before": before,
        "pdfe_processes_after": after,
        "results": results,
        "failed": sorted(c for c, r in results.items() if r["verdict"] != "pass"),
    }


def main(argv) -> int:
    ap = argparse.ArgumentParser(
        description="Measure ShellAndInstanceLaunchTest under concurrency (F63).")
    ap.add_argument("--exe", required=True,
                    help="path to pdfe_tests.exe; pdfe.exe must sit beside it")
    ap.add_argument("--out", required=True)
    ap.add_argument("--repeat", type=int, default=1,
                    help="repeat each group N times. A hazard that appears in "
                         "one run of three is still a hazard, and one clean "
                         "pass is not evidence of absence.")
    ap.add_argument("--expect-clean", action="store_true",
                    help="exit non-zero if ANY case failed, or if a pdfe.exe "
                         "was left behind. This is the post-fix regression "
                         "mode; without it the script only measures.")
    args = ap.parse_args(argv)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    record = {"exe": args.exe, "cases": CASES,
              "default_domain_cases": sorted(DEFAULT_DOMAIN_CASES), "runs": []}
    for i in range(args.repeat):
        record["runs"].append(run_group(args.exe, out, "serial%d" % i, False))
        record["runs"].append(run_group(args.exe, out, "concurrent%d" % i, True))

    path = out / "f63-measurement.json"
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, sort_keys=True)

    for run in record["runs"]:
        print("[f63] %-12s elapsed=%6.1fs pdfe before/after=%d/%d failed=%s"
              % (run["mode"], run["elapsed_s"], run["pdfe_processes_before"],
                 run["pdfe_processes_after"], run["failed"] or "none"),
              flush=True)
    print("[f63] wrote %s" % path, flush=True)

    if not args.expect_clean:
        return 0

    rc = 0
    for run in record["runs"]:
        if run["failed"]:
            print("[f63] REGRESSION: %s failed %s" % (run["mode"], run["failed"]),
                  flush=True)
            rc = 1
        # A leaked pdfe.exe is its own failure mode: it holds the machine-global
        # mutex and wedges the NEXT run, which then reads as a product defect.
        before, after = run["pdfe_processes_before"], run["pdfe_processes_after"]
        if before >= 0 and after > before:
            print("[f63] ORPHAN: %s left %d extra pdfe.exe running (%d -> %d)"
                  % (run["mode"], after - before, before, after), flush=True)
            rc = 1
    if rc == 0:
        print("[f63] CLEAN: concurrent execution matches serial, no orphans.",
              flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
