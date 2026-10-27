#requires -Version 5.1
<#
.SYNOPSIS
  Phase-8 full-history secrets scan. Runs gitleaks + trufflehog
  across the entire git history, normalizes findings into JSON,
  compares against a baseline, exits non-zero on any unaccepted
  finding.

.DESCRIPTION
  Phase 8 is OPT-IN: invoked manually or by phase-9 nightly.
  Where the existing security-audit hook (used between pipeline
  phases) only scans the current tree, this dispatcher walks the
  whole git log -- catching secrets that were committed earlier
  and then deleted. A removed secret is still leaked if its blob
  is in history.

  Outputs:
    tests/security/reports/secrets/findings.json   normalized
    tests/security/reports/secrets/summary.md      counts + actions
    tests/security/reports/secrets/gitleaks.json   raw gitleaks
    tests/security/reports/secrets/trufflehog.json raw trufflehog

  Exit codes:
    0 -- only baseline-accepted findings (or zero findings)
    1 -- at least one new finding outside baseline (true positive
         or new false positive needing triage)
    2 -- preflight failure (no git, no gitleaks AND no trufflehog,
         repo doesn't have a .git directory)

.PARAMETER GitleaksExe
  Path to gitleaks binary. Default: 'gitleaks' (PATH).

.PARAMETER TrufflehogExe
  Path to trufflehog binary. Default: 'trufflehog' (PATH).

.PARAMETER SkipGitleaks
  Skip gitleaks (e.g., not installed locally; rely on trufflehog).

.PARAMETER SkipTrufflehog
  Skip trufflehog.

.PARAMETER UpdateBaseline
  Replace tests/security/secrets/baseline.json with current
  findings hashes. Use only after triaging in triage.md.

.EXAMPLE
  pwsh scripts/security/run_secrets.ps1
  pwsh scripts/security/run_secrets.ps1 -SkipTrufflehog
  pwsh scripts/security/run_secrets.ps1 -UpdateBaseline

.NOTES
  This dispatcher is local-only -- no telemetry, no upload of
  secret material to any external service. trufflehog has an
  optional verification mode that calls out to the actual
  service to confirm secrets are valid; we deliberately do NOT
  enable it (`--no-verification` not needed because the default
  is non-verifying as of trufflehog v3).
#>
[CmdletBinding()]
param(
    [string]$GitleaksExe = 'gitleaks',
    [string]$TrufflehogExe = 'trufflehog',
    [switch]$SkipGitleaks,
    [switch]$SkipTrufflehog,
    [switch]$UpdateBaseline
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$SecretsDir  = Join-Path $RepoRoot 'tests\security\secrets'
$ReportDir   = Join-Path $RepoRoot 'tests\security\reports\secrets'
$ConfigToml  = Join-Path $SecretsDir '.gitleaks.toml'

# Resolve gitleaks/trufflehog defaults. install_tools.ps1 places these
# at <repo>/tools/bin/ but doesn't modify PATH for the current
# process; probe that location if PATH lookup fails.
if ($GitleaksExe -eq 'gitleaks' -and -not (Get-Command gitleaks -ErrorAction SilentlyContinue)) {
    $localGl = Join-Path $RepoRoot 'tools\bin\gitleaks.exe'
    if (Test-Path $localGl) { $GitleaksExe = $localGl }
}
if ($TrufflehogExe -eq 'trufflehog' -and -not (Get-Command trufflehog -ErrorAction SilentlyContinue)) {
    $localTh = Join-Path $RepoRoot 'tools\bin\trufflehog.exe'
    if (Test-Path $localTh) { $TrufflehogExe = $localTh }
}

function _bail([int]$code, [string]$msg) {
    [Console]::Error.WriteLine($msg)
    exit $code
}

if (-not (Test-Path (Join-Path $RepoRoot '.git'))) {
    _bail 2 "[run_secrets] $RepoRoot does not contain a .git directory"
}
New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null

$findings = @()

# Hoist a SHA1 hasher; deterministic dedup keys.
$sha1 = [System.Security.Cryptography.SHA1]::Create()
function _hash([string]$src) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($src)
    return ([System.BitConverter]::ToString($sha1.ComputeHash($bytes)) -replace '-','').ToLower()
}

# ============================================================
# gitleaks
# ============================================================
$gitleaksJson = Join-Path $ReportDir 'gitleaks.json'
$ranGitleaks = $false
if (-not $SkipGitleaks) {
    $hasGitleaks = $false
    try {
        # Native exe version check via Start-Process avoids the
        # PS5.1 NativeCommandError trap on stderr banners.
        # Start-Process refuses identical stdout/stderr targets,
        # so use two temp files.
        $glVerOut = Join-Path $env:TEMP "pdfe_gitleaks_ver_o.txt"
        $glVerErr = Join-Path $env:TEMP "pdfe_gitleaks_ver_e.txt"
        $glVerProc = Start-Process -FilePath $GitleaksExe `
            -ArgumentList @('version') -Wait -PassThru -NoNewWindow `
            -RedirectStandardOutput $glVerOut -RedirectStandardError $glVerErr
        Remove-Item $glVerOut, $glVerErr -ErrorAction SilentlyContinue
        $hasGitleaks = ($glVerProc.ExitCode -eq 0)
    } catch {}
    if (-not $hasGitleaks) {
        Write-Warning "[run_secrets] gitleaks not found ($GitleaksExe). Install from https://github.com/gitleaks/gitleaks/releases or pass -SkipGitleaks."
    } else {
        Push-Location $RepoRoot
        try {
            $gitleaksArgs = @(
                'detect',
                '--source=.',
                '--config=' + $ConfigToml,
                '--report-format=json',
                '--report-path=' + $gitleaksJson,
                '--log-opts=--all',
                '--exit-code=0'   # don't propagate; we read findings ourselves
            )
            Write-Host "[run_secrets] gitleaks $($gitleaksArgs -join ' ')"
            # gitleaks writes banner/progress to stderr; with
            # `$ErrorActionPreference = 'Stop'` PS5.1 escalates that
            # to a NativeCommandError. Use Start-Process with
            # explicit redirection instead so stderr is captured to
            # a discard file without triggering the trap.
            $glStderrTmp = Join-Path $env:TEMP "pdfe_gitleaks_stderr.txt"
            $glStdoutTmp = Join-Path $env:TEMP "pdfe_gitleaks_stdout.txt"
            $glProc = Start-Process -FilePath $GitleaksExe `
                -ArgumentList $gitleaksArgs `
                -Wait -PassThru -NoNewWindow `
                -RedirectStandardOutput $glStdoutTmp `
                -RedirectStandardError $glStderrTmp
            Remove-Item $glStdoutTmp, $glStderrTmp -ErrorAction SilentlyContinue
            $ranGitleaks = $true
        } finally { Pop-Location }

        if (Test-Path $gitleaksJson) {
            try {
                $glDoc = Get-Content $gitleaksJson -Raw | ConvertFrom-Json
                foreach ($f in @($glDoc)) {
                    $kind = switch -Regex ($f.RuleID) {
                        'private[-_]key|rsa|pem'        { 'private_key' }
                        'aws|gcp|azure|github|stripe'   { 'api_key' }
                        'token|jwt|bearer'              { 'token' }
                        'password|passwd|pwd'           { 'password' }
                        default                         { 'other' }
                    }
                    $entropy = if ($f.Entropy) { [double]$f.Entropy } else { 0.0 }
                    $hashSrc = "gitleaks|$($f.RuleID)|$($f.File)|$($f.Commit)|$($f.Match)"
                    $findings += @{
                        tool        = 'gitleaks'
                        rule_id     = [string]$f.RuleID
                        file        = [string]$f.File
                        line        = if ($f.StartLine) { [int]$f.StartLine } else { 0 }
                        commit      = [string]$f.Commit
                        secret_kind = $kind
                        entropy     = $entropy
                        hash        = (_hash $hashSrc)
                    }
                }
            } catch {
                Write-Warning "[run_secrets] failed to parse gitleaks output: $_"
            }
        }
    }
}

# ============================================================
# trufflehog
# ============================================================
$truffleJson = Join-Path $ReportDir 'trufflehog.json'
$ranTrufflehog = $false
if (-not $SkipTrufflehog) {
    $hasTrufflehog = $false
    try {
        # Same Start-Process pattern as gitleaks above: trufflehog
        # banner-prints to stderr, which trips PS5.1's
        # NativeCommandError under $ErrorActionPreference='Stop'
        # even though --version exits 0. Two temp files because
        # Start-Process refuses identical stdout/stderr targets.
        $thVerOut = Join-Path $env:TEMP "pdfe_trufflehog_ver_o.txt"
        $thVerErr = Join-Path $env:TEMP "pdfe_trufflehog_ver_e.txt"
        $thVerProc = Start-Process -FilePath $TrufflehogExe `
            -ArgumentList @('--version') -Wait -PassThru -NoNewWindow `
            -RedirectStandardOutput $thVerOut -RedirectStandardError $thVerErr
        Remove-Item $thVerOut, $thVerErr -ErrorAction SilentlyContinue
        $hasTrufflehog = ($thVerProc.ExitCode -eq 0)
    } catch {}
    if (-not $hasTrufflehog) {
        Write-Warning "[run_secrets] trufflehog not found ($TrufflehogExe). Install from https://github.com/trufflesecurity/trufflehog/releases or pass -SkipTrufflehog."
    } else {
        Push-Location $RepoRoot
        try {
            # Walk all of history. trufflehog v3 emits one JSON object
            # per finding on stdout; redirect stdout to file and stderr
            # to a discard file (banner/progress text otherwise trips
            # PS5.1's NativeCommandError trap under
            # $ErrorActionPreference = 'Stop').
            Write-Host "[run_secrets] trufflehog git file://. --json"
            $thStderrTmp = Join-Path $env:TEMP "pdfe_trufflehog_stderr.txt"
            $thArgs = @('git', ('file://' + $RepoRoot), '--json', '--no-update')
            $thProc = Start-Process -FilePath $TrufflehogExe `
                -ArgumentList $thArgs `
                -Wait -PassThru -NoNewWindow `
                -RedirectStandardOutput $truffleJson `
                -RedirectStandardError $thStderrTmp
            Remove-Item $thStderrTmp -ErrorAction SilentlyContinue
            $ranTrufflehog = $true
        } finally { Pop-Location }

        if (Test-Path $truffleJson) {
            try {
                foreach ($line in (Get-Content $truffleJson)) {
                    if (-not $line.Trim()) { continue }
                    $f = $null
                    try { $f = $line | ConvertFrom-Json } catch { continue }
                    if (-not $f.SourceMetadata) { continue }
                    $gitMeta = $f.SourceMetadata.Data.Git
                    if (-not $gitMeta) { continue }
                    $kind = switch -Regex ($f.DetectorName) {
                        'PrivateKey|RSAPrivateKey'  { 'private_key' }
                        'AWS|GCP|Azure|Github|Stripe|Slack' { 'api_key' }
                        'Token|JWT|Bearer'          { 'token' }
                        'Password'                  { 'password' }
                        default                     { 'other' }
                    }
                    $hashSrc = "trufflehog|$($f.DetectorName)|$($gitMeta.file)|$($gitMeta.commit)|$($f.Raw)"
                    $findings += @{
                        tool        = 'trufflehog'
                        rule_id     = [string]$f.DetectorName
                        file        = [string]$gitMeta.file
                        line        = if ($gitMeta.line) { [int]$gitMeta.line } else { 0 }
                        commit      = [string]$gitMeta.commit
                        secret_kind = $kind
                        entropy     = 0.0
                        hash        = (_hash $hashSrc)
                    }
                }
            } catch {
                Write-Warning "[run_secrets] failed to parse trufflehog output: $_"
            }
        }
    }
}

$sha1.Dispose()

# ============================================================
# Normalize findings.json
# ============================================================
if (-not $ranGitleaks -and -not $ranTrufflehog) {
    _bail 2 "[run_secrets] Neither gitleaks nor trufflehog ran. Install at least one scanner."
}

$findingsJson = @{
    schema_version = 1
    # `-AsUTC` is PS7-only; [DateTime]::UtcNow works on PS 5.1.
    stamp          = ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
    scanners_run   = @{
        gitleaks   = $ranGitleaks
        trufflehog = $ranTrufflehog
    }
    findings       = $findings
    counts         = @{
        total           = $findings.Count
        by_secret_kind  = @{}
        by_tool         = @{
            gitleaks   = (@($findings | Where-Object { $_.tool -eq 'gitleaks' })).Count
            trufflehog = (@($findings | Where-Object { $_.tool -eq 'trufflehog' })).Count
        }
    }
}
foreach ($f in $findings) {
    $k = $f.secret_kind
    if (-not $findingsJson.counts.by_secret_kind.ContainsKey($k)) {
        $findingsJson.counts.by_secret_kind[$k] = 0
    }
    $findingsJson.counts.by_secret_kind[$k]++
}
$findingsPath = Join-Path $ReportDir 'findings.json'
$findingsJson | ConvertTo-Json -Depth 6 | Set-Content -Path $findingsPath -Encoding UTF8

# ============================================================
# Summary md (counts per rule + per author bucket)
# ============================================================
# Per-author bucket comes from `git show -s --format=%ae <commit>`
# for each unique commit. Cached so we don't fork git per finding.
$authorCache = @{}
function _author([string]$commit) {
    if (-not $commit) { return '<unknown>' }
    if ($authorCache.ContainsKey($commit)) { return $authorCache[$commit] }
    $a = (& git show -s --format=%ae $commit 2>$null)
    if (-not $a) { $a = '<unknown>' }
    $authorCache[$commit] = $a.Trim()
    return $authorCache[$commit]
}

$summaryPath = Join-Path $ReportDir 'summary.md'
$byRule = $findings | Group-Object -Property rule_id | Sort-Object -Property Count -Descending
$byAuthor = $findings | Where-Object { $_.commit } | ForEach-Object {
    @{ author = (_author $_.commit); finding = $_ }
} | Group-Object -Property { $_.author } | Sort-Object -Property Count -Descending

$lines = @(
    '# Secrets scan summary',
    '',
    "Stamp: $($findingsJson.stamp)",
    "Total findings: $($findings.Count)",
    "  gitleaks:   $($findingsJson.counts.by_tool.gitleaks)",
    "  trufflehog: $($findingsJson.counts.by_tool.trufflehog)",
    '',
    '## By rule_id',
    '',
    '| Rule | Count |',
    '|---|---:|'
)
foreach ($g in $byRule) { $lines += "| ``$($g.Name)`` | $($g.Count) |" }
$lines += @('','## By commit author','','| Author | Count |','|---|---:|')
foreach ($g in $byAuthor) { $lines += "| ``$($g.Name)`` | $($g.Count) |" }

$lines | Set-Content -Path $summaryPath -Encoding UTF8

# ============================================================
# Baseline comparison
# ============================================================
$baselinePath = Join-Path $SecretsDir 'baseline.json'
if ($UpdateBaseline) {
    # AC3: each accepted entry has a `rationale` field. We default to
    # 'see tests/security/secrets/triage.md' when -UpdateBaseline is
    # invoked without a triage update; the contributor is expected to
    # populate triage.md *first* and then update the per-entry
    # rationale here to a one-line summary referencing the triage
    # entry. We do NOT write bare-hash arrays.
    $existingRationales = @{}
    if (Test-Path $baselinePath) {
        try {
            $prev = Get-Content $baselinePath -Raw | ConvertFrom-Json
            foreach ($e in @($prev.accepted_findings)) {
                if ($e.hash) { $existingRationales[$e.hash] = $e.rationale }
            }
        } catch {}
    }
    $accepted = @($findings | ForEach-Object {
        $r = if ($existingRationales.ContainsKey($_.hash)) {
                $existingRationales[$_.hash]
             } else {
                'see tests/security/secrets/triage.md'
             }
        @{ hash = $_.hash; rationale = $r }
    })
    @{
        schema_version    = 1
        stamp             = $findingsJson.stamp
        accepted_findings = $accepted
        note              = 'Each entry: {hash, rationale}. Rationale points to the triage.md entry that justifies acceptance (false-positive / true-positive-rotated / true-positive-history-rewritten). Update via run_secrets.ps1 -UpdateBaseline ONLY after triaging.'
    } | ConvertTo-Json -Depth 5 | Set-Content -Path $baselinePath -Encoding UTF8
    Write-Host "[run_secrets] baseline updated: $baselinePath ($($accepted.Count) entries)"
    exit 0
}

if (-not (Test-Path $baselinePath)) {
    Write-Warning "[run_secrets] no baseline at $baselinePath. First run: review findings.json + triage.md, then -UpdateBaseline."
    if ($findings.Count -gt 0) {
        Write-Host "[run_secrets] FIRST RUN -- $($findings.Count) finding(s); triage in tests/security/secrets/triage.md before -UpdateBaseline."
        exit 1
    }
    exit 0
}

$baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
$baselineSet = @{}
# Backwards-compat: accept either the new 'accepted_findings' array
# of {hash, rationale} OR the legacy 'accepted_hashes' bare-string
# array. New runs always emit accepted_findings. Skip $null entries
# (the legacy field doesn't exist on schema-1 baselines, and
# `foreach ($h in @($null))` yields one $null iteration that would
# blow up at $baselineSet[$null]=$true).
foreach ($e in @($baseline.accepted_findings)) {
    if ($e -and $e.hash) { $baselineSet[$e.hash] = $true }
}
foreach ($h in @($baseline.accepted_hashes)) {
    if ($h) { $baselineSet[$h] = $true }
}

$unaccepted = $findings | Where-Object { -not $baselineSet.ContainsKey($_.hash) }
if ($unaccepted) {
    Write-Host "[run_secrets] FAIL -- $($unaccepted.Count) unaccepted finding(s):"
    foreach ($f in $unaccepted) {
        Write-Host "  $($f.tool) $($f.rule_id) ($($f.secret_kind)) $($f.file):$($f.line) commit=$($f.commit)"
    }
    Write-Host "[run_secrets] If a TRUE POSITIVE: rotate the secret AND rewrite history with `git filter-repo`. Document in tests/security/secrets/triage.md."
    Write-Host "[run_secrets] If a FALSE POSITIVE: add to .gitleaks.toml allowlist or document in triage.md and -UpdateBaseline."
    exit 1
}

Write-Host "[run_secrets] PASS -- $($findings.Count) total findings, all in baseline."
exit 0
