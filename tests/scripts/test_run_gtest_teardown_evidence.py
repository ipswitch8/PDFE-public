# -*- coding: utf-8 -*-
"""GitHub #300 — a staged gtest-results.xml must carry its own crash evidence.

THE PROBLEM. A pipeline evidence gate passes only when a green
`gtest-results.xml` is staged beside its verdict artifact, and **that XML is the
sole evidence**. A JUnit XML cannot record that the process which wrote it then
died — by definition nothing was left to write that down. So a run that executes
every test, writes a complete green report, and crashes during teardown produces
an artifact a gate accepts exactly as it accepts a clean run's.

The mid-run case was already covered: a shard that dies without XML has every
assigned test synthesised as an `<error>`, so the aggregate is RED. The
**teardown** case is the one with no red anywhere — every assertion passed, and
the only carrier was the exit code, which does not survive into the file.

THE FIX under test: `inject_teardown_crash_evidence` writes the classification
into the artifact as the one thing no JUnit reader can ignore — a `<testcase>`
with an `<error>`, named for the shard that died.

WHAT THESE CASES PIN. Not "the function appends an element" — that would pass
with the element in a form no gate reads. They pin that **the decision a real
gate makes flips**, by running a PORT of `phase-gate.sh::validate_junit_xml`
over two artifacts that differ only in whether the writer crashed.

"PORT" is load-bearing, and was earned the hard way. The first version of
`_gate_would_accept` counted `<failure>`/`<error>` child ELEMENTS, reasoning
that GAP-A15 makes attributes untrustworthy. The real gate sums `<testsuite>`
ATTRIBUTES. Every assertion still passed, because the fix updates both in
lockstep — so the test agreed with the gate on these fixtures while testing a
different mechanism, and a regression confined to the attribute path would have
gone straight through. The phase-2 validator gate caught it. See the docstring
on `_gate_would_accept`, and `test_the_gate_port_follows_ATTRIBUTES_*` below,
which exist to keep the port honest in both directions.
"""
from __future__ import annotations

import importlib.util
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RUN_GTEST = ROOT / "tests" / "scripts" / "run_gtest.py"


def _load():
    spec = importlib.util.spec_from_file_location("run_gtest_tev", RUN_GTEST)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["run_gtest_tev"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mod():
    return _load()


def _green_aggregate() -> ET.Element:
    """A complete, green, two-test aggregate — what a teardown crash leaves."""
    root = ET.Element("testsuites", {
        "name": "AllTests", "tests": "2", "failures": "0",
        "errors": "0", "disabled": "0", "skipped": "0",
    })
    suite = ET.SubElement(root, "testsuite", {
        "name": "S", "tests": "2", "failures": "0", "errors": "0",
    })
    for n in ("A", "B"):
        ET.SubElement(suite, "testcase", {
            "name": n, "classname": "S", "status": "run", "result": "completed",
        })
    return root


def _gate_would_accept(root: ET.Element, target: str = "") -> bool:
    """`phase-gate.sh::validate_junit_xml`, PORTED -- not paraphrased.

    THIS FUNCTION USED TO BE WRONG, and the phase-2 validator gate caught it.
    It counted `<failure>`/`<error>` CHILD ELEMENTS, on the reasoning that
    GAP-A15 means attributes can lie and elements are the stricter test. That
    reasoning is fine as far as it goes, and it is the wrong thing to assert
    HERE: the criterion is that an evidence GATE can tell a crashed run from a
    clean one, and the real gate sums `<testsuite>`-level `failures`/`errors`/
    `tests` ATTRIBUTES, falling back to the root's attributes only when the
    suite-level `tests` sum is 0.

    The two mechanisms agreed on these fixtures, so every assertion passed --
    because `inject_teardown_crash_evidence` updates attributes and elements in
    lockstep. But agreement on the fixture is not the property: a regression
    confined to the attribute path, which is the path the gate actually runs,
    would have sailed through a test that only looked at elements. Verified at
    the wrong level is indistinguishable from verified, until it matters.

    So this is now a line-by-line port of the embedded Python in
    `~/.claude/hooks/phase-gate.sh` (the `validate_junit_xml` heredoc),
    including the `tt == 0` root-attribute fallback and the `status` default of
    "run". Keep it a port. If the hook changes, change this, and do not
    "improve" it -- its only job is to be the same decision.
    """
    def ai(el, name):
        try:
            return int(el.get(name, "0") or "0")
        except Exception:
            return 0

    tf = te = tt = 0
    for ts in root.iter("testsuite"):
        tf += ai(ts, "failures")
        te += ai(ts, "errors")
        tt += ai(ts, "tests")
    cases = list(root.iter("testcase"))
    if tt == 0:                       # single-suite / testsuites-only attrs
        tf += ai(root, "failures")
        te += ai(root, "errors")
        tt += ai(root, "tests")

    def is_run(tc):
        return tc.get("status", "run") == "run"

    run_cases = [tc for tc in cases if is_run(tc)]
    ok = (tf == 0 and te == 0 and tt > 0 and len(run_cases) > 0)
    if target:
        matches = [tc for tc in cases
                   if tc.get("name") == target
                   or (tc.get("classname", "") + "." + tc.get("name", ""))
                   == target]
        ok = ok and any(is_run(tc) for tc in matches)
    return ok


def test_a_clean_run_is_accepted_by_the_gate_check(mod):
    """The control. Without it, every assertion below could pass vacuously."""
    root = _green_aggregate()
    assert mod.inject_teardown_crash_evidence(root, []) == 0, (
        "nothing crashed, so nothing should have been injected")
    assert _gate_would_accept(root), (
        "a genuinely clean aggregate must still satisfy the gate -- if this "
        "fails the fix reddens healthy runs, which is how a guard gets "
        "switched off")


def test_a_teardown_crash_is_rejected_by_the_SAME_gate_check(mod):
    """THE DEFECT. Identical green report; only the writer's death differs."""
    root = _green_aggregate()
    assert _gate_would_accept(root), "precondition: the report starts green"

    n = mod.inject_teardown_crash_evidence(root, [(7, 3221225477)])
    assert n == 1, "one crashed shard should inject one case"
    assert not _gate_would_accept(root), (
        "a run whose process crashed after writing a green report was still "
        "accepted by the gate check -- this is GitHub #300 exactly: the XML is "
        "the sole evidence and it carried none")


def test_the_evidence_names_which_shard_died_and_its_exit_code(mod):
    """A gate refusing is necessary but not sufficient — a human must be able
    to act on the artifact without the console log that is already gone."""
    root = _green_aggregate()
    mod.inject_teardown_crash_evidence(root, [(7, 3221225477)])
    errs = [tc for tc in root.iter("testcase") if tc.find("error") is not None]
    assert len(errs) == 1
    name = errs[0].get("name", "")
    assert "7" in name and "3221225477" in name, (
        "the synthetic case should name the shard and its exit code; got %r"
        % name)
    err = errs[0].find("error")
    assert err.get("type") == "teardown-crash", (
        "the error type should distinguish this from an ordinary assertion "
        "failure, so a reader is not sent looking at the wrong thing")
    assert "destruction order" in (err.get("message") or ""), (
        "the message should point at where the defect actually lives -- a "
        "teardown crash is not an assertion problem")


def test_the_root_totals_are_updated_not_just_the_elements(mod):
    """GAP-A15 cuts both ways.

    Some readers trust the summary attributes over the elements. If only the
    elements were added, such a reader would still see `errors="0"` and accept
    the artifact -- the same false-green through a different door.
    """
    root = _green_aggregate()
    before_tests = int(root.get("tests"))
    before_errors = int(root.get("errors"))
    mod.inject_teardown_crash_evidence(root, [(7, 3221225477), (9, 1)])
    assert int(root.get("errors")) == before_errors + 2, (
        "root errors= was not updated; an attribute-trusting reader would "
        "still call this clean")
    assert int(root.get("tests")) == before_tests + 2, (
        "root tests= was not updated, so tests != passed+failed+skipped+errors "
        "and the no-silent-truncation invariant would flag the artifact")


def test_two_crashed_shards_are_both_recorded(mod):
    """One incident per dead process, not one for the run.

    If two shards died, collapsing them to a single case would under-report and
    the second would be invisible to whoever picks this up.
    """
    root = _green_aggregate()
    n = mod.inject_teardown_crash_evidence(root, [(2, 5), (11, 3221225477)])
    assert n == 2
    names = sorted(tc.get("name") for tc in root.iter("testcase")
                   if tc.find("error") is not None)
    assert any("shard_2_" in x for x in names) and \
           any("shard_11_" in x for x in names), names


# ---------------------------------------------------------- port fidelity
#
# These two cases do not test `inject_teardown_crash_evidence` at all. They
# test that `_gate_would_accept` is a faithful port of the hook, by driving the
# two mechanisms APART -- an artifact where the attributes and the elements
# disagree. The previous element-counting version passed every other case in
# this file while failing both of these, which is exactly why they are here.

def test_the_gate_port_follows_ATTRIBUTES_when_elements_look_clean():
    """Suite says errors="1"; no error ELEMENT anywhere. The gate REFUSES.

    This is the direction that made the old version wrong: a regression that
    updated the elements but not the attributes would leave the real gate
    rejecting a run this test called fine.
    """
    root = _green_aggregate()
    suite = root.find("testsuite")
    suite.set("errors", "1")
    assert not list(root.iter("error")), "fixture must have NO error element"
    assert not _gate_would_accept(root), (
        "the port ignored a suite-level errors=1 because no <error> element "
        "was present; the real gate sums attributes and would have refused")


def test_the_gate_port_follows_ATTRIBUTES_when_elements_look_dirty():
    """An error ELEMENT with attributes left at zero. The gate ACCEPTS.

    Uncomfortable, and correct: it is what the hook does. Asserting the
    comfortable answer here is how the port silently stops being a port. If
    this behaviour is ever judged wrong, the fix belongs in the HOOK, and this
    test should then be updated to follow it -- not relaxed.
    """
    root = _green_aggregate()
    case = root.find("testsuite").find("testcase")
    ET.SubElement(case, "error", {"message": "m", "type": "t"})
    assert _gate_would_accept(root), (
        "the port rejected an artifact the real gate accepts, so it is no "
        "longer the same decision and proves nothing about the gate")


def test_the_root_attribute_fallback_is_ported_too():
    """`tt == 0` -> fall back to the ROOT's attributes.

    A `<testsuites>` wrapper whose suites carry no `tests` attribute is a real
    shape, and the hook handles it with an explicit fallback. A port missing
    that branch would call such an artifact "0 tests" and refuse every one of
    them -- a false RED, which erodes trust in the gate as surely as a false
    green.
    """
    root = ET.Element("testsuites", {"tests": "2", "failures": "0",
                                     "errors": "0"})
    suite = ET.SubElement(root, "testsuite", {"name": "S"})  # no tests= attr
    for n in ("A", "B"):
        ET.SubElement(suite, "testcase", {"name": n, "classname": "S",
                                          "status": "run"})
    assert _gate_would_accept(root), (
        "the root-attribute fallback is missing from the port, so a suite "
        "without a tests= attribute reads as an empty run")
    # ...and the fallback must carry the failure counts too, not just `tests`.
    root.set("errors", "1")
    assert not _gate_would_accept(root), (
        "the fallback read root tests= but not root errors=, so a crashed run "
        "in this shape would be accepted")
