#requires -Version 5.1
<#
.SYNOPSIS
  Verify that A/V exclusions cover every folder PDFE's security suite
  uses for malicious / fuzz-input PDFs. Drops an EICAR-bearing PDF
  into each target folder, waits, then checks whether the file
  survived. Reports PASS / FAIL per folder.

.DESCRIPTION
  EICAR is the EICAR Standard Anti-Virus Test File -- a 68-byte
  ASCII string published by EICAR.org for this exact purpose.
  Every commercial AV (including Microsoft Defender) is required
  to detect and quarantine it as if it were real malware, even
  though it is harmless text. If a file containing EICAR survives
  for several seconds in a folder, the AV is NOT scanning that
  folder -- meaning the exclusion is effective.

  If a file containing EICAR is gone after the wait, the AV
  deleted/quarantined it -- meaning the exclusion is NOT
  effective there.

  Folders tested (matches the canonical list in the security
  suite's documentation):

    1. <repo>/tests/security/corpus/malicious_pdf/
    2. <repo>/tests/security/reports/fuzz/
    3. <repo>/tests/security/fuzz/seeds/
    4. <repo>/build/vs2022-asan/fuzz/seeds/  (build mirror)
    5. %TEMP%/pdfe_malicious_pdf_clone/
    6. %TEMP%/pdfe_fuzz/

  Folders 5 and 6 are the most easily missed because they live
  outside the repo tree.

.PARAMETER WaitSeconds
  How long to wait after writing each test file before checking
  for survival. Default: 5 seconds. Increase if your AV scans
  asynchronously and is slow.

.PARAMETER NoCleanup
  Leave surviving test files in place (for manual inspection).
  Default: clean up everything at end.

.PARAMETER ExtraPath
  Additional folder(s) to test beyond the canonical six. Useful
  if you have a non-default vcpkg or build location.

.EXAMPLE
  pwsh scripts/security/verify_av_exclusions.ps1
  pwsh scripts/security/verify_av_exclusions.ps1 -WaitSeconds 10
  pwsh scripts/security/verify_av_exclusions.ps1 -ExtraPath D:\custom\build\fuzz

.NOTES
  Read-only with respect to A/V config -- this script does NOT
  modify Windows Defender / third-party AV settings. It only
  drops files and checks if they survive. To add the exclusions
  themselves, see the snippet in the suite README; that requires
  Administrator + Add-MpPreference (or equivalent for your AV).
#>
[CmdletBinding()]
param(
    [int]$WaitSeconds = 5,
    [switch]$NoCleanup,
    [string[]]$ExtraPath = @()
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path

# The canonical EICAR test string. DO NOT modify -- the bytes
# below are exactly what every AV scans for. If you change a
# single character, AV will not recognize it and the test
# becomes meaningless.
#
# Source: https://www.eicar.org/?page_id=3950
# Assembled in pieces below so this script file itself does not
# contain the exact contiguous EICAR string (which would cause
# AV to delete the script).
$eicarParts = @(
    'X5O!P%@AP[4\PZX54(P^)',
    '7CC)7}$EICAR-STAN',
    'DARD-ANTIVIRUS-TEST-FILE!$H+H*'
)
$eicar = -join $eicarParts

# Wrap in a minimal PDF so the file extension matches what
# our suite handles. The EICAR bytes still appear in the file
# stream so AV detects regardless of the wrapper.
function _eicar_pdf_bytes() {
    $body = "%PDF-1.4`n% AV-exclusion test sentinel - " + $eicar + "`n%%EOF`n"
    return [System.Text.Encoding]::ASCII.GetBytes($body)
}

# ============================================================
# Folder list
# ============================================================
$folders = @(
    @{
        name = 'malicious_pdf corpus'
        path = (Join-Path $RepoRoot 'tests\security\corpus\malicious_pdf')
        category = 'repo'
    },
    @{
        name = 'fuzz reports'
        path = (Join-Path $RepoRoot 'tests\security\reports\fuzz')
        category = 'repo'
    },
    @{
        name = 'fuzz seeds (source)'
        path = (Join-Path $RepoRoot 'tests\security\fuzz\seeds')
        category = 'repo'
    },
    @{
        name = 'fuzz seeds (build mirror)'
        path = (Join-Path $RepoRoot 'build\vs2022-asan\fuzz\seeds')
        category = 'repo'
    },
    @{
        name = 'malicious-pdf clone (system temp)'
        path = (Join-Path $env:TEMP 'pdfe_malicious_pdf_clone')
        category = 'temp'
    },
    @{
        name = 'pdfe_fuzz iteration temp (system temp)'
        path = (Join-Path $env:TEMP 'pdfe_fuzz')
        category = 'temp'
    }
)
foreach ($extra in $ExtraPath) {
    $folders += @{ name = "extra: $extra"; path = $extra; category = 'extra' }
}

# ============================================================
# Helpers
# ============================================================
function _try_write([string]$path) {
    # Create the directory if missing. Returns the path of the
    # written file, or $null on failure.
    try {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    } catch {
        return @{ ok = $false; reason = "could not create dir: $_" ; file = $null }
    }
    $file = Join-Path $path 'av_exclusion_test.pdf'
    try {
        $bytes = _eicar_pdf_bytes
        [System.IO.File]::WriteAllBytes($file, $bytes)
    } catch {
        # Some AVs block the write before the file is even on
        # disk; that itself is a "AV is scanning this folder"
        # signal.
        return @{ ok = $false; reason = "write blocked: $($_.Exception.Message)"; file = $file }
    }
    if (-not (Test-Path $file)) {
        return @{ ok = $false; reason = "write succeeded but file vanished immediately" ; file = $file }
    }
    return @{ ok = $true; reason = ''; file = $file }
}

function _check_survival([string]$file, [int]$waitSec) {
    Start-Sleep -Seconds $waitSec
    if (Test-Path $file) {
        try {
            $size = (Get-Item $file).Length
            return @{ survived = $true; size = $size }
        } catch {
            # Test-Path was true but Get-Item failed -- AV held the
            # file open mid-quarantine. Treat as not-survived.
            return @{ survived = $false; size = 0 }
        }
    }
    return @{ survived = $false; size = 0 }
}

# ============================================================
# Run
# ============================================================
$results = @()
$createdFiles = @()

Write-Host "[verify_av] Dropping EICAR-bearing test files (wait $WaitSeconds s per folder)..."
Write-Host "[verify_av] EICAR is the canonical AV test string; harmless but every AV detects it."
Write-Host ""

foreach ($f in $folders) {
    Write-Host ("[verify_av] {0,-50} -> " -f $f.name) -NoNewline
    $w = _try_write $f.path
    if (-not $w.ok) {
        Write-Host "WRITE_BLOCKED: $($w.reason)" -ForegroundColor Red
        $results += @{
            name      = $f.name
            path      = $f.path
            category  = $f.category
            verdict   = 'FAIL_WRITE'
            note      = $w.reason
        }
        continue
    }
    $createdFiles += $w.file
    $s = _check_survival $w.file $WaitSeconds
    if ($s.survived) {
        Write-Host ("PASS (file present, {0} bytes)" -f $s.size) -ForegroundColor Green
        $results += @{
            name      = $f.name
            path      = $f.path
            category  = $f.category
            verdict   = 'PASS'
            note      = "survived $WaitSeconds s, $($s.size) bytes"
        }
    } else {
        Write-Host "FAIL (file deleted by AV)" -ForegroundColor Red
        $results += @{
            name      = $f.name
            path      = $f.path
            category  = $f.category
            verdict   = 'FAIL_QUARANTINED'
            note      = "AV deleted file within $WaitSeconds s -- exclusion NOT effective"
        }
    }
}

# ============================================================
# Cleanup
# ============================================================
if (-not $NoCleanup) {
    Write-Host ""
    Write-Host "[verify_av] Cleaning up surviving test files..."
    foreach ($f in $createdFiles) {
        if (Test-Path $f) {
            try { Remove-Item -Force $f } catch {}
        }
    }
}

# ============================================================
# Summary
# ============================================================
Write-Host ""
Write-Host "=== Summary ==="
$pad = 52
$results | ForEach-Object {
    $color = switch ($_.verdict) {
        'PASS'              { 'Green' }
        'FAIL_WRITE'        { 'Yellow' }
        'FAIL_QUARANTINED'  { 'Red' }
        default             { 'White' }
    }
    Write-Host (("  {0,-$pad} {1}" -f $_.name, $_.verdict) + " -- $($_.note)") -ForegroundColor $color
}

$failed = ($results | Where-Object { $_.verdict -ne 'PASS' }).Count
$passed = ($results | Where-Object { $_.verdict -eq 'PASS' }).Count
Write-Host ""
Write-Host "[verify_av] $passed/$($results.Count) folders properly excluded from A/V."

if ($failed -gt 0) {
    Write-Host ""
    Write-Host "Folders where the exclusion is NOT effective:" -ForegroundColor Red
    foreach ($r in ($results | Where-Object { $_.verdict -ne 'PASS' })) {
        Write-Host "  $($r.path)"
    }
    Write-Host ""
    Write-Host "Add these paths to your A/V's exclusion list. For Microsoft Defender:" -ForegroundColor Yellow
    Write-Host "  # Run from elevated PowerShell:"
    Write-Host "  Add-MpPreference -ExclusionPath @("
    foreach ($r in ($results | Where-Object { $_.verdict -ne 'PASS' })) {
        Write-Host "    `"$($r.path)`","
    }
    Write-Host "  )"
    exit 1
}

exit 0
