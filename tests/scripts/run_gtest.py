# -*- coding: utf-8 -*-
"""Run the pdfe_tests GoogleTest suite with a reliable green/red signal.

Two execution modes
--------------------
1. **Single-process** (``--shards 1``, the historical default): run the whole
   suite in ONE ``pdfe_tests.exe`` process, then re-run any FAILED test in a
   fresh process. This heals transient flakes but CANNOT survive a hard crash:
   a process that crashes before writing JUnit XML produces no XML at all, so
   there is nothing to enumerate and re-run — the run is silently lost.

2. **Sharded** (``--shards N`` with ``N > 1``): the canonical Windows harness.
   Enumerate the full test list ONCE, partition it deterministically into N
   disjoint subsets, and run each subset as its own SEQUENTIAL (never
   concurrent) ``pdfe_tests.exe`` process with its own JUnit XML. Merge the
   per-shard XMLs into one aggregate ``gtest_full.xml``. Then re-run genuine
   failures individually in fresh processes, exactly as mode 1 does.

Why sharding fixes the hard crash
---------------------------------
The single-process full-suite run non-deterministically hard-crashes at a
random late Qt/MainWindow-heavy test. Root cause is CUMULATIVE PROCESS STATE:
Qt global singletons, QApplication-level caches, leaked handles, and shared
temp/sqlite state accumulate across thousands of tests in one process until a
late widget-heavy test is corrupted (e.g. ``createToolBar()`` intermittently
drops ``zoomCombo`` — see ``MainWindowIntegrationTest.ZoomComboExists``). The
SAME tests pass reliably in a FRESH process and in isolation. A shard is a
fresh process running only a few hundred tests, so the accumulation never
reaches the corruption threshold. See ``docs/windows-fixes/phase-2-harness.md``.

Crashed-shard -> RED invariant
------------------------------
If a shard crashes without emitting a (parseable, non-empty) JUnit XML, EVERY
test assigned to that shard is reported as an ``error`` in the aggregate. A
crashed shard therefore turns the aggregate RED — its tests are NEVER silently
dropped, and the run can never be vacuously green. This preserves the GAP-A15
invariant (aggregate ``tests`` == passed + failed + skipped + errors, no silent
truncation).

Usage:
    # single-process (back-compat)
    python tests/scripts/run_gtest.py --exe build/vs2022/bin/Debug/pdfe_tests.exe \
        --output-dir build/gtest_out [--reruns 2] [--gtest-filter ...]

    # sharded (canonical Windows harness)
    python tests/scripts/run_gtest.py --exe .../pdfe_tests.exe \
        --output-dir build/gtest_out --shards 16 [--reruns 2]

Exit code 0 iff, after reruns, there are zero unresolved failures AND no shard
crashed unrecovered.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# --------------------------------------------------------------------------- #
# Pure logic (unit-tested in tests/scripts/test_run_gtest_sharding.py; these
# functions touch no pdfe_tests process and no filesystem).
# --------------------------------------------------------------------------- #


def parse_gtest_list(text: str) -> list[str]:
    """Parse ``pdfe_tests.exe --gtest_list_tests`` output into full test ids.

    The gtest listing format is::

        SuiteName.
          TestName1
          TestName2
        Parameterized/Suite.
          Test/0  # GetParam() = ...
          Test/1  # GetParam() = ...

    A non-indented line ending in '.' is a suite prefix; each subsequent
    indented line is a test under it, joined as ``Suite.Test``. Any trailing
    ``# ...`` value-parameterized comment is stripped.

    For convenience this also accepts a plain "one full id per line" list
    (lines like ``Suite.Test`` with no indentation and not ending in '.'),
    so a cached test-list file can be fed in directly.
    """
    ids: list[str] = []
    suite: str | None = None
    for raw in text.splitlines():
        if not raw.strip():
            continue
        # Strip value-parameterized comment (e.g. "Test/0  # GetParam() = 1").
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        indented = line[0] in (" ", "\t")
        stripped = line.strip()
        if not indented and stripped.endswith("."):
            # Suite header line: "SuiteName."
            suite = stripped[:-1]
            continue
        if indented:
            # Test line under the current suite.
            if suite:
                ids.append(f"{suite}.{stripped}")
            # An indented line with no current suite is malformed; skip it.
            continue
        # Non-indented, does not end in '.'. Treat as a plain full id if it
        # looks like one: contains a '.' and no whitespace (real test ids never
        # contain spaces). This rejects gtest banner text like
        # "Running main() from gmock_main.cc".
        if "." in stripped and not any(c.isspace() for c in stripped):
            ids.append(stripped)
            suite = None
    return ids


def partition_tests(test_ids: list[str], shards: int) -> list[list[str]]:
    """Deterministically partition ``test_ids`` into ``shards`` disjoint lists.

    Round-robin assignment (test i -> shard ``i % shards``). Properties, all
    asserted by the unit tests:
      * deterministic — same input always yields the same partition,
      * disjoint + total — every id appears in exactly one shard,
      * balanced — shard sizes differ by at most 1,
      * order-preserving within a shard.

    Empty shards (when ``shards`` > ``len(test_ids)``) are returned as empty
    lists so shard indices stay stable.
    """
    if shards < 1:
        raise ValueError("shards must be >= 1")
    buckets: list[list[str]] = [[] for _ in range(shards)]
    for i, tid in enumerate(test_ids):
        buckets[i % shards].append(tid)
    return buckets


# GitHub #247. The per-shard filter is passed in a `--gtest_flagfile`, NOT on
# the command line -- see `write_flagfile` and `flagfile_is_honoured`. A file's
# size is bounded by the filesystem; a CreateProcess command line is capped at
# 32,767 characters. Measured on this tree: a 42,948-character filter naming 801
# tests, which argv cannot carry at all, selects exactly those 801 via a
# flagfile, and `--shards 8` runs.
#
# The constants below are the FALLBACK path's budget, used only when the probe
# finds a googletest that does not apply `--gtest_flagfile`. They were the whole
# mechanism until 2026-09-26, when the filter moved into a file; the measured
# argv lengths that made `--shards 8` impossible were:
#
#     --shards 16   21,728 chars   fits
#     --shards 12   28,870 chars   fits
#     --shards 10   34,643 chars   OVER
#     --shards  8   43,232 chars   OVER
#
# Kept rather than deleted because the fallback is real: a build without
# flagfile support still has to refuse a shard count it cannot launch, and must
# say why rather than dying with a bare `WinError 206`.
#
# Still NOT done, deliberately: switching to gtest's own GTEST_SHARD_INDEX
# partitioning. That would change which tests land in which shard, and this
# harness attributes a crashed shard's tests from its OWN partition, so a
# mismatch would silently mis-report what crashed. The flagfile removes the
# limit without touching the partition.
WINDOWS_CMDLINE_LIMIT = 32767

#: Room for the executable path, --gtest_output, and the rest of the argv.
#: Fallback path only; the flagfile path is not bounded by this.
_SHARD_ARGV_OVERHEAD = 2048


def shard_filter_budget(test_ids: list[str], shards: int,
                        limit: int = WINDOWS_CMDLINE_LIMIT) -> tuple:
    """(ok, worst_len, min_viable_shards) for an explicit-filter launch."""
    usable = limit - _SHARD_ARGV_OVERHEAD
    worst = max((len(shard_filter(p)) for p in partition_tests(test_ids, shards)),
                default=0)
    min_viable = shards
    while min_viable < len(test_ids):
        w = max((len(shard_filter(p))
                 for p in partition_tests(test_ids, min_viable)), default=0)
        if w <= usable:
            break
        min_viable += 1
    return worst <= usable, worst, min_viable


def shard_filter(test_ids: list[str]) -> str:
    """Build a ``--gtest_filter`` value selecting exactly ``test_ids``.

    gtest filters are ':'-separated positive patterns. Full test ids contain
    no ':' so they can be joined directly.
    """
    return ":".join(test_ids)


def _synthesize_error_cases(assigned: list[str], message: str) -> list[ET.Element]:
    """Build ``<testcase>`` elements carrying a fatal ``<error>`` for each id.

    Used to represent the tests of a shard that crashed without emitting XML,
    so they surface as errors in the aggregate rather than vanishing.
    """
    cases: list[ET.Element] = []
    for tid in assigned:
        suite, _, name = tid.partition(".")
        tc = ET.Element("testcase", {
            "name": name or tid,
            "classname": suite or tid,
            "status": "run",
        })
        err = ET.SubElement(tc, "error", {
            "message": message,
            "type": "shard-crash",
        })
        err.text = message
        cases.append(tc)
    return cases


def inject_teardown_crash_evidence(
        root: ET.Element, crashed: list[tuple[int, int]]) -> int:
    """GitHub #300 — make a teardown crash visible to a reader of the XML ALONE.

    THE PROBLEM THIS SOLVES. A pipeline evidence gate passes when a green
    `gtest-results.xml` is staged beside its verdict artifact, and that XML is
    the sole evidence. **A JUnit XML cannot record that the process which wrote
    it then died** — by definition nothing was left to write that down. So a run
    that executes every test, writes a complete green report, and crashes during
    teardown produces an artifact that satisfies the gate exactly as a clean run
    does.

    A crash that kills a shard MID-run is already covered: `aggregate_shards`
    synthesises an `<error>` for every test the dead shard was assigned, so the
    aggregate is RED and any gate refuses it. The teardown case is the one with
    no red anywhere — every assertion passed, and the only carrier of the
    failure was the exit code, which does not survive into the file.

    So the classification is written INTO the artifact, as the one thing a
    JUnit reader cannot ignore: a `<testcase>` with an `<error>`. After this, a
    gate that counts `failures`/`errors` — which is exactly what
    `phase-gate.sh::validate_junit_xml` does — sees the crash without needing
    to know anything about exit codes or sidecar files.

    Deliberately an `<error>` and not a `<skipped>` or a bare property: a
    property is invisible to every existing reader, and this must be impossible
    to pass by accident. The synthetic case is named for the shard so the
    artifact says WHICH process died, not merely that one did.

    Returns the number of cases injected, so a caller can report it rather than
    assume it.
    """
    if not crashed:
        return 0
    suite = ET.SubElement(root, "testsuite", {
        "name": "ProcessTeardown",
        "tests": str(len(crashed)),
        "failures": "0",
        "errors": str(len(crashed)),
        "disabled": "0",
        "skipped": "0",
    })
    for idx, rc in crashed:
        msg = (f"shard {idx} ran and PASSED every assigned test, then the "
               f"process exited rc={rc}. The report is green and the process "
               f"died anyway: look at static/global destruction order, not at "
               f"the assertions. Recorded here because a JUnit XML otherwise "
               f"carries no evidence of its own writer's exit code "
               f"(GitHub #300).")
        tc = ET.SubElement(suite, "testcase", {
            "name": f"shard_{idx}_exited_rc_{rc}",
            "classname": "ProcessTeardown",
            "status": "run",
        })
        err = ET.SubElement(tc, "error", {
            "message": msg,
            "type": "teardown-crash",
        })
        err.text = msg
    # Keep the root totals honest, or a reader that trusts the attributes over
    # the elements (which GAP-A15 says some do) would still see a clean run.
    for attr, delta in (("tests", len(crashed)), ("errors", len(crashed))):
        root.set(attr, str(int(root.get(attr, "0") or "0") + delta))
    return len(crashed)


def _is_abnormal_exit(rc: int) -> bool:
    """True if ``rc`` is a CRASH rather than an ordinary test-failure exit.

    Added phase-61. ``_is_crashed()`` below only inspects the XML, so a shard
    that runs every test, writes a COMPLETE and GREEN report, and then dies
    during process teardown was reported as ``rc=3221225477 -> ok``. A real
    access violation — caused by MuPDF objects held in a function-local static
    and therefore destroyed after ``main()`` returned — was invisible to the
    whole harness, because everything that reads results reads assertions, and
    the crash arrived after the last one.

    This is GAP-A15's sibling: that gap was an exit code of ZERO hiding
    failures in the XML; this is a NON-zero exit code hidden BY the XML.

    gtest exits 1 for ordinary failures, so 0 and 1 are normal. Anything else
    is a structured exception (Windows ``0xC0000005`` arrives as 3221225477) or
    a signal (POSIX shells report 128+N; Python reports -N).
    """
    return rc not in (0, 1)


def _is_crashed(root: ET.Element | None) -> bool:
    """A shard is 'crashed' if it produced no parseable, non-empty XML."""
    if root is None:
        return True
    # A parseable root that reports zero testcases means the process died
    # before running/recording anything -> treat as crashed.
    if not list(root.iter("testcase")):
        return True
    return False


def aggregate_shards(
    shards: list[tuple[list[str], ET.Element | None]],
) -> tuple[ET.Element, dict, list[dict]]:
    """Merge per-shard results into one ``<testsuites>`` aggregate.

    ``shards`` is a list of ``(assigned_test_ids, shard_xml_root_or_None)``
    in shard-index order. A ``None`` root (or an empty one) means that shard
    CRASHED: every assigned test becomes an ``error`` (the crashed-shard ->
    RED invariant).

    Returns ``(aggregate_root, totals, per_shard_status)`` where ``totals`` is
    ``{tests, failures, errors, disabled, skipped}`` counted from the actual
    aggregated testcases (GAP-A15: tests == passed + failed + skipped + errors),
    and ``per_shard_status`` is a list of per-shard summary dicts.
    """
    agg = ET.Element("testsuites")
    # Map suite-name -> aggregated <testsuite> element to keep the tree valid.
    suites: dict[str, ET.Element] = {}
    per_shard: list[dict] = []

    def _suite_for(name: str) -> ET.Element:
        el = suites.get(name)
        if el is None:
            el = ET.SubElement(agg, "testsuite", {"name": name})
            suites[name] = el
        return el

    for idx, (assigned, root) in enumerate(shards):
        crashed = _is_crashed(root)
        if crashed:
            cases = _synthesize_error_cases(
                assigned,
                f"shard {idx} crashed without emitting JUnit XML "
                f"({len(assigned)} assigned test(s) recorded as errors)",
            )
        else:
            cases = list(root.iter("testcase"))  # type: ignore[union-attr]

        s_tests = s_fail = s_err = s_skip = 0
        for tc in cases:
            _suite_for(tc.get("classname", "") or "UnknownSuite").append(tc)
            s_tests += 1
            if tc.find("failure") is not None:
                s_fail += 1
            if tc.find("error") is not None:
                s_err += 1
            # gtest marks a skip via status/result attrs or a <skipped> child.
            if (tc.find("skipped") is not None
                    or tc.get("status") == "notrun"
                    or tc.get("result") == "skipped"):
                s_skip += 1
        per_shard.append({
            "shard": idx,
            "assigned": len(assigned),
            "recorded": s_tests,
            "failures": s_fail,
            "errors": s_err,
            "skipped": s_skip,
            "crashed": crashed,
        })

    # Roll up totals + per-suite attributes from the actual aggregated cases.
    totals = {"tests": 0, "failures": 0, "errors": 0, "disabled": 0, "skipped": 0}
    for suite_el in suites.values():
        st = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0}
        for tc in suite_el.findall("testcase"):
            st["tests"] += 1
            if tc.find("failure") is not None:
                st["failures"] += 1
            if tc.find("error") is not None:
                st["errors"] += 1
            if (tc.find("skipped") is not None
                    or tc.get("status") == "notrun"
                    or tc.get("result") == "skipped"):
                st["skipped"] += 1
        for k, v in st.items():
            suite_el.set(k, str(v))
        for k in ("tests", "failures", "errors", "skipped"):
            totals[k] += st[k]

    for k in ("tests", "failures", "errors", "disabled", "skipped"):
        agg.set(k, str(totals[k]))
    return agg, totals, per_shard


def _failed_ids_from_root(root: ET.Element | None) -> list[str]:
    """Return 'Suite.Name' ids of every failed/errored testcase in a root."""
    out: list[str] = []
    if root is None:
        return out
    for tc in root.iter("testcase"):
        if tc.find("failure") is not None or tc.find("error") is not None:
            suite = tc.get("classname", "")
            name = tc.get("name", "")
            if suite and name:
                out.append(f"{suite}.{name}")
    return out


# --------------------------------------------------------------------------- #
# Filesystem / process helpers (thin wrappers over the pure logic above).
# --------------------------------------------------------------------------- #


def _failed_testcases(xml_path: Path) -> list[str]:
    """Return 'Suite.Name' ids of every failed/errored testcase in a gtest XML."""
    if not xml_path.exists():
        return []
    try:
        root = ET.parse(str(xml_path)).getroot()
    except ET.ParseError:
        return []
    return _failed_ids_from_root(root)


def _totals(xml_path: Path) -> dict:
    t = {"tests": 0, "failures": 0, "errors": 0, "disabled": 0, "skipped": 0}
    if not xml_path.exists():
        return t
    try:
        root = ET.parse(str(xml_path)).getroot()
    except ET.ParseError:
        return t
    for k in t:
        t[k] = int(root.get(k, "0") or 0)
    return t


def _parse_root(xml_path: Path) -> ET.Element | None:
    """Parse a JUnit XML into a root element, or None if missing/corrupt."""
    if not xml_path.exists():
        return None
    try:
        return ET.parse(str(xml_path)).getroot()
    except ET.ParseError:
        return None


def write_flagfile(path: Path, gtest_filter: str) -> Path:
    """Write a one-line ``--gtest_filter=...`` flagfile and return its path.

    GoogleTest reads ``--gtest_flagfile=FILE`` and applies the flags inside it
    as if they had been on the command line — but the FILE's size is bounded by
    the filesystem, not by the 32,767-character CreateProcess limit. Measured on
    this tree: a 42,948-character filter naming 801 tests, which cannot be
    passed on argv at all, selects exactly those 801 through a flagfile.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n" so the file is byte-identical regardless of core.autocrlf.
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("--gtest_filter=%s\n" % gtest_filter)
    return path


def flagfile_is_honoured(exe: str, scratch: Path) -> tuple:
    """(ok, detail) — does THIS binary actually apply a --gtest_flagfile?

    This is the load-bearing preflight for the whole mechanism, and it exists
    because the failure is silent and expensive. A googletest built without
    flagfile support does not reject the flag loudly; the filter simply never
    applies, so every shard would run the ENTIRE suite. Sixteen shards would
    each run 6,401 tests, the aggregate would count each test sixteen times,
    and the run would look like a slow success.

    So the support is MEASURED, once, before any shard launches: write a
    flagfile selecting exactly one known test and check that exactly one is
    listed. A binary that ignores the flag lists the whole suite and fails here
    instead of producing a plausible, wrong run.
    """
    ids = _list_tests(exe, None, None, scratch / "_flagfile_probe_list.log")
    if not ids:
        return False, "could not enumerate any tests to probe with"
    probe = ids[0]
    ff = write_flagfile(scratch / "_flagfile_probe.txt", probe)
    proc = subprocess.run([exe, f"--gtest_flagfile={ff}", "--gtest_list_tests"],
                          capture_output=True, text=True, errors="replace")
    listed = parse_gtest_list(proc.stdout or "")
    if listed == [probe]:
        return True, "verified: a flagfile selecting 1 test listed exactly 1"
    return False, (
        "a flagfile selecting 1 test (%s) listed %d test(s). This binary does "
        "not apply --gtest_flagfile, so a per-shard filter would be silently "
        "ignored and every shard would run the whole suite."
        % (probe, len(listed)))


def _run(exe: str, out_xml: Path, log: Path, gtest_filter: str | None,
         flagfile: Path | None = None) -> int:
    cmd = [exe, "--gtest_color=no", f"--gtest_output=xml:{out_xml}"]
    if flagfile is not None:
        # GitHub #247: the filter goes in a FILE, not on argv, so the shard
        # count is no longer bounded by the Windows command-line limit.
        cmd.append(f"--gtest_flagfile={flagfile}")
    elif gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    return proc.returncode


def _list_tests(exe: str, gtest_filter: str | None,
                test_list_file: Path | None, log: Path) -> list[str]:
    """Enumerate full test ids, either from a cached file or the exe itself.

    When ``test_list_file`` is provided it is read directly (no process is
    spawned) — this is how the harness partitions without touching pdfe_tests.
    Otherwise ``--gtest_list_tests`` is invoked AT RUN TIME. Enumeration does
    not execute any test body; it is near-instant and lists only.
    """
    if test_list_file is not None:
        text = Path(test_list_file).read_text(encoding="utf-8", errors="replace")
        return parse_gtest_list(text)
    cmd = [exe, "--gtest_list_tests"]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write(proc.stdout or "")
        f.write(proc.stderr or "")
    return parse_gtest_list(proc.stdout or "")


# GitHub #264. A failure message that means the PROCESS FAULTED, not that an
# assertion failed. googletest reports a caught structured exception as
# "SEH exception with code 0xc0000005 thrown in the test body"; the others cover
# POSIX signals and the wording used when gtest is not catching.
#
# The distinction is a KIND, not a grade, and that is the whole point: an
# assertion failure and an access violation used to arrive by the same path and
# get the same remedy.
_CRASH_MESSAGE_RE = re.compile(
    r"SEH exception with code 0x[0-9a-fA-F]+"
    r"|\bAccess violation\b"
    r"|\bSegmentation fault\b"
    r"|terminated by signal\b"
    r"|\bsignal SIG[A-Z]+",
    re.IGNORECASE)


def crashed_test_ids(root: ET.Element | None) -> set:
    """Ids whose failure/error text says the process faulted.

    Read from the ORIGINAL run's XML, not from a rerun: the rerun is precisely
    where the evidence disappears.
    """
    out: set = set()
    if root is None:
        return out
    for tc in root.iter("testcase"):
        for node in list(tc.findall("failure")) + list(tc.findall("error")):
            text = " ".join(filter(None, [node.get("message") or "",
                                          node.text or ""]))
            if _CRASH_MESSAGE_RE.search(text):
                suite = tc.get("classname") or ""
                name = tc.get("name") or ""
                out.add(f"{suite}.{name}" if suite else name)
    return out


def _rerun_failures(exe: str, out: Path, failed: list[str], reruns: int,
                    unhealable: set | None = None) -> tuple[list[str], list[str]]:
    """Re-run each failed test in a FRESH process; return (healed, unresolved).

    GitHub #264 -- a CRASH CANNOT BE HEALED HERE, whatever the rerun does.

    This function re-runs a failed test ALONE and treats passing as proof of
    flakiness. That is sound for order-independent flakiness and exactly
    inverted for an order-dependent crash: isolation is what makes both
    disappear, so the single diagnostic performed is the one guaranteed to
    misclassify memory corruption. A `--shards 8` run healed two access
    violations this way and printed CLEAN.

    So ids in `unhealable` are still re-run -- "it passes alone" is a real and
    useful diagnostic -- but the result is reported, never applied. Passing
    alone is that defect's SIGNATURE, not its refutation.
    """
    unhealable = unhealable or set()
    healed_tests: list[str] = []
    unresolved: list[str] = []
    for tid in failed:
        healed = False
        crash = tid in unhealable
        for attempt in range(1, reruns + 1):
            safe = tid.replace("/", "_")
            rerun_xml = out / f"rerun_{safe}_{attempt}.xml"
            rc = _run(exe, rerun_xml, out / f"rerun_{safe}_{attempt}.log", tid)
            rfailed = _failed_testcases(rerun_xml)
            rt = _totals(rerun_xml)
            if rt["tests"] >= 1 and not rfailed:
                if crash:
                    print(f"[gtest]   {tid}: CRASHED in the run and PASSES "
                          f"alone on rerun {attempt}. NOT healed -- that is "
                          f"the signature of an order-dependent crash, not "
                          f"evidence of a flake (GitHub #264). Reproduce with "
                          f"the shard's flagfile, not the test alone.",
                          flush=True)
                elif _is_abnormal_exit(rc):
                    # GitHub #288, the FIFTH path. `rc` was already captured
                    # above and printed on the still-failing branch, but the
                    # HEALED decision never consulted it -- so a rerun whose
                    # test passed and whose PROCESS then died in teardown was
                    # recorded as a transient flake. That launders a crash into
                    # a success, which is strictly worse than missing it: the
                    # run returns 0 AND the ledger asserts the test is fine.
                    #
                    # The `crash` branch above does not cover this. It keys on
                    # `tid in unhealable`, i.e. tests the INITIAL run saw
                    # faulting. A crash that happens only during the rerun of an
                    # ordinary assertion failure was never in that set.
                    #
                    # This function's own docstring already states the rule --
                    # "a CRASH CANNOT BE HEALED HERE, whatever the rerun does",
                    # written after a --shards 8 run healed two access
                    # violations and printed CLEAN. The rule was right; it was
                    # only enforced for one of the two ways a crash can arrive.
                    print(f"[gtest]   {tid}: passed alone on rerun {attempt} "
                          f"but the PROCESS CRASHED afterwards (rc={rc}). NOT "
                          f"healed -- a green report from a process that died "
                          f"is not evidence of a flake. See _is_abnormal_exit().",
                          flush=True)
                else:
                    print(f"[gtest]   {tid}: PASSED on fresh rerun {attempt} "
                          f"(transient flake).", flush=True)
                    healed = True
                break
            print(f"[gtest]   {tid}: still failing on rerun {attempt} "
                  f"(rc={rc}).", flush=True)
        (healed_tests if healed else unresolved).append(tid)
    return healed_tests, unresolved


def apply_healed_to_aggregate(agg_root: ET.Element,
                              healed: list[str]) -> int:
    """Make the merged aggregate agree with the run's actual final state (#158).

    The aggregate is written BEFORE reruns happen, so a test that failed and then
    passed on a fresh rerun stayed recorded as a failure forever. The harness
    then printed "CLEAN" while the artifact it had just produced said
    ``failures="1"``. Everything downstream reads the artifact, not the console,
    so a single healed flake made the whole run unusable as green evidence --
    including for the pipeline gate hook, which froze a phase for exactly this
    reason.

    This does NOT hide the flake, and that distinction is the whole design:

    * the ``<failure>`` element is replaced by a ``<system-out>`` note saying the
      test healed on a fresh rerun, so the history is still in the XML;
    * the testcase is marked ``healed-on-rerun="true"``, so a healed test stays
      machine-distinguishable from one that passed first time;
    * ``healed_flakes.json`` and ``tools/gate/check_flaky_quarantine.py`` are
      untouched and remain the enforcement -- an unregistered healed test still
      fails the build.

    Returns the number of testcases actually rewritten, so the caller can report
    a mismatch rather than assume the rewrite landed.
    """
    if not healed:
        return 0
    wanted = set(healed)
    rewritten = 0

    for suite in agg_root.iter("testsuite"):
        suite_failures = 0
        suite_errors = 0
        for case in suite.findall("testcase"):
            ident = f"{case.get('classname', '')}.{case.get('name', '')}"
            if ident not in wanted:
                continue
            removed_f = 0
            removed_e = 0
            # GitHub #264: KEEP the original failure text. Clearing the element
            # without it destroys the only record of WHY the test failed, and
            # that is the difference between an assertion and an access
            # violation. Auditing the three healed incidents in committed
            # evidence was only possible because two of them happened to be
            # quoted in a notes.md by hand; from the artifact alone they were
            # indistinguishable. "Marked healed rather than hidden" has to
            # include the reason or the artifact cannot answer the question
            # anyone will actually ask of it later.
            original: list = []
            for child in list(case):
                if child.tag == "failure":
                    removed_f += 1
                    original.append(("failure", (child.get("message")
                                                 or child.text or "").strip()))
                    case.remove(child)
                elif child.tag == "error":
                    removed_e += 1
                    original.append(("error", (child.get("message")
                                               or child.text or "").strip()))
                    case.remove(child)
            if not (removed_f or removed_e):
                # Nothing to clear -- do not touch the counters, or a repeated
                # invocation would drive them negative.
                continue
            note = ET.SubElement(case, "system-out")
            detail = "".join(
                "\n  ORIGINAL %s: %s" % (kind, (text or "(no message)")[:2000])
                for kind, text in original)
            note.text = (f"HEALED ON RERUN: this test failed in the sharded run "
                         f"and PASSED in a fresh process. Recorded in "
                         f"healed_flakes.json; must be registered in "
                         f"tools/gate/flaky_quarantine.json." + detail)
            case.set("healed-on-rerun", "true")
            suite_failures += removed_f
            suite_errors += removed_e
            rewritten += 1

        if suite_failures or suite_errors:
            for attr, delta in (("failures", suite_failures),
                                ("errors", suite_errors)):
                if not delta:
                    continue
                cur = int(suite.get(attr, "0") or "0")
                suite.set(attr, str(max(0, cur - delta)))

    # Root totals last, from the same counts, so the root and the suites cannot
    # drift apart.
    if rewritten:
        for attr in ("failures", "errors"):
            total = sum(int(s.get(attr, "0") or "0")
                        for s in agg_root.iter("testsuite"))
            agg_root.set(attr, str(total))

    return rewritten


def _write_healed_ledger(out: Path, healed: list[str], unresolved: list[str],
                         never_ran: set[str] | None = None) -> None:
    """R-37: record healed-on-rerun tests for the flaky-quarantine gate.

    F70(b) — A SHARD THAT DIED WITHOUT XML MUST NOT FEED ITS PARTITION IN HERE.

    When a shard exits without emitting JUnit XML, ``_synthesize_error_cases``
    marks EVERY test assigned to it as an error so the work does not silently
    vanish from the aggregate. Those tests then look like failures, get re-run
    individually, pass -- because they never ran in the first place -- and were
    recorded as "healed on rerun".

    Measured once already: one dead shard put **348** healthy, never-executed
    tests into this file. That is not a legibility problem, it is contamination
    of a record that FEEDS A GATE. ``check_flaky_quarantine.py`` fails when a
    healed test is absent from ``flaky_quarantine.json``, so the ledger left
    that gate with two bad exits and no good one: fail with 348 untracked
    heals, or "fix" it by registering 348 healthy tests as known flakes, which
    destroys the registry's entire discriminating value -- its purpose is to
    stop a genuinely degrading test hiding behind self-healing, and it cannot
    do that job with 349 entries of which 348 are fictional.

    So never-run ids are excluded from ``healed`` and recorded separately, as
    what they are. They are NOT dropped from ``unresolved``: a never-run test
    that also fails its rerun is a real failure and hiding it would be a
    different defect. The dead shard is reported as ONE incident, because that
    is what it is -- the old console line read as 348 distinct problems.
    """
    never_ran = never_ran or set()
    contaminating = sorted((set(healed) | set(unresolved)) & never_ran)
    healed = [t for t in healed if t not in never_ran]

    if contaminating:
        print(f"[gtest] {len(contaminating)} test(s) belonged to a shard that "
              f"died without emitting XML. They never executed, so a passing "
              f"rerun is not evidence of flakiness: EXCLUDED from the healed "
              f"ledger (F70b).", flush=True)

    healed_path = out / "healed_flakes.json"
    healed_path.write_text(
        json.dumps({"healed": sorted(healed), "unresolved": sorted(unresolved),
                    "not_run_due_to_dead_shard": contaminating},
                   indent=2),
        encoding="utf-8")
    print(f"[gtest] healed-on-rerun ledger -> {healed_path} "
          f"({len(healed)} healed)", flush=True)


# --------------------------------------------------------------------------- #
# Execution modes.
# --------------------------------------------------------------------------- #


def run_single_process(exe: str, out: Path, gtest_filter: str | None,
                       reruns: int) -> int:
    """Historical mode: whole suite in one process, then rerun failures."""
    full_xml = out / "gtest_full.xml"
    print(f"[gtest] full run -> {full_xml}", flush=True)
    # GitHub #288. The return code is NOT optional here, and discarding it was
    # this path's share of the defect that issue describes.
    #
    # The sharded path has consulted it since phase-61 (see the two
    # `teardown_crash = (not crashed) and _is_abnormal_exit(rc)` sites), but
    # THIS function -- the DEFAULT mode, because `--shards` defaults to 1 --
    # threw the code away and decided everything from the XML. So both crash
    # shapes reached "CLEAN on first run" and exit 0:
    #
    #   * run every test, write a COMPLETE and GREEN report, then die during
    #     teardown. Nothing in the results betrays it; only the exit code does.
    #     That is the real MuPDF-function-local-static access violation
    #     `_is_abnormal_exit`'s docstring records, invisible again on the path
    #     most people actually run.
    #   * crash before writing any XML at all, which left `_totals` reading an
    #     absent/empty report as zeros, so `failed` was empty and the run
    #     reported CLEAN with tests=0.
    #
    # Found by p96 phase-4's `validator` gate, which FAILED the phase after I
    # had already reported #288 as addressed on the strength of the sharded
    # path alone. Three covered paths were not proof of a fourth.
    rc = _run(exe, full_xml, out / "gtest_full.log", gtest_filter)

    root = _parse_root(full_xml)
    crashed = _is_crashed(root)
    teardown_crash = (not crashed) and _is_abnormal_exit(rc)

    if crashed:
        print(f"[gtest] the run CRASHED (no/empty XML), rc={rc}. Nothing was "
              f"reported, so NOTHING passed -- this is not a clean run.",
              flush=True)
        return 1

    totals = _totals(full_xml)
    failed = _failed_testcases(full_xml)
    print(f"[gtest] tests={totals['tests']} failures={totals['failures']} "
          f"errors={totals['errors']} skipped={totals['skipped']}", flush=True)
    report_external_corpus_skips(full_xml)

    if teardown_crash:
        print(f"[gtest] TESTS OK BUT PROCESS CRASHED AFTER THE LAST TEST "
              f"(rc={rc}). The report is green and complete; the process still "
              f"died. A rerun cannot heal this because no assertion failed -- "
              f"see _is_abnormal_exit(). Reporting RED.", flush=True)
        return 1

    if not failed:
        print("[gtest] CLEAN on first run.", flush=True)
        return 0

    # GitHub #264: same rule on the single-process path -- a crash is a kind of
    # failure, not a grade of one, and no rerun clears it.
    crashers = crashed_test_ids(_parse_root(full_xml))
    if crashers:
        print(f"[gtest] {len(crashers)} of these FAULTED the process "
              f"(access violation / signal). They cannot be healed:",
              flush=True)
        for t in sorted(crashers):
            print(f"[gtest]     {t}", flush=True)

    print(f"[gtest] {len(failed)} failed test(s); re-running each in a FRESH "
          f"process (transient-flake mitigation):", flush=True)
    healed, unresolved = _rerun_failures(exe, out, failed, reruns, crashers)
    _write_healed_ledger(out, healed, unresolved)

    # #158, single-process path: same rewrite as the sharded path, for the same
    # reason -- the artifact must not contradict the verdict printed below.
    if healed:
        _single_root = _parse_root(full_xml)
        if _single_root is not None:
            _n = apply_healed_to_aggregate(_single_root, healed)
            if _n:
                ET.ElementTree(_single_root).write(str(full_xml),
                                                   encoding="utf-8",
                                                   xml_declaration=True)
                print(f"[gtest] aggregate updated: {_n} healed test(s) cleared "
                      f"in {full_xml} (marked healed-on-rerun, NOT hidden)",
                      flush=True)
            if _n != len(healed):
                print(f"[gtest] WARNING: {len(healed)} test(s) healed but only "
                      f"{_n} were found in the aggregate; the XML may still be "
                      f"RED.", flush=True)

    if unresolved:
        # GitHub #264: "failed every rerun" is not true of a crash that passes
        # alone, and saying so is how the old message invited the wrong
        # conclusion. Report the two kinds separately.
        crashed_unresolved = [t for t in unresolved if t in crashers]
        plain_unresolved = [t for t in unresolved if t not in crashers]
        if crashed_unresolved:
            print(f"[gtest] PROCESS FAULTS (unhealable, GitHub #264): "
                  f"{crashed_unresolved}", flush=True)
            print("[gtest]   These faulted the process during the run. Some "
                  "may pass when re-run alone -- that is the signature of an "
                  "order-dependent crash, not evidence against it. Reproduce "
                  "with the shard's .flagfile in this output directory.",
                  flush=True)
        if plain_unresolved:
            print(f"[gtest] GENUINE FAILURES (failed every rerun): "
                  f"{plain_unresolved}", flush=True)
        return 1
    print(f"[gtest] all {len(failed)} failure(s) were transient flakes that "
          f"passed on a fresh rerun. CLEAN.", flush=True)
    return 0


def run_sharded(exe: str, out: Path, shards: int, gtest_filter: str | None,
                reruns: int, test_list_file: Path | None) -> int:
    """Canonical Windows harness: N sequential fresh-process shards + merge."""
    # 1. Enumerate ONCE (from a cache file if provided, else --gtest_list_tests).
    all_ids = _list_tests(exe, gtest_filter, test_list_file,
                          out / "gtest_list.log")
    if not all_ids:
        print("[gtest] ERROR: enumerated ZERO tests — cannot shard. Check the "
              "exe / --test-list-file / --gtest-filter.", flush=True)
        return 1
    print(f"[gtest] enumerated {len(all_ids)} test(s); partitioning into "
          f"{shards} shard(s).", flush=True)

    # 1b. GitHub #247. Each shard's filter goes in a FLAGFILE, not on argv, so
    #     the shard count is no longer bounded by the 32,767-character Windows
    #     command line. This replaces a refusal: --shards 8 used to need a
    #     43,232-character filter and was rejected up front.
    #
    #     The support is MEASURED before anything launches, because the failure
    #     is silent: a binary that ignores --gtest_flagfile applies no filter at
    #     all, so every shard runs the WHOLE suite and the aggregate counts each
    #     test once per shard. That is a plausible-looking, entirely wrong run.
    #     If the probe fails we fall back to the argv filter and re-impose the
    #     command-line budget, which is the behaviour this replaced.
    use_flagfile, detail = flagfile_is_honoured(exe, out)
    if use_flagfile:
        print(f"[gtest] per-shard filter via --gtest_flagfile ({detail}).",
              flush=True)
    else:
        print(f"[gtest] --gtest_flagfile NOT usable: {detail}", flush=True)
        print("[gtest]   Falling back to the argv filter, which IS bounded by "
              "the Windows command line.", flush=True)
        ok, worst, min_viable = shard_filter_budget(all_ids, shards)
        if not ok:
            print(f"[gtest] REFUSING TO RUN: --shards {shards} needs a "
                  f"per-shard --gtest_filter of up to {worst:,} characters, "
                  f"and Windows caps a process command line at "
                  f"{WINDOWS_CMDLINE_LIMIT:,} (minus ~{_SHARD_ARGV_OVERHEAD:,} "
                  f"for the rest of the argv).", flush=True)
            print(f"[gtest]   This is a command-line LENGTH limit, not a test "
                  f"failure: every shard would die with 'WinError 206: The "
                  f"filename or extension is too long' before running "
                  f"anything.", flush=True)
            print(f"[gtest]   Use --shards {min_viable} or more "
                  f"({len(all_ids):,} tests at this size), or use a build "
                  f"whose googletest supports --gtest_flagfile.", flush=True)
            return 1

    # 2. Partition deterministically.
    buckets = partition_tests(all_ids, shards)

    # 3. Run each shard SEQUENTIALLY in its own fresh process.
    shard_results: list[tuple[list[str], ET.Element | None]] = []
    crashed_shards: list[int] = []
    teardown_crashed_shards: list[tuple[int, int]] = []
    for i, bucket in enumerate(buckets):
        if not bucket:
            shard_results.append((bucket, None))
            continue
        shard_xml = out / f"gtest_shard_{i}.xml"
        if use_flagfile:
            ff = write_flagfile(out / f"gtest_shard_{i}.flagfile",
                                shard_filter(bucket))
            rc = _run(exe, shard_xml, out / f"gtest_shard_{i}.log",
                      None, flagfile=ff)
        else:
            rc = _run(exe, shard_xml, out / f"gtest_shard_{i}.log",
                      shard_filter(bucket))
        root = _parse_root(shard_xml)
        crashed = _is_crashed(root)
        # A shard can finish every test, write a green XML, and STILL die on
        # the way out. That is not "ok" and must not read as it.
        teardown_crash = (not crashed) and _is_abnormal_exit(rc)
        if crashed:
            status = "CRASHED (no/empty XML)"
            crashed_shards.append(i)
        elif teardown_crash:
            status = "TESTS OK BUT PROCESS CRASHED AFTER THE LAST TEST"
            teardown_crashed_shards.append((i, rc))
        else:
            status = "ok"
        print(f"[gtest]   shard {i}: {len(bucket)} test(s), rc={rc} -> {status}",
              flush=True)
        shard_results.append((bucket, root))

    # 4. Merge into a GAP-A15-compliant aggregate.
    agg_root, totals, per_shard = aggregate_shards(shard_results)
    # GitHub #300. A teardown crash leaves nothing red in the report, so the
    # classification is injected into the artifact itself before it is written
    # — otherwise the XML a gate reads is indistinguishable from a clean run's.
    # Done here, before the first write, so the file on disk is never briefly
    # green-and-wrong.
    n_teardown = inject_teardown_crash_evidence(agg_root, teardown_crashed_shards)
    if n_teardown:
        totals["tests"] = totals.get("tests", 0) + n_teardown
        totals["errors"] = totals.get("errors", 0) + n_teardown
        print(f"[gtest] {n_teardown} teardown crash(es) recorded in the "
              f"aggregate as ProcessTeardown error case(s), so the staged XML "
              f"carries the evidence on its own (GitHub #300).", flush=True)
    full_xml = out / "gtest_full.xml"
    ET.ElementTree(agg_root).write(str(full_xml), encoding="utf-8",
                                   xml_declaration=True)
    passed = totals["tests"] - totals["failures"] - totals["errors"] - totals["skipped"]
    print(f"[gtest] aggregate -> {full_xml}", flush=True)
    print(f"[gtest] tests={totals['tests']} passed={passed} "
          f"failures={totals['failures']} errors={totals['errors']} "
          f"skipped={totals['skipped']}", flush=True)
    report_external_corpus_skips(full_xml)
    if crashed_shards:
        print(f"[gtest] CRASHED shard(s): {crashed_shards} — their assigned "
              f"tests are recorded as ERRORS (aggregate is RED).", flush=True)

    # 5. Rerun genuine failures/errors individually in fresh processes.
    #    (A crashed shard's tests are included here so a spurious crash can heal;
    #    a real failure stays red.)
    if teardown_crashed_shards:
        for idx, rc in teardown_crashed_shards:
            print(f"[gtest] TEARDOWN CRASH: shard {idx} ran and PASSED every "
                  f"assigned test, then exited rc={rc}. The XML is green and "
                  f"the process died anyway — look at static/global destruction "
                  f"order, not at the assertions.", flush=True)
        print("[gtest] NOT CLEAN: a teardown crash is a real defect that no "
              "assertion can see. Fix it or explain it; do not read the green "
              "totals above as a passing run.", flush=True)

    failed = _failed_ids_from_root(agg_root)
    if not failed:
        if teardown_crashed_shards:
            return 1
        # GitHub #288, the same rule at the OTHER exit. Normally a dead shard
        # cannot reach here, because _synthesize_error_cases marks every test
        # it was assigned as an error and `failed` is then non-empty. It can if
        # the dead shard's bucket was EMPTY -- and then "no failures" is true
        # and "clean" is still false. Guarded rather than reasoned about,
        # because the cost is one condition and the failure mode is a green
        # verdict over a crashed process.
        if crashed_shards:
            print(f"[gtest] NOT CLEAN: shard(s) {sorted(crashed_shards)} died "
                  f"without emitting XML. No test reported a failure, which is "
                  f"not the same as nothing having gone wrong.", flush=True)
            return 1
        print("[gtest] CLEAN across all shards (no failures/errors).", flush=True)
        return 0

    # GitHub #264: classify from the ORIGINAL aggregate, before any rerun. A
    # crash is unhealable no matter what the isolated rerun does.
    crashers = crashed_test_ids(agg_root)
    if crashers:
        print(f"[gtest] {len(crashers)} of these FAULTED the process "
              f"(access violation / signal), not merely failed an assertion. "
              f"They cannot be healed by a rerun:", flush=True)
        for t in sorted(crashers):
            print(f"[gtest]     {t}", flush=True)

    print(f"[gtest] {len(failed)} failed/errored test(s); re-running each in a "
          f"FRESH process:", flush=True)
    healed, unresolved = _rerun_failures(exe, out, failed, reruns, crashers)
    # F70(b): every id assigned to a shard that produced no XML never executed,
    # so a passing rerun is not evidence of flakiness. They are kept out of the
    # healed ledger and recorded as what they are -- see _write_healed_ledger.
    never_ran = {t for i in crashed_shards for t in buckets[i]}
    _write_healed_ledger(out, healed, unresolved, never_ran)

    # #158: rewrite the aggregate so it reflects the reruns it was written
    # before. Only tests that genuinely healed are cleared -- `unresolved` and
    # `never_ran` are deliberately excluded, so a real failure and a test that
    # never executed both stay RED.
    healed_for_xml = [t for t in healed if t not in never_ran]
    n = apply_healed_to_aggregate(agg_root, healed_for_xml)
    if n:
        ET.ElementTree(agg_root).write(str(full_xml), encoding="utf-8",
                                       xml_declaration=True)
        print(f"[gtest] aggregate updated: {n} healed test(s) cleared in "
              f"{full_xml} (marked healed-on-rerun, NOT hidden)", flush=True)
    if n != len(healed_for_xml):
        # A silent partial rewrite would leave the artifact disagreeing with the
        # verdict again -- the exact defect this fixes.
        print(f"[gtest] WARNING: {len(healed_for_xml)} test(s) healed but only "
              f"{n} were found in the aggregate; the XML may still be RED.",
              flush=True)

    if unresolved:
        # GitHub #264: "failed every rerun" is not true of a crash that passes
        # alone, and saying so is how the old message invited the wrong
        # conclusion. Report the two kinds separately.
        crashed_unresolved = [t for t in unresolved if t in crashers]
        plain_unresolved = [t for t in unresolved if t not in crashers]
        if crashed_unresolved:
            print(f"[gtest] PROCESS FAULTS (unhealable, GitHub #264): "
                  f"{crashed_unresolved}", flush=True)
            print("[gtest]   These faulted the process during the run. Some "
                  "may pass when re-run alone -- that is the signature of an "
                  "order-dependent crash, not evidence against it. Reproduce "
                  "with the shard's .flagfile in this output directory.",
                  flush=True)
        if plain_unresolved:
            print(f"[gtest] GENUINE FAILURES (failed every rerun): "
                  f"{plain_unresolved}", flush=True)
        return 1
    if teardown_crashed_shards:
        print(f"[gtest] all {len(failed)} failure(s)/error(s) healed on a fresh "
              f"rerun, but a TEARDOWN CRASH stands. NOT CLEAN.", flush=True)
        return 1
    # GitHub #288. THE BRANCH THAT WAS MISSING.
    #
    # A shard that died mid-run lands in none of the branches above:
    # `unresolved` is empty because every one of its tests PASSED when re-run
    # alone (they never ran in the first place), and `teardown_crashed_shards`
    # is empty because the shard died DURING the run rather than on the way
    # out. Control therefore fell through to the unconditional CLEAN below.
    #
    # Measured on a 16-shard run 2026-10-07, when TileRendererTest.CancelPage
    # (GitHub #287) killed shard 7: the merged XML said `errors=408`, the
    # ledger listed 408 names under `not_run_due_to_dead_shard`, and the
    # console still printed "all 408 failure(s)/error(s) healed on a fresh
    # rerun. CLEAN." with exit code 0. The data layer was honest; only the
    # verdict was not.
    #
    # `never_ran` is already computed above for the ledger and for the
    # aggregate rewrite. It was simply never consulted here -- which is why
    # F70(b) stopped those tests being CALLED healed without stopping the run
    # being CALLED clean.
    #
    # A passing isolated rerun is NOT evidence for these ids. They did not run
    # in situ, and GitHub #264 already establishes that an order-dependent
    # crash passes when re-run alone -- so "it passes by itself" is the
    # signature of the defect, not an argument against it.
    if never_ran:
        print(f"[gtest] NOT CLEAN: {len(crashed_shards)} shard(s) "
              f"{sorted(crashed_shards)} died mid-run, so {len(never_ran)} "
              f"test(s) never executed. They passed when re-run ALONE, which "
              f"is not evidence they pass in situ -- an order-dependent crash "
              f"has exactly that signature (GitHub #264). Reproduce with the "
              f"dead shard's .flagfile in this output directory.", flush=True)
        return 1
    print(f"[gtest] all {len(failed)} failure(s)/error(s) healed on a fresh "
          f"rerun. CLEAN.", flush=True)
    return 0


# ---------------------------------------------------------------------------
# BINARY STALENESS PREFLIGHT — operator directive, 2026-08-15.
#
# A test process that cannot tell whether it is running current code cannot
# support either half of the rule this exists for: that after a fix, every
# LATER test runs the new binary, and every EARLIER test the change could have
# touched is invalidated and must be re-run.
#
# The instance that prompted it: local runs built only the `pdfe_tests` target,
# so `pdfe.exe` — which ShellAndInstanceLaunchTest, DllDependencyTest,
# ProcessMitigations and the preflight cases LAUNCH as a child process — sat
# three days stale, reporting green for code that was not there. Invisible to
# the test-runner gate, whose freshness check compares the result XML against
# SOURCE mtimes and never looks at a second binary.
#
# Duplicated on purpose with tests/test_binary_staleness_guard.cpp. The gtest
# holds however the suite is invoked; this one fails the run BEFORE ~25 minutes
# are spent producing results that are void. Neither subsumes the other.
#
# Ordering is a NECESSARY condition, not a sufficient one: it does not prove the
# binary was built from those sources. Claiming more would be the unverified
# confident assertion this project keeps recording.
# ---------------------------------------------------------------------------
_SRC_DIRS = ("src", "tests", "include")
_SRC_EXTS = (".cpp", ".cxx", ".cc", ".h", ".hpp", ".hxx", ".inl")
#: Path components that are scratch, never compiled inputs. See _newest_source.
_SCRATCH_DIRS = frozenset({"_tmp", "__pycache__", ".pytest_cache"})


def module_of(test_id: str) -> str:
    """The MODULE a gtest id belongs to: the suite name before the first '.'.

    Parametrised ids look like `Suite/Case.Name/0`; the module is still the part
    before the first '.', so `Foo/Bar.Baz/0` -> `Foo/Bar`. Deliberately not
    splitting on '/' as well: `Foo/Bar` is one typed/parametrised suite, and
    treating its instantiations as separate modules would let `--modules Foo`
    silently select nothing.
    """
    return test_id.split(".", 1)[0]


def select_modules(test_ids: list[str], include: list[str],
                   exclude: list[str]) -> list[str]:
    """Filter ids by module name, preserving order.

    `include` empty means "all modules". Exclusion is applied AFTER inclusion,
    so `--modules A,B --exclude-modules B` is A -- the reading that makes an
    exclude flag useful for narrowing a selection rather than fighting it.
    """
    inc = set(include)
    exc = set(exclude)
    out = []
    for tid in test_ids:
        m = module_of(tid)
        if inc and m not in inc:
            continue
        if m in exc:
            continue
        out.append(tid)
    return out


def _manifest_path(out: Path) -> Path:
    return out / "shard_manifest.json"


def _manifest_for(shards: int, ids: list[str], include: list[str],
                  exclude: list[str], gtest_filter: str | None) -> dict:
    return {
        "shards": shards,
        "test_count": len(ids),
        "modules": sorted(include),
        "exclude_modules": sorted(exclude),
        "gtest_filter": gtest_filter or "",
    }


def _check_manifest(out: Path, current: dict) -> list[str]:
    """Refuse to merge across a DIFFERENT partition.

    The partition is a function of (enumeration, module selection, shard count).
    Merging shard XMLs produced under one partition using the assignments of
    another silently mis-attributes results: shard 2's XML would be checked
    against the tests shard 2 owns under the NEW partition, which are not the
    tests it ran. Nothing about that is visible in the totals, which is what
    makes it worth failing on rather than warning about.
    """
    path = _manifest_path(out)
    if not path.exists():
        return []          # nothing recorded (e.g. an all-in-one run); no claim
    try:
        with open(path, "r", encoding="utf-8") as fh:
            prev = json.load(fh)
    except (OSError, ValueError) as exc:
        return [f"shard_manifest.json is unreadable ({exc}); refusing to merge "
                f"against an unknown partition."]
    diffs = [f"{k}: shards wrote {prev.get(k)!r}, this merge assumes "
             f"{current.get(k)!r}" for k in sorted(current) if prev.get(k) != current.get(k)]
    if diffs:
        return ["the shard XMLs were produced under a DIFFERENT partition, so "
                "merging them would attribute results to the wrong tests:"] + \
               ["    " + d for d in diffs]
    return []


def _shard_status_path(out: Path, shard: int) -> Path:
    return out / f"gtest_shard_{shard}.status.json"


def _write_shard_status(out: Path, shard: int, rc: int, crashed: bool,
                        teardown_crash: bool) -> None:
    """Persist a shard's EXIT-CODE verdict beside its XML (GitHub #288).

    A JUnit XML records assertions. It cannot record that the process which
    wrote it then died, because by definition nothing was left to write it
    down. `run_one_shard` computes that classification correctly and then --
    before this existed -- returned it as a process exit code and nothing else,
    which a later `--merge-only` in a DIFFERENT process cannot observe.

    So the crash classification is written to disk next to the report it
    qualifies. This is deliberately a separate file rather than an attribute
    injected into the XML: the XML is the gtest contract that every downstream
    gate parses, and adding non-standard attributes to it would be a second,
    quieter compatibility problem.
    """
    payload = {
        "_what": ("Exit-code verdict for this shard, written by run_one_shard "
                  "and required by merge_shards. GitHub #288: a green XML from "
                  "a process that crashed on the way out is not a pass, and "
                  "the XML alone cannot say so."),
        "shard": shard,
        "rc": rc,
        "crashed": bool(crashed),
        "teardown_crash": bool(teardown_crash),
    }
    with open(_shard_status_path(out, shard), "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)


def _read_shard_status(out: Path, shard: int) -> dict | None:
    """Return a shard's recorded verdict, or None if it cannot be trusted.

    Unreadable is treated exactly like absent. A status file that will not
    parse is not evidence of health, and the caller fails closed on None.
    """
    path = _shard_status_path(out, shard)
    if not path.exists():
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def _buckets_for(exe: str, out: Path, shards: int, gtest_filter: str | None,
                 test_list_file: Path | None,
                 include: list[str] | None = None,
                 exclude: list[str] | None = None) -> list[list[str]]:
    """The same partition run_sharded would compute, derived the same way.

    Both --shard and --merge-only depend on this being reproducible across
    SEPARATE PROCESS INVOCATIONS -- that is the whole basis of running shards
    independently and merging later. partition_tests is pure round-robin over
    the enumeration order, so the only way it can drift is if the enumeration
    drifts; pass --test-list-file to pin that too.
    """
    all_ids = _list_tests(exe, gtest_filter, test_list_file,
                          out / "gtest_list.log")
    if not all_ids:
        return []
    if include or exclude:
        before = len(all_ids)
        all_ids = select_modules(all_ids, include or [], exclude or [])
        print(f"[gtest] module selection: {before} -> {len(all_ids)} test(s)",
              flush=True)
    return partition_tests(all_ids, shards)


def run_one_shard(exe: str, out: Path, shards: int, shard: int,
                  gtest_filter: str | None,
                  test_list_file: Path | None,
                  include: list[str] | None = None,
                  exclude: list[str] | None = None) -> int:
    """Run exactly ONE shard and leave its XML for a later --merge-only.

    WHY THIS EXISTS. --shards N partitions but cannot be resumed: a single red
    shard means re-running all N, which on this suite is the difference between
    minutes and hours, and is a large part of why the lane hits the 720-minute
    cap. With --shard i/N the operator re-runs only the shard that failed and
    merges its new XML into the existing set.

    Deliberately does NOT aggregate or rerun failures. Aggregation over a
    partial set would write a gtest_full.xml with totals lower than the suite,
    and that file is what every downstream gate reads -- a quietly short
    aggregate is exactly the false-green this harness exists to prevent.
    """
    if not 0 <= shard < shards:
        print(f"[gtest] ERROR: --shard {shard} is out of range for "
              f"--shards {shards} (valid 0..{shards - 1}).", flush=True)
        return 2
    buckets = _buckets_for(exe, out, shards, gtest_filter, test_list_file,
                           include, exclude)
    if not buckets:
        print("[gtest] ERROR: enumerated ZERO tests — cannot shard.", flush=True)
        return 1
    # Record the partition this XML was produced under, so a later --merge-only
    # cannot silently combine shards from two different partitions.
    manifest = _manifest_for(shards, [t for b in buckets for t in b],
                             include or [], exclude or [], gtest_filter)
    with open(_manifest_path(out), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
    bucket = buckets[shard]
    if not bucket:
        print(f"[gtest] shard {shard}/{shards}: EMPTY (more shards than tests). "
              f"Nothing to run.", flush=True)
        return 0
    shard_xml = out / f"gtest_shard_{shard}.xml"
    print(f"[gtest] shard {shard}/{shards}: {len(bucket)} test(s) -> {shard_xml}",
          flush=True)
    rc = _run(exe, shard_xml, out / f"gtest_shard_{shard}.log",
              shard_filter(bucket))
    root = _parse_root(shard_xml)
    crashed = _is_crashed(root)
    teardown_crash = (not crashed) and _is_abnormal_exit(rc)
    status = ("CRASHED (no/empty XML)" if crashed
              else "TESTS OK BUT PROCESS CRASHED AFTER THE LAST TEST"
              if teardown_crash else "ok")
    print(f"[gtest]   shard {shard}: rc={rc} -> {status}", flush=True)
    # GitHub #288, the SIXTH site. Everything above lives in THIS process, and
    # the documented use pattern for this function is a later, SEPARATE
    # --merge-only invocation that sees only what is on disk. `_run()` persists
    # the exit code nowhere, so a shard that ran every test, wrote a green XML
    # and then died in teardown left a perfect XML and no trace of the crash --
    # and the merge trusted the XML. The classification has to outlive the
    # process that computed it, so write it down next to the report it qualifies.
    _write_shard_status(out, shard, rc, crashed, teardown_crash)
    print(f"[gtest] shard XML written. Merge with: --merge-only --shards "
          f"{shards} --output-dir {out}", flush=True)
    if crashed or teardown_crash:
        return 1
    # GAP-A15: the verdict comes from the XML, never from the exit code.
    return 1 if _failed_ids_from_root(root) else 0


def merge_shards(exe: str, out: Path, shards: int, gtest_filter: str | None,
                 test_list_file: Path | None,
                 include: list[str] | None = None,
                 exclude: list[str] | None = None) -> int:
    """Aggregate already-written per-shard XMLs into gtest_full.xml.

    The point of the phase-25 criterion: a result set assembled from separately
    executed modules must be equivalent in totals to a single full run. That
    holds because the partition is reproducible (see _buckets_for) and because
    aggregation is the SAME aggregate_shards() the all-in-one path uses -- not a
    second implementation that could drift from it.

    A MISSING shard XML is NOT silently dropped. It is passed to
    aggregate_shards as a null root, which synthesises a fatal <error> for every
    test that shard owned, so the merged total still equals the full suite count
    and the aggregate is RED. A merge that quietly reported 5,500 of 5,896 tests
    all passing would be the worst possible failure of this harness.
    """
    buckets = _buckets_for(exe, out, shards, gtest_filter, test_list_file,
                           include, exclude)
    if not buckets:
        print("[gtest] ERROR: enumerated ZERO tests — cannot merge.", flush=True)
        return 1
    problems = _check_manifest(out, _manifest_for(
        shards, [t for b in buckets for t in b], include or [], exclude or [],
        gtest_filter))
    if problems:
        for line in problems:
            print(f"[gtest] {line}", flush=True)
        return 2
    shard_results: list[tuple[list[str], ET.Element | None]] = []
    missing: list[int] = []
    # GitHub #288, the SIXTH site. A present XML must come with exit-code
    # evidence; see _write_shard_status. Collected per shard, adjudicated below
    # so that one message can name every affected shard at once.
    crashed_shards: list[tuple[int, int]] = []
    unevidenced: list[int] = []
    for i, bucket in enumerate(buckets):
        if not bucket:
            shard_results.append((bucket, None))
            continue
        xml = out / f"gtest_shard_{i}.xml"
        root = _parse_root(xml) if xml.exists() else None
        if root is None:
            missing.append(i)
        else:
            # Only demand status for a shard whose XML is actually here. A
            # missing XML is already RED by the mechanism below, and asking for
            # its status too would report one fault as two.
            st = _read_shard_status(out, i)
            if st is None:
                unevidenced.append(i)
            elif st.get("crashed") or st.get("teardown_crash"):
                crashed_shards.append((i, st.get("rc")))
        shard_results.append((bucket, root))

    agg_root, totals, _per = aggregate_shards(shard_results)
    full_xml = out / "gtest_full.xml"
    ET.ElementTree(agg_root).write(str(full_xml), encoding="utf-8",
                                   xml_declaration=True)
    passed = totals["tests"] - totals["failures"] - totals["errors"] - totals["skipped"]
    print(f"[gtest] merged {shards - len(missing)}/{shards} shard XML(s) -> "
          f"{full_xml}", flush=True)
    print(f"[gtest] tests={totals['tests']} passed={passed} "
          f"failures={totals['failures']} errors={totals['errors']} "
          f"skipped={totals['skipped']}", flush=True)
    report_external_corpus_skips(full_xml)
    if missing:
        print(f"[gtest] MISSING shard XML(s): {missing} — every test they own is "
              f"recorded as an ERROR, so the merged total still equals the full "
              f"suite and the aggregate is RED. Re-run them with "
              f"--shard <i> --shards {shards}, then merge again.", flush=True)
        return 1
    # GitHub #288: the exit-code verdicts, which the XMLs cannot carry.
    if crashed_shards:
        for i, rc in crashed_shards:
            print(f"[gtest] shard {i} CRASHED (rc={rc}) — its XML may look "
                  f"complete and green, but the process that wrote it died. "
                  f"A report from a dead process is not a pass.", flush=True)
        print(f"[gtest] merge is RED on exit-code evidence from "
              f"{len(crashed_shards)} shard(s), independently of the "
              f"assertions above.", flush=True)
        return 1
    if unevidenced:
        print(f"[gtest] REFUSING TO MERGE: shard(s) {unevidenced} have an XML "
              f"but no gtest_shard_<i>.status.json beside it, so there is no "
              f"record of the exit code the shard process returned.",
              flush=True)
        print(f"[gtest] A JUnit XML records assertions; it cannot record that "
              f"the process which wrote it then crashed. Merging a bare XML "
              f"would silently grant it a clean bill of health -- which is "
              f"GitHub #288 across the --shard/--merge-only process boundary. "
              f"Re-run those shards with --shard <i> --shards {shards} using "
              f"this version of the runner, then merge again.", flush=True)
        return 1
    return 1 if _failed_ids_from_root(agg_root) else 0


def report_external_corpus_skips(xml_path: Path) -> None:
    """Say, by name, which oracles did NOT execute (GitHub #189).

    `test_PDFs/` is gitignored -- 332 MB of third-party documents, 1 of 64 files
    tracked -- so the oracles that resolve it GTEST_SKIP on every clean
    checkout, which is every CI run. A skip counts as green in the summary line
    above, in the JUnit XML, and to every gate that reads either. The skip
    REASONS are honestly worded; nothing was reading them.

    So the run says it out loud, next to the pass count, every time. This
    intentionally never changes the exit code -- an absent corpus is not a
    regression and must not train anyone to ignore a red -- but it removes
    "nobody could have known" as an explanation. The growth of the silent set
    IS gated, statically, by tools/gate/check_external_corpus_skips.py in the
    per-push fast-checks lane.
    """
    checker = (_repo_root() / "tools" / "gate" /
               "check_external_corpus_skips.py")
    if not checker.exists() or not Path(xml_path).exists():
        return
    try:
        sys.path.insert(0, str(checker.parent))
        import check_external_corpus_skips as _ecs  # noqa: E402
        accepted = _ecs.load_baseline(str(_ecs.DEFAULT_BASELINE))
        _ecs.report_xml(str(xml_path), accepted)
    except Exception as exc:                       # pragma: no cover
        # Never let the reporter break a run it only observes. But SAY so --
        # a reporter that fails silently is the same defect one layer up.
        print(f"[external-corpus] reporter failed ({exc!r}); corpus-gated "
              f"skips were NOT enumerated for this run.", flush=True)


def _repo_root() -> Path:
    # tests/scripts/run_gtest.py -> repo root is two parents up.
    return Path(__file__).resolve().parent.parent.parent


def _newest_source(root: Path, dirs=_SRC_DIRS):
    """(mtime, path) of the newest compilable source in `dirs`, or (None, None).

    EACH BINARY IS COMPARED AGAINST THE SOURCES *IT* IS BUILT FROM. pdfe.exe is
    not built from tests/, so a test-file edit must not accuse it of staleness.
    The first version of this preflight swept all three directories for both
    binaries and refused to run purely because a test file had been touched --
    an over-broad guard that fires on unrelated edits gets switched off, and a
    switched-off guard protects nothing.

        pdfe_tests.exe  <- src + include + tests
        pdfe.exe        <- src + include

    SCRATCH DIRECTORIES ARE EXCLUDED, for the same reason the sweep is split per
    binary. `tests/_tmp/` is where this repo's own Python harness tests write
    throwaway trees -- including files literally named `new.cpp` by a case that
    exists to check staleness detection. Running the gate self-tests and then the
    suite therefore refused to run at all:

        [gtest] STALE test binary: ...\\pdfe_tests.exe
        [gtest]   is 0.8h older than ...\\tests\\_tmp\\test_the_newest_source_is_the_0\\new.cpp

    Nothing was stale; a gitignored scratch file was being treated as a
    compilable input. An over-broad guard that fires on unrelated edits gets
    switched off, which is the failure this docstring already warns about one
    paragraph up -- this is the same defect in a second form.
    """
    newest_t, newest_p = None, None
    for d in dirs:
        base = root / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.suffix.lower() not in _SRC_EXTS or not p.is_file():
                continue
            # Relative to the swept base, NOT the absolute path. Matching
            # absolute parts excludes everything when the tree itself lives
            # under a directory of that name -- which is exactly what happens
            # under pytest, whose tmp area here IS tests/_tmp. The first version
            # of this exclusion swept up its own test fixture and reported "no
            # sources at all", turning a false STALE into a false CLEAN.
            try:
                rel_parts = p.relative_to(base).parts
            except ValueError:
                rel_parts = p.parts
            if any(part in _SCRATCH_DIRS for part in rel_parts):
                continue
            try:
                mt = p.stat().st_mtime
            except OSError:
                continue
            if newest_t is None or mt > newest_t:
                newest_t, newest_p = mt, p
    return newest_t, newest_p


# GitHub #245. The preflight below compares a BINARY's mtime against its
# sources, which cannot distinguish two different situations that produce the
# same reading: a correct rebuild, and a relink that carried a stale object.
# MSBuild relinks whenever anything in the link line changes, so the exe is
# freshly dated either way, and the suite then runs green about code that is not
# in the tree.
#
# Objects are the level where the two differ: a stale `.obj` predates the source
# it was compiled from, whatever the exe says.
def stale_object_pairs(objs, sources):
    """Pure core: [(obj, mtime)], {stem: (src, mtime)} -> list of complaints.

    Split out from the filesystem walk so it can be tested on synthetic inputs
    rather than by staging a real half-built tree.
    """
    out = []
    for obj_path, obj_mtime in objs:
        stem = os.path.splitext(os.path.basename(obj_path))[0].lower()
        hit = sources.get(stem)
        if not hit:
            continue                      # no source of that name; not ours
        src_path, src_mtime = hit
        if obj_mtime < src_mtime:
            out.append((obj_path, src_path, src_mtime - obj_mtime))
    return out


def _source_index(root, dirs):
    """{lowercased stem: (path, mtime)} for compilable sources under `dirs`."""
    index = {}
    for d in dirs:
        base = os.path.join(root, d)
        for dirpath, _dirnames, filenames in os.walk(base):
            if _SCRATCH_DIRS.intersection(Path(dirpath).parts):
                continue          # same exclusion as _newest_source
            for fn in filenames:
                if not fn.endswith((".cpp", ".cc", ".cxx")):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    mt = os.path.getmtime(p)
                except OSError:
                    continue
                stem = os.path.splitext(fn)[0].lower()
                prev = index.get(stem)
                # Newest wins: a duplicate stem should not let the older copy
                # excuse a stale object.
                if prev is None or mt > prev[1]:
                    index[stem] = (p, mt)
    return index


def report_stale_objects(exe: str, dirs=("src", "include", "tests")) -> bool:
    """True when any compiled object predates the source it came from."""
    root = _repo_root()
    exe_dir = os.path.dirname(os.path.abspath(exe))
    # MSBuild writes intermediates under <preset>/<...>/<target>.dir/<config>/.
    # Walk up from bin/<config> to THIS PRESET's root -- not to build/, which
    # holds other presets (vs2022-msvc-asan, repro-full, ...) whose objects are
    # months old and have nothing to do with the exe under test. Going one level
    # too far reported 3439-hour-old objects from a preset nobody was running.
    build_root = os.path.dirname(os.path.dirname(exe_dir))

    # ...and only THIS configuration's objects. bin/Debug and bin/Release are
    # separate object trees; a Debug exe is not made stale by Release objects
    # nobody relinked, and reporting them is the over-broad guard that gets
    # switched off (the same lesson _newest_source records above).
    config = os.path.basename(exe_dir)
    config_marker = os.sep + config + os.sep

    objs = []
    for dirpath, _dirnames, filenames in os.walk(build_root):
        if ".dir" not in dirpath:
            continue
        if config_marker not in dirpath + os.sep:
            continue
        for fn in filenames:
            if not fn.endswith(".obj"):
                continue
            p = os.path.join(dirpath, fn)
            try:
                objs.append((p, os.path.getmtime(p)))
            except OSError:
                continue

    if not objs:
        print("[gtest] object staleness: no .obj files found under "
              f"{build_root} -- cannot establish freshness, not asserting it")
        return False

    stale = stale_object_pairs(objs, _source_index(root, dirs))
    if not stale:
        print(f"[gtest] object staleness OK: {len(objs)} object(s) all postdate "
              "their sources")
        return False

    print(f"[gtest] STALE OBJECT(S): {len(stale)} compiled object(s) predate "
          "their source. The executable may have been RELINKED without "
          "recompiling them, which dates the exe freshly while the code it "
          "contains is old (GitHub #245).")
    for obj, src, age in sorted(stale, key=lambda t: -t[2])[:10]:
        print(f"[gtest]   {obj}\n[gtest]     is {age / 3600.0:.1f}h older than {src}")
    return True


def report_stale_binaries(exe: str) -> bool:
    """Print a diagnosis; return True if any binary predates the sources.

    Checks the test binary AND the application binary beside it, because the
    second is the one that was actually broken and the one nothing else looks at.
    """
    root = _repo_root()
    newest_t, newest_p = _newest_source(root)          # test binary: all three
    app_t, app_p = _newest_source(root, ("src", "include"))  # app binary: no tests/
    if newest_t is None:
        # Sweep found nothing: refuse to claim freshness we did not establish.
        print("[gtest] staleness preflight: found NO source files under "
              f"{root} — cannot establish freshness, not asserting it")
        return False

    exe_path = Path(exe)
    candidates = [("test binary", exe_path)]
    app = exe_path.parent / ("pdfe.exe" if os.name == "nt" else "pdfe")
    if app.exists():
        candidates.append(("application binary (launched by several suites)", app))

    stale = False
    for label, p in candidates:
        is_app = (p != exe_path)
        ref_t = app_t if is_app else newest_t
        ref_p = app_p if is_app else newest_p
        if ref_t is None:
            continue
        try:
            bt = p.stat().st_mtime
        except OSError:
            print(f"[gtest] staleness preflight: cannot stat {p}")
            continue
        if bt < ref_t:
            stale = True
            age = (ref_t - bt) / 3600.0
            print(f"[gtest] STALE {label}: {p}")
            print(f"[gtest]   is {age:.1f}h older than {ref_p}")
        else:
            print(f"[gtest] staleness preflight OK: {label} postdates the newest source")
    return stale


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--output-dir", required=True, type=Path)
    ap.add_argument("--reruns", type=int, default=2,
                    help="Max fresh-process reruns per failed test.")
    ap.add_argument("--gtest-filter", default=None)
    ap.add_argument("--shards", type=int, default=1,
                    help="Number of sequential fresh-process shards. 1 (default) "
                         "keeps the historical single-process behavior; N>1 uses "
                         "the sharded Windows harness.")
    ap.add_argument("--shard", type=int, default=None,
                    help="Run ONLY this shard index (0-based) of --shards N and "
                         "leave its XML for a later --merge-only. Lets a single "
                         "failed shard be re-run alone instead of re-running the "
                         "whole suite.")
    ap.add_argument("--modules", default="",
                    help="Comma-separated MODULE names (gtest suite before the "
                         "'.') to run. Empty = all. Lets one module be re-run "
                         "alone after a failure.")
    ap.add_argument("--exclude-modules", default="",
                    help="Comma-separated module names to exclude. Applied "
                         "AFTER --modules.")
    ap.add_argument("--merge-only", action="store_true",
                    help="Do not run anything: aggregate the per-shard XMLs "
                         "already in --output-dir into gtest_full.xml. A missing "
                         "shard XML is recorded as errors, never dropped.")
    ap.add_argument("--test-list-file", type=Path, default=None,
                    help="Optional cached '--gtest_list_tests' output to partition "
                         "from instead of enumerating at run time (avoids spawning "
                         "pdfe_tests for enumeration).")
    ap.add_argument(
        "--allow-stale-binaries", action="store_true",
        help="Run even if a binary predates the sources. The results are then "
             "about a build that no longer exists; only for deliberately "
             "re-running an old binary.")
    args = ap.parse_args()

    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = str(Path(args.exe).resolve())

    # Tee this runner's OWN stdout into the output directory.
    #
    # Raised by the phase-8 test-runner gate. DCA 0028's whole point is that a
    # shard can pass every assertion and still crash on exit, and the only
    # evidence of that is the per-shard 'rc=' line and the TEARDOWN CRASH
    # status THIS script prints. Those lines existed nowhere but the terminal,
    # so a gate citing a run had to take the operator's word for them while the
    # XML beside it — the thing that cannot see a post-assertion crash — was
    # the artifact on disk. Writing run.log next to the XML makes the evidence
    # that contradicts the XML as durable as the XML.
    log_path = out / "run.log"

    class _Tee:
        """Mirror writes to the real stream and to run.log.

        Deliberately not logging.basicConfig: every existing diagnostic in this
        file is a plain print(), and rewriting them all would be a far larger
        change than the gap warrants.
        """

        def __init__(self, stream, sink):
            self._stream = stream
            self._sink = sink

        def write(self, data):
            self._stream.write(data)
            self._sink.write(data)
            # Unbuffered on the sink: if the run dies hard (which is exactly
            # the scenario this exists for) a buffered tail is lost.
            self._sink.flush()
            return len(data)

        def flush(self):
            self._stream.flush()
            self._sink.flush()

        def isatty(self):
            return getattr(self._stream, "isatty", lambda: False)()

    real_stdout, real_stderr = sys.stdout, sys.stderr
    with open(log_path, "w", encoding="utf-8", newline="") as sink:
        sys.stdout = _Tee(real_stdout, sink)
        sys.stderr = _Tee(real_stderr, sink)
        try:
            stale = report_stale_binaries(exe)
            # GitHub #245: a fresh exe and a stale object give the same reading
            # at the binary level, so the objects are checked too.
            stale = report_stale_objects(exe) or stale
            if stale and not args.allow_stale_binaries:
                print("[gtest] REFUSING TO RUN — see above. Rebuild both targets:")
                print("[gtest]   cmake --build build/vs2022 --config Debug "
                      "--target pdfe_tests pdfe --parallel 4 -- -nodeReuse:false")
                return 2

            _mods = [m.strip() for m in args.modules.split(",") if m.strip()]
            _excl = [m.strip() for m in args.exclude_modules.split(",") if m.strip()]
            if args.merge_only:
                # Deliberately BEFORE the shards<=1 branch: merging is about
                # XMLs on disk, not about how many shards this invocation runs.
                rc = merge_shards(exe, out, max(args.shards, 1),
                                  args.gtest_filter, args.test_list_file,
                                  _mods, _excl)
            elif args.shard is not None:
                rc = run_one_shard(exe, out, max(args.shards, 1), args.shard,
                                   args.gtest_filter, args.test_list_file,
                                   _mods, _excl)
            elif args.shards <= 1:
                rc = run_single_process(exe, out, args.gtest_filter, args.reruns)
            else:
                rc = run_sharded(exe, out, args.shards, args.gtest_filter,
                                 args.reruns, args.test_list_file)
        finally:
            sys.stdout, sys.stderr = real_stdout, real_stderr
    print(f"[gtest] runner log written to {log_path}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
