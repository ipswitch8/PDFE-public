<#
.SYNOPSIS
  Decide PASS/FAIL for an MSVC-ASan run, from artifacts only.

.DESCRIPTION
  SEC-21. This is the verdict logic that .github/workflows/windows-sanitizer.yml
  used to carry inline. It lives here so the suite can drive the EXACT code CI
  runs, with real ASan artifacts, instead of asserting things about YAML text.

  Why it was extracted (the fault injection that prompted it)
  -----------------------------------------------------------
  A deliberate heap-use-after-free was compiled with `cl /fsanitize=address` and
  run under this lane's exact ASAN_OPTIONS. Two facts came back:

    1. With `log_path` set, the ASan runtime writes its report to
       <base>.<pid> and NOT to stderr. The captured stdout contains ZERO
       "==<pid>==ERROR:" lines. The workflow comment described log_path as
       making ASan "ALSO" write to files and said "We scan BOTH" — but the
       second capture path did not join the first, it REPLACED it. The stdout
       scan has been dead since that hardening landed, so the documented
       redundancy had exactly one live source.

    2. That surviving source is CWD-coupled. `log_path` must be a relative,
       colon-free base (a drive-letter colon is an ASAN_OPTIONS separator and
       makes ASan abort), so the runtime resolves it against the PROCESS CWD.
       Scanning only $GITHUB_WORKSPACE finds the files solely because the step's
       CWD happens to equal it — an invariant nothing asserted.

  Both fixtures under tests/fixtures/sanitizer/ are the verbatim output of that
  run, not hand-written samples.

  The verdict itself is unchanged in spirit: any finding, or any non-zero gtest
  exit, or any failure/error in the JUnit XML, is FAIL. Two things are new:

    * -LogDir takes a LIST, so the report is found wherever ASan resolved the
      relative base, rather than in one directory by assumption.
    * A non-zero exit with zero collected findings is reported as an EVIDENCE
      GAP. That combination is what a real finding looks like when the report
      landed somewhere unscanned: the job still fails (ASan's default
      exitcode=1 promotes $rc), but the run's own summary says "findings: 0"
      and the report file is never collected into the artifact. The job failing
      for the right reason while reporting the wrong one is how a finding gets
      triaged as a flake.

.OUTPUTS
  A single JSON object on stdout. Exit code 0 if verdict is PASS, 1 if FAIL,
  so the workflow can use either.
#>
[CmdletBinding()]
param(
    # Merged stdout/stderr capture of the test process.
    [string]$StdoutLog = "",

    # Every directory in which an ASan log_path file may have landed. Order is
    # irrelevant; each is scanned and de-duplicated by full path.
    [string[]]$LogDir = @(),

    # Base name passed to ASAN_OPTIONS log_path (files are "<base>.<pid>").
    [string]$LogBase = "asan_ci_finding",

    # JUnit XML the test process was asked to write.
    [string]$XmlPath = "",

    # Exit code of the test process.
    [int]$Rc = 0,

    # If set, collected ASan report files are moved here for artifact upload.
    [string]$CollectTo = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ASan prints one of these per finding, in both stderr and log_path files.
$findingPattern = '==\d+==ERROR:'

$diagnostics = New-Object System.Collections.Generic.List[string]

# --- Source 1: the merged stdout capture. -----------------------------------
# Kept because it is correct whenever log_path is NOT set (a local run, or a
# future change that drops log_path). It is expected to be silent under this
# lane's options; that expectation is asserted by the suite, not assumed here.
$findingsStdout = 0
if ($StdoutLog -and (Test-Path -LiteralPath $StdoutLog)) {
    $findingsStdout = @(Select-String -LiteralPath $StdoutLog -Pattern $findingPattern).Count
} elseif ($StdoutLog) {
    $diagnostics.Add("stdout log not found at '$StdoutLog'")
}

# --- Source 2: log_path files, across every candidate directory. ------------
$seen = @{}
$logFiles = New-Object System.Collections.Generic.List[string]
$findingsLogFiles = 0
foreach ($dir in $LogDir) {
    if (-not $dir) { continue }
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    $full = (Resolve-Path -LiteralPath $dir).Path
    if ($seen.ContainsKey($full)) { continue }
    $seen[$full] = $true
    $found = @(Get-ChildItem -LiteralPath $full -Filter "$LogBase.*" -File -ErrorAction SilentlyContinue)
    foreach ($f in $found) {
        $logFiles.Add($f.FullName)
        $findingsLogFiles += @(Select-String -LiteralPath $f.FullName -Pattern $findingPattern).Count
    }
}

$findings = $findingsStdout + $findingsLogFiles

# --- ASAN_OPTIONS parse failure means the tests never ran at all. -----------
if ($StdoutLog -and (Test-Path -LiteralPath $StdoutLog)) {
    $optErr = @(Select-String -LiteralPath $StdoutLog -Pattern 'in ASAN_OPTIONS')
    if ($optErr.Count -gt 0) {
        $diagnostics.Add("ASAN_OPTIONS parse error - tests did NOT run: $($optErr[0].Line)")
        $Rc = 1
    }
}

# --- GAP-A15 mirror: a green exit code over a red XML. ----------------------
$gtestFailures = 0
if ($XmlPath) {
    if (Test-Path -LiteralPath $XmlPath) {
        try {
            [xml]$gx = Get-Content -LiteralPath $XmlPath
            $gtestFailures = [int]$gx.testsuites.failures + [int]$gx.testsuites.errors
        } catch {
            $diagnostics.Add("results XML at '$XmlPath' could not be parsed: $($_.Exception.Message)")
            $Rc = 1
        }
        if ($gtestFailures -gt 0) {
            $diagnostics.Add("gtest reported $gtestFailures failure(s)/error(s) in $XmlPath")
            $Rc = 1
        }
    } else {
        $diagnostics.Add("results XML not produced at '$XmlPath' - cannot verify subset results")
        $Rc = 1
    }
}

$verdict = if ($Rc -eq 0 -and $findings -eq 0) { "PASS" } else { "FAIL" }

# --- The evidence cross-check that the inline version did not have. ---------
# A non-zero exit with nothing collected is the signature of a finding whose
# report landed in a directory nobody scanned.
$evidenceCollected = $true
if ($Rc -ne 0 -and $findings -eq 0) {
    $evidenceCollected = $false
    $diagnostics.Add(
        "EVIDENCE GAP: process exited $Rc but zero ASan findings were collected. " +
        "ASan resolves a relative log_path against the PROCESS CWD; if the run's " +
        "CWD was not among the scanned directories (" +
        (($LogDir | Where-Object { $_ }) -join '; ') +
        ") the report exists but was never read. Do not triage this as a flake.")
}

if ($CollectTo -and $logFiles.Count -gt 0) {
    if (-not (Test-Path -LiteralPath $CollectTo)) {
        New-Item -ItemType Directory -Path $CollectTo -Force | Out-Null
    }
    foreach ($lf in $logFiles) {
        $target = Join-Path $CollectTo (Split-Path -Leaf $lf)
        if ((Resolve-Path -LiteralPath $lf).Path -ne $target) {
            Move-Item -LiteralPath $lf -Destination $target -Force -ErrorAction SilentlyContinue
        }
    }
}

[pscustomobject]@{
    verdict           = $verdict
    rc                = $Rc
    findings          = $findings
    findingsStdout    = $findingsStdout
    findingsLogFiles  = $findingsLogFiles
    logFiles          = @($logFiles)
    gtestFailures     = $gtestFailures
    evidenceCollected = $evidenceCollected
    diagnostics       = @($diagnostics)
} | ConvertTo-Json -Depth 4 -Compress

if ($verdict -eq "PASS") { exit 0 } else { exit 1 }
