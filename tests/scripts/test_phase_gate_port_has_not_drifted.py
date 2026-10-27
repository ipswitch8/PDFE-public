# -*- coding: utf-8 -*-
"""The `phase-gate.sh` port in the #300 tests must not silently drift.

WHY THIS FILE EXISTS. `tests/scripts/test_run_gtest_teardown_evidence.py`
contains `_gate_would_accept`, a port of the `validate_junit_xml` predicate
embedded in `~/.claude/hooks/phase-gate.sh`. The #300 acceptance criterion is
that a real evidence GATE can tell a crashed run from a clean one, so the port
being a faithful copy is load-bearing: if it diverges, that whole test file
goes on passing while asserting a decision the gate no longer makes.

The phase-2 claim-check gate raised exactly this (finding F1): a docstring
saying "keep it a port" is an instruction, not a control. Nothing failed if the
hook changed. This file is the control.

HOW, AND WHY NOT A HASH. Hashing the hook would redden on every cosmetic edit —
a renamed local, a reflowed comment — and a guard that cries wolf gets switched
off (`noisy_guard_masks_everything` is a standing lesson in this project). So
this tests **behavioural equivalence** instead: it extracts the hook's own
Python, executes it as the hook does, and requires it to agree with the port on
a battery of artifacts chosen to straddle every branch. Cosmetic edits pass.
A changed decision fails.

The hook lives OUTSIDE this repo, in `~/.claude/hooks`, and a CI runner has no
pipeline hooks. That is a genuine environment condition, expressed as a
filesystem-existence probe so `tools/gate/check_reachability_skip_shape.py` can
see an environment gate rather than a WIN-45 reachability skip.

But "skipped in CI" would make this a drift control for one workstation, so the
same battery runs everywhere against a VENDORED copy of the hook's predicate
(`fixtures/phase_gate_validate_junit_reference.py`), and a third test compares
that copy against the live hook wherever the hook exists. The chain has no
unchecked link:

    port == vendored        checked EVERYWHERE, CI included
    vendored == live hook   checked wherever the hook is installed
    => port == live hook

The third link matters more than it looks: without it the vendored copy could
sit frozen at an old hook while the CI battery cheerfully confirmed the port
matches it — two things agreeing with each other about a third that has moved
on, which is the shape of every two-copy divergence in this project. Re-vendor
with `python tests/scripts/revendor_phase_gate_predicate.py`.

An earlier version of this docstring claimed the vendored reference existed
when it did not; the claim was written before the mechanism. It was caught by
reading my own file rather than by a gate, which is luck, not process.
"""
from __future__ import annotations

import importlib.util
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TEV = ROOT / "tests" / "scripts" / "test_run_gtest_teardown_evidence.py"
HOOK = Path(os.path.expanduser("~/.claude/hooks/phase-gate.sh"))
VENDORED = (ROOT / "tests" / "scripts" / "fixtures"
            / "phase_gate_validate_junit_reference.py")


def _load_port():
    """Import `_gate_would_accept` from the #300 test module.

    Imported rather than copied, on purpose: a copy here would be a second
    thing to keep in sync, which is the defect this file exists to prevent.
    """
    spec = importlib.util.spec_from_file_location("tev_for_drift", TEV)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["tev_for_drift"] = mod
    spec.loader.exec_module(mod)
    return mod._gate_would_accept


def _extract_hook_python() -> str:
    """Pull the `validate_junit_xml` heredoc body out of the hook.

    Anchored on the function name and the quoted-heredoc delimiter the hook
    actually uses (`<<'PY'` ... `PY`). If the anchors stop matching, this
    RAISES rather than returning a guess -- a drift detector that silently
    verifies nothing is worse than no drift detector, because it reads as
    coverage.
    """
    text = HOOK.read_text(encoding="utf-8", errors="replace")
    start = text.find("validate_junit_xml() {")
    if start < 0:
        raise AssertionError(
            "validate_junit_xml() not found in ~/.claude/hooks/"
            "phase-gate.sh -- the hook has been restructured and this "
            "detector must be updated, not ignored")
    # The heredoc line carries trailing redirection -- the hook's is
    # `"$_py" - "$xml" "$target" <<'PY' 2>/dev/null` -- so anything after the
    # delimiter up to the newline must be tolerated. Matching `<<'PY'\n`
    # exactly found nothing, and the detector said so instead of guessing.
    m = re.search(r"<<'PY'[^\n]*\r?\n(.*?)\r?\nPY\r?\n", text[start:],
                  re.DOTALL)
    if not m:
        raise AssertionError(
            "the <<'PY' heredoc could not be located inside "
            "validate_junit_xml() in ~/.claude/hooks/phase-gate.sh; "
            "anchors have changed")
    return m.group(1)


def _decides(code: str, xml_path: Path, target: str, who: str) -> bool:
    """Run a predicate's source as a subprocess, the way the hook runs it.

    exit 0 -> accept, 1 -> reject, 2 -> unparseable. Anything else is not a
    decision and must not be silently coerced into one -- a crash coerced to
    "reject" would make this detector agree with anything that also crashes.
    """
    proc = subprocess.run([sys.executable, "-", str(xml_path), target],
                          input=code, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode not in (0, 1, 2):
        raise AssertionError(
            "%s exited %d, which is not one of its three documented "
            "outcomes; stderr=%r"
            % (who, proc.returncode, proc.stderr[-400:]))
    return proc.returncode == 0


def _hook_decides(xml_path: Path, target: str = "") -> bool:
    """The LIVE hook's decision. Needs the hook to be installed."""
    return _decides(_extract_hook_python(), xml_path, target,
                    "the live hook's predicate")


def _vendored_decides(xml_path: Path, target: str = "") -> bool:
    """The VENDORED copy's decision. Available everywhere, CI included."""
    return _decides(VENDORED.read_text(encoding="utf-8"), xml_path, target,
                    "the vendored reference predicate")


# ------------------------------------------------------------------ battery
#
# Each case names the branch it is there to exercise. A battery that only
# contains the happy path proves the two agree about nothing interesting.

def _xml(root: ET.Element, tmp: Path, name: str) -> Path:
    p = tmp / name
    ET.ElementTree(root).write(str(p), encoding="utf-8", xml_declaration=True)
    return p


def _suite(tests="2", failures="0", errors="0", n_cases=2, status="run",
           child=None, root_attrs=None, suite_attrs=True):
    attrs = {"tests": tests, "failures": failures, "errors": errors}
    root = ET.Element("testsuites", dict(root_attrs or attrs))
    s = ET.SubElement(root, "testsuite",
                      dict(attrs, name="S") if suite_attrs else {"name": "S"})
    for i in range(n_cases):
        tc = ET.SubElement(s, "testcase", {"name": "t%d" % i,
                                           "classname": "S",
                                           "status": status})
        if child:
            ET.SubElement(tc, child, {"message": "m", "type": "t"})
    return root


def _battery():
    """(label, root, target) tuples straddling every branch of the predicate."""
    return [
        ("clean-2-tests", _suite(), ""),
        ("suite-failures-1", _suite(failures="1"), ""),
        ("suite-errors-1", _suite(errors="1"), ""),
        # attributes clean, ELEMENTS dirty -- the exact pair that made the
        # original port wrong. The two implementations must AGREE here,
        # whatever they agree on.
        ("elements-dirty-attrs-clean", _suite(child="error"), ""),
        ("elements-failure-attrs-clean", _suite(child="failure"), ""),
        # tests=0 at suite level -> the root-attribute fallback
        ("suite-without-tests-attr", _suite(suite_attrs=False), ""),
        ("fallback-root-errors",
         _suite(suite_attrs=False, root_attrs={"tests": "2", "failures": "0",
                                               "errors": "1"}), ""),
        ("zero-tests-everywhere", _suite(tests="0", n_cases=0), ""),
        # no case with status=run
        ("all-notrun", _suite(status="notrun"), ""),
        # target matching, both spellings and a miss
        ("target-by-name", _suite(), "t0"),
        ("target-by-classname-dot-name", _suite(), "S.t0"),
        ("target-absent", _suite(), "nope"),
        ("target-on-notrun", _suite(status="notrun"), "t0"),
    ]


# The reason string must NOT interpolate HOOK: it resolves to a path under the
# user profile, and a skip reason travels into the JUnit XML, which is exactly
# how the account name has leaked into artifacts before. Describe the condition
# instead of printing the path.
@pytest.mark.skipif(not HOOK.exists(),
                    reason="environment probe: ~/.claude/hooks/phase-gate.sh "
                           "does not exist on this machine (a CI runner has "
                           "no pipeline hooks). The equivalence battery still "
                           "runs; only the against-the-live-hook comparison "
                           "needs it.")
@pytest.mark.parametrize("label", [c[0] for c in _battery()])
def test_the_port_agrees_with_the_LIVE_hook(label, tmp_path):
    """THE CONTROL. Behavioural equivalence, case by case.

    A disagreement means the hook changed and the #300 test file is now
    asserting a decision the real evidence gate does not make. Re-port
    `_gate_would_accept` from the hook; do not adjust this test to match.
    """
    port = _load_port()
    root, target = next((r, t) for (l, r, t) in _battery() if l == label)
    path = _xml(root, tmp_path, "a.xml")
    mine = port(ET.parse(str(path)).getroot(), target)
    theirs = _hook_decides(path, target)
    assert mine == theirs, (
        "PORT DRIFT on %r: the port says %s, the live hook says %s. "
        "`_gate_would_accept` in %s is no longer the decision "
        "`phase-gate.sh::validate_junit_xml` makes, so the #300 evidence "
        "claim no longer holds. Re-port it from the hook."
        % (label, mine, theirs, TEV.name))


@pytest.mark.skipif(not HOOK.exists(),
                    reason="environment probe: ~/.claude/hooks/phase-gate.sh "
                           "does not exist here")
def test_the_extractor_still_finds_the_hooks_predicate():
    """If the anchors rot, every case above would vacuously pass.

    `_extract_hook_python` raises rather than returning a guess, but nothing
    would notice the raise if it only ever ran inside a skipped test. This
    asserts the extraction works AND that what came out is the right code.
    """
    code = _extract_hook_python()
    for needle in ("def ai(", "iter(\"testsuite\")", "if tt == 0:",
                   "run_cases", "sys.exit(0 if ok else 1)"):
        assert needle in code, (
            "extracted hook predicate is missing %r -- the extraction anchors "
            "matched the wrong region, so the drift comparison would be "
            "meaningless: %r" % (needle, code[:300]))


def test_the_battery_actually_discriminates():
    """Not every case may give the same answer.

    A battery whose every case accepts (or every case rejects) would agree with
    ANY implementation, including a `return True`. This pins that the fixtures
    straddle the decision -- the property that makes the agreement test above
    worth running at all.
    """
    port = _load_port()
    answers = {}
    for label, root, target in _battery():
        answers[label] = port(root, target)
    assert any(answers.values()), answers
    assert not all(answers.values()), answers
    # And specifically: the attribute path and the element path must be
    # distinguishable, or the original defect could come back unnoticed.
    assert answers["suite-errors-1"] is False, answers
    assert answers["elements-dirty-attrs-clean"] is True, answers


# ------------------------------------------------- the CI-side drift control
#
# The equivalence battery above needs the live hook, which exists on a
# workstation and not on a GitHub-hosted runner. A drift control that only runs
# on one machine is a drift control for one machine, so the same battery runs
# here against the VENDORED copy of the hook's predicate, everywhere.
#
# The vendored copy is only worth anything if it is CURRENT, so a third test
# compares it against the live hook wherever the hook exists. That gives a
# chain with no unchecked link: port == vendored (checked everywhere),
# vendored == live hook (checked wherever the hook is), therefore
# port == live hook.

@pytest.mark.parametrize("label", [c[0] for c in _battery()])
def test_the_port_agrees_with_the_VENDORED_reference(label, tmp_path):
    """Runs in CI. Same battery, second witness.

    If this fails, `_gate_would_accept` has diverged from the predicate as
    vendored. Re-port it from the hook; do not edit this test or the fixture to
    agree.
    """
    port = _load_port()
    root, target = next((r, t) for (l, r, t) in _battery() if l == label)
    path = _xml(root, tmp_path, "a.xml")
    mine = port(ET.parse(str(path)).getroot(), target)
    theirs = _vendored_decides(path, target)
    assert mine == theirs, (
        "PORT DRIFT on %r: the port says %s, the vendored reference says %s. "
        "`_gate_would_accept` is no longer the decision "
        "`phase-gate.sh::validate_junit_xml` makes." % (label, mine, theirs))


@pytest.mark.skipif(not HOOK.exists(),
                    reason="environment probe: ~/.claude/hooks/phase-gate.sh "
                           "does not exist here, so there is nothing to "
                           "compare the vendored copy AGAINST")
def test_the_vendored_reference_matches_the_live_hook():
    """The link that keeps the vendored copy honest.

    Without this, the vendored reference could sit frozen at an old version of
    the hook while the CI battery cheerfully confirmed the port matches it --
    two things agreeing with each other about a third that has moved on. That
    is the shape of every two-copy divergence in this project.

    Compared as NORMALISED TEXT, not bytes: `core.autocrlf=true` here, so the
    vendored file is CRLF in the working tree while the hook is read as-is, and
    a byte comparison would fail for a reason that has nothing to do with
    drift.
    """
    live = _extract_hook_python()
    vendored_full = VENDORED.read_text(encoding="utf-8")
    # Strip the vendored header, which is ours and not the hook's.
    marker = "# is the point: the staleness is reported, not absorbed.\n"
    assert marker in vendored_full, (
        "the vendored file's header marker is missing, so the body cannot be "
        "isolated; re-run tests/scripts/revendor_phase_gate_predicate.py")
    vendored_body = vendored_full.split(marker, 1)[1]

    def norm(t):
        return "\n".join(line.rstrip() for line in t.replace("\r\n", "\n")
                         .strip().splitlines())

    assert norm(vendored_body) == norm(live), (
        "the VENDORED reference is STALE: "
        "tests/scripts/fixtures/phase_gate_validate_junit_reference.py no "
        "longer matches the predicate in ~/.claude/hooks/phase-gate.sh. The "
        "CI battery is therefore comparing the port against an old decision. "
        "Run `python tests/scripts/revendor_phase_gate_predicate.py`, re-port "
        "`_gate_would_accept`, and commit both together.")
