# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_fast_checks_citations.py (F43).

`pytest.ini` sets `testpaths = tests`, so a self-test under `tools/gate/` runs
NOWHERE unless it is named explicitly in the *"Gate self-tests + matrix
pass-semantics"* step of `.github/workflows/fast-checks.yml`.  It is, and
`test_selftest_is_named_in_fast_checks` below fails if that line is ever
removed -- the same trap F35 and F37 had to close.

The discriminating controls are the COMMITTED pre-F43 snapshots
`tools/gate/fixtures/fast-checks-unguarded-pre-f43.yml` and
`governance-unguarded-pre-f43.yml`: the real workflows as they stood at
`ed69ba6`, when run 31242065496 let one red step skip eight standing controls.
A checker that passed those would be decoration.
"""

from __future__ import annotations

import importlib.util
import re
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]

_spec = importlib.util.spec_from_file_location(
    "check_fast_checks_citations", _HERE / "check_fast_checks_citations.py"
)
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

FAST_CHECKS = _REPO / ".github" / "workflows" / "fast-checks.yml"
GOVERNANCE = _REPO / ".github" / "workflows" / "governance.yml"
FIX_FAST = _HERE / "fixtures" / "fast-checks-unguarded-pre-f43.yml"
FIX_GOV = _HERE / "fixtures" / "governance-unguarded-pre-f43.yml"


# ---------------------------------------------------------------- end to end

def test_live_tree_passes_every_gate():
    problems = mod.run_all()
    assert problems == [], "live tree fails its own gate:\n  " + "\n  ".join(problems)


def test_cli_exit_zero_on_live_tree():
    rc = subprocess.run(
        [sys.executable, str(_HERE / "check_fast_checks_citations.py")],
        cwd=str(_REPO), capture_output=True, text=True,
    )
    assert rc.returncode == 0, rc.stdout + rc.stderr


# ------------------------------------------------- G1: the masking regression

def _guard_problems(monkeypatch, targets):
    monkeypatch.setattr(mod, "TARGETS", targets)
    problems: list[str] = []
    mod.gate_guards_and_swallow(problems)
    return problems


def test_committed_pre_f43_fast_checks_fixture_FAILS_g1(monkeypatch, tmp_path):
    """The real file at ed69ba6 must be rejected, or the gate proves nothing."""
    rel = FIX_FAST.relative_to(_REPO).as_posix()
    problems = _guard_problems(
        monkeypatch, {rel: {"Checkout", "Set up Python", "Install check deps"}}
    )
    g1 = [p for p in problems if p.startswith("G1")]
    assert len(g1) == 12, f"expected all 12 check steps flagged, got {len(g1)}:\n" + "\n".join(g1)
    assert all("has NO `if:`" in p for p in g1)


def test_committed_pre_f43_governance_fixture_FAILS_g1(monkeypatch):
    rel = FIX_GOV.relative_to(_REPO).as_posix()
    problems = _guard_problems(monkeypatch, {rel: {"Checkout"}})
    g1 = [p for p in problems if p.startswith("G1")]
    # 3 unconditional steps with no `if:` at all, plus 2 that had an `if:`
    # WITHOUT a status function -- which GitHub still ANDs with success(), so
    # they were masked too. Missing that second class is the easy mistake.
    assert len(g1) == 5, "\n".join(g1)
    assert sum("has NO `if:`" in p for p in g1) == 3
    assert sum("no status function" in p for p in g1) == 2


def test_live_workflows_pass_g1(monkeypatch):
    problems = _guard_problems(monkeypatch, dict(mod.TARGETS))
    assert [p for p in problems if p.startswith("G1")] == []


def test_condition_without_status_function_is_rejected(monkeypatch, tmp_path):
    wf = tmp_path / "w.yml"
    wf.write_text(
        "jobs:\n  j:\n    steps:\n"
        "      - name: Checkout\n        run: true\n"
        "      - name: Check\n        if: ${{ inputs.x != '' }}\n        run: true\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    problems = _guard_problems(monkeypatch, {"w.yml": {"Checkout"}})
    assert any("no status function" in p for p in problems)


def test_merged_guard_is_accepted(monkeypatch, tmp_path):
    wf = tmp_path / "w.yml"
    wf.write_text(
        "jobs:\n  j:\n    steps:\n"
        "      - name: Checkout\n        run: true\n"
        "      - name: Check\n        if: ${{ !cancelled() && (inputs.x != '') }}\n"
        "        run: true\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    assert _guard_problems(monkeypatch, {"w.yml": {"Checkout"}}) == []


def test_setup_step_must_stay_fail_fast(monkeypatch, tmp_path):
    """Guarding Checkout is a FAILURE: if the env is absent the checks are meaningless."""
    wf = tmp_path / "w.yml"
    wf.write_text(
        "jobs:\n  j:\n    steps:\n"
        "      - name: Checkout\n        if: ${{ !cancelled() }}\n        run: true\n"
        "      - name: Check\n        if: ${{ !cancelled() }}\n        run: true\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    problems = _guard_problems(monkeypatch, {"w.yml": {"Checkout"}})
    assert any("MUST stay fail-fast" in p for p in problems)


def test_live_setup_steps_are_unguarded():
    for path, setup in (
        (FAST_CHECKS, {"Checkout", "Set up Python", "Install check deps"}),
        (GOVERNANCE, {"Checkout"}),
    ):
        found = {n for _, n, cond in mod.parse_steps(path) if cond is None}
        assert found == setup, f"{path.name}: unguarded steps {found} != setup {setup}"


# ------------------------------------------- G2: the guard must not swallow

def test_continue_on_error_is_rejected(monkeypatch, tmp_path):
    wf = tmp_path / "w.yml"
    wf.write_text(
        "jobs:\n  j:\n    steps:\n"
        "      - name: Checkout\n        run: true\n"
        "      - name: Check\n        if: ${{ !cancelled() }}\n"
        "        continue-on-error: true\n        run: true\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    problems = _guard_problems(monkeypatch, {"w.yml": {"Checkout"}})
    assert any(p.startswith("G2") for p in problems)


def test_live_workflows_have_no_continue_on_error():
    """This is what keeps `!cancelled()` a RUN-decision and not a swallow: a
    guarded step that fails must still fail the job."""
    for path in (FAST_CHECKS, GOVERNANCE):
        text = path.read_text(encoding="utf-8")
        assert not mod._CONTINUE_ON_ERROR_RE.search(text), path.name


# ---------------------------------------------------- G6: the incident, pinned

def test_all_eight_masked_steps_still_exist_and_are_guarded():
    steps = {n: c for _, n, c in mod.parse_steps(FAST_CHECKS)}
    assert len(mod.REGRESSION_STEPS) == 8
    for name in mod.REGRESSION_STEPS:
        assert name in steps, f"step 9-16 of run 31242065496 missing: {name!r}"
        assert steps[name] and mod._NONCANCELLED_RE.search(steps[name]), name


def test_renaming_a_masked_step_is_caught(monkeypatch, tmp_path):
    monkeypatch.setattr(mod, "REGRESSION_STEPS", ["A step that does not exist"])
    problems = _guard_problems(monkeypatch, dict(mod.TARGETS))
    assert any(p.startswith("G6") for p in problems)


# ------------------------------------------------------------ citation regex

@pytest.mark.parametrize("text,expected", [
    ("`.github/workflows/fast-checks.yml:92`", [("fast-checks.yml", "92")]),
    ("fast-checks.yml:77-89", [("fast-checks.yml", "77-89")]),
    ("see governance.yml:125 and fast-checks.yml:94",
     [("governance.yml", "125"), ("fast-checks.yml", "94")]),
])
def test_citation_regex_matches_real_forms(text, expected):
    assert [(m.group(1), m.group(2)) for m in mod.CITATION_RE.finditer(text)] == expected


def test_citation_regex_matches_the_path_prefixed_form():
    """Regression: an earlier revision excluded `/` in the lookbehind and so
    matched almost nothing, because nearly every real citation is written
    `.github/workflows/fast-checks.yml:NNN`."""
    assert mod.CITATION_RE.search("`.github/workflows/fast-checks.yml:143`")


def test_citation_regex_does_not_match_a_different_workflow():
    assert not mod.CITATION_RE.search("windows-governance.yml:51")
    assert not mod.CITATION_RE.search("my-fast-checks.yml:12")


def test_citation_regex_ignores_a_name_anchor():
    assert not mod.CITATION_RE.search('`fast-checks.yml` step *"DCA loop"*')


# ------------------------------------------------------------- anchor regex

def test_anchor_matches_across_a_line_break():
    text = ('wired into `.github/workflows/fast-checks.yml` step *"No unreviewed\n'
            'magnitude guard in tests (PERF-02 ratchet, F37)"*, with self-tests')
    m = mod.ANCHOR_RE.search(text)
    assert m and mod._norm(m.group(2)) == \
        "No unreviewed magnitude guard in tests (PERF-02 ratchet, F37)"


def test_every_anchor_in_the_live_rtm_resolves():
    text = (_REPO / "docs" / "audit" / "rtm-coverage.md").read_text(encoding="utf-8")
    names = {
        p.name: {n for _, n, _ in mod.parse_steps(_REPO / rel)}
        for rel in mod.TARGETS for p in [Path(rel)]
    }
    anchors = [(m.group(1), mod._norm(m.group(2))) for m in mod.ANCHOR_RE.finditer(text)]
    assert len(anchors) >= 6, f"expected the six re-anchored citations, got {anchors}"
    for wf, name in anchors:
        assert name in names[wf], f"{wf} has no step {name!r}"


# ------------------------------------------------- G3/G4/G5: classification

def test_classification_lists_are_disjoint():
    assert not (set(mod.LIVE_DOCS) & set(mod.HISTORICAL_DOCS))


def test_every_classified_document_exists():
    missing = [r for r in set(mod.LIVE_DOCS) | set(mod.HISTORICAL_DOCS)
               if not (_REPO / r).exists()]
    assert missing == []


def test_every_classification_carries_a_reason():
    for rel, why in {**mod.LIVE_DOCS, **mod.HISTORICAL_DOCS}.items():
        assert why and len(why) > 20, rel


def test_an_unclassified_citing_file_fails(monkeypatch, tmp_path):
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(mod, "LIVE_DOCS", {})
    monkeypatch.setattr(mod, "HISTORICAL_DOCS", {})
    monkeypatch.setattr(mod, "TARGETS", {})
    monkeypatch.setattr(mod, "_tracked_files", lambda: ["notes.md"])
    (tmp_path / "notes.md").write_text("see fast-checks.yml:92\n", encoding="utf-8")
    problems: list[str] = []
    mod.gate_citations(problems)
    assert any(p.startswith("G3") for p in problems)


def test_a_live_doc_with_a_line_number_fails(monkeypatch, tmp_path):
    _stub_targets(monkeypatch, tmp_path)
    monkeypatch.setattr(mod, "LIVE_DOCS", {"live.md": "x" * 30})
    monkeypatch.setattr(mod, "HISTORICAL_DOCS", {})
    monkeypatch.setattr(mod, "_tracked_files", lambda: ["live.md"])
    (tmp_path / "live.md").write_text(
        'fast-checks.yml:92 and `fast-checks.yml` step *"Check"*\n', encoding="utf-8")
    problems: list[str] = []
    mod.gate_citations(problems)
    assert any(p.startswith("G4") for p in problems)


def test_a_historical_doc_with_a_line_number_passes(monkeypatch, tmp_path):
    """The whole point of the policy: dated records keep their line numbers."""
    _stub_targets(monkeypatch, tmp_path)
    monkeypatch.setattr(mod, "LIVE_DOCS", {})
    monkeypatch.setattr(mod, "HISTORICAL_DOCS", {"hist.md": "x" * 30})
    monkeypatch.setattr(mod, "_tracked_files", lambda: ["hist.md"])
    (tmp_path / "hist.md").write_text("as of phase-30, fast-checks.yml:92\n", encoding="utf-8")
    problems: list[str] = []
    mod.gate_citations(problems)
    assert problems == []


def test_a_live_doc_anchoring_a_nonexistent_step_fails(monkeypatch, tmp_path):
    _stub_targets(monkeypatch, tmp_path)
    monkeypatch.setattr(mod, "LIVE_DOCS", {"live.md": "x" * 30})
    monkeypatch.setattr(mod, "HISTORICAL_DOCS", {})
    monkeypatch.setattr(mod, "_tracked_files", lambda: ["live.md"])
    (tmp_path / "live.md").write_text(
        '`fast-checks.yml` step *"Renamed away"*\n', encoding="utf-8")
    problems: list[str] = []
    mod.gate_citations(problems)
    assert any(p.startswith("G5") for p in problems)


def test_a_live_doc_with_no_anchor_at_all_fails(monkeypatch, tmp_path):
    """Guards against 'fixing' G4 by deleting the citation instead of anchoring
    it, which would leave G4/G5 watching nothing."""
    _stub_targets(monkeypatch, tmp_path)
    monkeypatch.setattr(mod, "LIVE_DOCS", {"live.md": "x" * 30})
    monkeypatch.setattr(mod, "HISTORICAL_DOCS", {})
    monkeypatch.setattr(mod, "_tracked_files", lambda: ["live.md"])
    (tmp_path / "live.md").write_text("nothing to see here\n", encoding="utf-8")
    problems: list[str] = []
    mod.gate_citations(problems)
    assert any(p.startswith("G5") for p in problems)


def _stub_targets(monkeypatch, tmp_path):
    wf = tmp_path / "fast-checks.yml"
    wf.write_text(
        "jobs:\n  j:\n    steps:\n"
        "      - name: Checkout\n        run: true\n"
        "      - name: Check\n        if: ${{ !cancelled() }}\n        run: true\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(mod, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(mod, "TARGETS", {"fast-checks.yml": {"Checkout"}})


# ------------------------------------------------------------- the resolver

def test_resolver_lines_actually_contain_the_step_name():
    for path in (FAST_CHECKS, GOVERNANCE):
        lines = path.read_text(encoding="utf-8").split("\n")
        for lineno, name, _ in mod.parse_steps(path):
            assert lines[lineno - 1].strip() == f"- name: {name}", (path.name, lineno)


def test_resolve_cli_returns_the_current_line():
    out = subprocess.run(
        [sys.executable, str(_HERE / "check_fast_checks_citations.py"),
         "--resolve", "Gate self-tests + matrix pass-semantics (R-24/R-26/R-28/R-32/R-37)"],
        cwd=str(_REPO), capture_output=True, text=True, encoding="utf-8",
    )
    assert out.returncode == 0, out.stderr
    ref = out.stdout.strip().split("\t")[0]                     # fast-checks.yml:NNN
    rel, lineno = ref.rsplit(":", 1)
    line = (_REPO / rel).read_text(encoding="utf-8").split("\n")[int(lineno) - 1]
    assert "Gate self-tests + matrix pass-semantics" in line


def test_resolve_cli_fails_on_an_unknown_step():
    out = subprocess.run(
        [sys.executable, str(_HERE / "check_fast_checks_citations.py"),
         "--resolve", "no such step"],
        cwd=str(_REPO), capture_output=True, text=True, encoding="utf-8",
    )
    assert out.returncode == 1


def test_list_steps_covers_both_workflows():
    out = subprocess.run(
        [sys.executable, str(_HERE / "check_fast_checks_citations.py"), "--list-steps"],
        cwd=str(_REPO), capture_output=True, text=True, encoding="utf-8",
    )
    assert out.returncode == 0
    assert "fast-checks.yml" in out.stdout and "governance.yml" in out.stdout


# ------------------------------------------- G7: the F44 windows-validation half
#
# Discriminating control: the COMMITTED pre-F44 snapshot
# `fixtures/windows-validation-unguarded-pre-f44.yml` -- the real workflow at
# `937b7c6`, with all nine `if: ${{ !inputs.ui_only }}` sites intact.  Every one
# of the nine must be individually named in the failure output, not merely "the
# file fails": a gate that reddened on the fixture for one reason would look
# identical to one that caught all nine.

WINVAL = _REPO / ".github" / "workflows" / "windows-validation.yml"
FIX_WV = _HERE / "fixtures" / "windows-validation-unguarded-pre-f44.yml"


def _g7(path):
    problems: list[str] = []
    mod.gate_windows_validation(problems, path)
    return problems


def test_the_nine_is_actually_nine():
    """Counted mechanically from the pre-fix snapshot, not from the brief."""
    text = FIX_WV.read_text(encoding="utf-8")
    assert text.count("if: ${{ !inputs.ui_only }}") == 9
    # EIGHT, not nine, since the UI leg became a 3-shard matrix. The
    # matrix-health gate moved out of the ui-e2e job into its own, where it has
    # no in-job producer to name -- its dependency is `needs: [ui-e2e]` and it
    # scores the MERGED per-shard aggregate. The fixture still carries all nine
    # original sites (asserted above) because it is a dated snapshot of the
    # pre-F44 file; only the LIVE table shrank, and deliberately.
    #
    # This is the one number in this file that may legitimately move, so it says
    # why rather than just tracking the code.
    assert len(mod.WV_DECISIONS) == 8


def test_live_windows_validation_passes_g7():
    problems = _g7(WINVAL)
    assert problems == [], "live workflow fails G7:\n  " + "\n  ".join(problems)


def test_no_bare_ui_only_condition_survives_in_the_live_workflow():
    for _ln, _name, cond, _sid, _cm in mod.parse_steps_rich(WINVAL):
        if cond and "inputs.ui_only" in cond:
            assert mod._NONCANCELLED_RE.search(cond), cond


def test_pre_f44_fixture_FAILS_g7_at_every_one_of_the_nine():
    problems = _g7(FIX_WV)
    assert problems, "the pre-fix snapshot passes G7 -- the gate is decoration"
    for name in mod.WV_DECISIONS:
        assert any(repr(name) in p and "NO status-check function" in p
                   for p in problems), (
            f"G7 did not flag the masking on {name!r}; a per-site control that "
            "misses a site is not per-site."
        )


def test_pre_f44_fixture_also_flags_the_missing_inline_comment():
    problems = _g7(FIX_WV)
    for name in mod.WV_DECISIONS:
        assert any(repr(name) in p and mod.WV_MARKER in p for p in problems)


@pytest.mark.parametrize("name", sorted(mod.WV_DECISIONS))
def test_reverting_one_single_site_reddens_g7(tmp_path, name):
    """Mutation: restore the ORIGINAL bare condition on exactly one step.

    Eight sites stay fixed.  A gate that only checks the file as a whole, or
    that stops at the first problem, would stay green here.
    """
    lines = WINVAL.read_text(encoding="utf-8").split("\n")
    target = None
    for ln, nm, _c, _sid, _cm in mod.parse_steps_rich(WINVAL):
        if nm == name:
            target = ln
            break
    assert target is not None
    # Replace this step's `if:` block with the pre-fix bare condition.
    out, i, done = [], 0, False
    while i < len(lines):
        line = lines[i]
        if i + 1 == target:
            out.append(line)
            i += 1
            while i < len(lines) and not mod._STEP_NAME_RE.match(lines[i]):
                stripped = lines[i].strip()
                if stripped.startswith("if:"):
                    out.append("        if: ${{ !inputs.ui_only }}")
                    done = True
                    # skip folded continuation lines
                    i += 1
                    while (i < len(lines) and lines[i].strip()
                           and lines[i].startswith("          ")
                           and not lines[i].strip().startswith("#")
                           and ":" not in lines[i].split("#")[0]):
                        i += 1
                    continue
                out.append(lines[i])
                i += 1
            continue
        out.append(line)
        i += 1
    assert done, f"could not find the `if:` of {name!r} to mutate"

    mutant = tmp_path / "windows-validation.yml"
    mutant.write_text("\n".join(out), encoding="utf-8")
    problems = _g7(mutant)
    assert any(repr(name) in p and "NO status-check function" in p
               for p in problems), (
        f"reverting ONLY {name!r} left G7 green -- the gate is not per-site"
    )


def test_typo_in_a_producer_reference_is_rejected(tmp_path):
    """A `steps.<id>` typo evaluates to null in Actions => the step never runs
    again, which is WORSE than the masking F44 removed."""
    text = WINVAL.read_text(encoding="utf-8").replace(
        "steps.build_release.outcome", "steps.build_relese.outcome")
    p = tmp_path / "wv.yml"
    p.write_text(text, encoding="utf-8")
    problems = _g7(p)
    # The consumer that still uses the STEP-level spelling after the chained-jobs
    # split is the installer step, inside the `release` job. Render moved to the
    # job-output spelling (see the companion test below), so naming render here
    # would be asserting against a topology the file no longer has.
    assert any("build_relese" in q and "NO step declares it" in q
               for q in problems), problems


def test_typo_in_a_job_output_producer_reference_is_rejected(tmp_path):
    """The cross-job spelling has the SAME null-evaluates-to-false hazard.

    `needs.release.outputs.release_buillt` is not an error in Actions -- it is
    empty, so the condition is permanently false and the step silently never
    runs again. This is the chained-jobs version of the typo above, and without
    it the refactor would have moved four consumers out of typo coverage.
    """
    text = WINVAL.read_text(encoding="utf-8").replace(
        "needs.release.outputs.release_built", "needs.release.outputs.release_buillt")
    p = tmp_path / "wv.yml"
    p.write_text(text, encoding="utf-8")
    problems = _g7(p)
    assert any("Run render-verification suite (Release pdfe.exe)" in q
               and "build_release" in q for q in problems), problems


def test_deleting_a_producer_id_declaration_is_rejected(tmp_path):
    """The other half: the `id:` line itself goes away, so every condition
    naming it silently becomes false."""
    text = WINVAL.read_text(encoding="utf-8").replace(
        "        id: build_release\n", "")
    p = tmp_path / "wv.yml"
    p.write_text(text, encoding="utf-8")
    problems = _g7(p)
    assert any("build_release" in q and "no step in this workflow declares that id"
               in q for q in problems), problems


def test_dropping_the_ui_only_term_is_rejected(tmp_path):
    text = WINVAL.read_text(encoding="utf-8").replace(
        " && !inputs.ui_only }}", " }}")
    p = tmp_path / "wv.yml"
    p.write_text(text, encoding="utf-8")
    problems = _g7(p)
    assert any("dropped the `!inputs.ui_only` term" in q for q in problems)


def test_swapping_a_producer_relation_is_rejected(tmp_path):
    """`!= 'skipped'` -> `== 'success'` on the JUnit verifier is the F44 defect
    in miniature: the adjudicator would stop running on the red runs it exists
    to adjudicate."""
    text = WINVAL.read_text(encoding="utf-8").replace(
        "steps.gtest.outcome != 'skipped'", "steps.gtest.outcome == 'success'")
    p = tmp_path / "wv.yml"
    p.write_text(text, encoding="utf-8")
    problems = _g7(p)
    assert any("steps.gtest.outcome != 'skipped'" in q for q in problems)


def test_a_new_tenth_masked_step_is_caught(tmp_path):
    """A tenth `!inputs.ui_only` site must be caught the day it lands.

    The vehicle must be a step that carries NO `if:` today, or the injection
    produces a duplicate key rather than a new masked site. It used to be
    *Build installer (CPack NSIS)*; the second karen gate on F46 found that
    step (and the DLL smoke test below it, and the WinAppDriver probe) still
    bare and had them guarded, which invalidated the vehicle. *Configure* is
    a setup step and is deliberately fail-fast, so it stays bare by policy.
    """
    vehicle = "      - name: Configure (windows-vs2022 preset)\n"
    text = WINVAL.read_text(encoding="utf-8")
    assert text.count(vehicle) == 1, "vehicle step must be unique"
    text = text.replace(vehicle, vehicle + "        if: ${{ !inputs.ui_only }}\n")
    p = tmp_path / "wv.yml"
    p.write_text(text, encoding="utf-8")
    problems = _g7(p)
    assert any("Configure (windows-vs2022 preset)" in q and "NEW instance" in q
               for q in problems), problems


def test_the_only_unguarded_steps_left_are_setup_steps(tmp_path):
    """Pins WHY the remaining bare steps are acceptable.

    F43's policy is that setup steps stay fail-fast and every CHECK step is
    guarded. Without this, "some steps have no `if:`" stays ambiguous forever
    and the next masked control hides among them -- which is exactly how
    *Build installer*, the DLL smoke test and the WinAppDriver probe survived
    the first pass of F46.
    """
    bare = [nm or "" for _ln, nm, cond, _sid, _cm
            in mod.parse_steps_rich(WINVAL) if not cond]
    # A SET, not a list. Since the chained-jobs split, checkout and the three
    # toolchain-env steps legitimately repeat once per job -- they mutate the
    # PROCESS environment, so a job that compiles must run them again. Counting
    # occurrences would make this test fail on every future job added or
    # removed, for no safety gain; what must not change is WHICH step names are
    # allowed to be unguarded.
    allowed = {
        "Checkout",
        "Export GitHub Actions cache env (for vcpkg x-gha)",
        "Set up MSVC environment",
        "Set up vcpkg (manifest mode)",
        "Fetch CNN font_embedder.onnx model",
        "Generate image-PDF test fixtures (a3_*.pdf + b3_*.pdf)",
        "Fetch ONNX Runtime pre-built SDK",
        "Configure (windows-vs2022 preset)",
    }
    unexpected = sorted(set(bare) - allowed)
    assert not unexpected, (
        "a step with NO `if:` appeared that is not a known setup step. If it is "
        "a CHECK step it is MASKED -- GitHub's implicit success() skips it "
        "whenever anything earlier is red. Got: %r" % (unexpected,))
    missing = sorted(allowed - set(bare))
    assert not missing, (
        "a setup step stopped being unguarded, or was renamed/removed: %r. Say "
        "why -- this list is the record of what is deliberately fail-fast."
        % (missing,))


# ------------------- G7, GitHub #205 half: a step with NO `if:` at all
#
# THE DEFECT THESE PIN.  The tenth-site loop opened with
# `if name in WV_DECISIONS or cond is None: continue`, so a gate step added with
# NO `if:` line -- the one shape to which GitHub applies an implicit success(),
# i.e. the original F44 shape -- was the one shape the detector skipped.  The
# F46 producer half reported a bare producer only for ids some other condition
# already named, so a brand new unconditional step nobody references was
# invisible to both halves.  These mutations plant such a step and require a
# verdict in BOTH directions: a gate step reddens, a setup step does not.

_SYNTH_ANCHOR = "      - name: Run render-verification suite (Release pdfe.exe)\n"


def _winval_with_step(tmp_path, block, filename="wv.yml"):
    """Live workflow + one synthetic step spliced in above the render step."""
    text = WINVAL.read_text(encoding="utf-8")
    assert text.count(_SYNTH_ANCHOR) == 1, "anchor step must be unique"
    p = tmp_path / filename
    p.write_text(text.replace(_SYNTH_ANCHOR, block + _SYNTH_ANCHOR),
                 encoding="utf-8")
    return p


_SYNTH_GATE = (
    "      - name: SYNTHETIC unconditional gate step (mutation self-test)\n"
    "        shell: pwsh\n"
    "        run: |\n"
    "          python \"$env:GITHUB_WORKSPACE\\tools\\gate\\check_render_junit.py\" `\n"
    "            \"$env:GITHUB_WORKSPACE\\render-junit.xml\"\n"
)

_SYNTH_SETUP = (
    "      - name: SYNTHETIC unconditional setup step (mutation self-test)\n"
    "        shell: pwsh\n"
    "        run: |\n"
    "          curl.exe -sSfL -o \"$env:GITHUB_WORKSPACE\\fixture.bin\" `\n"
    "            https://example.invalid/fixture.bin\n"
)

_SYNTH_ID = (
    "      - name: SYNTHETIC unconditional step declaring an id (mutation self-test)\n"
    "        id: synthetic_producer\n"
    "        shell: pwsh\n"
    "        run: |\n"
    "          Write-Host 'prepare'\n"
)


def test_a_gate_step_with_no_if_at_all_is_caught(tmp_path):
    """The exact GitHub #205 shape: no `if:`, and it runs a gate checker."""
    p = _winval_with_step(tmp_path, _SYNTH_GATE)
    problems = _g7(p)
    assert any("SYNTHETIC unconditional gate step" in q
               and "NO `if:` at all" in q
               and "tools/gate" in q for q in problems), problems


def test_a_gate_step_with_no_if_at_all_reddens_the_cli(tmp_path):
    """Non-zero EXIT, not just a problem string -- CI reads the exit code."""
    p = _winval_with_step(tmp_path, _SYNTH_GATE)
    rc = subprocess.run(
        [sys.executable, str(_HERE / "check_fast_checks_citations.py"),
         "--wv-file", str(p)],
        cwd=str(_REPO), capture_output=True, text=True,
    )
    assert rc.returncode == 1, rc.stdout + rc.stderr
    assert "SYNTHETIC unconditional gate step" in (rc.stdout + rc.stderr)


@pytest.mark.parametrize("runner,marker", [
    ("          python -m pytest tests\\ui_automation\\test_render_view_nav.py\n",
     "pytest"),
    ("          ctest -C Debug -L security\n", "ctest"),
    ("          \"$env:GITHUB_WORKSPACE\\build\\vs2022\\bin\\Debug\\pdfe_tests.exe\"\n",
     "pdfe_tests"),
    ("          Add-Content $env:GITHUB_STEP_SUMMARY 'verdict: ok'\n",
     "GITHUB_STEP_SUMMARY"),
])
def test_every_verdict_marker_reddens_an_unconditional_step(tmp_path, runner, marker):
    """One marker per mutant. A single mutant proves a single property, and a
    rule that only ever fires through `tools/gate` would pass a bare pytest
    step -- which is what the render step itself is."""
    block = ("      - name: SYNTHETIC unconditional %s step\n"
             "        shell: pwsh\n"
             "        run: |\n" % marker) + runner
    problems = _g7(_winval_with_step(tmp_path, block))
    assert any("SYNTHETIC unconditional %s step" % marker in q
               and "NO `if:` at all" in q for q in problems), problems


def test_an_unconditional_setup_step_stays_green(tmp_path):
    """The PAIRED direction. A rule that reddens every bare step would force an
    `if:` onto checkout and the toolchain env actions, and would then have to be
    silenced -- which is how the original `cond is None: continue` got written.
    A step that fetches a fixture publishes no verdict; skipping it hides
    nothing, because the job is already red from whatever skipped it."""
    problems = _g7(_winval_with_step(tmp_path, _SYNTH_SETUP))
    assert problems == [], (
        "a bare SETUP step reddened G7 -- the rule is not discriminating, it is "
        "just loud:\n  " + "\n  ".join(problems))


def test_an_unconditional_step_declaring_an_id_is_caught(tmp_path):
    """An id means some step is meant to read this one's `outcome`. F46 guards
    producers only once a consumer names them, so this catches the producer
    that lands before its consumer."""
    problems = _g7(_winval_with_step(tmp_path, _SYNTH_ID))
    assert any("synthetic_producer" in q and "NO `if:` at all" in q
               for q in problems), problems


def test_the_marker_scan_ignores_comments(tmp_path):
    """Several live setup steps mention `pdfe_tests` in their PROSE. Matching
    the raw block would flag them, the flags would be waived, and the waiver
    would be the allowlist this rule exists to avoid."""
    block = (
        "      - name: SYNTHETIC unconditional commented setup step\n"
        "        # This fixture is consumed later by pdfe_tests via ctest, and\n"
        "        # tools/gate/check_render_junit.py reads the pytest artifact.\n"
        "        shell: pwsh\n"
        "        run: |\n"
        "          Write-Host 'prepare'\n"
    )
    assert _g7(_winval_with_step(tmp_path, block)) == []


def test_guarding_the_synthetic_gate_step_clears_it(tmp_path):
    """The finding must be ANSWERABLE by the fix it prescribes, or it is noise."""
    guarded = _SYNTH_GATE.replace(
        "        shell: pwsh\n",
        "        if: ${{ !cancelled() && !inputs.ui_only }}\n        shell: pwsh\n")
    assert "if:" in guarded
    assert _g7(_winval_with_step(tmp_path, guarded)) == []


def test_g7_is_crlf_insensitive(tmp_path):
    """core.autocrlf=true: the committed fixture arrives CRLF after a fresh
    checkout and LF in the session that wrote it. G7 must give byte-identical
    verdicts for both, or it passes today and fails after any clone."""
    raw = FIX_WV.read_bytes().replace(b"\r\n", b"\n")
    lf = tmp_path / "lf.yml"
    crlf = tmp_path / "crlf.yml"
    lf.write_bytes(raw)
    crlf.write_bytes(raw.replace(b"\n", b"\r\n"))
    assert crlf.read_bytes() != lf.read_bytes()  # the mutation really happened
    a = [q.replace(str(lf), "X") for q in _g7(lf)]
    b = [q.replace(str(crlf), "X") for q in _g7(crlf)]
    assert a == b and len(a) > 0

    ok_lf = tmp_path / "ok_lf.yml"
    ok_crlf = tmp_path / "ok_crlf.yml"
    good = WINVAL.read_bytes().replace(b"\r\n", b"\n")
    ok_lf.write_bytes(good)
    ok_crlf.write_bytes(good.replace(b"\n", b"\r\n"))
    assert _g7(ok_lf) == [] and _g7(ok_crlf) == []


def test_every_live_condition_is_single_line():
    """The live workflow deliberately uses NO folded `if: >-` scalar.

    YAML does not fold lines that are MORE indented than the first content line
    -- it keeps their newlines -- so the tidy-looking folded form yields an
    expression string containing literal `\\n`. Actions' parser treats those as
    whitespace, but that is an assumption this repo cannot test locally, and an
    untestable assumption inside the fix for a masking defect is how the
    masking comes back. `Run ctest -L security` briefly used the folded form
    during F44 and was converted back.
    """
    for _ln, nm, cond, _sid, _cm in mod.parse_steps_rich(WINVAL):
        if cond and "ui_only" in cond:
            assert "\n" not in cond, f"{nm} carries a multi-line condition"


def test_parser_folds_a_folded_condition_anyway(tmp_path):
    """Capability regression control, kept even though the live file no longer
    needs it: the simple F43 parser reads `if: >-` as the literal string '>-',
    which would make G7 silently see no producer terms at all. If anyone
    reintroduces the folded form, G7 must still read it correctly rather than
    fail open."""
    p = tmp_path / "folded.yml"
    p.write_text(
        "jobs:\n  j:\n    steps:\n"
        "      - name: Folded step\n"
        "        if: >-\n"
        "          ${{ !cancelled()\n"
        "              && steps.build_debug.outcome == 'success'\n"
        "              && !inputs.ui_only }}\n"
        "        run: echo hi\n",
        encoding="utf-8")
    steps = mod.parse_steps_rich(p)
    cond = steps[0][2]
    assert cond not in (">", ">-", "|"), cond
    assert "steps.build_debug.outcome == 'success'" in cond
    assert "!inputs.ui_only" in cond
    assert mod._NONCANCELLED_RE.search(cond)


def test_wv_cli_discriminates():
    exe = [sys.executable, str(_HERE / "check_fast_checks_citations.py"), "--wv-file"]
    good = subprocess.run(exe + [str(WINVAL)], cwd=str(_REPO),
                          capture_output=True, text=True)
    bad = subprocess.run(exe + [str(FIX_WV)], cwd=str(_REPO),
                         capture_output=True, text=True)
    assert good.returncode == 0, good.stdout + good.stderr
    assert bad.returncode == 1, bad.stdout + bad.stderr


def test_windows_validation_is_not_in_the_citation_targets():
    """Deliberate scope statement, pinned so it is a DECISION and not a drift.

    107 `windows-validation.yml:NNN` citations across 26 tracked files would all
    need LIVE/HISTORICAL classification before this workflow could join
    TARGETS. F44 does not do that sweep; phase-46-dispositions.md records it.
    """
    assert mod.WINDOWS_VALIDATION not in mod.TARGETS


# ------------------------------------ G7, F46 half: the PRODUCERS must be guarded
#
# THE DEFECT THESE PIN.  F44 fixed the nine CONSUMERS and every artifact declared
# the step-skip masking removed.  It was not: four of the five producers those
# consumers name (`build_debug`, `pydeps`, `build_release`, `ui_e2e`) carried no
# `if:` at all, so implicit success() applied to the PRODUCER and an unrelated red
# above it skipped the producer -- making its outcome 'skipped' and skipping every
# consumer that named it.  The original G7 passed that workflow clean.
#
# DISCRIMINATING CONTROL: `fixtures/windows-validation-unguarded-producers-f46.yml`
# is the real workflow in exactly that state -- consumers fixed, producers bare.
# The pre-F44 snapshot CANNOT serve here: it has no producer terms at all, so the
# producer rule has nothing to derive and correctly stays silent on it.

FIX_WV_PROD = _HERE / "fixtures" / "windows-validation-unguarded-producers-f46.yml"

# Derived from the fixture, not from the brief, so a miscount cannot be inherited.
def _bare_producers_in(path):
    """Producer ids that carry NO `if:` in `path` -- DERIVED, not listed.

    This was a hardcoded 4-tuple, and when F46's own remediation added a fifth
    bare producer (`installer`) to the regenerated fixture, the parametrised
    mutations silently kept covering only the original four. The one producer
    guard the phase ADDED was the one with no single-site revert behind it, and
    the prose ("parametrised over all four producers") stayed literally true,
    which is exactly what hid it. Deriving the list means a sixth producer is
    covered the day it lands.
    """
    steps = mod.parse_steps_rich(path)
    named = set()
    for _ln, _nm, cond, _sid, _cm in steps:
        for m in re.finditer(r"steps\.([A-Za-z0-9_]+)\.outcome", cond or ""):
            named.add(m.group(1))
    bare = {sid for _ln, _nm, cond, sid, _cm in steps if sid and not cond}
    return tuple(sorted(named & bare))


_UNGUARDED_IN_FIXTURE = _bare_producers_in(FIX_WV_PROD)


def test_the_parametrised_producer_list_is_derived_and_complete():
    """Guards the guard: the mutation list must match the fixture.

    If this ever shrinks below the fixture's bare-producer set, some producer
    guard has lost its single-site mutation coverage -- the precise gap a karen
    gate found on F46's third pass.
    """
    assert _UNGUARDED_IN_FIXTURE, "no bare producers found -- the fixture or the parser broke"
    problems = _g7(FIX_WV_PROD)
    named = {p for p in _UNGUARDED_IN_FIXTURE
             if any("'%s'" % p in q for q in problems)}
    assert named == set(_UNGUARDED_IN_FIXTURE), (
        "every bare producer must be reported by G7: %r vs %r"
        % (sorted(named), sorted(_UNGUARDED_IN_FIXTURE)))


def _producers_of(path):
    """{producer id: condition or None}, derived exactly as the gate derives it."""
    steps = mod.parse_steps_rich(path)
    by_id = {sid: c for _ln, _n, c, sid, _cm in steps if sid}
    refs = set()
    for _ln, _n, cond, _sid, _cm in steps:
        if cond:
            refs.update(mod.producer_refs_in(cond))
    return {pid: by_id.get(pid) for pid in refs}


def test_the_producer_fixture_is_the_live_file_minus_the_producer_guards():
    """It must differ from the live workflow ONLY by the producer guards.

    A fixture that has drifted into something else stops being evidence about
    this defect.
    """
    prods = _producers_of(FIX_WV_PROD)
    live = set(_producers_of(WINVAL))
    # The fixture is a DATED SNAPSHOT and is deliberately never "fixed", so when
    # the live file legitimately loses a producer the right assertion is
    # containment, not equality. `ui_e2e` left the live producer set when the UI
    # leg became a 3-shard matrix and the matrix-health gate moved into its own
    # job -- there is no single ui_e2e step in that job to name, so its
    # dependency is `needs: [ui-e2e]` and check_chained_jobs.py enforces it.
    #
    # Containment still catches the thing this test exists for: a producer that
    # appears live but is absent from the fixture means the fixture has stopped
    # being evidence about the file it snapshots.
    # Producers introduced AFTER the snapshot was taken cannot be in it, by
    # construction. Each is listed individually rather than the assertion being
    # relaxed to "any difference is fine" -- an unexplained new producer is
    # still exactly the drift this test exists to catch.
    KNOWN_POST_SNAPSHOT = {
        # Became a producer when the cross-machine artifact upload started
        # naming it (multi-runner work: with 2-4 runners the machine that
        # builds is usually not the machine that tests).
        "publish_build",
        # Became a producer when the `build` job started publishing a
        # DEBUG-only closure for core-tests. core-tests declares
        # `needs: [build]` and so runs concurrently with `release`, where the
        # full closure is published -- it cannot wait for a store that does not
        # exist yet, so the Debug binaries are published by the job that makes
        # them, and the upload step names that publish.
        "publish_debug",
        # Became a producer when GitHub #182 split packaging out into its own
        # configure tree. The installer step used to name `build_release`
        # (build/vs2022, PDFE_TESTING=ON, the tree pdfe_tests.exe uses); it now
        # names the step that configures build/vs2022-package with
        # PDFE_TESTING=OFF, because that -- not the Release build of the test
        # tree -- is what it actually consumes.
        "configure_package",
    }
    missing_from_fixture = live - set(prods) - KNOWN_POST_SNAPSHOT
    assert not missing_from_fixture, (
        "the live workflow names producers the fixture does not, beyond the "
        "known post-snapshot additions: %r -- either the fixture has drifted "
        "into being about a different file, or a new producer needs adding to "
        "KNOWN_POST_SNAPSHOT with the reason it exists"
        % sorted(missing_from_fixture))
    assert set(prods) - live <= {"ui_e2e"}, (
        "the fixture names producers the live file dropped, beyond the known "
        "ui_e2e removal: %r" % sorted(set(prods) - live - {"ui_e2e"}))
    for pid in _UNGUARDED_IN_FIXTURE:
        assert prods[pid] is None, (
            f"fixture producer {pid!r} has acquired an `if:` -- it is supposed to "
            "be the BROKEN snapshot; do not 'fix' it"
        )


def test_producer_fixture_FAILS_g7_at_every_unguarded_producer():
    problems = _g7(FIX_WV_PROD)
    assert problems, (
        "the relocated-masking snapshot passes G7 -- the producer rule is decoration"
    )
    for pid in _UNGUARDED_IN_FIXTURE:
        assert any(f"id {pid!r}" in p and "has NO `if:` at all" in p
                   for p in problems), (
            f"G7 did not flag the unguarded producer {pid!r}; a per-producer "
            "control that misses a producer is not per-producer."
        )


def test_producer_fixture_also_flags_the_missing_decision_comment():
    problems = _g7(FIX_WV_PROD)
    for pid in _UNGUARDED_IN_FIXTURE:
        assert any(f"id {pid!r}" in p and mod.WV_PRODUCER_MARKER in p
                   for p in problems), pid


def _is_unconditional_rule(problem):
    """True for a finding from the GitHub #205 no-`if:`-at-all rule.

    Distinguished by wording, because that rule fires on the same STEPS as the
    F46 producer rule by design -- an unguarded producer is both a producer
    whose guard is missing and an unconditional verdict step. Two independent
    rules reaching the same conclusion is not double-counting to be silenced;
    it is why the one that was added later would have caught the case the
    earlier one only sees once a consumer names it.
    """
    return ("NO `if:` at all, yet it" in problem
            or "NO `if:` at all but DECLARES id" in problem)


def test_the_unconditional_rule_independently_catches_the_f46_producers():
    """The #205 rule must reach the F46 producers WITHOUT a consumer reference.

    F46's own rule only sees a producer once some other condition names its id.
    The whole GitHub #205 finding is that the producer landing FIRST is
    invisible. Measured on the F46 snapshot: build_debug, pydeps, build_release,
    installer and ui_e2e are all reported by the new rule alone.
    """
    problems = [p for p in _g7(FIX_WV_PROD) if _is_unconditional_rule(p)]
    for pid in ("build_debug", "pydeps", "build_release", "installer", "ui_e2e"):
        assert any(repr(pid) in p for p in problems), (pid, problems)


def test_the_ORIGINAL_consumer_rules_pass_the_producer_fixture():
    """THE WHOLE POINT OF F46, stated as a test.

    Every problem the fixture produces must come from the NEW producer rule (or,
    since 2026-09-18, from the GitHub #205 unconditional rule, which reaches the
    same steps by a different route -- see
    test_the_unconditional_rule_independently_catches_the_f46_producers, which
    pins that it really does). If the old CONSUMER rules also fired here, the
    fixture would not demonstrate that G7 used to certify this defect.
    """
    problems = _g7(FIX_WV_PROD)
    non_producer = [p for p in problems
                    if "PRODUCER step" not in p and not _is_unconditional_rule(p)]
    assert non_producer == [], (
        "the consumer-side G7 rules also fail this fixture, so it does not isolate "
        "the producer defect:\n  " + "\n  ".join(non_producer)
    )


def test_pre_f44_fixture_problem_count_is_unchanged_by_f46():
    """Regression pin on karen's control: exactly 26 problems.

    The pre-F44 snapshot has no producer terms, so the F46 rule must derive an
    empty producer set and add nothing.  A change here means the new rule is
    firing somewhere it has no business firing.

    WAS 28. It is 26 because WV_DECISIONS lost one entry -- the matrix-health
    gate, which moved out of the ui-e2e job when that leg became a 3-shard
    matrix. Each table entry contributes two problems against this snapshot (a
    missing producer term and a missing decision comment), so removing one entry
    removes exactly two: 28 - 2 = 26. The arithmetic is stated because "the
    number moved and I updated it" is how a control quietly stops meaning
    anything; if a future change moves it by anything other than 2 per removed
    entry, that is a finding, not a number to edit.

    2026-09-18, GitHub #205: the total is now 29. The consumer-rule count is
    UNCHANGED at 26 and is asserted separately below, so this control keeps its
    discriminating power instead of absorbing a new number. The three added are
    the unconditional-verdict-step rule firing on the snapshot's own bare gate
    steps -- *Build app + tests* (runs pdfe_tests), *Verify WinAppDriver
    version* (writes the job summary) and *Run UI E2E suite* (runs pytest). All
    three are real: in that snapshot each of them is deleted from the run by any
    earlier red, which is the defect the snapshot exists to record. A rule that
    left them alone would be the `cond is None: continue` it replaced.
    """
    problems = _g7(FIX_WV)
    consumer = [p for p in problems if not _is_unconditional_rule(p)]
    assert len(consumer) == 26, "\n  ".join(consumer)

    unconditional = [p for p in problems if _is_unconditional_rule(p)]
    assert len(unconditional) == 3, "\n  ".join(unconditional)
    for step in ("Build app + tests (debug-vs2022 preset)",
                 "Verify WinAppDriver version (PDFE-LIMITATION-WINAPPDRIVER-V0)",
                 "Run UI E2E suite (pywinauto + pyautogui via pytest)"):
        assert any(repr(step) in p for p in unconditional), (step, unconditional)

    assert len(problems) == 29


def test_every_live_producer_carries_a_status_check_function():
    prods = _producers_of(WINVAL)
    assert prods, "no producers derived from the live workflow -- parser broken"
    for pid, cond in sorted(prods.items()):
        assert cond is not None, f"live producer {pid!r} has no `if:`"
        assert mod._NONCANCELLED_RE.search(cond), f"live producer {pid!r}: {cond}"


def test_gtest_is_both_a_consumer_and_a_producer_and_is_guarded():
    """The fifth producer -- the one that already had an `if:`.  It is checked by
    BOTH halves of G7, and its F44 marker satisfies the producer-comment rule."""
    prods = _producers_of(WINVAL)
    assert "gtest" in prods
    assert mod._NONCANCELLED_RE.search(prods["gtest"])
    assert "Run pdfe_tests" in mod.WV_DECISIONS


# `ui_e2e` is excluded: these two mutate the LIVE workflow, and a producer the
# live file no longer NAMES cannot be reddened by stripping its guard. It left
# when matrix-health moved to its own job. Excluded here rather than in
# _UNGUARDED_IN_FIXTURE so the fixture-derivation test above still sees it.
_LIVE_PRODUCERS_TO_MUTATE = [p for p in _UNGUARDED_IN_FIXTURE if p != "ui_e2e"]


@pytest.mark.parametrize("pid", _LIVE_PRODUCERS_TO_MUTATE)
def test_stripping_one_single_producer_guard_reddens_g7(tmp_path, pid):
    """Mutation: remove the guard from exactly ONE live producer.

    Three producers stay guarded.  A rule that checked "some producer is
    guarded", or stopped at the first problem, would stay green here.
    """
    lines = WINVAL.read_text(encoding="utf-8").split("\n")
    target = None
    for ln, _nm, _c, sid, _cm in mod.parse_steps_rich(WINVAL):
        if sid == pid:
            target = ln
            break
    assert target is not None, f"no step declares id {pid!r}"

    out, i, done = [], 0, False
    while i < len(lines):
        if i + 1 == target:
            out.append(lines[i])
            i += 1
            while i < len(lines) and not mod._STEP_NAME_RE.match(lines[i]):
                if lines[i].strip().startswith("if:"):
                    done = True          # drop the `if:` line entirely
                    i += 1
                    continue
                out.append(lines[i])
                i += 1
            continue
        out.append(lines[i])
        i += 1
    assert done, f"could not find the `if:` of producer {pid!r} to strip"

    mutant = tmp_path / "windows-validation.yml"
    mutant.write_text("\n".join(out), encoding="utf-8")
    problems = _g7(mutant)
    assert any(f"id {pid!r}" in p and "has NO `if:` at all" in p
               for p in problems), (
        f"stripping ONLY the guard on {pid!r} left G7 green -- not per-producer"
    )


@pytest.mark.parametrize("pid", _LIVE_PRODUCERS_TO_MUTATE)
def test_weakening_a_producer_guard_to_a_bare_condition_is_rejected(tmp_path, pid):
    """The subtler mutation: the producer keeps an `if:`, but loses the status
    function.  Implicit success() comes straight back."""
    lines = WINVAL.read_text(encoding="utf-8").split("\n")
    target = None
    for ln, _nm, _c, sid, _cm in mod.parse_steps_rich(WINVAL):
        if sid == pid:
            target = ln
            break
    out, i, done = [], 0, False
    while i < len(lines):
        if i + 1 == target:
            out.append(lines[i])
            i += 1
            while i < len(lines) and not mod._STEP_NAME_RE.match(lines[i]):
                if lines[i].strip().startswith("if:"):
                    out.append("        if: ${{ github.event_name == 'schedule' }}")
                    done = True
                    i += 1
                    continue
                out.append(lines[i])
                i += 1
            continue
        out.append(lines[i])
        i += 1
    assert done
    mutant = tmp_path / "windows-validation.yml"
    mutant.write_text("\n".join(out), encoding="utf-8")
    problems = _g7(mutant)
    assert any(f"id {pid!r}" in p and "NO status-check function" in p
               for p in problems), problems


# Where the two synthetic-step injections below are spliced into a copy of the
# live workflow. It must match a step name that is REALLY THERE: when GitHub
# #182 renamed this step ("... from the PACKAGING tree") the stale marker turned
# `text.replace` into a no-op, so the injection silently did not happen and both
# tests failed reporting "no problems found" -- indistinguishable from the
# checker itself having broken. Hence the explicit assert at each site.
INJECTION_MARKER = "      - name: Build installer (CPack NSIS) from the PACKAGING tree\n"
_MARKER_GONE = (
    "injection marker %r is no longer present in windows-validation.yml. The "
    "mutation this test depends on would silently not apply, which reads "
    "exactly like a surviving mutant. Update INJECTION_MARKER to a step name "
    "that exists.")


def test_a_sixth_producer_is_covered_without_editing_the_script(tmp_path):
    """The producer set is derived FROM THE FILE, not from a pinned list.

    Add a tenth consumer naming a brand-new unguarded producer.  Nothing in
    WV_DECISIONS mentions either step, so a table-driven rule would miss both.
    """
    text = WINVAL.read_text(encoding="utf-8")
    marker = INJECTION_MARKER
    assert marker in text, _MARKER_GONE % marker
    injected = (
        "      - name: Brand new producer\n"
        "        id: newprod\n"
        "        shell: pwsh\n"
        "        run: |\n"
        "          Write-Host 'hi'\n"
        "\n"
        "      - name: Brand new consumer\n"
        "        # F44 STEP-MASKING DECISION - guarded consumer, bare producer.\n"
        "        if: ${{ !cancelled() && steps.newprod.outcome == 'success'"
        " && !inputs.ui_only }}\n"
        "        shell: pwsh\n"
        "        run: |\n"
        "          Write-Host 'hi'\n"
        "\n"
    )
    p = tmp_path / "wv.yml"
    p.write_text(text.replace(marker, injected + marker), encoding="utf-8")
    problems = _g7(p)
    assert any("id 'newprod'" in q and "has NO `if:` at all" in q
               for q in problems), problems


def test_an_undeclared_producer_id_is_reported_even_off_the_pinned_table(tmp_path):
    text = WINVAL.read_text(encoding="utf-8")
    marker = INJECTION_MARKER
    assert marker in text, _MARKER_GONE % marker
    injected = (
        "      - name: Consumer of a ghost\n"
        "        # F44 STEP-MASKING DECISION - names an id nothing declares.\n"
        "        if: ${{ !cancelled() && steps.ghost_id.outcome == 'success'"
        " && !inputs.ui_only }}\n"
        "        shell: pwsh\n"
        "        run: |\n"
        "          Write-Host 'hi'\n"
        "\n"
    )
    p = tmp_path / "wv.yml"
    p.write_text(text.replace(marker, injected + marker), encoding="utf-8")
    problems = _g7(p)
    assert any("'ghost_id'" in q and "NO step declares it" in q
               for q in problems), problems


def test_wv_cli_discriminates_on_the_producer_fixture():
    exe = [sys.executable, str(_HERE / "check_fast_checks_citations.py"), "--wv-file"]
    bad = subprocess.run(exe + [str(FIX_WV_PROD)], cwd=str(_REPO),
                         capture_output=True, text=True)
    assert bad.returncode == 1, bad.stdout + bad.stderr
    assert "PRODUCER step" in bad.stdout


def test_producer_fixture_is_tracked():
    """An untracked fixture is not a control -- it vanishes on a fresh clone."""
    out = subprocess.run(["git", "ls-files", "--error-unmatch",
                          "tools/gate/fixtures/windows-validation-unguarded-producers-f46.yml"],
                         cwd=str(_REPO), capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr


# ---------------------------------------------------- pytest.ini testpaths trap

def test_selftest_is_named_in_fast_checks():
    text = FAST_CHECKS.read_text(encoding="utf-8")
    assert "tools/gate/test_check_fast_checks_citations.py" in text, (
        "pytest.ini sets `testpaths = tests`, so this file runs NOWHERE unless the "
        '"Gate self-tests" step names it.'
    )


def test_checker_itself_is_a_fast_checks_step():
    text = FAST_CHECKS.read_text(encoding="utf-8")
    assert "python tools/gate/check_fast_checks_citations.py" in text


# ---------------------------------------------------------------------------
# G2 over windows-validation.yml (multi-runner work).
#
# G2 forbids `continue-on-error`, and its docstring has always cited
# windows-validation.yml's two false greens (R-32, GAP-A15) as the reason it
# exists -- but the rule only ever RAN over TARGETS, which is fast-checks.yml
# and governance.yml. The validation lane was never in scope, so a comment in
# that lane asserting "this lane forbids it" described a rule nothing enforced.
#
# These pin the new enforcement. Without them the rule was proved exactly once,
# by hand, by a gate agent -- which is not a property the next edit inherits.
# ---------------------------------------------------------------------------

def test_live_windows_validation_continue_on_error_is_allowlisted():
    """The live file passes -- both uses are known and justified."""
    problems = []
    mod.gate_windows_validation_continue_on_error(problems)
    assert problems == [], "live workflow fails G2:\n  " + "\n  ".join(problems)


def test_a_non_allowlisted_continue_on_error_is_caught(tmp_path):
    """THE defect. Adding the flag to a real gating step must redden, and the
    message must NAME that step -- 'the file fails' is not actionable."""
    text = WINVAL.read_text(encoding="utf-8")
    marker = "      - name: Run pdfe_tests"
    assert marker in text, "anchor step missing; update this test deliberately"
    i = text.index(marker)
    j = text.index("shell: pwsh", i)
    mutant = text[:j] + "continue-on-error: true\n        " + text[j:]

    p = tmp_path / "wv.yml"
    p.write_text(mutant, encoding="utf-8")
    problems = []
    mod.gate_windows_validation_continue_on_error(problems, p)
    assert any("Run pdfe_tests" in q and "continue-on-error" in q
               for q in problems), (
        "a continue-on-error on a gating step was NOT caught, or was caught "
        "without naming the step: %r" % problems)


def test_the_allowlist_names_steps_that_actually_exist():
    """Guards the allowlist itself.

    If a step is renamed, its allowlist entry stops matching -- which is SAFE
    (the use then gets flagged). The dangerous direction is an entry that
    matches nothing yet reads as if it authorises something: dead entries
    accumulate and the next reader assumes they are load-bearing.
    """
    live = {n for _ln, n, _c, _sid, _cm in mod.parse_steps_rich(WINVAL)}
    dead = sorted(set(mod.WV_CONTINUE_ON_ERROR_ALLOWED) - live)
    assert not dead, (
        "allowlist entries name steps that no longer exist: %r -- remove them "
        "or fix the names; a dead entry reads as an authorisation it is not"
        % dead)


def test_every_allowlist_entry_states_a_reason():
    """A bare permit is how an allowlist becomes a blanket ban-removal."""
    for step, reason in mod.WV_CONTINUE_ON_ERROR_ALLOWED.items():
        assert isinstance(reason, str) and len(reason.strip()) > 60, (
            "allowlist entry %r has no substantive reason: %r" % (step, reason))
