<#
.SYNOPSIS
    Install the dependencies a pdfe-windows self-hosted runner needs.

.DESCRIPTION
    Reads runner-requirements.json (the same file Test-PdfeRunner.ps1 reads) and
    installs what is missing. It does NOT decide whether the machine is ready --
    that is Test-PdfeRunner.ps1's job, deliberately kept separate so the thing
    doing the work is not also the thing grading it.

    WHAT THIS SCRIPT WILL NOT DO, and why:

      * It will not register the runner with GitHub. Registration needs a
        short-lived token that a human must fetch from the repo's Settings ->
        Actions -> Runners page. Scripting a token grab would mean handling
        credentials this script has no business holding. It prints the exact
        command to run instead.
      * It will not install the runner as a Windows SERVICE. A service runs in
        session 0 with no visible desktop, and every UIA-driven UI test would
        blind or hang there. See the README for the autologon + watchdog
        arrangement used instead.
      * It will not silently continue past a failed install. A dependency that
        "probably installed" is how a runner ends up failing 40 minutes into a
        job -- exactly the OpenCppCoverage situation this package addresses.

.PARAMETER RepoRoot
    Path to a PDFE checkout, used for the pinned pip requirements.

.PARAMETER VcpkgRoot
    Where to clone/bootstrap vcpkg. Default C:\vcpkg.

.PARAMETER Skip
    Comma-separated requirement ids to skip (e.g. 'vs2022' when VS is already
    installed by other means). Skipped items are REPORTED as skipped, and
    Test-PdfeRunner.ps1 will still check them independently.

.PARAMETER RunnerProfile
    Which role this machine will play: 'build' (compiles pdfe -- needs the full
    MSVC + vcpkg toolchain and >= 120 GB) or 'consume' (runs tests against
    binaries built elsewhere and restored by the cross-machine build store --
    needs neither, and >= 15 GB). Requirements are selected from the `profiles`
    array each requirement in runner-requirements.json carries, so a profile
    cannot drift out of sync with the requirement list.

    NAMED -RunnerProfile AND NOT -Profile ON PURPOSE: `$Profile` is a PowerShell
    AUTOMATIC variable (the path to the current profile script). A parameter of
    that name shadows it for the whole script scope, which works right up until
    something in here or in a dot-sourced helper reads $Profile expecting the
    automatic. Shadowing an automatic to save five characters is not a trade
    worth making.

.PARAMETER WhatIf
    Show what would be installed without installing it.

.EXAMPLE
    # A build-capable machine, from an ELEVATED PowerShell in an interactive
    # desktop session:
    .\Install-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -VcpkgRoot C:\vcpkg

.EXAMPLE
    # A consume-only machine -- no Visual Studio, no vcpkg, ~15 GB:
    .\Install-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -RunnerProfile consume
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$RepoRoot,
    [string]$VcpkgRoot = 'C:\vcpkg',
    [string]$Skip = '',
    [ValidateSet('build', 'consume')]
    [string]$RunnerProfile = 'build'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- preconditions ----------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    # NOT fatal any more, deliberately. Refusing outright made the no-elevation
    # path unreachable by exactly the person who needs it: this runner's account
    # is not in the local Administrators group, and OpenCppCoverage's portable
    # fallback requires no elevation at all. A script that will not start cannot
    # run the one step that would have worked.
    #
    # Everything needing HKLM or Program Files is now REPORTED as requiring
    # elevation rather than attempted and failed, so the summary distinguishes
    # "not done because you are not admin" from "tried and broke".
    Write-Warning "Not elevated. Requirements needing HKLM or Program Files will be REPORTED, not attempted."
    Write-Warning "Re-run from an elevated PowerShell to complete those. Items with a portable fallback still install."
    # NB: no Note "" here -- Note is defined further down, and calling it this
    # early throws CommandNotFoundException at runtime (PowerShell resolves
    # function calls when executed, not at parse time, so it parses clean).
}
if ((Get-Process -Id $PID).SessionId -eq 0) {
    Write-Error "Running in session 0. Install from an INTERACTIVE desktop session -- the runner this prepares must itself run interactively for UI tests to work."
    exit 2
}

$reqPath = Join-Path $PSScriptRoot 'runner-requirements.json'
$spec = Get-Content $reqPath -Raw | ConvertFrom-Json
$skipIds = @($Skip -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })

# --- profile selection ------------------------------------------------------
# Fail LOUDLY on an unknown or unexpressed profile rather than falling back to
# "install everything". A silent fallback would provision a 120 GB toolchain on
# a machine the operator asked to keep small, and the operator would find out
# from the disk, not from this script.
if (-not ($spec.PSObject.Properties.Name -contains 'profiles')) {
    Write-Error "runner-requirements.json has no 'profiles' block; this script cannot select a role's requirements and will not guess."
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
    Write-Error "profile '$RunnerProfile' selected ZERO requirements. Either every requirement lost its 'profiles' tag or the profile name is wrong; installing nothing and reporting success would be the worst outcome here."
    exit 2
}

$log = [System.Collections.Generic.List[string]]::new()
function Note { param($m, $c = 'Gray') Write-Host $m -ForegroundColor $c; $log.Add($m) }

Note "pdfe-windows runner installer" 'Cyan'
Note "requirements: $reqPath"
Note "profile: $RunnerProfile -- $($selectedReqs.Count) of $(@($spec.requirements).Count) requirements, >= $($profileSpec.min_free_gb) GB free"
Note "labels: $($profileSpec.labels -join ',')"
Note ""

# --- helpers ----------------------------------------------------------------

# WHY THERE IS NO WINGET PATH HERE.
#
# Four requirements used to install via winget. On the machines this package
# targets that is not a degraded path, it is a dead one: the existing runner is
# Windows Server 2019, which has no Microsoft Store, and winget ships as the
# App Installer Store package. Measured on the live host, winget is absent and
# the Appx module will not even load ("Operation is not supported on this
# platform"), so App Installer cannot be added either.
#
# So every installable requirement is now a pinned URL plus explicit silent
# args, which is ONE code path that runs identically on Server 2019 and on a
# Windows 11 box that happens to have winget. A winget-first-with-fallback
# design would mean the fallback -- the only path that ever runs here -- is the
# one that gets least exercised.

function Test-Prop {
    # Set-StrictMode -Version Latest makes `$obj.missing` a runtime ERROR, not
    # $null. So `if (-not $inst.url)` never reports "no url" for an entry that
    # HAS no url -- StrictMode throws first, with a message about the property
    # rather than about the requirement. The mutation harness caught exactly
    # this: three shape checks were killing their mutations, but by way of
    # StrictMode, which made the checks themselves dead code.
    param($Object, [string]$Name)
    return [bool](($Object.PSObject.Properties.Name -contains $Name) -and $Object.$Name)
}

function Update-ProcessPath {
    # Installers write the MACHINE PATH; this process inherited its PATH at
    # launch and will not see the change. That matters within this very script:
    # the python-ui-deps step shells out to `python` and the vcpkg step to
    # `git`, both potentially installed moments earlier in this same loop. Re-
    # reading the registry-backed PATH here is what makes install order safe.
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user    = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:PATH = (@($machine, $user) | Where-Object { $_ }) -join ';'
}

function Install-FromUrl {
    param(
        [string]$Url,
        [string]$SilentArgs,
        [int[]]$OkExitCodes = @(0, 3010, 1641)
    )
    $file = Join-Path $env:TEMP (Split-Path $Url -Leaf)
    Note "  downloading $Url"
    # -UseBasicParsing so this works on a machine with no IE first-run profile,
    # which a freshly imaged Server box will not have.
    Invoke-WebRequest -Uri $Url -OutFile $file -UseBasicParsing
    if (-not (Test-Path $file) -or (Get-Item $file).Length -eq 0) {
        throw "download of $Url produced an empty file"
    }

    if ($file -match '\.msi$') {
        # SilentArgs is deliberately ignored for MSIs: quiet/norestart is the
        # whole vocabulary and msiexec needs its own argument order.
        Note "  msiexec /i $file /quiet /norestart"
        $p = Start-Process msiexec.exe -ArgumentList "/i `"$file`" /quiet /norestart" -Wait -PassThru
    } else {
        # Silent args come from runner-requirements.json rather than being
        # inferred from the extension. The inference this replaces was wrong:
        # it assumed NSIS /S for every .exe, but Git for Windows is Inno Setup
        # (which ignores /S and would sit at an interactive prompt forever on a
        # headless deploy) and the VS bootstrapper is neither.
        Note "  $file $SilentArgs"
        $p = Start-Process $file -ArgumentList $SilentArgs -Wait -PassThru
    }

    # 3010 and 1641 mean "succeeded, reboot required/initiated". Treating those
    # as failure would have failed a correct VS or MSI install.
    if ($OkExitCodes -notcontains $p.ExitCode) {
        throw "installer '$file' failed with exit code $($p.ExitCode)"
    }
    if ($p.ExitCode -in 3010, 1641) { Note "  (exit $($p.ExitCode): installed, REBOOT REQUIRED)" 'Yellow' }
    Update-ProcessPath
}

function Install-Portable {
    <#
      Extract an Inno Setup installer's payload instead of running it, and put
      the result somewhere that needs no elevation.

      WHY THIS EXISTS. OpenCppCoverage ships ONLY as an Inno installer, which
      requires administrator rights -- and a runner account is frequently not in
      the local Administrators group. That made a REQUIRED dependency (R-32:
      absence is a hard error) depend on an operator action that may never
      happen; on this project it had not happened, and the coverage step had
      consequently NEVER produced a measurement in any recorded run.

      It turns out not to need installing at all. The payload runs standalone --
      it instruments through the debugging APIs, which work fine against a
      same-user process. Verified on the live runner from an unelevated
      extracted copy: a real Cobertura report, lines-valid=53399.

      Adding the destination to the USER PATH also needs no elevation. Note a
      process started BEFORE that change will not see it, which is why
      windows-validation.yml probes the directory directly rather than relying
      on PATH alone.
    #>
    param($Spec, [string]$InstallerUrl)

    $dest = [Environment]::ExpandEnvironmentVariables($Spec.dest)
    $work = Join-Path $env:TEMP ("pdfe-portable-" + [IO.Path]::GetFileName($dest))
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $work -Force | Out-Null

    Note "  fetching innoextract"
    $zip = Join-Path $work 'innoextract.zip'
    Invoke-WebRequest -Uri $Spec.innoextract_url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $work -Force
    $ie = Get-ChildItem $work -Recurse -Filter 'innoextract.exe' | Select-Object -First 1
    if (-not $ie) { throw "innoextract.exe not found in $($Spec.innoextract_url)" }

    # Install-FromUrl already downloaded this before the install attempt
    # failed; reuse it rather than fetching 16 MB twice.
    $installer = Join-Path $env:TEMP (Split-Path $InstallerUrl -Leaf)
    if (-not (Test-Path $installer)) {
        Note "  re-downloading installer for extraction"
        Invoke-WebRequest -Uri $InstallerUrl -OutFile $installer -UseBasicParsing
    }

    Note "  extracting payload"
    Push-Location $work
    try {
        & $ie.FullName -e -s $installer | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "innoextract failed ($LASTEXITCODE)" }
    } finally { Pop-Location }

    $app = Join-Path $work 'app'
    if (-not (Test-Path $app)) { throw "extraction produced no 'app' directory" }

    Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    Copy-Item (Join-Path $app '*') $dest -Recurse -Force
    Note "  placed at $dest"

    if ($Spec.add_to_user_path) {
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        if (-not $userPath) { $userPath = '' }
        if (($userPath -split ';') -notcontains $dest) {
            $new = if ($userPath.Trim()) { "$userPath;$dest" } else { $dest }
            [Environment]::SetEnvironmentVariable('Path', $new, 'User')
            Note "  added to USER PATH (no elevation needed)"
        } else {
            Note "  already on USER PATH"
        }
        $env:PATH = "$dest;$env:PATH"
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

function Set-MachineRegistry {
    param([string]$Path, [string]$Name, [int]$Value)
    if (-not (Test-Path $Path)) { New-Item -Path $Path -Force | Out-Null }
    New-ItemProperty -Path $Path -Name $Name -Value $Value -PropertyType DWord -Force | Out-Null
    Note "  set $Path\$Name = $Value"
}

# --- install loop -----------------------------------------------------------
$failed = @()
$needElevation = @()
$dryRunSeen = $false
# $selectedReqs, NOT $spec.requirements: the profile decides what this machine
# needs. Iterating the full list and filtering inside the loop would have worked
# too, but then every future edit to the loop body has to remember the filter --
# and the failure mode of forgetting is installing Visual Studio on a 15 GB box.
foreach ($req in $selectedReqs) {
    $id = $req.id
    if ($skipIds -contains $id) { Note "SKIP  $id (requested)" 'Yellow'; continue }
    if (-not ($req.PSObject.Properties.Name -contains 'install')) {
        if ($req.PSObject.Properties.Name -contains 'installable' -and -not $req.installable) {
            Note "n/a   $id -- $($req.name) (cannot be installed by script; Test-PdfeRunner.ps1 checks it)" 'DarkGray'
        }
        continue
    }

    $inst = $req.install
    Note "==> $id : $($req.name)" 'White'
    # NB: NOT `if (-not ShouldProcess) { continue }`, which is what this used to
    # be. That skipped the whole branch, so -WhatIf proved only that the loop
    # iterates -- it never touched the per-kind code that decodes each entry, and
    # a requirement with a missing url, an unknown kind or a malformed
    # portable_fallback looked identical to a correct one. Only 1 of the 11
    # requirements was ever executed by any test.
    #
    # Instead the branches now ALL run and resolve their arguments, and the
    # effectful calls alone are gated on $DryRun. A dry run therefore exercises
    # the dispatch for every requirement and fails on a malformed one.
    $DryRun = -not $PSCmdlet.ShouldProcess($id, "install")
    if ($DryRun) { Note "  [dry-run] resolving only; nothing will be installed" 'DarkGray' }

    try {
        switch ($inst.kind) {

            'download' {
                $silent = if ($inst.PSObject.Properties.Name -contains 'silent_args') { $inst.silent_args } else { '' }
                $okCodes = if ($inst.PSObject.Properties.Name -contains 'ok_exit_codes') {
                    [int[]]$inst.ok_exit_codes
                } else { @(0, 3010, 1641) }
                $hasPortable = $inst.PSObject.Properties.Name -contains 'portable_fallback'
                if (-not (Test-Prop $inst 'url')) { throw "install.kind 'download' with no url" }
                if ($hasPortable -and -not (Test-Prop $inst.portable_fallback 'dest')) {
                    throw "portable_fallback with no dest"
                }
                if ($DryRun) {
                    Note "  [dry-run] would download $($inst.url)"
                    Note "  [dry-run] silent args: '$silent'  ok-exit-codes: $($okCodes -join ',')"
                    if ($hasPortable) { Note "  [dry-run] portable fallback -> $($inst.portable_fallback.dest)" }
                    break
                }
                if (-not $isAdmin -and -not $hasPortable) {
                    Note "  NEEDS ELEVATION — not attempted. Re-run elevated." 'Yellow'
                    $needElevation += $id
                    break
                }
                if (-not $isAdmin -and $hasPortable) {
                    # Do not run an installer we know will fail; go straight to
                    # the path that works unelevated.
                    Note "  not elevated — using the PORTABLE install directly" 'Yellow'
                    Install-Portable -Spec $inst.portable_fallback -InstallerUrl $inst.url
                    break
                }
                try {
                    Install-FromUrl -Url $inst.url -SilentArgs $silent -OkExitCodes $okCodes
                } catch {
                    if ($hasPortable) {
                        Note "  installer failed ($($_.Exception.Message))" 'Yellow'
                        Note "  trying the PORTABLE fallback (no elevation required)" 'Yellow'
                        Install-Portable -Spec $inst.portable_fallback -InstallerUrl $inst.url
                    } else { throw }
                }
            }

            'registry-set' {
                if (-not (Test-Prop $inst 'path') -or -not (Test-Prop $inst 'name')) {
                    throw "registry-set with no path/name"
                }
                if ($DryRun) {
                    Note "  [dry-run] would set $($inst.path)\$($inst.name) = $([int]$inst.value)"
                    break
                }
                Set-MachineRegistry -Path $inst.path -Name $inst.name -Value ([int]$inst.value)
            }

            'git-clone-bootstrap' {
                $dest = if ($VcpkgRoot) { $VcpkgRoot } else { $inst.default_dest }
                if (-not (Test-Prop $inst 'repo') -or -not (Test-Prop $inst 'bootstrap')) {
                    throw "git-clone-bootstrap with no repo/bootstrap"
                }
                if ($DryRun) {
                    Note "  [dry-run] would clone $($inst.repo) -> $dest, run $($inst.bootstrap), set VCPKG_ROOT"
                    break
                }
                if (-not (Test-Path (Join-Path $dest '.git'))) {
                    Note "  cloning $($inst.repo) -> $dest"
                    & git clone $inst.repo $dest
                    if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }
                } else {
                    Note "  $dest already a git checkout; leaving it alone"
                }
                $bootstrap = Join-Path $dest $inst.bootstrap
                Note "  bootstrapping"
                & $bootstrap
                if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed ($LASTEXITCODE)" }
                [Environment]::SetEnvironmentVariable('VCPKG_ROOT', $dest, 'Machine')
                $env:VCPKG_ROOT = $dest
                Note "  VCPKG_ROOT=$dest (machine scope)"
            }

            'pip-requirements' {
                if (-not $RepoRoot) {
                    throw "-RepoRoot is required to install the pinned UI dependencies (they live in the checkout at $($inst.path))"
                }
                $file = Join-Path $RepoRoot $inst.path
                if (-not (Test-Path $file)) { throw "requirements file not found at $file" }
                if ($DryRun) {
                    Note "  [dry-run] would run: python -m pip install $($inst.args) -r $file"
                    break
                }
                Note "  python -m pip install $($inst.args) -r $file"
                & python -m pip install --upgrade pip
                & python -m pip install $inst.args -r $file
                if ($LASTEXITCODE -ne 0) { throw "pip install failed ($LASTEXITCODE)" }
            }

            'actions-runner' {
                # The registration itself needs a credential this installer has
                # no business holding, so it is still a separate step -- but it
                # is no longer a PROSE step. Register-PdfeRunner.ps1 prompts for
                # the token, says exactly where to get one, preflights the runner
                # directory BEFORE asking (so nobody burns a 60-minute token and
                # then finds the path wrong), and starts the runner under the
                # watchdog. The hand-typed form below is kept as the fallback and
                # as the thing check_label_drift.py compares against.
                Note "  NEXT STEP -- registration. One command, it prompts for the token:" 'Green'
                Note "    .\Register-PdfeRunner.ps1 -RunnerDir C:\actions-runner -Start"
                Note ""
                Note "  Or by hand, if you prefer:" 'Yellow'
                Note "    1. https://github.com/ipswitch8/PDFE/settings/actions/runners/new"
                Note "    2. Follow the download block it shows, into e.g. C:\actions-runner"
                Note "    3. config.cmd --url https://github.com/ipswitch8/PDFE --token <TOKEN> \"
                # PDFE-DEFAULT-LABELS -- the canonical default registration command.
                # check_label_drift.py reads the label set from the NEXT --labels
                # after this marker. Do not add a second marker in this file.
                Note "         --labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder --unattended"
                Note "       pdfe-builder is REQUIRED, not optional. Only windows-validation's" 'Yellow'
                Note "       build and release jobs ask for it, and GitHub does not error on a" 'Yellow'
                Note "       runs-on no runner satisfies -- the job queues, then reports" 'Yellow'
                Note "       'cancelled', which alerts nobody. Omit it and those two die silently." 'Yellow'
                Note "       Drop it ONLY on a multi-machine fleet where another registered" 'Yellow'
                Note "       machine carries it (see profiles.consume in runner-requirements.json)." 'Yellow'
                Note "    4. Start it under tools\ops\runner_watchdog.ps1, NOT bare run.cmd"
                Note "       and NOT --runasservice. A service has no desktop, so every UI"
                Note "       test fails; bare run.cmd dies silently, and a dead runner looks"
                Note "       exactly like an idle one until a job queues and reports cancelled."
            }

            default { throw "unknown install kind '$($inst.kind)'" }
        }
        if ($DryRun) { $dryRunSeen = $true; Note "  resolved ok (dry run — nothing installed)" 'DarkGray' }
        else { Note "  ok" 'Green' }
    } catch {
        Note "  FAILED: $($_.Exception.Message)" 'Red'
        $failed += $id
    }
}

Note ""
if ($needElevation.Count -gt 0) {
    Note "NOT ATTEMPTED (need an elevated shell): $($needElevation -join ', ')" 'Yellow'
}
if ($failed.Count -gt 0) {
    Note "Installs that FAILED: $($failed -join ', ')" 'Red'
} elseif ($dryRunSeen) {
    # Deliberately NOT "completed with no failures": a dry run installed nothing,
    # and reporting it as a successful pass is precisely the say-nothing-exit-0
    # shape this phase exists to remove.
    Note "DRY RUN: every requirement resolved, NOTHING was installed. Re-run without -WhatIf." 'Yellow'
} else {
    Note "Install pass completed with no reported failures." 'Green'
}

Note ""
Note "NOW VERIFY -- this script's report is what it TRIED to do, not what is true:" 'Cyan'
$verifyCmd = ".\Test-PdfeRunner.ps1"
if ($RepoRoot) { $verifyCmd += " -RepoRoot `"$RepoRoot`"" }
$verifyCmd += " -RunnerDir C:\actions-runner"
Note "    $verifyCmd" 'Cyan'

$logPath = Join-Path $PSScriptRoot 'install-log.txt'
$log | Out-File -FilePath $logPath -Encoding utf8
Note "log -> $logPath"

# A failed install must not exit 0. The verifier is the authority on readiness,
# but silently exiting 0 here would let a deploy script continue as if all was
# well.
if ($failed.Count -gt 0) { exit 1 }
exit 0
