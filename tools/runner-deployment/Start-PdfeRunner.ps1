<#
.SYNOPSIS
    Start the PDFE runner in THIS interactive desktop session, and optionally
    create the shortcuts that make that a double-click.

.DESCRIPTION
    These machines cannot use autologon. That is a deliberate constraint, not an
    oversight, and it decides the whole availability story:

      * after a reboot the box sits at the sign-in screen with no desktop;
      * a human signs in;
      * the runner starts in THAT session, and only then can the UI lanes see a
        real desktop.

    Nothing on the machine can shorten that, because the desktop does not exist
    until someone logs on. What this script does is make the part after the
    sign-in a single obvious action instead of a remembered command.

    THE TWO ROUTES, AND WHY BOTH
    ----------------------------
    A LOGON TASK (tools/ops/register_runner_service.ps1 -Mechanism LogonTask)
    fires automatically when the user signs in, so on a normal reboot-and-login
    nobody has to do anything. It does NOT require autologon -- the trigger is
    the human's interactive logon, which is exactly the case here.

    A SHORTCUT covers everything the task does not: the task was never
    installed, someone stopped the runner, the watchdog was killed, or you are
    signing in as a different account. `-CreateShortcut` puts "Start PDFE
    Runner" on the Desktop and in the Start Menu.

    Install both. The task is the normal path; the shortcut is the one you want
    to exist at 8am when the task did not fire and nobody remembers the command.

.PARAMETER RunnerDir
    The Actions runner directory.

.PARAMETER RepoRoot
    The PDFE checkout, which is where the watchdog lives.

.PARAMETER CreateShortcut
    Create/refresh the Desktop and Start Menu shortcuts, then exit without
    starting anything.

.PARAMETER Force
    Start even if a runner process already appears to be running.

.EXAMPLE
    .\Start-PdfeRunner.ps1 -CreateShortcut     # set up the double-click
.EXAMPLE
    .\Start-PdfeRunner.ps1                     # start it now, in this session

.NOTES
    Exit 0 started (or shortcuts created), 1 already running, 2 a precondition
    that would make the runner useless if ignored.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$RunnerDir = 'C:\actions-runner',
    [string]$RepoRoot  = '',
    [switch]$CreateShortcut,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Say  { param($m, $c = 'Gray') Write-Host $m -ForegroundColor $c }
function Warn { param($m) Write-Host $m -ForegroundColor Yellow }
function Good { param($m) Write-Host $m -ForegroundColor Green }

# --- locate the watchdog ----------------------------------------------------
# It lives in the repo checkout on purpose: there is exactly one of it, and a
# second copy shipped inside this package was deleted rather than maintained.
if (-not $RepoRoot) {
    $guess = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    if (Test-Path (Join-Path $guess 'tools\ops\runner_watchdog.ps1')) { $RepoRoot = $guess }
}
$watchdog = if ($RepoRoot) { [IO.Path]::Combine($RepoRoot, 'tools\ops\runner_watchdog.ps1') } else { '' }

# ---------------------------------------------------------------------------
if ($CreateShortcut) {
    if (-not $watchdog -or -not (Test-Path $watchdog)) {
        Write-Error "Cannot find tools\ops\runner_watchdog.ps1. Pass -RepoRoot <checkout>."
        exit 2
    }
    # -WindowStyle Normal, NOT Hidden. A hidden runner is indistinguishable from
    # a runner that never started, and this whole design exists because someone
    # has to be able to SEE that the machine is serving jobs.
    $target    = (Get-Command powershell.exe).Source
    $arguments = '-NoExit -NoProfile -ExecutionPolicy Bypass -File "{0}" -RunnerDir "{1}"' -f $watchdog, $RunnerDir

    $targets = @(
        (Join-Path ([Environment]::GetFolderPath('Desktop'))    'Start PDFE Runner.lnk'),
        (Join-Path ([Environment]::GetFolderPath('StartMenu'))  'Programs\Start PDFE Runner.lnk')
    )
    $shell = New-Object -ComObject WScript.Shell
    foreach ($lnk in $targets) {
        $dir = Split-Path $lnk -Parent
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        if ($PSCmdlet.ShouldProcess($lnk, 'create shortcut')) {
            $s = $shell.CreateShortcut($lnk)
            $s.TargetPath       = $target
            $s.Arguments        = $arguments
            $s.WorkingDirectory = $RunnerDir
            $s.Description      = 'Start the PDFE self-hosted Actions runner in this desktop session'
            $s.IconLocation     = "$target,0"
            $s.Save()
        }
        Good "  shortcut: $lnk"
    }
    Say ''
    Say '  After signing in, double-click "Start PDFE Runner".'
    Say '  Leave the window OPEN and the session UNLOCKED -- a locked desktop is'
    Say '  invisible to UIA, so the UI lanes fail while the runner reports online.'
    exit 0
}

# ---------------------------------------------------------------------------
Say 'Starting the PDFE runner in this session'
Say ''

if (-not [Environment]::UserInteractive) {
    Write-Error @"
This session is not interactive, so the runner started here would have no
desktop and every UI lane would fail while the runner reported healthy.

Sign in at the console (or an RDP session you will leave CONNECTED) and run this
again there.
"@
    exit 2
}

if (-not (Test-Path ([IO.Path]::Combine($RunnerDir, '.runner')))) {
    Write-Error @"
$RunnerDir is not a registered runner (.runner is missing).

Register it first:
    .\Register-PdfeRunner.ps1 -RunnerDir $RunnerDir
"@
    exit 2
}

if (-not $watchdog -or -not (Test-Path $watchdog)) {
    Write-Error @"
Cannot find tools\ops\runner_watchdog.ps1.

Pass -RepoRoot <path-to-PDFE-checkout>. Do not substitute bare run.cmd: it dies
silently, and a dead runner looks exactly like an idle one until a job queues
and reports cancelled.
"@
    exit 2
}

$running = @(Get-Process -Name 'Runner.Listener' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0 -and -not $Force) {
    Warn "  A runner already appears to be running (Runner.Listener, PID $($running[0].Id))."
    Warn '  Nothing to do. Use -Force to start another anyway (you almost certainly'
    Warn '  do not want two listeners against the same registration).'
    exit 1
}

Say "  runner dir : $RunnerDir"
Say "  watchdog   : $watchdog"
Say ''
Say '  Leave this window OPEN. Closing it stops the runner.'
Say '  Do NOT lock the session -- a locked desktop is invisible to UIA.'
Say ''

if ($PSCmdlet.ShouldProcess($RunnerDir, 'start the runner under the watchdog')) {
    & $watchdog -RunnerDir $RunnerDir
}
exit 0
