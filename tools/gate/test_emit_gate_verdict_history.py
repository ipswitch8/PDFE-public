# -*- coding: utf-8 -*-
"""The gate-verdict writer must RETAIN a superseded FAIL (GitHub #206).

THE DEFECT
==========
`emit-gate-verdict.sh` writes `<agent>.json` with `>` at a path fixed per
(phase, agent). So the ordinary cycle -- a gate FAILs, the finding is fixed, the
same gate re-runs green -- OVERWRITES the FAIL. Every completed pipeline is
therefore guaranteed to end up all-PASS, which is why a census of 784 artifacts
across 39 pipelines found zero FAIL. That is not an inability to express failure:
the emitter validates with `case "$VERDICT" in PASS|FAIL)` and writes FAIL
byte-correctly. The record simply does not survive being superseded.

Two real `claim-check` FAILs in `p87-open-issues` became real work and survive
nowhere on disk.

WHY THIS TEST PATCHES A COPY RATHER THAN THE LIVE HOOK
======================================================
`emit-gate-verdict.sh` lives in `~/.claude/hooks/`, outside this repository, and
it governs the gating of the very pipeline this test was written in. The patch's
own header says so, and records that a previous phase's attempt to edit it was
refused by the permission layer -- "the correct outcome: a phase editing the
mechanism that judges it should be a deliberate, reviewed act by the operator,
not a side effect of the work being judged."

So this test does the honest thing available to it: it applies the reviewed patch
to a COPY in a temporary directory and exercises that, proving

  (a) the patch still applies to the hook as it exists now -- a patch that has
      silently gone stale is indistinguishable from a fix that was never needed;
  (b) the patched writer keeps the earlier FAIL; and
  (c) `<agent>.json` is byte-identical to the unpatched writer's output, so no
      consumer's verdict resolution changes.

If the operator applies the patch for real, these same assertions hold against
the live hook; nothing here depends on it being a copy.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
PATCH = REPO_ROOT / "tools" / "gate" / "patches" / "emit-gate-verdict-history.patch"
LIVE_HOOK = Path(os.path.expanduser("~/.claude/hooks/emit-gate-verdict.sh"))

pytestmark = pytest.mark.skipif(
    not LIVE_HOOK.is_file() or not PATCH.is_file(),
    reason="needs ~/.claude/hooks/emit-gate-verdict.sh and the reviewed patch",
)


def _bash():
    for candidate in (r"C:\Program Files\Git\bin\bash.exe",
                      r"C:\Program Files\Git\usr\bin\bash.exe",
                      "bash"):
        if candidate == "bash" or os.path.isfile(candidate):
            return candidate
    return "bash"


def _run(script: Path, cwd: Path, *args):
    return subprocess.run(
        [_bash(), str(script), *args],
        cwd=str(cwd), capture_output=True, text=True,
        encoding="utf-8", errors="replace")


def _patched_copy(tmp: Path) -> Path:
    """The hook with the reviewed patch applied, or a hard failure."""
    dest = tmp / "emit-gate-verdict.sh"
    shutil.copy2(LIVE_HOOK, dest)
    os.chmod(dest, 0o755)
    proc = subprocess.run(
        ["patch", str(dest), str(PATCH)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    assert proc.returncode == 0, (
        "the reviewed patch no longer applies to the live hook -- it has gone "
        "stale, which is indistinguishable from a fix that was never needed:\n"
        + proc.stdout + proc.stderr)
    return dest


def test_the_reviewed_patch_still_applies_to_the_live_hook(tmp_path):
    """Checked on its own, because every assertion below depends on it and a
    stale patch should fail loudly here rather than as a confusing downstream
    error."""
    _patched_copy(tmp_path)


def test_a_superseded_fail_survives_in_the_history(tmp_path):
    """THE CRITERION. FAIL then PASS for the same (phase, agent): the artifact
    shows the latest verdict, and the history retains BOTH."""
    hook = _patched_copy(tmp_path)
    work = tmp_path / "work"
    work.mkdir()

    first = _run(hook, work, "phase-demo", "demo-gate", "FAIL", "--pipeline", "demo")
    assert first.returncode == 0, first.stdout + first.stderr
    second = _run(hook, work, "phase-demo", "demo-gate", "PASS", "--pipeline", "demo")
    assert second.returncode == 0, second.stdout + second.stderr

    base = work / ".claude" / "pipelines" / "demo"
    artifact = base / "gate-artifacts" / "phase-demo" / "demo-gate.json"
    history = base / "gate-history.jsonl"

    assert artifact.is_file(), "the artifact the gate hook reads must still exist"
    assert json.loads(artifact.read_text(encoding="utf-8"))["verdict"] == "PASS", \
        "the artifact must still carry the LATEST verdict"

    assert history.is_file(), (
        "no gate-history.jsonl was written, so the superseded FAIL is gone -- "
        "which is the whole defect in #206")
    lines = [json.loads(ln) for ln in
             history.read_text(encoding="utf-8").splitlines() if ln.strip()]
    verdicts = [ln.get("verdict") for ln in lines]
    assert verdicts == ["FAIL", "PASS"], (
        "the history must retain the earlier FAIL in order; got %r" % (verdicts,))


def test_the_artifact_is_byte_identical_to_the_unpatched_writer(tmp_path):
    """The patch must change nothing any consumer reads. `phase-gate.sh` resolves
    verdicts from `<agent>.json` alone, so if that file differs at all, the patch
    is not the no-op it claims to be."""
    patched_dir = tmp_path / "p"
    patched_dir.mkdir()
    patched = _patched_copy(patched_dir)

    pristine_dir = tmp_path / "u"
    pristine_dir.mkdir()
    pristine = pristine_dir / "emit-gate-verdict.sh"
    shutil.copy2(LIVE_HOOK, pristine)
    os.chmod(pristine, 0o755)

    out = {}
    for name, hook in (("patched", patched), ("pristine", pristine)):
        work = tmp_path / ("work-" + name)
        work.mkdir()
        proc = _run(hook, work, "phase-demo", "demo-gate", "PASS",
                    "--pipeline", "demo")
        assert proc.returncode == 0, proc.stdout + proc.stderr
        out[name] = (work / ".claude" / "pipelines" / "demo" / "gate-artifacts"
                     / "phase-demo" / "demo-gate.json").read_bytes()

    assert out["patched"] == out["pristine"], (
        "the patch changed the artifact a gate actually reads; it is supposed to "
        "add history WITHOUT touching verdict resolution")


def test_the_unpatched_writer_loses_the_fail(tmp_path):
    """Guard the guard. Without this, the test above would pass just as happily
    against a writer that already kept history, and would prove nothing about the
    patch. This pins that the defect is real in the CURRENT hook."""
    pristine_dir = tmp_path / "u"
    pristine_dir.mkdir()
    pristine = pristine_dir / "emit-gate-verdict.sh"
    shutil.copy2(LIVE_HOOK, pristine)
    os.chmod(pristine, 0o755)

    work = tmp_path / "work"
    work.mkdir()
    _run(pristine, work, "phase-demo", "demo-gate", "FAIL", "--pipeline", "demo")
    _run(pristine, work, "phase-demo", "demo-gate", "PASS", "--pipeline", "demo")

    base = work / ".claude" / "pipelines" / "demo"
    artifact = base / "gate-artifacts" / "phase-demo" / "demo-gate.json"
    assert json.loads(artifact.read_text(encoding="utf-8"))["verdict"] == "PASS"

    history = base / "gate-history.jsonl"
    assert not history.exists(), (
        "the live hook already keeps a history -- #206 may be fixed upstream, in "
        "which case this test and the patch should be retired rather than left "
        "asserting a defect that no longer exists")
