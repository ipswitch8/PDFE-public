#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_workflow_expressions.py.

WIRING (load-bearing). ``pytest.ini`` sets ``testpaths = tests``, so a
self-test living under ``tools/gate/`` runs NOWHERE unless named explicitly.
This file is named in the *"Gate self-tests + matrix pass-semantics"* step of
``.github/workflows/fast-checks.yml``.

WHAT IS BEING PROVEN, AND WHY IT IS NOT CIRCULAR

The guard exists because `df57fb7c` put `matrix.shard == 1` in
`jobs.ui-e2e.if`, which made windows-validation.yml an invalid workflow file —
undispatchable, with a failed run named after the file path on every push.
PyYAML parsed it, `${{`/`}}` balanced, and the 104-check local aggregate passed,
so nothing local objected.

Three levels, and the third is the one that matters:

  1. the pure predicate (`context_roots`) extracts roots and ignores functions
     and string literals;
  2. `check_workflow` applies rules A/B/C to synthetic documents, including the
     legal forms that must NOT be flagged;
  3. a DISCRIMINATION CONTROL against the REAL historical file — `git show
     df57fb7c:.github/workflows/windows-validation.yml` — because a guard that
     satisfies planted samples and misses the actual defect is decoration, and
     this project has a documented history of exactly that.
"""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys

import pytest

_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
_MOD_PATH = _HERE / "check_workflow_expressions.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_workflow_expressions",
                                                  _MOD_PATH)
    mod = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(mod)
    return mod


wfx = _load()


# ---------------------------------------------------------------- level 1
def test_functions_are_not_contexts():
    """`always()` / `cancelled()` are calls, not context references."""
    assert wfx.context_roots("${{ always() }}") == set()
    assert wfx.context_roots("${{ !cancelled() && success() }}") == set()


def test_string_literals_are_not_scanned():
    """A dotted identifier inside a quoted path is not a context.

    Regression: this was the guard's own false positive on
    `'bin/Debug/pdfe_tests.exe'`, which yielded a root of `pdfe_tests`.
    """
    expr = "${{ inputs.x == '' && 'bin/Debug/pdfe_tests.exe' || '' }}"
    assert wfx.context_roots(expr) == {"inputs"}


def test_roots_are_extracted():
    assert wfx.context_roots("${{ needs.build.outputs.built == 'success' }}") \
        == {"needs"}
    assert wfx.context_roots("${{ matrix.shard == 1 }}") == {"matrix"}


def test_an_unbraced_if_is_still_scanned():
    """GitHub allows a bare `if:` without `${{ }}`; the rule must still apply,
    or it could be bypassed by dropping the braces."""
    assert wfx.context_roots("matrix.shard == 1") == {"matrix"}


# ---------------------------------------------------------------- level 2
def _rules(doc):
    return sorted({f[2] for f in wfx.check_workflow("<test>", doc)})


def test_matrix_in_job_if_is_rejected():
    doc = {"jobs": {"ui-e2e": {"if": "${{ !cancelled() && matrix.shard == 1 }}"}}}
    assert _rules(doc) == ["A"]


def test_matrix_in_a_step_if_is_allowed():
    doc = {"jobs": {"ui-e2e": {"steps": [{"if": "${{ matrix.shard == 1 }}"}]}}}
    assert _rules(doc) == []


@pytest.mark.parametrize("cond", [
    "${{ !cancelled() }}",
    "${{ always() }}",
    "${{ !cancelled() && !inputs.ui_only }}",
    "${{ needs.build.outputs.built == 'success' }}",
    "${{ github.event_name == 'push' }}",
])
def test_legal_job_level_conditions_are_not_flagged(cond):
    assert _rules({"jobs": {"j": {"if": cond}}}) == []


def test_steps_in_job_outputs_is_allowed():
    """jobs.<id>.outputs is evaluated after the steps run; four real jobs in
    this repo export a step outcome that way."""
    doc = {"jobs": {"build": {"outputs":
                              {"built": "${{ steps.build_debug.outcome }}"}}}}
    assert _rules(doc) == []


def test_unknown_context_root_is_rejected():
    doc = {"jobs": {"a": {"steps": [{"run": "echo ${{ input.foo }}"}]}}}
    assert _rules(doc) == ["B"]


def test_non_vacuity_guard_rejects_an_empty_tree(tmp_path):
    """A scan that matches nothing must FAIL, not pass for free."""
    (tmp_path / ".github" / "workflows").mkdir(parents=True)
    # --min-gate-calls 0 isolates the job-if vacuity check; otherwise rule D's
    # own non-vacuity guard trips first and this test would pass for a reason
    # it is not trying to prove.
    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--repo-root", str(tmp_path), "--min-gate-calls", "0"],
                       capture_output=True, text=True)
    assert r.returncode != 0, "an empty workflow tree must not pass"
    assert "meaningless" in (r.stdout + r.stderr)


def test_rule_d_non_vacuity_guard_rejects_a_tree_with_no_gate_calls(tmp_path):
    """Rule D must refuse to pass when it inspected nothing."""
    d = tmp_path / ".github" / "workflows"
    d.mkdir(parents=True)
    (d / "w.yml").write_text(
        "on: push\njobs:\n  a:\n    if: ${{ github.ref == 'x' }}\n"
        "    steps:\n      - run: echo hi\n"
        "  b:\n    if: ${{ inputs.x }}\n    steps:\n      - run: echo hi\n"
        "  c:\n    if: ${{ needs.a.result }}\n    steps:\n      - run: echo hi\n",
        encoding="utf-8")
    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--repo-root", str(tmp_path)],
                       capture_output=True, text=True)
    assert r.returncode != 0, "rule D must not pass having checked 0 scripts"
    assert "carries no information" in (r.stdout + r.stderr)


# ---------------------------------------------------------------- level 3
def test_it_catches_the_real_historical_defect(tmp_path):
    """THE control: the actual file GitHub rejected must be reported.

    `df57fb7c` is the commit that introduced `matrix.shard` into
    `jobs.ui-e2e.if`. If this ever stops failing, the guard has lost the only
    defect it was built for.
    """
    blob = subprocess.run(
        ["git", "show", "df57fb7c:.github/workflows/windows-validation.yml"],
        cwd=str(_REPO), capture_output=True)
    if blob.returncode != 0 or not blob.stdout:
        pytest.skip("df57fb7c not reachable in this clone")

    d = tmp_path / ".github" / "workflows"
    d.mkdir(parents=True)
    (d / "windows-validation.yml").write_bytes(blob.stdout)

    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--repo-root", str(tmp_path), "--min-job-ifs", "1",
                        "--min-gate-calls", "0"],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    assert r.returncode != 0, "the guard passed the file GitHub rejected"
    assert "rule A" in out
    assert "`matrix` is not available in jobs.<id>.if" in out
    assert "ui-e2e" in out


# ------------------------------------------------- level 3, rule D
def test_rule_d_catches_this_guards_own_broken_placement(tmp_path):
    """THE control for rule D: the placement that actually reddened CI.

    `d8ee05bc` wired the guard step into `installer-dry-run`, which installs
    only pytest, so both invocations died on `PyYAML is required`
    (run 38198830707). Rule D exists for exactly this, and must name the job
    and the missing distribution.

    Only `tools/gate/*.py` is reconstructed, so unrelated local-module imports
    (`run_gtest`, `tests`) cannot resolve and are reported too. That is a
    property of the reduced fixture, not of the guard -- the real tree is
    clean -- so this asserts on the SPECIFIC finding rather than the count.
    """
    blob = subprocess.run(
        ["git", "show", "d8ee05bc:.github/workflows/fast-checks.yml"],
        cwd=str(_REPO), capture_output=True)
    if blob.returncode != 0 or not blob.stdout:
        pytest.skip("d8ee05bc not reachable in this clone")

    d = tmp_path / ".github" / "workflows"
    d.mkdir(parents=True)
    (d / "fast-checks.yml").write_bytes(blob.stdout)

    gate = tmp_path / "tools" / "gate"
    gate.mkdir(parents=True)
    listing = subprocess.run(["git", "ls-tree", "--name-only",
                              "d8ee05bc", "tools/gate/"],
                             cwd=str(_REPO), capture_output=True, text=True)
    for rel in listing.stdout.split():
        if not rel.endswith(".py"):
            continue
        content = subprocess.run(["git", "show", "d8ee05bc:" + rel],
                                 cwd=str(_REPO), capture_output=True)
        if content.returncode == 0:
            (tmp_path / rel).write_bytes(content.stdout)

    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--repo-root", str(tmp_path),
                        "--min-job-ifs", "0", "--min-gate-calls", "10"],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    assert r.returncode != 0, "rule D passed the placement that broke CI"
    assert "rule D" in out
    assert "installer-dry-run" in out, out[-1200:]
    assert "pyyaml" in out, out[-1200:]
    assert "check_workflow_expressions.py" in out, out[-1200:]


def test_rule_d_passes_the_fixed_tree():
    """And the relocated step must pass -- a guard that cannot pass is as
    useless as one that cannot fail."""
    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--repo-root", str(_REPO)],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-1500:]
    assert "dependency-checked" in r.stdout


def test_a_function_local_import_is_not_demanded():
    """The `cryptography` false positive: a function-local import is
    conditional by construction and handled as `unavailable`."""
    doc = {"jobs": {"j": {"steps": [
        {"run": "python -m pip install pytest"},
        {"run": "python tools/gate/g.py"}]}}}
    import tempfile, os as _os, shutil
    td = tempfile.mkdtemp()
    try:
        gd = _os.path.join(td, "tools", "gate")
        _os.makedirs(gd)
        with open(_os.path.join(gd, "g.py"), "w", encoding="utf-8") as fh:
            fh.write("def f():\n    import cryptography\n    return 1\n")
        assert wfx.check_job_dependencies("<t>", doc, td) == []
    finally:
        shutil.rmtree(td, ignore_errors=True)


def test_a_module_level_try_guarded_import_is_demanded():
    """The inverse: this script's own `yaml` import sits in a try/except that
    exits 2. Defensive, not optional."""
    doc = {"jobs": {"j": {"steps": [
        {"run": "python -m pip install pytest"},
        {"run": "python tools/gate/g.py"}]}}}
    import tempfile, os as _os, shutil
    td = tempfile.mkdtemp()
    try:
        gd = _os.path.join(td, "tools", "gate")
        _os.makedirs(gd)
        with open(_os.path.join(gd, "g.py"), "w", encoding="utf-8") as fh:
            fh.write("try:\n    import yaml\nexcept ImportError:\n"
                     "    raise SystemExit(2)\n")
        f = wfx.check_job_dependencies("<t>", doc, td)
        assert [x[2] for x in f] == ["D"], f
        assert "pyyaml" in f[0][3]
    finally:
        shutil.rmtree(td, ignore_errors=True)


def test_the_current_tree_passes():
    """And the FIXED tree must pass -- a guard that cannot pass is as useless as
    one that cannot fail."""
    r = subprocess.run([sys.executable, str(_MOD_PATH),
                        "--repo-root", str(_REPO)],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-1500:]


def test_module_self_test_passes():
    r = subprocess.run([sys.executable, str(_MOD_PATH), "--self-test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-1500:]
