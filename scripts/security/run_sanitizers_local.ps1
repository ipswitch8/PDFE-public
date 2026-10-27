<#
.SYNOPSIS
    Wrapper for scripts/security/run_sanitizers.ps1 that bootstraps
    the Visual Studio Developer Environment first.

.DESCRIPTION
    The bare run_sanitizers.ps1 assumes vcvars64.bat is already
    loaded (INCLUDE / LIB / PATH set up). When invoked from a plain
    PowerShell session that wasn't launched from a Developer
    Command Prompt, clang-cl can't find <utility>, <vector>, etc.

    This wrapper:
    1. Locates VS 2022 via vswhere
    2. Imports vcvars64.bat environment into the current PS session
    3. Then calls run_sanitizers.ps1 with passed-through args

.EXAMPLE
    pwsh scripts/security/run_sanitizers_local.ps1
    pwsh scripts/security/run_sanitizers_local.ps1 -NoBuild
#>
[CmdletBinding()]
param(
    [switch]$ConfigureOnly,
    [switch]$NoBuild,
    [switch]$UpdateBaseline
)

$ErrorActionPreference = 'Stop'

# Locate VS 2022
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found at $vswhere"
    exit 2
}
$vsInstall = & $vswhere -latest -property installationPath
if (-not $vsInstall) {
    Write-Error "No VS 2022 installation found"
    exit 2
}
$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at $vcvars"
    exit 2
}

Write-Host "[wrapper] Loading VS dev environment from $vcvars" -ForegroundColor Cyan

# Run vcvars64.bat in cmd.exe, then dump the resulting environment.
# Import the diff into the current PowerShell session.
$envOutput = cmd.exe /c "`"$vcvars`" >NUL && set"
foreach ($line in $envOutput) {
    if ($line -match '^([^=]+)=(.*)$') {
        $name = $matches[1]
        $value = $matches[2]
        # Skip a few PS-special vars
        if ($name -notin @('PROMPT', 'TERM_SESSION_ID')) {
            Set-Item -Path "Env:$name" -Value $value -Force
        }
    }
}

Write-Host "[wrapper] INCLUDE has $((($env:INCLUDE -split ';') | Where-Object { $_ }).Count) paths" -ForegroundColor Gray
Write-Host "[wrapper] LIB has $((($env:LIB -split ';') | Where-Object { $_ }).Count) paths" -ForegroundColor Gray

# vcvars sets LIB to MSVC + Windows SDK libs but does NOT add the
# clang_rt sanitizer runtime libraries. Without them on LIB,
# lld-link can't resolve __asan_init and the cmake compiler probe
# fails. Locate the clang_rt windows lib dir and prepend it.
$rtBase = Join-Path $vsInstall "VC\Tools\Llvm\x64\lib\clang"
$rtLib = Get-ChildItem $rtBase -Directory -ErrorAction SilentlyContinue |
         ForEach-Object { Join-Path $_.FullName "lib\windows" } |
         Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $rtLib) {
    Write-Error "clang_rt windows lib dir not found under $rtBase"
    exit 2
}
Write-Host "[wrapper] Adding clang_rt: $rtLib" -ForegroundColor Gray
$env:LIB = "$rtLib;$env:LIB"
$env:PATH = "$rtLib;$env:PATH"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$BinDir = Join-Path $RepoRoot 'build\vs2022-asan\bin\Debug'
$inner = Join-Path $ScriptDir "run_sanitizers.ps1"

# Stage asan_dynamic DLL into the build output dir alongside the
# test exe. Windows DLL search checks the exe's directory before
# PATH; ctest's gtest_discover_tests fails with 0xc0000135 if the
# DLL isn't reachable that way (PATH inheritance through
# ninja/cmake/ctest sub-shells is unreliable for runtime DLL
# load). Function so we can call it before AND after build.
function Stage-AsanDll {
    if (-not (Test-Path $BinDir)) { return }
    Get-ChildItem $rtLib -Filter "clang_rt.asan_dynamic*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item $_.FullName $BinDir -Force
        Write-Host "[wrapper] Staged $($_.Name) -> $BinDir" -ForegroundColor Gray
    }
}

# If specific phases were requested, forward directly
if ($ConfigureOnly -or $NoBuild) {
    $forwardArgs = @{}
    if ($ConfigureOnly) { $forwardArgs['ConfigureOnly'] = $true }
    if ($NoBuild) { Stage-AsanDll; $forwardArgs['NoBuild'] = $true }
    if ($UpdateBaseline) { $forwardArgs['UpdateBaseline'] = $true }
    & $inner @forwardArgs
    exit $LASTEXITCODE
}

# Default: do configure+build via run_sanitizers, then stage DLL,
# then run tests via run_sanitizers -NoBuild.
Write-Host "[wrapper] Phase 1: configure + build" -ForegroundColor Cyan
& $inner -ConfigureOnly:$false -NoBuild:$false -UpdateBaseline:$UpdateBaseline.IsPresent
# run_sanitizers.ps1 exits 2 on preflight, 1 on build/test fail.
# We want to continue iff configure+build succeeded so the DLL
# stage can happen. Detect: bin/Debug/pdfe_tests.exe must exist.
$testExe = Join-Path $BinDir 'pdfe_tests.exe'
if (-not (Test-Path $testExe)) {
    Write-Error "[wrapper] Build failed (pdfe_tests.exe not produced); aborting"
    exit 1
}

Write-Host "[wrapper] Phase 2: stage asan_dynamic DLL" -ForegroundColor Cyan
Stage-AsanDll

# Phase 3: re-invoke run_sanitizers with -NoBuild to skip
# rebuild and just run ctest. But that's redundant — we already
# ran the full thing once. The first run_sanitizers invocation
# already attempted ctest and failed because the DLL wasn't
# staged yet. So just re-run with -NoBuild.
Write-Host "[wrapper] Phase 3: re-run tests with DLL staged" -ForegroundColor Cyan
& $inner -NoBuild
exit $LASTEXITCODE
