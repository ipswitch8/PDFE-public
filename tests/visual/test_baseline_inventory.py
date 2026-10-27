# -*- coding: utf-8 -*-
"""Executed, headless checks over the visual-baseline set (A1-66 / A1-100).

This module is the honest counterpart to ``test_visual_baselines.py``: every
test here RUNS on any lane — no PDFE, no display, no DPI gate, no xfail — so
the visual-baseline situation is asserted rather than asserted-about.

Three things are locked in:

1. **The seeded/not-seeded ledger is exact.** 8 of the 14 per-subsystem canvas
   baselines are seeded; 6 hold only a ``.gitkeep`` and therefore guard
   nothing. Both directions fail: silently dropping a seeded baseline, and
   silently adding one without updating the ledger.
2. **The deterministic goldens really are deterministic and really do match.**
   ``baseline/reference/*.png`` are regenerated from the committed generator
   into a temp directory and compared byte-for-byte with the committed files —
   the one part of the visual set that can be verified without the live UI, so
   it is verified for real.
3. **Absences are reported as NOT-RUN, not as green.** The live-UI-gated module
   routes its skips through :mod:`tests.notrun`.
"""
from __future__ import annotations

import importlib.util
import io
import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.visual import baseline_inventory as inv  # noqa: E402


#: The exact, current truth. Any drift in either direction fails.
EXPECTED_NOT_SEEDED = {
    "annotations", "forms", "image_edit", "page_ops", "redaction", "text_edit",
}
EXPECTED_SEEDED = {
    "chrome", "compliance", "dialogs", "encryption", "menus", "panels",
    "signatures", "visual_fidelity",
}


def _generator():
    path = (REPO_ROOT / "tests" / "fixtures" / "ui_screenshots"
            / "generate_reference_goldens.py")
    spec = importlib.util.spec_from_file_location(
        "pdfe_generate_reference_goldens", path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# 1. The ledger is exact
# ---------------------------------------------------------------------------

def test_subsystem_baseline_ledger_is_exact():
    """MUTATION KILLED: deleting a committed canvas baseline (the set of
    seeded subsystems shrinks), or adding an empty subsystem dir and letting
    it read as covered. Also kills 'fix' by .gitkeep-deletion: removing the
    marker without adding a PNG still leaves the subsystem unseeded."""
    subs = [s for s in inv.scan_subsystem_baselines() if not s.generated]
    seeded = {s.subsystem for s in subs if s.seeded}
    not_seeded = {s.subsystem for s in subs if not s.seeded}
    assert seeded == EXPECTED_SEEDED, (
        f"seeded set drifted: added={sorted(seeded - EXPECTED_SEEDED)} "
        f"removed={sorted(EXPECTED_SEEDED - seeded)}")
    assert not_seeded == EXPECTED_NOT_SEEDED, (
        f"unseeded set drifted: added={sorted(not_seeded - EXPECTED_NOT_SEEDED)} "
        f"fixed={sorted(EXPECTED_NOT_SEEDED - not_seeded)} — if a subsystem "
        f"was genuinely seeded, move it to EXPECTED_SEEDED")


def test_reported_ratio_matches_the_audit_claim():
    """The audit row (HM-20 / A1-100) says 8 of 14. Assert the number the
    inventory reports, so any report quoting it is quoting the filesystem.
    MUTATION KILLED: an inventory that counts the generated 'reference' or
    harness 'smoke' dirs as subsystem coverage, inflating the ratio."""
    s = inv.summarise()
    assert (s["subsystem_seeded"], s["subsystem_total"]) == (8, 14)
    assert "reference" in s["generated_subsystems"]


def test_unseeded_directories_really_contain_no_png():
    """MUTATION KILLED: an inventory that reports NOT-SEEDED from a hard-coded
    list rather than from the filesystem."""
    for name in EXPECTED_NOT_SEEDED:
        d = inv.UI_SCREENSHOT_BASELINE_DIR / name
        assert d.is_dir(), f"{d} vanished"
        assert not list(d.glob("*.png")), (
            f"{d} now has PNGs — update EXPECTED_SEEDED/NOT_SEEDED")


def test_visual_plan_pngs_are_all_committed():
    """MUTATION KILLED: losing one of the five window-capture baselines that
    test_visual_baselines.py compares against — which, given
    ``_run_baseline_check`` SKIPS on a missing baseline, would otherwise be
    completely silent."""
    present = inv.scan_visual_baselines()
    missing = sorted(k for k, v in present.items() if not v)
    assert missing == [], f"missing committed visual baselines: {missing}"


# ---------------------------------------------------------------------------
# 2. The deterministic goldens are verified for real
# ---------------------------------------------------------------------------

def test_committed_reference_goldens_match_the_generator_byte_for_byte():
    """The only visual baselines that can be checked WITHOUT the live UI —
    so check them, on every lane.

    MUTATION KILLED: editing a committed reference PNG by hand (or letting a
    capture overwrite it), and changing the generator spec without
    regenerating the committed goldens. Either way the bytes diverge.
    """
    gen = _generator()
    dest_dir = gen.baseline_dir()
    checked = 0
    for name, render in gen.REFERENCES.items():
        committed = dest_dir / f"{name}.png"
        assert committed.is_file(), f"reference golden missing: {committed}"
        buf = io.BytesIO()
        render().save(buf, format="PNG")
        assert buf.getvalue() == committed.read_bytes(), (
            f"committed reference golden {name}.png does not match what "
            f"generate_reference_goldens.py produces — regenerate it "
            f"deliberately, or revert the hand edit")
        checked += 1
    assert checked == 4, f"expected 4 reference goldens, checked {checked}"


def test_generator_is_deterministic_across_invocations():
    """MUTATION KILLED: introducing a timestamp, RNG or environment read into
    the generator, which would make 'regenerate and compare' meaningless."""
    gen = _generator()
    for name, render in gen.REFERENCES.items():
        a, b = io.BytesIO(), io.BytesIO()
        render().save(a, format="PNG")
        render().save(b, format="PNG")
        assert a.getvalue() == b.getvalue(), f"{name} render is not stable"


def test_defect_classes_are_detectably_different_from_the_goldens():
    """A golden that its own defect-mutants match would guard nothing.
    MUTATION KILLED: replacing a reference spec with a flat/symmetric image
    (a mirror of which is identical to the original)."""
    gen = _generator()
    for name, render in gen.REFERENCES.items():
        base = render()
        for defect, mutate in gen.DEFECTS.items():
            mutated = mutate(base)
            same = (mutated.size == base.size
                    and mutated.convert("RGB").tobytes()
                    == base.convert("RGB").tobytes())
            assert not same, (
                f"reference golden {name!r} is indistinguishable from its "
                f"{defect!r} mutant — it cannot detect that defect class")


# ---------------------------------------------------------------------------
# 3. Absence is reported as NOT-RUN
# ---------------------------------------------------------------------------

def test_live_ui_visual_module_reports_not_run_rather_than_skipping_quietly():
    """MUTATION KILLED: reverting the live-UI/DPI gates in
    test_visual_baselines.py to bare ``pytest.skip``/``skipif`` reasons, which
    render as an ordinary green session."""
    text = (REPO_ROOT / "tests" / "visual"
            / "test_visual_baselines.py").read_text(encoding="utf-8")
    assert "tests.notrun" in text or "from tests import notrun" in text
    assert "NOT_RUN" in text or "format_reason" in text or \
        "skip_not_run" in text


def test_inventory_report_names_the_unseeded_subsystems():
    """The report a human reads must name what is NOT covered.
    MUTATION KILLED: a summary that prints only the ratio."""
    report = inv.format_report()
    for name in EXPECTED_NOT_SEEDED:
        assert name in report
    assert "NOT-SEEDED" in report
    assert "NOT covered" in report
