# -*- coding: utf-8 -*-
"""Self-tests for check_third_party_notices.py (F46).

A checker no mutation can redden is decoration, so every rule below is proved
against a deliberately broken input. The live tree is asserted GREEN separately,
which is the only assertion of the two that could pass vacuously -- hence the
mutations.
"""

from __future__ import annotations

import hashlib
import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]

_spec = importlib.util.spec_from_file_location(
    "check_third_party_notices", _HERE / "check_third_party_notices.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

NOTICES = _REPO / mod.NOTICES
FIXTURE = "test_PDFs/crazyones-pdfa.pdf"
FAST_CHECKS = _REPO / ".github" / "workflows" / "fast-checks.yml"


def _live_text():
    return NOTICES.read_text(encoding="utf-8")


# ------------------------------------------------------------------ live state

def test_the_live_tree_passes():
    problems = mod.check([])
    assert problems == [], "live tree fails:\n  " + "\n  ".join(problems)


def test_the_notices_file_is_tracked():
    out = subprocess.run(["git", "ls-files", "--error-unmatch", mod.NOTICES],
                         cwd=str(_REPO), capture_output=True, text=True)
    assert out.returncode == 0, "an untracked NOTICE is not an attribution"


def test_the_attributed_fixture_is_tracked():
    """The whole point: a `.gitignore` re-include that silently reverts would
    leave the attribution describing a file no clean checkout has."""
    out = subprocess.run(["git", "ls-files", "--error-unmatch", FIXTURE],
                         cwd=str(_REPO), capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr


def test_the_recorded_hash_is_the_real_hash():
    """Derived from the bytes, not copied from the brief."""
    raw = (_REPO / FIXTURE).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == mod.ENTRIES[FIXTURE]["sha256"]
    assert len(raw) == mod.ENTRIES[FIXTURE]["size"]


def test_no_other_notice_file_has_appeared():
    """If someone adds a NOTICE/CREDITS file, attribution splits across two
    places and this checker guards only one of them."""
    out = subprocess.run(["git", "ls-files"], cwd=str(_REPO),
                         capture_output=True, text=True, encoding="utf-8")
    others = [ln for ln in out.stdout.splitlines()
              if ln != mod.NOTICES
              and Path(ln).name.upper().startswith(
                  ("NOTICE", "THIRD", "CREDITS", "AUTHORS"))]
    assert others == [], f"attribution split across extra files: {others}"


# -------------------------------------------------------------------- mutations

def test_dropping_the_entry_is_caught():
    text = _live_text().replace(FIXTURE, "some/other/file.pdf")
    problems = mod.check([], notices_text=text)
    assert any("does not mention" in p for p in problems), problems


def test_a_stale_hash_in_the_checker_is_caught():
    """Simulates the fixture being replaced: the recorded hash no longer matches
    the committed bytes."""
    entries = {FIXTURE: dict(mod.ENTRIES[FIXTURE], sha256="0" * 64)}
    problems = mod.check([], notices_text=_live_text(), entries=entries)
    assert any("SHA-256 is" in p and "did not" in p for p in problems), problems


def test_a_hash_missing_from_the_notice_is_caught():
    """The checker and the file agree, but the NOTICE dropped the hash -- the
    binding between attribution and artifact is gone."""
    text = _live_text().replace(mod.ENTRIES[FIXTURE]["sha256"], "(redacted)")
    problems = mod.check([], notices_text=text)
    assert any("does not record the SHA-256" in p for p in problems), problems


def test_a_wrong_size_is_caught():
    entries = {FIXTURE: dict(mod.ENTRIES[FIXTURE], size=1234)}
    problems = mod.check([], notices_text=_live_text(), entries=entries)
    assert any("bytes; the recorded size is" in p for p in problems), problems


def test_an_entry_for_an_absent_file_is_caught():
    entries = {"test_PDFs/definitely-not-here.pdf": dict(
        mod.ENTRIES[FIXTURE], must_mention=[])}
    text = _live_text() + "\n\ntest_PDFs/definitely-not-here.pdf\n"
    problems = mod.check([], notices_text=text, entries=entries)
    assert any("is not in the tree" in p for p in problems), problems


@pytest.mark.parametrize("needle", sorted(mod.ENTRIES[FIXTURE]["must_mention"]))
def test_dropping_any_required_attribution_field_is_caught(needle):
    """Upstream project, upstream URL and licence id are the attribution. Losing
    any ONE of them is losing the attribution, so each is checked individually."""
    text = _live_text().replace(needle, "REMOVED")
    problems = mod.check([], notices_text=text)
    assert any(repr(needle) in p for p in problems), (needle, problems)


@pytest.mark.parametrize("phrase", [
    "This material is cleared for use.",
    "There are no licensing concerns.",
    "Our review confirms we are permitted to redistribute it.",
])
def test_turning_the_record_into_a_clearance_is_caught(phrase):
    """The record states facts. Editing it into a legal opinion is the specific
    overreach this phase was told not to commit."""
    problems = mod.check([], notices_text=_live_text() + "\n" + phrase + "\n")
    assert any("not a licence clearance" in p for p in problems), problems


# ------------------------------------------------ the notice says what it must

def test_the_notice_records_the_unresolved_rights_questions():
    """Both are facts karen established and neither is resolved here; a notice
    that quietly drops them reads as a clearance by omission."""
    text = _live_text()
    assert "Copyright 2000 Adobe Systems Incorporated" in text
    assert "Think Different" in text
    assert "not a legal clearance" in text or "not a licence clearance" in text


def test_the_notice_states_the_cpack_consequence():
    """It is deliberately not installed; if that ever changes the reason must be
    re-read, so the reason has to be IN the file."""
    assert "CPack" in _live_text()


def test_notices_file_is_not_installed_by_cpack():
    """Pins the decision recorded above against silent drift in either direction."""
    cml = (_REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    assert mod.NOTICES not in cml, (
        "THIRD-PARTY-NOTICES.md is now installed; it must then also enumerate the "
        "shipped libraries, and this test's rationale needs updating deliberately."
    )


# ---------------------------------------------------- pytest.ini testpaths trap

def test_selftest_is_named_in_fast_checks():
    text = FAST_CHECKS.read_text(encoding="utf-8")
    assert "tools/gate/test_check_third_party_notices.py" in text, (
        "pytest.ini sets `testpaths = tests`, so this file runs NOWHERE unless the "
        '"Gate self-tests" step names it.'
    )


def test_checker_itself_is_a_fast_checks_step():
    text = FAST_CHECKS.read_text(encoding="utf-8")
    assert "python tools/gate/check_third_party_notices.py" in text
