#requires -Version 5.1
<#
.SYNOPSIS
  Phase-9 security regression check. Compares fresh outputs from
  phases 2-8 against the canonical baselines under
  tests/security/regression_baselines/, exits non-zero on any new
  high-severity item.

.DESCRIPTION
  This is the script registered as the security CTest entry
  `security_regression_check`. It is the durable gate that prevents
  later commits from silently regressing what phases 2-8 made
  visible.

  The script does NOT itself run gitleaks / cppcheck / cargo-audit
  / ctest-with-asan. Those are slow, tool-dependent campaigns
  invoked manually or by the nightly target. What this script
  does is compare WHATEVER fresh outputs are present under
  tests/security/reports/<phase>/ to the per-phase baselines.

  If a fresh output is absent for a given phase (because the
  scanner isn't installed, or the live run hasn't been done since
  the baseline was set), the phase is labeled SKIPPED rather than
  FAIL. SKIPPED phases do not fail the gate -- the dispatcher
  is reporting honestly that no comparison was possible.

  The OPT-IN invariant verification (phase-1 verify_optin.ps1) is
  registered separately in the CTest 'security' label and runs
  alongside this check.

  Outputs:
    tests/security/reports/regression_check.md    summary report
    tests/security/reports/regression_check.json  machine-readable

  Exit codes:
    0 -- no new high-severity items vs any committed baseline
    1 -- at least one regression detected
    2 -- preflight failure (no baselines committed, etc.)

.PARAMETER Phase
  Limit the check to one phase: corpus | fuzz | sast | deps | sanitizers | secrets

.EXAMPLE
  pwsh scripts/security/check_regressions.ps1
  pwsh scripts/security/check_regressions.ps1 -Phase secrets
#>
[CmdletBinding()]
param(
    [ValidateSet('all','corpus','fuzz','sast','deps','sanitizers','secrets')]
    [string]$Phase = 'all'
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$BaselineDir = Join-Path $RepoRoot 'tests\security\regression_baselines'
$ReportDir   = Join-Path $RepoRoot 'tests\security\reports'

function _bail([int]$code, [string]$msg) {
    [Console]::Error.WriteLine($msg)
    exit $code
}

if (-not (Test-Path $BaselineDir)) {
    _bail 2 "[check_regressions] $BaselineDir not found"
}

$results = @()

# ============================================================
# Helper: load JSON or null
# ============================================================
function _load_json([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    try { return (Get-Content $path -Raw | ConvertFrom-Json) }
    catch { return $null }
}

# Helper: register a phase result
function _add_result($phase, $verdict, $newItems, $note) {
    $script:results += @{
        phase     = $phase
        verdict   = $verdict      # pass | fail | skipped
        new_items = @($newItems)
        note      = $note
    }
}

# ============================================================
# Phase 3 corpus: tests/security/reports/corpus_run_*.json (latest)
# ============================================================
if ($Phase -in @('all','corpus')) {
    $baseline = _load_json (Join-Path $BaselineDir 'corpus_run_baseline.json')
    if (-not $baseline) {
        _add_result 'corpus' 'skipped' @() 'no baseline committed yet'
    } else {
        $latest = Get-ChildItem -Path $ReportDir -Filter 'corpus_run_*.json' -ErrorAction SilentlyContinue |
                  Sort-Object -Property LastWriteTime -Descending | Select-Object -First 1
        if (-not $latest) {
            _add_result 'corpus' 'skipped' @() 'no fresh corpus_run_*.json under reports/'
        } else {
            $fresh = _load_json $latest.FullName
            $newFails = $fresh.records | Where-Object { $_.final_decision -eq 'fail' }
            if ($newFails -and $newFails.Count -gt ($baseline.records | Where-Object { $_.final_decision -eq 'fail' }).Count) {
                _add_result 'corpus' 'fail' $newFails 'new corpus failures vs baseline'
            } else {
                _add_result 'corpus' 'pass' @() ''
            }
        }
    }
}

# ============================================================
# Phase 4 fuzz: tests/security/reports/fuzz/run_<stamp>/summary.json
# ============================================================
if ($Phase -in @('all','fuzz')) {
    $baseline = _load_json (Join-Path $BaselineDir 'fuzz_baseline.json')
    if (-not $baseline) {
        _add_result 'fuzz' 'skipped' @() 'no baseline committed yet'
    } else {
        $latest = Get-ChildItem -Path (Join-Path $ReportDir 'fuzz') -Filter 'run_*' -Directory -ErrorAction SilentlyContinue |
                  Sort-Object -Property LastWriteTime -Descending | Select-Object -First 1
        if (-not $latest) {
            _add_result 'fuzz' 'skipped' @() 'no fresh fuzz run dir'
        } else {
            $fresh = _load_json (Join-Path $latest.FullName 'summary.json')
            if ($fresh -and $fresh.harnesses) {
                $newCrashes = $fresh.harnesses | ForEach-Object { $_.unique_crashes } |
                              Measure-Object -Sum
                if ($newCrashes.Sum -gt 0) {
                    _add_result 'fuzz' 'fail' $fresh.harnesses 'new fuzz crashes'
                } else {
                    _add_result 'fuzz' 'pass' @() ''
                }
            } elseif (-not (Test-Path (Join-Path $latest.FullName 'summary.json'))) {
                # ABSENT is not MALFORMED. The two notes used to be one string,
                # so a run directory that simply had no summary.json was
                # reported as a corrupt one -- sending a reader to debug a
                # parser when the real answer is "that phase never wrote its
                # summary". GitHub #201.
                _add_result 'fuzz' 'skipped' @() 'no summary.json in latest fuzz run dir'
            } else {
                _add_result 'fuzz' 'skipped' @() 'malformed fresh summary.json'
            }
        }
    }
}

# ============================================================
# Phase 5 SAST
# ============================================================
if ($Phase -in @('all','sast')) {
    $baseline = _load_json (Join-Path $BaselineDir 'sast_baseline.json')
    $fresh = _load_json (Join-Path $ReportDir 'sast\findings.json')
    if (-not $baseline) {
        _add_result 'sast' 'skipped' @() 'no baseline committed yet'
    } elseif (-not $fresh) {
        _add_result 'sast' 'skipped' @() 'no fresh findings.json (run_sast.ps1 not run)'
    } else {
        $baseHashes = @{}
        foreach ($h in @($baseline.high_severity_hashes)) {
            if ($h) { $baseHashes[$h] = $true }
        }
        $newHigh = @($fresh.findings | Where-Object {
            $_.severity -eq 'high' -and -not $baseHashes.ContainsKey($_.hash)
        })
        if ($newHigh.Count -gt 0) {
            _add_result 'sast' 'fail' $newHigh "$($newHigh.Count) new high-sev SAST findings"
        } else {
            _add_result 'sast' 'pass' @() ''
        }
    }
}

# ============================================================
# Phase 6 deps
# ============================================================
if ($Phase -in @('all','deps')) {
    $baseline = _load_json (Join-Path $BaselineDir 'deps_baseline.json')
    $fresh = _load_json (Join-Path $ReportDir 'deps\cves.json')
    if (-not $baseline) {
        _add_result 'deps' 'skipped' @() 'no baseline committed yet'
    } elseif (-not $fresh) {
        _add_result 'deps' 'skipped' @() 'no fresh cves.json (run_deps.ps1 not run)'
    } else {
        $baseSet = @{}
        foreach ($id in @($baseline.cve_ids_ge_7)) {
            if ($id) { $baseSet[$id] = $true }
        }
        $newHigh = @($fresh.findings | Where-Object {
            [double]$_.cvss -ge 7.0 -and -not $baseSet.ContainsKey($_.cve_id)
        })
        if ($newHigh.Count -gt 0) {
            _add_result 'deps' 'fail' $newHigh "$($newHigh.Count) new CVSS>=7 CVE(s)"
        } else {
            _add_result 'deps' 'pass' @() ''
        }
    }
}

# ============================================================
# Phase 7 sanitizers
# ============================================================
if ($Phase -in @('all','sanitizers')) {
    $baseline = _load_json (Join-Path $BaselineDir 'sanitizers_baseline.json')
    $latest = Get-ChildItem -Path (Join-Path $ReportDir 'sanitizers') -Filter 'run_*' -Directory -ErrorAction SilentlyContinue |
              Sort-Object -Property LastWriteTime -Descending | Select-Object -First 1
    if (-not $baseline) {
        _add_result 'sanitizers' 'skipped' @() 'no baseline committed yet'
    } elseif (-not $latest) {
        _add_result 'sanitizers' 'skipped' @() 'no fresh sanitizer run dir'
    } else {
        $freshPath = Join-Path $latest.FullName 'findings.json'
        $fresh = _load_json $freshPath
        if (-not (Test-Path $freshPath)) {
            # ABSENT is not MALFORMED -- see the fuzz branch above. Measured on
            # 2026-09-18: the newest sanitizer run dir on a developer box held
            # only results.xml/stdout.log, and this branch reported it as a
            # corrupt findings.json for months. GitHub #201.
            _add_result 'sanitizers' 'skipped' @() 'no findings.json in latest sanitizer run dir'
        } elseif (-not $fresh) {
            _add_result 'sanitizers' 'skipped' @() 'malformed fresh findings.json'
        } else {
            $baseSet = @{}
            foreach ($h in @($baseline.suppressed_hashes)) {
                if ($h) { $baseSet[$h] = $true }
            }
            $unsuppressed = @($fresh.findings | Where-Object { -not $baseSet.ContainsKey($_.hash) })
            if ($unsuppressed.Count -gt 0) {
                _add_result 'sanitizers' 'fail' $unsuppressed "$($unsuppressed.Count) non-suppressed sanitizer finding(s)"
            } else {
                _add_result 'sanitizers' 'pass' @() ''
            }
        }
    }
}

# ============================================================
# Phase 8 secrets
# ============================================================
if ($Phase -in @('all','secrets')) {
    $baseline = _load_json (Join-Path $BaselineDir 'secrets_baseline.json')
    $fresh = _load_json (Join-Path $ReportDir 'secrets\findings.json')
    if (-not $baseline) {
        _add_result 'secrets' 'skipped' @() 'no baseline committed yet'
    } elseif (-not $fresh) {
        _add_result 'secrets' 'skipped' @() 'no fresh findings.json (run_secrets.ps1 not run)'
    } else {
        $baseSet = @{}
        foreach ($e in @($baseline.accepted_findings)) {
            if ($e -and $e.hash) { $baseSet[$e.hash] = $true }
        }
        foreach ($h in @($baseline.accepted_hashes)) {
            if ($h) { $baseSet[$h] = $true }
        }
        $unaccepted = @($fresh.findings | Where-Object { -not $baseSet.ContainsKey($_.hash) })
        if ($unaccepted.Count -gt 0) {
            _add_result 'secrets' 'fail' $unaccepted "$($unaccepted.Count) unaccepted secrets finding(s)"
        } else {
            _add_result 'secrets' 'pass' @() ''
        }
    }
}

# ============================================================
# Aggregate + emit reports
# ============================================================
$overallFail = ($results | Where-Object { $_.verdict -eq 'fail' }).Count -gt 0
$overall = if ($overallFail) { 'fail' } else { 'pass' }

$summary = @{
    schema_version  = 1
    # `-AsUTC` is PowerShell 7-only; use [DateTime]::UtcNow for
    # cross-version compatibility with Windows PowerShell 5.1.
    stamp           = ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
    overall_verdict = $overall
    results         = $results
}
$jsonPath = Join-Path $ReportDir 'regression_check.json'
$summary | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding UTF8

$mdPath = Join-Path $ReportDir 'regression_check.md'
$lines = @(
    '# Phase-9 security regression check',
    '',
    "Stamp: $($summary.stamp)",
    "Overall: **$($overall.ToUpper())**",
    '',
    '| Phase | Verdict | Note |',
    '|---|---|---|'
)
foreach ($r in $results) { $lines += "| $($r.phase) | $($r.verdict) | $($r.note) |" }
$lines | Set-Content -Path $mdPath -Encoding UTF8

Write-Host "[check_regressions] verdict=$overall"
Write-Host "[check_regressions] $mdPath"
foreach ($r in $results) {
    Write-Host ("  {0,-12} {1,-7} {2}" -f $r.phase, $r.verdict, $r.note)
}
if ($overall -eq 'fail') { exit 1 } else { exit 0 }
