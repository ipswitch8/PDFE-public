# -*- coding: utf-8 -*-
"""Tests for the local fast-checks runner (DCA 0051).

WHY THIS EXISTS. The runner is the class-level defence for "CI was red for ten
pushes and nobody looked". A defence nobody has proved can fail is decoration,
and this one has two specific ways to be quietly useless:

  1. **Under-discovery.** Its first version matched one of the three invocation
     shapes in `fast-checks.yml`, found 16 of 64 referenced scripts, and would
     have printed a confident PASS having run a quarter of the guards. That is
     the same silent under-coverage it was written to fix, rebuilt inside the
     fix.
  2. **False reds.** Its second version dropped the arguments off folded
     `run: >` blocks, so two correct guards failed purely because the runner
     invoked them wrong. DCA 0047's finding is that a guard which reddens on
     correct code gets muted -- at which point it is not there for the real case.

Both are pinned below, because both actually happened.
"""
from __future__ import annotations

import importlib.util
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "run_fast_checks_locally_under_test",
    ROOT / "tools" / "gate" / "run_fast_checks_locally.py")
assert _SPEC and _SPEC.loader
runner = importlib.util.module_from_spec(_SPEC)
sys.modules["run_fast_checks_locally_under_test"] = runner
_SPEC.loader.exec_module(runner)


def test_every_referenced_gate_script_is_discovered():
    """The coverage assertion: discovery must not silently miss a shape.

    This is the one that matters. If a future edit to fast-checks.yml uses a
    fourth invocation shape, this fails rather than the runner quietly covering
    less and still saying OK.
    """
    cmds = runner.discover(runner.WORKFLOW)
    referenced = runner.referenced_scripts(runner.WORKFLOW)
    discovered = runner.discovered_scripts(cmds)
    missing = sorted(referenced - discovered)
    assert not missing, (
        "fast-checks.yml references these gate scripts but the runner does not "
        "discover them, so it would report a pass it never established: "
        + ", ".join(missing))


def test_discovery_is_not_vacuous():
    cmds = runner.discover(runner.WORKFLOW)
    assert len(cmds) > 20, (
        f"only {len(cmds)} invocations discovered; a near-empty discovery would "
        f"make every later assertion pass trivially")


def test_folded_block_arguments_are_absorbed():
    """A `run: >` command must keep its arguments.

    Without this the runner invokes e.g. check_magnitude_guard.py with no
    --paths/--baseline and reports a failure that is entirely its own fault.
    """
    cmds = runner.discover(runner.WORKFLOW)
    for script in ("check_reachability_skip_shape.py", "check_magnitude_guard.py"):
        matches = [c for c in cmds if script in c]
        assert matches, f"{script} not discovered at all"
        assert any("--baseline" in c and "--paths" in c for c in matches), (
            f"{script} was discovered without its folded arguments: {matches!r}")


def test_negative_controls_are_not_run_as_ordinary_checks():
    """The red-sentinel step EXPECTS a non-zero exit and inverts it in-step.

    Running it bare and calling non-zero a failure would report correct
    behaviour as broken.
    """
    negatives = runner.negative_control_commands(runner.WORKFLOW)
    assert any("sentinel_red" in c for c in negatives), (
        "the GAP-A15 red sentinel was not recognised as a negative control")
    cmd = [c for c in negatives if "sentinel_red" in c][0]
    ok, why = runner.classify(cmd, negatives)
    assert not ok and "negative control" in why


def test_context_needing_checks_are_skipped_with_a_reason():
    """check_dca_presence needs a base/head pair CI supplies. Skipped, not run."""
    cmds = runner.discover(runner.WORKFLOW)
    dca = [c for c in cmds if "check_dca_presence.py" in c]
    assert dca, "check_dca_presence.py not discovered"
    ok, why = runner.classify(dca[0], runner.negative_control_commands(runner.WORKFLOW))
    assert not ok and "GitHub run context" in why


def test_missing_workflow_is_a_failure_not_a_pass(tmp_path):
    """A runner that cannot find the workflow must NOT report success.

    The dangerous failure mode for this tool is answering "OK" when it did
    nothing, so the empty case is pinned explicitly.
    """
    assert runner.discover(tmp_path / "nope.yml") == []


def test_self_test_passes_on_the_real_workflow():
    assert runner.self_test() == 0


# ---------------------------------------------------------------------------
# GitHub #260 -- the mirror modelled commands and not JOBS.
#
# Every step looked alike to it, and the developer's interpreter has every
# package, so a step placed in a job that never installs what it imports passed
# here and died on CI with ModuleNotFoundError. That is precisely how commit
# 4cd0e101 went red.
# ---------------------------------------------------------------------------
_WF_TWO_JOBS = """\
name: demo
on:
  push:
jobs:
  rich-job:
    steps:
      - name: deps
        run: python -m pip install --quiet pytest pyyaml
      - name: use yaml
        run: python tools/gate/needs_yaml.py
  poor-job:
    steps:
      - name: deps
        run: python -m pip install --quiet pytest
      - name: use yaml
        run: python tools/gate/needs_yaml.py
"""


def _stage(tmp_path, workflow_text, script_text):
    wf = tmp_path / ".github" / "workflows" / "fast-checks.yml"
    wf.parent.mkdir(parents=True)
    wf.write_text(workflow_text, encoding="utf-8")
    script = tmp_path / "tools" / "gate" / "needs_yaml.py"
    script.parent.mkdir(parents=True)
    script.write_text(script_text, encoding="utf-8")
    return wf


def test_jobs_and_their_installs_are_modelled(tmp_path):
    wf = _stage(tmp_path, _WF_TWO_JOBS, "import yaml\n")
    jobs = runner.jobs_with_steps(wf)
    assert set(jobs) == {"rich-job", "poor-job"}
    assert "pyyaml" in jobs["rich-job"]["installs"]
    assert "pyyaml" not in jobs["poor-job"]["installs"]


def test_a_step_in_a_job_lacking_its_dependency_is_flagged(tmp_path):
    wf = _stage(tmp_path, _WF_TWO_JOBS, "import yaml\n")
    problems = runner.missing_job_dependencies(wf, tmp_path)
    jobs = {j for j, _c, _m, _p in problems}
    assert jobs == {"poor-job"}, problems
    assert all(m == "yaml" and p == "pyyaml" for _j, _c, m, p in problems)


def test_the_same_step_in_the_job_that_installs_it_is_not_flagged(tmp_path):
    """The check must discriminate, or it is a ban on the import."""
    wf = _stage(tmp_path, _WF_TWO_JOBS, "import yaml\n")
    problems = runner.missing_job_dependencies(wf, tmp_path)
    assert "rich-job" not in {j for j, _c, _m, _p in problems}


def test_an_optional_import_inside_try_is_not_flagged(tmp_path):
    """A guarded import is a documented degradation, not a latent crash.

    `check_crl_chain_fixture.py` imports cryptography in a try block and prints
    "regeneration check unavailable" when it is missing -- which CI shows today
    and which is working as designed. Flagging it would redden the build for
    correct behaviour, and a guard that reddens on correct code gets muted.
    """
    script = ("try:\n    import yaml\nexcept ImportError:\n    yaml = None\n")
    wf = _stage(tmp_path, _WF_TWO_JOBS, script)
    assert runner.missing_job_dependencies(wf, tmp_path) == []


def test_the_live_workflow_has_no_job_dependency_mismatch():
    """The real tree must be clean, or this check would be reporting a bug it
    cannot distinguish from its own false positives."""
    problems = runner.missing_job_dependencies(runner.WORKFLOW, runner.REPO_ROOT)
    assert problems == [], problems


# ---------------------------------------------------------------------------
# GitHub #311 — a script named only in a YAML COMMENT is not "referenced".
#
# `referenced_scripts()` used to grep the whole workflow text, so the comment
# at `fast-checks.yml:292` ("tree is tests/scripts/remediation_summary.py,
# which builds an ...") counted as a reference. The workflow never invokes that
# script, so `test_every_referenced_gate_script_is_discovered` failed
# permanently and `fast-checks` could not go green from a clean checkout.
#
# These pin the property rather than the symptom: a comment is not an
# invocation. Without them the next cleverer-regex rewrite reintroduces the bug
# silently, and the suggestion recorded on #311 was explicitly that adding
# `remediation_summary.py` to an allowlist would hide the symptom and leave the
# comment-blindness in place.
# ---------------------------------------------------------------------------


def _write_workflow(tmp_path, body: str):
    wf = tmp_path / "wf.yml"
    io.open(wf, "w", encoding="utf-8", newline="\n").write(body)
    return wf


def test_a_script_named_only_in_a_comment_is_not_referenced(tmp_path):
    wf = _write_workflow(tmp_path, """
name: fixture
jobs:
  checks:
    runs-on: windows-latest
    steps:
      - name: a real invocation
        run: python tools/gate/check_real.py
      # prose about tests/scripts/only_in_a_comment.py, which is not run here
      - name: another
        run: |
          python tools/gate/check_second.py
          # python tools/gate/check_commented_out.py
""")
    refs = runner.referenced_scripts(wf)
    assert "check_real.py" in refs
    assert "check_second.py" in refs
    assert "only_in_a_comment.py" not in refs, (
        "GitHub #311: a script mentioned only in a YAML comment is being "
        "counted as referenced by the workflow. The coverage comparison then "
        "demands the runner discover a command that does not exist, and the "
        "guard is red for a reason unrelated to any push.")
    assert "check_commented_out.py" not in refs, (
        "a commented-out line inside a run: block is not an invocation either")


def test_the_real_workflows_commented_script_is_not_counted():
    # The specific instance #311 reports, asserted against the real file so a
    # future edit that reintroduces the scrape cannot pass this.
    refs = runner.referenced_scripts(runner.WORKFLOW)
    assert "remediation_summary.py" not in refs, (
        "GitHub #311: remediation_summary.py appears in fast-checks.yml ONLY "
        "inside a comment (line 292) and the real file lives in "
        "tests/scripts/, yet it is being reported as referenced.")


def test_referenced_scripts_is_not_vacuous_on_the_real_workflow():
    # The fix narrows what counts as a reference, so the failure mode to guard
    # against now is the opposite one: narrowing it to nothing would make the
    # coverage comparison pass by measuring an empty set.
    refs = runner.referenced_scripts(runner.WORKFLOW)
    assert len(refs) > 20, (
        f"only {len(refs)} referenced script(s) found in the real workflow; "
        f"the yardstick has collapsed and the coverage check is now vacuous")
