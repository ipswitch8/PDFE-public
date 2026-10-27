# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_f45_ci_only_markers.py (F45, phase-46).

`pytest.ini` sets `testpaths = tests`, so a self-test under `tools/gate/` runs
NOWHERE unless it is named explicitly in the *"Gate self-tests + matrix
pass-semantics"* step of `.github/workflows/fast-checks.yml`.
`test_selftest_is_named_in_fast_checks` below fails if that line is ever
removed -- the same trap F35, F37 and F43 each had to close.

The discriminating control is a SNAPSHOT of the real test sources with the
marker stripped: the checker must fail on it, per test, and pass on the tree as
it stands. A checker that could not be reddened would be decoration.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]

_spec = importlib.util.spec_from_file_location(
    "check_f45_ci_only_markers", _HERE / "check_f45_ci_only_markers.py"
)
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

FAST_CHECKS = _REPO / ".github" / "workflows" / "fast-checks.yml"


def _snapshot(tmp_path: Path) -> Path:
    """A minimal tree carrying only the four tests' sources."""
    root = tmp_path / "snap"
    for rel in set(mod.TRACKED.values()):
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(_REPO / rel, dst)
    return root


def _markers_only(root: Path):
    problems: list[str] = []
    mod.gate_markers(problems, root)
    return problems


# ------------------------------------------------------------------ live tree

def test_live_tree_passes():
    assert mod.run_all() == [], "\n  ".join(mod.run_all())


def test_cli_exit_zero_on_live_tree():
    r = subprocess.run(
        [sys.executable, str(_HERE / "check_f45_ci_only_markers.py")],
        cwd=str(_REPO), capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_exactly_the_four_named_tests_are_tracked():
    assert len(mod.TRACKED) == 4
    assert set(mod.TRACKED) == {
        "F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact",
        "FidelityR3_19_PdfaIndependent.ExternalPdfa_VeraPdfReference",
        "ConformanceIndependentTest.KnownGoodExternalPdfAIsAcceptedByChecker",
        "ConformanceIndependentTest.OwnValidatorAgreesWithIndependentChecker",
    }


def test_snapshot_with_markers_intact_passes(tmp_path):
    assert _markers_only(_snapshot(tmp_path)) == []


# --------------------------------------------------- discrimination, per test

@pytest.mark.parametrize("fq", sorted(mod.TRACKED))
def test_removing_one_marker_reddens_the_checker(tmp_path, fq):
    root = _snapshot(tmp_path)
    rel = mod.TRACKED[fq]
    p = root / rel
    text = p.read_text(encoding="utf-8")
    line = f"// {mod.MARKER}: {fq}"
    assert line in text, "the snapshot did not carry the marker to begin with"
    p.write_text(text.replace(line + "\n", ""), encoding="utf-8")

    problems = _markers_only(root)
    assert any(fq in q and ("M3" in q or "M4" in q) for q in problems), (
        f"stripping the marker from {fq} left the checker green -- it is not "
        f"per-test. Got: {problems}"
    )
    # The other three must NOT be implicated.
    for other in mod.TRACKED:
        if other == fq or mod.TRACKED[other] == rel:
            continue
        assert not any(other in q for q in problems)


def test_a_marker_naming_the_wrong_test_is_rejected(tmp_path):
    root = _snapshot(tmp_path)
    fq = "FidelityR3_19_PdfaIndependent.ExternalPdfa_VeraPdfReference"
    p = root / mod.TRACKED[fq]
    p.write_text(
        p.read_text(encoding="utf-8").replace(
            f"{mod.MARKER}: {fq}", f"{mod.MARKER}: Some.OtherTest"),
        encoding="utf-8")
    problems = _markers_only(root)
    assert any("M4" in q and fq in q for q in problems), problems


def test_a_marker_without_the_disposition_reference_is_rejected(tmp_path):
    root = _snapshot(tmp_path)
    fq = "F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact"
    p = root / mod.TRACKED[fq]
    p.write_text(
        p.read_text(encoding="utf-8").replace(mod.DISPOSITION_DOC, "somewhere.md"),
        encoding="utf-8")
    problems = _markers_only(root)
    assert any("M5" in q and fq in q for q in problems), problems


def test_renaming_a_tracked_test_is_caught(tmp_path):
    root = _snapshot(tmp_path)
    fq = "ConformanceIndependentTest.OwnValidatorAgreesWithIndependentChecker"
    p = root / mod.TRACKED[fq]
    p.write_text(
        p.read_text(encoding="utf-8").replace(
            "TEST_F(ConformanceIndependentTest, OwnValidatorAgreesWithIndependentChecker)",
            "TEST_F(ConformanceIndependentTest, OwnValidatorAgreesRenamed)"),
        encoding="utf-8")
    problems = _markers_only(root)
    assert any("M2" in q and fq in q for q in problems), problems


def test_a_marker_belonging_to_the_previous_test_is_not_credited(tmp_path):
    """The lookback must stop at the previous TEST macro.

    Otherwise a single marker at the top of a file would satisfy every test in
    it -- the "one marker, many tests" false green.
    """
    root = tmp_path / "one"
    rel = "tests/fake.cpp"
    p = root / rel
    p.parent.mkdir(parents=True)
    fq = "ConformanceIndependentTest.KnownGoodExternalPdfAIsAcceptedByChecker"
    p.write_text(
        f"// {mod.MARKER}: {fq}\n"
        f"// {mod.DISPOSITION_DOC}\n"
        "TEST_F(ConformanceIndependentTest, KnownGoodExternalPdfAIsAcceptedByChecker) {}\n"
        "TEST_F(ConformanceIndependentTest, OwnValidatorAgreesWithIndependentChecker) {}\n",
        encoding="utf-8")
    problems: list[str] = []
    saved = dict(mod.TRACKED)
    try:
        mod.TRACKED.clear()
        mod.TRACKED.update({
            fq: rel,
            "ConformanceIndependentTest.OwnValidatorAgreesWithIndependentChecker": rel,
        })
        mod.gate_markers(problems, root)
    finally:
        mod.TRACKED.clear()
        mod.TRACKED.update(saved)
    assert any("OwnValidatorAgreesWithIndependentChecker" in q and "M3" in q
               for q in problems), problems
    assert not any("KnownGoodExternalPdfAIsAcceptedByChecker" in q for q in problems)


def test_a_far_away_marker_is_not_credited(tmp_path):
    root = tmp_path / "far"
    rel = "tests/far.cpp"
    p = root / rel
    p.parent.mkdir(parents=True)
    fq = "F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact"
    p.write_text(
        f"// {mod.MARKER}: {fq}\n"
        f"// {mod.DISPOSITION_DOC}\n"
        + "// filler\n" * (mod._LOOKBACK + 5)
        + "TEST(F19DocClaims, ComparisonDocUiE2eCountsMatchTheSummaryArtifact) {}\n",
        encoding="utf-8")
    problems: list[str] = []
    saved = dict(mod.TRACKED)
    try:
        mod.TRACKED.clear()
        mod.TRACKED[fq] = rel
        mod.gate_markers(problems, root)
    finally:
        mod.TRACKED.clear()
        mod.TRACKED.update(saved)
    assert any("M3" in q for q in problems), problems


# -------------------------------------------------- the fix's own mechanism

def test_both_reincluded_inputs_are_tracked_by_git():
    """M6 in the live tree. `git check-ignore` is deliberately NOT the probe --
    it exits 0 on a `!` re-include match and would report these as ignored."""
    out = subprocess.run(["git", "-C", str(_REPO), "ls-files"],
                         capture_output=True, text=True, check=True)
    tracked = set(out.stdout.replace("\\", "/").split("\n"))
    for rel in mod.REQUIRED_TRACKED_INPUTS:
        assert rel in tracked, f"{rel} is no longer tracked; F45's fix is reverted"


#: Every corpus file allowed to be tracked, named ONE BY ONE on purpose.
#:
#: The corpus is 71 entries totalling ~331 MiB, and several of them must never
#: be published from this repository -- a real business invoice bearing the
#: owner's domain, a commercially published book, 279 MB of construction
#: specifications for a named school, and vendor documents from HPE, PrinceXML
#: and Smallpdf. An exact list is the only form of this pin that keeps working:
#: a count, or a size budget, would silently accept a swap.
#:
#: To ADD one, all four must hold, and the reviewer should check all four:
#:   1. a test resolves it BY NAME (not a directory glob), so tracking it turns
#:      a silent GTEST_SKIP into a real assertion;
#:   2. its provenance is established and recorded;
#:   3. `tools/gate/scan_binary_path_hygiene.py` reports it clean -- the text
#:      detector cannot read a .pdf, so it has NOT covered it;
#:   4. the size is stated in the change, so repo growth stays deliberate.
ALLOWED_TRACKED_CORPUS = [
    # F45 / phase-46. 16 KB. Three tests hard-assert its existence before
    # consulting the conformance checker. CC-BY-SA-4.0, recorded in
    # THIRD-PARTY-NOTICES.md.
    "test_PDFs/crazyones-pdfa.pdf",
    # GitHub #189 (p87 phase-4). 77 KB. The only OTHER corpus file any test
    # resolves by name -- test_layout_extraction_corpus.cpp:332 and
    # test_fidelity_r3_07_text_extraction.cpp:169, one of which is the
    # project's only column-major reading-order oracle, which did not run on a
    # clean checkout and still read green. Synthetic LaTeX Lorem Ipsum
    # (pdfTeX-1.40.21, author field "Your Name"): no third-party content, no
    # personal data, no licence encumbrance. Scanned clean across the raw bytes
    # and all 11 inflated FlateDecode streams.
    "test_PDFs/multicolumn.pdf",
]


def test_the_reinclude_did_not_drag_in_the_rest_of_the_corpus():
    """A `!` re-include written as `!test_PDFs/` instead of per-file lines would
    commit a ~70-file binary corpus. Pin the blast radius.

    Equality against an explicit list, not a count: the point is that every
    tracked corpus file was named by a person who checked it, and a count would
    accept a substitution without noticing.
    """
    out = subprocess.run(["git", "-C", str(_REPO), "ls-files", "test_PDFs"],
                         capture_output=True, text=True, check=True)
    files = sorted(f for f in out.stdout.replace("\\", "/").split("\n") if f)
    assert files == sorted(ALLOWED_TRACKED_CORPUS), files

    out = subprocess.run(
        ["git", "-C", str(_REPO), "ls-files", "tests/_runtime"],
        capture_output=True, text=True, check=True)
    files = sorted(f for f in out.stdout.replace("\\", "/").split("\n") if f)
    assert files == ["tests/_runtime/.gitkeep",
                     "tests/_runtime/discovery_phase8/ui-suite-summary.json"], files


def test_missing_input_is_reported(tmp_path):
    """M6 discrimination: an empty tree has neither input tracked."""
    subprocess.run(["git", "-C", str(tmp_path), "init", "-q"], check=True)
    problems: list[str] = []
    mod.gate_inputs_are_tracked(problems, tmp_path)
    assert len(problems) == 3  # M7 + both inputs
    for rel in mod.REQUIRED_TRACKED_INPUTS:
        assert any(rel in q and "M6" in q for q in problems)


def test_disposition_doc_exists_and_is_tracked():
    assert (_REPO / mod.DISPOSITION_DOC).is_file()


# ---------------------------------------------------- pytest.ini testpaths trap

def test_selftest_is_named_in_fast_checks():
    text = FAST_CHECKS.read_text(encoding="utf-8")
    assert "tools/gate/test_check_f45_ci_only_markers.py" in text, (
        "pytest.ini sets `testpaths = tests`, so this file runs NOWHERE unless the "
        '"Gate self-tests" step names it.'
    )


def test_checker_itself_is_a_fast_checks_step():
    text = FAST_CHECKS.read_text(encoding="utf-8")
    assert "python tools/gate/check_f45_ci_only_markers.py" in text
