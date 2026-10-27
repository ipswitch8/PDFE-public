# PDFE security harness — single entry-point dispatcher.
# Phase 1 stub: each per-phase script is a no-op until its phase
# lands. Run `.\run_all.ps1 -Help` for usage.

[CmdletBinding()]
param(
    [switch]$Smoke,
    [switch]$Nightly,
    [switch]$SkipAvCheck,
    [switch]$SkipCorpus,
    [switch]$SkipRunner,        # skip phase 3 (the 36-min corpus runner)
    [switch]$SkipFuzz,
    [switch]$SkipSast,
    [switch]$SkipDeps,
    [switch]$SkipSanitizers,
    [switch]$SkipSecrets,
    [switch]$AutoElevate,
    [switch]$IgnorePhaseFailures,   # continue past a failing phase
    [switch]$_Elevated,         # internal: set when self-elevation re-enters
    [string]$TranscriptPath,    # internal: where to write transcript
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)

# ============================================================
# Elevation handling
# ============================================================
function _is_elevated {
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return ([Security.Principal.WindowsPrincipal]$id).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { return $false }
}

# Phase 3 needs admin (netsh firewall + pfirewall.log). Phases 0/2/4-8
# do not. If the user wants the full sequence to run unattended from
# an unelevated session, -AutoElevate self-elevates ONCE up front so
# everything runs in a single elevated child with one UAC prompt and
# everything captured to a transcript file the parent can read.
if ($AutoElevate -and -not $_Elevated -and -not (_is_elevated)) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $transcriptDir = Join-Path $repo 'tests\security\reports\run_all'
    New-Item -ItemType Directory -Path $transcriptDir -Force | Out-Null
    $transcript = Join-Path $transcriptDir "run_all_$stamp.log"
    Write-Host "[run_all] -AutoElevate: re-launching elevated; transcript -> $transcript"
    Write-Host "[run_all] Accept the UAC prompt; the elevated child will run unattended."

    # Forward all the original switches to the elevated child, plus
    # the internal markers so it doesn't re-elevate forever and so
    # it knows where to write the transcript.
    $forwarded = @()
    if ($Smoke)          { $forwarded += '-Smoke' }
    if ($Nightly)        { $forwarded += '-Nightly' }
    if ($SkipAvCheck)    { $forwarded += '-SkipAvCheck' }
    if ($SkipCorpus)     { $forwarded += '-SkipCorpus' }
    if ($SkipRunner)     { $forwarded += '-SkipRunner' }
    if ($SkipFuzz)       { $forwarded += '-SkipFuzz' }
    if ($SkipSast)       { $forwarded += '-SkipSast' }
    if ($SkipDeps)       { $forwarded += '-SkipDeps' }
    if ($SkipSanitizers) { $forwarded += '-SkipSanitizers' }
    if ($SkipSecrets)    { $forwarded += '-SkipSecrets' }
    if ($IgnorePhaseFailures) { $forwarded += '-IgnorePhaseFailures' }
    $forwarded += @('-_Elevated', '-TranscriptPath', $transcript)

    $myPath = $MyInvocation.MyCommand.Path
    # Build the inner pwsh command line. -NoProfile keeps it
    # deterministic; -ExecutionPolicy Bypass lets unsigned scripts
    # run; -File invokes ourselves with the forwarded args.
    $innerArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$myPath) + $forwarded
    $proc = Start-Process -FilePath pwsh -ArgumentList $innerArgs `
        -Verb RunAs -Wait -PassThru
    Write-Host ""
    Write-Host "[run_all] elevated child finished rc=$($proc.ExitCode)"
    if (Test-Path $transcript) {
        Write-Host "[run_all] transcript: $transcript"
        Write-Host "[run_all] tail of transcript:"
        Get-Content $transcript -Tail 20 | ForEach-Object { Write-Host "  $_" }
    }
    exit $proc.ExitCode
}

# If we are the elevated child, start a transcript so the parent
# (or a future analyst) can read what happened.
if ($_Elevated -and $TranscriptPath) {
    try {
        Start-Transcript -Path $TranscriptPath -IncludeInvocationHeader | Out-Null
        $_haveTranscript = $true
    } catch { $_haveTranscript = $false }
}

if ($Help) {
    @"
PDFE security harness dispatcher

Usage:
  .\run_all.ps1 -Smoke         # default: 5-min budget per phase
  .\run_all.ps1 -Nightly       # 60-min budget, full corpus
  .\run_all.ps1 -SkipFuzz      # skip a specific phase
  .\run_all.ps1 -SkipAvCheck   # skip the A/V-exclusion precheck
                                  (only if you've verified manually)
  .\run_all.ps1 -AutoElevate   # one UAC prompt up front, runs full
                                  sequence in elevated child with
                                  transcript captured to
                                  tests/security/reports/run_all/run_all_<stamp>.log
  .\run_all.ps1 -IgnorePhaseFailures
                               # continue past failing phases instead
                                  of stopping at the first non-zero
                                  phase exit. Useful for exercising
                                  phases 4-8 when phase 3 has known
                                  findings under triage. Final exit
                                  code reflects the worst phase.

Step 0 (precheck): verify_av_exclusions.ps1
  Drops EICAR test files into every folder phases 2-8 use, waits
  5s, checks survival. Aborts the whole run if any folder is not
  excluded -- the rest of the sequence would be unreliable
  because A/V would silently delete malicious-PDF inputs and
  fuzz crash artifacts.

Phases (in order, each runs only if its precondition tools exist):
  2 corpus      gen_malicious_corpus.ps1
  3 runner      run_corpus.py (the main test step)
  4 fuzz        run_fuzz.ps1
  5 sast        run_sast.ps1
  6 deps        run_deps.ps1
  7 sanitizers  run_sanitizers.ps1
  8 secrets     run_secrets.ps1

Outputs land under tests/security/reports/.

Note: this is the OPT-IN entry point. The standard ctest run does
not invoke any of this. See docs/security-testing.md.
"@
    exit 0
}

$mode = if ($Nightly) { 'nightly' } else { 'smoke' }
Write-Host "[run_all] mode=$mode" -ForegroundColor Cyan

# ============================================================
# Step 0 -- A/V exclusion precheck
# ============================================================
# Before any phase runs, verify that every folder which will
# receive malicious / fuzz-input PDFs is excluded from A/V.
# Without this, A/V can silently delete payloads mid-run and
# corpus_run / fuzz reports will look "clean" only because their
# inputs got quarantined.
if (-not $SkipAvCheck) {
    $avCheck = Join-Path $here 'verify_av_exclusions.ps1'
    if (-not (Test-Path $avCheck)) {
        Write-Host "[run_all] WARNING: $avCheck not found; skipping A/V precheck" -ForegroundColor Yellow
    } else {
        Write-Host "[run_all] step 0: A/V exclusion precheck ($avCheck)" -ForegroundColor Green
        & $avCheck
        if ($LASTEXITCODE -ne 0) {
            Write-Host ""
            Write-Host "[run_all] ABORT: A/V precheck failed (rc=$LASTEXITCODE)." -ForegroundColor Red
            Write-Host "[run_all] Some folder used by phases 2-8 is NOT excluded from A/V." -ForegroundColor Red
            Write-Host "[run_all] Rest of sequence aborted -- A/V would silently delete inputs." -ForegroundColor Red
            Write-Host "[run_all] Re-run with -SkipAvCheck to bypass after manual verification." -ForegroundColor Yellow
            exit $LASTEXITCODE
        }
        Write-Host "[run_all] step 0 OK -- all required folders excluded from A/V" -ForegroundColor Green
    }
} else {
    Write-Host "[run_all] step 0: A/V precheck skipped by -SkipAvCheck" -ForegroundColor Yellow
}

# Dispatch table. The `path` field is the absolute location of the
# script. Phases 2, 4-8 live next to this dispatcher under
# scripts/security/; phase 3's runner lives under
# tests/security/runner/ because it ships alongside its
# decision/listener helpers and pytest unit tests.
$dispatch = @(
    @{ id = 2; name = 'corpus';     skip = $SkipCorpus;     path = (Join-Path $here 'gen_malicious_corpus.ps1') },
    @{ id = 3; name = 'runner';     skip = $SkipRunner;     path = (Join-Path $repo 'tests\security\runner\run_corpus.py') },
    @{ id = 4; name = 'fuzz';       skip = $SkipFuzz;       path = (Join-Path $here 'run_fuzz.ps1')             },
    @{ id = 5; name = 'sast';       skip = $SkipSast;       path = (Join-Path $here 'run_sast.ps1')             },
    @{ id = 6; name = 'deps';       skip = $SkipDeps;       path = (Join-Path $here 'run_deps.ps1')             },
    @{ id = 7; name = 'sanitizers'; skip = $SkipSanitizers; path = (Join-Path $here 'run_sanitizers.ps1')       },
    @{ id = 8; name = 'secrets';    skip = $SkipSecrets;    path = (Join-Path $here 'run_secrets.ps1')          }
)

$phaseResults = @()
$worstExit = 0

foreach ($p in $dispatch) {
    if ($p.skip) {
        Write-Host "[run_all] phase $($p.id) $($p.name): skipped by flag" -ForegroundColor Yellow
        $phaseResults += @{ id = $p.id; name = $p.name; verdict = 'skipped'; rc = 0 }
        continue
    }
    $scriptPath = $p.path
    $scriptName = Split-Path -Leaf $scriptPath
    if (-not (Test-Path $scriptPath)) {
        # Hard fail rather than silently skip. A missing dispatcher
        # means the suite is broken, not "this phase is a future
        # stub" -- every phase 2-8 dispatcher exists in the
        # committed state. (Karen-found bug fix: prior version did
        # `Join-Path $here $p.script` for ALL phases, which caused
        # phase 3's run_corpus.py to silently miss because it lives
        # under tests/security/runner/, not next to this script.)
        Write-Host "[run_all] phase $($p.id) $($p.name): MISSING dispatcher at $scriptPath" -ForegroundColor Red
        Write-Host "[run_all] aborting -- a missing dispatcher means the suite is in an inconsistent state." -ForegroundColor Red
        if ($_haveTranscript) { try { Stop-Transcript | Out-Null } catch {} }
        exit 1
    }
    # Forward the mode switch where applicable. run_fuzz.ps1 takes
    # -Smoke / -Nightly explicitly. run_sast.ps1 / run_deps.ps1 /
    # run_secrets.ps1 / run_sanitizers.ps1 are mode-agnostic (no
    # switches, they always do the full thing).
    #
    # Direct switch invocation rather than splatting -- splat with an
    # array containing a leading-`-` token tripped PowerShell's
    # parameter binder ("A positional parameter cannot be found that
    # accepts argument '-'") on the run_fuzz.ps1 call when invoked
    # from inside the elevated transcript-wrapped pwsh -File path.
    $modeLabel = if ($Nightly) { 'Nightly' } else { 'Smoke' }
    Write-Host "[run_all] phase $($p.id) $($p.name): running $scriptName $modeLabel" -ForegroundColor Green
    if ($scriptName.EndsWith('.py')) {
        & python $scriptPath
    } elseif ($scriptName -eq 'run_fuzz.ps1') {
        if ($Nightly) { & $scriptPath -Nightly } else { & $scriptPath -Smoke }
    } else {
        & $scriptPath
    }
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        Write-Host "[run_all] phase $($p.id) FAILED rc=$rc" -ForegroundColor Red
        $phaseResults += @{ id = $p.id; name = $p.name; verdict = 'fail'; rc = $rc }
        if ($rc -gt $worstExit) { $worstExit = $rc }
        if (-not $IgnorePhaseFailures) {
            if ($_haveTranscript) { try { Stop-Transcript | Out-Null } catch {} }
            exit $rc
        }
        Write-Host "[run_all] -IgnorePhaseFailures set; continuing to next phase" -ForegroundColor Yellow
    } else {
        $phaseResults += @{ id = $p.id; name = $p.name; verdict = 'pass'; rc = 0 }
    }
}

# Per-phase summary so the transcript has a clear pass/fail map
# regardless of which phases ran or were skipped.
Write-Host ""
Write-Host "[run_all] === Phase summary ==="
foreach ($r in $phaseResults) {
    $color = switch ($r.verdict) {
        'pass'    { 'Green' }
        'skipped' { 'Yellow' }
        'fail'    { 'Red' }
        default   { 'White' }
    }
    $rcStr = if ($r.rc -ne 0) { " (rc=$($r.rc))" } else { '' }
    Write-Host (("  phase {0} {1,-12} {2}{3}" -f $r.id, $r.name, $r.verdict, $rcStr)) -ForegroundColor $color
}

if ($worstExit -ne 0 -and $IgnorePhaseFailures) {
    Write-Host "[run_all] One or more phases FAILED but -IgnorePhaseFailures was set; final rc=$worstExit" -ForegroundColor Red
} elseif ($worstExit -eq 0) {
    Write-Host "[run_all] done" -ForegroundColor Green
}
if ($_haveTranscript) {
    try { Stop-Transcript | Out-Null } catch {}
}
exit $worstExit
