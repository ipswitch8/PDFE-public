<#
.SYNOPSIS
    Extract the NSIS installer payload and assert no shipped binary carries the
    build account name. GitHub #199 / #191.

.DESCRIPTION
    WHY THIS IS A SEPARATE STEP FROM THE DLL SMOKE TEST

    scripts/verify-installer-deps.ps1 already extracts the payload to
    %TEMP%\pdfe_depcheck, and this script could have reused that directory. It
    deliberately does not. That script deletes and recreates the directory at
    its START and never removes it at the end, so the leftovers are reusable
    only as long as that remains true -- and a guard whose population depends
    on another script's incidental leftover state reports "clean" the moment the
    other script starts tidying up after itself. An empty directory satisfies
    "no occurrences" forever. So this extracts its own copy.

    WHY THE PAYLOAD AND NOT THE BUILD DIRECTORY

    Measured 2026-09-30: build/vs2022/bin/Release contains eight binaries
    carrying the account name, but three of them -- pdfe_tests.exe, gtest.dll,
    Qt6Test.dll -- are test binaries the installer never ships. CMakeLists.txt
    confirms this independently: the explicit _required_dlls list does not
    include gtest or Qt6Test, and the comment there records that the list exists
    precisely to avoid bundling Qt6Test. pdfe_tests.exe has 850 occurrences, 531
    of them __FILE__ paths from this project's own test sources -- correct and
    expected for a binary nobody distributes. A guard scoped to the build
    directory would therefore fail permanently on something that is not a
    defect, which is the shape that gets a guard switched off.

.PARAMETER Installer
    Path to the installer. Defaults to the newest PDFE-*-win64.exe under
    build/vs2022-package/installer, matching verify-installer-deps.ps1.

.PARAMETER Account
    Account name to search for. Defaults to $env:USERNAME.

.PARAMETER SevenZip
    Path to 7z.exe. Auto-detected if omitted.
#>
[CmdletBinding()]
param(
    [string] $Installer,
    [string] $Account = $env:USERNAME,
    [string] $SevenZip
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Fail($msg) { Write-Host "FAIL: $msg" -ForegroundColor Red; exit 1 }

# A leak scanner that prints the string it hunts becomes the next producer.
# The Python half is careful about this -- it redacts every sample and its
# self-test asserts redact() does not leave the name in its own output -- and
# this wrapper was NOT, which made the pair inconsistent in the worst place: the
# two lines it prints first. Both interpolate absolute paths, and on Windows
# $work is under %TEMP%, i.e. C:\Users\<account>\AppData\Local\Temp. These are
# CI step logs, and CI logs get pasted into issues; that is the same route by
# which a committed gtest XML once carried the account name into the repository
# once per test.
function Redact([string]$s) {
    if (-not $Account) { return $s }
    return [regex]::Replace($s, [regex]::Escape($Account), '<account>',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
}

if (-not $Account) { Fail "no account name to search for (pass -Account)" }

if (-not $Installer) {
    $Installer = Get-ChildItem (Join-Path $repoRoot 'build/vs2022-package/installer') `
            -Filter 'PDFE-*-win64.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Installer -or -not (Test-Path $Installer)) {
    Fail "installer not found (pass -Installer <path>)"
}

if (-not $SevenZip) {
    $SevenZip = @("C:\Program Files\7-Zip\7z.exe", "C:\Program Files (x86)\7-Zip\7z.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $SevenZip) {
        $c = Get-Command 7z -ErrorAction SilentlyContinue
        if ($c) { $SevenZip = $c.Source }
    }
}
if (-not $SevenZip) { Fail "7-Zip (7z.exe) not found; needed to extract the NSIS payload" }

Write-Host "Installer : $(Redact $Installer)"

$work = Join-Path ([System.IO.Path]::GetTempPath()) "pdfe_acctscan"
if (Test-Path $work) { & cmd /c "rmdir /s /q `"$work`"" 2>$null }
New-Item -ItemType Directory -Path $work -Force | Out-Null

& $SevenZip x -aoa -o"$work" "$Installer" > $null 2>&1
if ($LASTEXITCODE -ne 0) { Fail "7-Zip failed to extract the installer ($LASTEXITCODE)" }

# The payload must look like the real thing before its contents are judged
# clean. Without this, a failed or partial extraction yields a near-empty tree
# that the scanner passes trivially.
$exe = Join-Path $work 'bin\pdfe.exe'
if (-not (Test-Path $exe)) { Fail "bin\pdfe.exe not found in the extracted payload" }

$binCount = (Get-ChildItem (Join-Path $work 'bin') -Include '*.dll', '*.exe' -Recurse `
                -ErrorAction SilentlyContinue).Count
Write-Host "Payload   : $(Redact $work)  ($binCount binaries)"
if ($binCount -lt 20) {
    Fail ("only $binCount binaries in the payload -- expected the full bundle " +
          "(40+). Refusing to report a clean scan over a population this small.")
}

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { Fail "python not found; needed to run the scanner" }

& $py.Source (Join-Path $repoRoot 'tools/gate/check_package_account_name.py') `
    $work --account $Account `
    --allowlist (Join-Path $repoRoot 'tools/gate/package_leak_allowlist.json')
$rc = $LASTEXITCODE

try { & cmd /c "rmdir /s /q `"$work`"" 2>$null } catch { }

if ($rc -ne 0) {
    Write-Host ""
    Write-Host ("The shipped installer payload carries the build account name. " +
                "See GitHub #199 / #191, and scripts/Set-NeutralBuildRoot.ps1 " +
                "for the mechanism that keeps it out.") -ForegroundColor Red
    exit 1
}

Write-Host "Installer payload carries no build account name." -ForegroundColor Green
exit 0
