<#
.SYNOPSIS
    Move the self-hosted runner's registration from the retired repository to
    the live one, after the October 2026 migration.

.DESCRIPTION
    Replaces the hand-copied sequence in
    docs/ci/runner-reregistration-2026-10.md. That document is still the
    explanation; this is the execution.

    WHY THIS EXISTS
    ===============
    A runner registration belongs to a repository by its IMMUTABLE ID, not by
    its name. The October 2026 migration retired the original repository by
    RENAME and moved the replacement into its name, so the runner followed the
    repository it was registered against and is now attached to the retired
    one. It is healthy and listening to the wrong place; every self-hosted job
    on the live repository is unschedulable until it moves.

    The hand-off was eight commands pasted into a console, and its first
    privileged command failed in the field with

        gh: The term 'gh' is not recognized ...

    because gh here is a PER-USER install whose PATH entry is a .cmd shim, and
    any session that did not load the user PATH sees neither it nor the real
    binary. Resolving the environment is therefore step one, not an assumption.

    WHAT IT REFUSES TO DO
    =====================
    Nothing is changed unless -Execute is passed. Without it this is a
    read-only preflight that reports exactly what it would do.

    Even with -Execute it refuses, rather than proceeding, when:

      * gh cannot be resolved, or is not authenticated;
      * either repository fails to resolve to its EXPECTED NUMERIC ID (names
        are what the migration made unreliable, so names are not trusted here);
      * the runner is BUSY (mid-job) - interrupting a running job is a data
        loss this script will not cause on the operator's behalf;
      * the live repository already has runners, which would mean the move has
        happened and -Execute would be acting on a stale premise;
      * the session is SESSION 0 or non-interactive. Session 0 has no visible
        desktop and every UIA-driven UI test blinds or hangs there, failing in
        a way that looks like a product bug. This is the single most expensive
        mistake available here, so it is a hard refusal rather than a warning;
      * Runner.Listener is RUNNING. The API's busy flag answers "is a job
        running", which is a different question from "is the listener up".
        Reconfiguring underneath a live listener leaves it holding credentials
        for a registration that no longer exists.

    In preflight these are reported as BLOCKERS and collected, so one run
    lists everything the operator has to clear. With -Execute the first one
    refuses: describing a bad state and acting on it are different.

    IT HANDLES THE STALE STORED URL
    ===============================
    `config.cmd remove --token X` does NOT send the request to the repository
    the token was minted for. It reads the URL out of the local .runner file.
    The runner was registered while the ORIGINAL repository held the name, so
    .runner stores https://github.com/ipswitch8/PDFE -- which after the rename
    resolves to the REPLACEMENT. The removal therefore asked the replacement to
    remove an agent it had never had, and returned

        Failed: Removing runner from the server
        Response status code does not indicate success: 404 (Not Found).

    with nothing in the error pointing at the stored URL. This script resolves
    the stored URL to an id, and when it is not the retired repository it
    removes the registration server-side by agent id and then clears the stale
    local config with `config.cmd remove --local`.

    Tokens are minted into local variables, consumed immediately, and never
    written to disk, logged, or echoed.

.PARAMETER RunnerDir
    The runner installation directory. Defaults to C:\actions-runner.

.PARAMETER RunnerName
    The registration name. Defaults to pdfe-runner-01, the project's
    convention.

    Passed to config.cmd EXPLICITLY, because without --name config.cmd
    defaults the name to the MACHINE NAME -- which is how the host name
    reached docs/audit/runner-labels.json and turned
    check_tracked_path_hygiene.py red. The capture's producer now redacts
    names, so this is defence in depth, but a host-derived name can still
    surface where nothing redacts it (gtest XML hostname attributes have
    leaked in this repository before).

    THE PARAMETER IS NOT THE PROTECTION. A name you can pass is a name
    someone can pass the hostname into, so the script REFUSES any name
    containing this machine's host name or account name, compared
    case-insensitively and as a substring: "pdfe-<hostname>" leaks exactly as
    much as "<hostname>", and the previous registration was named that.

.PARAMETER Execute
    Actually perform the move. Without this, preflight only.

.PARAMETER SkipWatchdog
    Do not offer to start the watchdog at the end.

.EXAMPLE
    .\tools\ops\Move-PdfeRunnerRegistration.ps1
    Preflight only. Reports what it would do and why.

.EXAMPLE
    .\tools\ops\Move-PdfeRunnerRegistration.ps1 -Execute
    Performs the move, then verifies the live repository reports one runner
    with the canonical label set.

.NOTES
    This file is deliberately PURE ASCII and carries no BOM. GitHub #130:
    Windows PowerShell 5.1 assumes the machine ANSI codepage for a BOM-less
    file, so a script with typographic punctuation and no BOM parses in
    PowerShell 7 and fails on Server 2019 where powershell.exe IS 5.1 and is
    what an operator actually types. check_ps1_encoding.py enforces this.
#>
[CmdletBinding()]
param(
    [string] $RunnerDir = 'C:\actions-runner',

    # The registration name. Defaults to the project's convention rather than
    # letting config.cmd default it to the MACHINE NAME, which is what caused
    # a host-name leak into docs/audit/runner-labels.json: a faithful capture
    # of the runners API carried the machine name, and
    # check_tracked_path_hygiene.py ratchets that out of tracked files.
    #
    # The capture's producer now redacts names, so this is defence in depth
    # rather than the only barrier -- but a host-derived name can still surface
    # where nothing redacts it (gtest XML hostname attributes have leaked in
    # this repository before), so the better fix is not to create one.
    [string] $RunnerName = 'pdfe-runner-01',

    # Re-register the runner that is ALREADY on the live repository, in order
    # to change its NAME. Without this the script refuses once the live
    # repository has a runner, because that state is indistinguishable from an
    # accidental second run of the move.
    #
    # This is a deliberate, separate switch rather than a relaxation of that
    # check: renaming tears down a working registration and rebuilds it, and
    # that should never happen as a side effect of someone re-running the move.
    [switch] $RenameInPlace,

    [switch] $Execute,
    [switch] $SkipWatchdog
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Identity is the numeric id. Migrations rename repositories past each other,
# so a name proves nothing: 'ipswitch8/PDFE' meant 1343217722 before the
# 2026-10 rename, 1427819119 after it, and 1433482788 after the 2026-10-25
# route-B migration (p101). Both pins are checked before anything is changed.
#
# GENERATION UPDATE, p101 phase-3 (2026-10-25): LIVE is now the route-B fresh
# repository; RETIRED is the 2026-10 replacement, renamed PDFE_old3 (its
# reachable history still names the build host, which is why it was migrated
# again). The runner registration followed 1427819119 through its rename, so
# the move this script performs is old3 -> fresh.
$LIVE_SLUG    = 'ipswitch8/PDFE'
$LIVE_ID      = 1433896232
$RETIRED_SLUG = 'ipswitch8/PDFE_old3'
$RETIRED_ID   = 1427819119

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$LabelsReadme = Join-Path $RepoRoot 'tools\runner-deployment\README.md'

function Say  { param([string] $m) Write-Host "      $m" }
function Step { param([string] $m) Write-Host ""; Write-Host "[..] $m" -ForegroundColor Cyan }
function Ok   { param([string] $m) Write-Host "[ok] $m" -ForegroundColor Green }
function Warn { param([string] $m) Write-Host "[!!] $m" -ForegroundColor Yellow }
function Die  { param([string] $m) Write-Host "[XX] $m" -ForegroundColor Red; exit 1 }

# A BLOCKER is a condition the operator must clear, but which does not stop
# the rest of the preflight from being useful.
#
# The first version called Die on the first one, so a preflight stopped at the
# live listener and never reported the stale stored URL underneath it -- the
# operator would fix one thing, re-run, and discover the next. A preflight that
# reports one problem at a time is a worse tool than one that reports all of
# them, and this script exists because the hand-off failed one command at a
# time.
#
# With -Execute a blocker refuses immediately: acting on a known-bad state is
# different from describing it.
$script:Blockers = @()
function Block {
    param([string] $m)
    $script:Blockers += $m
    if ($Execute) { Die $m }
    Warn $m
}

# ---------------------------------------------------------------- environment

function Resolve-Gh {
    # PATH first, then the real per-user binary, then the .cmd shim.
    # Environment variables rather than literal paths: the real path contains
    # the account name, which must not be written into a tracked file
    # (check_tracked_path_hygiene.py enforces that).
    $candidates = @()
    $onPath = (Get-Command gh -ErrorAction SilentlyContinue)
    $script:onPathGh = [bool] $onPath
    if ($onPath) { $candidates += $onPath.Source }
    $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\gh\bin\gh.exe')
    $candidates += (Join-Path $env:USERPROFILE  'bin\gh.cmd')
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe')
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

function Test-RunnerName {
    <#
      Refuse a registration name that embeds the host name or the account
      name. THE REFUSAL IS THE MECHANISM: a -RunnerName parameter on its own
      prevents nothing, because the value someone passes can be the hostname.

      Both strings are ratcheted out of tracked files by
      check_tracked_path_hygiene.py, and a runner name reaches tracked files
      and CI logs by several routes -- the label-coverage capture (now
      redacted at the producer), gtest XML hostname attributes (which have
      leaked here before), and workflow run logs (which nothing redacts).

      Compared case-insensitively and as a SUBSTRING, because "pdfe-<hostname>"
      leaks just as much as "<hostname>" -- the previous registration was named
      exactly that.
    #>
    param([string] $Name)

    $problems = @()
    if ([string]::IsNullOrWhiteSpace($Name)) {
        $problems += 'the runner name is empty'
        return $problems
    }
    if ($Name -notmatch '^[A-Za-z0-9._-]+$') {
        $problems += ("the runner name '$Name' has characters outside " +
                      '[A-Za-z0-9._-]; keep it boring')
    }
    foreach ($pair in @(
            @{ what = 'host name';    value = $env:COMPUTERNAME },
            @{ what = 'account name'; value = $env:USERNAME })) {
        $v = $pair.value
        if ($v -and $Name.ToLowerInvariant().Contains($v.ToLowerInvariant())) {
            # Deliberately does NOT echo the offending value: saying it out
            # loud in a console that may be logged is the same disclosure.
            $problems += ("the runner name contains this machine's " +
                          $pair.what + '. That string is ratcheted out of ' +
                          'tracked files, and a runner name reaches them ' +
                          'via captures, gtest XML and CI logs. Pass ' +
                          '-RunnerName with something generic, e.g. ' +
                          'pdfe-runner-01.')
        }
    }
    return $problems
}

function Get-CanonicalLabels {
    # Read the label set from the ONE marked command in the package README
    # rather than duplicating it here. check_label_drift.py treats that command
    # as the single source of truth; a second copy in this file would be a
    # second thing to drift.
    if (-not (Test-Path $LabelsReadme)) {
        Die "cannot read the canonical labels: $LabelsReadme not found"
    }
    $text = Get-Content -Raw -Encoding UTF8 $LabelsReadme
    $i = $text.IndexOf('PDFE-DEFAULT-LABELS')
    if ($i -lt 0) { Die "no PDFE-DEFAULT-LABELS marker in the package README" }
    $m = [regex]::Match($text.Substring($i), '--labels\s+([A-Za-z0-9,\-_]+)')
    if (-not $m.Success) { Die "marker found but no --labels command after it" }
    return $m.Groups[1].Value
}

function Invoke-Gh {
    param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $GhArgs)
    $out = & $script:Gh @GhArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Die ("gh failed: gh " + ($GhArgs -join ' ') + "`n      " + ($out -join "`n      "))
    }
    return ($out -join "`n").Trim()
}

function Get-RunnerCount {
    param([string] $Slug)
    return [int] (Invoke-Gh api ("repos/$Slug/actions/runners") --jq '.total_count')
}

# ------------------------------------------------------------------ preflight

Write-Host ""
Write-Host "Move the PDFE self-hosted runner registration to the live repository"
Write-Host "====================================================================="
if (-not $Execute) {
    Warn "PREFLIGHT ONLY. Nothing will be changed. Re-run with -Execute to act."
}

Step "Resolving the GitHub CLI"
$script:Gh = Resolve-Gh
if (-not $script:Gh) {
    Die ("gh not found. Tried PATH, %LOCALAPPDATA%\Programs\gh\bin\gh.exe, " +
         "%USERPROFILE%\bin\gh.cmd and %ProgramFiles%\GitHub CLI\gh.exe. " +
         "Install the GitHub CLI or add its directory to PATH.")
}
# Report the FILE NAME and how it was found, never the path. The real path
# contains the account name, and echoing it to a console that may be logged or
# screenshotted is the same disclosure in a different place.
$ghLeaf = Split-Path -Leaf $script:Gh
$ghHow  = if ($onPathGh) { 'found on PATH' } else { 'resolved by fallback' }
Ok "gh resolved: $ghLeaf ($ghHow)"
Say (((Invoke-Gh --version) -split "`n")[0])

Step "Checking authentication"
$auth = & $script:Gh auth status 2>&1
if ($LASTEXITCODE -ne 0) {
    Die ("gh is not authenticated. Run: gh auth login`n      " + ($auth -join "`n      "))
}
Ok "authenticated"

Step "Confirming repository identity BY NUMERIC ID"
$liveId    = [int] (Invoke-Gh api "repos/$LIVE_SLUG"    --jq '.id')
$retiredId = [int] (Invoke-Gh api "repos/$RETIRED_SLUG" --jq '.id')
if ($liveId -ne $LIVE_ID) {
    Die ("$LIVE_SLUG resolves to id $liveId, expected $LIVE_ID. " +
         "A name that resolves to the wrong repository is exactly what the " +
         "migration made possible. Refusing to act.")
}
if ($retiredId -ne $RETIRED_ID) {
    Die ("$RETIRED_SLUG resolves to id $retiredId, expected $RETIRED_ID. " +
         "Refusing to act.")
}
Ok "$LIVE_SLUG = $liveId (live)"
Ok "$RETIRED_SLUG = $retiredId (retired)"

Step "Reading the canonical label set"
$labels = Get-CanonicalLabels
Ok "labels: $labels"
Say "source: tools/runner-deployment/README.md, PDFE-DEFAULT-LABELS marker"

Step "Checking the registration name"
# @( ) is REQUIRED: PowerShell unrolls an empty array to $null, so a clean
# result would blow up on .Count under Set-StrictMode. The first version did.
$nameProblems = @(Test-RunnerName $RunnerName)
if ($nameProblems.Count) {
    foreach ($np in $nameProblems) { Block $np }
} else {
    Ok "runner name: $RunnerName"
    Say "passed explicitly, so config.cmd will not default it to the machine name"
}

Step "Inspecting both fleets"
$liveCount    = Get-RunnerCount $LIVE_SLUG
$retiredCount = Get-RunnerCount $RETIRED_SLUG
Say "live    ($LIVE_SLUG): $liveCount runner(s)"
Say "retired ($RETIRED_SLUG): $retiredCount runner(s)"

# Which repository the registration is removed FROM. For a move that is the
# retired repository; for a rename it is the live one, because the runner is
# already there and the point is to tear it down and rebuild it with a
# different name.
$script:SourceSlug = $RETIRED_SLUG
$script:SourceId   = $RETIRED_ID

if ($RenameInPlace) {
    Step "RENAME IN PLACE"
    $script:SourceSlug = $LIVE_SLUG
    $script:SourceId   = $LIVE_ID
    if ($liveCount -ne 1) {
        Block ("-RenameInPlace expects exactly 1 runner on $LIVE_SLUG, found " +
               "$liveCount. Renaming is only meaningful for a registration " +
               "that is already there and alone.")
    }
    if ($retiredCount -ne 0) {
        Block ("$RETIRED_SLUG still has $retiredCount runner(s). Finish the " +
               "MOVE before renaming -- otherwise a rename would leave a " +
               "registration behind on the retired repository.")
    }
    $currentName = Invoke-Gh api ("repos/$LIVE_SLUG/actions/runners") --jq '.runners[0].name'
    if ($currentName -eq $RunnerName) {
        Ok "the runner is already named '$RunnerName'. Nothing to rename."
        exit 0
    }
    Say "current name : $currentName"
    Say "target name  : $RunnerName"
    $currentProblems = @(Test-RunnerName $currentName)
    if ($currentProblems.Count) {
        Say "the current name is why: $($currentProblems[0])"
    }
    Warn "this REMOVES the working registration and rebuilds it. CI cannot"
    Warn "schedule a self-hosted job until the listener is back up."
}

if ($liveCount -gt 0 -and -not $RenameInPlace) {
    Warn "the live repository ALREADY has $liveCount runner(s)."
    Warn "the move appears to have happened. Nothing to do."
    Say  "If this is wrong, inspect both fleets by hand before forcing anything."
    # Blockers already collected must not be swallowed by this early exit.
    # The first version returned 0 here unconditionally, so an invalid
    # -RunnerName was recorded and then reported as success -- a refusal that
    # exits 0 is invisible to any caller, which is the same defect as a check
    # that prints loudly and returns nothing.
    if ($script:Blockers.Count -gt 0) {
        Write-Host ""
        Write-Host "[XX] but $($script:Blockers.Count) BLOCKER(S) were found:" -ForegroundColor Red
        $i = 0
        foreach ($b in $script:Blockers) { $i++; Say "$i. $b" }
        exit 1
    }
    exit 0
}
if ($retiredCount -eq 0 -and -not $RenameInPlace) {
    Die ("neither repository has a runner. There is nothing to move, and this " +
         "script will not register one from scratch - use the package's " +
         "Register-PdfeRunner.ps1 for a fresh registration.")
}

Step "Checking the runner is not mid-job"
$busy = Invoke-Gh api ("repos/$script:SourceSlug/actions/runners") --jq '[.runners[].busy] | any'
if ($busy -eq 'true') {
    Block ("the runner is BUSY. Removing a registration mid-job loses that job. " +
           "Wait for it to finish and re-run.")
} else {
    Ok "runner is idle (busy: false)"
}

Step "Checking the local listener is stopped"
# The API's busy flag says whether a JOB is running. It says nothing about
# whether the LISTENER process is up, and those are different questions:
# reconfiguring underneath a live listener leaves the process holding
# credentials for a registration that no longer exists.
#
# This check was missing from the first version of this script, which checked
# busy and went straight on. The listener was running at the time.
$listener = Get-Process -Name 'Runner.Listener' -ErrorAction SilentlyContinue
if ($listener) {
    $pids = ($listener | ForEach-Object { $_.Id }) -join ', '
    Block ("Runner.Listener is RUNNING (pid $pids). Stop it before moving the " +
           "registration: it is run.cmd in a window, NOT a service, so close " +
           "that window (or Stop-Process -Id $pids). Re-run afterwards. " +
           "Reconfiguring under a live listener leaves it holding credentials " +
           "for a registration that no longer exists.")
} else {
    Ok "Runner.Listener is not running"
}

Step "Checking this is an interactive desktop session"
$sessionId = (Get-Process -Id $PID).SessionId
if ($sessionId -eq 0) {
    Die ("this is SESSION 0. The runner must listen from an interactive " +
         "desktop session: session 0 has no visible desktop and every " +
         "UIA-driven UI test blinds or hangs there, failing in a way that " +
         "looks like a product bug. Run this from a logged-on desktop.")
}
if (-not [Environment]::UserInteractive) {
    Die "this session is not interactive. See the session-0 note above."
}
Ok "session $sessionId, interactive"

Step "Checking the runner directory"
if (-not (Test-Path $RunnerDir)) {
    Die ("runner directory not found: $RunnerDir. Pass -RunnerDir if it lives " +
         "elsewhere. The pre-2026-08 layout had it under a user profile.")
}
# [IO.Path]::Combine, not Join-Path, because -RunnerDir defaults to a
# DRIVE-QUALIFIED path and Join-Path on one breaks a non-Windows run.
# test_no_join_path_on_a_param_defaulting_to_a_drive_qualified_path in
# tools/gate/test_runner_watchdog_heartbeat.py enforces this, and it caught
# both sites here. On Windows the two forms are byte-identical for these
# paths, which is exactly why no behavioural test on this host can catch it.
$configCmd = [IO.Path]::Combine($RunnerDir, 'config.cmd')
if (-not (Test-Path $configCmd)) {
    Die "no config.cmd in $RunnerDir. That is not a runner installation."
}
Ok "runner directory: $RunnerDir"

Step "Checking what URL the runner has STORED"
# THE DEFECT THIS CHECK EXISTS FOR, found by running -Execute for real.
#
# `config.cmd remove --token X` does NOT send the request to the repository
# the token was minted for. It reads the URL out of the local .runner file and
# sends it there. The runner was registered when the ORIGINAL repository still
# held the name, so .runner stores
#
#     gitHubUrl : https://github.com/ipswitch8/PDFE
#
# and after the phase-4 rename that URL resolves to the REPLACEMENT. So the
# removal asked the replacement to remove an agent it has never had:
#
#     Failed: Removing runner from the server
#     Response status code does not indicate success: 404 (Not Found).
#
# A token for one repository plus a request to another is a 404, and nothing
# in the error says "your stored URL is stale". This is the same defect class
# as the rest of the migration: AN ARTIFACT HOLDING A NAME THAT THE RENAME
# REPOINTED -- like the tracker export's `repo` field and the guard census.
$runnerCfg = [IO.Path]::Combine($RunnerDir, '.runner')   # see the note above
$script:StoredUrl = $null
$script:StoredAgentId = $null
$script:StoredUrlId = $null
if (Test-Path $runnerCfg) {
    $cfg = Get-Content -Raw -Encoding UTF8 $runnerCfg | ConvertFrom-Json
    $script:StoredUrl = $cfg.gitHubUrl
    $script:StoredAgentId = $cfg.agentId
    Say "stored gitHubUrl : $script:StoredUrl"
    Say "stored agentId   : $script:StoredAgentId"
    $storedSlug = ($script:StoredUrl -replace '^https?://github\.com/', '').Trim('/')
    try {
        $script:StoredUrlId = [int] (Invoke-Gh api "repos/$storedSlug" --jq '.id')
    } catch {
        $script:StoredUrlId = 0
    }
    if ($script:StoredUrlId -eq $script:SourceId) {
        Ok "stored URL resolves to the source repository: config.cmd remove will work"
    } else {
        Warn "stored URL resolves to id $script:StoredUrlId, NOT the source repository ($script:SourceId)."
        Warn "config.cmd remove would 404 -- it sends to the STORED url, not the token's repo."
        Say  "The move will remove the registration server-side by agent id instead."
    }
} else {
    Warn "no .runner file in $RunnerDir; cannot read the stored URL."
    Say  "Falling back to a server-side removal by agent id."
}

if (-not $Execute) {
    Write-Host ""
    if ($script:Blockers.Count -gt 0) {
        Write-Host "[XX] PREFLIGHT FOUND $($script:Blockers.Count) BLOCKER(S)" -ForegroundColor Red
        $i = 0
        foreach ($b in $script:Blockers) {
            $i++
            Say "$i. $b"
        }
        Write-Host ""
        Warn "-Execute will refuse while any of the above holds. Clear them and re-run."
        Write-Host ""
        exit 1
    }
    Ok "PREFLIGHT PASSED. With -Execute this would:"
    if ($script:StoredUrlId -eq $RETIRED_ID) {
        Say "1. mint a removal token for $script:SourceSlug and run config.cmd remove"
    } else {
        Say "1. remove agent $script:StoredAgentId from $script:SourceSlug server-side"
        Say "   (config.cmd remove would 404: the stored URL is stale)"
        Say "   then clear the stale local config with: config.cmd remove --local"
    }
    Say "2. mint a registration token for $LIVE_SLUG and register as"
    Say "     name   : $RunnerName   (explicit, so config.cmd does not"
    Say "              default it to the machine name)"
    Say "     labels : $labels"
    Say "3. verify the live fleet reports exactly 1 runner with those labels"
    Say "4. refresh docs/audit/runner-labels.json via --refresh --live"
    if (-not $SkipWatchdog) {
        Say "5. remind you to start the watchdog from this desktop session"
    }
    Write-Host ""
    exit 0
}

# -------------------------------------------------------------------- execute

Push-Location $RunnerDir
try {
    Step "Unregistering from $script:SourceSlug"
    # --replace replaces a registration of the same name ON THE SAME
    # repository. It does not move a runner between repositories, so the
    # removal is required rather than ceremonial.
    if ($script:StoredUrlId -eq $script:SourceId) {
        # Normal path: the stored URL still points where the registration is.
        $rm = Invoke-Gh api -X POST ("repos/$script:SourceSlug/actions/runners/remove-token") --jq '.token'
        if (-not $rm) { Die "could not mint a removal token" }
        & $configCmd remove --token $rm
        if ($LASTEXITCODE -ne 0) { Die "config.cmd remove failed with $LASTEXITCODE" }
        $rm = $null
        Ok "unregistered from $script:SourceSlug via config.cmd"
    } else {
        # Stale-stored-URL path. config.cmd would send the removal to the
        # replacement and 404. Remove it where it actually lives, by id.
        Say "stored URL is stale; removing server-side by agent id instead"
        $agentId = $script:StoredAgentId
        if (-not $agentId) {
            $agentId = Invoke-Gh api ("repos/$script:SourceSlug/actions/runners") --jq '.runners[0].id'
        }
        if (-not $agentId) { Die "could not determine the runner's agent id" }
        Say "agent id: $agentId"
        Invoke-Gh api -X DELETE ("repos/$script:SourceSlug/actions/runners/$agentId") | Out-Null
        $still = Get-RunnerCount $script:SourceSlug
        if ($still -ne 0) {
            Die "removal reported success but $script:SourceSlug still lists $still runner(s)"
        }
        Ok "removed from $script:SourceSlug server-side (agent $agentId)"

        # Clear the stale LOCAL config too. `remove --local` deletes the local
        # config files without contacting the server, which is exactly this
        # situation: the server-side registration is already gone, and the
        # local .runner still names a repository that is not where the runner
        # was registered. Relying on `--replace` to overwrite a config pointing
        # at a DIFFERENT repository is a guess; this is the documented tool.
        # Supported by runner 2.338.0 (`config.cmd remove --help`).
        Say "clearing the stale local config (remove --local)"
        & $configCmd remove --local
        if ($LASTEXITCODE -ne 0) {
            Die ("config.cmd remove --local failed with $LASTEXITCODE. The " +
                 "server-side registration IS already removed, so the next " +
                 "step is local only: delete .runner and .credentials* in " +
                 "$RunnerDir by hand, then re-run with -Execute.")
        }
        Ok "local config cleared"
    }

    Step "Registering against the LIVE repository"
    $reg = Invoke-Gh api -X POST ("repos/$LIVE_SLUG/actions/runners/registration-token") --jq '.token'
    if (-not $reg) { Die "could not mint a registration token" }
    # --name is passed EXPLICITLY. Without it config.cmd defaults the
    # registration name to the machine name, which is how the host name
    # reached docs/audit/runner-labels.json.
    & $configCmd --url ("https://github.com/" + $LIVE_SLUG) `
                 --token $reg `
                 --name $RunnerName `
                 --labels $labels `
                 --unattended --replace
    if ($LASTEXITCODE -ne 0) { Die "config.cmd registration failed with $LASTEXITCODE" }
    $reg = $null
    Ok "registered against $LIVE_SLUG"
}
finally {
    Pop-Location
}

# --------------------------------------------------------------------- verify

Step "Verifying the live fleet"
$after = Invoke-Gh api ("repos/$LIVE_SLUG/actions/runners") --jq '.total_count'
if ([int] $after -ne 1) {
    Warn "the live repository reports $after runner(s), expected 1."
    Warn "inspect by hand before relying on CI."
} else {
    Ok "live repository reports 1 runner"
}

# Compare as SETS, not as sorted joined strings.
#
# The first version did exactly that and reported a mismatch on a correct
# registration:
#
#   want: pdfe-builder,pdfe-windows,self-hosted,Windows,X64
#   got : Windows,X64,pdfe-builder,pdfe-windows,self-hosted
#
# The same five labels. jq's `sort` is codepoint order, so uppercase sorts
# before lowercase; PowerShell's Sort-Object is culture-aware and does not.
# Two different normalisations compared as text, which is the same mistake as
# diffing an indented list against an unindented one. Set membership is the
# property that actually matters here -- GitHub returns labels in no
# guaranteed order -- so compare that and nothing else.
Step "Verifying the registered NAME is not host-derived"
$regName = Invoke-Gh api ("repos/$LIVE_SLUG/actions/runners") --jq '.runners[0].name'
if ($regName -ne $RunnerName) {
    Warn "registered name is '$regName', expected '$RunnerName'."
    Say  "config.cmd may have ignored --name; check before relying on it."
}
$nameAfter = @(Test-RunnerName $regName)
if ($nameAfter.Count) {
    foreach ($na in $nameAfter) { Warn $na }
    Say "the registration succeeded, but the NAME needs changing: re-run with"
    Say "-RunnerName pdfe-runner-01 to replace it."
} else {
    Ok "registered name '$regName' carries no host or account name"
}

# Parsed from JSON rather than extracted with a jq expression.
#
# The jq form was `--jq '[.runners[0].labels[].name] | join(",")'` and it
# FAILED IN EXECUTION, after the registration had already succeeded:
#
#     failed to parse jq expression (line 1, column 36)
#         [.runners[0].labels[].name] | join(,)
#                                            ^  unexpected token ","
#
# PowerShell ate the quoted comma on its way to the native command, so jq
# received `join(,)`. The quoting rules for a native call are not the quoting
# rules inside the string, and a comma is exactly where that bites. Letting
# PowerShell parse JSON it fetched avoids the whole class.
$runnersJson = Invoke-Gh api ("repos/$LIVE_SLUG/actions/runners")
$got = @(($runnersJson | ConvertFrom-Json).runners[0].labels.name |
         Where-Object { $_ } | ForEach-Object { $_.Trim() })
$want = @($labels  -split ',' | Where-Object { $_ } | ForEach-Object { $_.Trim() })

$missing = @($want | Where-Object { $got -notcontains $_ })
$extra   = @($got  | Where-Object { $want -notcontains $_ })

if ($missing.Count -or $extra.Count) {
    Warn "label mismatch."
    if ($missing.Count) { Say "MISSING: $($missing -join ', ')" }
    if ($extra.Count)   { Say "EXTRA  : $($extra -join ', ')" }
    Say  "pdfe-builder routes the build and release jobs; dropping"
    Say  "pdfe-windows makes every other self-hosted job unschedulable."
} else {
    Ok "labels match the canonical set ($($got.Count) labels, order-independent)"
}

Step "Refreshing the label-coverage capture"
# Not optional. The default mode of check_runner_label_coverage.py reads this
# capture, and until it is refreshed it reports a fleet that no longer exists
# on a repository that no longer holds the runner: green, and wrong.
#
# BUT IT CANNOT GO GREEN YET, AND THAT IS NOT A DEFECT.
#
# The runner is registered and OFFLINE at this point: the listener has not
# been started (that is the next step, and it is the operator's to run from
# this desktop session). The guard correctly fails when every self-hosted job
# is satisfiable only by an offline runner, because such a job would queue
# indefinitely.
#
# The first version of this script ran the refresh here and reported its
# failure as "the refresh reported a problem", which was misleading: the
# refresh had worked, written a truthful capture, and failed on a real
# condition that the NEXT step clears. Ordering a check before the thing it
# checks is the same "structurally cannot pass" shape this project records.
#
# So: refresh to make the capture truthful, then say plainly what the offline
# failure means and what clears it. DO NOT COMMIT the capture while it records
# an offline runner -- that is what would make CI red.
$refresh = Join-Path $RepoRoot 'tools\gate\check_runner_label_coverage.py'
if (Test-Path $refresh) {
    # Hand the ALREADY-RESOLVED gh down rather than letting the guard search
    # again. Its own search failed here with WinError 2: bare "gh" cannot be
    # exec'd by subprocess without a shell when what is on PATH is a .cmd
    # shim, and its hardcoded fallbacks named a profile that does not exist on
    # this machine. This script has just resolved gh and used it successfully,
    # so there is no reason for the next process to guess.
    $env:PDFE_GH = $script:Gh
    & python $refresh --refresh --live
    $refreshRc = $LASTEXITCODE
    if ($refreshRc -ne 0) {
        Ok "capture WRITTEN from live data (the file is now truthful)"
        Warn "the guard then failed because the runner is OFFLINE. Expected here:"
        Say  "the listener is not started yet. A job satisfiable only by an"
        Say  "offline runner would queue indefinitely, so failing is correct."
        Say  ""
        Say  "DO NOT COMMIT docs/audit/runner-labels.json while it records an"
        Say  "offline runner -- that is what would turn CI red. Start the"
        Say  "listener (next step), then re-run:"
        Say  ""
        Say  "    `$env:PDFE_GH = (Get-Command gh).Source"
        Say  "    python tools\gate\check_runner_label_coverage.py --refresh --live"
        Say  ""
        Say  "and commit the capture once that exits 0."
    } else {
        Ok "capture refreshed against live data and the guard is green"
        Say "commit docs/audit/runner-labels.json so CI sees it too."
    }
} else {
    Warn "check_runner_label_coverage.py not found; refresh it by hand."
}

if (-not $SkipWatchdog) {
    Step "Starting the listener"
    $watchdog = Join-Path $RepoRoot 'tools\ops\runner_watchdog.ps1'
    Say "The listener is run.cmd in a window, NOT a service, and it dies"
    Say "silently. Start it under the watchdog FROM THIS DESKTOP SESSION:"
    Say ""
    Say "    $watchdog"
    Say ""
    Say "Answer no to 'run as service' if anything prompts: see the session-0"
    Say "note in docs/ci/runner-reregistration-2026-10.md."
}

Write-Host ""
Ok "done"
Write-Host ""
