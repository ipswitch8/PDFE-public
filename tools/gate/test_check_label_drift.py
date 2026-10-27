# -*- coding: utf-8 -*-
"""Tests for check_label_drift.py.

The guard's own `--self-test` covers its logic against fixtures. These cases add
the two things a self-test cannot give you:

  * that the guard is green against the REAL tree as committed, so it is a live
    control rather than a fixture exercise, and
  * that it goes RED on the real tree when the historical defect is reintroduced
    -- a guard no mutation can redden is decoration, and this project has shipped
    that mistake before (see DCA 0035, 0037).
"""
from __future__ import annotations

import io
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GUARD = os.path.join("tools", "gate", "check_label_drift.py")
INSTALLER = os.path.join(ROOT, "tools", "runner-deployment", "Install-PdfeRunner.ps1")

sys.path.insert(0, HERE)
import check_label_drift as mod  # noqa: E402


def _run(*args):
    return subprocess.run([sys.executable, GUARD, *args], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace")


FULL = {"self-hosted", "Windows", "X64", "pdfe-windows", "pdfe-builder"}
AGREE = {
    "runner-requirements.json:target_labels": set(FULL),
    "Install-PdfeRunner.ps1": set(FULL),
    "runner-deployment/README.md": set(FULL),
}
JOBS = [
    ("windows-validation.yml", "build", set(FULL)),
    ("governance.yml", "governance",
     {"self-hosted", "Windows", "X64", "pdfe-windows"}),
]


def test_self_test_passes():
    r = _run("--self-test")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "self-test:" in r.stdout


def test_green_against_the_real_tree():
    """Not a fixture: the guard must pass on the repository as committed."""
    r = _run()
    assert r.returncode == 0, r.stdout + r.stderr


def test_agreeing_declarations_pass():
    assert mod.check(AGREE, JOBS) == []


def test_a_declaration_missing_a_label_is_caught():
    drifted = dict(AGREE)
    drifted["Install-PdfeRunner.ps1"] = FULL - {"pdfe-builder"}
    problems = mod.check(drifted, JOBS)
    assert any("Install-PdfeRunner.ps1" in p and "pdfe-builder" in p
               for p in problems), problems


def test_a_job_needing_an_unregistered_label_is_caught():
    narrow = {k: (FULL - {"pdfe-builder"}) for k in AGREE}
    problems = mod.check(narrow, JOBS)
    assert any("build" in p and "pdfe-builder" in p for p in problems), problems


def test_the_ephemeral_hetzner_set_is_exempt():
    heth = [("hetzner-sanitizer.yml", "sanitize",
             {"self-hosted", "linux", "hetzner", "sanitizer"})]
    assert mod.check(AGREE, heth) == []


def test_a_typod_ephemeral_set_is_not_exempt():
    """The exemption is an EXACT set match, so a near-miss must still fail.

    An allowlist keyed on 'looks a bit like the Hetzner one' would let a real
    unsatisfiable runs-on through under cover of the exemption.
    """
    typo = [("hetzner-sanitizer.yml", "sanitize",
             {"self-hosted", "linux", "hetzner", "sanitiser"})]
    assert any("sanitiser" in p for p in mod.check(AGREE, typo))


def test_empty_models_fail_rather_than_pass_vacuously():
    assert mod.check({}, JOBS)
    assert mod.check(AGREE, [])


def test_it_reddens_when_the_historical_defect_is_reintroduced():
    """Mutate the REAL installer the way the original defect had it, and require
    the guard to fail. Restored in a finally, so a failure here cannot leave the
    tree mutated for the next test."""
    original = io.open(INSTALLER, "rb").read()
    mutated = original.replace(
        b"self-hosted,Windows,X64,pdfe-windows,pdfe-builder",
        b"self-hosted,Windows,X64,pdfe-windows", 1)
    assert mutated != original, (
        "could not reintroduce the defect -- the installer no longer contains "
        "the full label string, so this test is not measuring what it claims")
    try:
        io.open(INSTALLER, "wb").write(mutated)
        r = _run()
        assert r.returncode == 1, (
            "the guard stayed GREEN with pdfe-builder dropped from the "
            "installer, which is the exact defect it exists to catch:\n"
            + r.stdout + r.stderr)
        assert "pdfe-builder" in (r.stdout + r.stderr)
    finally:
        io.open(INSTALLER, "wb").write(original)
    assert _run().returncode == 0, "tree not restored after the mutation"


# ---------------------------------------------------------------------------
# The hole the security-audit gate FAILed phase-2 for.
#
# The guard's first design took the WIDEST `--labels` string it could find in a
# file. `tools/runner-deployment/README.md` carries two such strings (the
# registration block and the Nth-runner walkthrough), so narrowing ONE of them
# left the other to mask it: the guard stayed green while an operator following
# the narrowed instruction registered a machine that silently could not build.
#
# The fix is a POSITIVE marker naming the canonical command. These cases pin it,
# against the real files rather than fixtures, because the failure was a property
# of the real files' redundancy.
# ---------------------------------------------------------------------------
README = os.path.join(ROOT, "tools", "runner-deployment", "README.md")


def test_the_real_files_each_name_exactly_one_canonical_command():
    for path in (INSTALLER, README):
        text = io.open(path, encoding="utf-8", errors="replace").read()
        n = text.count(mod.DEFAULT_MARKER)
        assert n == 1, (
            "%s carries %d %s markers; exactly one command may be canonical, or a "
            "narrowed default could hide behind a wider sibling"
            % (os.path.basename(path), n, mod.DEFAULT_MARKER))


def test_a_wider_unmarked_sibling_cannot_mask_a_narrowed_canonical():
    """Narrow ONLY the marked command in the README; leave the wider unmarked
    one alone. Under the old widest-wins extraction this was invisible."""
    original = io.open(README, "rb").read()
    text = original.decode("utf-8")
    at = text.index(mod.DEFAULT_MARKER)
    head, tail = text[:at], text[at:]
    narrowed = tail.replace(
        "self-hosted,Windows,X64,pdfe-windows,pdfe-builder",
        "self-hosted,Windows,X64,pdfe-windows", 1)
    assert narrowed != tail, "could not narrow the marked command; test is inert"
    # The wider sibling must still be present, or this proves nothing.
    assert "self-hosted,Windows,X64,pdfe-windows,pdfe-builder" in narrowed, (
        "the wider unmarked sibling is gone, so this is not the masking scenario")
    try:
        io.open(README, "wb").write((head + narrowed).encode("utf-8"))
        r = _run()
        assert r.returncode == 1, (
            "a narrowed CANONICAL command was masked by a wider unmarked "
            "sibling -- the widest-wins hole is back:\n" + r.stdout + r.stderr)
        assert "README.md" in (r.stdout + r.stderr)
    finally:
        io.open(README, "wb").write(original)
    assert _run().returncode == 0, "tree not restored after the mutation"


def test_a_missing_marker_fails_rather_than_guessing():
    problems = []
    assert mod._marked_label_set("  --labels a,b\n", "f", problems) is None
    assert problems


def test_two_markers_are_ambiguous_and_fail():
    problems = []
    two = ("# %s\n  --labels a,b\n# %s\n  --labels a,b,c\n"
           % (mod.DEFAULT_MARKER, mod.DEFAULT_MARKER))
    assert mod._marked_label_set(two, "f", problems) is None
    assert problems
