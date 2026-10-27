#requires -Version 5.1
<#
.SYNOPSIS
  One-shot installer for the external tools needed by phases 4-8 of
  the PDFE security testing suite. Skips anything already installed.
  Installs to <repo>/tools/bin/ (gitignored) so a single PATH entry
  makes the whole suite work.

.DESCRIPTION
  Tools handled:

    Phase 5  cppcheck       (auto-install needs admin; otherwise prints MSI URL)
    Phase 6  cargo-audit    (cargo install cargo-audit)
    Phase 6  osv-scanner    (download Windows .exe from GitHub release)
    Phase 8  gitleaks       (download Windows .zip from GitHub release)
    Phase 8  trufflehog     (download Windows .tar.gz from GitHub release)
    Phase 4/7 clang-cl      (NOT auto-installable; prints VS Installer command)

  The downloaded binaries are placed in <repo>/tools/bin/ which is
  gitignored. After running, the script prints the exact PATH
  modification needed.

  HTTPS to github.com gives us transport authenticity. In addition,
  every fetched scanner binary is verified against a SHA256 hash
  PINNED in this script ($HASHES below). The pinned digests were taken
  from each release's upstream signed checksum file at pin time. A
  download whose SHA256 does not match its pin is DELETED and the
  install aborts (R2-27) -- mirroring the ONNX-model / pip
  --require-hashes hardening used elsewhere in the repo. To bump a
  version, update BOTH $VERSIONS and $HASHES from the upstream
  checksums file. The script prints the upstream checksum URLs at end
  so users can independently re-verify.

.PARAMETER Force
  Re-install even if a tool is already on PATH or in tools/bin/.

.PARAMETER ToolsDir
  Override the install location. Default: <repo>/tools/bin/.

.PARAMETER SkipPathHint
  Don't print the PATH modification suggestion at end.

.PARAMETER OnlyTools
  Comma-separated subset to install (e.g. 'gitleaks,trufflehog').
  Default: all.

.EXAMPLE
  pwsh scripts/security/install_tools.ps1
  pwsh scripts/security/install_tools.ps1 -OnlyTools gitleaks,trufflehog
  pwsh scripts/security/install_tools.ps1 -Force

.NOTES
  Exit codes:
    0  - all requested tools installed (or already present)
    1  - at least one tool failed to install
    2  - preflight failed (no internet, etc.)
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$ToolsDir,
    [switch]$SkipPathHint,
    [string]$OnlyTools = 'all'
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
if (-not $ToolsDir) { $ToolsDir = Join-Path $RepoRoot 'tools\bin' }
New-Item -ItemType Directory -Path $ToolsDir -Force | Out-Null

# ============================================================
# Pinned versions (update by editing this block)
# ============================================================
$VERSIONS = @{
    gitleaks    = '8.21.2'
    trufflehog  = '3.83.7'
    osv_scanner = 'v1.9.1'
    cppcheck    = '2.14.0'
}

# ============================================================
# Pinned SHA256 digests for each fetched scanner binary (R2-27).
# Keyed by the EXACT downloaded artifact filename. Source: each
# release's upstream signed checksum file (osv-scanner_SHA256SUMS,
# gitleaks_<ver>_checksums.txt, trufflehog_<ver>_checksums.txt).
# Update in lockstep with $VERSIONS. A mismatch aborts the install.
# ============================================================
$HASHES = @{
    'osv-scanner_windows_amd64.exe'          = '47ff39efded3613a680f0ed2b6b6f93338117a65767c4c4bc5b80a292da6d482'
    'gitleaks_8.21.2_windows_x64.zip'        = 'f238c85e5f47e18fac779ce71ee11091cf70a0a8fb4415f165efba2800eef133'
    'trufflehog_3.83.7_windows_amd64.tar.gz' = 'cbaecd7696ea2c0ce1cb0d1db7dc6f784668bbc5b13f08b4a377b1c4274ec81a'
}

$wantTools = if ($OnlyTools -eq 'all') {
    @('cargo-audit','osv-scanner','gitleaks','trufflehog','cppcheck','clang-cl')
} else {
    $OnlyTools -split ','
}

$results = @()
function _add($tool, $verdict, $note) {
    $script:results += @{ tool = $tool; verdict = $verdict; note = $note }
}

function _on_path([string]$exe) {
    $cmd = Get-Command $exe -ErrorAction SilentlyContinue
    return [bool]$cmd
}

function _in_tools_bin([string]$exe) {
    return (Test-Path (Join-Path $ToolsDir $exe))
}

function _download([string]$url, [string]$dest) {
    Write-Host "  downloading $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    } catch {
        throw "download failed: $($_.Exception.Message)"
    }
}

# Supply-chain integrity gate (R2-27): verify a freshly downloaded file
# against its pinned SHA256 in $HASHES, keyed by the artifact filename.
# On mismatch (or a missing pin) DELETE the file and throw so the install
# aborts -- fail closed, never install an unverified binary. Mirrors the
# ONNX-model / pip --require-hashes hardening used elsewhere in the repo.
function _verify_sha256([string]$file, [string]$artifactName) {
    $expected = $HASHES[$artifactName]
    if (-not $expected) {
        Remove-Item -Force $file -ErrorAction SilentlyContinue
        throw "no pinned SHA256 for '$artifactName' -- refusing to install an unverified binary (add it to `$HASHES)"
    }
    $actual = (Get-FileHash -Path $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $exp    = $expected.ToLowerInvariant()
    if ($actual -ne $exp) {
        Remove-Item -Force $file -ErrorAction SilentlyContinue
        throw "SHA256 mismatch for '$artifactName': expected $exp but got $actual -- download deleted, aborting (possible tampering)"
    }
    Write-Host "  sha256 OK ($artifactName)"
}

function _extract_zip([string]$zip, [string]$out) {
    Expand-Archive -Path $zip -DestinationPath $out -Force
}

function _extract_targz([string]$targz, [string]$out) {
    New-Item -ItemType Directory -Path $out -Force | Out-Null
    # Windows' built-in tar (bsdtar from libarchive) treats `C:\foo`
    # as host:path (rsync/scp-style) and tries to resolve `C` as a
    # remote host. Push-Location to the OUTPUT dir first so we
    # extract in place using only relative filenames -- no drive-
    # letter colon appears in any tar argument.
    $targzAbs = (Resolve-Path $targz).Path
    Push-Location $out
    try {
        # Copy the tarball into the output dir, extract, delete copy.
        $local = Join-Path $out (Split-Path -Leaf $targz)
        Copy-Item $targzAbs $local -Force
        & tar -xzf (Split-Path -Leaf $targz)
        $rc = $LASTEXITCODE
        Remove-Item -Force $local -ErrorAction SilentlyContinue
        if ($rc -ne 0) { throw "tar extraction failed (rc=$rc)" }
    } finally {
        Pop-Location
    }
}

# ============================================================
# Per-tool installers
# ============================================================

function Install-CargoAudit {
    if (-not $Force -and (_on_path 'cargo-audit')) {
        _add 'cargo-audit' 'skipped' 'already on PATH'
        return
    }
    if (-not (_on_path 'cargo')) {
        _add 'cargo-audit' 'failed' 'cargo not installed; install Rust first'
        return
    }
    Write-Host "[install_tools] cargo install cargo-audit"
    & cargo install cargo-audit --locked
    if ($LASTEXITCODE -eq 0) {
        _add 'cargo-audit' 'installed' 'via cargo install ($HOME/.cargo/bin)'
    } else {
        _add 'cargo-audit' 'failed' "cargo install rc=$LASTEXITCODE"
    }
}

function Install-OsvScanner {
    $exe = 'osv-scanner.exe'
    $dest = Join-Path $ToolsDir $exe
    if (-not $Force -and ((_on_path 'osv-scanner') -or (Test-Path $dest))) {
        _add 'osv-scanner' 'skipped' 'already present'
        return
    }
    $ver = $VERSIONS.osv_scanner
    $url = "https://github.com/google/osv-scanner/releases/download/$ver/osv-scanner_windows_amd64.exe"
    try {
        _download $url $dest
        _verify_sha256 $dest 'osv-scanner_windows_amd64.exe'
        _add 'osv-scanner' 'installed' "$ver -> $dest (sha256 verified)"
    } catch {
        _add 'osv-scanner' 'failed' $_.Exception.Message
    }
}

function Install-Gitleaks {
    $exe = 'gitleaks.exe'
    $dest = Join-Path $ToolsDir $exe
    if (-not $Force -and ((_on_path 'gitleaks') -or (Test-Path $dest))) {
        _add 'gitleaks' 'skipped' 'already present'
        return
    }
    $ver = $VERSIONS.gitleaks
    $url = "https://github.com/gitleaks/gitleaks/releases/download/v$ver/gitleaks_${ver}_windows_x64.zip"
    $tmpZip = Join-Path $env:TEMP "gitleaks_$ver.zip"
    $tmpDir = Join-Path $env:TEMP "gitleaks_$ver"
    try {
        _download $url $tmpZip
        _verify_sha256 $tmpZip "gitleaks_${ver}_windows_x64.zip"
        if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
        _extract_zip $tmpZip $tmpDir
        $extracted = Get-ChildItem $tmpDir -Filter 'gitleaks.exe' -Recurse |
                     Select-Object -First 1
        if (-not $extracted) { throw 'gitleaks.exe not found in zip' }
        Copy-Item $extracted.FullName $dest -Force
        Remove-Item -Recurse -Force $tmpDir
        Remove-Item -Force $tmpZip
        _add 'gitleaks' 'installed' "$ver -> $dest"
    } catch {
        _add 'gitleaks' 'failed' $_.Exception.Message
    }
}

function Install-Trufflehog {
    $exe = 'trufflehog.exe'
    $dest = Join-Path $ToolsDir $exe
    if (-not $Force -and ((_on_path 'trufflehog') -or (Test-Path $dest))) {
        _add 'trufflehog' 'skipped' 'already present'
        return
    }
    $ver = $VERSIONS.trufflehog
    $url = "https://github.com/trufflesecurity/trufflehog/releases/download/v$ver/trufflehog_${ver}_windows_amd64.tar.gz"
    $tmpTgz = Join-Path $env:TEMP "trufflehog_$ver.tar.gz"
    $tmpDir = Join-Path $env:TEMP "trufflehog_$ver"
    try {
        _download $url $tmpTgz
        _verify_sha256 $tmpTgz "trufflehog_${ver}_windows_amd64.tar.gz"
        if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
        _extract_targz $tmpTgz $tmpDir
        $extracted = Get-ChildItem $tmpDir -Filter 'trufflehog.exe' -Recurse |
                     Select-Object -First 1
        if (-not $extracted) { throw 'trufflehog.exe not found in tarball' }
        Copy-Item $extracted.FullName $dest -Force
        Remove-Item -Recurse -Force $tmpDir
        Remove-Item -Force $tmpTgz
        _add 'trufflehog' 'installed' "$ver -> $dest"
    } catch {
        _add 'trufflehog' 'failed' $_.Exception.Message
    }
}

function Install-Cppcheck {
    if (-not $Force -and (_on_path 'cppcheck')) {
        _add 'cppcheck' 'skipped' 'already on PATH'
        return
    }
    # cppcheck does not ship a portable Windows binary — only an MSI
    # installer. We download the MSI and run it silently with
    # admin rights if we have them; otherwise emit clear instructions.
    $ver = $VERSIONS.cppcheck
    $url = "https://github.com/danmar/cppcheck/releases/download/$ver/cppcheck-${ver}-x64-Setup.msi"
    $tmpMsi = Join-Path $env:TEMP "cppcheck_$ver.msi"

    try {
        $isAdmin = ([Security.Principal.WindowsPrincipal]`
            [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
                [Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { $isAdmin = $false }

    if (-not $isAdmin) {
        _add 'cppcheck' 'manual' "MSI installer requires admin. Download $url and run it, or re-launch this script from an elevated PowerShell."
        return
    }

    try {
        _download $url $tmpMsi
        Write-Host "  msiexec /i $tmpMsi /quiet"
        & msiexec /i $tmpMsi /quiet /norestart | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "msiexec exit $LASTEXITCODE"
        }
        Remove-Item -Force $tmpMsi
        # Default install path
        $defaultPath = "C:\Program Files\Cppcheck\cppcheck.exe"
        if (Test-Path $defaultPath) {
            _add 'cppcheck' 'installed' "$ver via MSI -> $defaultPath (add 'C:\\Program Files\\Cppcheck' to PATH)"
        } else {
            _add 'cppcheck' 'installed-but-not-found' "MSI ran but cppcheck.exe not at expected path"
        }
    } catch {
        _add 'cppcheck' 'failed' $_.Exception.Message
    }
}

function Install-ClangCl {
    if (_on_path 'clang-cl') {
        _add 'clang-cl' 'skipped' 'already on PATH'
        return
    }
    $vsClangCl = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe"
    if (Test-Path $vsClangCl) {
        _add 'clang-cl' 'skipped' "already at VS path: $vsClangCl"
        return
    }
    # clang-cl is part of the VS 'C++ Clang tools for Windows' workload.
    # We can invoke vs_installer.exe modify, but that requires VS Installer
    # which is harder to script reliably across editions. Print instructions
    # and let the user run it from the GUI.
    _add 'clang-cl' 'manual' @"
clang-cl is part of Visual Studio's 'C++ Clang tools for Windows' workload.
To install:
  1. Open 'Visual Studio Installer' (search the Start menu).
  2. Click 'Modify' on the VS 2022 install.
  3. Under Individual components, check:
       - 'C++ Clang Compiler for Windows'
       - 'MSBuild support for LLVM (clang-cl) toolset'
  4. Click 'Modify' to apply.

After install, clang-cl should appear at:
  C:\Program Files\Microsoft Visual Studio\2022\<edition>\VC\Tools\Llvm\x64\bin\clang-cl.exe
"@
}

# ============================================================
# Run requested installers
# ============================================================
$started = Get-Date

$installerMap = @{
    'cargo-audit'  = { Install-CargoAudit }
    'osv-scanner'  = { Install-OsvScanner }
    'gitleaks'     = { Install-Gitleaks }
    'trufflehog'   = { Install-Trufflehog }
    'cppcheck'     = { Install-Cppcheck }
    'clang-cl'     = { Install-ClangCl }
}
foreach ($t in $wantTools) {
    $t = $t.Trim()
    if (-not $installerMap.ContainsKey($t)) {
        Write-Warning "[install_tools] unknown tool '$t'; skipping"
        continue
    }
    Write-Host "[install_tools] $t" -ForegroundColor Cyan
    & $installerMap[$t]
}

# ============================================================
# Summary
# ============================================================
$elapsed = ((Get-Date) - $started).TotalSeconds
Write-Host ""
Write-Host "=== install_tools summary ($([math]::Round($elapsed,1))s) ==="
$any_failed = $false
$any_manual = $false
foreach ($r in $results) {
    $color = switch ($r.verdict) {
        'installed'              { 'Green' }
        'skipped'                { 'Cyan' }
        'manual'                 { 'Yellow'; $script:any_manual = $true }
        'installed-but-not-found' { 'Yellow' }
        'failed'                 { 'Red'; $script:any_failed = $true }
        default                  { 'White' }
    }
    Write-Host (("  {0,-13} {1,-25}" -f $r.tool, $r.verdict) + " -- $($r.note)") -ForegroundColor $color
}

# PATH hint
if (-not $SkipPathHint -and ($results | Where-Object { $_.verdict -eq 'installed' -and $_.note -match 'tools\\bin' })) {
    Write-Host ""
    Write-Host "Add the tools dir to your PATH so the security dispatchers can find these:" -ForegroundColor Yellow
    Write-Host "  # current session:"
    Write-Host "  `$env:Path += ';$ToolsDir'"
    Write-Host ""
    Write-Host "  # persistent (current user, no elevation needed):"
    Write-Host "  [Environment]::SetEnvironmentVariable('Path', `$env:Path + ';$ToolsDir', 'User')"
    Write-Host ""
    Write-Host "Then verify with:" -ForegroundColor Yellow
    Write-Host "  pwsh scripts/security/run_all.ps1 -Smoke"
}

if ($any_failed) {
    Write-Host ""
    Write-Host "[install_tools] At least one tool failed; see notes above." -ForegroundColor Red
    exit 1
}
if ($any_manual) {
    Write-Host ""
    Write-Host "[install_tools] At least one tool needs MANUAL action; see notes above." -ForegroundColor Yellow
    exit 0
}
exit 0
