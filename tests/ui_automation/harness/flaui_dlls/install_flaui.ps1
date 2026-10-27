# Bootstrap FlaUI DLLs into this directory.
#
# FlaUI is a NuGet-distributed .NET library (~31 MB total with deps).
# We don't commit the binaries; this script fetches them on demand.
# The flaui_bridge.py harness searches this directory automatically.
#
# Usage (from repo root):
#   pwsh tests/ui_automation/harness/flaui_dlls/install_flaui.ps1
#   pwsh tests/ui_automation/harness/flaui_dlls/install_flaui.ps1 -WhatIf   # dry run
#
# Or set PDFE_FLAUI_DLL_DIR to point at any directory containing the
# DLLs and the bridge will pick that up instead.
#
# Requires: pythonnet (pip install pythonnet) - the bridge will skip
# gracefully without it.
#
# ---------------------------------------------------------------------------
# SUPPLY CHAIN (GitHub #68)
#
# This script used to do three unsafe things, each of which ends in code this
# repo did not choose running on the machine that builds and signs the product:
#
#   1. It downloaded nuget.exe from the `/latest/` URL. "latest" is not a
#      version -- it is whatever is served at the moment of the request. The
#      binary was then EXECUTED. Nothing checked what it was; nothing could,
#      because nothing knew what it was supposed to be.
#   2. It installed FlaUI.Core, FlaUI.UIA3 and their transitive dependencies
#      with no -Version, so the package set floated with whatever nuget.org
#      resolved that day.
#   3. It verified nothing after the fact.
#
# Now: nuget.exe is pinned to a specific version URL and its SHA256 is checked
# BEFORE it is executed, every package is version-pinned, and every downloaded
# .nupkg is checked against the SHA256 of the bytes nuget.org publishes for
# that exact version (verified against the flat-container endpoint, not merely
# against whatever happened to be on this disk).
#
# To move a version: change it here, run the script, and take the "expected"
# hash from the mismatch error -- but only after satisfying yourself the new
# bytes are the publisher's.
# ---------------------------------------------------------------------------

[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = "Stop"
$flaui_dir = $PSScriptRoot
Set-Location $flaui_dir

# nuget.exe: pinned version, not "latest".
$NUGET_VERSION = "v6.13.2"
$NUGET_URL     = "https://dist.nuget.org/win-x86-commandline/$NUGET_VERSION/nuget.exe"
$NUGET_SHA256  = "506085572a662f03bec1a5ffe43f81a240ff99b3765b8dbc0e22c33532775227"

# Package set: name -> version + SHA256 of the published .nupkg.
# Interop.UIAutomationClient arrives as a transitive dependency of FlaUI.UIA3;
# it is listed so it is verified rather than merely trusted.
$PACKAGES = @(
    @{ Name = "FlaUI.Core";                 Version = "5.0.0";       Sha256 = "191cc65ea82036b77f1872e6d4ebf743d3d120895bba2fa5248d126cd6f568a7"; Direct = $true  },
    @{ Name = "FlaUI.UIA3";                 Version = "5.0.0";       Sha256 = "d5d2e083539a04bf6c9053781dfd47332854a76faeb8e9c3c68cc82109f711f2"; Direct = $true  },
    @{ Name = "System.Drawing.Common";      Version = "8.0.0";       Sha256 = "c4f3672944dc662aa74ed46023661accca1981acbfa6bc0aac98db6546d599c8"; Direct = $true  },
    @{ Name = "Interop.UIAutomationClient"; Version = "10.19041.0";  Sha256 = "0d2ed17db2cb13a262f0580ab992db87577651c25a9899053df4114b4d9ff3b1"; Direct = $false }
)

function Test-Sha256 {
    param([string]$Path, [string]$Expected, [string]$What)
    $actual = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $Expected.ToLower()) {
        throw "$What SHA256 mismatch.`n  expected: $Expected`n  actual:   $actual`n  path:     $Path"
    }
    Write-Host "  SHA256 OK: $What"
}

# ---- nuget.exe ------------------------------------------------------------
$nuget_exe = Join-Path $flaui_dir "nuget.exe"

# An EXISTING nuget.exe is checked too. Skipping the check when the file is
# already there would mean the very first unverified download is trusted
# forever after -- which is how the old `/latest/` binary survived on this
# machine at an unknown version.
$need_download = $true
if (Test-Path $nuget_exe) {
    $h = (Get-FileHash $nuget_exe -Algorithm SHA256).Hash.ToLower()
    if ($h -eq $NUGET_SHA256) {
        Write-Host "nuget.exe present and pinned to $NUGET_VERSION (SHA256 OK)"
        $need_download = $false
    } else {
        Write-Host "nuget.exe present but is NOT the pinned $NUGET_VERSION (sha $h); replacing."
    }
}

if ($need_download) {
    if ($PSCmdlet.ShouldProcess($nuget_exe, "download nuget.exe $NUGET_VERSION and verify SHA256")) {
        Write-Host "Downloading nuget.exe $NUGET_VERSION ..."
        $tmp = "$nuget_exe.download"
        Invoke-WebRequest -Uri $NUGET_URL -OutFile $tmp -UseBasicParsing
        # Verified BEFORE it is moved into place, so a failed check never leaves
        # an unverified executable at the path this script later invokes.
        Test-Sha256 -Path $tmp -Expected $NUGET_SHA256 -What "nuget.exe $NUGET_VERSION"
        Move-Item $tmp $nuget_exe -Force
    } else {
        Write-Host "WhatIf: would download and verify nuget.exe $NUGET_VERSION from $NUGET_URL"
    }
}

# ---- packages -------------------------------------------------------------
foreach ($pkg in $PACKAGES) {
    if (-not $pkg.Direct) { continue }
    $desc = "$($pkg.Name) $($pkg.Version)"
    if ($PSCmdlet.ShouldProcess($desc, "nuget install (version-pinned)")) {
        & $nuget_exe install $pkg.Name -OutputDirectory $flaui_dir -ExcludeVersion -Version $pkg.Version
        if ($LASTEXITCODE -ne 0) {
            throw "nuget install $desc failed with exit code $LASTEXITCODE"
        }
    } else {
        Write-Host "WhatIf: would run nuget install $desc"
    }
}

# Verify every package we have a pin for, direct or transitive.
Write-Host ""
Write-Host "Verifying package integrity:"
foreach ($pkg in $PACKAGES) {
    $nupkg = Join-Path $flaui_dir "$($pkg.Name)\$($pkg.Name).nupkg"
    if (-not (Test-Path $nupkg)) {
        if ($WhatIfPreference) {
            Write-Host "  WhatIf: would verify $($pkg.Name) $($pkg.Version)"
            continue
        }
        throw "expected package not found after install: $nupkg"
    }
    Test-Sha256 -Path $nupkg -Expected $pkg.Sha256 -What "$($pkg.Name) $($pkg.Version)"
}

# ---- stage the DLLs -------------------------------------------------------
# Stage the right framework's DLLs to the top-level so pythonnet's
# clr loader can find them when this directory is added to sys.path.
if ($PSCmdlet.ShouldProcess($flaui_dir, "stage framework DLLs to the top level")) {
    $frameworks = @("net8.0-windows7.0", "net6.0-windows7.0", "net48")
    foreach ($fw in $frameworks) {
        if (Test-Path "$flaui_dir\FlaUI.Core\lib\$fw\FlaUI.Core.dll") {
            Copy-Item "$flaui_dir\FlaUI.Core\lib\$fw\*.dll" -Destination $flaui_dir -Force
            Copy-Item "$flaui_dir\FlaUI.UIA3\lib\$fw\*.dll" -Destination $flaui_dir -Force
            break
        }
    }
    $interop_fws = @("netcoreapp3.0", "net48", "net45", "net40", "net35")
    foreach ($ifw in $interop_fws) {
        $interop = "$flaui_dir\Interop.UIAutomationClient\lib\$ifw\Interop.UIAutomationClient.dll"
        if (Test-Path $interop) {
            Copy-Item $interop -Destination $flaui_dir -Force
            break
        }
    }
    if (Test-Path "$flaui_dir\System.Drawing.Common\lib\net8.0\System.Drawing.Common.dll") {
        Copy-Item "$flaui_dir\System.Drawing.Common\lib\net8.0\System.Drawing.Common.dll" -Destination $flaui_dir -Force
    }

    Write-Host ""
    Write-Host "Top-level DLLs:"
    Get-ChildItem -Path $flaui_dir -Filter "*.dll" -Depth 0 | ForEach-Object {
        Write-Host "  $($_.Name) ($($_.Length) bytes)"
    }
} else {
    Write-Host "WhatIf: would stage framework DLLs to $flaui_dir"
}

Write-Host ""
Write-Host "Verify with: python -c `"from tests.ui_automation.harness import flaui_bridge; print(flaui_bridge.is_available())`""
