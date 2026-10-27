# -*- coding: utf-8 -*-
"""GitHub #301 — a UI file whose application crashed must not report PASS.

THE DEFECT, as measured during the p96 phase-7 re-measurement. `pdfe.exe`
faulted and wrote a 46 MB minidump 9 s into
`test_final_phase8[annot_select_delete]`, and the harness reported
**11 passed, 0 failed** for that file. The merged JUnit was green. Nothing in
the headline numbers moved.

The harness was not blind. `ui-suite-summary.json` carried
`"app_crashed": true` and `"unhealable_app_crash": true`, and
`file_has_problem()` counted the file among `hidden problems: 17`. #265 had
already made sure a crash was not *healed* by a passing retry. What was missing
was the last step: turning that knowledge into a FAILURE in the artifact a
reader or a gate consumes. So the information existed and the verdict did not
— the same shape as #288 (a dead shard) and #300 (a teardown crash).

WHAT THESE CASES PIN. Not "the function appends an element". They reintroduce
the crash condition — a per-file JUnit that is completely green, alongside a
result dict marked `app_crashed`, which is byte-for-byte the situation above —
drive the REAL `merge_junits` over it, read the merged artifact back **off
disk**, and assert the reader's decision flips. Two artifacts differing only in
whether the application faulted must not get the same verdict.

The paired green case is not decoration: a fix that reddens healthy runs is a
fix that gets switched off (`noisy_guard_masks_everything`), so the specificity
case is what makes the sensitivity case worth anything.
"""
from __future__ import annotations

import importlib.util
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RUN_UI_SUITE = ROOT / "tests" / "scripts" / "run_ui_suite.py"


def _load():
    spec = importlib.util.spec_from_file_location("run_ui_suite_acv",
                                                  RUN_UI_SUITE)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["run_ui_suite_acv"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mod():
    return _load()


# ---------------------------------------------------------------- fixtures

#: The file that actually crashed, and the count it actually reported.
CRASHED_FILE = "tests/ui_automation/test_final_phase8.py"
REPORTED_PASSES = 11


def _green_per_file_xml(path: Path, name: str, passes: int) -> Path:
    """A per-file JUnit exactly as pytest leaves it when every test passed.

    This is the crux of #301. The crash is NOT visible here and cannot be:
    pytest's own tests all passed, and the process that faulted was `pdfe.exe`,
    a separate process the report has no vocabulary for. Any fix that expects
    to find the crash in this document is looking in the wrong place.
    """
    root = ET.Element("testsuites")
    suite = ET.SubElement(root, "testsuite", {
        "name": "pytest", "tests": str(passes), "failures": "0",
        "errors": "0", "skipped": "0", "time": "90.0",
    })
    for i in range(passes):
        ET.SubElement(suite, "testcase", {
            "classname": name, "name": f"test_case_{i}",
            "status": "run", "time": "8.0",
        })
    ET.ElementTree(root).write(str(path), encoding="utf-8",
                              xml_declaration=True)
    return path


def _result(file: str, *, crashed: bool, healed: bool = False) -> dict:
    """A per-file result dict shaped like the one `run_one` produces."""
    r = {
        "file": file,
        "returncode": 0,       # pytest itself succeeded -- it did
        "timed_out": False,
        "elapsed_s": 90.0,
    }
    if crashed:
        # Both keys, as the retry loop sets them (#265): a fault anywhere in
        # the sequence marks the file, healed or not.
        r["app_crashed"] = True
        r["unhealable_app_crash"] = True
        r["app_crash_dumps"] = ["dumps/attempt1/pdfe.exe.1234.dmp"]
        if healed:
            r["healed_on_retry"] = True
    return r


def _reader_would_call_it_passing(merged_path: Path) -> bool:
    """The decision any JUnit consumer makes, counted from ELEMENTS.

    Elements rather than the summary attributes because GAP-A15 in this project
    is precisely that the two can disagree; asserting against the stricter of
    the pair means a pass here cannot be an artefact of a different defect.
    `test_the_root_totals_move_too` covers the attribute-trusting reader.
    """
    root = ET.parse(str(merged_path)).getroot()
    cases = list(root.iter("testcase"))
    bad = [tc for tc in cases
           if tc.find("failure") is not None or tc.find("error") is not None]
    return not bad and len(cases) > 0


# ---------------------------------------------------------------- the cases

def test_a_clean_run_is_still_reported_as_passing(mod, tmp_path):
    """SPECIFICITY. The control that stops every case below being vacuous.

    If this fails, the fix reddens runs where nothing crashed, and the right
    response is to fix the fix -- not to relax the assertion below.
    """
    xml = _green_per_file_xml(tmp_path / "a.xml", CRASHED_FILE, REPORTED_PASSES)
    merged = tmp_path / "merged.xml"
    totals = mod.merge_junits([xml], merged,
                              [_result(CRASHED_FILE, crashed=False)])
    assert _reader_would_call_it_passing(merged), (
        "a run with no application fault must still read as passing")
    assert totals["errors"] == 0, totals
    assert totals["tests"] == REPORTED_PASSES, totals


def test_a_crash_under_a_green_report_is_NOT_reported_as_passing(mod,
                                                                 tmp_path):
    """SENSITIVITY — GitHub #301 itself.

    Identical green per-file report; the only difference is that the
    application faulted. That is the #301 artifact reproduced.
    """
    xml = _green_per_file_xml(tmp_path / "a.xml", CRASHED_FILE, REPORTED_PASSES)
    merged = tmp_path / "merged.xml"

    # Precondition: prove the input really is green, so the assertion after it
    # is measuring the fix and not a malformed fixture.
    pre = tmp_path / "pre.xml"
    mod.merge_junits([xml], pre, [])
    assert _reader_would_call_it_passing(pre), (
        "precondition: the per-file report must start green, as pytest left it")

    mod.merge_junits([xml], merged, [_result(CRASHED_FILE, crashed=True)])
    assert not _reader_would_call_it_passing(merged), (
        "pdfe.exe faulted and the merged junit still read as PASS -- this is "
        "GitHub #301 exactly: the harness knew (app_crashed: true) and the "
        "verdict did not")


def test_a_crash_that_passed_on_retry_is_also_not_passing(mod, tmp_path):
    """#265's rule must hold through to the verdict, not just the summary.

    A crash that does not recur on a fresh launch is the signature of an
    order-dependent fault, not evidence of a flake. If the retry could heal it
    here, the fix would be defeated by the one path most likely to be taken.
    """
    xml = _green_per_file_xml(tmp_path / "a.xml", CRASHED_FILE, REPORTED_PASSES)
    merged = tmp_path / "merged.xml"
    mod.merge_junits([xml], merged,
                     [_result(CRASHED_FILE, crashed=True, healed=True)])
    assert not _reader_would_call_it_passing(merged), (
        "a file that PASSED on retry but faulted pdfe.exe read as PASS; #265 "
        "says a fault anywhere in the sequence keeps the file a problem")


def test_the_evidence_names_the_file_whose_app_crashed(mod, tmp_path):
    """A red artifact is necessary but not sufficient.

    Whoever picks this up has the artifact and not the console log, so the
    artifact has to say which file was driving and where the dump is.
    """
    xml = _green_per_file_xml(tmp_path / "a.xml", CRASHED_FILE, REPORTED_PASSES)
    merged = tmp_path / "merged.xml"
    mod.merge_junits([xml], merged, [_result(CRASHED_FILE, crashed=True)])

    root = ET.parse(str(merged)).getroot()
    errs = [tc for tc in root.iter("testcase") if tc.find("error") is not None]
    assert len(errs) == 1, ET.tostring(root)
    assert "test_final_phase8" in errs[0].get("name", ""), errs[0].get("name")
    err = errs[0].find("error")
    assert err.get("type") == "app-crash", (
        "the type must distinguish this from an assertion failure, or a reader "
        "is sent looking at the test instead of at the application")
    msg = err.get("message") or ""
    assert CRASHED_FILE in msg and "dump" in msg.lower(), msg


def test_the_root_totals_move_too(mod, tmp_path):
    """GAP-A15 cuts both ways.

    Some readers trust `errors=` on the root over walking the elements. If only
    the elements were added, such a reader would still see `errors="0"` -- the
    same false green through the other door.
    """
    xml = _green_per_file_xml(tmp_path / "a.xml", CRASHED_FILE, REPORTED_PASSES)
    merged = tmp_path / "merged.xml"
    totals = mod.merge_junits([xml], merged,
                              [_result(CRASHED_FILE, crashed=True)])
    assert totals["errors"] == 1, totals
    assert totals["tests"] == REPORTED_PASSES + 1, (
        "root tests= must count the synthetic case too, or tests != "
        "passed+failed+skipped+errors and the no-silent-truncation invariant "
        "would flag the artifact: %r" % (totals,))
    root = ET.parse(str(merged)).getroot()
    assert root.get("errors") == "1", ET.tostring(root)[:400]


def test_two_crashed_files_are_both_recorded(mod, tmp_path):
    """One incident per crashed application, not one for the run."""
    a = _green_per_file_xml(tmp_path / "a.xml", CRASHED_FILE, 2)
    b = _green_per_file_xml(tmp_path / "b.xml", "tests/ui_automation/t2.py", 2)
    merged = tmp_path / "merged.xml"
    totals = mod.merge_junits(
        [a, b], merged,
        [_result(CRASHED_FILE, crashed=True),
         _result("tests/ui_automation/t2.py", crashed=True)])
    assert totals["errors"] == 2, totals
    names = sorted(tc.get("name") for tc in ET.parse(str(merged)).getroot()
                   .iter("testcase") if tc.find("error") is not None)
    assert len(names) == 2 and len(set(names)) == 2, (
        "the two crashes collapsed into one case, so the second is invisible "
        "to whoever reads this: %r" % (names,))


def test_the_predicate_the_harness_uses_already_agreed(mod):
    """Shows where the gap WAS, so this file documents the defect's shape.

    `file_has_problem` returned True for a crashed file long before #301 --
    the knowledge was never the missing piece. Pinning it here means a future
    change that weakens the predicate fails loudly rather than silently
    disarming the fix above, which reads the same keys.
    """
    assert mod.file_has_problem(_result(CRASHED_FILE, crashed=True)), (
        "file_has_problem must keep treating a crashed file as a problem; the "
        "#301 fix reads the same keys and is defeated if this changes")
    assert not mod.file_has_problem(_result(CRASHED_FILE, crashed=False))
