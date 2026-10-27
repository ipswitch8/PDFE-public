# Phase 2: malicious-pdf corpus generation.
#
# Clones jonaslejon/malicious-pdf at a pinned SHA, runs the generator
# with a fixed loopback callback host (127.0.0.1) and fixed inputs so
# repeated runs produce the same files, moves results into
# tests/security/corpus/malicious_pdf/, and writes provenance metadata.
#
# Idempotent: safe to re-run. Existing payloads under malicious_pdf/
# are deleted before regeneration so a partial previous run can't
# leave stale files mixed with fresh ones.
#
# Usage:
#   .\scripts\security\gen_malicious_corpus.ps1
#     [-PinnedSha c33050c637c75dfd9bea8afcabad0f41a928c070]
#     [-CallbackHost 127.0.0.1]
#     [-CallbackPort 9999]

[CmdletBinding()]
param(
    [string]$PinnedSha    = 'c33050c637c75dfd9bea8afcabad0f41a928c070',
    [string]$CallbackHost = '127.0.0.1',
    [int]$CallbackPort    = 9999
)

$ErrorActionPreference = 'Stop'
$thisFile  = $MyInvocation.MyCommand.Path
$thisDir   = Split-Path -Parent $thisFile
$repoDir   = Split-Path -Parent (Split-Path -Parent $thisDir)
$corpusDir = Join-Path $repoDir 'tests\security\corpus\malicious_pdf'
$workDir   = Join-Path $env:TEMP 'pdfe_malicious_pdf_clone'

Write-Host "[gen-corpus] repoDir:    $repoDir"
Write-Host "[gen-corpus] corpusDir:  $corpusDir"
Write-Host "[gen-corpus] pinnedSha:  $PinnedSha"
Write-Host "[gen-corpus] callback:   ${CallbackHost}:${CallbackPort}"

# --- Clone (or refresh) the upstream repo at the pinned SHA ---------
# Check for `.git` inside $workDir, not just $workDir itself: a prior
# failed run can leave the directory created but empty (also: when
# AutoElevate spawns the elevated child, $env:TEMP may resolve to a
# different per-session subdir than the unelevated parent's, so a
# previously-cached clone in the parent's TEMP won't be reused
# anyway -- but the elevated child can still find a stale empty
# dir created earlier under its own TEMP).
$dotGit = Join-Path $workDir '.git'
if ((Test-Path $workDir) -and -not (Test-Path $dotGit)) {
    Write-Host "[gen-corpus] $workDir exists but has no .git; removing for clean re-clone"
    Remove-Item -Recurse -Force $workDir
}
if (-not (Test-Path $dotGit)) {
    & git clone --quiet https://github.com/jonaslejon/malicious-pdf $workDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed (rc=$LASTEXITCODE)" }
}
Push-Location $workDir
try {
    & git fetch --quiet origin
    & git checkout --quiet $PinnedSha
    if ($LASTEXITCODE -ne 0) {
        throw "git checkout $PinnedSha failed (rc=$LASTEXITCODE)"
    }
    & pip install --quiet --disable-pip-version-check -r requirements.txt
}
finally {
    Pop-Location
}

# --- Reset target dir + generate ------------------------------------
if (Test-Path $corpusDir) {
    # Preserve the .gitkeep / SOURCE.txt / manifest.json / README we
    # commit; remove only the generated payloads.
    Get-ChildItem $corpusDir -File -Recurse |
        Where-Object { $_.Extension -in '.pdf', '.svg', '.xml' } |
        Remove-Item -Force
} else {
    New-Item -ItemType Directory -Path $corpusDir -Force | Out-Null
}

Push-Location $workDir
try {
    & python 'malicious-pdf.py' --output-dir $corpusDir $CallbackHost
    if ($LASTEXITCODE -ne 0) { throw "generator failed (rc=$LASTEXITCODE)" }
}
finally {
    Pop-Location
}

# --- Provenance metadata --------------------------------------------
$sourceFile = Join-Path $corpusDir 'SOURCE.txt'
$genTime    = Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'
@"
PDFE security harness -- malicious-pdf corpus provenance

Upstream:    https://github.com/jonaslejon/malicious-pdf
Pinned SHA:  $PinnedSha
License:     MIT (upstream)
Callback:    ${CallbackHost}:${CallbackPort}
Generated:   $genTime
Generator:   scripts/security/gen_malicious_corpus.ps1

The PDFs in this directory are SYNTHETIC TEST PAYLOADS, not actual
malware. They contain phone-home patterns (URI/Launch/SubmitForm/JS)
designed to make a vulnerable PDF reader connect to the callback
host above. PDFE's correct behavior for every PDF here is: open
without callback, refuse to follow Launch without prompting, and
honor the SubmitForm host allowlist.

DO NOT open these files with a non-sandboxed PDF reader (Acrobat,
Edge, Chrome). Use only the phase-3 sandboxed runner.

The corpus itself is gitignored; only this SOURCE.txt, manifest.json,
README, and the generator script are checked in.
"@ | Set-Content -Encoding UTF8 $sourceFile
Write-Host "[gen-corpus] Wrote $sourceFile" -ForegroundColor Green

# --- Build manifest -------------------------------------------------
$manifestScript = Join-Path $thisDir 'build_manifest.py'
if (Test-Path $manifestScript) {
    Write-Host "[gen-corpus] Building manifest..." -ForegroundColor Cyan
    & python $manifestScript --corpus $corpusDir --out (Join-Path (Split-Path -Parent $corpusDir) 'manifest.json')
    if ($LASTEXITCODE -ne 0) { throw "manifest build failed (rc=$LASTEXITCODE)" }
} else {
    Write-Host "[gen-corpus] WARN: build_manifest.py missing -- manifest will be stale" -ForegroundColor Yellow
}

# --- Summary --------------------------------------------------------
$count = (Get-ChildItem $corpusDir -File -Recurse |
          Where-Object { $_.Extension -in '.pdf', '.svg', '.xml' }).Count
Write-Host "[gen-corpus] Generated $count payload file(s) in $corpusDir" -ForegroundColor Green
Write-Host "[gen-corpus] Done."
