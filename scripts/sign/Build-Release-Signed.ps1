# -*- coding: utf-8 -*-
<#
.SYNOPSIS
    Build a fully code-signed PDFE release installer (Azure Trusted Signing).

.DESCRIPTION
    End-to-end signed-release pipeline:
      1. Build the Release `pdfe` target.
      2. Sign the app binaries (pdfe.exe, pdfe_preflight.exe) BEFORE packaging,
         so the *installed* executables are signed.
      3. Build the NSIS installer with CPack (now bundling signed binaries).
      4. Sign the installer itself (the file users download and run).
      5. Verify every signature and print the installer's SHA-256.

    Only PDFE's own binaries are signed. Third-party DLLs (Qt, MuPDF, OpenSSL,
    ...) ship already signed by their vendors and are left untouched.

    Requires Trusted Signing config + Azure auth (see Sign-Artifacts.ps1 and
    docs/code-signing.md). Run `az login` first on a dev box.

    THE TREE THIS BUILDS FROM (GitHub #182). Packaging happens out of
    `build/vs2022-package`, configured by `cmake --preset windows-vs2022-package`,
    which sets PDFE_TESTING=OFF. It is NOT `build/vs2022`: that tree is the one
    pdfe_tests.exe is configured for and it carries PDFE_TESTING=ON, which
    compiles IN the test-only introspection seams (PDFE_TEST_PIPES -- the
    dialog-inventory and tool-activation pipes any local process could drive by
    setting one environment variable). This script used to default to
    build/vs2022, which is how every signed release to date, v0.2.0 included,
    shipped carrying the seam.

    The flag is configure-scoped and cannot be a per-target property (pdfe_ui is
    a static library linked into both pdfe.exe and pdfe_tests.exe, and dialogs
    in several other libraries include the same header), so a second tree is the
    mechanism, not a convenience.

    Step 0 below READS THE CACHE of whatever tree it was pointed at and refuses
    to package a tree whose PDFE_TESTING is not OFF. Passing -BuildDir does not
    get you around it: a wrong tree is a hard stop, not a warning.

.PARAMETER BuildDir
    CMake build directory (default build/vs2022-package -- see above; it must be
    a tree configured with PDFE_TESTING=OFF or this script refuses to run).

.PARAMETER Configuration
    Build configuration (default Release).

.EXAMPLE
    az login
    $env:TRUSTED_SIGNING_ENDPOINT = "https://eus.codesigning.azure.net/"
    $env:TRUSTED_SIGNING_ACCOUNT  = "my-ts-account"
    $env:TRUSTED_SIGNING_PROFILE  = "my-cert-profile"
    ./scripts/sign/Build-Release-Signed.ps1
#>
[CmdletBinding()]
param(
    [string] $BuildDir = "build/vs2022-package",
    [string] $Configuration = "Release",
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repoRoot
$signScript = Join-Path $PSScriptRoot 'Sign-Artifacts.ps1'

function Find-Tool($name, $candidates) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in $candidates) { if (Test-Path $p) { return $p } }
    throw "$name not found."
}

$cmake = Find-Tool 'cmake' @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
$cpack = Find-Tool 'cpack' @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cpack.exe')
# NSIS on PATH for CPack
$nsisDir = 'C:\Program Files (x86)\NSIS'
if ((Test-Path $nsisDir) -and ($env:PATH -notlike "*$nsisDir*")) { $env:PATH = "$env:PATH;$nsisDir" }

# 0. Refuse a tree that would ship the test seam (GitHub #182) ---------------
# Read the CACHE, not the preset name and not this script's own default: the
# question is what the tree on disk was actually configured with. A missing
# tree is configured here rather than guessed at, because the alternative --
# silently falling back to build/vs2022 -- is precisely the defect.
$packagePreset = 'windows-vs2022-package'
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cacheFile)) {
    Write-Host "==> $BuildDir is not configured; configuring preset $packagePreset" -ForegroundColor Cyan
    & $cmake --preset $packagePreset
    if ($LASTEXITCODE -ne 0) { throw "Configuring the packaging tree ($packagePreset) failed." }
}
if (-not (Test-Path $cacheFile)) {
    throw "No CMakeCache.txt in $BuildDir after configuring -- cannot verify PDFE_TESTING, refusing to package."
}
$testingLine = (Select-String -Path $cacheFile -Pattern '^PDFE_TESTING:BOOL=' -Encoding utf8 |
                Select-Object -First 1).Line
if (-not $testingLine) {
    throw "PDFE_TESTING is absent from $cacheFile. A tree that does not carry the flag at all cannot be shown to have it OFF -- refusing to package."
}
if ($testingLine.Trim() -ne 'PDFE_TESTING:BOOL=OFF') {
    throw @"
REFUSING TO PACKAGE: $BuildDir is configured $($testingLine.Trim()).
A PDFE_TESTING=ON binary compiles IN the test-only introspection seams
(PDFE_TEST_PIPES), giving any local process that can set one environment
variable a control channel into the shipped app. GitHub #182.
Package from the dedicated tree instead:
    cmake --preset $packagePreset
    ./scripts/sign/Build-Release-Signed.ps1
"@
}
Write-Host "==> Packaging tree $BuildDir verified: $($testingLine.Trim())" -ForegroundColor Green

# 1. Build ------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "==> Building $Configuration pdfe + pdfe_preflight" -ForegroundColor Cyan
    & $cmake --build $BuildDir --config $Configuration --target pdfe pdfe_preflight
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
}

# 2. Sign app binaries ------------------------------------------------------
$binDir = Join-Path $BuildDir "bin/$Configuration"
$appBinaries = @('pdfe.exe','pdfe_preflight.exe') |
    ForEach-Object { Join-Path $binDir $_ } | Where-Object { Test-Path $_ }
if (-not $appBinaries) { throw "No app binaries found in $binDir." }
Write-Host "==> Signing app binaries (pre-package)" -ForegroundColor Cyan
& $signScript -Files $appBinaries

# 3. Package ----------------------------------------------------------------
Write-Host "==> Building NSIS installer (CPack)" -ForegroundColor Cyan
Remove-Item -Recurse -Force (Join-Path $BuildDir '_CPack_Packages') -ErrorAction SilentlyContinue
& $cpack -C $Configuration -G NSIS --config (Join-Path $BuildDir 'CPackConfig.cmake')
if ($LASTEXITCODE -ne 0) { throw "CPack failed." }

$installer = Get-ChildItem (Join-Path $BuildDir 'installer') -Filter 'PDFE-*-win64.exe' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $installer) { throw "Installer not produced." }

# 4. Sign installer ---------------------------------------------------------
Write-Host "==> Signing installer" -ForegroundColor Cyan
& $signScript -Files $installer.FullName

# 5. Report -----------------------------------------------------------------
$sha = (Get-FileHash $installer.FullName -Algorithm SHA256).Hash
Write-Host "`n==================== SIGNED RELEASE ====================" -ForegroundColor Green
Write-Host "Installer : $($installer.FullName)"
Write-Host "Size      : $([math]::Round($installer.Length/1MB,1)) MB"
Write-Host "SHA-256   : $sha"
Write-Host "=======================================================" -ForegroundColor Green
Write-Host "Next: submit to SmartScreen with scripts/sign/Submit-SmartScreen.ps1"
