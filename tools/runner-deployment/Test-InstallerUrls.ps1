<#
.SYNOPSIS
    Verify every pinned installer URL in runner-requirements.json still
    downloads, and still IS the kind of installer its silent_args assume.

.DESCRIPTION
    Run this after bumping any pinned version in runner-requirements.json.

    WHY THIS EXISTS, concretely. The installer used to infer silent arguments
    from the file EXTENSION -- NSIS `/S` for anything ending .exe. That was
    wrong for two of the six pinned installers, and wrong in the worst possible
    way: Inno Setup IGNORES switches it does not recognise and falls back to its
    INTERACTIVE wizard. Under `Start-Process -Wait` on a headless runner that
    does not fail, it HANGS -- until the 720-minute job cap, reported as
    `cancelled`, alerting nobody. A silent-install bug is invisible precisely
    where it is most expensive.

    So this checks the thing that actually varies: it downloads each installer
    and identifies its packaging family from the bytes and version resource,
    then asserts the recorded silent_args are the ones that family obeys. A URL
    silently re-pointed at a differently-packaged installer fails HERE, at
    deploy time, instead of at minute 600 of a CI job.

    An UNDETECTED family is a FAILURE, not a pass. A checker that shrugs at the
    case it cannot classify would report green on exactly the situation it was
    written to catch.

.PARAMETER KeepDownloads
    Leave the downloaded installers in place (they are deleted by default).
    Useful if you want to install one by hand afterwards.

.EXAMPLE
    .\Test-InstallerUrls.ps1
#>
[CmdletBinding()]
param([switch]$KeepDownloads)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$reqPath = Join-Path $PSScriptRoot 'runner-requirements.json'
$spec = Get-Content $reqPath -Raw | ConvertFrom-Json

$dl = Join-Path $env:TEMP 'pdfe-installer-url-check'
New-Item -ItemType Directory -Path $dl -Force | Out-Null

function Get-InstallerFamily {
    param([string]$Path)

    # MSI is an OLE compound document; check that before reading anything else.
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $head = New-Object byte[] 8
        [void]$fs.Read($head, 0, 8)
        if ($head[0] -eq 0xD0 -and $head[1] -eq 0xCF -and $head[2] -eq 0x11 -and $head[3] -eq 0xE0) {
            return 'msi'
        }
        $len = [int][Math]::Min($fs.Length, 6MB)
        $fs.Position = 0
        $buf = New-Object byte[] $len
        [void]$fs.Read($buf, 0, $len)
        $txt = [System.Text.Encoding]::ASCII.GetString($buf)
    } finally { $fs.Dispose() }

    # The VS bootstrapper carries no packager signature string, so identify it
    # by its version resource instead of leaving it unclassified.
    $vi = (Get-Item $Path).VersionInfo
    if ($vi.FileDescription -match 'Visual Studio Installer') { return 'vsboot' }

    if ($txt -match 'Inno Setup|JR\.Inno')      { return 'inno' }
    if ($txt -match 'Nullsoft\.NSIS|NullsoftInst') { return 'nsis' }
    # WiX "Burn" bundle -- python.org's installers are these. The .wixburn PE
    # section name is the reliable marker.
    if ($txt -match '\.wixburn')                { return 'burn' }
    return $null
}

# What each family REQUIRES of silent_args, as a regex over the args string.
$expect = @{
    'msi'    = @{ pattern = '^$';                  desc = "empty -- Install-FromUrl drives msiexec /quiet itself" }
    'inno'   = @{ pattern = '/VERYSILENT|/SILENT'; desc = "/VERYSILENT (Inno IGNORES /S and goes interactive)" }
    'nsis'   = @{ pattern = '/S(\s|$)';            desc = "/S" }
    'burn'   = @{ pattern = '/quiet|/passive';     desc = "/quiet (WiX Burn bundle)" }
    'vsboot' = @{ pattern = '--quiet(?=.*--wait)'; desc = "--quiet AND --wait (else the install races the next step)" }
}

$rows = @()
$fail = 0
foreach ($req in $spec.requirements) {
    if (-not ($req.PSObject.Properties.Name -contains 'install')) { continue }
    $i = $req.install
    if ($i.kind -ne 'download') { continue }

    $out = Join-Path $dl (Split-Path $i.url -Leaf)
    $status = 'ok'; $detail = ''
    try {
        # RETRY, deliberately. This probe reported vs2022 as DOWNLOAD-FAIL on a
        # single TCP connect timeout to aka.ms -- while the URL was perfectly
        # good: it 301s to download.visualstudio.microsoft.com and a ranged GET
        # returns 206. aka.ms is a redirector and is intermittently slow to
        # answer from this network.
        #
        # A one-shot network probe that reports a hard FAIL is a FLAKY GATE, and
        # a flaky gate gets ignored -- which is worse than not having it, because
        # the one time it is right nobody believes it. Three attempts with
        # backoff and a generous timeout, and the failure text now says how many
        # attempts were made so a real outage is distinguishable from a blip.
        $attempts = 3
        $lastErr = $null
        for ($a = 1; $a -le $attempts; $a++) {
            try {
                Invoke-WebRequest -Uri $i.url -OutFile $out -UseBasicParsing `
                    -TimeoutSec 180 -MaximumRedirection 10
                $lastErr = $null
                break
            } catch {
                $lastErr = $_
                if ($a -lt $attempts) {
                    Write-Host ("  {0}: attempt {1}/{2} failed ({3}); retrying" -f `
                        $req.id, $a, $attempts, $_.Exception.Message) -ForegroundColor DarkGray
                    Start-Sleep -Seconds (5 * $a)
                }
            }
        }
        if ($lastErr) {
            throw ("after $attempts attempt(s): " + $lastErr.Exception.Message)
        }
        $size = (Get-Item $out).Length
        if ($size -eq 0) { throw 'downloaded 0 bytes' }

        $fam = Get-InstallerFamily -Path $out
        $iargs = [string]$i.silent_args

        if (-not $fam) {
            # Deliberately a failure. See the header.
            $status = 'FAMILY-UNKNOWN'
            $detail = "could not classify the packaging, so silent_args '$iargs' are UNVERIFIED -- classify it by hand and teach this script"
            $fail++
        } elseif ($iargs -notmatch $expect[$fam].pattern) {
            $status = 'ARGS-MISMATCH'
            $detail = "family=$fam wants $($expect[$fam].desc); silent_args='$iargs'"
            $fail++
        } else {
            $detail = "family={0}  {1:N1} MB  args='{2}'" -f $fam, ($size / 1MB), $iargs
        }
    } catch {
        $status = 'DOWNLOAD-FAIL'
        $detail = $_.Exception.Message
        $fail++
    }
    $rows += [pscustomobject]@{ id = $req.id; status = $status; detail = $detail }
}

foreach ($r in $rows) {
    $colour = if ($r.status -eq 'ok') { 'Green' } else { 'Red' }
    Write-Host ('{0,-16} {1,-15} {2}' -f $r.id, $r.status, $r.detail) -ForegroundColor $colour
}
Write-Host ''

if (-not $KeepDownloads) { Remove-Item $dl -Recurse -Force -ErrorAction SilentlyContinue }
else { Write-Host "downloads kept in $dl" }

if ($rows.Count -eq 0) {
    Write-Host "No kind:'download' requirements found -- that is itself suspicious." -ForegroundColor Red
    exit 1
}
if ($fail -gt 0) {
    Write-Host "RESULT: $fail of $($rows.Count) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "RESULT: all $($rows.Count) pinned installers downloaded and match their silent_args" -ForegroundColor Green
exit 0
