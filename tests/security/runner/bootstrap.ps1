# PDFE security test harness bootstrap.
# Idempotent: safe to run multiple times.
# Creates tests/security/.venv with pinned deps, populates
# tool_inventory.json with which external tools are present.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$securityDir = Split-Path -Parent $here
$venvDir = Join-Path $securityDir '.venv'
$reqFile = Join-Path $securityDir 'requirements.txt'
$toolFile = Join-Path $securityDir 'tool_inventory.json'

Write-Host "[bootstrap] PDFE security harness setup" -ForegroundColor Cyan
Write-Host "[bootstrap] securityDir: $securityDir"
Write-Host "[bootstrap] venvDir:     $venvDir"

# --- Python venv ------------------------------------------------------
if (-not (Test-Path $venvDir)) {
    Write-Host "[bootstrap] Creating venv..." -ForegroundColor Yellow
    & python -m venv $venvDir
    if ($LASTEXITCODE -ne 0) {
        throw "venv creation failed (rc=$LASTEXITCODE)"
    }
} else {
    Write-Host "[bootstrap] venv already exists, skipping creation"
}

$pip = Join-Path $venvDir 'Scripts\pip.exe'
if (-not (Test-Path $pip)) {
    throw "pip not found at $pip after venv creation"
}

Write-Host "[bootstrap] Installing requirements..." -ForegroundColor Yellow
& $pip install --quiet --disable-pip-version-check -r $reqFile
if ($LASTEXITCODE -ne 0) {
    throw "pip install failed (rc=$LASTEXITCODE)"
}

# --- Tool inventory probe ---------------------------------------------
# Probes each external tool by running its --version (or equivalent).
# Records availability + observed version into tool_inventory.json so
# later phases can skip steps with clear messages instead of failing.

$tools = @(
    @{ name = 'malicious-pdf';
       install_cmd = 'git clone https://github.com/jonaslejon/malicious-pdf';
       entry_point = 'python malicious_pdf.py';
       expected_version = 'pinned-by-sha';
       probe_cmd = $null; # repo presence checked separately in phase 2
    },
    @{ name = 'cppcheck';
       install_cmd = 'choco install cppcheck';
       entry_point = 'cppcheck';
       expected_version = '>=2.13';
       probe_cmd = @('cppcheck', '--version'); },
    @{ name = 'clang-tidy';
       install_cmd = 'comes with LLVM (clang-cl)';
       entry_point = 'clang-tidy';
       expected_version = '>=18';
       probe_cmd = @('clang-tidy', '--version'); },
    @{ name = 'clang-cl';
       install_cmd = 'install LLVM Windows';
       entry_point = 'clang-cl';
       expected_version = '>=18';
       probe_cmd = @('clang-cl', '--version'); },
    @{ name = 'cargo-audit';
       install_cmd = 'cargo install cargo-audit';
       entry_point = 'cargo audit';
       expected_version = '>=0.21';
       probe_cmd = @('cargo', 'audit', '--version'); },
    @{ name = 'osv-scanner';
       install_cmd = 'go install github.com/google/osv-scanner/cmd/osv-scanner@latest';
       entry_point = 'osv-scanner';
       expected_version = '>=1.9';
       probe_cmd = @('osv-scanner', '--version'); },
    @{ name = 'gitleaks';
       install_cmd = 'choco install gitleaks';
       entry_point = 'gitleaks';
       expected_version = '>=8.18';
       probe_cmd = @('gitleaks', 'version'); },
    @{ name = 'trufflehog';
       install_cmd = 'go install github.com/trufflesecurity/trufflehog/v3@latest';
       entry_point = 'trufflehog';
       expected_version = '>=3.80';
       probe_cmd = @('trufflehog', '--version'); },
    @{ name = 'AFL++';
       install_cmd = 'WSL build (Windows port limited)';
       entry_point = 'afl-fuzz';
       expected_version = '>=4.20';
       probe_cmd = @('afl-fuzz', '--version'); }
)

$inventory = @()
foreach ($t in $tools) {
    $entry = [ordered]@{
        name             = $t.name
        install_cmd      = $t.install_cmd
        entry_point      = $t.entry_point
        expected_version = $t.expected_version
        available        = $false
        observed_version = $null
        notes            = $null
    }
    if ($t.probe_cmd) {
        try {
            $exe = $t.probe_cmd[0]
            $args = if ($t.probe_cmd.Length -gt 1) { $t.probe_cmd[1..($t.probe_cmd.Length - 1)] } else { @() }
            $out = & $exe @args 2>&1 | Out-String
            if ($LASTEXITCODE -eq 0) {
                $entry.available = $true
                $entry.observed_version = ($out -split "`r?`n")[0].Trim()
            } else {
                $entry.notes = "probe rc=$LASTEXITCODE"
            }
        } catch {
            $entry.notes = "probe exception: $($_.Exception.Message)"
        }
    } else {
        $entry.notes = 'probed separately in its phase'
    }
    $inventory += [pscustomobject]$entry
}

# Coverity is intentionally NOT included in this inventory. Phase 5
# of the security pipeline forbids any Coverity invocation; a future
# contributor adding it back must first reinstate phase 5's
# Coverity-conditional logic and obtain a license. Do not add a
# Coverity entry above.

$inventory | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 $toolFile
Write-Host "[bootstrap] Wrote $toolFile" -ForegroundColor Green
Write-Host "[bootstrap] Tool availability summary:" -ForegroundColor Cyan
$inventory | Format-Table -Property name, available, observed_version, notes -AutoSize

Write-Host "[bootstrap] Done." -ForegroundColor Green
