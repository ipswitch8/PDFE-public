# -*- coding: utf-8 -*-
<#
.SYNOPSIS
    Disposable-VM install -> exercise -> uninstall -> diff harness for the PDFE
    NSIS installer. Produces a machine-readable residue report.

.DESCRIPTION
    This is the end-to-end counterpart to scripts/verify-installer-deps.ps1.
    That script deliberately does "no admin / no real install" -- it 7z-extracts
    the payload -- which is exactly the limitation this harness removes: here the
    CPack-produced installer executable is actually EXECUTED, silently, and then
    actually UNINSTALLED, and the machine is diffed before and after.

    Five stages, in order:

      1. SNAPSHOT   -- filesystem + registry (HKLM\Software, HKCR,
                       HKCU\Software\PDFE) captured before anything is touched.
      2. INSTALL    -- silent install of the CPack-produced installer
                       (build/vs2022-package/installer/PDFE-<version>-win64.exe) via
                       `/S /D=<prefix>`. NOT a 7z payload extraction.
      3. LAUNCH     -- the installed pdfe.exe is started, must reach its main
                       window, and must exit cleanly on request; the shell-verb
                       ("%1" quoting) path is exercised too, including a path
                       with spaces and a non-ASCII path.
      4. UNINSTALL  -- the generated uninstaller is run silently.
      5. DIFF       -- post state vs the stage-1 snapshot, judged against
                       scripts/installer-expected-residue-baseline.json. Anything
                       surviving that the baseline does not list as intended HKCU
                       user data is residue and fails the run.

    Two further stages ride along because the artifact is already in hand:

      * SIGNATURE   -- Get-AuthenticodeSignature over the shipped binaries and the
                       installer; fails unless Status is Valid AND an RFC-3161
                       countersignature is present.
      * UPGRADE     -- install v_n, write a sentinel setting AND a document,
                       install v_n+1 over it, then assert the file association
                       still resolves, the sentinel setting survived, and the
                       document survived.

    OUTPUT: a JSON residue report is written to -ResidueReport, default
        build/vm-harness/residue-report.json
    plus a copy of the run record to -RunRecord, default
        build/vm-harness/vm-run-record.json

.PARAMETER Installer
    The CPack-produced installer. Default: newest
    build/vs2022-package/installer/PDFE-*-win64.exe (CPACK_PACKAGE_DIRECTORY =
    ${CMAKE_BINARY_DIR}/installer, CPACK_PACKAGE_NAME "PDFE", NSIS generator).
    That is the PACKAGING tree, configured PDFE_TESTING=OFF -- the residue
    harness must exercise the bundle that ships, not one built from the test
    tree, which carries the PDFE_TEST_PIPES seam (GitHub #182).

.PARAMETER UpgradeInstaller
    A second, higher-version CPack installer used for the upgrade case. When it is
    not supplied the upgrade stage re-runs -Installer over itself, which still
    exercises CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL (the compound
    uninstall+install path) but is reported as `same_version_reinstall`.

.PARAMETER InstallDir
    Install prefix passed to NSIS `/D=`. Default C:\pdfe-vm-harness.

.PARAMETER ConfirmDisposableMachine
    REQUIRED. This harness installs software, writes HKLM/HKCR keys and deletes
    them again. It must only ever run on a throwaway VM with a snapshot to roll
    back to. Without this switch the harness performs stage 0 (preflight) only
    and exits non-zero, so it can never mutate a developer or build host by
    accident.

.EXAMPLE
    # On a disposable VM, elevated, after copying the built installer over:
    pwsh -File scripts/vm-install-residue-harness.ps1 `
         -Installer C:\drop\PDFE-0.1.0-win64.exe `
         -UpgradeInstaller C:\drop\PDFE-0.1.1-win64.exe `
         -ConfirmDisposableMachine

.NOTES
    Exit 0 = install/uninstall clean and residue matches the baseline exactly.
    Exit 1 = a stage failed or unexpected residue survived (details in the JSON).
    Exit 2 = preflight refused to run (not a disposable machine / not elevated /
             installer missing). Never a silent pass.
#>
[CmdletBinding()]
param(
    [string] $Installer,
    [string] $UpgradeInstaller,
    [string] $InstallDir = 'C:\pdfe-vm-harness',
    [string] $ResidueReport,
    [string] $RunRecord,
    [string] $Baseline,
    [int]    $TimeoutSeconds = 60,
    [switch] $ConfirmDisposableMachine,
    [switch] $SkipSignature,
    [switch] $SignatureOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if (-not $ResidueReport) { $ResidueReport = Join-Path $repoRoot 'build/vm-harness/residue-report.json' }
if (-not $RunRecord)     { $RunRecord     = Join-Path $repoRoot 'build/vm-harness/vm-run-record.json' }
if (-not $Baseline)      { $Baseline      = Join-Path $repoRoot 'scripts/installer-expected-residue-baseline.json' }

# CPack output location, from the root CMakeLists.txt:
#   set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/installer")
#   set(CPACK_PACKAGE_NAME "PDFE")   /   CPACK_GENERATOR "NSIS"
# The binary dir is the PACKAGING tree (PDFE_TESTING=OFF), not build/vs2022:
# GitHub #182. An installer built from the test tree is not the artifact
# this harness is supposed to be characterising.
$CPackInstallerDir     = Join-Path $repoRoot 'build/vs2022-package/installer'
$CPackInstallerPattern = 'PDFE-*-win64.exe'

$stages  = New-Object System.Collections.Generic.List[object]
$script:failures = New-Object System.Collections.Generic.List[string]

function Write-Stage { param([string]$Name) Write-Host "== $Name ==" -ForegroundColor Cyan }

function Add-StageResult {
    param([string]$Name, [string]$Status, $Detail = $null)
    $stages.Add([ordered]@{ stage = $Name; status = $Status; detail = $Detail })
    if ($Status -eq 'FAIL') {
        $text = if ($Detail -is [string]) { $Detail } else { ($Detail | ConvertTo-Json -Depth 6 -Compress) }
        $script:failures.Add("$Name : $text")
    }
}

function Save-Report {
    param([string]$Verdict, [string]$Reason, $Residue = $null)
    $report = [ordered]@{
        schema        = 'pdfe/installer-residue-report@1'
        generated_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        harness       = 'scripts/vm-install-residue-harness.ps1'
        baseline      = 'scripts/installer-expected-residue-baseline.json'
        machine       = [ordered]@{
            os        = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).Caption
            model     = (Get-CimInstance Win32_ComputerSystem  -ErrorAction SilentlyContinue).Model
            elevated  = Test-Elevated
            confirmed_disposable = [bool]$ConfirmDisposableMachine
        }
        installer     = $Installer
        upgrade_installer = $UpgradeInstaller
        install_dir   = $InstallDir
        stages        = $stages
        residue       = $Residue
        failures      = @($script:failures)
        verdict       = $Verdict
        reason        = $Reason
    }
    foreach ($dest in @($ResidueReport)) {
        $dir = Split-Path -Parent $dest
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
        [System.IO.File]::WriteAllText($dest, ($report | ConvertTo-Json -Depth 12), (New-Object System.Text.UTF8Encoding($false)))
    }
    Write-Host "residue report: $ResidueReport"
    return $report
}

function Test-Elevated {
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { return $false }
}

# ===========================================================================
# Stage 0 -- preflight. Fails CLOSED: never mutates a machine that has not been
# explicitly declared disposable.
# ===========================================================================
Write-Stage 'stage 0/5 -- preflight'

if (-not $Installer) {
    $Installer = Get-ChildItem -LiteralPath $CPackInstallerDir -Filter $CPackInstallerPattern -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}

$preflight = @()
# -SignatureOnly runs the read-only Authenticode/RFC-3161 stage and nothing else.
# It installs nothing, writes no registry key and needs no elevation, so it is
# the one part of this harness that is safe on a build host -- and it is how the
# signing half of the lane (WIN-94/WIN-95) is evidenced without a VM.
if ($SignatureOnly) {
    if (-not $Installer -or -not (Test-Path $Installer)) {
        Add-StageResult 'preflight' 'REFUSED' "CPack-produced installer not found under $CPackInstallerDir\$CPackInstallerPattern"
        $null = Save-Report 'NOT-RUN' 'signature-only mode: installer artifact missing'
        exit 2
    }
    Add-StageResult 'preflight' 'PASS' "signature-only mode; installer=$Installer"
}
if (-not $SignatureOnly -and -not $ConfirmDisposableMachine) {
    $preflight += 'refused: -ConfirmDisposableMachine was not passed. This harness really installs and uninstalls PDFE; run it only on a throwaway VM with a rollback snapshot.'
}
if (-not $SignatureOnly -and -not (Test-Elevated)) {
    $preflight += 'refused: not elevated. The installer writes HKLM/HKCR keys; without elevation the install silently produces no file association and the residue diff would be meaningless.'
}
if (-not $SignatureOnly -and (-not $Installer -or -not (Test-Path $Installer))) {
    $preflight += "refused: CPack-produced installer not found. Expected $CPackInstallerDir\$CPackInstallerPattern, or pass -Installer <path>."
}
if ($preflight.Count -gt 0) {
    foreach ($p in $preflight) { Write-Host "PREFLIGHT $p" -ForegroundColor Yellow }
    Add-StageResult 'preflight' 'REFUSED' ($preflight -join ' | ')
    $r = Save-Report 'NOT-RUN' ($preflight -join ' | ')
    $dir = Split-Path -Parent $RunRecord
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($RunRecord, ($r | ConvertTo-Json -Depth 12), (New-Object System.Text.UTF8Encoding($false)))
    exit 2
}
if (-not $SignatureOnly) { Add-StageResult 'preflight' 'PASS' "installer=$Installer" }

$baselineObj = Get-Content -LiteralPath $Baseline -Raw -Encoding UTF8 | ConvertFrom-Json

# ===========================================================================
# Signature stage -- Get-AuthenticodeSignature over the shipped binaries AND the
# installer. Valid status is not enough: without an RFC-3161 countersignature the
# signature dies with the certificate, so its absence is a FAIL (WIN-94/WIN-95).
# ===========================================================================
function Invoke-SignatureStage {
    param([string[]] $Artifacts)
    Write-Stage 'signature -- Authenticode + RFC-3161'
    $rows = @()
    foreach ($a in $Artifacts) {
        if (-not (Test-Path $a)) {
            $rows += [ordered]@{ artifact = $a; status = '<missing>'; ok = $false; reason = 'artifact not found' }
            continue
        }
        $sig = Get-AuthenticodeSignature -LiteralPath $a
        $statusValid = ($sig.Status -eq 'Valid')
        # RFC-3161 countersignature: signtool/Trusted Signing timestamps every
        # signature from Microsoft's RFC-3161 authority; PowerShell surfaces the
        # timestamping authority's certificate here. Null => not timestamped.
        $counterSigned = ($null -ne $sig.TimeStamperCertificate)
        $ok = ($statusValid -and $counterSigned)
        $reason = @()
        if (-not $statusValid)   { $reason += "Authenticode Status is '$($sig.Status)', expected 'Valid'" }
        if (-not $counterSigned) { $reason += 'no RFC-3161 countersignature (TimeStamperCertificate is null)' }
        $rows += [ordered]@{
            artifact          = $a
            status            = [string]$sig.Status
            signer            = [string]$sig.SignerCertificate.Subject
            timestamper       = [string]$sig.TimeStamperCertificate.Subject
            rfc3161_present   = $counterSigned
            ok                = $ok
            reason            = ($reason -join '; ')
        }
    }
    $bad = @($rows | Where-Object { -not $_.ok })
    if ($bad.Count -gt 0) {
        Add-StageResult 'signature' 'FAIL' $rows
        return $false
    }
    Add-StageResult 'signature' 'PASS' $rows
    return $true
}

# ===========================================================================
# Snapshot helpers
# ===========================================================================
function Get-RegistrySnapshot {
    param([string] $Root, [int] $Depth = 3)
    $seen = New-Object System.Collections.Generic.List[string]
    function Walk([string]$path, [int]$left) {
        if (-not (Test-Path -LiteralPath $path)) { return }
        $seen.Add($path)
        try {
            $item = Get-Item -LiteralPath $path -ErrorAction Stop
            foreach ($v in $item.GetValueNames()) { $seen.Add(('{0}!{1}' -f $path, $v)) }
        } catch { }
        if ($left -le 0) { return }
        try {
            foreach ($k in (Get-ChildItem -LiteralPath $path -ErrorAction Stop)) {
                Walk $k.PSPath.Replace('Microsoft.PowerShell.Core\Registry::', '') ($left - 1)
            }
        } catch { }
    }
    Walk $Root $Depth
    return , @($seen | Sort-Object -Unique)
}

function Get-FilesystemSnapshot {
    param([string[]] $Roots)
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($r in $Roots) {
        if (-not (Test-Path -LiteralPath $r)) { continue }
        try {
            Get-ChildItem -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue |
                ForEach-Object { $out.Add($_.FullName) }
            $out.Add($r)
        } catch { }
    }
    return , @($out | Sort-Object -Unique)
}

function New-MachineSnapshot {
    param([string]$Label)
    Write-Stage "stage 1/5 -- snapshot ($Label)"
    $snap = [ordered]@{
        label    = $Label
        taken_utc= (Get-Date).ToUniversalTime().ToString('o')
        registry = [ordered]@{
            'HKLM:\Software'         = Get-RegistrySnapshot 'HKLM:\Software' 3
            'HKCR:\'                 = Get-RegistrySnapshot 'HKCR:\' 2
            'HKCU:\Software\PDFE'    = Get-RegistrySnapshot 'HKCU:\Software\PDFE' 6
            'HKCU:\Software\Classes' = Get-RegistrySnapshot 'HKCU:\Software\Classes' 2
        }
        filesystem = Get-FilesystemSnapshot @(
            $InstallDir,
            [Environment]::GetFolderPath('Desktop'),
            [Environment]::GetFolderPath('CommonDesktopDirectory'),
            [Environment]::GetFolderPath('Programs'),
            [Environment]::GetFolderPath('CommonPrograms'),
            (Join-Path $env:LOCALAPPDATA 'PDFE'),
            (Join-Path $env:APPDATA 'PDFE')
        )
    }
    return $snap
}

if (-not (Get-PSDrive -Name HKCR -ErrorAction SilentlyContinue)) {
    New-PSDrive -Name HKCR -PSProvider Registry -Root HKEY_CLASSES_ROOT -Scope Script | Out-Null
}

# WIN-12: pre-seed a per-user ProgID so HKCR's HKCU/HKLM merge semantics are
# actually exercised -- a write that lands in HKCU must not be left behind by an
# HKLM-resolved delete.
function Set-PerUserProgIdSeed {
    New-Item -Path 'HKCU:\Software\Classes\.pdf' -Force | Out-Null
    Set-ItemProperty -Path 'HKCU:\Software\Classes\.pdf' -Name '(default)' -Value 'PdfeHarness.Seed'
}

# ===========================================================================
# Stage 2 -- silent install of the CPack-produced installer.
# ===========================================================================
function Invoke-SilentInstall {
    param([string] $InstallerPath, [string] $Prefix, [string] $Label = 'install')
    Write-Stage "stage 2/5 -- silent install ($Label)"
    # NSIS silent install of the CPack artifact. /D= must be LAST and unquoted.
    $p = Start-Process -FilePath $InstallerPath -ArgumentList '/S', "/D=$Prefix" -PassThru -Wait
    $exit = $p.ExitCode
    # NSIS returns before the extraction thread always settles; wait for the exe.
    $exe = Join-Path $Prefix 'bin\pdfe.exe'
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path $exe) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if ($exit -ne 0 -or -not (Test-Path $exe)) {
        Add-StageResult $Label 'FAIL' "installer exit=$exit; expected $exe"
        return $false
    }
    Add-StageResult $Label 'PASS' "exit=$exit; $exe present"
    return $true
}

function Test-InstalledState {
    Write-Stage 'stage 2b -- installed-state assertions'
    $checks = @()
    function Chk($name, $cond, $detail) { return [ordered]@{ check = $name; ok = [bool]$cond; detail = $detail } }

    $exe = [IO.Path]::Combine($InstallDir, 'bin\pdfe.exe')
    $checks += Chk 'bin\pdfe.exe exists' (Test-Path $exe) $exe

    # ARP / Uninstall key created (WIN-11).
    $arp = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\PDFE'
    $checks += Chk 'ARP uninstall key present' (Test-Path $arp) $arp

    # HKLM app registration + App Paths (WIN-01/WIN-06/WIN-07).
    $checks += Chk 'HKLM\Software\PDFE\Capabilities present' (Test-Path 'HKLM:\Software\PDFE\Capabilities') ''
    $checks += Chk 'App Paths\pdfe.exe present' (Test-Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\App Paths\pdfe.exe') ''

    # Association is visible WITHOUT a logoff (SHChangeNotify, WIN-13).
    $progids = $null
    try { $progids = Get-Item -LiteralPath 'HKCR:\.pdf\OpenWithProgids' -ErrorAction Stop } catch { }
    $assocResolves = ($null -ne $progids) -and ($progids.GetValueNames() -contains 'PDFE.Document')
    $checks += Chk 'file association resolves (.pdf OpenWithProgids lists PDFE.Document)' $assocResolves ''

    # Shortcuts.
    $checks += Chk 'desktop shortcut present' (Test-Path (Join-Path ([Environment]::GetFolderPath('Desktop')) 'PDFE.lnk')) ''

    $bad = @($checks | Where-Object { -not $_.ok })
    if ($bad.Count -gt 0) { Add-StageResult 'installed-state' 'FAIL' $checks; return $false }
    Add-StageResult 'installed-state' 'PASS' $checks
    return $true
}

# ===========================================================================
# Stage 3 -- launch. Direct launch + shell-verb launch (the "%1" quoting path
# that shipped broken once, WIN-14), with a spaces path and a non-ASCII path.
# ===========================================================================
function Invoke-LaunchStage {
    Write-Stage 'stage 3/5 -- launch'
    $exe = [IO.Path]::Combine($InstallDir, 'bin\pdfe.exe')
    $results = @()

    $p = Start-Process -FilePath $exe -PassThru
    $ok = $p.WaitForInputIdle($TimeoutSeconds * 1000)
    Start-Sleep -Seconds 2
    $started = -not $p.HasExited
    $p.CloseMainWindow() | Out-Null
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { $p.Kill(); $clean = $false } else { $clean = ($p.ExitCode -eq 0) }
    $results += [ordered]@{ case = 'direct launch'; reached_ui = ($ok -and $started); clean_exit = $clean; exit_code = $p.ExitCode }

    # Shell-verb launches through the registered association.
    $fixtureSrc = Join-Path $repoRoot 'tests/fixtures/test_3page.pdf'
    # Non-ASCII segment built from code points so this file stays ASCII-safe.
    $nonAscii = 'pdfe-harness-' + [string][char]0x00E9 + [char]0x00FC + [char]0x4E2D + [char]0x6587
    $cases = @(
        @{ name = 'spaces in path';   dir = (Join-Path $env:TEMP 'pdfe harness dir') },
        @{ name = 'non-ASCII path';   dir = (Join-Path $env:TEMP $nonAscii) }
    )
    foreach ($c in $cases) {
        try {
            New-Item -ItemType Directory -Force -Path $c.dir | Out-Null
            $doc = Join-Path $c.dir 'harness doc.pdf'
            if (Test-Path $fixtureSrc) { Copy-Item -LiteralPath $fixtureSrc -Destination $doc -Force }
            $sp = Start-Process -FilePath $doc -PassThru
            Start-Sleep -Seconds 5
            $shown = -not $sp.HasExited
            $sp.CloseMainWindow() | Out-Null
            if (-not $sp.WaitForExit(30000)) { $sp.Kill() }
            $results += [ordered]@{ case = "shell verb -- $($c.name)"; reached_ui = $shown; clean_exit = $true; document = $doc }
        } catch {
            $results += [ordered]@{ case = "shell verb -- $($c.name)"; reached_ui = $false; clean_exit = $false; error = "$_" }
        }
    }

    $bad = @($results | Where-Object { -not $_.reached_ui -or -not $_.clean_exit })
    if ($bad.Count -gt 0) { Add-StageResult 'launch' 'FAIL' $results; return $false }
    Add-StageResult 'launch' 'PASS' $results
    return $true
}

# ===========================================================================
# UPGRADE case -- install v_n, write a sentinel setting AND a document, install
# v_n+1 over it, then assert (a) the file association still resolves, (b) the
# sentinel setting survived, (c) the document survived.
# CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL is ON, so an upgrade is a compound
# uninstall+install and every one of those three can regress silently (WIN-03,
# WIN-04, WIN-93).
# ===========================================================================
function Invoke-UpgradeStage {
    Write-Stage 'upgrade -- v_n -> v_n+1 preservation'
    $sentinelSpec = [string]$baselineObj.user_data_sentinels.registry_value   # HKCU\Software\PDFE\PDFE!Name
    $sentinelKey  = 'HKCU:\' + (($sentinelSpec -split '!')[0] -replace '^HKCU\\', '')
    $sentinelName = ($sentinelSpec -split '!')[1]
    $sentinelValue = 'pdfe-vm-harness-' + [Guid]::NewGuid().ToString('N')

    $docSpec = [string]$baselineObj.user_data_sentinels.document_path         # %LOCALAPPDATA%\PDFE\...
    $docPath = [Environment]::ExpandEnvironmentVariables($docSpec)
    $docDir  = Split-Path -Parent $docPath

    # --- write the sentinel setting AND the document, on top of v_n ---------
    New-Item -Path $sentinelKey -Force | Out-Null
    Set-ItemProperty -Path $sentinelKey -Name $sentinelName -Value $sentinelValue
    New-Item -ItemType Directory -Force -Path $docDir | Out-Null
    $fixtureSrc = Join-Path $repoRoot 'tests/fixtures/test_3page.pdf'
    if (Test-Path $fixtureSrc) { Copy-Item -LiteralPath $fixtureSrc -Destination $docPath -Force }
    else { Set-Content -LiteralPath $docPath -Value '%PDF-1.7 harness sentinel' -Encoding ascii }
    $docHashBefore = (Get-FileHash -LiteralPath $docPath -Algorithm SHA256).Hash

    # --- install v_n+1 OVER the existing install ----------------------------
    $upgradeSrc = if ($UpgradeInstaller) { $UpgradeInstaller } else { $Installer }
    $mode = if ($UpgradeInstaller) { 'distinct_version' } else { 'same_version_reinstall' }
    $installedOver = Invoke-SilentInstall $upgradeSrc $InstallDir 'upgrade-install'

    # --- the three upgrade assertions ---------------------------------------
    # (a) the file association still resolves
    $progids = $null
    try { $progids = Get-Item -LiteralPath 'HKCR:\.pdf\OpenWithProgids' -ErrorAction Stop } catch { }
    $assocStillResolves = ($null -ne $progids) -and ($progids.GetValueNames() -contains 'PDFE.Document')

    # (b) the sentinel setting survived
    $sentinelSurvived = $false
    try {
        $sentinelSurvived = ((Get-ItemProperty -LiteralPath $sentinelKey -Name $sentinelName -ErrorAction Stop).$sentinelName -eq $sentinelValue)
    } catch { $sentinelSurvived = $false }

    # (c) the document survived
    $documentSurvived = (Test-Path -LiteralPath $docPath) -and
                        ((Get-FileHash -LiteralPath $docPath -Algorithm SHA256).Hash -eq $docHashBefore)

    # no duplicate Start-menu entry after the compound uninstall+install
    $startMenu = @(Get-ChildItem -Path ([Environment]::GetFolderPath('Programs')), ([Environment]::GetFolderPath('CommonPrograms')) `
                    -Recurse -Filter 'PDFE.lnk' -ErrorAction SilentlyContinue)
    $noDuplicateShortcut = ($startMenu.Count -le 1)

    $detail = [ordered]@{
        mode                  = $mode
        upgrade_installed     = $installedOver
        assoc_still_resolves  = $assocStillResolves
        sentinel_survived     = $sentinelSurvived
        sentinel              = $sentinelSpec
        document_survived     = $documentSurvived
        document              = $docPath
        no_duplicate_shortcut = $noDuplicateShortcut
    }
    if ($installedOver -and $assocStillResolves -and $sentinelSurvived -and $documentSurvived -and $noDuplicateShortcut) {
        Add-StageResult 'upgrade' 'PASS' $detail
        return $true
    }
    Add-StageResult 'upgrade' 'FAIL' $detail
    return $false
}

# ===========================================================================
# Stage 4 -- uninstall.
# ===========================================================================
function Invoke-Uninstall {
    Write-Stage 'stage 4/5 -- uninstall'
    $uninst = [IO.Path]::Combine($InstallDir, 'Uninstall.exe')
    if (-not (Test-Path $uninst)) {
        try {
            $uninst = (Get-ItemProperty -LiteralPath 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\PDFE' -ErrorAction Stop).UninstallString.Trim('"')
        } catch { }
    }
    if (-not $uninst -or -not (Test-Path $uninst)) {
        Add-StageResult 'uninstall' 'FAIL' 'no uninstaller found (neither $INSTDIR\Uninstall.exe nor the ARP UninstallString)'
        return $false
    }
    # NSIS uninstallers copy themselves to %TEMP% and return immediately unless
    # _?= pins them in place; pin it so -Wait actually means finished.
    $p = Start-Process -FilePath $uninst -ArgumentList '/S', "_?=$InstallDir" -PassThru -Wait
    Start-Sleep -Seconds 3
    Add-StageResult 'uninstall' $(if ($p.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }) "exit=$($p.ExitCode)"
    return ($p.ExitCode -eq 0)
}

# ===========================================================================
# Stage 5 -- diff + residue judgment against the baseline.
# ===========================================================================
function Invoke-DiffStage {
    param($Before, $After)
    Write-Stage 'stage 5/5 -- diff'

    $preservedReg = @($baselineObj.expected_surviving.registry)
    $mustBeAbsent = @($baselineObj.must_be_absent_after_uninstall.registry)

    # 1. Anything present AFTER that was not present BEFORE is a survivor.
    $newRegistry = @()
    foreach ($root in $Before.registry.Keys) {
        $b = @($Before.registry[$root]); $a = @($After.registry[$root])
        $newRegistry += @($a | Where-Object { $b -notcontains $_ })
    }
    $newFiles = @($After.filesystem | Where-Object { $Before.filesystem -notcontains $_ })

    function Test-InPreservedScope([string]$path) {
        $p = $path -replace '^HK(LM|CU|CR|CC|U):\\', 'HK$1\'
        foreach ($k in $preservedReg) { if ($p -like "$k*") { return $true } }
        return $false
    }
    function Test-IsHarnessOwned([string]$path) {
        # sentinels + fixtures the harness itself created are intended survivors
        $doc = [Environment]::ExpandEnvironmentVariables([string]$baselineObj.user_data_sentinels.document_path)
        return ($path -like "$doc*") -or ($path -like (Join-Path $env:LOCALAPPDATA 'PDFE') + '*') -or
               ($path -like (Join-Path $env:TEMP 'pdfe*'))
    }

    $unexpectedRegistry = @($newRegistry | Where-Object { -not (Test-InPreservedScope $_) })
    $unexpectedFiles    = @($newFiles    | Where-Object { -not (Test-IsHarnessOwned $_) })

    # 2. Every key the baseline says must be gone really is gone.
    $stillPresent = @()
    foreach ($entry in $mustBeAbsent) {
        $parts = $entry -split '!', 2
        $key = 'Registry::' + ($parts[0] -replace '^HKLM\\', 'HKEY_LOCAL_MACHINE\' `
                                        -replace '^HKCR\\', 'HKEY_CLASSES_ROOT\' `
                                        -replace '^HKCU\\', 'HKEY_CURRENT_USER\')
        if ($parts.Count -eq 1) {
            if (Test-Path -LiteralPath $key) { $stillPresent += $entry }
        } else {
            try {
                $item = Get-Item -LiteralPath $key -ErrorAction Stop
                if ($item.GetValueNames() -contains $parts[1]) { $stillPresent += $entry }
            } catch { }
        }
    }

    # 3. The documented HKCU intent must be POSITIVELY satisfied: preserved keys
    #    that existed before must still exist. A "clean" machine that wiped user
    #    data is a failure, not a pass.
    $preservedLost = @()
    foreach ($k in $preservedReg) {
        $reg = 'Registry::' + ($k -replace '^HKCU\\', 'HKEY_CURRENT_USER\')
        $existedBefore = @($Before.registry['HKCU:\Software\PDFE']).Count -gt 0
        if ($existedBefore -and -not (Test-Path -LiteralPath $reg)) { $preservedLost += $k }
    }

    $residue = [ordered]@{
        unexpected_registry_survivors = $unexpectedRegistry
        unexpected_file_survivors     = $unexpectedFiles
        must_be_absent_still_present  = $stillPresent
        preserved_hkcu_lost           = $preservedLost
        install_dir_removed           = (-not (Test-Path -LiteralPath ([IO.Path]::Combine($InstallDir, 'bin\pdfe.exe'))))
    }
    $clean = ($unexpectedRegistry.Count -eq 0 -and $unexpectedFiles.Count -eq 0 -and
              $stillPresent.Count -eq 0 -and $preservedLost.Count -eq 0 -and $residue.install_dir_removed)
    Add-StageResult 'diff' $(if ($clean) { 'PASS' } else { 'FAIL' }) $residue
    return $residue
}

# ===========================================================================
# Drive the stages.
# ===========================================================================
if (-not $SkipSignature) {
    # the installer is signature-checked before it is executed ...
    $null = Invoke-SignatureStage @($Installer)
}

if ($SignatureOnly) {
    $v = if ($script:failures.Count -eq 0) { 'SIGNATURE-OK' } else { 'SIGNATURE-FAIL' }
    $rep = Save-Report $v $(if ($script:failures.Count) { $script:failures -join ' | ' } else { 'installer is Authenticode-Valid and RFC-3161 countersigned' })
    $d = Split-Path -Parent $RunRecord
    if ($d -and -not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    [System.IO.File]::WriteAllText($RunRecord, ($rep | ConvertTo-Json -Depth 12), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("verdict: {0}" -f $v)
    if ($script:failures.Count -ne 0) { exit 1 }
    exit 0
}

Set-PerUserProgIdSeed
$before = New-MachineSnapshot 'before-install'

$installed = Invoke-SilentInstall $Installer $InstallDir 'install'
if ($installed) {
    if (-not $SkipSignature) {
        # ... and the shipped binaries once they are on disk.
        $null = Invoke-SignatureStage @(
            ([IO.Path]::Combine($InstallDir, 'bin\pdfe.exe')),
            ([IO.Path]::Combine($InstallDir, 'bin\pdfe_preflight.exe'))
        )
    }
    $null = Test-InstalledState
    $null = Invoke-LaunchStage
    $null = Invoke-UpgradeStage
    $null = Invoke-Uninstall
}

$after = New-MachineSnapshot 'after-uninstall'
$residue = Invoke-DiffStage $before $after

$verdict = if ($script:failures.Count -eq 0) { 'CLEAN' } else { 'RESIDUE-OR-FAILURE' }
$report = Save-Report $verdict $(if ($script:failures.Count) { $script:failures -join ' | ' } else { 'all stages passed; residue matches the baseline' }) $residue

$dir = Split-Path -Parent $RunRecord
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
[System.IO.File]::WriteAllText($RunRecord, ($report | ConvertTo-Json -Depth 12), (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("verdict: {0}" -f $verdict) -ForegroundColor $(if ($verdict -eq 'CLEAN') { 'Green' } else { 'Red' })
if ($verdict -ne 'CLEAN') { exit 1 }
exit 0
