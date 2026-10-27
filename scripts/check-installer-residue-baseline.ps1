# -*- coding: utf-8 -*-
<#
.SYNOPSIS
    Self-consistency check: scripts/installer-expected-residue-baseline.json vs
    docs/installer-residue-and-hkcu-intent.md (+ packaging/file_assoc_uninstall.nsh).

.DESCRIPTION
    The expected-residue baseline consumed by scripts/vm-install-residue-harness.ps1
    is only trustworthy if it says exactly what the authoritative intent note says.
    This script RE-DERIVES every baseline section from the documents and reports any
    difference. It performs NO install, NO uninstall, touches NO registry and needs
    no elevation -- it is a pure text/JSON comparison, safe to run anywhere, and it
    is the piece of the F7 lane that is verifiable without a disposable VM.

    Checks performed (each contributes to `differences` on failure):

      1. installer_sources        == the "## Installer sources" table in the note
                                     (all three columns, verbatim).
      2. hkcu_intent              == the note's decision keyword (PRESERVED).
      3. expected_surviving.registry
                                  == the set of HKCU\Software\PDFE[\PDFE] keys the
                                     note names as preserved user data.
      4. expected_surviving.filesystem is empty AND the note justifies it by
         recording QSettings NativeFormat (registry-backed) storage.
      5. uninstall_directives.required / .forbidden == the literal directives the
         note requires / forbids, AND those literals are respectively present in /
         absent from packaging/file_assoc_uninstall.nsh.
      6. must_be_absent_after_uninstall.registry, minus the CPack-generated ARP key,
         == the directive targets parsed out of packaging/file_assoc_uninstall.nsh.
      7. every entry of must_be_absent_after_uninstall.registry_cpack_generated is
         justified by a row of the note's installer-sources table.
      8. must_be_absent_after_uninstall.filesystem == the installer-created paths
         named in the note.
      9. user_data_sentinels all lie INSIDE the preserved scope (a sentinel outside
         it would make the harness assert preservation of something the intent note
         never promised to preserve).

.PARAMETER OutFile
    Where to write the machine-readable result. Defaults to
    docs/audit/meta-audit-2026-07/followup/phase-7/baseline-consistency.json.

.PARAMETER AlsoWriteTo
    Additional path(s) to receive an identical copy (used to place the gate-artifact
    copy under .claude/pipelines/... which is gitignored).

.EXAMPLE
    pwsh -File scripts/check-installer-residue-baseline.ps1

.NOTES
    Exit code 0 = zero differences; 1 = at least one difference (details in the JSON).
#>
[CmdletBinding()]
param(
    [string]   $OutFile,
    [string[]] $AlsoWriteTo = @()
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$baselinePath = Join-Path $repoRoot 'scripts/installer-expected-residue-baseline.json'
$docPath      = Join-Path $repoRoot 'docs/installer-residue-and-hkcu-intent.md'
$uninstNsh    = Join-Path $repoRoot 'packaging/file_assoc_uninstall.nsh'

if (-not $OutFile) {
    $OutFile = Join-Path $repoRoot 'docs/audit/meta-audit-2026-07/followup/phase-7/baseline-consistency.json'
}

foreach ($p in @($baselinePath, $docPath, $uninstNsh)) {
    if (-not (Test-Path $p)) { throw "required input missing: $p" }
}

$baseline = Get-Content -LiteralPath $baselinePath -Raw -Encoding UTF8 | ConvertFrom-Json
$doc      = Get-Content -LiteralPath $docPath      -Raw -Encoding UTF8
$uninst   = Get-Content -LiteralPath $uninstNsh    -Raw -Encoding UTF8

$differences = New-Object System.Collections.Generic.List[object]
$checks      = New-Object System.Collections.Generic.List[object]

function Add-Difference {
    param([string]$Check, [string]$Detail, $Expected, $Actual)
    $differences.Add([ordered]@{ check = $Check; detail = $Detail; expected = $Expected; actual = $Actual })
}

function Add-Check {
    param([string]$Name, [string]$Derivation, [int]$Before)
    $checks.Add([ordered]@{
        name       = $Name
        derivation = $Derivation
        result     = $(if ($differences.Count -eq $Before) { 'MATCH' } else { 'DIFFERENT' })
    })
}

function Compare-Set {
    param([string]$Check, [string[]]$Expected, [string[]]$Actual)
    $e = @($Expected | Sort-Object)
    $a = @($Actual   | Sort-Object)
    foreach ($m in ($e | Where-Object { $a -notcontains $_ })) {
        Add-Difference $Check 'present in the document-derived set but missing from the baseline' $m $null
    }
    foreach ($m in ($a | Where-Object { $e -notcontains $_ })) {
        Add-Difference $Check 'present in the baseline but not derivable from the document' $null $m
    }
}

# --- 1. installer_sources table --------------------------------------------
$before = $differences.Count
$sec = ($doc -split '## Installer sources', 2)[1]
$sec = ($sec -split "`n## ", 2)[0]
$rows = @()
foreach ($line in ($sec -split "`r?`n")) {
    $t = $line.Trim()
    if (-not $t.StartsWith('|')) { continue }
    $cells = @($t.Trim('|') -split '\|' | ForEach-Object { $_.Trim() })
    if ($cells.Count -lt 3) { continue }
    if ($cells[0] -match '^-+$' -or $cells[0] -eq 'Artifact') { continue }
    $rows += , $cells
}
if ($rows.Count -ne @($baseline.installer_sources).Count) {
    Add-Difference 'installer_sources' 'row count differs' $rows.Count @($baseline.installer_sources).Count
} else {
    for ($i = 0; $i -lt $rows.Count; $i++) {
        $b = $baseline.installer_sources[$i]
        foreach ($pair in @(@('artifact', 0), @('created_by', 1), @('removed_by', 2))) {
            if ($b.($pair[0]) -ne $rows[$i][$pair[1]]) {
                Add-Difference 'installer_sources' "row $($i+1) column '$($pair[0])' differs" $rows[$i][$pair[1]] $b.($pair[0])
            }
        }
    }
}
Add-Check 'installer_sources' 'the "## Installer sources" table in docs/installer-residue-and-hkcu-intent.md' $before

# --- 2. hkcu_intent decision ------------------------------------------------
$before = $differences.Count
$docIntent = if ($doc -match '##\s*HKCU user-data intent:\s*\*\*(?<kw>[A-Za-z]+)') { $Matches['kw'].ToUpperInvariant() } else { '<not found>' }
if ($docIntent -ne $baseline.hkcu_intent) {
    Add-Difference 'hkcu_intent' 'baseline decision keyword differs from the note heading' $docIntent $baseline.hkcu_intent
}
Add-Check 'hkcu_intent' 'the "## HKCU user-data intent: **...**" heading' $before

# --- 3. preserved HKCU keys -------------------------------------------------
$before = $differences.Count
$docHkcu = @([regex]::Matches($doc, 'HKCU\\Software\\PDFE(?:\\PDFE)?') | ForEach-Object { $_.Value } | Sort-Object -Unique)
Compare-Set 'expected_surviving.registry' $docHkcu @($baseline.expected_surviving.registry)
Add-Check 'expected_surviving.registry' 'every HKCU\Software\PDFE[\PDFE] key named in the intent note' $before

# --- 4. no filesystem survivors, and the note justifies it ------------------
$before = $differences.Count
if (@($baseline.expected_surviving.filesystem).Count -ne 0) {
    Add-Difference 'expected_surviving.filesystem' 'the note records registry-backed (QSettings NativeFormat) user data only; no filesystem survivor is documented' @() @($baseline.expected_surviving.filesystem)
}
if ($doc -notmatch 'NativeFormat') {
    Add-Difference 'expected_surviving.filesystem' 'the note no longer records the NativeFormat (registry) storage backend that justifies an empty filesystem-survivor set' 'NativeFormat' '<absent>'
}
Add-Check 'expected_surviving.filesystem' 'the note''s QSettings NativeFormat statement' $before

# --- 5. required / forbidden uninstall directives ---------------------------
$before = $differences.Count
foreach ($d in @($baseline.uninstall_directives.required)) {
    if ($doc -notmatch [regex]::Escape($d))    { Add-Difference 'uninstall_directives.required' 'baseline requires a directive the intent note does not' $null $d }
    if ($uninst -notmatch [regex]::Escape($d)) { Add-Difference 'uninstall_directives.required' 'required directive is missing from packaging/file_assoc_uninstall.nsh' $d '<absent>' }
}
foreach ($d in @($baseline.uninstall_directives.forbidden)) {
    if ($doc -notmatch [regex]::Escape($d)) { Add-Difference 'uninstall_directives.forbidden' 'baseline forbids a directive the intent note does not name as forbidden' $null $d }
    if ($uninst -match [regex]::Escape($d))  { Add-Difference 'uninstall_directives.forbidden' 'forbidden directive IS present in packaging/file_assoc_uninstall.nsh -- user data would be destroyed against the documented intent' '<absent>' $d }
}
Add-Check 'uninstall_directives' 'the note''s scope-guard paragraph, cross-checked against packaging/file_assoc_uninstall.nsh' $before

# --- 6. must-be-absent registry set from the uninstall .nsh -----------------
$before = $differences.Count
$derived = @()
foreach ($m in [regex]::Matches($uninst, '(?im)^\s*DeleteRegKey\s+(?<root>HK\w+)\s+"(?<key>[^"]+)"')) {
    $derived += ('{0}\{1}' -f $m.Groups['root'].Value.ToUpperInvariant(), $m.Groups['key'].Value)
}
foreach ($m in [regex]::Matches($uninst, '(?im)^\s*DeleteRegValue\s+(?<root>HK\w+)\s+"(?<key>[^"]+)"\s+"(?<val>[^"]*)"')) {
    $derived += ('{0}\{1}!{2}' -f $m.Groups['root'].Value.ToUpperInvariant(), $m.Groups['key'].Value, $m.Groups['val'].Value)
}
$cpackGen = @($baseline.must_be_absent_after_uninstall.registry_cpack_generated)
$baseNsh  = @($baseline.must_be_absent_after_uninstall.registry | Where-Object { $cpackGen -notcontains $_ })
Compare-Set 'must_be_absent_after_uninstall.registry' $derived $baseNsh
Add-Check 'must_be_absent_after_uninstall.registry' 'every DeleteRegKey/DeleteRegValue target in packaging/file_assoc_uninstall.nsh' $before

# --- 7. CPack-generated ARP key justified by the note -----------------------
$before = $differences.Count
foreach ($k in $cpackGen) {
    if (@($baseline.must_be_absent_after_uninstall.registry) -notcontains $k) {
        Add-Difference 'registry_cpack_generated' 'CPack-generated key is not listed in the must-be-absent set it qualifies' $k '<absent>'
    }
    $leaf = ($k -split '\\')[-1]
    $parent = ($k -split '\\')[-2]
    if ($doc -notmatch ('{0}\\+\.\.\.\\+{1}\\+{2}' -f 'HKLM', $parent, $leaf)) {
        Add-Difference 'registry_cpack_generated' 'the intent note has no installer-sources row documenting this CPack-generated key' ('a row naming HKLM\...\{0}\{1}' -f $parent, $leaf) $k
    }
}
Add-Check 'registry_cpack_generated' 'the installer-sources row documenting the CPack/NSIS uninstall (ARP) key' $before

# --- 8. must-be-absent filesystem paths -------------------------------------
$before = $differences.Count
$docPaths = @()
if ($doc -match '\$INSTDIR')                                        { $docPaths += '$INSTDIR' }
if ($doc -match '\$DESKTOP\\PDFE\.lnk')                             { $docPaths += '$DESKTOP\PDFE.lnk' }
if ($doc -match '\$SMPROGRAMS\\\$STARTMENU_FOLDER\\PDFE\.lnk')      { $docPaths += '$SMPROGRAMS\$STARTMENU_FOLDER\PDFE.lnk' }
Compare-Set 'must_be_absent_after_uninstall.filesystem' $docPaths @($baseline.must_be_absent_after_uninstall.filesystem)
Add-Check 'must_be_absent_after_uninstall.filesystem' 'the installer-created paths named in the intent note' $before

# --- 9. sentinels inside the preserved scope --------------------------------
$before = $differences.Count
$sentinelReg = [string]$baseline.user_data_sentinels.registry_value
$sentinelDoc = [string]$baseline.user_data_sentinels.document_path
if ($sentinelReg -notmatch '^HKCU\\Software\\PDFE\\') {
    Add-Difference 'user_data_sentinels' 'registry sentinel lies outside the preserved HKCU\Software\PDFE scope' 'a value under HKCU\Software\PDFE\...' $sentinelReg
}
if ($sentinelDoc -notmatch '^%LOCALAPPDATA%\\PDFE\\') {
    Add-Difference 'user_data_sentinels' 'document sentinel lies outside the per-user app-data scope' 'a path under %LOCALAPPDATA%\PDFE\...' $sentinelDoc
}
Add-Check 'user_data_sentinels' 'the preserved HKCU scope + per-user app-data scope' $before

# --- Result -----------------------------------------------------------------
$result = [ordered]@{
    schema           = 'pdfe/baseline-consistency@1'
    phase            = 'phase-7'
    generated_utc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    generated_by     = 'scripts/check-installer-residue-baseline.ps1'
    baseline         = 'scripts/installer-expected-residue-baseline.json'
    source_of_truth  = 'docs/installer-residue-and-hkcu-intent.md'
    registry_authority = 'packaging/file_assoc_uninstall.nsh'
    checks           = $checks
    difference_count = $differences.Count
    differences      = $differences
    verdict          = $(if ($differences.Count -eq 0) { 'CONSISTENT' } else { 'INCONSISTENT' })
}

$json = $result | ConvertTo-Json -Depth 8
foreach ($dest in (@($OutFile) + $AlsoWriteTo)) {
    $dir = Split-Path -Parent $dest
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($dest, $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "wrote $dest"
}

Write-Host ("verdict: {0} ({1} difference(s))" -f $result.verdict, $differences.Count)
if ($differences.Count -ne 0) {
    $differences | ForEach-Object { Write-Host ("  - [{0}] {1}" -f $_.check, $_.detail) -ForegroundColor Red }
    exit 1
}
exit 0
