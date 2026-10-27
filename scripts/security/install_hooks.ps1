#requires -Version 5.1
<#
.SYNOPSIS
  Install PDFE security git hooks (currently: pre-push). OPT-IN
  per developer -- the hooks are NOT auto-installed by `cmake
  --preset` or the build, because not every developer runs the
  security suite locally.

.DESCRIPTION
  Copies scripts/security/hooks/pre-push into .git/hooks/pre-push
  and ensures the executable bit is set on Unix-like shells. On
  Windows, git's bash wrapper runs the file regardless of mode bit.

.PARAMETER Force
  Overwrite an existing .git/hooks/pre-push without prompting.

.PARAMETER Uninstall
  Remove .git/hooks/pre-push (only if it's the PDFE one).

.EXAMPLE
  pwsh scripts/security/install_hooks.ps1
  pwsh scripts/security/install_hooks.ps1 -Force
  pwsh scripts/security/install_hooks.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$Source    = Join-Path $ScriptDir 'hooks\pre-push'
$DotGit    = Join-Path $RepoRoot '.git'
if (-not (Test-Path $DotGit)) {
    [Console]::Error.WriteLine("[install_hooks] $DotGit not found")
    exit 2
}
$HooksDir  = Join-Path $DotGit 'hooks'
$Target    = Join-Path $HooksDir 'pre-push'

New-Item -ItemType Directory -Path $HooksDir -Force | Out-Null

if ($Uninstall) {
    if (Test-Path $Target) {
        $existing = Get-Content $Target -Raw -ErrorAction SilentlyContinue
        if ($existing -match 'PDFE pre-push security regression hook') {
            Remove-Item $Target -Force
            Write-Host "[install_hooks] uninstalled $Target"
            exit 0
        } else {
            [Console]::Error.WriteLine("[install_hooks] $Target exists but is NOT the PDFE hook; refusing to remove. Inspect/delete manually.")
            exit 1
        }
    }
    Write-Host "[install_hooks] no PDFE pre-push hook to uninstall."
    exit 0
}

if ((Test-Path $Target) -and -not $Force) {
    $existing = Get-Content $Target -Raw -ErrorAction SilentlyContinue
    if ($existing -match 'PDFE pre-push security regression hook') {
        Write-Host "[install_hooks] PDFE pre-push hook already installed; -Force to refresh."
        exit 0
    } else {
        [Console]::Error.WriteLine("[install_hooks] $Target exists but is NOT the PDFE hook; -Force to overwrite or remove manually.")
        exit 1
    }
}

Copy-Item -Path $Source -Destination $Target -Force
# Best-effort chmod for Unix shells; harmless no-op on pure Windows.
try { & chmod +x $Target 2>$null } catch {}
Write-Host "[install_hooks] installed $Target"
Write-Host "[install_hooks] next push will run `ctest -L security` first; bypass once with `git push --no-verify`."
exit 0
