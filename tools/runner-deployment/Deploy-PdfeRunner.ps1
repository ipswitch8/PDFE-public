<#
.SYNOPSIS
    One command that takes a fresh Windows box to a runner picking up PDFE jobs.

.DESCRIPTION
    The package was a correct set of parts and a README telling you the order to
    run them in. That is a checklist, and a checklist executed by a person at
    11pm is where deployments go wrong: a step skipped, a step run in the wrong
    order, or the last one forgotten entirely so the machine looks provisioned
    and serves nothing.

    This drives the whole sequence. It ORCHESTRATES the existing scripts rather
    than reimplementing them -- Install, Test, Register, the watchdog and the
    autostart helper each remain the single definition of their own job, and
    each can still be run alone. Duplicating any of them here would recreate the
    drift class the label guard exists to prevent.

    THE DEPENDENCY CYCLE IT BREAKS
    ------------------------------
    Install-PdfeRunner's `python-ui-deps` requirement installs from a pinned
    requirements file that lives IN THE REPO CHECKOUT. But `gh`, which is how
    you clone a private repo, is itself one of the things the installer
    provides. Run naively, the install fails on a checkout that does not exist
    yet, or you clone by hand before you have the tool to clone with.

    So the wizard installs in two passes: everything except `python-ui-deps`,
    then authenticate, then clone, then that one requirement. The README's
    linear ordering does not mention this because a human doing it by hand
    stumbles into the right order by trial and error.

    WHAT IT STILL ASKS YOU FOR
    --------------------------
    Only what a script genuinely cannot know or decide:

      * GitHub authentication, if `gh` is not already logged in;
      * the runner registration token (Register-PdfeRunner explains where);
      * Cloudflare R2 credentials, if you want crash-dump upload (optional);
      * whether to install an autostart mechanism, because that changes how the
        machine behaves at logon and is not a decision to take on someone's
        behalf.

    Everything else is detected, decided or installed.

.PARAMETER RepoRoot
    Where the PDFE checkout lives, or should be cloned to.

.PARAMETER RunnerDir
    Where the Actions runner lives.

.PARAMETER RunnerProfile
    'build' (the default, and the only correct one for a solo machine) or
    'consume' for a multi-machine fleet.

.PARAMETER SkipR2
    Do not offer Cloudflare R2 configuration. Crash dumps stay local.

.PARAMETER Unattended
    Take the safe default at every optional prompt instead of asking. Does NOT
    skip the registration token -- nothing can invent that.

.PARAMETER WhatIf
    Walk the whole sequence, print every decision and command, change nothing.

.EXAMPLE
    .\Deploy-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -RunnerDir C:\actions-runner

.EXAMPLE
    .\Deploy-PdfeRunner.ps1 -WhatIf        # see the plan before running it

.NOTES
    Exit 0 complete, 1 finished with steps still outstanding (it says which),
    2 a precondition this script will not work around.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$RepoRoot   = 'C:\src\PDFE',
    [string]$RunnerDir  = 'C:\actions-runner',
    [string]$VcpkgRoot  = 'C:\vcpkg',
    [ValidateSet('build', 'consume')]
    [string]$RunnerProfile = 'build',
    [string]$Repo       = 'ipswitch8/PDFE',
    [switch]$SkipR2,
    [switch]$Unattended
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$script:Outstanding = @()

function Say  { param($m, $c = 'Gray') Write-Host $m -ForegroundColor $c }
function Head {
    param($n, $of, $m)
    Write-Host ''
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
    Write-Host ("  STEP $n of $of -- $m") -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
}
function Warn    { param($m) Write-Host $m -ForegroundColor Yellow }
function Good    { param($m) Write-Host $m -ForegroundColor Green }
function Defer   { param($m) $script:Outstanding += $m; Warn "  DEFERRED: $m" }

function Ask-YesNo {
    param([string]$Question, [bool]$DefaultYes = $true)
    if ($Unattended) {
        Say "  $Question -> $(if ($DefaultYes) {'yes'} else {'no'}) (unattended)"
        return $DefaultYes
    }
    $suffix = if ($DefaultYes) { '[Y/n]' } else { '[y/N]' }
    $a = Read-Host "  $Question $suffix"
    if ([string]::IsNullOrWhiteSpace($a)) { return $DefaultYes }
    return $a -match '^[Yy]'
}

# Run a child script and surface its exit code. Uses & rather than Start-Process
# so the child's output lands in this console in real time -- a wizard that
# swallows the installer's progress is worse than the checklist it replaces.
function Invoke-Step {
    param([string]$Script, [string[]]$Arguments, [string]$What)
    Say "  > $Script $($Arguments -join ' ')" 'DarkGray'
    if (-not $PSCmdlet.ShouldProcess($What, 'run')) { return 0 }
    & $Script @Arguments
    return $LASTEXITCODE
}

$TOTAL = 8

# ---------------------------------------------------------------------------
Head 1 $TOTAL 'Environment preconditions'
# ---------------------------------------------------------------------------
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$id).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$interactive = [Environment]::UserInteractive

Say "  user            : $($id.Name)"
Say "  elevated        : $isAdmin"
Say "  interactive     : $interactive"
Say "  PowerShell      : $($PSVersionTable.PSVersion)"
Say "  repo checkout   : $RepoRoot"
Say "  runner dir      : $RunnerDir"
Say "  profile         : $RunnerProfile"

if (-not $interactive) {
    Write-Error @"
This session is not interactive.

The UI suites drive a real desktop through UIA, so the runner must live in an
interactive session -- and so must this deployment, because several installers
show UI. Run it from a console session on the machine, not from a service, a
remote-exec tool, or an RDP session you intend to disconnect.
"@
    exit 2
}
# -WhatIf changes nothing, so elevation is irrelevant to it -- and previewing
# the plan BEFORE opening an elevated shell is the natural order. Gating the dry
# run on elevation made the one command you would reach for first refuse to run.
if (-not $isAdmin -and $WhatIfPreference) {
    Warn ''
    Warn '  Not elevated -- fine for -WhatIf, which changes nothing. A real run'
    Warn '  needs an elevated, interactive PowerShell.'
}
elseif (-not $isAdmin) {
    Warn ''
    Warn '  NOT ELEVATED. Several installers need admin, and the unelevated'
    Warn '  fallbacks in this package are the least-exercised code in it.'
    Warn '  Strongly prefer re-running from an elevated PowerShell.'
    if (-not (Ask-YesNo 'Continue unelevated anyway?' $false)) {
        Say ''
        Say '  Re-run from an elevated PowerShell:' 'White'
        Say "    Start-Process powershell -Verb RunAs -ArgumentList '-NoExit','-File','$PSCommandPath'" 'Cyan'
        exit 2
    }
    Defer 'ran unelevated -- re-verify anything that reported a fallback path'
}

# ---------------------------------------------------------------------------
Head 2 $TOTAL 'Installer URLs still resolve'
# ---------------------------------------------------------------------------
Say '  Checking the pinned installers BEFORE installing anything, so a dead'
Say '  URL fails here rather than half way through a provision.'
$rc = Invoke-Step (Join-Path $Here 'Test-InstallerUrls.ps1') @() 'installer URL probe'
if ($rc -ne 0) {
    Warn '  One or more pinned installers did not resolve.'
    if (-not (Ask-YesNo 'Continue anyway? (the affected requirement will fail later)' $false)) { exit 2 }
    Defer 'a pinned installer URL did not resolve -- re-run Test-InstallerUrls.ps1'
}

# ---------------------------------------------------------------------------
Head 3 $TOTAL 'Install prerequisites (pass 1 -- everything but the pinned pip deps)'
# ---------------------------------------------------------------------------
# python-ui-deps installs FROM the checkout, and the checkout needs gh, which
# this pass provides. Split deliberately; see the header.
Say '  Skipping python-ui-deps in this pass: it installs from a requirements'
Say '  file inside the checkout, and the checkout needs gh, which this pass'
Say '  is what provides. It is installed in step 5, after the clone.'
$rc = Invoke-Step (Join-Path $Here 'Install-PdfeRunner.ps1') `
        @('-RepoRoot', $RepoRoot, '-VcpkgRoot', $VcpkgRoot,
          '-RunnerProfile', $RunnerProfile, '-Skip', 'python-ui-deps') `
        'prerequisite install (pass 1)'
if ($rc -ne 0) {
    Warn "  Installer exited $rc. Some requirements did not install."
    if (-not (Ask-YesNo 'Continue and let the verify step show exactly what is missing?' $true)) { exit 1 }
    Defer "Install-PdfeRunner pass 1 exited $rc"
}

# ---------------------------------------------------------------------------
Head 4 $TOTAL 'GitHub authentication and the repository checkout'
# ---------------------------------------------------------------------------
$needCheckout = -not (Test-Path ([IO.Path]::Combine($RepoRoot, '.git')))
if (-not $needCheckout) {
    Good "  Checkout already present at $RepoRoot"
} else {
    Say "  No checkout at $RepoRoot. It is needed for the pinned pip"
    Say '  requirements and for tools/ops/runner_watchdog.ps1.'

    $ghOk = $false
    if (Get-Command gh -ErrorAction SilentlyContinue) {
        & gh auth status 2>&1 | Out-Null
        $ghOk = ($LASTEXITCODE -eq 0)
    }
    if (-not $ghOk -and $env:GITHUB_TOKEN) {
        Say '  gh is not logged in, but GITHUB_TOKEN is set; git will use it.'
        $ghOk = $true
    }
    if (-not $ghOk) {
        Warn ''
        Warn '  GitHub authentication is required: the repository is PRIVATE, so an'
        Warn '  unauthenticated clone returns 404 rather than an obvious auth error.'
        Say ''
        Say '  Two ways, either is fine:'
        Say '    a. gh auth login          -- interactive, opens a browser or device code'
        Say '    b. $env:GITHUB_TOKEN=...  -- a PAT with read access to the repo'
        Say ''
        if ((Get-Command gh -ErrorAction SilentlyContinue) -and
            (Ask-YesNo 'Run `gh auth login` now?' $true)) {
            if ($PSCmdlet.ShouldProcess('gh', 'interactive login')) { & gh auth login }
            & gh auth status 2>&1 | Out-Null
            $ghOk = ($LASTEXITCODE -eq 0)
        }
    }
    if ($ghOk) {
        $rc = Invoke-Step 'gh' @('repo', 'clone', $Repo, $RepoRoot) 'repository clone'
        if ($rc -ne 0) { Defer "cloning $Repo into $RepoRoot failed (gh exit $rc)" }
        else { Good "  Cloned $Repo -> $RepoRoot" }
    } else {
        Defer "no GitHub auth -- clone $Repo into $RepoRoot by hand, then re-run"
    }
}

# ---------------------------------------------------------------------------
Head 5 $TOTAL 'Install prerequisites (pass 2 -- the pinned pip deps)'
# ---------------------------------------------------------------------------
if (Test-Path ([IO.Path]::Combine($RepoRoot, 'tests\ui_automation\requirements-locked.txt'))) {
    $rc = Invoke-Step (Join-Path $Here 'Install-PdfeRunner.ps1') `
            @('-RepoRoot', $RepoRoot, '-VcpkgRoot', $VcpkgRoot,
              '-RunnerProfile', $RunnerProfile) `
            'prerequisite install (pass 2)'
    if ($rc -ne 0) { Defer "Install-PdfeRunner pass 2 exited $rc" }
} else {
    Defer 'pinned UI requirements not found in the checkout -- pass 2 skipped'
}

# ---------------------------------------------------------------------------
Head 6 $TOTAL 'Verify, and prove the UIA stack actually works'
# ---------------------------------------------------------------------------
Say "  The installer's own report is not evidence. This re-checks the machine."
$rc = Invoke-Step (Join-Path $Here 'Test-PdfeRunner.ps1') `
        @('-RepoRoot', $RepoRoot, '-RunnerDir', $RunnerDir,
          '-RunnerProfile', $RunnerProfile) 'verification'
$verifyOk = ($rc -eq 0)
if (-not $verifyOk) {
    Warn ''
    Warn '  Verification reported failures (listed above).'
    Warn '  Registering a machine in this state produces jobs that fail deep into'
    Warn '  a run, or report cancelled and alert nobody.'
    if (-not (Ask-YesNo 'Continue to registration anyway?' $false)) {
        Defer 'verification failed -- fix the items above, then re-run'
        Say ''
        Say 'Stopping here, with nothing registered.' 'Yellow'
        exit 1
    }
    Defer 'registered despite failing verification'
}

Say ''
Say '  UIA smoke test (5 seconds, no build needed):'
if ($PSCmdlet.ShouldProcess('smoke_uia.py', 'run')) {
    & python (Join-Path $Here 'smoke_uia.py')
    if ($LASTEXITCODE -ne 0) {
        Defer 'the UIA smoke test failed -- UI suites will fail on this machine'
        Warn '  A locked session is the usual cause: UIA cannot see a locked desktop.'
    } else { Good '  UIA stack works.' }
}

# ---------------------------------------------------------------------------
Head 7 $TOTAL 'Cloudflare R2 (crash-dump upload) -- optional'
# ---------------------------------------------------------------------------
if ($SkipR2) {
    Say '  Skipped (-SkipR2). Crash dumps stay on this machine.'
} elseif ($env:PDFE_R2_BUCKET) {
    Good "  Already configured (PDFE_R2_BUCKET=$env:PDFE_R2_BUCKET)."
} else {
    Say '  Without this, crash dumps stay local and a crash on this runner'
    Say '  cannot be triaged from elsewhere. It is optional, not required.'
    Say ''
    Say '  NOTE: never point this at a bucket whose credentials can WRITE if this'
    Say '  is a consume-only machine -- see the README on read-only tokens.'
    if (Ask-YesNo 'Configure Cloudflare R2 now?' $false) {
        $rc = Invoke-Step (Join-Path $Here 'Set-R2Config.ps1') `
                @('-RunnerProfile', $RunnerProfile) 'R2 configuration'
        if ($rc -ne 0) { Defer "Set-R2Config exited $rc" }
    } else {
        Defer 'R2 not configured -- crash dumps will stay on this machine'
    }
}

# ---------------------------------------------------------------------------
Head 8 $TOTAL 'Register, arrange autostart, and start'
# ---------------------------------------------------------------------------
$regArgs = @('-RunnerDir', $RunnerDir, '-Repo', $Repo, '-RepoRoot', $RepoRoot)
$rc = Invoke-Step (Join-Path $Here 'Register-PdfeRunner.ps1') $regArgs 'registration'
if ($rc -ne 0) {
    Defer "registration exited $rc -- re-run .\Register-PdfeRunner.ps1 when ready"
} else {
    Say ''
    Say '  AUTOSTART. Precisely: start-on-LOGON, not start-on-boot. With no'
    Say '  autologon on these machines the distinction is the whole story -- the'
    Say '  task fires when a human signs in, and cannot fire before that because'
    Say '  there is no desktop to start into. The helper is DRY-RUN by default'
    Say '  because this changes how the machine behaves at logon.'
    Say ''
    Say '  Prefer a LOGON task over a service: a service has no interactive'
    Say '  desktop, and every UI suite fails there in ways that look like product'
    Say '  bugs rather than like a misconfigured host.'
    $svc = Join-Path (Split-Path (Split-Path $Here -Parent) -Parent) 'tools\ops\register_runner_service.ps1'
    if (-not (Test-Path $svc) -and $RepoRoot) {
        $svc = [IO.Path]::Combine($RepoRoot, 'tools\ops\register_runner_service.ps1')
    }
    if (Test-Path $svc) {
        if (Ask-YesNo 'Install a logon task so the runner starts when you sign in?' $true) {
            $rc2 = Invoke-Step $svc @('-Mechanism', 'LogonTask', '-RunnerDir', $RunnerDir, '-Apply') 'autostart task'
            if ($rc2 -ne 0) { Defer "autostart helper exited $rc2" }
        } else {
            Defer 'no autostart configured -- the runner will not survive a reboot'
        }
        # NO AUTOLOGON ON THESE MACHINES. That is a standing constraint, so
        # the reboot story is: the box comes up at the sign-in screen, a human
        # signs in, and the LOGON TASK starts the runner in that session. The
        # task's trigger is the interactive logon itself, so it works fine
        # without autologon -- what it cannot do is conjure a desktop nobody has
        # signed in to create.
        #
        # An earlier version of this block told the operator to enable
        # autologon. That was advice they cannot take, and a deferred item that
        # can never be cleared teaches people to ignore the whole list.
        Say ''
        Say '  AFTER A REBOOT, on these machines:' 'White'
        Say '    1. someone signs in at the console (no autologon here, by policy);'
        Say '    2. the logon task starts the runner in that session;'
        Say '    3. if it does not, double-click "Start PDFE Runner" on the Desktop.'
        Say ''
        Say '  Until someone signs in there is no desktop, so the UI lanes have'
        Say '  nothing to drive. No on-host mechanism can change that.'
        Say ''
        if (Ask-YesNo 'Create the Desktop / Start Menu shortcut for manual start?' $true) {
            $rc4 = Invoke-Step (Join-Path $Here 'Start-PdfeRunner.ps1') `
                    @('-RunnerDir', $RunnerDir, '-RepoRoot', $RepoRoot, '-CreateShortcut') `
                    'manual-start shortcut'
            if ($rc4 -ne 0) { Defer "creating the start shortcut exited $rc4" }
        } else {
            Defer 'no manual-start shortcut -- the operator must remember the command'
        }
        Say ''
        Say '  Leave the session UNLOCKED while the runner is up: a locked'
        Say '  workstation is invisible to UIA, so the UI lanes fail while the'
        Say '  runner reports online -- which reads as a product bug, not a host one.'
    } else {
        Defer "register_runner_service.ps1 not found -- arrange autostart by hand"
    }

    Say ''
    if (Ask-YesNo 'Start the runner under the watchdog now?' $true) {
        $rc3 = Invoke-Step (Join-Path $Here 'Register-PdfeRunner.ps1') ($regArgs + '-Start') 'runner start'
        if ($rc3 -ne 0) { Defer "starting the runner exited $rc3" }
    } else {
        Defer 'runner not started -- .\Register-PdfeRunner.ps1 -Start when ready'
    }
}

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host ("=" * 78) -ForegroundColor DarkCyan
if ($script:Outstanding.Count -eq 0) {
    Good '  DEPLOYMENT COMPLETE -- nothing outstanding.'
    Say ''
    Say "  Confirm the runner is IDLE (not Offline) at:"
    Say "    https://github.com/$Repo/settings/actions/runners" 'Cyan'
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
    exit 0
}
Warn "  DEPLOYMENT FINISHED WITH $($script:Outstanding.Count) ITEM(S) OUTSTANDING"
Write-Host ''
$i = 1
foreach ($o in $script:Outstanding) { Warn ("   {0}. {1}" -f $i, $o); $i++ }
Write-Host ''
Say '  Re-running this script is safe: every step detects work already done.'
Say "  Confirm runner state at https://github.com/$Repo/settings/actions/runners"
Write-Host ("=" * 78) -ForegroundColor DarkCyan
exit 1
