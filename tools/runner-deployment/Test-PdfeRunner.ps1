<#
.SYNOPSIS
    Verify that this machine can actually serve as a pdfe-windows self-hosted runner.

.DESCRIPTION
    Reads runner-requirements.json and checks every entry INDEPENDENTLY of the
    installer. This is the point of the script: the installer reports what it
    tried to do, which is not the same as what is true. A runner that reports
    "OpenCppCoverage installed" and then fails the coverage step 40 minutes into
    a job is the exact failure this package exists to prevent.

    DESIGN RULES, all of them learned the hard way on this project:

      * ABSENCE IS A FAILURE, NEVER A SKIP. A check that cannot determine an
        answer reports FAIL, not PASS and not SKIP. "Could not verify" reported
        as green is how a broken runner looks healthy.
      * NOTHING IS REPORTED AS CHECKED THAT WAS NOT CHECKED. Every line of
        output corresponds to a probe that actually ran.
      * THE EXIT CODE IS LOAD-BEARING. 0 only when every non-warn requirement
        passed. Warn-severity items (currently: gh) can fail without failing
        the run, and are reported as WARN so they are still visible.

.PARAMETER RepoRoot
    Path to a PDFE checkout. Needed for the pinned-requirements check.
    Optional: without it, that one check reports FAIL with a clear reason
    rather than silently passing.

.PARAMETER RunnerDir
    Path to the GitHub Actions runner directory (contains run.cmd).

.PARAMETER Json
    Emit machine-readable results to the given path in addition to the console
    table, so a workflow step can consume this rather than re-implementing it.

.PARAMETER RunnerProfile
    Which role this machine plays: 'build' or 'consume'. Only that profile's
    requirements are checked, and the free-space floor is the profile's own --
    checking a consume-only box against the build profile's 120 GB would FAIL a
    machine that is entirely correct for its role, and a floor that reports a
    false red is a floor nobody keeps.

    NAMED -RunnerProfile AND NOT -Profile ON PURPOSE: $Profile is a PowerShell
    AUTOMATIC variable (the path to the current profile script), and a parameter
    of that name shadows it for the whole script scope.

.EXAMPLE
    .\Test-PdfeRunner.ps1 -RepoRoot C:\actions-runner\_work\PDFE\PDFE -RunnerDir C:\actions-runner
#>
[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$RunnerDir,
    [string]$Json,
    [ValidateSet('build', 'consume')]
    [string]$RunnerProfile = 'build'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$reqPath = Join-Path $PSScriptRoot 'runner-requirements.json'
if (-not (Test-Path $reqPath)) {
    Write-Error "runner-requirements.json not found next to this script ($reqPath). This script cannot verify anything without it, and reporting success would be a lie."
    exit 2
}
$spec = Get-Content $reqPath -Raw | ConvertFrom-Json

# --- profile selection ------------------------------------------------------
# Refuses rather than falling back. A verifier that quietly checks the WRONG
# requirement set is worse than one that will not start: it produces a PASS
# table the operator has every reason to believe.
if (-not ($spec.PSObject.Properties.Name -contains 'profiles')) {
    Write-Error "runner-requirements.json has no 'profiles' block, so this script cannot tell which requirements apply to a '$RunnerProfile' machine. Refusing to guess."
    exit 2
}
if (-not ($spec.profiles.PSObject.Properties.Name -contains $RunnerProfile)) {
    Write-Error "unknown profile '$RunnerProfile'. Available: $(($spec.profiles.PSObject.Properties.Name) -join ', ')"
    exit 2
}
$profileSpec = $spec.profiles.$RunnerProfile
$selectedReqs = @($spec.requirements | Where-Object {
    ($_.PSObject.Properties.Name -contains 'profiles') -and ($_.profiles -contains $RunnerProfile)
})
if ($selectedReqs.Count -eq 0) {
    Write-Error "profile '$RunnerProfile' selected ZERO requirements. An empty check set exits 0 and reads as a clean machine, which is the single most misleading thing this script could do."
    exit 2
}
Write-Host ("profile: {0} -- {1} of {2} requirements, >= {3} GB free, labels {4}" -f `
    $RunnerProfile, $selectedReqs.Count, @($spec.requirements).Count,
    $profileSpec.min_free_gb, ($profileSpec.labels -join ',')) -ForegroundColor Cyan

$results = [System.Collections.Generic.List[object]]::new()

function Add-Result {
    param($Id, $Name, $Status, $Detail)
    $results.Add([pscustomobject]@{
        id = $Id; name = $Name; status = $Status; detail = $Detail
    })
}

function Test-VersionAtLeast {
    param([string]$Found, [string]$Min)
    # Extract the first dotted-numeric run; tool --version output is chatty and
    # inconsistent ("git version 2.45.1.windows.1", "cmake version 3.31.2").
    $m = [regex]::Match($Found, '(\d+)\.(\d+)(?:\.(\d+))?')
    if (-not $m.Success) { return $null }   # null = could not determine, NOT false
    $f = [version]::new([int]$m.Groups[1].Value, [int]$m.Groups[2].Value,
                        $(if ($m.Groups[3].Success) { [int]$m.Groups[3].Value } else { 0 }))
    return ($f -ge [version]$Min)
}

function Get-CommandVersion {
    param([string]$Command, [string]$VersionArg)
    $cmd = Get-Command $Command -ErrorAction SilentlyContinue
    if (-not $cmd) { return $null }
    try {
        $out = & $Command $VersionArg 2>&1 | Select-Object -First 1
        return [string]$out
    } catch {
        return "present (version probe failed: $($_.Exception.Message))"
    }
}

# $selectedReqs, NOT $spec.requirements -- see the profile-selection block.
foreach ($req in $selectedReqs) {
    $id = $req.id
    $name = $req.name
    # PER-PROFILE SEVERITY. The same requirement can be fatal on one profile
    # and merely advisory on another -- r2-config is the case: a build machine
    # without R2 still compiles and publishes locally (warn), a consume-only
    # machine without it cannot obtain binaries at all (error). Picking one
    # severity would be wrong for one of the two profiles, in the direction
    # that either blocks a working builder or greenlights a useless consumer.
    # Same shape as disk-space's min_gb_by_profile.
    $sev = if ($req.PSObject.Properties.Name -contains 'severity') { $req.severity } else { 'error' }
    if (($req.PSObject.Properties.Name -contains 'severity_by_profile') -and
        ($req.severity_by_profile.PSObject.Properties.Name -contains $RunnerProfile)) {
        $sev = $req.severity_by_profile.$RunnerProfile
    }
    $v = $req.verify
    $kind = $v.kind

    try {
        switch ($kind) {

            'os-arch' {
                $is64 = [Environment]::Is64BitOperatingSystem
                $os = (Get-CimInstance Win32_OperatingSystem).Caption
                if ($is64) { Add-Result $id $name 'PASS' "$os (64-bit)" }
                else       { Add-Result $id $name 'FAIL' "$os is not 64-bit" }
            }

            'interactive-session' {
                # Session 0 is the service session and has no visible desktop.
                # UIA-driven UI tests cannot work there. This is the single most
                # important check in this file: a runner installed as a service
                # will pass every OTHER check and then fail every UI test in a
                # way that looks like a product bug.
                $sid = (Get-Process -Id $PID).SessionId
                $interactive = [Environment]::UserInteractive
                if ($sid -ne 0 -and $interactive) {
                    Add-Result $id $name 'PASS' "session $sid, UserInteractive=True"
                } else {
                    Add-Result $id $name 'FAIL' ("session $sid, UserInteractive=$interactive -- " +
                        "UI E2E tests need a real desktop. Do NOT install the runner as a service; " +
                        "use an interactive logon session (see README).")
                }
            }

            'command' {
                $found = Get-CommandVersion $v.command $v.version_arg
                if (-not $found) {
                    # allow_missing_if: this tool may be supplied by another
                    # requirement (cmake comes with VS). Only honoured when that
                    # other requirement actually PASSED -- otherwise a missing
                    # tool would excuse itself with a second missing tool.
                    $excuse = $null
                    if ($v.PSObject.Properties.Name -contains 'allow_missing_if') {
                        $excuse = $results | Where-Object { $_.id -eq $v.allow_missing_if -and $_.status -eq 'PASS' }
                    }
                    if ($excuse) {
                        Add-Result $id $name 'PASS' "not on PATH, but supplied by '$($v.allow_missing_if)' which passed"
                    } else {
                        Add-Result $id $name 'FAIL' "'$($v.command)' not found on PATH"
                    }
                } elseif ($v.PSObject.Properties.Name -contains 'min_version') {
                    $ok = Test-VersionAtLeast $found $v.min_version
                    if ($null -eq $ok) {
                        Add-Result $id $name 'FAIL' "found '$found' but could not parse a version to compare against $($v.min_version) -- unverified, so not passed"
                    } elseif ($ok) {
                        Add-Result $id $name 'PASS' $found
                    } else {
                        Add-Result $id $name 'FAIL' "$found is below the required $($v.min_version)"
                    }
                } else {
                    Add-Result $id $name 'PASS' $found
                }
            }

            'path-any' {
                $paths = @($v.paths | ForEach-Object { $_ -replace '%RUNNER_DIR%', $RunnerDir })
                $hit = $paths | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
                if ($hit) {
                    $extra = ''
                    if ($v.PSObject.Properties.Name -contains 'also_command') {
                        $onPath = Get-Command $v.also_command -ErrorAction SilentlyContinue
                        $extra = if ($onPath) { ' (and on PATH)' } else { ' (NOT on PATH -- the workflow prepends its directory, so this is survivable, but note it)' }
                    }
                    Add-Result $id $name 'PASS' "$hit$extra"
                } elseif ($id -eq 'actions-runner' -and -not $RunnerDir) {
                    Add-Result $id $name 'FAIL' '-RunnerDir was not supplied, so this could not be checked (reported as FAIL rather than skipped)'
                } else {
                    Add-Result $id $name 'FAIL' "none of: $($paths -join ' ; ')"
                }
            }

            'path-version' {
                if (-not (Test-Path $v.path)) {
                    Add-Result $id $name 'FAIL' "not installed at $($v.path)"
                } else {
                    $ver = (Get-Item $v.path).VersionInfo.ProductVersion
                    $ok = Test-VersionAtLeast $ver $v.min_version
                    if ($null -eq $ok) {
                        Add-Result $id $name 'FAIL' "installed but version '$ver' is unparseable against $($v.min_version)"
                    } elseif ($ok) {
                        Add-Result $id $name 'PASS' "v$ver"
                    } else {
                        Add-Result $id $name 'FAIL' "v$ver is below the required $($v.min_version) -- a too-old WinAppDriver answers /status but cannot create sessions, which is worse than absent"
                    }
                }
            }

            'env-set' {
                # Every named variable must be set at MACHINE or USER scope.
                # Reports WHICH are missing, not just that something is --
                # "R2 is not configured" sends the operator to check four
                # things; naming the two that are absent does not.
                #
                # Values are never printed. Two of these are credentials, and a
                # readiness report is exactly the artifact somebody pastes into
                # a ticket.
                $missing = @()
                foreach ($e in @($v.env)) {
                    $val = [Environment]::GetEnvironmentVariable($e, 'Machine')
                    if (-not $val) { $val = [Environment]::GetEnvironmentVariable($e, 'User') }
                    if (-not $val) {
                        $item = Get-Item "env:$e" -ErrorAction SilentlyContinue
                        if ($item) { $val = $item.Value }
                    }
                    if (-not $val) { $missing += $e }
                }
                if ($missing.Count -eq 0) {
                    Add-Result $id $name 'PASS' ("all $(@($v.env).Count) variable(s) set")
                } else {
                    Add-Result $id $name 'FAIL' ("not set: $($missing -join ', ')")
                }
            }
            'env-path' {
                $val = [Environment]::GetEnvironmentVariable($v.env, 'Machine')
                if (-not $val) { $val = [Environment]::GetEnvironmentVariable($v.env, 'User') }
                if (-not $val) {
                    # Under Set-StrictMode, dereferencing .Value on the null
                    # returned by a missing env: item throws rather than
                    # yielding null -- which surfaced as "check threw" instead
                    # of the honest "VCPKG_ROOT is not set". Found by running it.
                    $item = Get-Item "env:$($v.env)" -ErrorAction SilentlyContinue
                    if ($item) { $val = $item.Value }
                }
                if (-not $val) {
                    Add-Result $id $name 'FAIL' "$($v.env) is not set"
                } else {
                    $probe = Join-Path $val $v.must_contain
                    if (Test-Path $probe) { Add-Result $id $name 'PASS' "$($v.env)=$val" }
                    else { Add-Result $id $name 'FAIL' "$($v.env)=$val but $probe is missing (not a bootstrapped vcpkg)" }
                }
            }

            'python-module' {
                $py = Get-Command python -ErrorAction SilentlyContinue
                if (-not $py) {
                    Add-Result $id $name 'FAIL' 'python not on PATH, so modules could not be probed'
                } else {
                    $missing = @()
                    foreach ($m in $v.modules) {
                        & python -c "import $m" 2>$null
                        if ($LASTEXITCODE -ne 0) { $missing += $m }
                    }
                    if ($missing.Count -eq 0) { Add-Result $id $name 'PASS' "import ok: $($v.modules -join ', ')" }
                    else { Add-Result $id $name 'FAIL' "cannot import: $($missing -join ', ')" }
                }
            }

            'registry' {
                if (-not (Test-Path $v.path)) {
                    Add-Result $id $name 'FAIL' "registry key $($v.path) does not exist"
                } else {
                    $item = Get-ItemProperty -Path $v.path -Name $v.name -ErrorAction SilentlyContinue
                    if ($null -eq $item) {
                        Add-Result $id $name 'FAIL' "$($v.path)\$($v.name) is not set"
                    } elseif ([int]$item.$($v.name) -eq [int]$v.expect) {
                        Add-Result $id $name 'PASS' "$($v.name)=$($item.$($v.name))"
                    } else {
                        Add-Result $id $name 'FAIL' "$($v.name)=$($item.$($v.name)), expected $($v.expect)"
                    }
                }
            }

            'free-space-gb' {
                # PROFILE-DRIVEN FLOOR. min_gb stays the build figure so an old
                # caller is unchanged; min_gb_by_profile overrides it when the
                # requirement expresses one. Checking a 15 GB consume-only box
                # against 120 GB would fail a machine that is correct.
                $floor = $v.min_gb
                if (($v.PSObject.Properties.Name -contains 'min_gb_by_profile') -and
                    ($v.min_gb_by_profile.PSObject.Properties.Name -contains $RunnerProfile)) {
                    $floor = $v.min_gb_by_profile.$RunnerProfile
                }
                $drive = (Get-Item $PSScriptRoot).PSDrive.Name
                $free = (Get-PSDrive $drive).Free / 1GB
                if ($free -ge $floor) {
                    Add-Result $id $name 'PASS' ("{0:N1} GB free on {1}:" -f $free, $drive)
                } else {
                    Add-Result $id $name 'FAIL' ("only {0:N1} GB free on {1}:, need {2} GB" -f $free, $drive, $floor)
                }
            }

            default {
                # An unrecognised check kind is a defect in this script, and the
                # honest response is to fail rather than to quietly pass an
                # entry nobody verified.
                Add-Result $id $name 'FAIL' "no verifier implemented for kind '$kind' -- this script cannot vouch for this requirement"
            }
        }
    } catch {
        Add-Result $id $name 'FAIL' "check threw: $($_.Exception.Message)"
    }

    # Downgrade to WARN only for explicitly warn-severity items.
    if ($sev -eq 'warn') {
        $last = $results[$results.Count - 1]
        if ($last.status -eq 'FAIL') { $last.status = 'WARN' }
    }
}

# --- the pinned-requirements check needs the repo, handled separately --------
if ($RepoRoot) {
    $reqFile = Join-Path $RepoRoot 'tests\ui_automation\requirements-locked.txt'
    if (Test-Path $reqFile) {
        Add-Result 'requirements-file' 'Pinned UI requirements file present in checkout' 'PASS' $reqFile
    } else {
        Add-Result 'requirements-file' 'Pinned UI requirements file present in checkout' 'FAIL' "not found at $reqFile"
    }
} else {
    Add-Result 'requirements-file' 'Pinned UI requirements file present in checkout' 'FAIL' '-RepoRoot not supplied, so this could not be checked (FAIL, not skipped)'
}

# --- report -----------------------------------------------------------------
$w = ($results | ForEach-Object { $_.name.Length } | Measure-Object -Maximum).Maximum
Write-Host ''
Write-Host 'pdfe-windows runner readiness' -ForegroundColor Cyan
Write-Host ('-' * ($w + 60))
foreach ($r in $results) {
    $colour = switch ($r.status) { 'PASS' { 'Green' } 'WARN' { 'Yellow' } default { 'Red' } }
    # NOTE the parentheses around the concatenation: PowerShell's -f operator
    # binds TIGHTER than +, so without them this parses as
    #   '{0,-6} {1,-' + ($w + ('}  {2}' -f ...))
    # and throws "Input string was not in a correct format" at runtime. Found by
    # running the script, not by reading it.
    $fmt = ('{0,-6} {1,-' + $w + '}  {2}')
    Write-Host ($fmt -f $r.status, $r.name, $r.detail) -ForegroundColor $colour
}
Write-Host ('-' * ($w + 60))

$fails = @($results | Where-Object { $_.status -eq 'FAIL' })
$warns = @($results | Where-Object { $_.status -eq 'WARN' })
Write-Host ("{0} passed, {1} FAILED, {2} warnings" -f
    @($results | Where-Object { $_.status -eq 'PASS' }).Count, $fails.Count, $warns.Count)

if ($Json) {
    $results | ConvertTo-Json -Depth 4 | Out-File -FilePath $Json -Encoding utf8
    Write-Host "machine-readable results -> $Json"
}

if ($fails.Count -gt 0) {
    Write-Host ''
    Write-Host 'This machine is NOT ready to serve as a pdfe-windows runner.' -ForegroundColor Red
    Write-Host 'Registering it with these failures produces jobs that fail deep into a run,' -ForegroundColor Red
    Write-Host 'or worse, report cancelled and alert nobody.' -ForegroundColor Red
    exit 1
}
Write-Host ''

# Prerequisites are not the same thing as a working runner. This block used
# to be absent, so the script said 'Ready.' to a machine that had never been
# registered and could not pick up a single job. Reported, never FAILED on:
# provisioned-but-unregistered is the correct state immediately after step 1,
# and a check that reddens on a correct state is one people learn to ignore.
$runnerCfg = Join-Path $RunnerDir '.runner'
if (Test-Path $runnerCfg) {
    Write-Host 'Ready, and this runner is REGISTERED.' -ForegroundColor Green
    Write-Host ''
    Write-Host '  Confirm it is actually ONLINE (registered but never started shows'
    Write-Host '  as Offline, which looks the same as a machine nobody set up):'
    Write-Host '    https://github.com/ipswitch8/PDFE/settings/actions/runners' -ForegroundColor Cyan
    Write-Host ''
    Write-Host '  If it is Offline, start it under the watchdog:'
    Write-Host ('    .\Register-PdfeRunner.ps1 -RunnerDir ' + $RunnerDir + ' -Start') -ForegroundColor Cyan
} else {
    Write-Host 'Prerequisites are ready. NOT REGISTERED yet.' -ForegroundColor Yellow
    Write-Host ''
    Write-Host '  Nothing is wrong -- this is the expected state after step 1.'
    Write-Host '  This machine will not pick up any job until it is registered.'
    Write-Host ''
    Write-Host '  Next (prompts for the token and tells you where to get one):'
    Write-Host ('    .\Register-PdfeRunner.ps1 -RunnerDir ' + $RunnerDir + ' -Start') -ForegroundColor Cyan
}
exit 0
