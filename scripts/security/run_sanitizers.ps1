#requires -Version 5.1
<#
.SYNOPSIS
  Phase-7 sanitizer-instrumented test run. Builds the PDFE
  GoogleTest suite with clang-cl + ASan + UBSan, runs ctest with
  sanitizer environment variables pointing at lsan.supp / ubsan.supp,
  parses any reports into findings.json, exits non-zero on any
  non-suppressed finding.

.DESCRIPTION
  OPT-IN: invoked manually or by phase-9 nightly. Not part of the
  default ctest workflow. Uses a SEPARATE binary dir
  (build/vs2022-asan) so the regular VS2022 build under
  build/vs2022 is undisturbed.

  MSan is intentionally NOT invoked: clang-cl on Windows does not
  support -fsanitize=memory. Adding MSan requires a Linux build of
  PDFE + Qt6 + dependencies, which doesn't exist yet. See
  tests/security/sanitizers/README.md '"MSan: deferred to Linux"'
  section.

  Outputs:
    tests/security/reports/sanitizers/<run_id>/
        ctest.log       full ctest stdout/stderr including any
                          ASan/UBSan reports
        findings.json   normalized findings (kind, test_name,
                          frames[5], hash)
        summary.md      counts by kind + non-suppressed verdict

  Exit codes:
    0 -- ctest passed AND no non-suppressed sanitizer findings
    1 -- one or more non-suppressed findings (or ctest failed)
    2 -- preflight failure (no clang-cl, no asan build dir, etc.)

.PARAMETER ConfigureOnly
  Only run the CMake configure step; don't build or run tests.
  Useful as a quick sanity check that the asan preset is wired
  correctly.

.PARAMETER NoBuild
  Skip the cmake --build step (assume the asan tree is already
  built). Useful for re-running tests after a manual edit.

.PARAMETER UpdateBaseline
  Replace tests/security/sanitizers/baseline.json with the current
  run's findings hashes. Use only after triaging in triage.md.

.EXAMPLE
  pwsh scripts/security/run_sanitizers.ps1
  pwsh scripts/security/run_sanitizers.ps1 -ConfigureOnly
  pwsh scripts/security/run_sanitizers.ps1 -UpdateBaseline
#>
[CmdletBinding()]
param(
    [switch]$ConfigureOnly,
    [switch]$NoBuild,
    [switch]$UpdateBaseline
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$SanDir      = Join-Path $RepoRoot 'tests\security\sanitizers'
$ReportRoot  = Join-Path $RepoRoot 'tests\security\reports\sanitizers'
$BuildDir    = Join-Path $RepoRoot 'build\vs2022-asan'

# ============================================================
# Preflight
# ============================================================
# Helper: write to stderr without raising a terminating error.
# Plain Write-Error under ErrorActionPreference=Stop unwinds before
# any subsequent `exit <code>` line can execute, breaking the
# documented exit-code contract. Use stderr+exit pairs instead.
function _bail([int]$code, [string]$msg) {
    [Console]::Error.WriteLine($msg)
    exit $code
}

$cmakeExe = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmakeExe)) {
    $cmakeOnPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeOnPath) { $cmakeExe = $cmakeOnPath.Path }
    else {
        _bail 2 "[run_sanitizers] cmake not found at VS path or on PATH"
    }
}

# Verify clang-cl is available somewhere CMake will find it.
$clangCl = Get-Command clang-cl -ErrorAction SilentlyContinue
if (-not $clangCl) {
    $vsClangCl = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe"
    if (-not (Test-Path $vsClangCl)) {
        _bail 2 @"
[run_sanitizers] clang-cl not found.
Install via Visual Studio Installer -> 'C++ Clang tools for Windows'
workload, or place clang-cl on PATH. The asan preset requires
clang-cl because cl.exe doesn't support -fsanitize=address,undefined.
"@
    }
}

# `-AsUTC` is PS7-only; [DateTime]::UtcNow works on PS 5.1 too.
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$runReportDir = Join-Path $ReportRoot ('run_' + $stamp)
New-Item -ItemType Directory -Path $runReportDir -Force | Out-Null

# ============================================================
# Configure (asan preset)
# ============================================================
$lsanSupp = Join-Path $SanDir 'lsan.supp'
$ubsanSupp = Join-Path $SanDir 'ubsan.supp'

Push-Location $RepoRoot
try {
    Write-Host "[run_sanitizers] cmake --preset windows-vs2022-asan"
    & $cmakeExe --preset windows-vs2022-asan
    if ($LASTEXITCODE -ne 0) {
        _bail 2 "[run_sanitizers] configure failed"
    }

    if ($ConfigureOnly) {
        Write-Host "[run_sanitizers] -ConfigureOnly: stopping after configure."
        exit 0
    }

    if (-not $NoBuild) {
        Write-Host "[run_sanitizers] cmake --build (Debug)"
        & $cmakeExe --build $BuildDir --config Debug --target pdfe_tests
        if ($LASTEXITCODE -ne 0) {
            _bail 2 "[run_sanitizers] build failed"
        }
    }
} finally {
    Pop-Location
}

# ============================================================
# Run ctest with sanitizer env wired up
# ============================================================
$ctestLog = Join-Path $runReportDir 'ctest.log'

# ASAN_OPTIONS / UBSAN_OPTIONS / LSAN_OPTIONS:
#  - print_stacktrace=1: full backtrace per finding
#  - halt_on_error=0:    don't abort on first issue; collect them all
#  - exitcode=42:        distinct exit code so we can detect at least
#                        one finding even when the test itself "passed"
#  - abort_on_error=0:   do not raise SIGABRT (Windows quirks)
#  - suppressions=...:   our supp files
$asanOpts  = "halt_on_error=0:abort_on_error=0:print_stacktrace=1:exitcode=42:detect_stack_use_after_return=1"
$ubsanOpts = "halt_on_error=0:print_stacktrace=1:print_summary=1:suppressions=$ubsanSupp"
$lsanOpts  = "suppressions=$lsanSupp:print_suppressions=1"

$prevAsan  = $env:ASAN_OPTIONS
$prevUbsan = $env:UBSAN_OPTIONS
$prevLsan  = $env:LSAN_OPTIONS
$env:ASAN_OPTIONS  = $asanOpts
$env:UBSAN_OPTIONS = $ubsanOpts
$env:LSAN_OPTIONS  = $lsanOpts

try {
    Push-Location $BuildDir
    Write-Host "[run_sanitizers] ctest -C Debug -LE security --output-on-failure"
    & ctest -C Debug -LE security --output-on-failure 2>&1 | Tee-Object -FilePath $ctestLog
    $ctestExit = $LASTEXITCODE
    Pop-Location
} finally {
    $env:ASAN_OPTIONS  = $prevAsan
    $env:UBSAN_OPTIONS = $prevUbsan
    $env:LSAN_OPTIONS  = $prevLsan
}

# ============================================================
# Parse sanitizer findings out of ctest log
# ============================================================
$findings = @()
$rxAsanHeader = [regex]'==\d+==(?<kind>ERROR: AddressSanitizer: [a-z\-]+|WARNING: AddressSanitizer: [a-z\-]+|ERROR: LeakSanitizer: [a-z\-]+)'
$rxUbsanHeader = [regex]'(?<file>[^:]+):(?<line>\d+):(?<col>\d+): runtime error: (?<msg>.*)'
$rxFrame = [regex]'^\s+#(?<idx>\d+)\s+0x[0-9a-f]+\s+in\s+(?<sym>[^\s]+)'

$lines = if (Test-Path $ctestLog) { Get-Content $ctestLog } else { @() }
$currentReport = $null
$currentFrames = @()
$currentTest   = '<unknown>'
$rxTestName = [regex]'^\d+:.*Test\s+#\d+:\s+(?<name>\S+)'
$rxTestFromCtest = [regex]'^\s*Start\s+\d+:\s+(?<name>\S+)'

foreach ($line in $lines) {
    if ($line -match $rxTestFromCtest) {
        $currentTest = $matches['name']
    }
    $m = $rxAsanHeader.Match($line)
    if ($m.Success) {
        if ($currentReport) {
            $findings += $currentReport
        }
        $kind = $m.Groups['kind'].Value -replace '^(ERROR|WARNING):\s*(AddressSanitizer|LeakSanitizer):\s*',''
        $currentReport = @{
            kind      = $kind
            test_name = $currentTest
            frames    = @()
        }
        $currentFrames = @()
        continue
    }
    if ($currentReport -and ($line -match $rxFrame)) {
        if ($currentReport.frames.Count -lt 5) {
            $currentReport.frames += $matches['sym']
        }
    }
    $um = $rxUbsanHeader.Match($line)
    if ($um.Success) {
        $findings += @{
            kind      = ('UB-' + ($um.Groups['msg'].Value -replace '\s+',' ').Substring(0, [Math]::Min(60, $um.Groups['msg'].Value.Length)))
            test_name = $currentTest
            frames    = @("$($um.Groups['file'].Value):$($um.Groups['line'].Value)")
        }
    }
}
if ($currentReport) { $findings += $currentReport }

# Hash each finding for stable cross-run dedup.
$sha1 = [System.Security.Cryptography.SHA1]::Create()
foreach ($f in $findings) {
    $hashSrc = "$($f.kind)|$($f.test_name)|" + (($f.frames -join '|'))
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($hashSrc)
    $f.hash = ([System.BitConverter]::ToString($sha1.ComputeHash($bytes)) -replace '-','').ToLower()
}
$sha1.Dispose()

# ============================================================
# Compare to baseline
# ============================================================
$baselinePath = Join-Path $SanDir 'baseline.json'
$baselineHashes = @{}
if (Test-Path $baselinePath) {
    $baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
    foreach ($h in $baseline.suppressed_hashes) { $baselineHashes[$h] = $true }
}

$nonSuppressed = $findings | Where-Object {
    -not $baselineHashes.ContainsKey($_.hash)
}

# ============================================================
# Emit findings.json + summary.md
# ============================================================
$findingsJson = @{
    schema_version = 1
    stamp          = ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
    ctest_exit     = $ctestExit
    findings       = $findings
    counts_by_kind = @{}
    non_suppressed = $nonSuppressed.Count
}
foreach ($f in $findings) {
    $k = $f.kind
    if (-not $findingsJson.counts_by_kind.ContainsKey($k)) {
        $findingsJson.counts_by_kind[$k] = 0
    }
    $findingsJson.counts_by_kind[$k]++
}
$findingsPath = Join-Path $runReportDir 'findings.json'
$findingsJson | ConvertTo-Json -Depth 6 | Set-Content -Path $findingsPath -Encoding UTF8

$summaryPath = Join-Path $runReportDir 'summary.md'
$verdict = if ($nonSuppressed.Count -eq 0 -and $ctestExit -eq 0) { 'PASS' } else { 'FAIL' }
$mdLines = @(
    "# Sanitizer run $stamp",
    "",
    "**Verdict: $verdict**",
    "",
    "ctest exit code: $ctestExit",
    "Total findings:  $($findings.Count)",
    "Non-suppressed:  $($nonSuppressed.Count)",
    "",
    "## By kind"
)
foreach ($k in ($findingsJson.counts_by_kind.Keys | Sort-Object)) {
    $mdLines += "- ``$k`` x $($findingsJson.counts_by_kind[$k])"
}
$mdLines += @("","## Non-suppressed findings")
foreach ($f in $nonSuppressed) {
    $mdLines += "- $($f.kind) in $($f.test_name): top frame $($f.frames[0])"
}
$mdLines | Set-Content -Path $summaryPath -Encoding UTF8

# Mirror summary.md to canonical location for phase-9 baseline ref
$canonicalSummary = Join-Path $ReportRoot 'summary.md'
Copy-Item $summaryPath $canonicalSummary -Force

# ============================================================
# Update baseline if requested
# ============================================================
if ($UpdateBaseline) {
    @{
        schema_version    = 1
        stamp             = $findingsJson.stamp
        suppressed_hashes = @($findings | ForEach-Object { $_.hash })
        note              = 'Hashes of accepted/known sanitizer findings (e.g. Qt static globals). Update via run_sanitizers.ps1 -UpdateBaseline only after triaging in tests/security/sanitizers/triage.md.'
    } | ConvertTo-Json -Depth 4 | Set-Content -Path $baselinePath -Encoding UTF8
    Write-Host "[run_sanitizers] baseline updated: $baselinePath"
    exit 0
}

Write-Host "[run_sanitizers] verdict=$verdict ctest_exit=$ctestExit findings=$($findings.Count) non_suppressed=$($nonSuppressed.Count)"
Write-Host "[run_sanitizers] $summaryPath"
if ($verdict -eq 'PASS') { exit 0 } else { exit 1 }
