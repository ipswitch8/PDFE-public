#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regression tests for the watchdog's desktop-pause branch.

WHY THIS EXISTS
---------------
The pause was added because these runners need a live interactive desktop, the
operator reaches them over RDP, and a disconnect leaves the session alive with
no input desktop -- the runner then keeps taking jobs and the UIA lanes drive a
desktop that is not there.

It shipped verified only by a MANUAL one-off run with the probe forced negative.
An ad-hoc review pointed out, correctly, that the "16/16 watchdog gate tests"
cited alongside it are the pre-existing heartbeat self-test, which never touches
Test-InteractiveDesktop or Test-JobInFlight. So the branch that decides whether
to STOP A RUNNING LISTENER had no automated coverage at all. This is that
coverage.

HOW IT WORKS
------------
The watchdog is copied to a temp file with its desktop probe rewritten to a
constant, which is exactly what the manual run did by hand, and with every
machine-global lookup replaced by a double: all THREE
`Get-Process -Name 'Runner.Listener'` sites and the `Test-JobInFlight` probe
that guards the stop path. Nothing here touches the real runner, the real
watchdog, or any live path.

That second clause was FALSE until 2026-09-23, and the correction is kept
visible rather than quietly folded in. Only the desktop probe was doubled;
`Get-Process` is machine-global, so on a host where the self-hosted runner sat
IDLE -- listener up, no `Runner.Worker` -- the stop branch would execute for
real and `Stop-Process -Force` would kill the live listener. It never bit for
two reasons, both luck: this box normally has the runner down, and in CI a job
is by definition in flight, so the `Runner.Worker present` branch takes
precedence and returns before the kill.

A test file that claims it cannot touch a live path must be MADE unable to,
not merely observed not to have done so yet.

WHAT IT CANNOT DO, STATED SO NOBODY READS MORE INTO A GREEN RUN
---------------------------------------------------------------
These tests cover everything downstream of the probe: they do not exercise
`OpenInputDesktop` itself, because the probe is replaced by a double.

The premise the probe rests on -- that `OpenInputDesktop` really does fail on a
real RDP disconnect -- **has been measured**, so this section no longer needs to
disclaim it. GitHub #132 closed on 2026-09-04 with a real disconnect/reconnect
on a real host: `OpenInputDesktop` failed with error 5 one second after the WTS
state went Disconnected, stayed failing for eleven seconds AFTER the session
reported Active again, and was corroborated by TerminalServices events 24 and 25
from a log this project does not write.

(This paragraph previously said the premise was "NOT measured" and cited #132 as
outstanding. It had been stale since 2026-09-04 and was still stale when this
file was edited on 2026-09-12 -- in a change whose entire subject was stale
references to closed work. Found by auditing issue state against the tree
rather than by reading the file.)
"""
from __future__ import annotations

import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WATCHDOG = os.path.join(REPO, "tools", "ops", "runner_watchdog.ps1")

pytestmark = pytest.mark.skipif(
    os.name != "nt", reason="the watchdog is Windows-only; nothing to exercise elsewhere"
)


def _powershell() -> str:
    for exe in ("powershell.exe", "pwsh"):
        p = shutil.which(exe)
        if p:
            return p
    pytest.skip("no PowerShell on PATH")


#: How the listener lookup is doubled. See `_variant`.
#:
#: WHY THIS EXISTS AT ALL — two reasons, and the second is a live hazard.
#:
#: 1. REACHABILITY. The entire stop path lives inside `if ($listener)`. With no
#:    listener on the host, that branch never executes, so no mutation of the
#:    stop path can redden a test -- measured: reverting the re-query to the
#:    sampled-PID form left `test_never_stops_a_listener_that_does_not_exist`
#:    GREEN. A guard that cannot reach the code it names is decoration.
#:
#: 2. SAFETY. `Get-Process -Name 'Runner.Listener'` is MACHINE-GLOBAL and was
#:    not doubled. On a host where the self-hosted runner is idle -- listener
#:    up, no `Runner.Worker` -- the else branch runs for real and
#:    `Stop-Process -Force` KILLS THE LIVE LISTENER. It has never bitten
#:    because this box happens to have the runner down and because in CI a job
#:    is by definition in flight, so the `Runner.Worker present` branch takes
#:    precedence. Both are luck, not design. This file's header claimed
#:    "Nothing here touches the real runner ... or any live path"; that was
#:    false until this double existed.
#:
#: NONE means "no listener", which is the hermetic default every existing test
#: wants. ANOMALY reproduces GitHub #131: the SAMPLE returns a truthy object
#: for a process that does not exist, while the RE-QUERY -- the thing
#: `9812b7e2` added -- correctly finds nothing.
LISTENER_NONE = "none"
LISTENER_ANOMALY = "anomaly"

#: The PID the anomaly double reports. Arbitrary, but it must not be a real
#: process, and it is asserted on, so it is named rather than inlined.
ANOMALY_PID = 424242


def _double_external_probes(patched: str, listener: str) -> str:
    """Replace every machine-global lookup in the watchdog with a double.

    Applied by every variant builder, so no test in this file can reach the
    real process table -- see the LISTENER_* comment above for why that is a
    safety property and not only a determinism one.

    THE JOB-IN-FLIGHT PROBE IS DOUBLED HERE TOO, AND THE REASON IS A CI RED.
    An earlier version of this helper doubled only the three listener lookups
    and left `Test-JobInFlight` live. That function enumerates `Runner.Worker`,
    which is also machine-global, and the stop path is guarded by it:

        if (job in flight)      -> NOT stopping
        elseif ($NoRestart)     -> observing only
        else                    -> re-query + stop

    So on any host where a job IS running the first branch wins and the stop
    path is unreachable. That is fine on this box, where no runner runs -- and
    it is FALSE on `windows-latest`, where the very act of running these tests
    means a job is in flight. `test_the_requery_refuses_to_kill_a_listener_that_does_not_exist`
    passed locally and failed in CI for exactly that reason.

    This is the defect this file's own DCA (0089) is about, committed a second
    time inside the fix for it: one external call left undoubled makes a test's
    meaning conditional on the machine. The DCA claimed this particular call
    was "not currently reachable by any test" -- true when written, made false
    by the new test in the same commit.

    Default is "no job in flight", which is what every test here wants: it is
    the branch that leads to the code under test.
    """
    # The SAMPLE, at the top of the loop. count=1 and the `$listener =` anchor
    # matter: the re-query below is byte-identical apart from its assignment,
    # and doubling the wrong one would invert the whole point of the anomaly.
    sample = {
        LISTENER_NONE: "$listener = $null  # test double",
        LISTENER_ANOMALY:
            "$listener = [pscustomobject]@{ Id = %d }  # test double: #131"
            % ANOMALY_PID,
    }[listener]
    patched, n = re.subn(
        r"\$listener = Get-Process -Name 'Runner\.Listener'"
        r" -ErrorAction SilentlyContinue",
        lambda _m: sample,
        patched,
        count=1,
    )
    assert n == 1, (
        "could not inject the listener SAMPLE double -- the Get-Process line "
        "was renamed or reshaped. Refusing to run: without it this test both "
        "exercises nothing and may kill a live Runner.Listener."
    )

    # The RE-QUERY, immediately before the kill. Always empty: that is what
    # makes the anomaly an anomaly, and what keeps every variant off the real
    # process table.
    patched, n = re.subn(
        r"\$live = @\(Get-Process -Name 'Runner\.Listener'"
        r" -ErrorAction SilentlyContinue\)",
        lambda _m: "$live = @()  # test double",
        patched,
        count=1,
    )
    assert n == 1, (
        "could not inject the listener RE-QUERY double -- the stop path's "
        "Get-Process line was renamed or reshaped. Refusing to run: this is "
        "the call that stands between the test and Stop-Process -Force on a "
        "real runner."
    )

    # The RESTART VERIFICATION, which confirms the listener came back up.
    # Read-only, so not dangerous in the way the stop path is -- but it is
    # still a live process-table query, and leaving it undoubled would make
    # the harness's "touches no live path" claim false again by a third route.
    # This one was found by test_no_variant_can_reach_the_real_process_table,
    # not by hand: a per-site fix had already "finished" at two sites.
    patched, n = re.subn(
        r"\$now = Get-Process -Name 'Runner\.Listener'"
        r" -ErrorAction SilentlyContinue",
        lambda _m: "$now = $null  # test double",
        patched,
        count=1,
    )
    assert n == 1, (
        "could not inject the restart-verification double -- the Get-Process "
        "line in the restart path was renamed or reshaped."
    )

    # The JOB-IN-FLIGHT probe. Machine-global (it enumerates Runner.Worker) and
    # it GUARDS the stop path, so leaving it live makes the stop path reachable
    # only on a host with no running job. See this function's docstring.
    patched, n = re.subn(
        r"(function Test-JobInFlight \{)",
        r"\1\n    return $false  # test double",
        patched,
        count=1,
    )
    assert n == 1, (
        "could not inject the job-in-flight double -- Test-JobInFlight was "
        "renamed or removed. Refusing to run: without it the stop path is "
        "unreachable on any host that has a job running, which is every CI "
        "runner executing this very test."
    )
    return patched


def _variant(tmpdir: str, desktop_present: bool,
             listener: str = LISTENER_NONE) -> str:
    """A copy of the watchdog whose desktop probe and listener lookup are doubled."""
    src = io.open(WATCHDOG, encoding="utf-8-sig").read()
    literal = "$true" if desktop_present else "$false"
    patched, n = re.subn(
        r"(function Test-InteractiveDesktop \{)",
        r"\1\n    return %s  # test double" % literal,
        src,
        count=1,
    )
    assert n == 1, (
        "could not inject the desktop probe double -- Test-InteractiveDesktop "
        "was renamed or removed, and this test would otherwise pass while "
        "exercising nothing"
    )

    patched = _double_external_probes(patched, listener)
    dest = os.path.join(tmpdir, "watchdog_variant.ps1")
    io.open(dest, "w", encoding="utf-8-sig", newline="").write(patched)
    return dest


def _run(script: str, runner_dir: str, extra=(), iterations: int = 1) -> str:
    """Run the watchdog variant, with the outage journal ISOLATED.

    GitHub #178 -- this used to inherit the ambient environment, so
    `Get-OutageLogPath` fell through to `%ProgramData%\\PDFE\\desktop-outages.jsonl`
    and every pause test wrote a REAL `outage-start` into the machine-global
    journal. The probe is pinned `$false` in those tests, so the outage never
    closed either.

    That is not cosmetic. `tests/helpers/desktop_outage.py` reads that journal to
    decide whether a UI failure happened during an outage, and reclassifies any
    overlapping failure from `red` to `blocked`. A permanently-open fabricated
    outage therefore DOWNGRADES EVERY SUBSEQUENT REAL UI FAILURE on the machine
    to "could not be driven" -- a false-negative generator, produced by the test
    suite itself. Measured on this machine before the fix: 232 journal lines,
    the tail of them unpaired `outage-start` records from repeated test runs.

    Isolating per-run costs one environment variable.
    """
    os.makedirs(runner_dir, exist_ok=True)
    cmd = [_powershell(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
           "-RunnerDir", runner_dir, "-MaxIterations", str(iterations),
           "-IntervalSeconds", "1"]
    cmd.extend(extra)
    env = dict(os.environ)
    env["PDFE_DESKTOP_OUTAGE_LOG"] = os.path.join(runner_dir, "outages-isolated.jsonl")
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=180, env=env)
    return p.stdout + p.stderr


#: Calls to Test-InteractiveDesktop per loop iteration. `_variant_sequence` takes
#: PER-ITERATION values and expands each to this many calls.
#:
#: WAS 2, NOW 1 (2026-10-03). The watchdog used to probe twice per iteration --
#: once for the heartbeat record, once for the pause decision -- and this constant
#: existed to absorb that. The two samples could disagree across the milliseconds
#: between them, which meant the heartbeat could report `up` in the same iteration
#: that the gate paused the runner: an operator reading the heartbeat would be
#: told the opposite of what the watchdog had just done. The pause decision now
#: reuses the sample the heartbeat reported, so there is one probe and one answer.
#:
#: The constant stays, and so does the test pinning it, because the ORIGINAL
#: hazard is unchanged: the sequenced double feeds values per ITERATION, so if the
#: call count drifts from this number the values desynchronise and the #169 outage
#: opens or closes on the wrong pass -- passing or failing for the wrong reason.
#: The first version of that double fed one value per CALL, so [gone, back] gave
#: "gone" to the heartbeat and "back" to the decision inside a single iteration,
#: the outage never opened, the journal stayed empty, and the failure looked like
#: a product defect.
CALLS_PER_ITERATION = 1


def _variant_sequence(tmpdir: str, per_iteration) -> str:
    """A watchdog copy whose desktop probe answers differently PER ITERATION.

    `_variant` above pins the probe to a constant, which is right for the pause
    tests but cannot express the thing GitHub #169 is about: an outage that
    OPENS and then CLOSES. That needs the probe to say "gone" and later "back".

    `per_iteration` is one boolean per loop iteration; each is repeated
    CALLS_PER_ITERATION times. The script-scoped index holds the last value once
    it runs off the end, so a run with more iterations than values simply stays
    in the final state.
    """
    src = io.open(WATCHDOG, encoding="utf-8-sig").read()
    values = []
    for v in per_iteration:
        values.extend([v] * CALLS_PER_ITERATION)
    literals = ", ".join("$true" if v else "$false" for v in values)
    injected = "\n".join([
        "",
        # Test-Path variable:, NOT `$null -eq $script:...`. The watchdog runs
        # under StrictMode, where READING an unset variable throws -- the first
        # version of this double did exactly that and the probe never ran.
        "    if (-not (Test-Path variable:script:__probeIdx)) {",
        "        $script:__probeSeq = @(%s)" % literals,
        "        $script:__probeIdx = 0",
        "    }",
        "    $i = [Math]::Min($script:__probeIdx, $script:__probeSeq.Count - 1)",
        "    $script:__probeIdx = $script:__probeIdx + 1",
        "    return $script:__probeSeq[$i]  # test double (sequence)",
    ])
    patched, n = re.subn(
        r"(function Test-InteractiveDesktop \{)",
        lambda m: m.group(1) + injected,
        src,
        count=1,
    )
    assert n == 1, (
        "could not inject the sequenced desktop probe -- Test-InteractiveDesktop "
        "was renamed or removed, and this test would otherwise pass while "
        "exercising nothing"
    )
    # Same listener doubling as _variant: the sequenced variant runs the same
    # loop and would otherwise reach the real process table too.
    patched = _double_external_probes(patched, LISTENER_NONE)
    dest = os.path.join(tmpdir, "watchdog_sequence.ps1")
    io.open(dest, "w", encoding="utf-8-sig", newline="").write(patched)
    return dest


def _run_with_journal(script: str, runner_dir: str, journal: str,
                      extra=(), iterations: int = 1) -> str:
    os.makedirs(runner_dir, exist_ok=True)
    cmd = [_powershell(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
           "-RunnerDir", runner_dir, "-MaxIterations", str(iterations),
           "-IntervalSeconds", "1"]
    cmd.extend(extra)
    env = dict(os.environ)
    env["PDFE_DESKTOP_OUTAGE_LOG"] = journal
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=180, env=env)
    return p.stdout + p.stderr


def _journal_events(path: str):
    # utf-8-SIG, matching tests/helpers/desktop_outage.py:101. PowerShell's
    # `Add-Content -Encoding UTF8` writes a BOM on Windows PowerShell 5.1, and a
    # plain utf-8 read fails with "Unexpected UTF-8 BOM" on the very first line.
    # The product reader already gets this right; this helper did not, and the
    # resulting JSONDecodeError read at first glance like the watchdog had
    # written a malformed journal.
    if not os.path.exists(path):
        return []
    out = []
    with io.open(path, encoding="utf-8-sig") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            out.append(json.loads(line)["event"])
    return out


def test_pauses_when_there_is_no_desktop():
    with tempfile.TemporaryDirectory() as d:
        out = _run(_variant(d, False), os.path.join(d, "runner"))
    assert "NO INTERACTIVE DESKTOP" in out, out
    assert "PAUSING" in out, out


def test_does_not_restart_the_listener_while_paused():
    """The whole point: restarting into a desktop-less session is the thing being
    prevented. If this regresses, the runner comes back online and takes jobs it
    cannot serve."""
    # TWO iterations, deliberately. With MaxIterations=1 the pause branch hits
    # its own break BEFORE the `continue`, so the fall-through path is never
    # reached and this test passes without exercising anything -- measured: a
    # mutant that deleted the `continue` survived the one-iteration form.
    with tempfile.TemporaryDirectory() as d:
        out = _run(_variant(d, False), os.path.join(d, "runner"), iterations=2)
    assert "launching detached run.cmd" not in out, out


def test_normal_path_is_untouched_when_a_desktop_is_present():
    """The false-positive direction. A pause that fires with a desktop present
    would take a healthy runner offline, which is worse than not having the
    feature."""
    with tempfile.TemporaryDirectory() as d:
        out = _run(_variant(d, True), os.path.join(d, "runner"), extra=("-NoRestart",))
    assert "NO INTERACTIVE DESKTOP" not in out, out
    assert "PAUSING" not in out, out
    # Assert the loop RAN TO COMPLETION down the normal path, not that some
    # particular line was logged. The first version asserted "Runner.Listener
    # DOWN", which passed locally and failed on windows-latest: what the
    # watchdog reports about the listener depends on whether the host has a
    # runner installed at all, and that is incidental to the property under
    # test. Reaching MaxIterations is the durable signal that it neither paused
    # nor exited early.
    assert "reached MaxIterations" in out, out


def test_ignore_switch_disables_the_pause_entirely():
    """-IgnoreDesktopState is the escape hatch for runners serving only non-UI
    lanes. If it stopped working, those runners would idle for no reason."""
    with tempfile.TemporaryDirectory() as d:
        out = _run(_variant(d, False), os.path.join(d, "runner"),
                   extra=("-IgnoreDesktopState", "-NoRestart"))
    assert "PAUSING" not in out, out


def test_never_stops_a_listener_that_does_not_exist():
    """No listener present -> the watchdog must not claim to stop one.

    WHAT THIS DOES NOT DO, corrected 2026-09-23. It used to say it "pins" the
    #131 fix. It does not and could not: the whole stop path sits inside
    `if ($listener)`, this fixture has no listener, so the branch never
    executes -- in the fixed code and in any mutation of it. Reverting the
    re-query to the sampled-PID form leaves this test GREEN. There was not even
    a pre-fix version to catch: `9812b7e2`, which introduced the desktop-pause
    branch, already re-queried.

    The real pin is
    `test_the_requery_refuses_to_kill_a_listener_that_does_not_exist`, which
    drives a listener double into the stop path so the branch can be reached at
    all.

    KEPT ON PURPOSE. This still guards a narrower, real property -- that the
    ordinary no-listener path stays silent about stopping anything -- and it
    would redden if a stop-log were ever emitted OUTSIDE the `if ($listener)`
    guard. Two narrow tests, not one redundant one.
    """
    with tempfile.TemporaryDirectory() as d:
        out = _run(_variant(d, False), os.path.join(d, "runner"))
    assert "stopping listener" not in out, out


def test_the_requery_refuses_to_kill_a_listener_that_does_not_exist():
    """GitHub #131, reproduced deterministically instead of waited for.

    THE OBSERVATION. One run logged `stopping listener (pid 9064)` on a host
    with zero Runner.Listener processes. `$listener` was truthy and `$lpid` was
    9064 for a process that did not exist. Three further runs behaved
    correctly and it has never reproduced since.

    WHY THE EXISTING TEST DOES NOT PIN THIS, measured rather than argued.
    `test_never_stops_a_listener_that_does_not_exist` asserts only that
    "stopping listener" is absent on a host with no runner. But the whole stop
    path sits inside `if ($listener)`, so with no listener it never executes --
    in the fixed code AND in any mutation of it. Reverting the re-query to the
    pre-`9812b7e2` sampled-PID form leaves that test GREEN. It guards a branch
    it cannot reach.

    WHAT THIS ONE DOES. LISTENER_ANOMALY makes the SAMPLE return a truthy
    object for a process that does not exist, exactly as observed, while the
    RE-QUERY finds nothing. That drives execution INTO the stop path with the
    contradiction present, which is the only way to exercise the fix.

    The assertions are positive-then-negative on purpose. Asserting only the
    absence of "stopping listener" would pass on a run that never reached the
    branch -- the same vacuity as the test above. `PAUSING` proves the gate
    fired; `listener already gone` proves the re-query ran and decided; and
    only then does the absence of a kill mean anything.
    """
    with tempfile.TemporaryDirectory() as d:
        out = _run(_variant(d, False, listener=LISTENER_ANOMALY),
                   os.path.join(d, "runner"))

    assert "PAUSING" in out, (
        "the desktop gate did not fire, so the stop path was never reached "
        "and the assertions below would be vacuous:\n%s" % out)
    assert "listener already gone -> nothing to stop" in out, (
        "the re-query did not run or did not find the listener absent -- this "
        "is the branch 9812b7e2 added, and it is what makes killing an "
        "unrelated process impossible by construction:\n%s" % out)
    assert "stopping listener" not in out, (
        "the watchdog tried to stop a listener for a process that does not "
        "exist (GitHub #131). The sampled PID was trusted instead of "
        "re-queried:\n%s" % out)

    # THE RECURRENCE CAPTURE. #131's investigation record asks a future
    # reproducer for four values in the same instant -- the sampled PID, the
    # sample's runtime type, its count, and the re-query result -- because the
    # original observation's evidence went to a console that no longer exists.
    # "Capture, do not reconstruct." Assert all four reach the log, so the
    # capture cannot rot into a message that says less than it claims.
    assert "requery=0" in out, (
        "the sample/re-query disagreement was not captured, so a recurrence "
        "would again leave nothing to diagnose:\n%s" % out)
    assert str(ANOMALY_PID) in out.split("PAUSING", 1)[-1], (
        "the capture did not record WHICH pid the sample claimed:\n%s" % out)
    assert "count=1" in out, (
        "the capture did not record the sample's count -- one of the four "
        "values the investigation record asks for:\n%s" % out)
    assert "type=" in out and "<null>" not in out.split("requery=0")[0][-120:], (
        "the capture did not record the sample's runtime type, which is the "
        "value that would settle the partially-populated-object "
        "hypothesis:\n%s" % out)


def test_no_variant_can_reach_the_real_process_table():
    """CLASS-LEVEL. Every variant builder must double BOTH Get-Process sites.

    The instance above pins one behaviour. This pins the PROPERTY that made the
    instance possible, for every variant builder in this file including ones
    added later.

    Why it is needed. `Get-Process -Name 'Runner.Listener'` is machine-global.
    Only the desktop probe was ever doubled, so on a host where the self-hosted
    runner sat IDLE -- listener up, no `Runner.Worker` -- the stop branch would
    execute for real and `Stop-Process -Force` would kill the live listener.
    The file header asserted this could not happen. It could; it simply had not.

    A per-test assertion cannot close that, because the hazard belongs to the
    BUILDER, not to any one test: a new test calling a new builder would
    reintroduce it silently. So the oracle is the generated script itself --
    if the literal survives anywhere in it, something can still reach the real
    process table.
    """
    seen = 0
    with tempfile.TemporaryDirectory() as d:
        builders = [
            ("variant/none", lambda t: _variant(t, False)),
            ("variant/present", lambda t: _variant(t, True)),
            ("variant/anomaly",
             lambda t: _variant(t, False, listener=LISTENER_ANOMALY)),
            ("variant_sequence", lambda t: _variant_sequence(t, [True, False])),
        ]
        for label, build in builders:
            sub = os.path.join(d, label.replace("/", "_"))
            os.makedirs(sub, exist_ok=True)
            text = io.open(build(sub), encoding="utf-8-sig").read()
            seen += 1
            assert "Get-Process -Name 'Runner.Listener'" not in text, (
                "%s still contains a LIVE listener lookup. A test built from "
                "it can query the real process table and, with an idle "
                "self-hosted runner, Stop-Process -Force the real listener "
                "(GitHub #131 follow-up)." % label)

            # The job-in-flight probe GUARDS the stop path, so leaving it live
            # makes the stop path reachable only on a host with no running
            # job. Asserted HERE, in the class-level test, because the failure
            # it prevents is invisible on this box: no runner runs locally, so
            # a live probe returns false and everything passes. It reddened
            # only on windows-latest, where running the test IS a job in
            # flight. This assertion makes that CI-only failure a locally
            # checkable property of the generated script.
            assert "function Test-JobInFlight {\n    return $false" in text, (
                "%s does not double Test-JobInFlight. The stop path is behind "
                "it, so on any host with a job running the tests below it "
                "exercise nothing -- and pass, which is worse." % label)

    # Non-vacuity: if the builder list were emptied or the loop skipped, the
    # assertion above would pass by never running -- the exact shape this
    # file's own history warns about.
    assert seen == 4, (
        "expected to inspect 4 generated variants, inspected %d -- the sweep "
        "is not covering the builders it claims to." % seen)


def test_a_restored_desktop_closes_the_open_outage():
    """GitHub #169 -- the journal's CLOSE event, asserted against the WRITER.

    The mechanism already existed (runner_watchdog.ps1 writes 'outage-end' when
    $desktopWasMissing and the desktop is back). What did not exist was anything
    proving the watchdog emits it. Every 'outage-end' elsewhere in the test tree
    is a HAND-WRITTEN fixture record fed to the reader in
    tests/helpers/desktop_outage.py -- so the reader was well covered and the
    producer was not covered at all. A journal that never closes an outage would
    have left every one of those reader tests green while
    `read_outages()` saw a permanently-open window in the field.

    The probe says "gone" on the first iteration and "back" afterwards, so a
    real outage opens and then closes through the script's own code path.
    """
    with tempfile.TemporaryDirectory() as d:
        journal = os.path.join(d, "outages.jsonl")
        out = _run_with_journal(
            _variant_sequence(d, [False, True, True]),
            os.path.join(d, "runner"),
            journal,
            extra=("-NoRestart",),
            iterations=3,
        )
        events = _journal_events(journal)
        # Read through the PRODUCT reader here, INSIDE the TemporaryDirectory
        # block. The first version did this after the block had exited, so the
        # journal was already deleted and read_outages() correctly returned []
        # -- which looked exactly like a reader defect and was not one.
        sys.path.insert(0, os.path.join(REPO, "tests"))
        try:
            from helpers import desktop_outage  # noqa: E402
            windows = desktop_outage.read_outages(journal)
        finally:
            sys.path.pop(0)

    # Non-vacuity first: an empty journal would satisfy "no unclosed outage"
    # trivially, and an empty-collection oracle is this project's most-repeated
    # false green.
    assert events, (
        "the outage journal is empty -- nothing was recorded, so this test "
        "proves nothing about closing.\n" + out
    )
    assert "outage-start" in events, (
        "no outage-start was recorded, so there was no open outage to close "
        "and the close assertion below would be meaningless.\n%r\n%s"
        % (events, out)
    )
    assert "outage-end" in events, (
        "an outage was opened and the desktop came back, but no outage-end was "
        "recorded. read_outages() will report a permanently-open window and "
        "every subsequent failure gets reclassified as blocked.\n%r\n%s"
        % (events, out)
    )
    # Order matters: an 'outage-end' before its 'outage-start' would close
    # nothing, and the reader pairs them positionally.
    assert events.index("outage-start") < events.index("outage-end"), (
        "outage-end precedes outage-start in the journal: %r\n%s" % (events, out)
    )
    assert "interactive desktop RESTORED" in out, out

    # The PRODUCT reader's view, not only the raw event list. The events being
    # present is necessary but not sufficient: read_outages() PAIRS them, and a
    # window it reports as still-open is what actually reclassifies later
    # failures as `blocked`. This is the assertion that connects the writer to
    # the consumer that cares.
    assert windows, "read_outages() found no window in a journal that has one"
    assert all(end is not None for _start, end in windows), (
        "read_outages() reports an OPEN window after the desktop came back: %r"
        % (windows,)
    )


def test_a_desktop_that_never_drops_writes_no_outage_at_all():
    """The control for the test above, and it is not optional.

    Without it, a watchdog that wrote 'outage-start'+'outage-end' on EVERY
    iteration regardless of desktop state would satisfy every assertion above.
    That failure mode is not hypothetical -- it is what a misplaced call outside
    the `if ($desktopWasMissing)` guard produces.
    """
    with tempfile.TemporaryDirectory() as d:
        journal = os.path.join(d, "outages.jsonl")
        out = _run_with_journal(
            _variant(d, True),
            os.path.join(d, "runner"),
            journal,
            extra=("-NoRestart",),
            iterations=2,
        )
        events = _journal_events(journal)

    assert events == [], (
        "the watchdog wrote outage events while the desktop was present "
        "throughout: %r\n%s" % (events, out)
    )
    assert "reached MaxIterations" in out, out


def test_the_probe_is_called_exactly_CALLS_PER_ITERATION_times():
    """Pins the assumption CALLS_PER_ITERATION encodes.

    If a call site is added or removed, the sequenced double silently
    desynchronises: values meant for iteration N get consumed by iteration N-1,
    the outage opens or closes on the wrong pass, and the #169 test fails or
    passes for the wrong reason. A source count is cheap and makes that a loud
    failure instead -- and it did exactly that when the second probe was removed
    on 2026-10-03, which is the behaviour wanted from a pin.

    One call is also the correct number on its own merits: see the note on
    CALLS_PER_ITERATION for why two samples per iteration could make the
    heartbeat contradict the pause decision it was recorded alongside.
    """
    src = io.open(WATCHDOG, encoding="utf-8-sig").read()
    body = src[src.index("# --- main loop"):] if "# --- main loop" in src else src
    calls = [ln for ln in body.split(chr(10))
             if "Test-InteractiveDesktop" in ln and "function " not in ln]
    assert len(calls) == CALLS_PER_ITERATION, (
        "expected %d Test-InteractiveDesktop call sites, found %d:%s%s%s"
        "CALLS_PER_ITERATION in this file must be updated together with them."
        % (CALLS_PER_ITERATION, len(calls), chr(10), chr(10).join(calls), chr(10))
    )


def test_no_test_here_writes_to_the_machine_global_outage_journal():
    """GitHub #178 -- the leak that made the suite a false-negative generator.

    Every helper that launches the watchdog MUST set PDFE_DESKTOP_OUTAGE_LOG.
    Without it Get-OutageLogPath falls through to the ProgramData journal and the
    pause tests -- whose probe is pinned $false -- append an outage-start that
    never closes. desktop_outage.read_outages() then sees a permanently-open
    window and reclassifies every later real UI failure on this machine from
    `red` to `blocked`.

    A source check rather than a behavioural one, deliberately: the behavioural
    version would have to write to the real journal to prove it does not.
    """
    src = io.open(__file__, encoding="utf-8").read()
    launchers = [name for name in ("_run", "_run_with_journal")]
    for name in launchers:
        start = src.index("def %s(" % name)
        body = src[start:]
        # Bound at the next top-level def so one helper's env= cannot satisfy
        # the assertion for another.
        nxt = body.find(chr(10) + "def ", 1)
        if nxt != -1:
            body = body[:nxt]
        assert "PDFE_DESKTOP_OUTAGE_LOG" in body, (
            "%s() launches the watchdog without isolating the outage journal. "
            "It will write into the machine-global ProgramData journal and "
            "poison every subsequent UI run on this machine." % name
        )
        assert "env=env" in body, (
            "%s() builds an env but does not pass it to subprocess.run" % name
        )


def test_job_in_flight_probe_fails_safe_by_source_inspection():
    """Test-JobInFlight must return TRUE when it cannot tell, because $false
    falls through to the branch that kills the listener. The first version
    returned a bare -gt 0 with no catch, and its own comment claimed the
    opposite of what it did."""
    src = io.open(WATCHDOG, encoding="utf-8-sig").read()
    body = src[src.index("function Test-JobInFlight"):]
    body = body[:body.index("\n}\n")]
    assert "catch" in body, "Test-JobInFlight has no catch: an enumeration failure returns false and kills a live job"
    tail = body[body.index("catch"):]
    assert "return $true" in tail, "the catch must return $true (assume a job IS running)"
    assert "ParentProcessId" in body, (
        "the name-independent child-process signal is gone; a renamed worker "
        "would again be a single point of failure on a Stop-Process path")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
