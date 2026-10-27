<#
.SYNOPSIS
    Register this machine as a self-hosted GitHub Actions runner for PDFE, and
    start it under the watchdog. Steps 4 and 5 of the deployment package.

.DESCRIPTION
    Install-PdfeRunner.ps1 provisions the prerequisites and then stops, because
    registration needs a credential. That left the last two steps as prose in a
    README: fetch a token from a URL you have to find, retype a `config.cmd`
    line with five labels in the right order, then remember that bare `run.cmd`
    is the wrong way to start it. Every one of those is a place to get it
    subtly wrong in a way that looks fine.

    This script does the parts a script can do and asks for exactly the one
    thing it cannot know.

    WHAT IT WILL NOT DO, AND WHY
    ----------------------------
    It will not store a registration token. Those expire in about an hour, so a
    stored one is a stale credential on disk that buys nothing -- the next
    registration needs a fresh token anyway. A Personal Access Token CAN be
    stored, because it is long-lived and can mint registration tokens on
    demand, but only with -SaveToken and only DPAPI-encrypted to this user on
    this machine.

    RESIDUAL RISK, stated rather than hidden
    ----------------------------------------
    `config.cmd` takes the token as a command-line argument. That means it is
    visible in the process table (`Get-CimInstance Win32_Process`) for the
    seconds the process lives, to any other account that can query it. There is
    no stdin path in the runner configurator, so this cannot be avoided while
    using the supported tool. It is acceptable because a registration token is
    single-use and short-lived, but on a shared machine you should know it, and
    it is a reason not to pass a long-lived PAT this way -- which is why a PAT
    is only ever used to mint a registration token over HTTPS, never handed to
    `config.cmd`.

.PARAMETER RunnerDir
    Where the runner is installed. Must already contain config.cmd.

.PARAMETER Repo
    owner/name. Defaults to the repository this package belongs to.

.PARAMETER Pat
    A Personal Access Token, to mint a registration token automatically. Prefer
    the interactive prompt; a value passed here is visible in your shell history.

.PARAMETER SaveToken
    DPAPI-encrypt the supplied PAT to this user on this machine so later runs do
    not re-prompt. Never applies to a registration token.

.PARAMETER Start
    After a successful registration, start the runner under the watchdog.

.PARAMETER WhatIf
    Print every decision and command without registering anything.

.EXAMPLE
    .\Register-PdfeRunner.ps1 -RunnerDir C:\actions-runner -Start

.NOTES
    Exit 0 registered (or already registered), 2 a setup/credential problem,
    3 the runner rejected the registration.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$RunnerDir = 'C:\actions-runner',
    [string]$Repo      = 'ipswitch8/PDFE',
    [string]$Pat,
    [switch]$SaveToken,
    [switch]$Start,
    [string]$RepoRoot  = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Say  { param($m, $c = 'Gray')    Write-Host $m -ForegroundColor $c }
function Head { param($m) Write-Host ''; Write-Host $m -ForegroundColor Cyan }
function Warn { param($m) Write-Host $m -ForegroundColor Yellow }

# `pdfe-builder` is NOT optional on a solo machine: windows-validation's `build`
# and `release` jobs are the only ones that request it, and GitHub does not
# error on a `runs-on` that no runner satisfies -- the job queues and then
# reports `cancelled`, which alerts nobody. A machine registered without it
# looks healthy and silently cannot build.
#
# PDFE-DEFAULT-LABELS -- the canonical default registration label set for this
# package, in the AUTOMATED path. tools/gate/check_label_drift.py reads the set
# from the NEXT `--labels` below and requires it to agree with
# runner-requirements.json, Install-PdfeRunner.ps1 and the package README.
# Exactly one marker may appear in this file, and it must sit immediately above
# the command it names.
$LabelsArg     = '--labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder'
$DefaultLabels = $LabelsArg -replace '^--labels\s+', ''

$SecretStore = Join-Path $env:LOCALAPPDATA 'PDFE\runner-pat.dpapi'

# ---------------------------------------------------------------------------
# Preflight. Everything checkable before a credential is requested is checked
# BEFORE it, so nobody fetches a 60-minute token and then discovers the runner
# directory is wrong.
# ---------------------------------------------------------------------------
Head 'PDFE runner registration -- preflight'

$configCmd = [IO.Path]::Combine($RunnerDir, 'config.cmd')
if (-not (Test-Path $configCmd)) {
    Write-Error @"
No config.cmd in $RunnerDir.

Either the runner archive was never downloaded there, or -RunnerDir is wrong.
Install-PdfeRunner.ps1 places it via the 'actions-runner' requirement; check
with:  .\Test-PdfeRunner.ps1 -RunnerDir $RunnerDir
"@
    exit 2
}
Say "  runner dir : $RunnerDir"
Say "  repo       : $Repo"
Say "  labels     : $DefaultLabels"

$alreadyConfigured = Test-Path ([IO.Path]::Combine($RunnerDir, '.runner'))
if ($alreadyConfigured) {
    Warn ''
    Warn 'This runner is ALREADY configured (.runner exists).'
    Warn 'Re-registering requires removing the old registration first:'
    Warn "    cd $RunnerDir; .\config.cmd remove --token <REMOVAL-TOKEN>"
    Warn ''
    Warn 'NOTE: .runner records the repository by ID, not by name. After a repo'
    Warn 'RENAME its gitHubUrl field still shows the OLD name while the runner'
    Warn 'works fine -- do not "fix" that by re-registering on the strength of'
    Warn 'the URL alone.'
    if (-not $Start) { exit 0 }
    Say ''
    Say 'Skipping registration; proceeding to start it, as -Start was given.'
}

# ---------------------------------------------------------------------------
# Credential. Two routes; the default is the one that stores nothing.
# ---------------------------------------------------------------------------
function Read-Plain {
    param([string]$Prompt)
    $sec = Read-Host -AsSecureString $Prompt
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try   { [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

function Get-SavedPat {
    if (-not (Test-Path $SecretStore)) { return $null }
    try {
        $sec = Get-Content $SecretStore -Raw | ConvertTo-SecureString
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
        try   { [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    } catch {
        # DPAPI is scoped to user+machine. A store copied from elsewhere, or
        # left by another account, cannot be decrypted -- and that is the
        # feature, not a fault. Say so instead of failing cryptically.
        Warn "Could not decrypt $SecretStore (DPAPI is per-user, per-machine). Ignoring it."
        $null
    }
}

function Save-Pat {
    param([string]$Value)
    $dir = Split-Path $SecretStore -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    ($Value | ConvertTo-SecureString -AsPlainText -Force | ConvertFrom-SecureString) |
        Set-Content -Path $SecretStore -Encoding ascii
    Say "  PAT saved, DPAPI-encrypted to this user on this machine:"
    Say "    $SecretStore"
    Say "  Delete that file to revoke local use. Revoking the PAT itself is a"
    Say "  separate action on GitHub."
}

function Get-RegistrationTokenFromPat {
    param([string]$PatValue)
    $uri = "https://api.github.com/repos/$Repo/actions/runners/registration-token"
    $headers = @{
        Authorization          = "Bearer $PatValue"
        Accept                 = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2022-11-28'
        'User-Agent'           = 'pdfe-runner-deployment'
    }
    try {
        $resp = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -TimeoutSec 30
        return $resp.token
    } catch {
        $status = $null
        try { $status = $_.Exception.Response.StatusCode.value__ } catch { }
        switch ($status) {
            401 { Write-Error "GitHub rejected the token (401). It is wrong, expired, or revoked." ; exit 2 }
            403 { Write-Error "GitHub returned 403. The token authenticated but lacks permission to mint a runner registration token for $Repo -- it needs administration:write on the repo (fine-grained) or the `repo` scope (classic)." ; exit 2 }
            404 { Write-Error "GitHub returned 404 for $Repo. Either the repo name is wrong or the token cannot see it -- a fine-grained PAT must explicitly list this repository." ; exit 2 }
            default {
                # Deliberately does not echo the exception body: an API error
                # response can contain the request, and the request carries the
                # Authorization header.
                Write-Error "Could not mint a registration token (HTTP $status). Check network egress to api.github.com."
                exit 2
            }
        }
    }
}

$regToken = $null

if (-not $alreadyConfigured) {
    Head 'Step 4 of 5 -- registration token'
    Say ''
    Say 'A runner registration token is short-lived (about one hour) and single'
    Say 'use. There are two ways to get one; the first stores nothing.'
    Say ''
    Say '  [1] Paste a registration token (recommended)' 'White'
    Say '      Open this page -- it is the runner-creation page for this repo:'
    Say ''
    Say "        https://github.com/$Repo/settings/actions/runners/new?arch=x64&os=win" 'Cyan'
    Say ''
    Say '      On that page:'
    Say '        a. leave the OS as Windows and the architecture as x64;'
    Say '        b. scroll to the "Configure" box;'
    Say '        c. the token is the value after --token in the ./config.cmd'
    Say '           line, a ~29-character string beginning with A;'
    Say '        d. copy JUST that token, not the whole command.'
    Say ''
    Say '      If you cannot see Settings, you are not an admin on the repo and'
    Say '      this route is not available to you.'
    Say ''
    Say '  [2] Paste a Personal Access Token instead' 'White'
    Say '      The script then mints a registration token over HTTPS, so the'
    Say '      long-lived credential is never handed to config.cmd. Needs'
    Say '      administration:write (fine-grained) or the repo scope (classic).'
    Say "      Create at: https://github.com/settings/tokens" 'Cyan'
    Say ''

    $savedPat = Get-SavedPat
    if ($Pat) {
        Say 'Using the -Pat value supplied on the command line.'
        $regToken = Get-RegistrationTokenFromPat -PatValue $Pat
        if ($SaveToken) { Save-Pat -Value $Pat }
    }
    elseif ($savedPat) {
        Say "Found a saved PAT at $SecretStore; using it to mint a registration token."
        $regToken = Get-RegistrationTokenFromPat -PatValue $savedPat
    }
    else {
        $choice = Read-Host 'Choose 1 (registration token) or 2 (PAT) [1]'
        if ([string]::IsNullOrWhiteSpace($choice)) { $choice = '1' }

        if ($choice -eq '2') {
            $patValue = Read-Plain 'Personal Access Token (input hidden)'
            if (-not $patValue) { Write-Error 'No token supplied.'; exit 2 }
            $regToken = Get-RegistrationTokenFromPat -PatValue $patValue
            Say '  PAT accepted; registration token minted.' 'Green'
            if ($SaveToken) { Save-Pat -Value $patValue }
            elseif (-not $WhatIfPreference) {
                Say '  (Not saved. Re-run with -SaveToken to avoid re-prompting.)'
            }
            $patValue = $null
        }
        else {
            $regToken = Read-Plain 'Registration token (input hidden)'
            if (-not $regToken) { Write-Error 'No token supplied.'; exit 2 }

            # Shape check only. A registration token CANNOT be validated without
            # consuming it -- there is no GitHub endpoint that says "is this
            # good?" -- so this catches the common paste mistakes and nothing
            # more. Do not read a pass here as "the token works".
            if ($regToken -match '^\s*(\./)?config\.cmd' -or $regToken -match '--token') {
                Write-Error 'That looks like the whole ./config.cmd line. Paste only the token that follows --token.'
                exit 2
            }
            if ($regToken -match '^gh[pousr]_') {
                Write-Error 'That is a Personal Access Token (it starts with gh*_), not a registration token. Re-run and choose option 2.'
                exit 2
            }
            if ($regToken.Length -lt 20) {
                Write-Error "That is only $($regToken.Length) characters; a registration token is around 29. Check the paste."
                exit 2
            }
            Say '  Token shape looks right (it cannot be verified without using it).' 'Green'
        }
    }
}

# ---------------------------------------------------------------------------
# Register.
# ---------------------------------------------------------------------------
if (-not $alreadyConfigured) {
    Head 'Registering'
    $shown = "config.cmd --url https://github.com/$Repo --token <REDACTED> $LabelsArg --unattended --replace"
    Say "  $shown"

    if ($PSCmdlet.ShouldProcess($RunnerDir, 'register the GitHub Actions runner')) {
        Push-Location $RunnerDir
        try {
            # TOKEN-ON-CMDLINE: ACCEPTED, and the only accepted site in the
            # package. config.cmd offers no stdin path (see the header), so the
            # registration token is briefly visible in the process table. It is
            # single-use, short-lived, never persisted, redacted in the echoed
            # command above, and cleared in the finally below.
            # tools/gate/test_runner_wizard_scripts.py allows exactly this one
            # annotated line; a second site must justify itself the same way.
            & $configCmd --url "https://github.com/$Repo" --token $regToken `
                         --labels $DefaultLabels --unattended --replace
            $rc = $LASTEXITCODE
        } finally {
            Pop-Location
            $regToken = $null
            [GC]::Collect()
        }
        if ($rc -ne 0) {
            Write-Error @"
config.cmd exited $rc.

The usual causes, in the order they actually happen:
  * the token expired -- they last about an hour, so fetch a fresh one;
  * the token was already used -- they are single use;
  * this machine already has a registration; remove it first with
    .\config.cmd remove --token <REMOVAL-TOKEN>.
"@
            exit 3
        }
        Say '  Registered.' 'Green'
    }
}

# ---------------------------------------------------------------------------
# Step 5. Start under the watchdog, never bare run.cmd.
# ---------------------------------------------------------------------------
Head 'Step 5 of 5 -- starting the runner'

if (-not $RepoRoot) {
    # The watchdog deliberately lives in the repo checkout, not in this package,
    # so there is exactly one of it. Find it rather than making the operator
    # supply a path they already gave the installer.
    $guess = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    if (Test-Path (Join-Path $guess 'tools\ops\runner_watchdog.ps1')) { $RepoRoot = $guess }
}
$watchdog = if ($RepoRoot) { [IO.Path]::Combine($RepoRoot, 'tools\ops\runner_watchdog.ps1') } else { $null }

Say ''
Say 'Do NOT start it with bare run.cmd, and do NOT install it as a service:'
Say '  * the UI suites need an INTERACTIVE desktop session, which a service'
Say '    does not have -- as a service they fail in ways that look like product'
Say '    bugs;'
Say '  * bare run.cmd dies silently, and a dead runner is indistinguishable from'
Say '    an idle one until a job queues and reports cancelled.'
Say ''

if ($watchdog -and (Test-Path $watchdog)) {
    Say "  watchdog: $watchdog"
    if ($Start) {
        if ($PSCmdlet.ShouldProcess($watchdog, 'start the runner under the watchdog')) {
            & $watchdog -RunnerDir $RunnerDir
        }
    } else {
        Say ''
        Say '  Start it with:' 'White'
        Say "    $watchdog -RunnerDir $RunnerDir" 'Cyan'
        Say ''
        Say '  Or re-run this script with -Start.'
    }
} else {
    Warn "  Could not locate tools\ops\runner_watchdog.ps1."
    Warn "  Pass -RepoRoot <path-to-PDFE-checkout>, or start it by hand from there."
    Warn "  Do not substitute bare run.cmd -- see above."
}

Head 'Next'
Say "  Confirm the runner appears, and is IDLE, at:"
Say "    https://github.com/$Repo/settings/actions/runners" 'Cyan'
Say '  A runner that registered but never came online shows as Offline there.'
Say ''
exit 0
