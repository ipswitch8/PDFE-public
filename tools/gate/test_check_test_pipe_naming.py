# -*- coding: utf-8 -*-
"""Companion test for `check_test_pipe_naming.py` (GitHub #303).

The checker's own `--self-test` exercises its judgement on synthetic trees.
This file does the two things a self-test cannot:

  * runs it against the REAL tree, so a scanner that has gone blind on the
    actual construction idiom is caught (the self-test's fixtures would keep
    passing);
  * pins that the checker's naming rule and the C++ guard's naming rule are
    THE SAME RULE. They are two copies of one decision, in two languages, and
    nothing else would notice them drifting apart -- at which point the
    checker would cheerfully certify pipes the guard cannot actually see.
"""
from __future__ import annotations

import importlib.util
import io
import pathlib
import subprocess
import sys

import pytest

_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
_MOD_PATH = _HERE / "check_test_pipe_naming.py"
_GUARD_HEADER = _REPO / "src" / "ui" / "TestPipeFocusGuard.h"


def _load():
    spec = importlib.util.spec_from_file_location("ctpn", _MOD_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["ctpn"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_the_checkers_own_self_test_passes():
    r = subprocess.run([sys.executable, str(_MOD_PATH), "--self-test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-3000:]


def test_the_real_tree_is_clean():
    """Every structural pipe in the product is reachable by the focus guard."""
    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--repo-root", str(_REPO)],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out[-3000:]
    # Non-vacuity: a pass that scanned nothing carries no information, and the
    # checker's "blind" branch is the only thing standing between this
    # assertion and a silent zero.
    assert "structural test pipe(s)" in out, out[-2000:]


def test_the_scan_still_finds_the_real_population():
    """A FLOOR on the census, for the reason the census exists.

    #303 said "~25 query/result pipe pairs originating at MainWindow.cpp:673".
    The real population measured 2026-10-14 is **108** structural pipes across
    three files (29 query, 37 result, the rest single pipes), and the cited
    line had already drifted. If this number collapses, the construction idiom
    changed and the scanner went blind -- which would read as "every pipe is
    covered" when it means "no pipe was looked at".

    A floor, not an equality: pipes get added, and a guard that reddens
    whenever the product grows is a guard that gets switched off.
    """
    mod = _load()
    pipes = mod.scan_tree(_REPO)
    assert len(pipes) >= 100, (
        "structural pipe census COLLAPSED to %d (was 108 on 2026-10-14). The "
        "QLineEdit + Qt::NoFocus + zero-size idiom has probably changed; fix "
        "the scanner rather than lowering this floor." % len(pipes))
    files = {p[0] for p in pipes}
    assert len(files) >= 3, sorted(files)


def test_every_pipe_has_a_resolvable_name():
    """`objectName=None` means the scanner could not resolve a name.

    That is not the same as a misnamed pipe and must not be allowed to pass
    quietly as one: a pipe whose name cannot be resolved is a pipe whose
    coverage is UNKNOWN. The first run of the checker produced eleven of
    these -- ten from a `setObjectName(` split across two lines, one from a
    factory lambda -- and both were scanner limitations rather than product
    defects.
    """
    mod = _load()
    unresolved = [p for p in mod.scan_tree(_REPO) if not p[3]]
    assert not unresolved, (
        "the scanner could not resolve a name for %d pipe(s), so their "
        "coverage by TestPipeFocusGuard is unknown rather than confirmed: %r"
        % (len(unresolved), unresolved[:5]))


def test_the_python_rule_and_the_cpp_guard_agree():
    """TWO COPIES, ONE DECISION.

    `check_test_pipe_naming.NAME_RE` is `^test.*Pipe$`; the guard's
    `isTestPipe` does `startsWith("test") && endsWith("Pipe")` on a
    `QLineEdit`. If either side is changed alone, the checker certifies a
    population the guard does not actually cover -- and every test would still
    be green, because each side is self-consistent. This is the only thing
    that looks at both.
    """
    mod = _load()
    assert mod.NAME_RE.pattern == r"^test.*Pipe$", mod.NAME_RE.pattern

    src = io.open(_GUARD_HEADER, encoding="utf-8").read()
    assert 'startsWith(QStringLiteral("test"))' in src, (
        "the C++ guard no longer tests a `test` prefix, so the Python rule "
        "%r is certifying a population the guard does not cover"
        % mod.NAME_RE.pattern)
    assert 'endsWith(QStringLiteral("Pipe"))' in src, (
        "the C++ guard no longer tests a `Pipe` suffix, so the Python rule "
        "%r is certifying a population the guard does not cover"
        % mod.NAME_RE.pattern)
    assert "qobject_cast<const QLineEdit*>" in src, (
        "the C++ guard no longer restricts itself to QLineEdit, so it may now "
        "match widgets this checker never examines")


@pytest.mark.parametrize("name,expected", [
    ("testToolActivatePipe", True),
    ("testCustomizeAvailableActionsResultPipe", True),
    ("testPipe", True),
    ("searchField", False),
    ("testSomethingElse", False),     # prefix only
    ("renderPipe", False),            # suffix only
    ("TestThingPipe", False),         # capital T -- startsWith is exact
    ("", False),
])
def test_the_naming_rule_matches_what_the_guard_accepts(name, expected):
    """Cases chosen to straddle the rule, including the three near-misses.

    A rule tested only on names that match proves nothing about what it
    rejects, and the near-misses are where a regex gets quietly loosened.
    """
    mod = _load()
    assert bool(mod.NAME_RE.match(name)) is expected, name


# ------------------------------------------------- the THIRD copy of the rule
#
# Raised as finding 1 by the p97 phase-3 claim-check gate, which PROVED it was
# unmechanised: it dropped the `startswith("test")` requirement from
# `qinvoke.focus_is_a_test_pipe` and every non-live test stayed green --
# including the one whose entire job is to prove that detector works.
#
# There are THREE copies of one decision:
#
#   1. `TestPipeFocusGuard::isTestPipe`              (C++, the real guard)
#   2. `check_test_pipe_naming.NAME_RE`              (this checker)
#   3. `tests/helpers/qinvoke.focus_is_a_test_pipe`  (the LIVE test's detector)
#
# `test_the_python_rule_and_the_cpp_guard_agree` couples 1 and 2. Nothing
# coupled 3 to anything, and 3 is the one the only UIA-driven proof of #303
# depends on: if it drifts, that test stops detecting the defect it exists to
# detect and stays green. A detector nobody watches is worse than no detector,
# because its green is read as evidence.

def _focus_detector():
    """Import `focus_is_a_test_pipe` without importing pywinauto.

    `tests/helpers/qinvoke.py` is a UIA module, but the function under test is
    pure string logic. Loaded by file so this coupling check runs anywhere --
    including CI, where there is no UIA and no desktop. A check that only runs
    on one workstation would leave the hole open everywhere else.
    """
    spec = importlib.util.spec_from_file_location(
        "qinvoke_for_coupling", _REPO / "tests" / "helpers" / "qinvoke.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["qinvoke_for_coupling"] = mod
    try:
        spec.loader.exec_module(mod)
    except Exception as exc:  # pragma: no cover
        pytest.fail(
            "could not load tests/helpers/qinvoke.py to check the third copy "
            "of the pipe-naming rule, so that copy is unwatched: %r" % (exc,))
    return mod.focus_is_a_test_pipe


#: (objectName, className) pairs straddling the rule. The near-misses are the
#: point: a battery of names that all match proves nothing about what the
#: detector rejects, and rejection is where the claim-check mutation landed.
_FIELD_CASES = [
    ("testKeyTraceQueryPipe", "QLineEdit"),
    ("testSetZoomPipe", "QLineEdit"),
    ("testPipe", "QLineEdit"),
    ("searchField", "QLineEdit"),
    ("testSomethingElse", "QLineEdit"),      # prefix only
    ("renderPipe", "QLineEdit"),             # suffix only -- the mutation case
    ("TestThingPipe", "QLineEdit"),          # wrong case
    ("pdfCanvas", "PdfCanvas"),
    ("annotContentsEdit", "QTextEdit"),
    ("testMisleadingPipe", "QWidget"),       # right name, wrong class
]


@pytest.mark.parametrize("name,cls", _FIELD_CASES)
def test_the_live_tests_detector_agrees_with_this_checkers_rule(name, cls):
    """Couple copy 3 to copy 2, which is already coupled to copy 1.

    The expected answer is DERIVED from `NAME_RE` plus the QLineEdit
    restriction the C++ guard applies, not written out by hand -- a hand-written
    expectation would be a fourth copy and would drift too.
    """
    mod = _load()
    focus_is_a_test_pipe = _focus_detector()
    expected = bool(mod.NAME_RE.match(name)) and cls == "QLineEdit"
    field = "%s[%s]" % (name, cls)
    assert focus_is_a_test_pipe(field) is expected, (
        "qinvoke.focus_is_a_test_pipe(%r) returned %r but this checker's rule "
        "(%s, restricted to QLineEdit as TestPipeFocusGuard::isTestPipe does) "
        "says %r. The live UIA regression test for GitHub #303 uses that "
        "function as its detector, so a disagreement means that test has "
        "stopped detecting the defect while still passing."
        % (field, focus_is_a_test_pipe(field), mod.NAME_RE.pattern, expected))


def test_the_detector_battery_is_not_all_one_answer():
    """Anti-vacuity for the battery itself.

    If every case expected False, the parametrised test above would pass
    against a detector that always returns False -- which is exactly the
    direction that silently disarms the live test.
    """
    mod = _load()
    expectations = [bool(mod.NAME_RE.match(n)) and c == "QLineEdit"
                    for n, c in _FIELD_CASES]
    assert any(expectations), _FIELD_CASES
    assert not all(expectations), _FIELD_CASES
