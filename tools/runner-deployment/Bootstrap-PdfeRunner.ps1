<#
.SYNOPSIS
    Fetch the pdfe-windows runner deployment package from GitHub onto a clean
    machine, then hand off to Install-PdfeRunner.ps1.

.DESCRIPTION
    This is the ONE file you copy to a new machine by hand. It retrieves the
    rest of tools/runner-deployment/ from the repository.

    THE REPOSITORY IS PRIVATE. That is the fact that shapes this script, and it
    is worth stating plainly because the obvious approach silently does not
    work: the usual

        iwr -useb https://raw.githubusercontent.com/<org>/<repo>/<branch>/...  | iex

    one-liner returns 404 for a private repo, and `| iex` on a 404 body is a
    confusing failure rather than an obvious one. So this script authenticates,
    and it downloads to disk before running anything.

    HOW IT AUTHENTICATES, in order of preference:
      1. `gh` CLI already logged in (gh auth status) -- preferred; no token is
         ever written to disk or command history by this script.
      2. $env:GITHUB_TOKEN / $env:GH_TOKEN -- a PAT with `repo` read scope.
      3. Nothing -- the script stops and tells you, rather than falling back to
         an unauthenticated fetch that would 404.

    WHAT IT PINS, and why. By default it fetches a specific COMMIT, not a
    branch. A branch moves; a commit does not. Deploying two runners a week
    apart from `master` can silently give them different tooling, and the
    resulting "works on runner A, fails on runner B" is expensive to chase.
    Pass -Ref to deliberately take something else.

    WHAT IT DOES NOT DO. It does not install anything itself, and it does not
    pipe downloaded content straight into the shell. It writes files, prints
    their SHA-256, and tells you the next command. You can read what arrived
    before you run it.

.PARAMETER Destination
    Where to place the package. Default C:\pdfe-runner-deploy.

.PARAMETER Ref
    Commit SHA, tag or branch to fetch. Defaults to the pinned commit below.

.PARAMETER Repo
    owner/name. Default ipswitch8/PDFE.

.PARAMETER Run
    Also invoke Install-PdfeRunner.ps1 after a successful fetch. Off by default:
    fetching and executing are separate decisions, and you should be able to
    look at what landed first.

.EXAMPLE
    # On the new machine, from an elevated interactive PowerShell:
    .\Bootstrap-PdfeRunner.ps1
    # ...read what it fetched, then:
    C:\pdfe-runner-deploy\Install-PdfeRunner.ps1 -RepoRoot C:\src\PDFE

.EXAMPLE
    # Take the current tip of master instead of the pinned commit:
    .\Bootstrap-PdfeRunner.ps1 -Ref master
#>
[CmdletBinding()]
param(
    [string]$Destination = 'C:\pdfe-runner-deploy',
    # PINNED COMMIT. Update this when the package changes in a way new runners
    # must pick up. Leaving it stale is safer than tracking a moving branch:
    # a stale pin deploys a known-good older package, whereas a moving branch
    # can deploy something nobody has run yet.
    #
    # The pin necessarily lags THIS file's own commit by one -- a commit cannot
    # contain its own SHA. That is harmless here because this file is copied to
    # the new machine by hand, not fetched; the pin only governs the files it
    # downloads.
    #
    # THE PIN IS THE `$Ref` VALUE BELOW AND NOWHERE ELSE. This comment used to
    # open by naming the current SHA, which meant two sources of truth for one
    # value -- and the prose one went stale immediately, because every advance
    # updates the assignment and nothing updates the sentence. It sat four
    # advances behind before anyone noticed, quietly describing a pin that had
    # not been current for days. Removed rather than corrected: a duplicate that
    # nothing checks will drift again, and the fix is to stop duplicating.
    #
    # It matters that the pin is kept CURRENT, not merely valid. An early pin
    # predated every fix made to these scripts afterwards, so a machine
    # bootstrapped from it would have received
    # Install-PdfeRunner.ps1 with three shape checks that never fired (Set-
    # StrictMode turns `$obj.missing` into an error, not $null, so StrictMode
    # threw before the check could), a URL probe that calls a good installer
    # dead on one TCP timeout, and no Test-InstallerDryRun.py at all. A stale
    # pin is not a safe default -- it is a silently OLD deployment.
    #
    # Anything earlier than c136529 additionally still used winget (unreachable
    # on Server 2019) and NSIS-style silent args for an Inno installer, which
    # hangs on the interactive wizard until the job cap.
    #
    # 2026-09-02: THE PIN WAS CLOBBERED BY A DOCS TOOL, not by neglect.
    # fed52797 advanced it past the label-drift guard; 8cc495be -- an ad-hoc
    # bulk remap of the SHA citations invalidated by the account-name history
    # rewrite -- then rewrote it BACKWARDS to 5f7680a, because a bare 7-hex
    # string in a .ps1 is indistinguishable from a documentation citation to a
    # regex. It survived a month unnoticed: the package files happened not to
    # change in between, so nothing broke and nothing complained.
    #
    # That is the failure mode to keep in mind. A wrong pin is invisible until
    # the package next changes, and then it is invisible in the other direction
    # too -- the new machine reports a completely successful bootstrap while
    # fetching the previous generation of these scripts. If you ever run a
    # history rewrite or a bulk SHA remap again, CHECK THIS LINE afterwards.
    [string]$Ref = '233391dc',
    [string]$Repo = 'ipswitch8/PDFE',
    [switch]$Run
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$PackagePath = 'tools/runner-deployment'
# Kept in sync with the directory listing; the fetch VERIFIES this list arrived
# rather than trusting whatever the API happened to return.
$ExpectedFiles = @(
    'runner-requirements.json',
    'Deploy-PdfeRunner.ps1',
    'Install-PdfeRunner.ps1',
    'Register-PdfeRunner.ps1',
    'Start-PdfeRunner.ps1',
    'Test-PdfeRunner.ps1',
    'Test-InstallerUrls.ps1',
    'Test-InstallerUrls.Mutations.ps1',
    'Test-InstallerDryRun.py',
    'Set-R2Config.ps1',
    'Test-SetR2Config.py',
    # The TEMPLATE, not a .env. A real .env must never be fetched from the
    # repository -- it is git-ignored precisely so it never gets there.
    '.env.example',
    'smoke_uia.py',
    'README.md'
)

Write-Host "pdfe-windows runner bootstrap" -ForegroundColor Cyan
Write-Host "  repo   : $Repo (PRIVATE -- authentication required)"
Write-Host "  ref    : $Ref"
Write-Host "  dest   : $Destination"
Write-Host ""

# --- authentication ---------------------------------------------------------
$useGh = $false
$token = $null

$ghCmd = Get-Command gh -ErrorAction SilentlyContinue
if ($ghCmd) {
    & gh auth status *> $null
    if ($LASTEXITCODE -eq 0) {
        $useGh = $true
        Write-Host "auth: gh CLI is logged in" -ForegroundColor Green
    }
}
if (-not $useGh) {
    $token = if ($env:GITHUB_TOKEN) { $env:GITHUB_TOKEN } elseif ($env:GH_TOKEN) { $env:GH_TOKEN } else { $null }
    if ($token) {
        Write-Host "auth: using a token from the environment" -ForegroundColor Green
    }
}

if (-not $useGh -and -not $token) {
    Write-Host ""
    Write-Error @"
No GitHub authentication available, and $Repo is PRIVATE.

An unauthenticated fetch would return 404, which is a confusing way to fail, so
this stops here instead. Pick one:

  * Supply a personal access token with 'repo' read scope, for this shell only.
    This is the FASTEST route on a clean machine and needs nothing installed:
        `$env:GITHUB_TOKEN = '<token>'

  * Or install and log in the GitHub CLI (no token touches disk). NOTE: do not
    reach for winget here -- on Windows Server 2019 there is no Microsoft
    Store, so winget (which ships as the App Installer Store package) is not
    available and cannot be added. Download the MSI directly; the URL is public
    and needs no authentication, unlike the repo:
        `$m = "`$env:TEMP\gh.msi"
        Invoke-WebRequest -UseBasicParsing -OutFile `$m ``
          https://github.com/cli/cli/releases/download/v2.97.0/gh_2.97.0_windows_amd64.msi
        Start-Process msiexec.exe -Wait -ArgumentList "/i `"`$m`" /quiet /norestart"
        # then reopen the shell so gh is on PATH, and:
        gh auth login
    In this Claude Code session you can run the interactive login yourself with:
        ! gh auth login
"@
    exit 2
}

# --- fetch ------------------------------------------------------------------
New-Item -ItemType Directory -Path $Destination -Force | Out-Null

# Borrow gh's token rather than shelling out to `gh api` per file.
#
# WHY, because the obvious version is subtly wrong: `gh api ... > $OutFile`
# routes the response through PowerShell's TEXT redirection operator. That
# rewrites LF to CRLF, and under Windows PowerShell 5.1 it writes UTF-16LE by
# default -- which does not merely reformat the file, it corrupts it. Measured
# on PowerShell 7 the fetched files came back CRLF where the repo has LF; the
# content survived, but only because 7's default is UTF-8. Downloading the
# bytes with Invoke-WebRequest -OutFile is verbatim on both.
#
# The token is held in memory for the life of this script and never written to
# disk or echoed.
if ($useGh -and -not $token) {
    $token = (& gh auth token).Trim()
    if (-not $token) { Write-Error "gh reported it is logged in but produced no token."; exit 2 }
}

function Get-RepoFile {
    param([string]$RepoRelPath, [string]$OutFile)
    $apiPath = "repos/$Repo/contents/$RepoRelPath`?ref=$Ref"
    $uri = "https://api.github.com/$apiPath"
    $headers = @{
        Authorization = "Bearer $token"
        Accept        = 'application/vnd.github.raw'
        'User-Agent'  = 'pdfe-runner-bootstrap'
    }
    Invoke-WebRequest -Uri $uri -Headers $headers -OutFile $OutFile -UseBasicParsing
    if (-not (Test-Path $OutFile) -or (Get-Item $OutFile).Length -eq 0) {
        throw "$RepoRelPath downloaded as empty -- refusing to continue"
    }
}

# DRIFT CHECK. $ExpectedFiles is hand-maintained, and it has already gone stale
# once: Test-InstallerUrls.ps1 and its mutation harness were added to the
# package and NOT to this list, so a bootstrap would have silently deployed a
# runner without the URL checker. An explicit list is still worth having -- it
# is a positive guarantee rather than "whatever the API happened to return" --
# but it must be able to notice when the directory has outgrown it.
try {
    $listing = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/contents/$PackagePath`?ref=$Ref" -Headers @{
        Authorization = "Bearer $token"
        Accept        = 'application/vnd.github+json'
        'User-Agent'  = 'pdfe-runner-bootstrap'
    }
    $actual = @($listing | Where-Object { $_.type -eq 'file' } | ForEach-Object { $_.name })
    $unlisted = @($actual | Where-Object { $ExpectedFiles -notcontains $_ -and $_ -ne 'Bootstrap-PdfeRunner.ps1' })
    if ($unlisted) {
        Write-Host ("WARNING: package contains files this script does not fetch: {0}" -f ($unlisted -join ', ')) -ForegroundColor Yellow
        Write-Host "         Add them to `$ExpectedFiles. Continuing with the listed set." -ForegroundColor Yellow
        Write-Host ""
    }
} catch {
    # Non-fatal: this is a drift WARNING, not the fetch itself. Failing the
    # deployment because a convenience check could not run would be worse than
    # the drift it looks for.
    Write-Host "note: could not list the package directory to check for drift ($($_.Exception.Message))" -ForegroundColor DarkGray
}

$fetched = @()
foreach ($f in $ExpectedFiles) {
    $out = [IO.Path]::Combine($Destination, $f)
    Write-Host "fetching $PackagePath/$f"
    try {
        Get-RepoFile -RepoRelPath "$PackagePath/$f" -OutFile $out
        $hash = (Get-FileHash -Path $out -Algorithm SHA256).Hash
        Write-Host ("  ok  {0}  sha256 {1}" -f (Get-Item $out).Length, $hash.Substring(0, 16) + '...')
        $fetched += $f
    } catch {
        Write-Host "  FAILED: $($_.Exception.Message)" -ForegroundColor Red
    }
}

Write-Host ""
$missing = $ExpectedFiles | Where-Object { $fetched -notcontains $_ }
if ($missing) {
    # A partial package is worse than none: Install-PdfeRunner.ps1 would run
    # against a requirements file that might itself be the missing piece.
    Write-Error "Package is INCOMPLETE -- missing: $($missing -join ', '). Not proceeding."
    exit 1
}

Write-Host "All $($ExpectedFiles.Count) files fetched to $Destination" -ForegroundColor Green

# Record exactly what was deployed, so a runner behaving oddly later can be
# traced to a package version rather than guessed at.
$manifest = [pscustomobject]@{
    repo         = $Repo
    ref          = $Ref
    fetched_utc  = (Get-Date).ToUniversalTime().ToString('o')
    machine      = $env:COMPUTERNAME
    files        = @($ExpectedFiles | ForEach-Object {
                        [pscustomobject]@{
                            name   = $_
                            sha256 = (Get-FileHash -Path ([IO.Path]::Combine($Destination, $_)) -Algorithm SHA256).Hash
                        }
                    })
}
$manifestPath = [IO.Path]::Combine($Destination, 'deployed-manifest.json')
$manifest | ConvertTo-Json -Depth 4 | Out-File -FilePath $manifestPath -Encoding utf8
Write-Host "provenance -> $manifestPath"

Write-Host ""
Write-Host "Next:" -ForegroundColor Cyan
Write-Host "  1. Read what arrived (especially README.md) before running it."
Write-Host "  2. $Destination\Install-PdfeRunner.ps1 -RepoRoot <path-to-a-PDFE-checkout>"
Write-Host "  3. $Destination\Test-PdfeRunner.ps1 -RepoRoot <same> -RunnerDir C:\actions-runner"
Write-Host "  4. python $Destination\smoke_uia.py"
Write-Host ""
Write-Host "Note: Install-PdfeRunner.ps1 needs -RepoRoot for the pinned pip requirements," -ForegroundColor Yellow
Write-Host "which live in the checkout, not in this package. Clone the repo first:" -ForegroundColor Yellow
Write-Host "  gh repo clone $Repo C:\src\PDFE" -ForegroundColor Yellow

if ($Run) {
    Write-Host ""
    Write-Host "-Run was specified; invoking the installer." -ForegroundColor Cyan
    & ([IO.Path]::Combine($Destination, 'Install-PdfeRunner.ps1'))
}
