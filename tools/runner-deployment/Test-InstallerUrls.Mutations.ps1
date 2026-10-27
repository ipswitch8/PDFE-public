# Mutation-test Test-InstallerUrls.ps1: prove it REDDENS on the exact defects
# it claims to catch. Operates on a COPY in TEMP; the real tree is untouched.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# $PSScriptRoot, not the caller's cwd: this must work when invoked from
# anywhere, including a deployed copy under C:\pdfe-runner-deploy.
$src = $PSScriptRoot
$work = Join-Path $env:TEMP 'pdfe-mutate-urlcheck'
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $work -Force | Out-Null
Copy-Item (Join-Path $src 'Test-InstallerUrls.ps1') $work
$jsonPath = Join-Path $work 'runner-requirements.json'

$full = Get-Content (Join-Path $src 'runner-requirements.json') -Raw | ConvertFrom-Json

function Write-Spec {
    param($Reqs)
    [pscustomobject]@{ schema = $full.schema; requirements = @($Reqs) } |
        ConvertTo-Json -Depth 8 | Out-File $jsonPath -Encoding utf8
}

function Run-Check {
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $work 'Test-InstallerUrls.ps1') 2>&1 | Out-String
}

# Two small installers only, to keep each run quick.
$occ = $full.requirements | Where-Object { $_.id -eq 'opencppcoverage' }
$wad = $full.requirements | Where-Object { $_.id -eq 'winappdriver' }

$results = @()

# --- M0: control. Unmutated -> must PASS (exit 0). --------------------------
Write-Spec @($occ, $wad)
$out = Run-Check; $rc = $LASTEXITCODE
$results += [pscustomobject]@{ id='M0-control'; expect='exit 0'; rc=$rc; verdict=$(if($rc -eq 0){'OK'}else{'BROKEN'}) }

# --- M1: the ACTUAL historical bug -- Inno installer given NSIS /S. ---------
$m1 = $occ | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$m1.install.silent_args = '/S'
Write-Spec @($m1, $wad)
$out1 = Run-Check; $rc = $LASTEXITCODE
$killed = ($rc -ne 0) -and ($out1 -match 'ARGS-MISMATCH')
$results += [pscustomobject]@{ id='M1-inno-given-/S'; expect='ARGS-MISMATCH, exit 1'; rc=$rc; verdict=$(if($killed){'KILLED'}else{'SURVIVED'}) }

# --- M2: URL re-pointed at a differently-packaged installer. ---------------
#     (opencppcoverage's URL swapped for an MSI) -> args no longer match.
$m2 = $occ | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$m2.install.url = $wad.install.url
Write-Spec @($m2)
$out2 = Run-Check; $rc = $LASTEXITCODE
$killed = ($rc -ne 0) -and ($out2 -match 'ARGS-MISMATCH')
$results += [pscustomobject]@{ id='M2-url-repointed'; expect='ARGS-MISMATCH, exit 1'; rc=$rc; verdict=$(if($killed){'KILLED'}else{'SURVIVED'}) }

# --- M3: dead URL -> must be DOWNLOAD-FAIL, not a silent pass. -------------
$m3 = $wad | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$m3.install.url = 'https://github.com/microsoft/WinAppDriver/releases/download/v9.9.9/nope_9.9.9.msi'
Write-Spec @($m3)
$out3 = Run-Check; $rc = $LASTEXITCODE
$killed = ($rc -ne 0) -and ($out3 -match 'DOWNLOAD-FAIL')
$results += [pscustomobject]@{ id='M3-dead-url'; expect='DOWNLOAD-FAIL, exit 1'; rc=$rc; verdict=$(if($killed){'KILLED'}else{'SURVIVED'}) }

''
$results | ForEach-Object { '{0,-20} {1,-24} rc={2,-3} {3}' -f $_.id, $_.expect, $_.rc, $_.verdict }
''
$bad = @($results | Where-Object { $_.verdict -in 'SURVIVED','BROKEN' })
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($bad) { "MUTATION RESULT: $($bad.Count) not killed"; exit 1 }
"MUTATION RESULT: control OK, $(($results | Where-Object {$_.verdict -eq 'KILLED'}).Count)/3 mutations killed"
exit 0
