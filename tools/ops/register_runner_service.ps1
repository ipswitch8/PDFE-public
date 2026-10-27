<#
.SYNOPSIS
    Report on — and, only with an explicit -Apply, register — an automatic
    start mechanism for the pdfe-runner-01 self-hosted GitHub Actions runner.

.DESCRIPTION
    Meta-audit follow-up F44, deferred-backlog row F46.

    THE PROBLEM
    ===========
    `pdfe-runner-01` is `run.cmd` under C:\Users\dev\actions-runner, started by
    hand. It is not a service, there is no scheduled task, and no Run-key entry.
    Nothing restarts it after a reboot, a crash, or a logout. The watchdog that
    was supposed to cover the crash case was itself started by hand and had died
    (see tools/ops/runner_watchdog.ps1 for what was actually wrong with it), so
    CI stopped for roughly two weeks — 2026-07-25 to 2026-08-07 — with no signal.

    SCOPE — READ THIS FIRST
    =======================
    **This script installs nothing by default and this phase does not install
    anything on the live host.** pdfe-runner-01 is the user's workstation; choosing
    an autostart mechanism changes how their machine behaves at logon and is
    their decision, not an audit phase's. A plain invocation prints a DRY RUN
    report of the current state and the exact commands it *would* run. `-Apply`
    is required to change anything, and even then the script refuses without an
    elevated shell.

    The runner is also ONLINE at the time of writing and may be executing a job.
    Nothing here stops, restarts, or reconfigures a running listener.

    THE THREE MECHANISMS, AND WHY THE OBVIOUS ONE IS NOT RECOMMENDED
    ===============================================================
    1. LogonTask  (RECOMMENDED)
       A scheduled task, trigger "At log on of <user>", "Run only when user is
       logged on", launching tools/ops/runner_watchdog.ps1. The watchdog then
       keeps run.cmd alive on its 60-second loop and writes its heartbeat.
       + The runner inherits a real INTERACTIVE desktop session.
       + Full user profile: vcpkg at C:\Users\dev\source\vcpkg, the VS 2022
         toolchain, Qt, and the user's PATH all resolve exactly as they do today.
       + No re-registration of the runner, so no registration token needed and
         no risk to the current registration.
       - Requires the user to be logged on. Pair with console auto-logon if the
         box is expected to come back unattended after a reboot; that is a
         security trade-off (an unattended logged-on desktop) and is the user's
         call, so this script does NOT configure auto-logon.

    2. StartupTask
       Same, but trigger "At startup" with "Run whether user is logged on or
       not". Survives reboot with nobody logged in.
       - Runs in a NON-INTERACTIVE session with no desktop.

    3. Service   (`config.cmd --runasservice`, the GitHub-documented route)
       - Requires RE-RUNNING config.cmd with a fresh registration token, i.e.
         re-registering a runner that currently works. Highest-risk option.
       - Runs as a service in session 0: no desktop, no interactive window
         station.
       - Requires the service account's password to be supplied at
         registration. This script will not accept, store, or echo one.

    THE CAVEAT THAT DECIDES IT FOR THIS REPOSITORY
    ==============================================
    PDFE is a NATIVE WINDOWS DESKTOP APP and its test estate includes live-UI
    lanes driven by pywinauto/UIA against a real PDFE.exe window. Those need a
    genuine interactive desktop: this project has already recorded that a
    desktop-less RDP session leaves UIA unable to see the window
    (`ui_pytest_requires_bash_shell`), and that pywinauto menu enumeration hangs
    on this very host (`gap_a14_uia_menu_hang`). Options 2 and 3 both remove the
    desktop, so the durable-looking answers are the ones most likely to turn
    green UI lanes into hangs.

    Hence: **LogonTask**, plus the repaired watchdog for the crash case, plus
    the off-host alarm (.github/workflows/ci-liveness.yml) for the case where
    the whole machine is gone — because no on-host mechanism can report its own
    host being down.

    Full write-up:
    docs/audit/meta-audit-2026-07/followup/phase-44/runner-availability-recommendation.md

.PARAMETER Mechanism
    LogonTask (default/recommended), StartupTask, or Service.

.PARAMETER Apply
    Actually create the scheduled task. Without it this is a report. `Service`
    is never applied by this script — it prints the commands for the user to run.

.PARAMETER TaskName
    Scheduled-task name. Default 'PDFE-RunnerWatchdog'.

.PARAMETER RunnerDir
    Default C:\actions-runner.

    Moved 2026-10-03 with runner_watchdog.ps1's default, for the same reason: the
    runner was relocated off the user profile on 2026-10-02 and these two
    tools/ops scripts were the stragglers still naming the retired path. A
    scheduled task registered with the old default would have installed a
    watchdog that could not write its heartbeat on every boot.

.PARAMETER WatchdogPath
    Path to runner_watchdog.ps1. Defaults to the copy beside this script.

.PARAMETER UserId
    Account the task runs as. Defaults to the current user.

.EXAMPLE
    pwsh -NoProfile -File tools/ops/register_runner_service.ps1
    # DRY RUN: report current state and the exact commands that would run.

.EXAMPLE
    pwsh -NoProfile -File tools/ops/register_runner_service.ps1 -Mechanism LogonTask -Apply
    # Requires an elevated shell. USER ACTION — not performed by any audit phase.
#>
[CmdletBinding()]
param(
    [ValidateSet('LogonTask', 'StartupTask', 'Service')]
    [string] $Mechanism    = 'LogonTask',
    [switch] $Apply,
    [string] $TaskName     = 'PDFE-RunnerWatchdog',
    [string] $RunnerDir    = 'C:\actions-runner',
    [string] $WatchdogPath = '',
    [string] $UserId       = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Say {
    # [Console]::Out, not the success stream -- see the note in runner_watchdog.ps1.
    param([string] $Message = '')
    [Console]::Out.WriteLine($Message)
}

if (-not $WatchdogPath) {
    $WatchdogPath = Join-Path $PSScriptRoot 'runner_watchdog.ps1'
}
if (-not $UserId) {
    $UserId = "$env:USERDOMAIN\$env:USERNAME"
}

$pwshExe = (Get-Command pwsh -ErrorAction SilentlyContinue)
$shellExe = if ($pwshExe) { $pwshExe.Source } else { 'powershell.exe' }

Say '============================================================'
Say ' PDFE self-hosted runner autostart helper (F44 / backlog F46)'
Say '============================================================'
Say ''

# ---------------------------------------------------------------------------
# Observed state -- reported, never changed
# ---------------------------------------------------------------------------
Say '-- CURRENT STATE ------------------------------------------'
Say ("  runner directory      : {0}  (exists: {1})" -f $RunnerDir, (Test-Path -LiteralPath $RunnerDir))
Say ("  watchdog script       : {0}  (exists: {1})" -f $WatchdogPath, (Test-Path -LiteralPath $WatchdogPath))

$listener = $null
try { $listener = Get-Process -Name 'Runner.Listener' -ErrorAction SilentlyContinue } catch { }
if ($listener) {
    Say ("  Runner.Listener       : RUNNING (pid {0}) -- left strictly alone" -f (@($listener)[0].Id))
} else {
    Say '  Runner.Listener       : not running'
}

$svc = $null
try { $svc = Get-Service -Name 'actions.runner.*' -ErrorAction SilentlyContinue } catch { }
if ($svc) {
    foreach ($s in @($svc)) { Say ("  runner service        : {0} [{1}]" -f $s.Name, $s.Status) }
} else {
    Say '  runner service        : none registered (this is the F46 finding)'
}

$task = $null
try { $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue } catch { }
if ($task) {
    Say ("  scheduled task        : {0} [{1}]" -f $TaskName, $task.State)
} else {
    Say ("  scheduled task        : '{0}' not registered" -f $TaskName)
}

$hb = [IO.Path]::Combine($RunnerDir, 'watchdog-heartbeat.json')
if (Test-Path -LiteralPath $hb) {
    try {
        $beat = Get-Content -LiteralPath $hb -Raw -Encoding utf8 | ConvertFrom-Json
        Say ("  watchdog heartbeat    : {0} (iteration {1}, listener {2})" -f `
             $beat.timestampUtc, $beat.iteration, $beat.listenerState)
    } catch {
        Say '  watchdog heartbeat    : present but unreadable -- treat as STALE'
    }
} else {
    Say '  watchdog heartbeat    : ABSENT -- the repaired watchdog is not running'
}

$isAdmin = $false
try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $isAdmin = ([Security.Principal.WindowsPrincipal]$id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { }
Say ("  elevated shell        : {0}" -f $isAdmin)
Say ''

# ---------------------------------------------------------------------------
# The plan
# ---------------------------------------------------------------------------
$argString = ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -RunnerDir "{1}"' -f `
              $WatchdogPath, $RunnerDir)

Say ("-- PLAN: {0} ----------------------------------------" -f $Mechanism)
switch ($Mechanism) {
    'LogonTask' {
        Say '  Trigger : At log on of the account below'
        Say '  Context : Run ONLY when the user is logged on (interactive desktop kept)'
        Say ("  Account : {0}" -f $UserId)
        Say ("  Action  : {0} {1}" -f $shellExe, $argString)
        Say ''
        Say '  RECOMMENDED for this repository: PDFE is a native Windows desktop'
        Say '  app whose live-UI lanes drive a real window through UIA, and only'
        Say '  an interactive session provides the desktop those lanes need.'
        Say '  Residual gap, stated: this does NOT survive a reboot to a locked'
        Say '  logon screen. Console auto-logon closes that, is a real security'
        Say '  trade-off, and is deliberately NOT configured here.'
    }
    'StartupTask' {
        Say '  Trigger : At system startup'
        Say '  Context : Run whether the user is logged on or not (NO desktop)'
        Say ("  Account : {0}" -f $UserId)
        Say ("  Action  : {0} {1}" -f $shellExe, $argString)
        Say ''
        Say '  CAVEAT: no interactive desktop. Build/test lanes that need only a'
        Say '  console are fine; the pywinauto/UIA live-UI lanes are not.'
    }
    'Service' {
        Say '  This script does NOT apply the service option, by design.'
        Say '  It requires re-registering a runner that currently works, with a'
        Say '  fresh registration token and the service account password, and it'
        Say '  runs in session 0 with no desktop. Commands, for the user to run'
        Say '  deliberately and by hand:'
        Say ''
        Say ("      cd {0}" -f $RunnerDir)
        Say '      .\config.cmd remove --token <REMOVAL_TOKEN>'
        Say '      .\config.cmd --url https://github.com/ipswitch8/PDFE --token <REG_TOKEN> `'
        Say '                   --runasservice --windowslogonaccount <DOMAIN\USER>'
        Say '      .\svc.cmd install'
        Say '      .\svc.cmd start'
        Say ''
        Say '  Tokens and passwords are secrets: never paste them into this repo,'
        Say '  a script, a log, or a commit message.'
    }
}
Say ''

# ---------------------------------------------------------------------------
# Apply -- explicit opt-in only
# ---------------------------------------------------------------------------
if (-not $Apply) {
    Say '-- DRY RUN -------------------------------------------------'
    Say '  no changes were made. Re-run with -Apply (in an ELEVATED shell) to'
    Say '  register the scheduled task. Installing this on the live host is a'
    Say '  USER ACTION and is explicitly out of scope for follow-up phase 44.'
    exit 0
}

if ($Mechanism -eq 'Service') {
    Say 'ERROR: -Apply is not supported for -Mechanism Service. Run the commands'
    Say '       above by hand, having read the caveats.'
    exit 2
}
if (-not $isAdmin) {
    Say 'ERROR: -Apply needs an elevated (Administrator) shell. Nothing was changed.'
    exit 2
}
if (-not (Test-Path -LiteralPath $WatchdogPath)) {
    Say ("ERROR: watchdog script not found at {0}. Nothing was changed." -f $WatchdogPath)
    exit 2
}

$action = New-ScheduledTaskAction -Execute $shellExe -Argument $argString -WorkingDirectory $RunnerDir
if ($Mechanism -eq 'LogonTask') {
    $trigger   = New-ScheduledTaskTrigger -AtLogOn -User $UserId
    $principal = New-ScheduledTaskPrincipal -UserId $UserId -LogonType Interactive -RunLevel Highest
} else {
    $trigger   = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId $UserId -LogonType S4U -RunLevel Highest
}
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 5) `
                -ExecutionTimeLimit ([TimeSpan]::Zero)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings -Force | Out-Null

Say ("APPLIED: scheduled task '{0}' registered ({1})." -f $TaskName, $Mechanism)
Say '  The runner itself was not touched. Verify with:'
Say ("      Get-ScheduledTask -TaskName {0}" -f $TaskName)
Say ("      Get-Content '{0}' -Raw" -f ([IO.Path]::Combine($RunnerDir, 'watchdog-heartbeat.json')))
exit 0
