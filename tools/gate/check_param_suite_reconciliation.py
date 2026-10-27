#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_param_suite_reconciliation.py -- reconcile the ENUMERATED test set
against the JUnit XML, so a test that was never instantiated cannot read as
PASSED.

GitHub #211. Every existing control in this repo is a property of the tests
that ARE in the XML:

  * ``tools/gate/check_junit_xml.py`` -- parses, failures==0, errors==0,
    tests>0, >=1 ran, optional ``--require-test``, optional ``--min-tests``.
  * ``tests/scripts/run_gtest.py`` -- the F70 shard-sum control: the 16 shard
    XMLs must sum to the merged total.

Neither compares the XML against an INDEPENDENT enumeration of what should
have been there. ``--min-tests`` is a floor on a total, so a suite that loses
40 parameterised instantiations and gains 40 elsewhere passes it; the shard-sum
control reconciles the XMLs against each other, never against an enumeration.

Why that is a live hole and not a theoretical one: ``tests/test_realworld_corpus.cpp``
is parameterised over the sidecars it finds under ``tests/fixtures/realworld/``.
The seventeen fixtures had none, so seventeen of its cases *did not exist* --
and the suite reported green for months, because a ``TEST_P`` case that is
never instantiated is not a failure, it is nothing at all. The generalisation
phase-61 drew is exact:

    A test parameterised over discovered artefacts reports [ PASSED ] for the
    artefacts that are missing. Its green is a statement about what exists,
    not about what should.

TWO LAYERS, AND THEY CATCH DIFFERENT THINGS
-------------------------------------------
Layer 1 (``--xml`` + ``--list-file``/``--exe``) reconciles the enumeration from
``pdfe_tests.exe --gtest_list_tests`` against the merged XML's testcases and
fails on any enumerated id with no matching ``<testcase>``. This catches a case
that WAS instantiated and then vanished from the report -- a dropped shard, a
crashed process, a filter that silently excluded it, a merge that lost a file.

Layer 1 CANNOT catch the PDF-79 shape, and this script says so rather than
implying coverage it does not have: with no sidecars present,
``--gtest_list_tests`` also omits the seventeen cases, so enumeration and XML
agree perfectly on a suite that is 100% missing. Enumeration is produced by the
same discovery that is under suspicion.

Layer 2 (``--expectations``) is the one that catches it. For a suite
parameterised over DISCOVERED ARTEFACTS, the artefacts are enumerated from the
filesystem -- a source independent of the test binary -- and the XML must carry
one instantiated testcase per artefact per case. Zero artefacts is itself a
finding, never a vacuous pass: "no artefacts, therefore no expectations, therefore
green" is precisely the defect.

Exit 0 = reconciled, 1 = a finding, 2 = usage/IO error.

Usage:
    python tools/gate/check_param_suite_reconciliation.py \
        --xml build/gtest_out/gtest_full.xml \
        --exe build/vs2022/bin/Debug/pdfe_tests.exe

    python tools/gate/check_param_suite_reconciliation.py \
        --xml build/gtest_out/gtest_full.xml \
        --list-file build/gtest_out/gtest_list.log \
        --expectations tools/gate/param_suite_expectations.json

    python tools/gate/check_param_suite_reconciliation.py --self-test
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

try:  # pragma: no cover - console encoding fixup
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

REPO_ROOT = Path(__file__).resolve().parents[2]

# Reuse -- do NOT re-implement -- the gtest listing parser that the sharding
# harness already owns and unit-tests. A second parser is a second place for
# the "Test/0  # GetParam() = ..." comment stripping to be subtly wrong.
_SCRIPTS = REPO_ROOT / "tests" / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))
try:
    from run_gtest import parse_gtest_list  # type: ignore # noqa: E402
except Exception as _exc:  # pragma: no cover - only if the harness moves
    parse_gtest_list = None  # type: ignore
    _PARSE_IMPORT_ERROR = _exc
else:
    _PARSE_IMPORT_ERROR = None


# --------------------------------------------------------------------------- #
# Pure logic
# --------------------------------------------------------------------------- #

def is_parameterised(test_id: str) -> bool:
    """True for a value- or type-parameterised gtest id.

    GoogleTest spells instantiated ids in exactly two shapes:
      * value-parameterised: ``Prefix/Suite.Case/0`` (a '/' on either side of
        the dotted core), and
      * type-parameterised:  ``Prefix/Suite/0.Case``.
    Both contain a '/'; a plain ``TEST``/``TEST_F`` id never does.
    """
    return "/" in test_id


def xml_case_ids(xml_path: Path) -> Tuple[Set[str], int, int]:
    """Return (ids, total_testcase_elements, ran_count) from a JUnit XML.

    An id is ``classname.name`` -- the same spelling ``--gtest_list_tests``
    produces, so the two sets are directly comparable.
    """
    root = ET.parse(str(xml_path)).getroot()
    ids: Set[str] = set()
    total = 0
    ran = 0
    for tc in root.iter("testcase"):
        total += 1
        name = tc.get("name") or ""
        classname = tc.get("classname") or ""
        ids.add(f"{classname}.{name}" if classname else name)
        if tc.get("status", "run") == "run":
            ran += 1
    return ids, total, ran


def reconcile(enumerated: Sequence[str], xml_ids: Set[str],
              parameterised_only: bool = True) -> Dict[str, List[str]]:
    """Compare an enumeration against the XML's testcase ids.

    ``DISABLED_`` tests are excluded from the enumerated side: they ARE listed
    by ``--gtest_list_tests`` but are deliberately not run, so treating them as
    missing would make this guard permanently red -- and a guard that is always
    red is decoration, because the next real finding is indistinguishable from
    the standing failure.
    """
    live = [t for t in enumerated if "DISABLED_" not in t]
    if parameterised_only:
        considered = [t for t in live if is_parameterised(t)]
    else:
        considered = list(live)
    missing = sorted(t for t in considered if t not in xml_ids)
    enum_set = set(live)
    extra = sorted(t for t in xml_ids if t not in enum_set
                   and (not parameterised_only or is_parameterised(t)))
    return {
        "considered": considered,
        "missing_from_xml": missing,
        "in_xml_not_enumerated": extra,
    }


def _suite_of(test_id: str) -> str:
    """``Prefix/Suite.Case/0`` -> ``Prefix/Suite``."""
    return test_id.rsplit(".", 1)[0] if "." in test_id else test_id


def _case_of(test_id: str) -> str:
    """``Prefix/Suite.Case/0`` -> ``Case`` (instantiation index stripped)."""
    tail = test_id.rsplit(".", 1)[1] if "." in test_id else test_id
    return tail.split("/", 1)[0]


def check_artefact_expectations(
    expectations: Sequence[dict],
    xml_ids: Set[str],
    repo_root: Path,
) -> List[str]:
    """Layer 2: one instantiated testcase per DISCOVERED ARTEFACT.

    Each expectation is::

        {"suite": "RealworldFixtures/RealworldCorpusTest",
         "artefacts": ["tests/fixtures/realworld/*.pdf"],
         "exclude": ["tests/fixtures/realworld/test_*.pdf"],
         "require_sibling": ".expected.json",
         "min_artefacts": 1,
         "why": "..."}

    ``suite`` is matched with fnmatch against the XML testcase's suite part, so
    an instantiation prefix that changes does not silently disable the check --
    a mismatch is reported, never skipped.

    ``exclude`` and ``require_sibling`` exist because the C++ discovery
    predicates are not plain globs: ``collectExternalCorpus()`` skips stems
    beginning ``test_``, and ``discoverFixtures()`` pairs each sidecar with a
    same-stem ``.pdf`` and drops unpaired ones. An expectation that did not
    mirror the predicate would be a guard that is permanently red for a reason
    that is not a defect -- which is how guards stop being read.
    """
    problems: List[str] = []
    for exp in expectations:
        suite_pat = exp.get("suite")
        if not suite_pat:
            problems.append("expectation entry has no 'suite' key")
            continue
        globs = exp.get("artefacts") or []
        excludes = exp.get("exclude") or []
        sibling = exp.get("require_sibling") or ""
        strip_suffix = exp.get("strip_suffix") or ""
        min_artefacts = int(exp.get("min_artefacts", 1))

        excluded: Set[str] = set()
        for g in excludes:
            for p in repo_root.glob(g):
                excluded.add(p.as_posix())

        artefacts: Set[str] = set()
        for g in globs:
            for p in sorted(repo_root.glob(g)):
                posix = p.as_posix()
                if posix in excluded:
                    continue
                if sibling:
                    name = p.name
                    stem = (name[:-len(strip_suffix)] if strip_suffix
                            and name.endswith(strip_suffix) else p.stem)
                    if not (p.parent / (stem + sibling)).exists():
                        continue
                artefacts.add(posix)
        n_art = len(artefacts)

        matched = sorted(t for t in xml_ids
                         if fnmatch.fnmatch(_suite_of(t), suite_pat))
        if not matched:
            problems.append(
                f"{suite_pat}: no <testcase> in the XML belongs to this suite. "
                f"Either the suite was never instantiated (the #211 defect) or "
                f"its instantiation prefix changed and this expectation is now "
                f"pointing at nothing. Both are findings.")
            continue

        if n_art < min_artefacts:
            problems.append(
                f"{suite_pat}: {n_art} artefact(s) found for {globs} but at "
                f"least {min_artefacts} are required. A suite parameterised "
                f"over discovered artefacts with no artefacts reports green "
                f"while testing nothing (PDF-79).")
            continue

        cases: Dict[str, int] = {}
        for t in matched:
            cases[_case_of(t)] = cases.get(_case_of(t), 0) + 1
        for case, n in sorted(cases.items()):
            if n != n_art:
                problems.append(
                    f"{suite_pat}.{case}: {n} instantiated testcase(s) in the "
                    f"XML but {n_art} artefact(s) match {globs}. Every "
                    f"artefact must produce exactly one case; a shortfall is a "
                    f"case that was never instantiated and therefore never "
                    f"could fail.")
    return problems


def offline_audit(expectations: Sequence[dict], repo_root: Path) -> List[str]:
    """Check the expectations file itself, with no XML and no test binary.

    This is what `fast-checks` can run on every push. It cannot reconcile
    against a run -- that needs the suite -- but it CAN catch the two ways an
    expectation silently stops meaning anything:

      1. the named suite no longer has an ``INSTANTIATE_TEST_SUITE_P`` in
         ``tests/`` (renamed or deleted -> the expectation matches nothing and
         would report "no <testcase>" forever, or worse, be quietly removed);
      2. the artefact globs match fewer files than ``min_artefacts`` (the
         corpus has been lost, which is the PDF-79 precondition, and it is
         observable from the filesystem alone).

    A guard whose configuration has rotted is the same as no guard, and this
    repo has several recorded instances of exactly that.
    """
    problems: List[str] = []
    tests_dir = repo_root / "tests"
    sources = ""
    for p in sorted(tests_dir.rglob("*.cpp")):
        try:
            sources += p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

    for exp in expectations:
        suite_pat = exp.get("suite") or ""
        if "/" not in suite_pat:
            problems.append(
                f"expectation suite {suite_pat!r} is not of the form "
                f"'InstantiationPrefix/SuiteName'")
            continue
        prefix, suite = suite_pat.split("/", 1)
        # INSTANTIATE_TEST_SUITE_P(Prefix, Suite, ...) -- whitespace/newlines
        # between the arguments are legal C++ and appear in this repo.
        rx = re.compile(r"INSTANTIATE_TEST_SUITE_P\s*\(\s*"
                        + re.escape(prefix) + r"\s*,\s*"
                        + re.escape(suite) + r"\s*,", re.S)
        if not rx.search(sources):
            problems.append(
                f"{suite_pat}: no INSTANTIATE_TEST_SUITE_P({prefix}, {suite}, "
                f"...) found under tests/. The expectation names a suite that "
                f"no longer exists, so it can never reconcile anything.")

        globs = exp.get("artefacts") or []
        excludes = exp.get("exclude") or []
        sibling = exp.get("require_sibling") or ""
        strip_suffix = exp.get("strip_suffix") or ""
        min_artefacts = int(exp.get("min_artefacts", 1))
        excluded = {p.as_posix() for g in excludes for p in repo_root.glob(g)}
        artefacts: Set[str] = set()
        for g in globs:
            for p in repo_root.glob(g):
                posix = p.as_posix()
                if posix in excluded:
                    continue
                if sibling:
                    name = p.name
                    stem = (name[:-len(strip_suffix)] if strip_suffix
                            and name.endswith(strip_suffix) else p.stem)
                    if not (p.parent / (stem + sibling)).exists():
                        continue
                artefacts.add(posix)
        if len(artefacts) < min_artefacts:
            problems.append(
                f"{suite_pat}: only {len(artefacts)} artefact(s) match {globs} "
                f"(min_artefacts={min_artefacts}). The corpus this suite is "
                f"parameterised over has shrunk below its floor -- the cases "
                f"for the missing artefacts do not exist and cannot fail.")
    return problems


# --------------------------------------------------------------------------- #
# Enumeration sourcing
# --------------------------------------------------------------------------- #

def enumerate_from_exe(exe: Path, gtest_filter: Optional[str] = None) -> str:
    cmd = [str(exe), "--gtest_list_tests"]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(
            f"--gtest_list_tests exited {proc.returncode}: {proc.stderr[:400]}")
    return proc.stdout


# --------------------------------------------------------------------------- #
# Self-test -- pins BOTH directions. A guard that cannot redden is decoration.
# --------------------------------------------------------------------------- #

_XML_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="{n}" failures="0" errors="0">
{bodies}
</testsuites>
"""


def _mk_xml(path: Path, ids: Sequence[str]) -> None:
    bodies = []
    for tid in ids:
        classname, name = tid.rsplit(".", 1)
        bodies.append(
            f'  <testsuite name="{classname}" tests="1" failures="0" errors="0">\n'
            f'    <testcase name="{name}" status="run" classname="{classname}"/>\n'
            f'  </testsuite>')
    path.write_text(_XML_TEMPLATE.format(n=len(ids), bodies="\n".join(bodies)),
                    encoding="utf-8")


def self_test() -> int:
    import tempfile
    failures: List[str] = []

    def expect(cond: bool, label: str) -> None:
        print(("  ok   " if cond else "  FAIL ") + label)
        if not cond:
            failures.append(label)

    listing = (
        "PlainSuite.\n"
        "  Case\n"
        "P/ParamSuite.\n"
        "  Cell/0  # GetParam() = 1\n"
        "  Cell/1  # GetParam() = 2\n"
        "  Cell/2  # GetParam() = 3\n"
    )
    enumerated = parse_gtest_list(listing) if parse_gtest_list else []
    expect(enumerated == ["PlainSuite.Case", "P/ParamSuite.Cell/0",
                          "P/ParamSuite.Cell/1", "P/ParamSuite.Cell/2"],
           "enumeration parsed via the harness's own parse_gtest_list")

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # --- direction 1: a complete XML reconciles green -------------------
        full = tmp / "full.xml"
        _mk_xml(full, enumerated)
        ids, _, _ = xml_case_ids(full)
        res = reconcile(enumerated, ids)
        expect(res["missing_from_xml"] == [],
               "complete XML -> no missing ids (stays green on a good run)")

        # --- direction 2: a dropped instantiation reddens -------------------
        partial = tmp / "partial.xml"
        _mk_xml(partial, [t for t in enumerated if t != "P/ParamSuite.Cell/1"])
        ids, _, _ = xml_case_ids(partial)
        res = reconcile(enumerated, ids)
        expect(res["missing_from_xml"] == ["P/ParamSuite.Cell/1"],
               "XML missing one instantiated case -> reported (reddens)")

        # --- direction 3: a DISABLED_ test must not redden ------------------
        with_disabled = list(enumerated) + ["P/ParamSuite.DISABLED_Cell/0"]
        ids, _, _ = xml_case_ids(full)
        res = reconcile(with_disabled, ids)
        expect(res["missing_from_xml"] == [],
               "DISABLED_ ids excluded (guard is not permanently red)")

        # --- direction 4: a non-parameterised absence is not a param finding -
        noplain = tmp / "noplain.xml"
        _mk_xml(noplain, [t for t in enumerated if t != "PlainSuite.Case"])
        ids, _, _ = xml_case_ids(noplain)
        res = reconcile(enumerated, ids, parameterised_only=True)
        expect(res["missing_from_xml"] == [],
               "parameterised-only mode ignores a plain test (scoped, not blind)")
        res = reconcile(enumerated, ids, parameterised_only=False)
        expect(res["missing_from_xml"] == ["PlainSuite.Case"],
               "--all mode does report the plain test (the scoping is a flag, "
               "not a blind spot)")

        # --- direction 5: layer 2, the PDF-79 shape -------------------------
        art = tmp / "fixtures"
        art.mkdir()
        for i in range(3):
            (art / f"f{i}.pdf").write_text("x", encoding="utf-8")
        exp = [{"suite": "P/ParamSuite", "artefacts": ["fixtures/*.pdf"]}]
        ids, _, _ = xml_case_ids(full)
        probs = check_artefact_expectations(exp, ids, tmp)
        expect(probs == [],
               "3 artefacts, 3 instantiated cases -> green")

        (art / "f3.pdf").write_text("x", encoding="utf-8")
        probs = check_artefact_expectations(exp, ids, tmp)
        expect(len(probs) == 1 and "4 artefact" in probs[0],
               "a 4th artefact with no matching case -> reddens (PDF-79 shape)")

        for i in range(4):
            (art / f"f{i}.pdf").unlink()
        probs = check_artefact_expectations(exp, ids, tmp)
        expect(len(probs) == 1 and "at least 1 are required" in probs[0],
               "ZERO artefacts -> reddens, never a vacuous green")

        empty_xml = tmp / "empty.xml"
        _mk_xml(empty_xml, ["PlainSuite.Case"])
        ids, _, _ = xml_case_ids(empty_xml)
        for i in range(3):
            (art / f"f{i}.pdf").write_text("x", encoding="utf-8")
        probs = check_artefact_expectations(exp, ids, tmp)
        expect(len(probs) == 1 and "no <testcase>" in probs[0],
               "suite entirely absent from the XML -> reddens")

        # --- direction 6: the offline audit ---------------------------------
        # Uses a synthetic tests/ tree so it exercises the real predicate
        # (regex over INSTANTIATE_TEST_SUITE_P) rather than a stub.
        tdir = tmp / "tests"
        tdir.mkdir()
        (tdir / "test_x.cpp").write_text(
            "INSTANTIATE_TEST_SUITE_P(\n    P,\n    ParamSuite,\n"
            "    ::testing::ValuesIn(discover()));\n", encoding="utf-8")
        probs = offline_audit(exp, tmp)
        expect(probs == [],
               "offline audit green: suite instantiated + corpus above floor")

        probs = offline_audit(
            [{"suite": "Gone/NoSuchSuite", "artefacts": ["fixtures/*.pdf"]}],
            tmp)
        expect(len(probs) == 1 and "no INSTANTIATE_TEST_SUITE_P" in probs[0],
               "offline audit reddens on an expectation naming a dead suite")

        probs = offline_audit(
            [{"suite": "P/ParamSuite", "artefacts": ["fixtures/*.pdf"],
              "min_artefacts": 99}], tmp)
        expect(len(probs) == 1 and "below its floor" in probs[0],
               "offline audit reddens when the corpus drops below its floor")

    print("")
    if failures:
        print(f"[param-reconcile] SELF-TEST FAIL -- {len(failures)} assertion(s)")
        return 1
    print("[param-reconcile] SELF-TEST PASS -- both directions pinned")
    return 0


# --------------------------------------------------------------------------- #

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="run the built-in both-directions self-test and exit")
    ap.add_argument("--xml", type=Path, help="merged JUnit XML to reconcile")
    ap.add_argument("--exe", type=Path,
                    help="pdfe_tests.exe -- enumeration source (--gtest_list_tests)")
    ap.add_argument("--list-file", type=Path,
                    help="a saved --gtest_list_tests output to use instead of --exe")
    ap.add_argument("--expectations", type=Path,
                    help="JSON file of artefact expectations (layer 2)")
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    ap.add_argument("--all", action="store_true",
                    help="reconcile EVERY enumerated id, not just parameterised ones")
    ap.add_argument("--report-only", action="store_true",
                    help="print findings but exit 0 (for a first survey run)")
    ap.add_argument("--offline-audit", action="store_true",
                    help="audit the expectations file alone -- no XML, no test "
                         "binary. This is the part fast-checks can run on every "
                         "push.")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.offline_audit:
        exp_path = args.expectations or (
            REPO_ROOT / "tools" / "gate" / "param_suite_expectations.json")
        if not exp_path.exists():
            print(f"[param-reconcile] expectations not found: {exp_path}",
                  file=sys.stderr)
            return 2
        data = json.loads(exp_path.read_text(encoding="utf-8"))
        entries = data.get("expectations", data if isinstance(data, list) else [])
        if not entries:
            print("[param-reconcile] the expectations file lists ZERO "
                  "expectations -- an empty control is not a passing one.",
                  file=sys.stderr)
            return 1
        probs = offline_audit(entries, args.repo_root.resolve())
        if probs:
            print(f"[param-reconcile] OFFLINE AUDIT FAIL -- {len(probs)} "
                  f"finding(s):")
            for p in probs:
                print(f"    - {p}")
            return 1
        print(f"[param-reconcile] OFFLINE AUDIT PASS -- {len(entries)} "
              f"expectation(s): every named suite is instantiated under tests/ "
              f"and every artefact corpus is above its floor")
        return 0

    if parse_gtest_list is None:
        print(f"[param-reconcile] cannot import parse_gtest_list from "
              f"tests/scripts/run_gtest.py: {_PARSE_IMPORT_ERROR}", file=sys.stderr)
        return 2

    if not args.xml:
        ap.error("--xml is required (or use --self-test)")
    if not args.xml.exists():
        print(f"[param-reconcile] XML not found: {args.xml}", file=sys.stderr)
        return 2

    try:
        xml_ids, total, ran = xml_case_ids(args.xml)
    except ET.ParseError as exc:
        print(f"[param-reconcile] unparseable XML {args.xml}: {exc}",
              file=sys.stderr)
        return 2

    problems: List[str] = []
    did_layer1 = False

    if args.exe or args.list_file:
        if args.list_file:
            if not args.list_file.exists():
                print(f"[param-reconcile] list file not found: {args.list_file}",
                      file=sys.stderr)
                return 2
            listing = args.list_file.read_text(encoding="utf-8", errors="replace")
        else:
            if not args.exe.exists():
                print(f"[param-reconcile] exe not found: {args.exe}",
                      file=sys.stderr)
                return 2
            try:
                listing = enumerate_from_exe(args.exe)
            except RuntimeError as exc:
                print(f"[param-reconcile] {exc}", file=sys.stderr)
                return 2

        enumerated = parse_gtest_list(listing)
        if not enumerated:
            print("[param-reconcile] enumeration produced ZERO ids -- that is "
                  "itself a finding, not a clean reconciliation.", file=sys.stderr)
            return 1
        res = reconcile(enumerated, xml_ids, parameterised_only=not args.all)
        did_layer1 = True
        print(f"[param-reconcile] enumerated={len(enumerated)} "
              f"considered={len(res['considered'])} "
              f"xml_testcases={total} (ran={ran})")
        for tid in res["missing_from_xml"]:
            problems.append(
                f"enumerated but ABSENT from the XML: {tid}")
        # Reported, never fatal: an id in the XML that the CURRENT binary no
        # longer enumerates means the XML predates the binary, which is a
        # staleness signal for the operator rather than a suite defect.
        if res["in_xml_not_enumerated"]:
            print(f"[param-reconcile] note: {len(res['in_xml_not_enumerated'])} "
                  f"id(s) in the XML are not in the current enumeration "
                  f"(XML older than the binary, or a filtered run). First 5: "
                  f"{res['in_xml_not_enumerated'][:5]}")

    if args.expectations:
        if not args.expectations.exists():
            print(f"[param-reconcile] expectations not found: {args.expectations}",
                  file=sys.stderr)
            return 2
        data = json.loads(args.expectations.read_text(encoding="utf-8"))
        entries = data.get("expectations", data if isinstance(data, list) else [])
        problems.extend(
            check_artefact_expectations(entries, xml_ids,
                                        args.repo_root.resolve()))
        print(f"[param-reconcile] artefact expectations checked: {len(entries)}")

    if not did_layer1 and not args.expectations:
        print("[param-reconcile] nothing to reconcile: give --exe/--list-file "
              "and/or --expectations", file=sys.stderr)
        return 2

    if problems:
        print(f"[param-reconcile] FAIL -- {len(problems)} finding(s):")
        for p in problems:
            print(f"    - {p}")
        if args.report_only:
            print("[param-reconcile] --report-only: exiting 0 despite findings")
            return 0
        return 1

    print("[param-reconcile] PASS -- enumeration and XML reconcile")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
