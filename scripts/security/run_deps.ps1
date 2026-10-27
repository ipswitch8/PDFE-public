#requires -Version 5.1
<#
.SYNOPSIS
  Phase-6 dependency CVE audit. Produces an SBOM covering both the
  C++ vcpkg tree and the Rust tantivy_bridge Cargo tree, runs
  OSV-Scanner against it (with grype as fallback), runs cargo audit
  inside rust/tantivy_bridge, normalizes findings into cves.json,
  compares against the committed baseline, exits non-zero if a NEW
  CVSS-7+ CVE is introduced.

.DESCRIPTION
  OPT-IN: invoked manually or by phase-9's nightly job. Not part
  of regular ctest / pdfe_tests.exe. The dispatcher tolerates
  missing tools (OSV-Scanner / cargo-audit / grype) and warns
  rather than failing -- the SBOM enumeration itself succeeds even
  without an online scanner, and the report flags the gap so the
  partial run is visible rather than masquerading as clean.

  Outputs:
    tests/security/reports/deps/sbom.spdx.json      -- SPDX 2.3 SBOM
    tests/security/reports/deps/cves.json           -- normalized findings
    tests/security/reports/deps/summary.md          -- counts + actions
    tests/security/reports/deps/osv.json            -- raw OSV output
    tests/security/reports/deps/cargo_audit.json    -- raw cargo audit

  Exit codes:
    0 — no NEW high-severity CVEs vs baseline
    1 — at least one NEW CVSS>=7.0 CVE not in baseline
    2 — required input missing (vcpkg.json / Cargo.lock not found)

.PARAMETER VcpkgRoot
  Path to vcpkg checkout. Default: C:\Users\dev\source\vcpkg
  (this machine's install). Override for other systems.

.PARAMETER InstalledDir
  Path to a MANIFEST-mode vcpkg-installed tree (e.g.
  build\vs2022\vcpkg_installed), passed to `vcpkg list --x-install-root`.
  PDFE builds in manifest mode with an explicit -DVCPKG_INSTALLED_DIR, so
  packages never land in $VcpkgRoot's CLASSIC-mode `installed\` directory --
  `vcpkg list --x-json` against $VcpkgRoot alone silently reports an
  incomplete or stale package set (measured 2026-09-07: it omitted qtsvg,
  qttools, tesseract, pugixml and curl -- five of PDFE's direct
  dependencies -- and tripped the SBOM completeness gate below). Default
  '' auto-detects the first existing `build\*\vcpkg_installed` under the
  repo root; pass explicitly to target a specific build tree (a CI runner
  with several build directories should not guess which one is current).

.PARAMETER OsvScannerExe
  Path to osv-scanner. Default: 'osv-scanner' (PATH).

.PARAMETER GrypeExe
  Path to grype (fallback). Default: 'grype' (PATH).

.PARAMETER UpdateBaseline
  Overwrite tests/security/deps/baseline.json with the current cves.json
  CVE ID set after triaging in tests/security/deps/triage.md.

.PARAMETER SkipOsv
  Skip OSV-Scanner / grype (e.g., not installed; use cargo audit only).

.PARAMETER SkipCargoAudit
  Skip cargo audit.

.EXAMPLE
  pwsh scripts/security/run_deps.ps1
  pwsh scripts/security/run_deps.ps1 -SkipOsv
  pwsh scripts/security/run_deps.ps1 -UpdateBaseline
#>
[CmdletBinding()]
param(
    [string]$VcpkgRoot = 'C:\Users\dev\source\vcpkg',
    [string]$InstalledDir = '',
    [string]$OsvScannerExe = 'osv-scanner',
    [string]$GrypeExe = 'grype',
    [switch]$UpdateBaseline,
    [switch]$SkipOsv,
    [switch]$SkipCargoAudit
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path
$DepsDir     = Join-Path $RepoRoot 'tests\security\deps'
$ReportDir   = Join-Path $RepoRoot 'tests\security\reports\deps'
$VcpkgJson   = Join-Path $RepoRoot 'vcpkg.json'
$RustDir     = Join-Path $RepoRoot 'rust\tantivy_bridge'
$CargoLock   = Join-Path $RustDir 'Cargo.lock'

# Resolve OsvScannerExe default. install_tools.ps1 places the binary
# at <repo>/tools/bin/osv-scanner.exe but doesn't modify PATH for
# the current process; probe that location if PATH lookup fails.
if ($OsvScannerExe -eq 'osv-scanner' -and -not (Get-Command osv-scanner -ErrorAction SilentlyContinue)) {
    $localOsv = Join-Path $RepoRoot 'tools\bin\osv-scanner.exe'
    if (Test-Path $localOsv) { $OsvScannerExe = $localOsv }
}

if (-not (Test-Path $VcpkgJson)) {
    Write-Error "[run_deps] $VcpkgJson not found"
    exit 2
}
if (-not (Test-Path $CargoLock)) {
    Write-Warning "[run_deps] $CargoLock not found -- Rust scan will be empty"
}

New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null

# ============================================================
# Build the SBOM
# ============================================================

$packages = @()  # @{name; version; ecosystem; source}

# --- vcpkg side
$vcpkgManifest = Get-Content $VcpkgJson -Raw | ConvertFrom-Json
$declared = @()
foreach ($d in $vcpkgManifest.dependencies) {
    if ($d -is [string]) { $declared += $d }
    else { $declared += $d.name }
}

# Auto-detect a manifest-mode installed tree if none was given. PDFE's
# CMake presets pass -DVCPKG_INSTALLED_DIR=<build dir>/vcpkg_installed, so
# that is where packages actually land -- NOT $VcpkgRoot's classic
# `installed\` directory, which manifest mode never touches.
if (-not $InstalledDir) {
    $candidate = Get-ChildItem -Path (Join-Path $RepoRoot 'build') `
        -Filter 'vcpkg_installed' -Directory -Recurse -Depth 1 `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($candidate) {
        $InstalledDir = $candidate.FullName
        Write-Host "[run_deps] auto-detected manifest install dir: $InstalledDir"
    }
}

$vcpkgListJson = $null
$vcpkgExe = [IO.Path]::Combine($VcpkgRoot, 'vcpkg.exe')
if (Test-Path $vcpkgExe) {
    try {
        Push-Location $RepoRoot
        if ($InstalledDir -and (Test-Path $InstalledDir)) {
            # --x-install-root points vcpkg at the MANIFEST-mode tree PDFE
            # actually builds into (see .PARAMETER InstalledDir above). A
            # plain `vcpkg list --x-json` here queries $VcpkgRoot's classic
            # `installed\` directory instead, which is empty or stale for a
            # manifest-mode project and was measured to omit 5 of PDFE's
            # direct dependencies (qtsvg, qttools, tesseract, pugixml, curl).
            $listOut = & $vcpkgExe list --x-json "--x-install-root=$InstalledDir" 2>$null
        } else {
            Write-Warning "[run_deps] no manifest-mode vcpkg_installed dir found under build\ -- falling back to classic 'vcpkg list', which will likely under-report or miss packages entirely for this manifest-mode project."
            $listOut = & $vcpkgExe list --x-json 2>$null
        }
        Pop-Location
        if ($LASTEXITCODE -eq 0 -and $listOut) {
            $vcpkgListJson = $listOut -join "`n" | ConvertFrom-Json
        }
    } catch { Write-Warning "[run_deps] vcpkg list failed: $_" }
}

if ($vcpkgListJson) {
    foreach ($prop in $vcpkgListJson.PSObject.Properties) {
        $pkg = $prop.Value
        # Only x64-windows triplet entries; skip headers/etc.
        if ($pkg.package_name -and $pkg.version) {
            $packages += @{
                name      = [string]$pkg.package_name
                version   = [string]$pkg.version
                ecosystem = 'vcpkg'
                source    = 'vcpkg-list'
            }
        }
    }
} else {
    # Fallback: only direct deps from vcpkg.json (no transitives)
    foreach ($n in $declared) {
        $packages += @{
            name      = $n
            version   = 'unknown-no-vcpkg-list'
            ecosystem = 'vcpkg'
            source    = 'vcpkg-manifest'
        }
    }
    Write-Warning "[run_deps] vcpkg list not available; SBOM has direct deps only (transitives missing). Install vcpkg at $VcpkgRoot for full coverage."
}

# --- Cargo side: parse Cargo.lock (toml-lite)
function _parse_cargo_lock([string]$path) {
    $pkgs = @()
    if (-not (Test-Path $path)) { return $pkgs }
    $current = @{}
    foreach ($line in Get-Content $path) {
        if ($line -match '^\[\[package\]\]\s*$') {
            if ($current.name) { $pkgs += $current }
            $current = @{}
        } elseif ($line -match '^\s*name\s*=\s*"([^"]+)"\s*$') {
            $current.name = $matches[1]
        } elseif ($line -match '^\s*version\s*=\s*"([^"]+)"\s*$') {
            $current.version = $matches[1]
        }
    }
    if ($current.name) { $pkgs += $current }
    return $pkgs
}

foreach ($p in (_parse_cargo_lock $CargoLock)) {
    $packages += @{
        name      = $p.name
        version   = $p.version
        ecosystem = 'cargo'
        source    = 'Cargo.lock'
    }
}

Write-Host "[run_deps] SBOM has $($packages.Count) packages ($((($packages | Where-Object{$_.ecosystem -eq 'vcpkg'}).Count)) vcpkg + $((($packages | Where-Object{$_.ecosystem -eq 'cargo'}).Count)) cargo)"

# ---------------------------------------------------------------------------
# Meta-audit F13 (SEC-77): DIRECT-DEPENDENCY COMPLETENESS GATE.
#
# Before F13 this script could emit an SBOM that silently omitted a direct
# dependency -- and did: the committed SBOM listed 191 packages while missing
# qtsvg, tesseract, pugixml and curl (SEC-76). `vcpkg list` reports what is
# INSTALLED in the active triplet, so a dependency that is platform-conditional,
# not yet installed, or renamed simply never appears, and nothing noticed.
#
# Every name declared in vcpkg.json must now be present in $packages. If any is
# missing the run FAILS (exit 2) instead of writing an SBOM that a CVE triage
# would then read as "not affected" for a component the product ships.
# ---------------------------------------------------------------------------
$sbomNames = @($packages | ForEach-Object { $_.name })
$missingDirect = @($declared | Where-Object { $sbomNames -notcontains $_ })
if ($missingDirect.Count -gt 0) {
    Write-Error ("[run_deps] SBOM COMPLETENESS FAILURE: vcpkg.json declares " +
        "$($missingDirect.Count) direct dependency/dependencies that the SBOM does " +
        "not enumerate: $($missingDirect -join ', ').`n" +
        "An SBOM missing a direct dependency reports 'not affected' for a " +
        "component PDFE actually ships (meta-audit SEC-76/SEC-77). Install the " +
        "missing ports, or extend the collection above, then re-run. Cross-check " +
        "with: python scripts/security/sbom_direct_deps.py --check")
    exit 2
}

# Emit SPDX 2.3 JSON
$sbomPath = Join-Path $ReportDir 'sbom.spdx.json'
$sbom = @{
    spdxVersion       = 'SPDX-2.3'
    dataLicense       = 'CC0-1.0'
    SPDXID            = 'SPDXRef-DOCUMENT'
    name              = 'PDFE'
    # `-AsUTC` is PowerShell 7-only; use [DateTime]::UtcNow for
    # cross-version compatibility with Windows PowerShell 5.1.
    documentNamespace = "https://github.com/itwerx/PDFE/sbom-$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))"
    creationInfo      = @{
        created   = ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
        creators  = @('Tool: pdfe-run_deps')
    }
    packages = @()
}
# GitHub #181: the element id must be UNIQUE within the document and must use
# only letters, digits, '.' and '-'. The previous scheme keyed it on the package
# NAME alone (and substituted the off-charset '_'), so the committed SBOM carried
# 211 packages under 203 ids -- hashbrown x3, and zstd/foldhash/getrandom/r-efi/
# windows-sys/wit-bindgen x2 each. Ecosystem + version are both required: zstd
# ships from BOTH vcpkg (1.5.7) and cargo (0.13.3). Keep this in lockstep with
# scripts/security/gen_sbom.py's spdx_id_for(); a divergence silently reintroduces
# the defect on the next real run of this script.
function _spdx_component([string]$value) {
    $cleaned = ($value.Trim() -replace '[^A-Za-z0-9.\-]','-')
    if ([string]::IsNullOrEmpty($cleaned)) { return 'NONE' }
    return $cleaned
}
$sbomSeenIds = @{}
foreach ($p in $packages) {
    $spdxId = ('SPDXRef-' + (_spdx_component $p.ecosystem) + '-' +
               (_spdx_component $p.name) + '-' + (_spdx_component $p.version))
    if ($sbomSeenIds.ContainsKey($spdxId)) {
        Write-Error ("[run_deps] SPDXID COLLISION: '$spdxId' would be emitted twice " +
            "(second occurrence: $($p.name)@$($p.version)). SPDX 2.3 requires element " +
            "ids to be unique within a document; a colliding id lets a consumer keep " +
            "only the last package, so CVE triage reports 'not affected' for a version " +
            "that ships (GitHub #181). Refusing to write the SBOM.")
        exit 2
    }
    $sbomSeenIds[$spdxId] = $true
    $sbom.packages += @{
        SPDXID           = $spdxId
        name             = $p.name
        versionInfo      = $p.version
        downloadLocation = 'NOASSERTION'
        filesAnalyzed    = $false
        externalRefs     = @(
            @{
                referenceCategory = 'PACKAGE-MANAGER'
                referenceType     = 'purl'
                referenceLocator  = if ($p.ecosystem -eq 'cargo') {
                                        "pkg:cargo/$($p.name)@$($p.version)"
                                    } else {
                                        "pkg:vcpkg/$($p.name)@$($p.version)"
                                    }
            }
        )
    }
}
# PS 5.1 `Set-Content -Encoding UTF8` writes a BOM, which osv-scanner
# refuses ("invalid character 'ï' looking for beginning of value").
# Use [System.IO.File]::WriteAllText with UTF-8 sans-BOM encoding for
# cross-version BOM-free output.
$sbomJson = $sbom | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($sbomPath, $sbomJson,
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "[run_deps] wrote $sbomPath"

# ============================================================
# Run scanners
# ============================================================

$findings = @()

# --- OSV-Scanner (or grype fallback)
$osvJson = Join-Path $ReportDir 'osv.json'
$ranOsv = $false
if (-not $SkipOsv) {
    $hasOsv = $false
    try { & $OsvScannerExe --version 2>&1 | Out-Null; $hasOsv = ($LASTEXITCODE -eq 0) } catch {}
    if ($hasOsv) {
        Write-Host "[run_deps] osv-scanner --sbom=$sbomPath --format=json"
        # Stdout-only -- osv-scanner writes a "Scanned X as SPDX
        # SBOM and found N packages" message to stderr that, if
        # merged into the JSON output, breaks ConvertFrom-Json.
        & $OsvScannerExe --sbom=$sbomPath --format=json | Set-Content $osvJson -Encoding UTF8
        $ranOsv = $true
    } else {
        $hasGrype = $false
        try { & $GrypeExe version 2>&1 | Out-Null; $hasGrype = ($LASTEXITCODE -eq 0) } catch {}
        if ($hasGrype) {
            Write-Host "[run_deps] grype sbom:$sbomPath -o json"
            & $GrypeExe ('sbom:' + $sbomPath) -o json | Set-Content $osvJson -Encoding UTF8
            $ranOsv = $true
        } else {
            Write-Warning "[run_deps] osv-scanner / grype not found. SBOM CVE scan skipped. Install osv-scanner from https://github.com/google/osv-scanner/releases."
        }
    }
    if ($ranOsv -and (Test-Path $osvJson)) {
        try {
            $osvDoc = Get-Content $osvJson -Raw | ConvertFrom-Json
            # OSV-Scanner schema: results -> packages -> vulnerabilities
            foreach ($r in @($osvDoc.results)) {
                foreach ($pkg in @($r.packages)) {
                    foreach ($v in @($pkg.vulnerabilities)) {
                        $cvss = 0.0
                        foreach ($s in @($v.severity)) {
                            if ($s.score -match '^[0-9.]+$') {
                                $cvss = [double]$s.score; break
                            }
                            # CVSS v3 vector string fallback. We
                            # don't compute the base score here;
                            # instead extract a `Severity:`-style
                            # hint via OSV's separate `severity[].type`
                            # field if present, mapping to a
                            # numeric proxy (CRITICAL=9.0, HIGH=7.5,
                            # MEDIUM=5.0, LOW=2.0). Keeps the gate
                            # alive when scanners emit only vectors.
                            if ($s.type -and -not $cvss) {
                                switch ($s.type.ToString().ToUpper()) {
                                    'CRITICAL' { $cvss = 9.0 }
                                    'HIGH'     { $cvss = 7.5 }
                                    'MEDIUM'   { $cvss = 5.0 }
                                    'MODERATE' { $cvss = 5.0 }
                                    'LOW'      { $cvss = 2.0 }
                                }
                            }
                        }
                        $findings += @{
                            ecosystem      = if ($pkg.package.ecosystem) { [string]$pkg.package.ecosystem.ToLower() } else { 'unknown' }
                            package        = [string]$pkg.package.name
                            version        = [string]$pkg.package.version
                            cve_id         = if ($v.aliases) { @($v.aliases) -join ',' } else { [string]$v.id }
                            cvss           = $cvss
                            fixed_version  = ''
                            advisory_url   = "https://osv.dev/vulnerability/$($v.id)"
                            source         = 'osv-scanner'
                        }
                    }
                }
            }
        } catch {
            Write-Warning "[run_deps] failed to parse OSV/grype output: $_"
        }
    }
}

# --- cargo audit
$cargoAuditJson = Join-Path $ReportDir 'cargo_audit.json'
$ranCargoAudit = $false
if (-not $SkipCargoAudit -and (Test-Path $CargoLock)) {
    $hasCargoAudit = $false
    try {
        Push-Location $RustDir
        & cargo audit --version 2>&1 | Out-Null
        $hasCargoAudit = ($LASTEXITCODE -eq 0)
        Pop-Location
    } catch { $hasCargoAudit = $false; Pop-Location -ErrorAction SilentlyContinue }

    if ($hasCargoAudit) {
        Push-Location $RustDir
        Write-Host "[run_deps] cargo audit (in $RustDir)"
        & cargo audit --json 2>&1 | Set-Content $cargoAuditJson -Encoding UTF8
        Pop-Location
        $ranCargoAudit = $true
    } else {
        Write-Warning "[run_deps] cargo audit not installed. Run 'cargo install cargo-audit' to enable."
    }

    if ($ranCargoAudit -and (Test-Path $cargoAuditJson)) {
        try {
            $auditDoc = Get-Content $cargoAuditJson -Raw | ConvertFrom-Json
            foreach ($v in @($auditDoc.vulnerabilities.list)) {
                $advisory = $v.advisory
                $cvss = 0.0
                # cargo audit doesn't always populate cvss; try cvss field then
                # heuristic from informational/warning class.
                if ($advisory.cvss) { $cvss = [double]$advisory.cvss }
                $findings += @{
                    ecosystem      = 'cargo'
                    package        = [string]$v.package.name
                    version        = [string]$v.package.version
                    cve_id         = [string]$advisory.id
                    cvss           = $cvss
                    fixed_version  = if ($v.versions.patched) { (@($v.versions.patched) -join ',') } else { '' }
                    advisory_url   = [string]$advisory.url
                    source         = 'rustsec'
                }
            }
        } catch {
            Write-Warning "[run_deps] failed to parse cargo audit output: $_"
        }
    }
}

# ============================================================
# Normalize cves.json
# ============================================================
$cves = @{
    schema_version = 1
    stamp          = ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
    sbom           = $sbomPath
    scanners_run   = @{
        osv          = $ranOsv
        cargo_audit  = $ranCargoAudit
    }
    findings       = $findings
    counts         = @{
        total         = $findings.Count
        cvss_ge_7     = (@($findings | Where-Object { [double]$_.cvss -ge 7.0 })).Count
        cvss_ge_9     = (@($findings | Where-Object { [double]$_.cvss -ge 9.0 })).Count
        by_ecosystem  = @{
            vcpkg = (@($findings | Where-Object { $_.ecosystem -eq 'vcpkg' })).Count
            cargo = (@($findings | Where-Object { $_.ecosystem -eq 'cargo' })).Count
        }
    }
}
$cvesPath = Join-Path $ReportDir 'cves.json'
$cves | ConvertTo-Json -Depth 6 | Set-Content -Path $cvesPath -Encoding UTF8

# ============================================================
# MuPDF + QPDF explicit advisory check
# ============================================================
$mupdfBumpRequired = $findings | Where-Object {
    $_.package -match '^(libmupdf|mupdf)$' -and [double]$_.cvss -ge 7.0
}
$qpdfBumpRequired = $findings | Where-Object {
    $_.package -eq 'qpdf' -and [double]$_.cvss -ge 7.0
}
$criticalLibBump = @() + $mupdfBumpRequired + $qpdfBumpRequired

# ============================================================
# Summary md
# ============================================================
$summaryPath = Join-Path $ReportDir 'summary.md'
$lines = @(
    '# Dependency CVE audit summary',
    '',
    "Stamp: $($cves.stamp)",
    "SBOM: $($packages.Count) packages",
    '',
    '## Counts',
    '',
    '| Metric | Value |',
    '|---|---:|',
    "| Total findings | $($cves.counts.total) |",
    "| CVSS >= 7.0    | $($cves.counts.cvss_ge_7) |",
    "| CVSS >= 9.0    | $($cves.counts.cvss_ge_9) |",
    "| vcpkg          | $($cves.counts.by_ecosystem.vcpkg) |",
    "| cargo          | $($cves.counts.by_ecosystem.cargo) |",
    '',
    '## Scanners',
    '',
    "- OSV-Scanner / grype: $($ranOsv)",
    "- cargo audit:         $($ranCargoAudit)",
    ''
)
if ($criticalLibBump.Count -gt 0) {
    $lines += '## CRITICAL: MuPDF / QPDF advisory hits'
    $lines += ''
    foreach ($f in $criticalLibBump) {
        $lines += "- $($f.package) $($f.version): $($f.cve_id) (CVSS $($f.cvss)) -- $($f.advisory_url)"
    }
    $lines += ''
}
$lines | Set-Content -Path $summaryPath -Encoding UTF8

# ============================================================
# Baseline comparison
# ============================================================
$baselinePath = Join-Path $DepsDir 'baseline.json'
if ($UpdateBaseline) {
    $baselineCves = @($findings | Where-Object { [double]$_.cvss -ge 7.0 } | ForEach-Object { $_.cve_id })
    @{
        schema_version = 1
        stamp          = $cves.stamp
        cve_ids_ge_7   = @($baselineCves)
        note           = 'CVE IDs (CVSS >= 7.0) accepted in current baseline. Only update via -UpdateBaseline after triaging in tests/security/deps/triage.md.'
    } | ConvertTo-Json -Depth 4 | Set-Content -Path $baselinePath -Encoding UTF8
    Write-Host "[run_deps] baseline updated: $baselinePath"
    exit 0
}

if (-not (Test-Path $baselinePath)) {
    Write-Warning "[run_deps] no baseline at $baselinePath. First run: review cves.json + triage.md, then -UpdateBaseline."
    if ($criticalLibBump.Count -gt 0) {
        Write-Host "[run_deps] CRITICAL: MuPDF/QPDF unpatched-CVSS>=7 finding present (see summary.md)"
        exit 1
    }
    exit 0
}

$baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
$baselineSet = @{}
foreach ($id in $baseline.cve_ids_ge_7) { $baselineSet[$id] = $true }

$newHigh = $findings | Where-Object {
    [double]$_.cvss -ge 7.0 -and -not $baselineSet.ContainsKey($_.cve_id)
}
if ($newHigh) {
    Write-Host "[run_deps] FAIL -- $($newHigh.Count) NEW high-severity CVE(s) (CVSS >= 7.0):"
    foreach ($f in $newHigh) {
        Write-Host "  $($f.ecosystem)/$($f.package) $($f.version): $($f.cve_id) (CVSS $($f.cvss)) $($f.advisory_url)"
    }
    Write-Host "[run_deps] If accepted, document in tests/security/deps/triage.md and -UpdateBaseline."
    exit 1
}
# AC7 enforcement: any MuPDF/QPDF CVSS>=7 finding NOT in the
# accepted baseline fails the phase regardless of whether the
# baseline-vs-new-CVE check above already caught it. Written as
# "filter for unaccepted entries; exit 1 if any remain" rather
# than the inverted form so the intent is obvious to the next
# reader.
$criticalLibUnaccepted = $criticalLibBump | Where-Object {
    -not $baselineSet.ContainsKey($_.cve_id)
}
if ($criticalLibUnaccepted) {
    Write-Host "[run_deps] FAIL -- $($criticalLibUnaccepted.Count) MuPDF/QPDF unpatched-CVSS>=7 finding(s) not in baseline (AC7)."
    foreach ($f in $criticalLibUnaccepted) {
        Write-Host "  $($f.package) $($f.version): $($f.cve_id) (CVSS $($f.cvss))"
    }
    exit 1
}

Write-Host "[run_deps] PASS -- $($findings.Count) total findings, no new CVSS>=7.0 vs baseline."
exit 0
