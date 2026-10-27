# -*- coding: utf-8 -*-
<#
.SYNOPSIS
    Clean-machine dependency smoke test for the PDFE installer.

.DESCRIPTION
    Guards against the "app installs but won't start because a bundled DLL is
    missing" class of bug (e.g. the Qt6Network.dll gap in v0.1.0). It:

      1. Extracts the NSIS installer payload with 7-Zip (no admin / no real install).
      2. Launches the bundled pdfe.exe with the environment of a *clean* machine:
         PATH stripped to the Windows system directories only, and all Qt env
         vars cleared. This is the critical part -- on the build/CI box the real
         PATH contains vcpkg's bin, which masks missing bundled DLLs. Stripping it
         forces the loader to resolve every dependency from inside the bundle.
      3. PASSES only if pdfe.exe reaches its main window (title "PDFE"). A missing
         dependency makes it either exit early or stall on a "DLL was not found"
         system-error dialog -- both are treated as FAIL.

    Exit code 0 = self-contained bundle; non-zero = missing dependency / failure.

.PARAMETER Installer
    Path to the built installer (default:
    build/vs2022-package/installer/PDFE-*-win64.exe -- the PACKAGING tree,
    configured with PDFE_TESTING=OFF. NOT build/vs2022: that tree carries the
    test-only PDFE_TEST_PIPES seam and must never be the source of a shipped
    bundle (GitHub #182). Defaulting here to the tree that is packaged keeps
    the smoke test pointed at the artifact users actually receive.

.PARAMETER TimeoutSeconds
    How long to wait for the main window (default 30).

.EXAMPLE
    pwsh -File scripts/verify-installer-deps.ps1
#>
[CmdletBinding()]
param(
    [string] $Installer,
    [int]    $TimeoutSeconds = 30,
    [string] $SevenZip
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Fail($msg) { Write-Host "FAIL: $msg" -ForegroundColor Red; exit 1 }

# --- Resolve installer + 7-Zip ---------------------------------------------
if (-not $Installer) {
    $Installer = Get-ChildItem (Join-Path $repoRoot 'build/vs2022-package/installer') -Filter 'PDFE-*-win64.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Installer -or -not (Test-Path $Installer)) { Fail "installer not found (pass -Installer <path>)" }
if (-not $SevenZip) {
    $SevenZip = @("C:\Program Files\7-Zip\7z.exe", "C:\Program Files (x86)\7-Zip\7z.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $SevenZip) { $c = Get-Command 7z -ErrorAction SilentlyContinue; if ($c) { $SevenZip = $c.Source } }
}
if (-not $SevenZip) { Fail "7-Zip (7z.exe) not found; needed to extract the NSIS payload" }

Write-Host "Installer : $Installer"
Write-Host "7-Zip     : $SevenZip"

# --- Extract payload --------------------------------------------------------
$work = Join-Path ([System.IO.Path]::GetTempPath()) "pdfe_depcheck"
if (Test-Path $work) { & cmd /c "rmdir /s /q `"$work`"" 2>$null }
New-Item -ItemType Directory -Path $work -Force | Out-Null
& $SevenZip x -aoa -o"$work" "$Installer" > $null 2>&1
$exe = Join-Path $work 'bin\pdfe.exe'
if (-not (Test-Path $exe)) { Fail "bin\pdfe.exe not found in the installer payload" }
$qtCount = (Get-ChildItem (Join-Path $work 'bin') -Filter 'Qt6*.dll' -ErrorAction SilentlyContinue).Count
Write-Host "Payload   : $exe  (Qt6 DLLs bundled: $qtCount)"

# --- Launch with a clean-machine environment --------------------------------
$saved = @{ PATH = $env:PATH; QPP = $env:QT_PLUGIN_PATH; QPPP = $env:QT_QPA_PLATFORM_PLUGIN_PATH }
$sys = $env:SystemRoot
$env:PATH = "$sys\System32;$sys;$sys\System32\wbem;$sys\System32\WindowsPowerShell\v1.0"
$env:QT_PLUGIN_PATH = ''
$env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
$env:QML2_IMPORT_PATH = ''

Write-Host "`nLaunching with sanitized PATH ($($env:PATH))..."
$proc = $null
try {
    $proc = Start-Process -FilePath $exe -PassThru -ErrorAction Stop
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $proc.Refresh()
        if ($proc.HasExited) { break }
        if ($proc.MainWindowTitle -eq 'PDFE') { $ready = $true; break }
    }
    if ($ready) {
        Write-Host "PASS: pdfe.exe started from the bundle alone (main window 'PDFE', WS=$([math]::Round($proc.WorkingSet64/1MB))MB)." -ForegroundColor Green
        $result = 0
    } elseif ($proc.HasExited) {
        Write-Host "FAIL: pdfe.exe exited early (code=$($proc.ExitCode)) -- a bundled dependency is missing." -ForegroundColor Red
        $result = 1
    } else {
        Write-Host "FAIL: pdfe.exe never reached its main window in ${TimeoutSeconds}s (window='$($proc.MainWindowTitle)') -- likely blocked on a missing-DLL error dialog." -ForegroundColor Red
        $result = 1
    }
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Get-Process pdfe -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $env:PATH = $saved.PATH; $env:QT_PLUGIN_PATH = $saved.QPP; $env:QT_QPA_PLATFORM_PLUGIN_PATH = $saved.QPPP
    & cmd /c "rmdir /s /q `"$work`"" 2>$null
}
exit $result
