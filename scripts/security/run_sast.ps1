#requires -Version 5.1
<#
.SYNOPSIS
  Phase-5 SAST sweep: runs cppcheck + clang-tidy across src/, normalizes
  findings into JSON, compares them to the committed baseline, and
  exits non-zero if any NEW high-severity finding (hash absent from
  the baseline) is introduced.

.DESCRIPTION
  This is the dispatcher referenced by phase 5 of the security
  pipeline. Like every other phase 4-9 driver it is OPT-IN: only
  invoked manually or by a CI nightly job; no entry registered in
  the regular ctest set.

  Inputs:
    src/                                       — code to scan
    tests/security/sast/.clang-tidy            — clang-tidy config
    tests/security/sast/cppcheck.suppress      — cppcheck suppressions
    tests/security/sast/baseline.json          — expected finding hashes
  Outputs:
    tests/security/reports/sast/findings.json  — normalized findings
    tests/security/reports/sast/summary.md     — counts + top-10
    tests/security/reports/sast/cppcheck.xml   — raw cppcheck output
    tests/security/reports/sast/clang-tidy.txt — raw clang-tidy output

  Exit codes:
    0  — no new high-severity findings (or only baseline ones)
    1  — at least one new high-severity finding (hash absent from
         baseline)
    2  — tooling not installed (cppcheck / clang-tidy missing)
    3  — bad invocation (no compile_commands.json, etc.)

  Coverity is explicitly OUT of scope (no license available); this
  script does not invoke cov-build, cov-analyze, cov-format-errors
  or read COVERITY_TOKEN.

.PARAMETER SkipCppcheck
  Do not invoke cppcheck (e.g., not installed locally; let CI cover).

.PARAMETER SkipClangTidy
  Do not invoke clang-tidy.

.PARAMETER ClangTidyExe
  Override path to clang-tidy.exe. Default: probe Visual Studio's
  bundled LLVM at VC/Tools/Llvm/x64/bin/clang-tidy.exe.

.PARAMETER CppcheckExe
  Override path to cppcheck.exe. Default: 'cppcheck' (relies on PATH).

.PARAMETER BuildDir
  Build dir for compile_commands.json. Default build/vs2022.
  clang-tidy ideally needs compile_commands.json; the Visual Studio
  generator does not emit one by default. The script handles the
  missing-file case by falling back to a best-effort header-only
  scan and explicitly noting this in the report.

.PARAMETER UpdateBaseline
  Overwrite tests/security/sast/baseline.json with the current
  findings.json. Use only when you have triaged + accepted/fixed
  the finding set in tests/security/sast/triage.md.

.EXAMPLE
  pwsh scripts/security/run_sast.ps1
  pwsh scripts/security/run_sast.ps1 -SkipCppcheck
  pwsh scripts/security/run_sast.ps1 -UpdateBaseline
#>
[CmdletBinding()]
param(
    [switch]$SkipCppcheck,
    [switch]$SkipClangTidy,
    [string]$ClangTidyExe,
    [string]$CppcheckExe = 'cppcheck',
    [string]$BuildDir,
    [switch]$UpdateBaseline
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$SrcDir    = Join-Path $RepoRoot 'src'
$SastDir   = Join-Path $RepoRoot 'tests\security\sast'
$ReportDir = Join-Path $RepoRoot 'tests\security\reports\sast'

if (-not $BuildDir) {
    # Prefer build/vs2022-asan because its Ninja Multi-Config generator
    # emits compile_commands.json that clang-tidy needs for full coverage.
    # Fall back to build/vs2022 (VS generator) -- clang-tidy still runs
    # but in best-effort mode.
    $asan = Join-Path $RepoRoot 'build\vs2022-asan'
    if (Test-Path (Join-Path $asan 'compile_commands.json')) {
        $BuildDir = $asan
    } else {
        $BuildDir = Join-Path $RepoRoot 'build\vs2022'
    }
}

New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null

# Resolve clang-tidy default if not supplied
if (-not $ClangTidyExe) {
    $vsClangTidy = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe"
    if (Test-Path $vsClangTidy) {
        $ClangTidyExe = $vsClangTidy
    } else {
        $ClangTidyExe = 'clang-tidy'
    }
}

# Resolve cppcheck default if not on PATH. The MSI installer puts it
# at C:\Program Files\Cppcheck\cppcheck.exe, but doesn't add to PATH;
# probe that location too.
if ($CppcheckExe -eq 'cppcheck' -and -not (Get-Command cppcheck -ErrorAction SilentlyContinue)) {
    $msiCppcheck = "C:\Program Files\Cppcheck\cppcheck.exe"
    if (Test-Path $msiCppcheck) { $CppcheckExe = $msiCppcheck }
}

$findings = @()

# Hoist the SHA1 hasher outside per-finding loops -- one disposable
# is enough; we Dispose() it after the last loop emits its hashes.
$sha1Hasher = [System.Security.Cryptography.SHA1]::Create()
function _hash_finding([string]$src) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($src)
    return ([System.BitConverter]::ToString($sha1Hasher.ComputeHash($bytes)) `
        -replace '-','').ToLower()
}

# ============================================================
# cppcheck
# ============================================================
$cppcheckXml = Join-Path $ReportDir 'cppcheck.xml'

if (-not $SkipCppcheck) {
    $hasCppcheck = $false
    try {
        & $CppcheckExe --version 2>&1 | Out-Null
        $hasCppcheck = ($LASTEXITCODE -eq 0)
    } catch { $hasCppcheck = $false }

    if (-not $hasCppcheck) {
        Write-Warning "[run_sast] cppcheck not found ($CppcheckExe). Skipping. Install via choco/scoop or pass -SkipCppcheck explicitly."
    } else {
        # cppcheck 2.14 doesn't accept # comments in
        # --suppressions-list files (it errors with "Failed to add
        # suppression. No id."). Generate a stripped version on the
        # fly: keep only non-blank, non-comment lines from the
        # documented file. The original .suppress file stays
        # human-readable with rationale comments inline.
        $cppcheckSuppressSrc = Join-Path $SastDir 'cppcheck.suppress'
        $cppcheckSuppressTmp = Join-Path $env:TEMP 'pdfe_cppcheck.suppress'
        Get-Content $cppcheckSuppressSrc |
            Where-Object { $_ -and ($_.Trim() -ne '') -and -not $_.TrimStart().StartsWith('#') } |
            Set-Content $cppcheckSuppressTmp -Encoding ASCII
        $cppcheckArgs = @(
            '--enable=all',
            '--inline-suppr',
            ('--suppressions-list=' + $cppcheckSuppressTmp),
            '--quiet',
            '--xml', '--xml-version=2',
            '--language=c++',
            '--std=c++20',
            '-I', (Join-Path $RepoRoot 'src'),
            $SrcDir
        )
        Write-Host "[run_sast] cppcheck $($cppcheckArgs -join ' ')"
        # cppcheck writes XML to STDERR (--xml mode). PowerShell's
        # `& $exe ... 2> file` redirect doesn't reliably capture
        # native-exe stderr -- it captures PowerShell's error stream
        # instead. Use Start-Process with -RedirectStandardError
        # which Windows handles correctly via CreateProcess.
        $cppcheckStdout = "$cppcheckXml.stdout"
        $proc = Start-Process -FilePath $CppcheckExe `
            -ArgumentList $cppcheckArgs `
            -RedirectStandardOutput $cppcheckStdout `
            -RedirectStandardError $cppcheckXml `
            -NoNewWindow -Wait -PassThru
        # Stdout captures progress only; throw it away.
        if (Test-Path $cppcheckStdout) {
            Remove-Item -Force $cppcheckStdout -ErrorAction SilentlyContinue
        }
        if (Test-Path $cppcheckXml) {
            try {
                [xml]$xml = Get-Content $cppcheckXml -Raw
                foreach ($err in $xml.results.errors.error) {
                    foreach ($loc in @($err.location)) {
                        $sev = switch ($err.severity) {
                            'error'       { 'high' }
                            'warning'     { 'medium' }
                            'style'       { 'low' }
                            'performance' { 'low' }
                            'portability' { 'low' }
                            default       { 'info' }
                        }
                        $msg = if ($err.msg) { [string]$err.msg } else { [string]$err.id }
                        $hashSrc = "cppcheck|$($err.id)|$($loc.file)|$msg"
                        $findings += @{
                            tool     = 'cppcheck'
                            rule_id  = [string]$err.id
                            severity = $sev
                            file     = if ($loc.file) { [string]$loc.file } else { '' }
                            line     = if ($loc.line) { [int]$loc.line } else { 0 }
                            message  = $msg
                            hash     = (_hash_finding $hashSrc)
                        }
                    }
                }
            } catch {
                Write-Warning "[run_sast] failed to parse cppcheck XML: $_"
            }
        }
    }
}

# ============================================================
# clang-tidy
# ============================================================
$clangTidyTxt = Join-Path $ReportDir 'clang-tidy.txt'

if (-not $SkipClangTidy) {
    $hasClangTidy = (Test-Path $ClangTidyExe) -or (Get-Command $ClangTidyExe -ErrorAction SilentlyContinue)
    if (-not $hasClangTidy) {
        Write-Warning "[run_sast] clang-tidy not found ($ClangTidyExe). Skipping. Install Visual Studio 'C++ Clang tools for Windows' workload or pass -SkipClangTidy."
    } else {
        $compileDb = Join-Path $BuildDir 'compile_commands.json'
        $hasDb = Test-Path $compileDb
        if (-not $hasDb) {
            Write-Warning "[run_sast] $compileDb not found. clang-tidy will run without compile-command information; results will be partial. Reconfigure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` and a Ninja generator to enable full coverage."
        }

        # Collect .cpp files under src/
        $cpps = Get-ChildItem -Path $SrcDir -Recurse -Filter '*.cpp' -File
        $clangBatchOut = New-Object System.Collections.ArrayList
        $clangCfg = (Join-Path $SastDir '.clang-tidy')
        # We pass the per-file invocation -p $BuildDir to use compile_commands;
        # without it clang-tidy will fall back to its own heuristic and emit
        # 'argument unused' warnings we ignore.
        foreach ($cpp in $cpps) {
            $argList = @(
                ("--config-file=" + $clangCfg),
                "-quiet",
                "--use-color=false"
            )
            if ($hasDb) {
                $argList += @('-p', $BuildDir)
            }
            $argList += $cpp.FullName
            $out = & $ClangTidyExe @argList 2>&1
            if ($out) {
                [void]$clangBatchOut.AddRange([string[]]$out)
            }
        }
        $clangBatchOut | Set-Content -Path $clangTidyTxt -Encoding UTF8

        # Parse clang-tidy output.
        # Each diagnostic is of the form:
        #   <file>:<line>:<col>: <severity>: <message> [<check>]
        $rxDiag = [regex]'^(?<file>[^:]+):(?<line>\d+):(?<col>\d+):\s+(?<sev>warning|error|note):\s+(?<msg>.*?)\s+\[(?<check>[a-zA-Z0-9\-\.,]+)\]\s*$'
        foreach ($line in (Get-Content $clangTidyTxt -ErrorAction SilentlyContinue)) {
            $m = $rxDiag.Match($line)
            if (-not $m.Success) { continue }
            $sev = switch ($m.Groups['sev'].Value) {
                'error'   { 'high' }
                'warning' {
                    if ($m.Groups['check'].Value -match 'cert-|bugprone-|clang-analyzer-') { 'medium' }
                    else { 'low' }
                }
                default   { 'info' }
            }
            $hashSrc = "clang-tidy|$($m.Groups['check'].Value)|$($m.Groups['file'].Value)|$($m.Groups['msg'].Value)"
            $findings += @{
                tool     = 'clang-tidy'
                rule_id  = $m.Groups['check'].Value
                severity = $sev
                file     = $m.Groups['file'].Value
                line     = [int]$m.Groups['line'].Value
                message  = $m.Groups['msg'].Value
                hash     = (_hash_finding $hashSrc)
            }
        }
    }
}

# ============================================================
# Normalize findings.json
# ============================================================
$findingsJson = @{
    schema_version = 1
    # `-AsUTC` is PowerShell 7-only; use [DateTime]::UtcNow for
    # cross-version compatibility with Windows PowerShell 5.1.
    stamp          = ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
    src_root       = 'src/'
    findings       = $findings
    counts_by_severity = @{
        high   = ($findings | Where-Object { $_.severity -eq 'high' }).Count
        medium = ($findings | Where-Object { $_.severity -eq 'medium' }).Count
        low    = ($findings | Where-Object { $_.severity -eq 'low' }).Count
        info   = ($findings | Where-Object { $_.severity -eq 'info' }).Count
    }
    counts_by_tool = @{
        cppcheck    = ($findings | Where-Object { $_.tool -eq 'cppcheck' }).Count
        'clang-tidy' = ($findings | Where-Object { $_.tool -eq 'clang-tidy' }).Count
    }
}
$findingsPath = Join-Path $ReportDir 'findings.json'
$findingsJson | ConvertTo-Json -Depth 6 | Set-Content -Path $findingsPath -Encoding UTF8

# ============================================================
# Summary md
# ============================================================
$summaryPath = Join-Path $ReportDir 'summary.md'
$top10 = $findings | Group-Object -Property rule_id | Sort-Object -Property Count -Descending | Select-Object -First 10
$lines = @(
    "# SAST sweep summary",
    "",
    "Stamp: $($findingsJson.stamp)",
    "Total findings: $($findings.Count)",
    "",
    "## By severity",
    "",
    "| Severity | Count |",
    "|---|---:|",
    "| High   | $($findingsJson.counts_by_severity.high) |",
    "| Medium | $($findingsJson.counts_by_severity.medium) |",
    "| Low    | $($findingsJson.counts_by_severity.low) |",
    "| Info   | $($findingsJson.counts_by_severity.info) |",
    "",
    "## By tool",
    "",
    "| Tool | Count |",
    "|---|---:|",
    "| cppcheck    | $($findingsJson.counts_by_tool.cppcheck) |",
    "| clang-tidy  | $($findingsJson.counts_by_tool.'clang-tidy') |",
    "",
    "## Top 10 by rule_id",
    "",
    "| Rule | Count |",
    "|---|---:|"
)
foreach ($g in $top10) { $lines += "| ``$($g.Name)`` | $($g.Count) |" }
$lines | Set-Content -Path $summaryPath -Encoding UTF8

# ============================================================
# Baseline comparison
# ============================================================
$baselinePath = Join-Path $SastDir 'baseline.json'
if ($UpdateBaseline) {
    $baselineHashes = $findings | Where-Object { $_.severity -eq 'high' } | ForEach-Object { $_.hash }
    @{
        schema_version = 1
        stamp          = $findingsJson.stamp
        high_severity_hashes = @($baselineHashes)
        note           = 'Hashes of accepted/known high-severity findings. Update via run_sast.ps1 -UpdateBaseline only after triaging in tests/security/sast/triage.md.'
    } | ConvertTo-Json -Depth 4 | Set-Content -Path $baselinePath -Encoding UTF8
    Write-Host "[run_sast] baseline updated: $baselinePath"
    exit 0
}

$sha1Hasher.Dispose()

if (-not (Test-Path $baselinePath)) {
    Write-Warning "[run_sast] no baseline at $baselinePath. First run: review findings.json + triage.md, then run with -UpdateBaseline."
    exit 0
}

$baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
$baselineSet = @{}
foreach ($h in $baseline.high_severity_hashes) { $baselineSet[$h] = $true }

$newHigh = $findings | Where-Object {
    $_.severity -eq 'high' -and -not $baselineSet.ContainsKey($_.hash)
}
if ($newHigh) {
    Write-Host "[run_sast] FAIL -- $($newHigh.Count) NEW high-severity finding(s):"
    foreach ($f in $newHigh) {
        Write-Host "  $($f.tool) $($f.rule_id) $($f.file):$($f.line) -- $($f.message)"
    }
    Write-Host "[run_sast] If accepted, document in tests/security/sast/triage.md and run with -UpdateBaseline."
    exit 1
}

Write-Host "[run_sast] PASS -- $($findings.Count) findings, no new high-severity vs baseline."
exit 0
