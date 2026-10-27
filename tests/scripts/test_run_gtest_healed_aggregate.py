# -*- coding: utf-8 -*-
"""GitHub #158 — the merged aggregate must reflect the reruns it precedes.

WHY THIS EXISTS. `gtest_full.xml` is written BEFORE failures are re-run, and was
never updated afterwards. A test that failed in the sharded run and passed in a
fresh process stayed recorded as `failures="1"` forever, so the harness printed

    all 1 failure(s)/error(s) healed on a fresh rerun. CLEAN.

while the artifact it had just produced said the opposite. Everything downstream
reads the artifact, not the console.

That is not cosmetic. Consumers that gate on a green JUnit XML saw a permanent
red, so a SINGLE healed flake anywhere in a ~6,000-test suite made the entire
run unusable as green evidence. It froze the pipeline ledger during
`backlog-workable-p82` phase 7: three gate artifacts read PASS on disk while the
ledger recorded all three as `remediation`, and nothing reported why.

The fix must clear the failure WITHOUT hiding it -- otherwise it trades a
false red for a false green, which is strictly worse. These cases pin both
halves, plus the parts that are easy to over-fix: an unresolved failure and a
never-run test must both stay RED.
"""
from __future__ import annotations

import importlib.util
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "run_gtest_agg_under_test", ROOT / "tests" / "scripts" / "run_gtest.py")
assert _SPEC and _SPEC.loader
run_gtest = importlib.util.module_from_spec(_SPEC)
sys.modules["run_gtest_agg_under_test"] = run_gtest
_SPEC.loader.exec_module(run_gtest)


def _aggregate(cases):
    """Build a testsuites/testsuite tree; cases = [(classname, name, failed)]."""
    root = ET.Element("testsuites", {"tests": "0", "failures": "0",
                                     "errors": "0", "skipped": "0"})
    by_suite: dict[str, ET.Element] = {}
    for classname, name, failed in cases:
        suite = by_suite.get(classname)
        if suite is None:
            suite = ET.SubElement(root, "testsuite",
                                  {"name": classname, "tests": "0",
                                   "failures": "0", "errors": "0",
                                   "skipped": "0"})
            by_suite[classname] = suite
        case = ET.SubElement(suite, "testcase",
                             {"name": name, "classname": classname,
                              "status": "run"})
        if failed:
            f = ET.SubElement(case, "failure", {"message": "boom"})
            f.text = "expected 1 got 0"
        suite.set("tests", str(int(suite.get("tests")) + 1))
        if failed:
            suite.set("failures", str(int(suite.get("failures")) + 1))
    root.set("tests", str(sum(int(s.get("tests")) for s in by_suite.values())))
    root.set("failures",
             str(sum(int(s.get("failures")) for s in by_suite.values())))
    return root


def _counts(root):
    return (int(root.get("failures", "0")), int(root.get("errors", "0")))


def test_healed_test_clears_the_failure_counts():
    root = _aggregate([("Suite", "Flaky", True), ("Suite", "Solid", False)])
    assert _counts(root) == (1, 0)

    n = run_gtest.apply_healed_to_aggregate(root, ["Suite.Flaky"])

    assert n == 1
    assert _counts(root) == (0, 0), (
        "the aggregate must go green once the only failure healed -- that is "
        "the whole defect")
    suite = root.find("testsuite")
    assert suite.get("failures") == "0", "suite counters must move with the root"


def test_healing_is_recorded_not_erased():
    """A healed test must stay distinguishable from one that passed first time.

    Clearing the counter without leaving a trace would trade a false red for a
    false green.
    """
    root = _aggregate([("Suite", "Flaky", True)])
    run_gtest.apply_healed_to_aggregate(root, ["Suite.Flaky"])

    case = root.find("./testsuite/testcase")
    assert case.get("healed-on-rerun") == "true"
    note = case.find("system-out")
    assert note is not None and "HEALED ON RERUN" in (note.text or "")
    assert case.find("failure") is None, "the failure element itself is cleared"


def test_an_unhealed_failure_stays_red():
    root = _aggregate([("Suite", "Flaky", True), ("Suite", "Broken", True)])

    n = run_gtest.apply_healed_to_aggregate(root, ["Suite.Flaky"])

    assert n == 1
    assert _counts(root) == (1, 0), (
        "only the healed test may be cleared; a genuine failure must survive")
    broken = [c for c in root.iter("testcase") if c.get("name") == "Broken"][0]
    assert broken.find("failure") is not None


def test_a_healed_id_that_is_absent_is_reported_not_silently_counted():
    """The caller compares the return value against the healed list.

    A rewrite that silently found nothing would leave the artifact red while
    claiming success -- the same class of defect, one level up.
    """
    root = _aggregate([("Suite", "Solid", False)])

    n = run_gtest.apply_healed_to_aggregate(root, ["Suite.NotHere"])

    assert n == 0
    assert _counts(root) == (0, 0)


def test_running_twice_does_not_drive_counters_negative():
    """Idempotence. A second pass finds no failure element and must no-op."""
    root = _aggregate([("Suite", "Flaky", True)])
    run_gtest.apply_healed_to_aggregate(root, ["Suite.Flaky"])
    n2 = run_gtest.apply_healed_to_aggregate(root, ["Suite.Flaky"])

    assert n2 == 0
    assert _counts(root) == (0, 0)
    assert int(root.find("testsuite").get("failures")) == 0


def test_errors_are_cleared_as_well_as_failures():
    root = _aggregate([("Suite", "Crashy", False)])
    case = root.find("./testsuite/testcase")
    ET.SubElement(case, "error", {"message": "shard died"})
    root.find("testsuite").set("errors", "1")
    root.set("errors", "1")

    n = run_gtest.apply_healed_to_aggregate(root, ["Suite.Crashy"])

    assert n == 1
    assert _counts(root) == (0, 0)


def test_empty_healed_list_is_a_no_op():
    root = _aggregate([("Suite", "Broken", True)])
    assert run_gtest.apply_healed_to_aggregate(root, []) == 0
    assert _counts(root) == (1, 0)


# ---------------------------------------------------------------------------
# GitHub #264 -- the heal must not destroy the reason.
#
# Clearing the <failure> without keeping its text removed the only record of WHY
# a test failed, which is exactly the assertion-versus-crash distinction. The
# audit of the three healed incidents in committed evidence could only be
# completed because two of them happened to be quoted by hand in a notes.md.
# ---------------------------------------------------------------------------
def test_the_healed_note_preserves_the_original_failure_text():
    import xml.etree.ElementTree as ET
    import run_gtest as rg
    xml = ('<testsuites tests="1" failures="1"><testsuite name="S" tests="1" '
           'failures="1"><testcase classname="S" name="t">'
           '<failure message="SEH exception with code 0xc0000005 thrown in '
           'the test body."/></testcase></testsuite></testsuites>')
    root = ET.fromstring(xml)
    assert rg.apply_healed_to_aggregate(root, ["S.t"]) == 1
    case = root.find(".//testcase")
    assert case.get("healed-on-rerun") == "true"
    note = case.find("system-out").text
    assert "ORIGINAL failure" in note, note
    assert "0xc0000005" in note, (
        "the crash message was discarded; the artifact can no longer tell an "
        "assertion failure from an access violation")


def test_an_assertion_message_is_preserved_too():
    import xml.etree.ElementTree as ET
    import run_gtest as rg
    xml = ('<testsuites tests="1" failures="1"><testsuite name="S" tests="1" '
           'failures="1"><testcase classname="S" name="t">'
           '<failure message="Expected equality of these values: 1 and 2"/>'
           '</testcase></testsuite></testsuites>')
    root = ET.fromstring(xml)
    rg.apply_healed_to_aggregate(root, ["S.t"])
    note = root.find(".//testcase/system-out").text
    assert "Expected equality" in note, note
