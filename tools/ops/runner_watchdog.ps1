<#
.SYNOPSIS
    Keeps the pdfe-runner-01 self-hosted GitHub Actions runner alive, and — unlike
    its predecessor — proves at every moment that it is itself alive.

.DESCRIPTION
    Meta-audit follow-up F44, deferred-backlog row F46.

    WHAT WAS ACTUALLY WRONG WITH THE OLD WATCHDOG
    =============================================
    The previous script lives at C:\Users\dev\actions-runner\watchdog.ps1 and
    is NOT tracked in this repository. Read before replacing (criterion 4), and
    the finding is not the obvious one:

      * The restart LOGIC is sound. It polls Get-Process Runner.Listener every
        60 s and relaunches run.cmd by FULL PATH — the full path being itself a
        prior fix, because NoDefaultCurrentDirectoryInExePath=1 made the bare
        name fail and stranded the runner 2026-06-22..25. Its log shows that
        logic working: 2026-06-25 several restarts, 2026-07-03T12:52:02
        "Runner.Listener DOWN -> launching detached run.cmd" followed 20 s later
        by "Runner.Listener back UP (pid 9616)".

      * The DEFECT is that it emits output ONLY on a state change: at startup,
        and when the listener is found down. During healthy operation it writes
        NOTHING. So "watchdog running, runner healthy, nothing to report" and
        "watchdog process is gone" produce the SAME observable — an unchanging
        log. F46's evidence is exactly that ambiguity: watchdog.log's last entry
        is 2026-07-03, and there is no way to tell from the log alone whether
        the watchdog was alive and quiet for three weeks or dead the whole time.

      * The ambiguity was resolved only by a second fact: the runner's own _diag
        shows the listener stopped around 2026-07-25, and NO restart attempt was
        logged. The watchdog was therefore already dead when the thing it
        watches died. A monitor whose liveness must be inferred from the failure
        of the thing it monitors is not a monitor.

      * Second contributing defect: $ErrorActionPreference = 'SilentlyContinue'
        at the top of a script whose only output is its log. Any error in
        Add-Content, Start-Process or the loop body is discarded silently.

      * Third, and the reason a "fix the script" answer is incomplete: NOTHING
        INVOKES IT. There is no scheduled task, no service, no Run key — it was
        started by hand, so it does not survive a reboot or a logout any more
        than the runner does. Repairing the heartbeat is necessary and not
        sufficient; see tools/ops/register_runner_service.ps1 and
        docs/audit/meta-audit-2026-07/followup/phase-44/runner-availability-recommendation.md.

    WHAT THIS SCRIPT CHANGES
    ========================
      1. A HEARTBEAT IS WRITTEN ON EVERY ITERATION, healthy or not, to a JSON
         file (default: <RunnerDir>\watchdog-heartbeat.json) carrying an ISO-8601
         UTC timestamp, the watchdog pid, the iteration count, the observed
         listener state and the interval. "Quiet" now means "the file's
         timestamp is advancing"; a stale heartbeat is unambiguously a dead
         watchdog. The off-host CI liveness alarm
         (.github/workflows/ci-liveness.yml, F44) covers the case where the
         whole HOST is gone and no local file can be written at all.
      2. Errors are no longer swallowed: $ErrorActionPreference = 'Stop' with
         explicit try/catch around the things that are legitimately allowed to
         fail (process lookup, process launch), each of which LOGS.
      3. -SelfTest runs the heartbeat mechanism end to end in a temporary
         directory — write, re-read, validate every field, advance, re-read
         again — touching no runner and no live path, and exits non-zero if any
         step fails. tools/gate/test_runner_watchdog_heartbeat.py invokes this
         so the mechanism is proven from the working tree rather than asserted.

.PARAMETER RunnerDir
    The actions-runner directory. Default C:\actions-runner.

    THE DEFAULT MOVED, AND THE OLD ONE FAILED QUIETLY (2026-10-03).
    It used to be C:\Users\dev\actions-runner. The runner was relocated to
    C:\actions-runner on 2026-10-02 to stop build output embedding the account
    name (the shipped-leak fix), and this default was not moved with it. A real
    `-NoRestart -MaxIterations 3` run on the live host therefore produced:

        ERROR: heartbeat write failed: Access to the path 'C:\Users\dev' is denied.
        WARN:  cannot append to log C:\Users\dev\actions-runner\watchdog.log: ...

    three times -- and then **exited 0**. Nothing was written, nothing durable
    recorded the failure, and the exit code said healthy. That is F46's defect
    (quiet and dead are the same observable) reintroduced through a different
    door, which is why there are now TWO guards and not just a corrected string:
      * a missing $RunnerDir is refused at startup rather than warned about, and
      * the process exits NON-ZERO if the heartbeat did not land (see below).

.PARAMETER HeartbeatPath
    Override the heartbeat file location. Default <RunnerDir>\watchdog-heartbeat.json.

.PARAMETER LogPath
    Override the log location. Default <RunnerDir>\watchdog.log.

.PARAMETER IntervalSeconds
    Poll/heartbeat period. Default 60.

.PARAMETER MaxIterations
    Stop after N iterations instead of running forever. 0 = forever (default).
    Used by the tests; also useful for a scheduled-task-per-run model.

.PARAMETER NoRestart
    Observe and heartbeat, never launch run.cmd. For dry runs on a host whose
    runner is mid-job — the runner must never be disturbed while it is working.

.PARAMETER SelfTest
    Exercise the heartbeat mechanism in a temp directory and exit. Touches no
    runner, no default paths, and no network. Exit 0 pass, 1 fail.

.EXAMPLE
    pwsh -NoProfile -File tools/ops/runner_watchdog.ps1 -SelfTest

.EXAMPLE
    pwsh -NoProfile -File tools/ops/runner_watchdog.ps1 -NoRestart -MaxIterations 3
#>
[CmdletBinding()]
param(
    [string] $RunnerDir       = 'C:\actions-runner',
    [string] $HeartbeatPath   = '',
    [string] $LogPath         = '',
    [int]    $IntervalSeconds = 60,
    [int]    $MaxIterations   = 0,
    [switch] $NoRestart,
    # Keep the pre-2026-09 behaviour: never pause on desktop state. Use it if
    # this runner serves only non-UI lanes, where a missing desktop is
    # harmless and pausing would be pure lost capacity.
    [switch] $IgnoreDesktopState,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
# NOT SilentlyContinue. The predecessor discarded every error in the only code
# path that produces its output.
$ErrorActionPreference = 'Stop'

$script:LogFile = ''

function Say {
    <#
      [Console]::Out, NOT the success stream. A PowerShell function returns
      EVERYTHING it writes to the success stream, so an ordinary emit inside
      Invoke-SelfTest would be swallowed into that function's return value and
      never reach the console -- observed exactly once while building this
      script, which is why the note is here.
    #>
    param([string] $Message = '')
    [Console]::Out.WriteLine($Message)
}

function Get-UtcStamp {
    return (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
}

function Write-WatchdogLog {
    param([Parameter(Mandatory)][string] $Message)
    $line = '{0}  {1}' -f (Get-UtcStamp), $Message
    Say $line
    if ($script:LogFile) {
        try {
            Add-Content -Path $script:LogFile -Value $line -Encoding utf8
        } catch {
            # The log is best-effort; the heartbeat is the contract. Surface the
            # problem on stdout rather than dropping it the way the old script did.
            Say ('{0}  WARN: cannot append to log {1}: {2}' -f `
                (Get-UtcStamp), $script:LogFile, $_.Exception.Message)
        }
    }
}

function Write-Heartbeat {
    <#
      Writes the heartbeat atomically-ish (temp file + move) so a reader can
      never observe a half-written document, and returns the object written.
    #>
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][int]    $Iteration,
        [Parameter(Mandatory)][string] $ListenerState,
        [int] $ListenerPid = 0,
        [int] $Interval = 60
    )
    $beat = [ordered]@{
        schema           = 'pdfe.runner-watchdog.heartbeat/1'
        timestampUtc     = (Get-UtcStamp)
        watchdogPid      = $PID
        iteration        = $Iteration
        listenerState    = $ListenerState
        listenerPid      = $ListenerPid
        intervalSeconds  = $Interval
        machine          = $env:COMPUTERNAME
    }
    $json = ($beat | ConvertTo-Json -Depth 4)
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $tmp = "$Path.tmp"
    Set-Content -LiteralPath $tmp -Value $json -Encoding utf8
    Move-Item -LiteralPath $tmp -Destination $Path -Force
    return $beat
}

function ConvertTo-BeatTime {
    <#
      Normalise a heartbeat timestamp to a UTC [datetime].

      TRAP, found by -SelfTest rather than by reading the code:
      ConvertFrom-Json silently converts an ISO-8601 STRING into a [datetime]
      object, so $beat.timestampUtc is a string on the way out and a DateTime on
      the way back in. A naive string comparison of written-vs-read therefore
      fails, and -- worse -- [datetime]::TryParse against the round-tripped
      object only appears to work because PowerShell coerces the DateTime back
      to a CULTURE-FORMATTED string first, discarding the UTC marker. Both
      producers are funnelled through here so neither can drift.
    #>
    param([Parameter(Mandatory)] $Value)
    if ($Value -is [datetime]) { return ([datetime]$Value).ToUniversalTime() }
    $parsed = [datetime]::MinValue
    $styles = [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor `
              [System.Globalization.DateTimeStyles]::AssumeUniversal
    if ([datetime]::TryParse([string]$Value, [cultureinfo]::InvariantCulture, $styles, [ref]$parsed)) {
        return $parsed
    }
    throw "not a parseable timestamp: $Value"
}

function Read-Heartbeat {
    param([Parameter(Mandatory)][string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "heartbeat file does not exist: $Path"
    }
    $raw = Get-Content -LiteralPath $Path -Raw -Encoding utf8
    if ([string]::IsNullOrWhiteSpace($raw)) { throw "heartbeat file is empty: $Path" }
    return ($raw | ConvertFrom-Json)
}

function Invoke-SelfTest {
    <#
      Proves the heartbeat mechanism itself, in a temp directory, with no
      runner involvement. Criterion 4: the thing that distinguishes this from
      the predecessor is that "the heartbeat works" is CHECKED, not claimed.
    #>
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("pdfe-watchdog-selftest-" + [guid]::NewGuid().ToString('N'))
    $hb   = Join-Path $root 'watchdog-heartbeat.json'
    $log  = Join-Path $root 'watchdog.log'
    $script:LogFile = $log
    $failures = New-Object System.Collections.Generic.List[string]

    function _check([string] $what, [bool] $ok, [string] $detail = '') {
        if ($ok) { Say ("  PASS  {0}" -f $what) }
        else {
            Say ("  FAIL  {0} {1}" -f $what, $detail)
            $failures.Add($what)
        }
    }

    try {
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        Say "[watchdog-selftest] scratch: $root"

        _check 'heartbeat absent before first write' (-not (Test-Path -LiteralPath $hb))

        $first = Write-Heartbeat -Path $hb -Iteration 1 -ListenerState 'selftest' -ListenerPid 0 -Interval 60
        _check 'heartbeat file created' (Test-Path -LiteralPath $hb)
        _check 'no .tmp left behind' (-not (Test-Path -LiteralPath "$hb.tmp"))

        $read = Read-Heartbeat -Path $hb
        _check 'heartbeat re-reads as JSON' ($null -ne $read)
        foreach ($f in @('schema','timestampUtc','watchdogPid','iteration','listenerState','intervalSeconds')) {
            _check ("field present: $f") ($null -ne $read.$f)
        }
        _check 'schema matches' ($read.schema -eq 'pdfe.runner-watchdog.heartbeat/1') "got '$($read.schema)'"
        _check 'pid round-trips' ([int]$read.watchdogPid -eq $PID) "got $($read.watchdogPid), expected $PID"
        _check 'iteration round-trips' ([int]$read.iteration -eq 1) "got $($read.iteration)"
        $readTs = $null
        try { $readTs = ConvertTo-BeatTime $read.timestampUtc } catch { }
        _check 'timestamp parses as a date' ($null -ne $readTs) "raw: $($read.timestampUtc)"
        _check 'timestamp matches what was written' `
            ($null -ne $readTs -and $readTs -eq (ConvertTo-BeatTime $first.timestampUtc)) `
            "wrote $($first.timestampUtc), read back $readTs"

        # THE POINT OF THE WHOLE SCRIPT: a second beat must ADVANCE the file.
        # The predecessor's log did not advance during healthy operation, which
        # is why "quiet" and "dead" were indistinguishable.
        Start-Sleep -Milliseconds 25
        $null = Write-Heartbeat -Path $hb -Iteration 2 -ListenerState 'selftest' -ListenerPid 4242 -Interval 60
        $second = Read-Heartbeat -Path $hb
        _check 'second beat advances the iteration' ([int]$second.iteration -eq 2) "got $($second.iteration)"
        $t1 = ConvertTo-BeatTime $first.timestampUtc
        $t2 = ConvertTo-BeatTime $second.timestampUtc
        _check 'second beat advances the timestamp' ($t2 -gt $t1) "$t1 -> $t2"
        _check 'listener pid round-trips' ([int]$second.listenerPid -eq 4242) "got $($second.listenerPid)"

        # A heartbeat that cannot be written must be an ERROR, not silence.
        $bad = Join-Path $root 'no-such-dir\nested\beat.json'
        $created = $false
        try { $null = Write-Heartbeat -Path $bad -Iteration 3 -ListenerState 'selftest'; $created = $true } catch { }
        _check 'heartbeat creates missing parent directories' $created

        Write-WatchdogLog 'selftest wrote a log line'
        _check 'log file written' (Test-Path -LiteralPath $log)
        _check 'log line is present' ((Get-Content -LiteralPath $log -Raw) -match 'selftest wrote a log line')

        # Staleness detection is the consumer-side half of the contract.
        _check 'a fresh heartbeat is not stale' (-not (Test-HeartbeatStale -Path $hb -MaxAgeSeconds 3600))
        _check 'a heartbeat older than the bound IS stale' (Test-HeartbeatStale -Path $hb -MaxAgeSeconds -1)
    } catch {
        Say ("  FAIL  selftest threw: {0}" -f $_.Exception.Message)
        $failures.Add('exception')
    } finally {
        $script:LogFile = ''
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if ($failures.Count -gt 0) {
        Say ("[watchdog-selftest] FAILED: {0} check(s): {1}" -f $failures.Count, ($failures -join '; '))
        return 1
    }
    Say '[watchdog-selftest] OK - heartbeat written, re-read, advanced, and staleness-detected'
    return 0
}

function Test-HeartbeatStale {
    <#
      The consumer-side contract: is the watchdog itself alive? Exposed as a
      function so an operator (or a scheduled task) can ask the question the
      old log could not answer.
    #>
    param(
        [Parameter(Mandatory)][string] $Path,
        [int] $MaxAgeSeconds = 300
    )
    try {
        $beat = Read-Heartbeat -Path $Path
    } catch {
        return $true   # missing or unreadable heartbeat is STALE, never healthy
    }
    try {
        $ts = ConvertTo-BeatTime $beat.timestampUtc
    } catch {
        return $true   # an unparseable timestamp is STALE, never healthy
    }
    $age = ((Get-Date).ToUniversalTime() - $ts).TotalSeconds
    return ($age -gt $MaxAgeSeconds)
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if ($SelfTest) {
    # Assign, then exit: the function's only success-stream output is its
    # integer result (everything else goes through Say -> [Console]::Out).
    $rc = Invoke-SelfTest
    exit ([int]$rc)
}

if (-not $HeartbeatPath) { $HeartbeatPath = [IO.Path]::Combine($RunnerDir, 'watchdog-heartbeat.json') }
if (-not $LogPath)       { $LogPath       = [IO.Path]::Combine($RunnerDir, 'watchdog.log') }
$script:LogFile = $LogPath

# A RUNNER DIRECTORY THAT IS NOT THERE IS A CONFIGURATION ERROR, NOT A WARNING.
# Measured 2026-10-03: pointed at the retired C:\Users\dev\actions-runner, this
# script logged a heartbeat ERROR and a log WARN on every iteration and still
# exited 0. Everything downstream -- a scheduled task, a service wrapper, a gate
# -- reads that 0 as "the watchdog is fine". Refuse instead: it is the one moment
# at which the mistake is cheap to see.
#
# Deliberately AFTER the -SelfTest branch: the self-test must stay runnable on a
# host with no runner at all, which is most hosts and every CI lane.
if (-not (Test-Path -LiteralPath $RunnerDir)) {
    Write-WatchdogLog ("FATAL: RunnerDir does not exist: {0}" -f $RunnerDir)
    Write-WatchdogLog ('       Pass -RunnerDir <path>. A watchdog that cannot see the runner it ' +
                       'watches, cannot write its heartbeat, and cannot write its log has nothing ' +
                       'to report and must not exit 0.')
    exit 2
}

Write-WatchdogLog ("watchdog started (pid {0}); runnerDir={1}; interval={2}s; heartbeat={3}; noRestart={4}" -f `
    $PID, $RunnerDir, $IntervalSeconds, $HeartbeatPath, [bool]$NoRestart)

# ---------------------------------------------------------------------------
# INTERACTIVE DESKTOP DETECTION  (2026-09; the RDP-disconnect gap, WIN-74)
# ---------------------------------------------------------------------------
# These machines cannot use autologon, so an operator signs in -- usually over
# RDP -- and the runner lives in that session. When the RDP connection drops the
# session survives but its input desktop does not, so OpenInputDesktop fails and
# every UIA-driven lane is then driving a desktop that is not there.
#
# Nothing about that is visible to the runner. It stays online, takes the next
# job, and the UI lanes fail in ways that read as product defects.
#
# THE PREMISE IS NOW MEASURED, NOT REASONED (GitHub #132, 2026-09-03).
#
# It used to say "reasoned, not measured", because it was. A real RDP
# disconnect/reconnect on a real host settled it. Evidence:
# docs/audit/issue-132/desktop-probe-2026-09-03.log.
#
#   19:00:44.787   WTS -> Disconnected, OpenInputDesktop still OK
#   19:00:45.791   OpenInputDesktop -> FAIL(5), OpenDesktop("Default") still OK
#   19:02:15.263   WTS -> Active (reconnected), OpenInputDesktop STILL FAIL
#   19:02:26.372   OpenInputDesktop -> OK
#
# Corroborated independently: TerminalServices event 24 (DISCONNECTED) at
# 19:00:44.775, 12 ms before the probe saw it; event 25 (RECONNECTED) at
# 19:02:14.410.
#
# The premise holds, and holds in exactly the shape this design assumes: the
# named desktop stays open throughout while the INPUT desktop goes away. The
# session is alive; nothing is receiving input. Note the failure is FAIL(5) --
# ERROR_ACCESS_DENIED, not "no such desktop": the desktop still exists, this
# session simply may no longer touch it.
#
# TWO LAGS, AND WHY THE RESUME KEYS ON THE PROBE AND NOT ON WTS STATE.
# Reconnect reported Active a full ELEVEN SECONDS before the input desktop
# became usable. Anything that resumed on WTS state would restart the runner
# into a session that still cannot be driven -- a fresh instance of the very
# failure this guard exists to prevent, just eleven seconds wide. Because the
# resume waits on OpenInputDesktop, it cannot do that. (There is a ~1 s lag in
# the other direction too, which is harmless: the pause is one sample late.)
#
# On WIN-74: its three RDP workarounds are real but concern quirks of an ACTIVE
# session, not a disconnect. An earlier version of this comment cited them as
# making the transition "a known live defect source", which borrowed their
# CONFIRMED status for a claim they do not support. The measurement above is the
# evidence; WIN-74 never was.
#
# Downstream of the probe is covered by
# tools/gate/test_runner_watchdog_desktop_pause.py. The probe itself is covered
# by the measurement above, which is a one-off and cannot be a regression test --
# reproducing it requires a human to disconnect. tools/ops/probe_desktop_state.ps1
# is committed so it can be re-run rather than rebuilt.
#
# OpenInputDesktop is the signal, and it is the same one smoke_uia.py already
# uses, deliberately: two different definitions of "is there a desktop" would
# eventually disagree.
if (-not ([System.Management.Automation.PSTypeName]'PdfeDesktopProbe').Type) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PdfeDesktopProbe {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool CloseDesktop(IntPtr h);
}
'@ -Language CSharp
}

function Test-InteractiveDesktop {
    <#  $true when this session has an attached, unlocked input desktop.
        FAILS OPEN on an unexpected error: a probe that cannot make up its mind
        must not be the thing that stops a runner serving jobs. #>
    try {
        $h = [PdfeDesktopProbe]::OpenInputDesktop(0, $false, 0x0100)
        if ($h -ne [IntPtr]::Zero) {
            [void][PdfeDesktopProbe]::CloseDesktop($h)
            return $true
        }
        return $false
    } catch {
        Write-WatchdogLog ("WARN: desktop probe threw ({0}); assuming desktop present" -f $_.Exception.Message)
        return $true
    }
}

# ---------------------------------------------------------------------------
# THE OUTAGE JOURNAL  (the mid-test half of the desktop problem)
# ---------------------------------------------------------------------------
# Stopping the listener protects jobs that have NOT started. It does nothing for
# a job already running when the desktop goes away -- and that job is the worse
# case, because it does not merely fail, it fails DISHONESTLY: every UIA lookup
# misses, the suite goes red, and the red is indistinguishable from a genuine
# product defect. Someone then spends an afternoon in the product.
#
# Killing the worker was considered and rejected: an aborted job is a certain
# loss, and it still reports as a failure, so it trades a false red for a
# different false red. What is actually wrong is not that the job fails -- with
# no desktop it cannot do otherwise -- but that nothing records WHY.
#
# So: journal the window. A machine-wide JSONL file, because the test session
# and the watchdog are different processes and may be different sessions.
# tests/helpers/desktop_outage.py reads it and the root conftest reclassifies
# any failure overlapping an outage from `red` to `blocked`, which is this
# repository's existing vocabulary for "could not be driven" (WIN-45).
#
# Append-only and best-effort by construction: a journal write that throws must
# never take down the watchdog, since the watchdog's job is more important than
# the bookkeeping. A missing journal degrades to today's behaviour exactly.
function Get-OutageLogPath {
    if ($env:PDFE_DESKTOP_OUTAGE_LOG) { return $env:PDFE_DESKTOP_OUTAGE_LOG }
    $base = if ($env:ProgramData) { $env:ProgramData } else { $env:TEMP }
    return [IO.Path]::Combine($base, 'PDFE', 'desktop-outages.jsonl')
}

function Write-OutageEvent {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('outage-start', 'outage-end')]
        [string] $Event,
        [bool] $JobInFlight = $false
    )
    try {
        $path = Get-OutageLogPath
        $dir = Split-Path $path -Parent
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        # Round-trip ("o") format: unambiguous, sorts lexically, and Python's
        # datetime.fromisoformat reads it. A local-time string without an offset
        # would be ambiguous for exactly one hour every year, and the whole
        # mechanism is a time-window comparison.
        $record = [ordered]@{
            event       = $Event
            at          = (Get-Date).ToString('o')
            host        = $env:COMPUTERNAME
            session     = (Get-Process -Id $PID).SessionId
            jobInFlight = $JobInFlight
        }
        Add-Content -Path $path -Value ($record | ConvertTo-Json -Compress) -Encoding UTF8
        Write-WatchdogLog ("  outage journal <- {0} ({1})" -f $Event, $path)
    } catch {
        Write-WatchdogLog ("  WARN: could not write the outage journal ({0}); the pause still works, but a mid-test failure will not be reclassified" -f $_.Exception.Message)
    }
}

function Test-JobInFlight {
    <#  True when the runner appears to be executing a job, in which case the
        listener must NOT be stopped: aborting a live job is a certain loss,
        while letting an already-started job finish desktop-less is a loss that
        has already happened.

        THIS FUNCTION FAILS SAFE, and the first version did not. It tested only
        `Get-Process -Name Runner.Worker`, so a renamed worker -- or any
        enumeration failure -- returned $false, and $false falls through to the
        branch that KILLS the listener. The comment claimed the opposite ("the
        two together fail toward leave it alone"), which was wrong in the one
        direction that costs somebody a running job.

        Now: two independent signals, and "cannot tell" means "do not touch it".
        The child-process check is name-independent, so the documented worker
        name is no longer a single point of failure -- rtm-coverage.md records a
        real Runner.Worker.exe from an actual self-hosted run, so the name is
        very likely right today, but "very likely right" is not a thing to hang
        a Stop-Process on. #>
    param([int] $ListenerPid = 0)
    try {
        if (@(Get-Process -Name 'Runner.Worker' -ErrorAction SilentlyContinue).Count -gt 0) {
            return $true
        }
        # The worker is spawned BY the listener, so any child of it means work is
        # under way regardless of what that child is called.
        if ($ListenerPid -gt 0) {
            $kids = @(Get-CimInstance -ClassName Win32_Process `
                        -Filter "ParentProcessId=$ListenerPid" -ErrorAction Stop)
            if ($kids.Count -gt 0) { return $true }
        }
        return $false
    } catch {
        Write-WatchdogLog ("WARN: job-in-flight probe failed ({0}) -> assuming a job IS running; NOT stopping the listener" -f $_.Exception.Message)
        return $true
    }
}

$desktopWasMissing = $false

# THE HEARTBEAT IS THE CONTRACT, SO THE EXIT CODE HAS TO CARRY IT.
# Before this, every heartbeat write could fail and the process still exited 0 --
# see the RunnerDir note above for the run that proved it. These two counters are
# the only state needed to make the exit code honest: did the heartbeat EVER
# land, and did the LAST one land.
$heartbeatsWritten = 0
$heartbeatsFailed  = 0
$lastHeartbeatOk   = $false

$iteration = 0
while ($true) {
    $iteration++

    $listener = $null
    try {
        $listener = Get-Process -Name 'Runner.Listener' -ErrorAction SilentlyContinue
    } catch {
        Write-WatchdogLog ("WARN: Get-Process failed: {0}" -f $_.Exception.Message)
    }

    # 'paused-no-desktop' is a THIRD state, not a flavour of down. An operator
    # reading the heartbeat has to be able to tell "nobody is serving jobs
    # because the desktop went away" from "the listener crashed" -- they need
    # opposite responses.
    $desktopForHb = if ($IgnoreDesktopState) { $true } else { Test-InteractiveDesktop }
    $state = if (-not $desktopForHb) { 'paused-no-desktop' }
             elseif ($listener)      { 'up' }
             else                    { 'down' }
    $lpid  = if ($listener) { [int](@($listener)[0].Id) } else { 0 }

    # THE HEARTBEAT IS UNCONDITIONAL. This single line is the fix for F46's
    # "quiet and dead look identical".
    try {
        $null = Write-Heartbeat -Path $HeartbeatPath -Iteration $iteration `
                    -ListenerState $state -ListenerPid $lpid -Interval $IntervalSeconds
        $heartbeatsWritten++
        $lastHeartbeatOk = $true
    } catch {
        $heartbeatsFailed++
        $lastHeartbeatOk = $false
        # Keep looping rather than dying here: the restart logic is still worth
        # running, and a transient I/O blip must not take down the monitor. The
        # failure is carried out in the EXIT CODE instead, so a bounded run or a
        # wrapper cannot mistake it for health.
        Write-WatchdogLog ("ERROR: heartbeat write failed (iteration {0}, {1} failed / {2} written so far): {3}" -f `
            $iteration, $heartbeatsFailed, $heartbeatsWritten, $_.Exception.Message)
    }

    # --- desktop gate -------------------------------------------------------
    # The SAME sample the heartbeat reported, deliberately not a second probe.
    # Two calls per iteration can disagree across the few milliseconds between
    # them, and then the heartbeat says `up` while the gate pauses the runner --
    # an operator reading the heartbeat would be told the opposite of what the
    # watchdog just did. This script already argues the point about smoke_uia.py
    # ("two different definitions of 'is there a desktop' would eventually
    # disagree"); it was calling the one definition twice.
    $desktopOk = $desktopForHb

    if (-not $desktopOk) {
        if (-not $desktopWasMissing) {
            Write-WatchdogLog 'NO INTERACTIVE DESKTOP (RDP disconnected, or session locked) -> PAUSING'
            $desktopWasMissing = $true
            Write-OutageEvent -Event 'outage-start' -JobInFlight (Test-JobInFlight -ListenerPid $lpid)
        }
        if ($listener) {
            if (Test-JobInFlight -ListenerPid $lpid) {
                # Deliberately not killed. Let it finish or fail on its own; the
                # damage from aborting a live job is certain, the damage from
                # letting a started job run desktop-less is already done.
                #
                # But "already done" is not the same as "correctly reported".
                # A job that keeps running against a desktop that is not there
                # can fail in ways indistinguishable from a PRODUCT defect, and
                # that is the expensive kind of false failure: it sends someone
                # into the product looking for a bug that does not exist.
                #
                # HOW OFTEN, MEASURED (#133): less than I first claimed. This
                # comment used to say "every UIA lookup in it fails". It does
                # not. Across a real 40-second RDP disconnect, all seven live
                # tests in tests/test_ui_canvas_sendinput.py PASSED -- the
                # session's desktop objects survive, and only OpenInputDesktop
                # (which asks who is receiving INPUT) fails. Evidence:
                # docs/audit/issue-133/.
                #
                # So the reclassification below is insurance, not a routine
                # correction. It fires when a test genuinely could not be
                # driven, and stays silent otherwise -- which is the behaviour
                # you want from something that excuses a red.
                #
                # The outage journal above is what fixes that. It records the
                # window, and tests/helpers/desktop_outage.py lets the test
                # session reclassify any failure overlapping it from `red` to
                # `blocked` -- a recorded non-pass, with the reason. The job
                # still fails; it just stops lying about WHY.
                Write-WatchdogLog '  job in flight (Runner.Worker present) -> NOT stopping the listener; outage journalled so its failures are reclassified, not believed'
            } elseif ($NoRestart) {
                Write-WatchdogLog '  -NoRestart set -> observing only, listener left running'
            } else {
                # Stopping the listener takes the runner OFFLINE, so queued jobs
                # WAIT instead of running blind. That is the pause: the work is
                # deferred, not failed.
                # RE-QUERY, and verify the process is still what we think it
                # is, immediately before killing it. $listener/$lpid were
                # sampled earlier in the iteration and a PID can be recycled in
                # between -- and during development one run logged a PID that no
                # runner on this host could have owned, which I could not
                # reproduce. An unexplained value on the one code path that
                # calls Stop-Process is not something to reason away: this makes
                # killing an unrelated process impossible by construction rather
                # than by argument.
                $live = @(Get-Process -Name 'Runner.Listener' -ErrorAction SilentlyContinue)
                if ($live.Count -eq 0) {
                    # GitHub #131 RECURRENCE CAPTURE. Reaching here means the
                    # SAMPLE said a listener existed and the RE-QUERY, taken
                    # moments later, says none does. That is either an ordinary
                    # race (the listener exited in between -- benign, and the
                    # common case) or the unexplained 2026-09 observation, in
                    # which the sample was truthy on a host with zero such
                    # processes.
                    #
                    # The two are indistinguishable AFTER the fact, which is
                    # exactly why the original could not be diagnosed: its
                    # evidence went to a console that no longer exists. So
                    # capture the sample's own shape to the LOG FILE, in the
                    # same instant, rather than reconstructing it later. These
                    # are the four values the investigation record asks a
                    # future reproducer for.
                    #
                    # Observation only. It must not change control flow: the
                    # consequence is already closed by construction, and a
                    # behavioural change here would be new untested risk for a
                    # bug that has never reproduced.
                    $sampleType = if ($null -eq $listener) { '<null>' }
                                  else { $listener.GetType().FullName }
                    $sampleCount = @($listener).Count
                    Write-WatchdogLog ('  listener already gone -> nothing to stop (sample said pid {0}; type={1}; count={2}; requery=0)' -f $lpid, $sampleType, $sampleCount)
                } else {
                    foreach ($proc in $live) {
                        Write-WatchdogLog ('  stopping listener (pid {0}) so jobs queue instead of running blind' -f $proc.Id)
                        try { Stop-Process -InputObject $proc -Force -ErrorAction Stop }
                        catch { Write-WatchdogLog ("  ERROR: could not stop listener: {0}" -f $_.Exception.Message) }
                    }
                }
            }
        }
        # Do NOT fall through to the restart logic: restarting into a session
        # with no desktop is the exact thing being prevented.
        if ($MaxIterations -gt 0 -and $iteration -ge $MaxIterations) {
            Write-WatchdogLog ("reached MaxIterations={0}, exiting" -f $MaxIterations)
            break
        }
        Start-Sleep -Seconds $IntervalSeconds
        continue
    }

    if ($desktopWasMissing) {
        Write-WatchdogLog 'interactive desktop RESTORED -> resuming'
        $desktopWasMissing = $false
        Write-OutageEvent -Event 'outage-end'
    }

    if (-not $listener) {
        if ($NoRestart) {
            Write-WatchdogLog 'Runner.Listener DOWN -> -NoRestart set, observing only'
        } else {
            Write-WatchdogLog 'Runner.Listener DOWN -> launching detached run.cmd'
            # FULL PATH, deliberately: with NoDefaultCurrentDirectoryInExePath=1
            # cmd will not resolve a bare 'run.cmd' from the working directory,
            # which silently failed to restart the listener (runner stranded
            # offline 2026-06-22..25). Kept from the predecessor because it is a
            # real, hard-won fix.
            $runCmd = [IO.Path]::Combine($RunnerDir, 'run.cmd')
            try {
                if (-not (Test-Path -LiteralPath $runCmd)) {
                    throw "run.cmd not found at $runCmd"
                }
                Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', $runCmd `
                    -WorkingDirectory $RunnerDir -WindowStyle Hidden
            } catch {
                Write-WatchdogLog ("ERROR: restart launch failed: {0}" -f $_.Exception.Message)
            }
            Start-Sleep -Seconds 20
            $now = Get-Process -Name 'Runner.Listener' -ErrorAction SilentlyContinue
            if ($now) { Write-WatchdogLog ("Runner.Listener back UP (pid {0})" -f (@($now)[0].Id)) }
            else      { Write-WatchdogLog 'WARN: listener still not up after restart attempt' }
        }
    }

    if ($MaxIterations -gt 0 -and $iteration -ge $MaxIterations) {
        Write-WatchdogLog ("reached MaxIterations={0}, exiting" -f $MaxIterations)
        break
    }
    Start-Sleep -Seconds $IntervalSeconds
}

# ---------------------------------------------------------------------------
# EXIT CODE  (2026-10-03)
# ---------------------------------------------------------------------------
# "Ran to completion" is NOT the thing worth reporting -- the heartbeat landing
# is. Exit 0 only when the last beat of the run actually reached disk; a run that
# never wrote one is a failure however cleanly the loop finished.
Write-WatchdogLog ("watchdog exiting after {0} iteration(s); heartbeats written={1} failed={2}" -f `
    $iteration, $heartbeatsWritten, $heartbeatsFailed)

if ($heartbeatsWritten -eq 0) {
    Write-WatchdogLog ("FATAL: not one heartbeat was written to {0} -- this run proved nothing about the runner and must not report success." -f $HeartbeatPath)
    exit 3
}
if (-not $lastHeartbeatOk) {
    Write-WatchdogLog ("ERROR: the final heartbeat write to {0} failed, so the file on disk is STALE and a reader cannot distinguish that from a dead watchdog." -f $HeartbeatPath)
    exit 4
}
if ($heartbeatsFailed -gt 0) {
    Write-WatchdogLog ("WARN: {0} heartbeat write(s) failed during this run; the last one succeeded" -f $heartbeatsFailed)
}
exit 0
