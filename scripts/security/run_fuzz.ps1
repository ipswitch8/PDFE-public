#requires -Version 5.1
<#
.SYNOPSIS
  Drive the phase-4 PDFE fuzz harnesses for a configurable wall-clock
  budget per harness, capture crashes, emit a JSON summary.

.DESCRIPTION
  Runs each of the 17 fuzz harness binaries built under
  build/fuzz/ for a budget in seconds. By default smoke-runs every
  harness for 5 minutes (-Smoke); -Nightly is 60 minutes per harness.

  HARNESS-COUNT: 17   <- machine-checked against $AllHarnesses below.
  The prose above said "6" for months while the array held 11 and CI
  built 12 (meta-audit gap SEC-13). It cannot drift again: the test
  FuzzHarnessCountDocs in tests/test_doc_drift_guards.cpp parses this
  HARNESS-COUNT marker, the prose sentence, the $AllHarnesses array,
  tests/security/fuzz/CMakeLists.txt and .github/workflows/windows-fuzz.yml
  and fails unless all five agree.
  Crash inputs are minimized via libFuzzer -minimize_crash=1 before
  being saved to tests/security/reports/fuzz/<harness>/crashes/.

  The output JSON record matches the schema referenced by phase-4
  AC8 and AC9 so phase 9's regression check can compare future runs
  to a saved baseline.

.PARAMETER Smoke
  Use 5-minute-per-harness budget (default).

.PARAMETER Nightly
  Use 60-minute-per-harness budget.

.PARAMETER PerHarnessSeconds
  Override the budget. Mutually exclusive with -Smoke / -Nightly.

.PARAMETER Harness
  Run only the named harness (one of fuzz_document_open,
  fuzz_page_render, fuzz_text_extract, fuzz_annotation_walk,
  fuzz_form_parse, fuzz_embedded_files, fuzz_image_decode,
  fuzz_encryption_dict, fuzz_content_stream_edit, fuzz_save_roundtrip,
  fuzz_mujs_exec, fuzz_redaction_apply, fuzz_font_program,
  fuzz_ocsp_response, fuzz_crl_parse, fuzz_pkcs7_verify,
  fuzz_tsa_response). Default: all 17.

.PARAMETER BuildDir
  Path containing the fuzz/ subdir with built binaries.
  Default: build/vs2022 (Windows VS preset).

.PARAMETER ReportDir
  Path under tests/security/reports/fuzz/ where this run writes
  per-harness output and the rollup JSON.

.EXAMPLE
  pwsh scripts/security/run_fuzz.ps1 -Smoke
  pwsh scripts/security/run_fuzz.ps1 -Nightly -Harness fuzz_form_parse

.NOTES
  Requires PDFE_ENABLE_FUZZING=ON at CMake configure time and
  clang-cl as the C++ compiler. Without that, the binaries don't
  exist and this script bails with a clear error.
#>
[CmdletBinding(DefaultParameterSetName='Smoke')]
param(
    [Parameter(ParameterSetName='Smoke')]
    [switch]$Smoke,
    [Parameter(ParameterSetName='Nightly')]
    [switch]$Nightly,
    [Parameter(ParameterSetName='Custom', Mandatory=$true)]
    [int]$PerHarnessSeconds,
    [string]$Harness = 'all',
    [string]$BuildDir,
    [string]$ReportDir
)

$ErrorActionPreference = 'Stop'

# Resolve repo root + defaults relative to this script's location
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path

if (-not $BuildDir) {
    # Default to the asan build dir, which is the shared clang-cl
    # tree (phase 4 fuzz and phase 7 ASan both build there). The
    # regular build/vs2022 uses MSVC and cannot host fuzz binaries
    # since cl.exe lacks -fsanitize=fuzzer.
    $BuildDir = Join-Path $RepoRoot 'build\vs2022-asan'
}
if (-not $ReportDir) {
    # `-AsUTC` is PS7-only; [DateTime]::UtcNow works on PS 5.1 too.
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    $ReportDir = Join-Path $RepoRoot ("tests\security\reports\fuzz\run_" + $stamp)
}

# Decide budget
if ($PSCmdlet.ParameterSetName -eq 'Custom') {
    $budget = $PerHarnessSeconds
} elseif ($Nightly) {
    $budget = 60 * 60
} else {
    $budget = 5 * 60
}

$AllHarnesses = @(
    'fuzz_document_open',
    'fuzz_page_render',
    'fuzz_text_extract',
    'fuzz_annotation_walk',
    'fuzz_form_parse',
    'fuzz_embedded_files',
    # Meta-audit F4 / SEC-06: image-codec decode surface (JPX + DCT + Flate
    # image XObjects). CI (windows-fuzz.yml) and tests/security/fuzz/
    # CMakeLists.txt have built this since F4; this script did not run it
    # until F9 (SEC-13) -- the same drift the harness-count check now guards.
    'fuzz_image_decode',
    'fuzz_encryption_dict',
    # R2-15/16 write/mutation-surface harnesses (real historical-bug areas).
    'fuzz_content_stream_edit',
    'fuzz_save_roundtrip',
    'fuzz_mujs_exec',
    'fuzz_redaction_apply',
    # Meta-audit F13 / SEC-09 + SEC-54 + SEC-58: font-PROGRAM parse surface.
    # Attacker-controlled sfnt/CFF bytes reaching FreeType through
    # EmbeddedFontAnalyzer (FT_New_Face / FT_New_Memory_Face). Distinct from
    # tests/test_font_fuzz.cpp, which fuzzes font DICTIONARIES only.
    'fuzz_font_program',
    # Phase-73: the SIGNATURE-VALIDATION input class. Every harness above takes
    # PDF bytes; these take DER -- from an OCSP responder, a CRL distribution
    # point, a /DSS stream, or a TSA. They need -dict=asn1.dict, NOT pdf.dict:
    # PDF tokens cannot occur in a valid DER encoding, so splicing them spends
    # the mutator's budget on inputs d2i_* rejects at the first tag byte.
    'fuzz_ocsp_response',
    'fuzz_crl_parse',
    # A PROMOTION: the LLVMFuzzerTestOneInput in tests/test_pkcs7_fuzz.cpp was
    # compiled into the gtest binary and linked as a fuzzer by nothing for
    # fourteen phases (DCA 0037).
    'fuzz_pkcs7_verify',
    'fuzz_tsa_response'
)

if ($Harness -eq 'all') {
    $Harnesses = $AllHarnesses
} else {
    if (-not ($AllHarnesses -contains $Harness)) {
        Write-Error "Unknown -Harness '$Harness'. Pick from: $($AllHarnesses -join ', ')"
        exit 2
    }
    $Harnesses = @($Harness)
}

# Confirm binaries exist
$FuzzBinDir = Join-Path $BuildDir 'fuzz'
foreach ($h in $Harnesses) {
    $exe = Join-Path $FuzzBinDir ($h + '.exe')
    if (-not (Test-Path $exe)) {
        Write-Error @"
[run_fuzz] $exe not found.
Reconfigure CMake with -DPDFE_ENABLE_FUZZING=ON and a clang-cl
compiler (-DCMAKE_CXX_COMPILER=clang-cl), then rebuild. The
opt-in posture means these binaries are NOT built by default.
"@
        exit 3
    }
}

New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null
# Dictionaries, chosen PER HARNESS. pdf.dict is right for the thirteen
# PDF-bytes harnesses and actively wrong for the four DER ones: "endobj" and
# "%%EOF" spliced into a length-prefixed binary encoding produce inputs d2i_*
# rejects at the first tag byte, so a dictionary aimed at the wrong format is
# not neutral -- it spends the mutator's budget on impossible tokens.
$PdfDictPath  = Join-Path $FuzzBinDir 'pdf.dict'
$Asn1DictPath = Join-Path $FuzzBinDir 'asn1.dict'
$Asn1Harnesses = @('fuzz_ocsp_response', 'fuzz_crl_parse',
                   'fuzz_pkcs7_verify', 'fuzz_tsa_response')
$Records = @()

foreach ($h in $Harnesses) {
    $exe = Join-Path $FuzzBinDir ($h + '.exe')
    # CMake stages every harness's seeds here, including
    # fuzz_pkcs7_verify's, which are COPIED from the pre-existing
    # tests/corpus/pkcs7/ rather than duplicated into seeds/.
    $seedDir = Join-Path $FuzzBinDir ("seeds\$h")
    $hReportDir = Join-Path $ReportDir $h
    $crashDir = Join-Path $hReportDir 'crashes'
    $artifactPrefix = (Join-Path $crashDir 'crash-')
    New-Item -ItemType Directory -Path $crashDir -Force | Out-Null
    $logPath = Join-Path $hReportDir 'fuzz.log'

    Write-Host "[run_fuzz] $h budget=${budget}s seeds=$seedDir"

    $libFuzzerArgs = @(
        $seedDir,
        ("-max_total_time=" + $budget),
        ("-max_len=" + (4 * 1024 * 1024)),
        ("-rss_limit_mb=" + 2048),
        ("-timeout=" + 10),
        ("-print_final_stats=1"),
        ("-artifact_prefix=" + $artifactPrefix)
    )
    $DictPath = if ($Asn1Harnesses -contains $h) { $Asn1DictPath } else { $PdfDictPath }
    if (Test-Path $DictPath) {
        $libFuzzerArgs += ("-dict=" + $DictPath)
    }

    $start = Get-Date
    $proc = Start-Process -FilePath $exe -ArgumentList $libFuzzerArgs `
        -RedirectStandardOutput $logPath -RedirectStandardError "${logPath}.err" `
        -PassThru -NoNewWindow -Wait
    $elapsed = ((Get-Date) - $start).TotalSeconds

    # Crash post-processing: minimize each crash artifact
    $crashes = @()
    if (Test-Path $crashDir) {
        $crashFiles = Get-ChildItem -Path $crashDir -Filter 'crash-*' -File
        foreach ($cf in $crashFiles) {
            $minPath = Join-Path $hReportDir ('min_' + $cf.Name)
            $minArgs = @($cf.FullName, "-minimize_crash=1",
                         ("-runs=10000"),
                         ("-exact_artifact_path=" + $minPath))
            try {
                Start-Process -FilePath $exe -ArgumentList $minArgs `
                    -PassThru -NoNewWindow -Wait `
                    -RedirectStandardOutput ($logPath + '.min') | Out-Null
            } catch {
                # Minimization is best-effort; original crash is
                # already saved.
            }
            $crashes += @{
                file = $cf.Name
                size_bytes = $cf.Length
                minimized = (Test-Path $minPath)
            }
        }
    }

    # Pull execs/sec, coverage edges from the log
    $execsPerSec = 0
    $cov = 0
    $oom = 0
    $timeouts = 0
    if (Test-Path $logPath) {
        $logContent = Get-Content $logPath -Raw -ErrorAction SilentlyContinue
        if ($logContent -match 'stat::number_of_executed_units:\s+(\d+)') {
            $units = [int]$Matches[1]
            if ($elapsed -gt 0) {
                $execsPerSec = [math]::Round($units / $elapsed, 0)
            }
        }
        if ($logContent -match 'cov:\s+(\d+)') { $cov = [int]$Matches[1] }
        $oom = ([regex]::Matches($logContent, 'libFuzzer: out-of-memory')).Count
        $timeouts = ([regex]::Matches($logContent, 'libFuzzer: timeout')).Count
    }

    $Records += @{
        harness         = $h
        budget_seconds  = $budget
        elapsed_seconds = [math]::Round($elapsed, 1)
        exit_code       = $proc.ExitCode
        execs_per_sec   = $execsPerSec
        coverage_edges  = $cov
        unique_crashes  = $crashes.Count
        oom_count       = $oom
        timeout_count   = $timeouts
        crashes         = $crashes
    }
}

$summary = @{
    schema_version  = 1
    # `-AsUTC` is PS7-only; [DateTime]::UtcNow works on PS 5.1 too.
    stamp           = ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
    budget          = $budget
    harnesses       = $Records
    overall_verdict = ($Records.unique_crashes | Measure-Object -Sum).Sum -eq 0 `
        ? 'pass' : 'fail'
}

$jsonPath = Join-Path $ReportDir 'summary.json'
$summary | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding UTF8
Write-Host "[run_fuzz] verdict=$($summary.overall_verdict)  -> $jsonPath"

if ($summary.overall_verdict -eq 'fail') { exit 1 } else { exit 0 }
