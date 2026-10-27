# Verifies the OPT-IN posture invariant from pipeline phases 1 + 9.
#
# CTest does not have a built-in "label = excluded by default" mode.
# The opt-in posture is enforced via CMakePresets.json filters:
#   testPreset 'test-vs2022'           filter.exclude.label = security
#   testPreset 'test-vs2022-security'  filter.include.label = security
# Equivalent direct ctest invocations:
#   ctest -LE security  ->  regular dev workflow (no security tests)
#   ctest -L  security  ->  opt-in workflow (only security tests)
#
# This script asserts both halves of that contract by listing tests
# with each filter (no execution) and inspecting the output.
#
# Output: tests/security/reports/phase1_optin_verification.txt
# Exit codes:
#   0  invariant holds
#   2  regular workflow lists security tests (FAIL)
#   3  opt-in workflow lists no security tests (FAIL -- registration broken)

[CmdletBinding()]
param(
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
$thisFile = $MyInvocation.MyCommand.Path
$thisDir  = Split-Path -Parent $thisFile
$securityDir = Split-Path -Parent $thisDir
$repoDir  = Split-Path -Parent (Split-Path -Parent $securityDir)
if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $repoDir 'build\vs2022'
}
$reportFile = Join-Path $securityDir 'reports\phase1_optin_verification.txt'
New-Item -ItemType Directory -Path (Split-Path -Parent $reportFile) -Force | Out-Null

if (-not (Test-Path $BuildDir)) {
    Write-Host "[verify_optin] build dir not found: $BuildDir" -ForegroundColor Red
    Write-Host "[verify_optin] configure first with: cmake --preset windows-vs2022 -DPDFE_ENABLE_SECURITY_TESTS=ON"
    exit 2
}

# Resolve ctest. PowerShell's PATH may not have it on machines that
# only put cmake on the user PATH. Probe common VS install locations.
$ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrEmpty($ctest)) {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe',
        'C:\Program Files\CMake\bin\ctest.exe'
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $ctest = $c; break }
    }
}
if ([string]::IsNullOrEmpty($ctest)) {
    Write-Host "[verify_optin] ctest not found on PATH or in common VS install dirs" -ForegroundColor Red
    exit 2
}

Push-Location $BuildDir
try {
    Write-Host "[verify_optin] Listing tests with -LE security (regular dev workflow)..." -ForegroundColor Cyan
    $defaultListing = & $ctest -N -LE security -C Debug 2>&1 | Out-String
    $defaultLines   = ($defaultListing -split "`r?`n")
    $defaultSecurityHits = $defaultLines | Where-Object { $_ -match 'Test\s+#\d+:\s*security_' }

    Write-Host "[verify_optin] Listing tests with -L security (opt-in workflow)..." -ForegroundColor Cyan
    $securityListing = & $ctest -N -L security -C Debug 2>&1 | Out-String
    $securityCount   = if ($securityListing -match 'Total Tests:\s*(\d+)') { [int]$matches[1] } else { 0 }

    # Make the report content deterministic across runs so the
    # checked-in copy doesn't dirty `git status` on every ctest -L
    # security invocation. The verdict + counts are what matter as
    # audit evidence; the wall-clock timestamp doesn't, and is
    # recoverable from `git log` on the file when needed.
    # Build dir is shown repo-relative so different developers'
    # absolute paths don't churn the file either.
    # Normalize to forward-slash forms first; CMake passes BuildDir
    # with `/` while PowerShell's Split-Path emits `\`, so a direct
    # StartsWith would miss.
    $repoForward     = ($repoDir  -replace '\\','/').TrimEnd('/')
    $buildDirForward = ($BuildDir -replace '\\','/').TrimEnd('/')
    $relBuildDir = $buildDirForward
    if ($buildDirForward.StartsWith($repoForward, [System.StringComparison]::OrdinalIgnoreCase)) {
        $tail = $buildDirForward.Substring($repoForward.Length).TrimStart('/')
        if ([string]::IsNullOrEmpty($tail)) { $tail = '.' }
        $relBuildDir = '<repo>/' + $tail
    }
    # Strip the absolute repo path from the ctest listings too.
    $defaultListing  = $defaultListing  -replace [regex]::Escape($repoForward), '<repo>'
    $securityListing = $securityListing -replace [regex]::Escape($repoForward), '<repo>'

    $report = @"
PDFE security harness -- opt-in verification
Build dir: $relBuildDir

The opt-in posture (pipeline phase 1, 9): the regular developer test
workflow MUST NOT execute security tests; security tests run only
when the developer explicitly invokes them.

Mechanism (CMakePresets.json):
  testPreset 'test-vs2022'           filter.exclude.label = security
  testPreset 'test-vs2022-security'  filter.include.label = security
Equivalent direct ctest:
  ctest -LE security   regular dev workflow (this is what 'pdfe_tests' should
                       map to in any wrapping script)
  ctest -L  security   opt-in workflow

=== ctest -N -LE security (regular dev workflow) ===
$defaultListing

=== ctest -N -L security (explicit opt-in workflow) ===
$securityListing

=== Verdict ===
"@

    if ($defaultSecurityHits.Count -gt 0) {
        $report += "FAIL -- regular workflow (-LE security) still lists security tests:`n"
        foreach ($h in $defaultSecurityHits) { $report += "  $h`n" }
        $verdict = 2
    } elseif ($securityCount -lt 1) {
        $report += "FAIL -- opt-in workflow (-L security) lists ZERO security tests; security suite is not registered or is mis-labeled`n"
        $verdict = 3
    } else {
        $report += "PASS -- opt-in invariant holds.`n"
        $report += "  -LE security:  zero security tests`n"
        $report += "  -L  security:  $securityCount security test(s)`n"
        $verdict = 0
    }

    $report | Set-Content -Encoding UTF8 $reportFile
    Write-Host "[verify_optin] Report written to $reportFile" -ForegroundColor Green
    Write-Host $report
    exit $verdict
}
finally {
    Pop-Location
}
