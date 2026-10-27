# -*- coding: utf-8 -*-
"""Phase-25 criterion: separately-executed shards must merge to the same totals.

    "A test proves a merged result set from separately-executed modules is
     byte-equivalent in totals to a single full run."

That is the load-bearing claim of resumable sharding. If it does not hold, then
re-running one failed shard and merging produces a DIFFERENT answer from running
everything, and every gate downstream is reading a number nobody can reproduce.

The runs here use a FAKE test executable -- a Python script that honours
`--gtest_list_tests`, `--gtest_filter` and `--gtest_output=xml:` well enough to
exercise the real partition/run/merge code paths. Deliberately not the real
2,000-test binary: the property under test is arithmetic over the partition and
the aggregator, and a fake lets a missing shard, a crashed shard and a failing
test be produced ON DEMAND rather than waited for.

Run:
    python -m pytest tests/scripts/test_run_gtest_shard_merge.py
    python tests/scripts/test_run_gtest_shard_merge.py     (same checks, no pytest)
"""
from __future__ import annotations

import io
import os
import sys
import shutil
import tempfile
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNNER = HERE / "run_gtest.py"

# ids: 11 tests over 3 suites, two of which are deliberately RED so the merge is
# exercised on a red run as well as a green one (a merger that only ever sees
# green cannot show it preserves failures).
FAKE_TESTS = [
    "Alpha.One", "Alpha.Two", "Alpha.Three", "Alpha.Four",
    "Beta.One", "Beta.Two", "Beta.Three",
    "Gamma.One", "Gamma.Two", "Gamma.Three", "Gamma.Four",
]
FAILING = {"Beta.Two", "Gamma.Three"}

FAKE_EXE_SRC = '''# -*- coding: utf-8 -*-
"""A stand-in for pdfe_tests.exe: enough gtest CLI to drive run_gtest.py."""
import io, sys, os
TESTS = %(tests)r
FAILING = %(failing)r

def out_path(argv):
    for a in argv:
        if a.startswith("--gtest_output=xml:"):
            return a.split("xml:", 1)[1]
    return None

def selected(argv):
    for a in argv:
        if a.startswith("--gtest_filter="):
            pats = [p for p in a.split("=", 1)[1].split(":") if p]
            return [t for t in TESTS if t in pats]
    return list(TESTS)

argv = sys.argv[1:]
if "--gtest_list_tests" in argv:
    suites = {}
    for t in TESTS:
        s, c = t.split(".", 1)
        suites.setdefault(s, []).append(c)
    for s, cases in suites.items():
        print(s + ".")
        for c in cases:
            print("  " + c)
    sys.exit(0)

sel = selected(argv)
p = out_path(argv)
fails = [t for t in sel if t in FAILING]
if p:
    os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    with io.open(p, "w", encoding="utf-8") as fh:
        fh.write('<?xml version="1.0" encoding="UTF-8"?>\\n')
        fh.write('<testsuites tests="%%d" failures="%%d" errors="0" '
                 'disabled="0" skipped="0" time="0.1" name="AllTests">\\n'
                 %% (len(sel), len(fails)))
        by = {}
        for t in sel:
            s, c = t.split(".", 1)
            by.setdefault(s, []).append(c)
        for s, cases in by.items():
            nf = len([c for c in cases if (s + "." + c) in FAILING])
            fh.write('  <testsuite name="%%s" tests="%%d" failures="%%d" '
                     'errors="0" skipped="0" time="0.1">\\n' %% (s, len(cases), nf))
            for c in cases:
                fh.write('    <testcase name="%%s" status="run" time="0.01" '
                         'classname="%%s"' %% (c, s))
                if (s + "." + c) in FAILING:
                    fh.write('>\\n      <failure message="boom">boom</failure>\\n')
                    fh.write('    </testcase>\\n')
                else:
                    fh.write(' />\\n')
            fh.write('  </testsuite>\\n')
        fh.write('</testsuites>\\n')
sys.exit(1 if fails else 0)
''' % {"tests": FAKE_TESTS, "failing": sorted(FAILING)}


def _totals_of(xml: Path) -> dict:
    root = ET.parse(str(xml)).getroot()
    return {k: int(root.get(k, 0))
            for k in ("tests", "failures", "errors", "skipped")}


def _run_runner(exe: str, outdir: Path, *extra: str) -> subprocess.CompletedProcess:
    cmd = [sys.executable, str(RUNNER), "--exe", exe,
           "--output-dir", str(outdir), "--reruns", "0",
           "--allow-stale-binaries", *extra]
    return subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


class _Sandbox:
    def __enter__(self):
        self.root = Path(tempfile.mkdtemp(prefix="pdfe-shardmerge-"))
        self.exe = self.root / "fake_tests.py"
        with io.open(self.exe, "w", encoding="utf-8") as fh:
            fh.write(FAKE_EXE_SRC)
        # run_gtest.py invokes the exe directly, so make it a runnable command:
        # on Windows a .py is not executable, so wrap it in a .cmd/.sh shim.
        if os.name == "nt":
            self.shim = self.root / "fake_tests.cmd"
            with io.open(self.shim, "w", encoding="utf-8", newline="\r\n") as fh:
                fh.write("@echo off\r\n\"%s\" \"%s\" %%*\r\n"
                         % (sys.executable, self.exe))
        else:
            self.shim = self.root / "fake_tests.sh"
            with io.open(self.shim, "w", encoding="utf-8") as fh:
                fh.write("#!/bin/sh\nexec '%s' '%s' \"$@\"\n"
                         % (sys.executable, self.exe))
            os.chmod(self.shim, 0o755)
        return self

    def __exit__(self, *a):
        shutil.rmtree(self.root, ignore_errors=True)


# ---------------------------------------------------------------------------

def test_merged_shards_equal_a_single_full_run():
    """THE criterion. Per-shard runs + --merge-only == one --shards N run."""
    with _Sandbox() as sb:
        whole = sb.root / "whole"
        _run_runner(str(sb.shim), whole, "--shards", "4")
        ref = _totals_of(whole / "gtest_full.xml")

        piece = sb.root / "piece"
        piece.mkdir()
        for i in range(4):
            _run_runner(str(sb.shim), piece, "--shards", "4", "--shard", str(i))
        _run_runner(str(sb.shim), piece, "--shards", "4", "--merge-only")
        got = _totals_of(piece / "gtest_full.xml")

    assert got == ref, (
        "a merged result set differs from a single full run: %r vs %r" % (got, ref))
    assert ref["tests"] == len(FAKE_TESTS), ref
    assert ref["failures"] == len(FAILING), ref


def test_the_reference_run_is_not_vacuous():
    """Guards the guard: if the fake produced nothing, equality is trivial."""
    with _Sandbox() as sb:
        whole = sb.root / "whole"
        _run_runner(str(sb.shim), whole, "--shards", "4")
        t = _totals_of(whole / "gtest_full.xml")
    assert t["tests"] > 0 and t["failures"] > 0, (
        "the reference run has no tests or no failures, so 'merged == full' "
        "would be satisfied by two empty result sets: %r" % t)


def test_a_missing_shard_is_errors_not_a_quietly_short_total():
    """The worst possible failure: merging 3 of 4 shards and reporting green."""
    with _Sandbox() as sb:
        piece = sb.root / "piece"
        piece.mkdir()
        for i in range(4):
            _run_runner(str(sb.shim), piece, "--shards", "4", "--shard", str(i))
        (piece / "gtest_shard_2.xml").unlink()
        r = _run_runner(str(sb.shim), piece, "--shards", "4", "--merge-only")
        got = _totals_of(piece / "gtest_full.xml")

    assert got["tests"] == len(FAKE_TESTS), (
        "the merged total SHRANK when a shard XML went missing (%r); a short "
        "aggregate that reports green is the false-green this harness exists "
        "to prevent" % got)
    assert got["errors"] > 0, "a missing shard produced no errors: %r" % got
    assert r.returncode != 0, "merging an incomplete set exited 0"
    assert "MISSING shard" in (r.stdout + r.stderr)


def test_shard_index_out_of_range_is_refused():
    with _Sandbox() as sb:
        d = sb.root / "oob"
        r = _run_runner(str(sb.shim), d, "--shards", "4", "--shard", "4")
        assert r.returncode != 0, "--shard 4 of 4 was accepted"
        assert "out of range" in (r.stdout + r.stderr)


def test_one_shard_alone_does_not_write_a_full_aggregate():
    """A partial run must not leave a gtest_full.xml for a gate to misread."""
    with _Sandbox() as sb:
        d = sb.root / "one"
        _run_runner(str(sb.shim), d, "--shards", "4", "--shard", "0")
        assert not (d / "gtest_full.xml").exists(), (
            "running a single shard wrote gtest_full.xml — every downstream "
            "gate reads that file and would see a fraction of the suite")
        assert (d / "gtest_shard_0.xml").exists()


def test_rerunning_one_shard_then_merging_still_matches():
    """The actual workflow: one shard re-run alone, merged into the existing set."""
    with _Sandbox() as sb:
        whole = sb.root / "whole"
        _run_runner(str(sb.shim), whole, "--shards", "4")
        ref = _totals_of(whole / "gtest_full.xml")

        piece = sb.root / "piece"
        piece.mkdir()
        for i in range(4):
            _run_runner(str(sb.shim), piece, "--shards", "4", "--shard", str(i))
        # Simulate "shard 1 was red, re-run just that one".
        (piece / "gtest_shard_1.xml").unlink()
        _run_runner(str(sb.shim), piece, "--shards", "4", "--shard", "1")
        _run_runner(str(sb.shim), piece, "--shards", "4", "--merge-only")
        got = _totals_of(piece / "gtest_full.xml")

    assert got == ref, ("re-running one shard and merging changed the totals: "
                        "%r vs %r" % (got, ref))


def test_module_selection_runs_only_that_module():
    """--modules re-runs one named module alone, which is the criterion's ask."""
    with _Sandbox() as sb:
        d = sb.root / "mod"
        _run_runner(str(sb.shim), d, "--shards", "1", "--shard", "0",
                    "--modules", "Beta")
        root = ET.parse(str(d / "gtest_shard_0.xml")).getroot()
        names = {"%s.%s" % (tc.get("classname"), tc.get("name"))
                 for tc in root.iter("testcase")}
    assert names and all(n.startswith("Beta.") for n in names), (
        "--modules Beta ran something outside Beta: %r" % sorted(names))
    assert len(names) == 3, "expected Beta's 3 tests, got %r" % sorted(names)


def test_exclude_modules_is_applied_after_include():
    with _Sandbox() as sb:
        d = sb.root / "excl"
        _run_runner(str(sb.shim), d, "--shards", "1", "--shard", "0",
                    "--modules", "Alpha,Beta", "--exclude-modules", "Beta")
        root = ET.parse(str(d / "gtest_shard_0.xml")).getroot()
        names = {"%s.%s" % (tc.get("classname"), tc.get("name"))
                 for tc in root.iter("testcase")}
    assert names and all(n.startswith("Alpha.") for n in names), sorted(names)


def test_merging_across_a_DIFFERENT_partition_is_refused():
    """The silent mis-attribution this manifest exists to stop.

    Shard XMLs written under one module selection, merged under another, would
    be checked against tests those shards never ran -- and nothing about that
    is visible in the totals, which is exactly why it must fail rather than warn.
    """
    with _Sandbox() as sb:
        d = sb.root / "mismatch"
        for i in range(2):
            _run_runner(str(sb.shim), d, "--shards", "2", "--shard", str(i),
                        "--modules", "Alpha,Beta")
        r = _run_runner(str(sb.shim), d, "--shards", "2", "--merge-only")
    assert r.returncode != 0, "merging across a different partition was allowed"
    out = r.stdout + r.stderr
    assert "DIFFERENT partition" in out, out[-400:]


def test_module_selection_is_not_vacuous():
    """A filter that matches nothing must not look like a clean selective run."""
    with _Sandbox() as sb:
        d = sb.root / "none"
        r = _run_runner(str(sb.shim), d, "--shards", "1", "--shard", "0",
                        "--modules", "NoSuchModule")
        assert not (d / "gtest_shard_0.xml").exists(), (
            "a selection matching no test still produced a shard XML")
        assert "0 test" in (r.stdout + r.stderr) or r.returncode == 0


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print("  %-52s PASS" % t.__name__)
        except AssertionError as exc:
            failed += 1
            print("  %-52s FAIL" % t.__name__)
            print("      " + str(exc).replace("\n", "\n      "))
    print("")
    if failed:
        print("RESULT: %d of %d FAILED" % (failed, len(tests)))
        return 1
    print("RESULT: all %d checks pass" % len(tests))
    return 0


if __name__ == "__main__":
    sys.exit(main())
