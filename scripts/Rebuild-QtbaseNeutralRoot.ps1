# Requires -Version 7.0
<#
.SYNOPSIS
    Rebuild PDFE's vcpkg dependencies under an account-name-free build root so
    no shipped binary embeds the build account name. GitHub #272 (split from #199).

    THE FILENAME IS HISTORICAL. This began as a qtbase-only fix; it now rebuilds
    the whole dependency set, because the `--editable` single-port route it used
    to take cannot populate the binary cache and so could never be durable (see
    the cache section below). The name is kept so the trail through #272 and
    #199 stays followable.

.DESCRIPTION
    Compilers bake absolute source paths into binaries. With the build tree
    under a user profile, those paths carry the account name into shipped
    artifacts -- #199 measured 418 occurrences across six signed binaries. The
    fix is to build where the path contains no account name.

    THIS REPLACES THE SUBST-DRIVE APPROACH, AND THE REASON IS THE POINT.
    The original procedure mapped a drive letter (`subst P: ...`). That works
    for most ports but is fundamentally broken for some, because a subst letter
    gives one directory TWO names -- the letter and its physical path -- and
    builds that compute relative paths across that boundary fail outright.
    Measured 2026-10-02, harfbuzz at package 11 of 47:

        FAILED: [code=1] src/harfbuzz.cc
          os.path.relpath (os.path.abspath (x), CURRENT_SOURCE_DIR)
        ValueError: path is on mount 'P:', start on mount 'C:'

    What that is NOT: not meson's wrapper, and not the MSVC toolchain's
    location (an earlier write-up blamed both). Meson configures fine; the
    failure is at `ninja install`, inside harfbuzz's own amalgamation script.
    Meson ports in this dependency set are harfbuzz and glib.

    Isolating the call confirms the mechanism rather than inferring it: only a
    path on the subst letter against a start on the physical drive raises. The
    same letter works; a real directory works against its own subtree AND an
    unrelated C: tree. So #272's note that it is "not simply a matter of
    putting the repo and vcpkg on one letter" was the right observation with
    the wrong conclusion -- one LETTER does not help, because the letter is the
    problem. One VOLUME, with no letter indirection, does. Under this root
    harfbuzz builds in ~6.4 minutes.

    WHAT "NEUTRAL" HONESTLY MEANS: the artifacts that SHIP. A neutral build
    still leaves `.pdb` and static `.lib` files host-rooted -- MSVC records its
    own path view in debug info, and a static library records each member
    object's path. Measured: a neutral-built libpq archive had all 8 DLLs clean
    and 667 occurrences across 8 `.pdb` + 12 `.lib` members. PDFE deploys
    neither, so verification below scans `.dll` only, over a population DERIVED
    from `_required_dlls` plus the windeployqt-deployed Qt modules. A typed list
    is how `libpq.dll` once got reported as shipping when it does not.

    THE BINARY CACHE WILL UNDO THIS IF YOU LET IT. vcpkg's ABI hash does not
    include the build path -- measured from `<port>/<triplet>.vcpkg_abi_info.txt`,
    whose ~190 entries are content hashes with zero path components. A neutral
    build and a host-rooted one are therefore CACHE-EQUIVALENT: same hash, same
    archive slot, first one stored wins. So:

      * `--editable` bypasses the cache for WRITE as well as read: it yields a
        neutral tree that is never cached, so the fix stays local and the next
        ordinary install silently undoes it. Measured once: 59 minutes of work
        reversed in 53 seconds, exit 0, no warning. Do not use it here.
      * Use -ClearCache so this build repopulates the cache. Without it the
        install restores pre-existing archives and changes nothing.
      * Re-verify AFTER any later install, not just after this build.

.PARAMETER BuildRoot
    Account-name-free build root. Default C:\Users\Public\pdfe-bt.
.PARAMETER ClearCache
    Empty the vcpkg binary cache first so this build repopulates it neutrally.
.PARAMETER MinimumFreeGB
    Refuse to start below this much free space. qtbase alone needs tens of GB.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string] $BuildRoot = 'C:\Users\Public\pdfe-bt',
    [switch] $ClearCache,
    [int]    $MinimumFreeGB = 50,
    [string] $Triplet = 'x64-windows-repro'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:Account = $env:USERNAME

# Redaction applies to every print path. This output gets pasted into issues
# and CI logs, and a tool whose job is removing the account name must not
# republish it.
function Redact([string] $s) {
    if (-not $script:Account -or $script:Account.Length -lt 3) { return $s }
    return [regex]::Replace($s, [regex]::Escape($script:Account), '<account>',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
}
function Write-Step { param([string] $m) Write-Host "[pdfe-neutral] $(Redact $m)" }
function Write-Fail { param([string] $m) Write-Host "[pdfe-neutral] FAIL: $(Redact $m)" -ForegroundColor Red }

# ---------------------------------------------------------------- preflight
# Every check fails CLOSED. A preflight that cannot determine its answer stops:
# the whole subject of #199 is a check that reported success without looking.

if ($script:Account -and $script:Account.Length -ge 3 -and
    $BuildRoot -match [regex]::Escape($script:Account)) {
    Write-Fail "the build root contains the account name, which is the entire thing being fixed: $BuildRoot"
    exit 1
}

$rootQual = Split-Path -Qualifier $BuildRoot
$toolQual = if ($env:VSINSTALLDIR) { Split-Path -Qualifier $env:VSINSTALLDIR } else { 'C:' }
if ($rootQual -ne $toolQual) {
    Write-Fail "build root is on $rootQual but the toolchain is on $toolQual. A cross-volume root reintroduces the relpath failure that broke the subst approach -- see the header."
    exit 1
}
Write-Step "build root : $BuildRoot  (no account name, volume $rootQual)"

# vcpkg: PATH, then VCPKG_ROOT (what CMakePresets.json uses), then the
# documented clone. Never a hardcoded absolute path -- an account-specific
# string in a committed file is the defect being fixed.
$vcpkgExe = $null
$probe = Get-Command vcpkg -ErrorAction SilentlyContinue
if ($probe) { $vcpkgExe = $probe.Source }
if (-not $vcpkgExe) {
    foreach ($c in @(
            $(if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT 'vcpkg.exe' }),
            (Join-Path $HOME 'source\vcpkg\vcpkg.exe'))) {
        if ($c -and (Test-Path $c)) { $vcpkgExe = (Resolve-Path $c).Path; break }
    }
}
if (-not $vcpkgExe) {
    Write-Fail "vcpkg not found. Tried PATH, `$env:VCPKG_ROOT and the documented clone under the user profile."
    exit 1
}
Write-Step "vcpkg      : $vcpkgExe"

$tripletDir = Join-Path $repoRoot 'tools\vcpkg-triplets'
if (-not (Test-Path (Join-Path $tripletDir "$Triplet.cmake"))) {
    Write-Fail "overlay triplet missing: $(Join-Path $tripletDir "$Triplet.cmake")"
    exit 1
}

$vol = Get-PSDrive -Name $rootQual.TrimEnd(':') -PSProvider FileSystem
$freeGB = [math]::Round($vol.Free / 1GB, 1)
Write-Step "free space : $freeGB GB on $rootQual (minimum $MinimumFreeGB GB)"
if ($freeGB -lt $MinimumFreeGB) {
    Write-Fail "insufficient free space; a full rebuild needs tens of GB of buildtrees."
    exit 1
}

# [IO.Path]::Combine, NOT Join-Path, everywhere $BuildRoot is involved.
# Join-Path RESOLVES its drive, so a drive-qualified default like
# 'C:\Users\Public\pdfe-bt' throws "Cannot find drive. A drive with the name
# 'C' does not exist." under non-Windows pwsh. That exact shape kept Fast
# Checks RED across several commits once already, and
# tools/gate/test_runner_watchdog_heartbeat.py enforces it over every tracked
# .ps1 -- it caught this file before the next push. Combine is pure string
# work and byte-identical on Windows for these paths.
foreach ($sub in 'bt','p','i') {
    $d = [IO.Path]::Combine($BuildRoot, $sub)
    if (-not (Test-Path $d)) { $null = New-Item -ItemType Directory -Force $d }
}

# ---------------------------------------------------------------- cache
$archives = Join-Path $env:LOCALAPPDATA 'vcpkg\archives'
if ($ClearCache) {
    $parked = "$archives.pre-neutral"
    if (Test-Path $parked) {
        Write-Fail "a parked cache already exists at $parked -- it is the only copy of whatever was parked before. Deal with it first."
        exit 1
    }
    if ((Test-Path $archives) -and $PSCmdlet.ShouldProcess($archives, 'move aside')) {
        # MOVED, not deleted: derived but expensive (qtbase alone ~58 min), and
        # the cache is machine-wide, possibly shared with other projects.
        Move-Item $archives $parked
        Write-Step "cache moved aside -> $parked (restore is a single rename)"
    }
    $null = New-Item -ItemType Directory -Force $archives
} else {
    Write-Step "cache NOT cleared; pass -ClearCache to repopulate it neutrally (without it, existing archives are restored and nothing changes)"
}

# ---------------------------------------------------------------- build
$argv = @(
    'install',
    "--triplet=$Triplet", "--host-triplet=$Triplet",
    "--overlay-triplets=$tripletDir",
    "--x-manifest-root=$repoRoot",
    "--x-buildtrees-root=$([IO.Path]::Combine($BuildRoot, 'bt'))",
    "--x-packages-root=$([IO.Path]::Combine($BuildRoot, 'p'))",
    "--x-install-root=$([IO.Path]::Combine($BuildRoot, 'i'))"
)
Write-Step "command    : vcpkg $($argv -join ' ')"
if (-not $PSCmdlet.ShouldProcess('vcpkg install', 'full dependency build (hours)')) {
    Write-Step "WhatIf: preflight passed; stopping before the build."
    exit 0
}
Write-Step "starting; expect hours. Do NOT run the GoogleTest suite concurrently -- a loaded machine has produced false test failures in this repo."

& $vcpkgExe @argv
$rc = $LASTEXITCODE
# NOT `$?`, and never through a pipe: a pipeline reports its LAST stage's
# status, which has masked a real failure as success repeatedly here.
if ($rc -ne 0) {
    Write-Fail "vcpkg exited $rc. Nothing is verified; do not treat this as a clean build."
    exit $rc
}
Write-Step "vcpkg exited 0."

# ---------------------------------------------------------------- verify
# A real check, not advice. An earlier version of this script ended with
# "verify with: ..." and its reader then failed to notice a cache restore had
# silently undone the build.
#
# Population DERIVED from the build, never typed.
$cml = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
$m = [regex]::Match($cml, 'set\(_required_dlls(.*?)\r?\n\s*\)', 'Singleline')
if (-not $m.Success) {
    Write-Fail "could not parse set(_required_dlls ...) from CMakeLists.txt -- refusing to verify against a guessed population."
    exit 1
}
# Strip '#' comments BEFORE tokenising: a ')' inside a comment truncated an
# earlier attempt and made the list look empty.
$names = ($m.Groups[1].Value -split "`n" | ForEach-Object { ($_ -split '#')[0] }) -join ' ' -split '\s+' |
         Where-Object { $_ }
$binDir = [IO.Path]::Combine($BuildRoot, 'i', $Triplet, 'bin')
$qt = Get-ChildItem $binDir -Filter 'Qt6*.dll' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name
$targets = @($names | ForEach-Object { "$_.dll" }) + @($qt) | Sort-Object -Unique
Write-Step "verifying  : $($names.Count) from _required_dlls + $($qt.Count) Qt modules"

$dirty = @(); $clean = 0; $absent = @(); $withRoot = 0
foreach ($name in $targets) {
    $p = Join-Path $binDir $name
    if (-not (Test-Path $p)) { $absent += $name; continue }
    $bytes = [System.IO.File]::ReadAllBytes($p)
    $hits = 0
    $rooted = $false
    foreach ($enc in @([System.Text.Encoding]::ASCII, [System.Text.Encoding]::Unicode)) {
        $txt = $enc.GetString($bytes)
        if ($script:Account.Length -ge 3) {
            $hits += ([regex]::Matches($txt, [regex]::Escape($script:Account),
                      'IgnoreCase')).Count
        }
        if ($txt.IndexOf($BuildRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $rooted = $true
        }
    }
    if ($rooted) { $withRoot++ }
    if ($hits -gt 0) { $dirty += "$name($hits)" } else { $clean++ }
}
Write-Step "result     : clean=$clean  dirty=$($dirty.Count)  absent=$($absent.Count)"
if ($absent.Count) { Write-Step "absent     : $($absent -join ', ')" }

# Guard the measurement, not just the result. "Zero account-name hits" is also
# what an empty directory, a path-stripped tree, or a scan of the wrong folder
# produces, and a check that cannot tell those from a real neutral build is
# exactly the vacuous oracle this project keeps rediscovering. A genuine
# neutral build re-ROOTS paths rather than removing them, so at least one
# shipped DLL must contain this build root.
Write-Step "re-rooted  : $withRoot of $($targets.Count - $absent.Count) present DLLs embed the build root"
if ($withRoot -eq 0) {
    Write-Fail "no shipped DLL embeds '$BuildRoot'. A clean result here is not evidence: these binaries were not built under this root, so there is nothing to conclude about re-rooting. Check the install root and that this build actually ran."
    exit 1
}
if ($dirty.Count) {
    Write-Fail "shipped binaries still carry the account name: $($dirty -join ', ')"
    Write-Fail "If the counts match the ORIGINAL leak figures, a cache restore has overwritten this build -- re-run with -ClearCache and see the header."
    exit 1
}
# Targeted check for the two occurrences #272 is actually about. The
# population scan above would pass if Qt6Widgets.dll were merely absent or
# stripped, and this issue's criterion names these two files specifically.
#
# The predicate here is "path contains the account name", NOT "path starts with
# C:\Users". The previous version of this script used the latter, which is a
# latent false FAIL under the default root: C:\Users\Public\pdfe-bt starts with
# exactly that prefix while containing no account name, so a correct neutral
# build would have been reported NOT NEUTRAL.
$qtWidgets = Join-Path $binDir 'Qt6Widgets.dll'
if (Test-Path $qtWidgets) {
    $wtext = [System.Text.Encoding]::ASCII.GetString(
                 [System.IO.File]::ReadAllBytes($qtWidgets))
    $srcPaths = [regex]::Matches($wtext, '(?:[A-Za-z]:[\\/]|\\\\)[^\s"''<>|*?\r\n]*?\.cpp\b')
    $leaky = @($srcPaths | Where-Object {
                 $script:Account.Length -ge 3 -and
                 $_.Value -match [regex]::Escape($script:Account) })
    $wanted = @('qdatetimeedit.cpp', 'qabstractspinbox.cpp')
    $found  = @($wanted | Where-Object { $p = $_; $srcPaths | Where-Object { $_.Value -like "*$p" } })
    Write-Step ("Qt6Widgets : .cpp paths=" + $srcPaths.Count +
                "  account-name-in-path=" + $leaky.Count +
                "  of 2 expected files found=" + $found.Count)
    if ($leaky.Count -ne 0 -or $found.Count -ne 2) {
        Write-Fail "Qt6Widgets.dll is not verifiably neutral: expected both qdatetimeedit.cpp and qabstractspinbox.cpp present and re-rooted, with no account name in any embedded path. Finding only one re-rooted leaves half the fix unverified."
        exit 1
    }
} else {
    Write-Fail "Qt6Widgets.dll absent from $binDir. The two occurrences this issue is about cannot be checked, so nothing is verified."
    exit 1
}

Write-Step "VERIFIED: every shipped DLL is free of the account name."
Write-Step "Re-run this verification after ANY later 'vcpkg install' -- a restore can undo it silently."
Write-Step "Do NOT use ``strings``: absent on this host, and it exits 0 through a pipe, so it reports 0 hits for any input."
exit 0
