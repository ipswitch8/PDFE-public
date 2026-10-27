#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""'Could not check' must not read as 'checked and clean' (GitHub #187).

WHAT THIS PINS, AND WHY IT IS NOT THE TOOL'S --self-test
--------------------------------------------------------
``fz_try_raii_scan.py --self-test`` proves the ANALYSIS is right: that clang
answers the destructor-triviality and guarded-region questions the way the tool
claims. It says nothing about what happens when the tool is asked to analyse
something it cannot reach.

Until 2026-09-13 that case exited 0::

    $ python tools/gate/fz_try_raii_scan.py \
        --compile-commands build/vs2022-asan/compile_commands.json \
        --file src/measure/MeasureDictReader.cpp
    translation units : 0
    guarded regions   : 0
    hazards           : 0
    $ echo $?
    0

MeasureDictReader.cpp is a real file, in a real CMake target, that was simply
absent from a compile database generated before it was added. The scan reached
nothing and reported the same thing a clean sweep reports. A CI step inspecting
only the exit code -- which is what windows-validation.yml does -- cannot tell
the two apart, and a hazard of exactly this tool's shape shipped in p84 phase 6
behind such a pass.

These tests deliberately use a SYNTHETIC compile database and a SYNTHETIC
source tree, so they need neither libclang nor a configured build, and so they
run in the per-push fast-checks lane rather than the dispatch-only heavyweight
one. Every decision is asserted in BOTH directions: a check only ever observed
to redden proves as little as one that can only green.
"""
from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import fz_try_raii_scan as scan  # noqa: E402


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


class _Tree:
    """A throwaway repo root with a src/ tree and a compile database."""

    def __init__(self, sources, db_sources):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.abspath(self.tmp.name)
        for rel in sources:
            _write(os.path.join(self.root, rel), "int x;\n")
        db = [{"directory": self.root,
               "file": os.path.join(self.root, rel),
               "command": "clang-cl /c " + rel}
              for rel in db_sources]
        self.db = os.path.join(self.root, "compile_commands.json")
        _write(self.db, json.dumps(db, indent=1))

    def close(self):
        self.tmp.cleanup()


class CoverageDecisions(unittest.TestCase):
    """The pure decisions, both ways."""

    def test_named_file_missing_from_db_is_reported(self):
        t = _Tree(["src/a.cpp", "src/b.cpp"], ["src/a.cpp"])
        self.addCleanup(t.close)
        db_files = scan.db_file_set(
            scan.load_compile_commands(t.db, t.root, ("src",)))
        self.assertEqual(
            scan.unresolved_named_files(["src/b.cpp"], db_files, t.root),
            ["src/b.cpp"],
            "a --file absent from the compile database must be reported as "
            "unresolved; reporting nothing is what made the scan vacuous")

    def test_named_file_present_in_db_is_not_reported(self):
        t = _Tree(["src/a.cpp", "src/b.cpp"], ["src/a.cpp"])
        self.addCleanup(t.close)
        db_files = scan.db_file_set(
            scan.load_compile_commands(t.db, t.root, ("src",)))
        self.assertEqual(
            scan.unresolved_named_files(["src/a.cpp"], db_files, t.root), [],
            "a --file the database DOES cover must not be flagged -- a check "
            "that can only ever redden blocks every real scan")

    def test_named_file_accepts_an_absolute_spelling(self):
        t = _Tree(["src/a.cpp"], ["src/a.cpp"])
        self.addCleanup(t.close)
        db_files = scan.db_file_set(
            scan.load_compile_commands(t.db, t.root, ("src",)))
        self.assertEqual(
            scan.unresolved_named_files(
                [os.path.join(t.root, "src", "a.cpp")], db_files, t.root), [],
            "both the repo-relative and absolute spellings occur in real "
            "invocations; neither may be misread as uncovered")

    def test_on_disk_source_missing_from_db_is_uncovered(self):
        t = _Tree(["src/a.cpp", "src/sub/b.cpp"], ["src/a.cpp"])
        self.addCleanup(t.close)
        db_files = scan.db_file_set(
            scan.load_compile_commands(t.db, t.root, ("src",)))
        on_disk = scan.sources_on_disk(t.root, ("src",))
        self.assertEqual(len(on_disk), 2, "the tree walk must find both sources")
        self.assertEqual(
            scan.uncovered_sources(on_disk, db_files, t.root), ["src/sub/b.cpp"],
            "a source added since CMake last configured is invisible to the "
            "database and must be named as NOT SCANNED")

    def test_fully_covered_tree_reports_nothing_uncovered(self):
        t = _Tree(["src/a.cpp", "src/sub/b.cpp"], ["src/a.cpp", "src/sub/b.cpp"])
        self.addCleanup(t.close)
        db_files = scan.db_file_set(
            scan.load_compile_commands(t.db, t.root, ("src",)))
        on_disk = scan.sources_on_disk(t.root, ("src",))
        self.assertEqual(
            scan.uncovered_sources(on_disk, db_files, t.root), [],
            "a database that covers the whole tree must produce no coverage "
            "complaint, or the guard is unconditional and therefore inert")


class ExitCodes(unittest.TestCase):
    """main()'s contract. No clang is reached on any of these paths."""

    def _run(self, argv):
        return scan.main(argv)

    def test_absent_named_file_is_a_hard_failure(self):
        t = _Tree(["src/a.cpp", "src/b.cpp"], ["src/a.cpp"])
        self.addCleanup(t.close)
        rc = self._run(["--compile-commands", t.db, "--repo-root", t.root,
                        "--file", "src/b.cpp"])
        self.assertEqual(
            rc, 4,
            "naming a file the database does not cover must FAIL. Exiting 0 "
            "here is the #187 defect: it is indistinguishable from a clean scan")

    def test_stale_database_is_a_hard_failure(self):
        t = _Tree(["src/a.cpp", "src/b.cpp"], ["src/a.cpp"])
        self.addCleanup(t.close)
        rc = self._run(["--compile-commands", t.db, "--repo-root", t.root])
        self.assertEqual(
            rc, 4,
            "a whole-tree scan over a database missing an on-disk source must "
            "FAIL; the missing file reports zero hazards either way")

    def test_zero_translation_units_fails_even_with_the_opt_out(self):
        # --allow-parse-errors waives the COVERAGE complaint. It must not be
        # able to waive the fact that nothing at all was analysed: there is no
        # reduced-coverage reading of zero, only no evidence.
        t = _Tree(["src/a.cpp"], [])
        self.addCleanup(t.close)
        rc = self._run(["--compile-commands", t.db, "--repo-root", t.root,
                        "--allow-parse-errors"])
        self.assertEqual(
            rc, 4,
            "scanning 0 translation units must FAIL even under "
            "--allow-parse-errors; no flag may dress up 'no evidence' as a pass")

    def test_missing_database_still_exits_2(self):
        t = _Tree(["src/a.cpp"], ["src/a.cpp"])
        self.addCleanup(t.close)
        rc = self._run(["--compile-commands",
                        os.path.join(t.root, "nope.json"),
                        "--repo-root", t.root])
        self.assertEqual(
            rc, 2,
            "the pre-existing 'no compile database' exit code must not have "
            "been perturbed by the coverage work")


if __name__ == "__main__":
    unittest.main(verbosity=2)
