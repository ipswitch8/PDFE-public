#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for tools/ops/runner_watchdog.ps1's heartbeat (meta-audit F44, row F46).

WIRING (load-bearing). ``pytest.ini`` sets ``testpaths = tests``, so a
self-test living under ``tools/gate/`` runs **NOWHERE** unless named
explicitly. This file is named in the *"Gate self-tests + matrix
pass-semantics"* step of ``.github/workflows/fast-checks.yml``.

WHAT IS BEING PROVEN, AND WHY IT IS NOT CIRCULAR
================================================
F46's finding is that the old watchdog's log **last moved on 2026-07-03** while
the runner it watches died around **2026-07-25**. The root cause is not a
crash-prone script: it is that the old script emitted output ONLY on a state
change, so "alive and quiet" and "dead" were the same observable.

So the fix is a heartbeat written on EVERY iteration, and the thing that must be
proven is that the heartbeat mechanism actually works — the same class of claim
that, unproven, produced F46 in the first place. Three levels:

1. ``-SelfTest`` exercises write / re-read / field validation / advance /
   staleness in a temp directory and exits non-zero on any failure.
2. The REAL loop is run (``-NoRestart``, bounded iterations, temp paths) and the
   heartbeat file is inspected from Python — an independent reader, not the
   script's own assertions — proving it advances **while nothing changes**,
   which is exactly what the predecessor did not do.
3. A DISCRIMINATION CONTROL deliberately breaks the heartbeat in a copy of the
   script and requires ``-SelfTest`` to go RED. A self-check that cannot fail is
   decoration, and this project has a documented history of exactly that
   (``gate_caught_fake_fixes``).

``-NoRestart`` and temp paths are mandatory here: the live runner is online and
may be executing a job, and interfering with it wedges it.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
SCRIPT = _REPO / "tools" / "ops" / "runner_watchdog.ps1"


def _powershell() -> str:
    """pwsh is preinstalled on GitHub-hosted runners and on this Windows host.

    Deliberately NOT a skip: 'the heartbeat is unverified because no shell was
    found' is the F46 failure mode wearing a green tick.
    """
    for exe in ("pwsh", "powershell"):
        found = shutil.which(exe)
        if found:
            return found
    raise AssertionError(
        "neither pwsh nor powershell is on PATH; the watchdog heartbeat cannot "
        "be verified. Install PowerShell rather than skipping this test.")


def _run(*args, script: Path = None, timeout: int = 180):
    exe = _powershell()
    argv = [exe, "-NoProfile", "-NonInteractive", "-File", str(script or SCRIPT), *args]
    return subprocess.run(argv, capture_output=True, text=True, encoding="utf-8",
                          timeout=timeout, cwd=str(_REPO))


def test_the_script_exists_and_fails_loudly_rather_than_silently():
    assert SCRIPT.exists(), SCRIPT
    text = SCRIPT.read_text(encoding="utf-8")
    assert "$ErrorActionPreference = 'Stop'" in text, (
        "the predecessor set SilentlyContinue at the top of a script whose only "
        "output is its log, discarding every error in that path")
    # Match the STATEMENT, not the word: the script's own docstring quotes the
    # predecessor's `$ErrorActionPreference = 'SilentlyContinue'` while
    # explaining why it was wrong, and a substring search would flag that
    # explanation as the defect it describes.
    blanket = [ln for ln in text.splitlines()
               if re.match(r"^\s*\$ErrorActionPreference\s*=\s*'SilentlyContinue'\s*$", ln)]
    assert not blanket, (
        "a blanket SilentlyContinue preference has come back: %r" % blanket)
    # Per-call `-ErrorAction SilentlyContinue` is fine and is used deliberately
    # for the process lookup, where "no such process" is the normal answer.


# ---------------------------------------------------------------------------
# 1. The script's own self-check
# ---------------------------------------------------------------------------

def test_selftest_passes_and_reports_every_check():
    p = _run("-SelfTest")
    out = p.stdout + p.stderr
    assert p.returncode == 0, out
    assert "[watchdog-selftest] OK" in out, out
    passes = len(re.findall(r"^  PASS ", out, re.M))
    fails = len(re.findall(r"^  FAIL ", out, re.M))
    assert fails == 0, out
    # Derived, not asserted from memory: the self-check must actually be
    # checking a substantial number of things, not printing one green line.
    assert passes >= 20, "only %d PASS lines:\n%s" % (passes, out)


def test_selftest_touches_no_live_runner_path():
    """The live runner is online and may be mid-job. -SelfTest must confine
    itself to a temp directory."""
    p = _run("-SelfTest")
    out = p.stdout + p.stderr
    assert "actions-runner" not in out, out
    m = re.search(r"scratch: (.+)", out)
    assert m, out
    scratch = m.group(1).strip()
    assert "pdfe-watchdog-selftest-" in scratch, scratch
    # ...and it cleans up after itself.
    assert not Path(scratch).exists(), scratch


# ---------------------------------------------------------------------------
# 2. THE DISCRIMINATION CONTROL — the self-check must be able to go RED
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("mutation,label", [
    # The heartbeat never advances: the exact defect class F46 describes, where
    # a monitor's output is indistinguishable between "quiet" and "dead".
    ("        timestampUtc     = (Get-UtcStamp)",
     "frozen timestamp"),
    # The heartbeat is never written at all.
    ("    Move-Item -LiteralPath $tmp -Destination $Path -Force",
     "heartbeat never lands"),
])
def test_selftest_FAILS_when_the_heartbeat_mechanism_is_broken(tmp_path, mutation, label):
    """Break the mechanism in a copy and require RED.

    Without this, ``-SelfTest`` proves only that it prints PASS lines. This
    project has burned a phase on instrumentation that shared a bug with its own
    fix (see the ``gate_caught_fake_fixes`` record), so the control is here.
    """
    text = SCRIPT.read_text(encoding="utf-8")
    assert mutation in text, "mutation anchor no longer present: %r" % mutation
    if label == "frozen timestamp":
        broken = text.replace(
            mutation,
            "        timestampUtc     = '2026-07-03T12:52:02.000Z'")
    else:
        broken = text.replace(mutation, "    # heartbeat deliberately not written")
    victim = tmp_path / "runner_watchdog_broken.ps1"
    victim.write_text(broken, encoding="utf-8")

    p = _run("-SelfTest", script=victim)
    out = p.stdout + p.stderr
    assert p.returncode != 0, (
        "the self-check passed against a watchdog whose heartbeat is broken (%s); "
        "it is decoration, not a check:\n%s" % (label, out))
    assert "FAIL" in out, out


# ---------------------------------------------------------------------------
# 3. The REAL loop, read back by an independent reader
# ---------------------------------------------------------------------------

def test_real_loop_writes_an_advancing_heartbeat_with_no_state_change(tmp_path):
    hb = tmp_path / "watchdog-heartbeat.json"
    log = tmp_path / "watchdog.log"
    before = datetime.now(timezone.utc) - timedelta(seconds=5)

    p = _run("-RunnerDir", str(tmp_path),
             "-HeartbeatPath", str(hb),
             "-LogPath", str(log),
             "-IntervalSeconds", "1",
             "-MaxIterations", "3",
             # -IgnoreDesktopState: this test asserts on listenerState, and
             # without the flag the watchdog reports "paused-no-desktop"
             # whenever the operator happens to be disconnected -- which is
             # the NORMAL state of a headless runner host. Measured: these
             # tests went red during a real RDP disconnect (#133). A gate
             # self-test whose result depends on whether someone is looking
             # at the screen is the false-failure class this whole feature
             # exists to remove.
             "-IgnoreDesktopState",
             "-NoRestart")
    out = p.stdout + p.stderr
    assert p.returncode == 0, out

    assert hb.exists(), "no heartbeat file was produced:\n%s" % out
    beat = json.loads(hb.read_text(encoding="utf-8"))
    assert beat["schema"] == "pdfe.runner-watchdog.heartbeat/1", beat
    # THE FIX, stated behaviourally: three iterations happened, the listener
    # state never changed, and the heartbeat still advanced to 3. The old
    # watchdog would have written nothing at all across those iterations.
    assert beat["iteration"] == 3, beat
    assert beat["listenerState"] in ("up", "down"), beat
    assert int(beat["intervalSeconds"]) == 1, beat
    assert int(beat["watchdogPid"]) > 0, beat

    ts = datetime.fromisoformat(beat["timestampUtc"].replace("Z", "+00:00"))
    assert ts >= before, (ts, before)
    assert ts <= datetime.now(timezone.utc) + timedelta(seconds=5), ts

    assert log.exists(), out
    logtext = log.read_text(encoding="utf-8", errors="replace")
    assert "watchdog started" in logtext, logtext


def test_real_loop_with_norestart_never_launches_run_cmd(tmp_path):
    """The live runner must never be disturbed. -NoRestart observes only, and
    the run.cmd this temp dir does not contain must not be launched."""
    hb = tmp_path / "hb.json"
    p = _run("-RunnerDir", str(tmp_path),
             "-HeartbeatPath", str(hb),
             "-LogPath", str(tmp_path / "wd.log"),
             "-IntervalSeconds", "1", "-MaxIterations", "1",
             "-IgnoreDesktopState", "-NoRestart")
    out = p.stdout + p.stderr
    assert p.returncode == 0, out
    assert "launching detached run.cmd" not in out, out
    assert hb.exists()


def test_heartbeat_is_written_even_when_the_listener_is_down(tmp_path):
    """The state that matters most is the one the old script handled worst:
    the heartbeat must exist and record `down`, not merely log a restart."""
    hb = tmp_path / "hb.json"
    # tmp_path holds no run.cmd, so if a listener is somehow running on this
    # host the state will read `up`; the assertion below covers both and pins
    # the invariant that matters: the file is written either way.
    p = _run("-RunnerDir", str(tmp_path), "-HeartbeatPath", str(hb),
             "-LogPath", str(tmp_path / "wd.log"),
             "-IntervalSeconds", "1", "-MaxIterations", "1",
             "-IgnoreDesktopState", "-NoRestart")
    assert p.returncode == 0, p.stdout + p.stderr
    beat = json.loads(hb.read_text(encoding="utf-8"))
    assert beat["listenerState"] in ("up", "down")
    if beat["listenerState"] == "down":
        assert int(beat["listenerPid"]) == 0, beat


# ---------------------------------------------------------------------------
# 3b. THE EXIT CODE MUST CARRY THE HEARTBEAT  (2026-10-03)
# ---------------------------------------------------------------------------
# Measured on the live host: pointed at the retired C:\Users\dev\actions-runner,
# the watchdog logged
#     ERROR: heartbeat write failed: Access to the path 'C:\Users\dev' is denied.
# on all three iterations, could not append to its log either, and **exited 0**.
#
# Every test above this point passes against that build, because each one supplies
# a writable -HeartbeatPath and then reads the file back. None of them asks what
# the process reports when the heartbeat does NOT land -- and that is F46's own
# defect (a healthy-quiet watchdog and a dead one producing the same observable)
# reappearing in the exit code, the one channel a scheduled task or service
# wrapper actually reads. Hence these.


def test_a_runner_dir_that_does_not_exist_is_REFUSED_not_warned_about(tmp_path):
    """A watchdog aimed at a nonexistent runner directory must not report success."""
    missing = tmp_path / "no-such-runner-dir"
    p = _run("-RunnerDir", str(missing),
             "-IntervalSeconds", "1", "-MaxIterations", "1",
             "-IgnoreDesktopState", "-NoRestart")
    out = p.stdout + p.stderr
    assert p.returncode != 0, (
        "the watchdog ran and exited 0 against a RunnerDir that does not exist; "
        "this is how a stale default survived unnoticed:\n%s" % out)
    assert "FATAL" in out, out
    assert str(missing) in out, out


def test_exit_is_NONZERO_when_no_heartbeat_can_be_written(tmp_path):
    """The headline regression: heartbeat writes fail on every iteration.

    The parent of the heartbeat path is an ordinary FILE, so the directory
    creation inside Write-Heartbeat cannot succeed and no beat can ever land.
    The loop is still expected to RUN (a monitor must not die on an I/O fault --
    the restart logic is the half that still has value); what must not happen is
    exit 0.
    """
    blocker = tmp_path / "not-a-directory"
    blocker.write_text("this is a file, not a directory\n", encoding="utf-8")
    hb = blocker / "nested" / "watchdog-heartbeat.json"

    p = _run("-RunnerDir", str(tmp_path),
             "-HeartbeatPath", str(hb),
             "-LogPath", str(tmp_path / "wd.log"),
             "-IntervalSeconds", "1", "-MaxIterations", "2",
             "-IgnoreDesktopState", "-NoRestart")
    out = p.stdout + p.stderr

    assert not hb.exists(), "the heartbeat landed after all; this test proves nothing"
    assert "heartbeat write failed" in out, out
    assert p.returncode != 0, (
        "every heartbeat write failed and the watchdog still exited 0 -- the exact "
        "defect measured on 2026-10-03:\n%s" % out)
    # The loop must have completed its iterations rather than aborting on the
    # first fault: a monitor that dies on a transient blip is a worse monitor.
    assert "reached MaxIterations=2" in out, out
    assert "heartbeats written=0" in out, out


def test_a_healthy_bounded_run_still_exits_zero(tmp_path):
    """The companion control. A guard that can only redden is as useless as one
    that can only green (``gate_caught_fake_fixes``), so pin the other side."""
    hb = tmp_path / "hb.json"
    p = _run("-RunnerDir", str(tmp_path), "-HeartbeatPath", str(hb),
             "-LogPath", str(tmp_path / "wd.log"),
             "-IntervalSeconds", "1", "-MaxIterations", "2",
             "-IgnoreDesktopState", "-NoRestart")
    out = p.stdout + p.stderr
    assert p.returncode == 0, out
    assert "heartbeats written=2 failed=0" in out, out
    assert json.loads(hb.read_text(encoding="utf-8"))["iteration"] == 2


def test_no_tracked_script_still_defaults_to_the_retired_runner_dir():
    r"""`C:\Users\dev\actions-runner` is gone; defaulting to it fails silently.

    Keyed on the PARAM DEFAULT, not on a mention: both scripts legitimately
    discuss the old path in their docstrings while explaining why it moved, and a
    plain substring sweep would flag those explanations as the defect they
    describe (the same distinction ``test_the_script_exists_and_fails_loudly...``
    already has to make for SilentlyContinue).
    """
    tracked = subprocess.run(["git", "ls-files", "*.ps1"], cwd=str(_REPO),
                             capture_output=True, text=True).stdout.split()
    assert tracked, "no tracked .ps1 found -- the probe itself is broken"
    pat = re.compile(r"""^\s*\[string\]\s*\$\w+\s*=\s*['"]C:\\Users\\[^'"]*actions-runner""",
                     re.IGNORECASE)
    offenders = []
    for rel in tracked:
        for i, line in enumerate((_REPO / rel).read_text(
                encoding="utf-8", errors="replace").splitlines(), start=1):
            if pat.match(line):
                offenders.append("%s:%d: %s" % (rel, i, line.strip()))
    assert not offenders, (
        "these parameter defaults point at the runner directory retired on "
        "2026-10-02; a watchdog started with one cannot write its heartbeat or "
        "its log:\n  " + "\n  ".join(offenders))


# ---------------------------------------------------------------------------
# 4. Wiring and scope
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("relpath", [
    "tools/ops/runner_watchdog.ps1",
    "tools/gate/check_governance_liveness.py",
    "tools/gate/test_runner_watchdog_heartbeat.py",
    "tools/gate/test_check_governance_liveness.py",
    "tools/gate/fixtures/governance-runs-real-queued-cancelled-2026-08-06.json",
])
def test_every_file_this_phase_relies_on_is_actually_TRACKED_by_git(relpath):
    """`.gitignore` carries a blanket `tools/*` with explicit `!` re-includes.

    `tools/ops/` had no re-include, so both scripts this phase added were
    silently untracked: every test here passed on the author's disk and would
    have gone RED on a fresh CI checkout, against files that do not exist there.
    That is this programme's signature failure — a claim whose evidence could not
    establish it — so the fix is gated rather than remembered. `git ls-files`,
    not `Path.exists()`: existence on disk is exactly the thing that misled.
    """
    tracked = subprocess.run(
        ["git", "ls-files", "--error-unmatch", "--", relpath],
        capture_output=True, text=True, encoding="utf-8", cwd=str(_REPO)).returncode == 0
    if tracked:
        return
    # Not yet tracked is legitimate for brand-new work in the authoring tree, so
    # fall back to the property that actually decides whether it ever CAN be:
    # it must exist and must not be ignored. On a fresh CI checkout an ignored
    # or untracked file does not exist at all, so this branch is strict there.
    assert (_REPO / relpath).exists(), "%s does not exist" % relpath
    #
    # NOT `git check-ignore`: it exits 0 whenever the path matches ANY pattern,
    # INCLUDING a negating `!` re-include, so it reports `tools/gate/...` as a
    # match and would have failed every gate file here. Verified while writing
    # this test. `ls-files --others --ignored` lists only paths that are
    # genuinely excluded, and prints nothing otherwise.
    listed = subprocess.run(
        ["git", "ls-files", "--others", "--ignored", "--exclude-standard", "--", relpath],
        capture_output=True, text=True, encoding="utf-8", cwd=str(_REPO))
    assert listed.returncode == 0, listed.stderr
    assert not listed.stdout.strip(), (
        "%s is IGNORED by git, so it will never reach a fresh checkout and the "
        "CI lane that names it would go red on a missing file. Check the "
        "`tools/*` blanket ignore and its `!` re-includes in .gitignore."
        % relpath)


def test_this_selftest_is_named_in_the_fast_checks_gate_selftests_step():
    wf = (_REPO / ".github" / "workflows" / "fast-checks.yml").read_text(encoding="utf-8")
    assert "tools/gate/test_runner_watchdog_heartbeat.py" in wf, (
        "this self-test is not named in fast-checks.yml, so it does not run in "
        "CI (pytest.ini: testpaths = tests)")


def test_the_service_registration_helper_exists_and_installs_nothing_by_default():
    """Criterion 5: the helper is committed, but actually installing the service
    on the live host is the USER's action and is out of scope. Assert the script
    cannot install anything without an explicit opt-in flag."""
    helper = _REPO / "tools" / "ops" / "register_runner_service.ps1"
    assert helper.exists(), helper
    text = helper.read_text(encoding="utf-8")
    assert "-Apply" in text, "there is no explicit opt-in switch"
    assert "$Apply" in text
    # A plain invocation must be a report, not a change.
    p = _run(script=helper)
    out = p.stdout + p.stderr
    assert p.returncode == 0, out
    assert "DRY RUN" in out.upper(), out
    assert "no changes were made" in out.lower(), out


def test_no_join_path_on_a_param_defaulting_to_a_drive_qualified_path():
    r"""No tracked .ps1 may `Join-Path` a param whose default is drive-qualified.

    `Join-Path` RESOLVES its drive, so with a default like
    `C:\Users\...\actions-runner` it throws "Cannot find drive. A drive with the
    name 'C' does not exist." on a non-Windows pwsh. That is what happened: F44
    shipped `register_runner_service.ps1` with such a call, its dry run returned
    1 on the ubuntu-latest lane, and **Fast Checks was RED from 8c9e700 through
    937b7c6** while every local Windows run stayed green.

    This guard is keyed on the SHAPE, not on a variable name. The first version
    grepped `Join-Path $RunnerDir` in two files; a karen gate pointed out the
    same class was live 8 more times one directory over (`$VcpkgRoot` in
    scripts/security/run_deps.ps1, `$InstallDir` in
    scripts/vm-install-residue-harness.ps1), and that per the WIN-45 lesson a
    sweep keyed on phrasing always comes back. It now scans every tracked .ps1
    and derives the risky parameter names from each file's own param defaults.

    Static by necessity: on Windows the call SUCCEEDS, so no behavioural test
    here can catch it. `[IO.Path]::Combine` is pure string work, byte-identical
    on Windows for these paths, and is the portable form.
    """
    import re as _re
    import subprocess as _sp
    tracked = _sp.run(["git", "ls-files", "*.ps1"], cwd=str(_REPO),
                      capture_output=True, text=True).stdout.split()
    assert tracked, "no tracked .ps1 found -- the probe itself is broken"
    DEFAULT = _re.compile(r"""\$([A-Za-z_][A-Za-z0-9_]*)\s*=\s*['"][A-Za-z]:\\""")
    JOIN = _re.compile(r"Join-Path\s+\$([A-Za-z_][A-Za-z0-9_]*)")
    offenders = []
    scanned_with_defaults = 0
    for rel in tracked:
        text = (_REPO / rel).read_text(encoding="utf-8", errors="replace")
        risky = set(DEFAULT.findall(text))
        if not risky:
            continue
        scanned_with_defaults += 1
        for i, line in enumerate(text.splitlines(), start=1):
            m = JOIN.search(line)
            if m and m.group(1) in risky:
                offenders.append("%s:%d: %s" % (rel, i, line.strip()))
    assert scanned_with_defaults, (
        "no .ps1 with a drive-qualified param default was found -- the shape "
        "detector matched nothing, so this test would pass vacuously")
    assert not offenders, (
        "Join-Path on a param defaulting to a drive-qualified path breaks any "
        "non-Windows run; use [IO.Path]::Combine:\n  " + "\n  ".join(offenders))
