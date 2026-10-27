<#
.SYNOPSIS
    Map a drive letter to the dependency build root so built binaries cannot
    embed the build account name. GitHub #199 / #191.

.DESCRIPTION
    SUPERSEDED 2026-10-02 FOR A FULL REBUILD -- USE
    scripts/Rebuild-QtbaseNeutralRoot.ps1 INSTEAD (GitHub #272).

    The analysis below is still correct and worth reading. The MECHANISM this
    script uses -- `subst` a drive letter -- is not viable for a from-scratch
    rebuild, for a reason measured only after it was written:

        FAILED: [code=1] src/harfbuzz.cc
          os.path.relpath (os.path.abspath (x), CURRENT_SOURCE_DIR)
        ValueError: path is on mount 'P:', start on mount 'C:'

    `subst` gives one directory TWO names -- the letter and its physical path --
    and a build that computes a relative path across that boundary dies.
    Isolating the call confirms the mechanism: the ONLY failing case is a path
    on the subst letter against a start on the physical drive; the same letter
    works, and a real directory works both against its own subtree and against
    an unrelated C: tree. Meson ports in this dependency set are harfbuzz and
    glib. Under a real directory (C:\Users\Public\pdfe-bt) harfbuzz builds in
    6.4 minutes.

    There is a second, independent reason not to use the single-port
    `--editable` route this script was written for: `--editable` bypasses the
    binary cache for WRITE as well as read, so it yields a neutral tree that is
    never cached and the next ordinary `vcpkg install` silently restores the old
    dirty archive over it -- measured, 59 minutes of work undone in 53 seconds
    with exit 0. vcpkg's ABI hash excludes the build path, so a neutral build
    and a host-rooted one are cache-equivalent.

    Keep this script for the measurement record and for the /d1trimfile
    reasoning. Do not use it as the procedure.

    THE PROBLEM THIS SOLVES, MEASURED 2026-09-30

    Compilers bake absolute source paths into their output -- MSVC via __FILE__
    (and `assert`, which expands to `_wassert(L"" __FILE__, ...)`, so many of
    them are WIDE strings), OpenSSL additionally via configure-time data
    directories such as OPENSSLDIR and ENGINESDIR. When the build tree lives
    under a user profile, every one of those strings carries the account name
    into a shipped binary.

    Measured in the shipped DLL set before this change, 127 occurrences across
    five installed binaries:

        tesseract55.dll      106   __FILE__ from a vcpkg buildtree
        libcrypto-3-x64.dll   16   4 __FILE__, 12 configure-time data paths
        Qt6Widgets.dll         2   __FILE__ from a vcpkg buildtree
        libssl-3-x64.dll       2   __FILE__ from a vcpkg buildtree
        tiff.dll               1   __FILE__ from a vcpkg buildtree

    Every one of those paths began `C:\Users\<account>\source\PDFE\build\...`.
    The account name is in them for exactly one reason: the repository sits
    under the profile. Nothing about the ports themselves is at fault.

    WHY A SUBST DRIVE, AND NOT A COMPILER FLAG

    The obvious fix is MSVC's /d1trimfile, which does work -- verified on
    MSVC 14.44.35207: an absolute __FILE__ that carried the account name came
    out empty with the flag applied. But it reaches only the ports that both
    (a) compile with MSVC and (b) actually thread the triplet's compiler flags
    through. tools/vcpkg-triplets/x64-windows-repro.cmake already documents that
    (b) is unreliable -- it measured ports that honour VCPKG_LINKER_FLAGS for
    release links but not debug, because their build systems replace rather
    than append. OpenSSL builds via perl + nmake, and 12 of its 16 occurrences
    are configure-time DATA paths that no compiler flag can reach at all.

    Relocating the build root is mechanism-independent. It does not matter how
    a port compiles, or whether it honours any flag, because there is no
    account name in the path for anything to embed.

    WHY subst AND NOT A JUNCTION OR A PLAIN SHORT PATH

    Verified: cl.exe records the path AS GIVEN. Compiling the same file through
    the physical path and through the subst drive produced

        C:\Users\<account>\source\PDFE\build\bt\substtest\s.c
        P:\substtest\s.c

    A directory junction resolves to its physical target and so does not help.
    A plain short root such as C:\pdfe-bt would also work, but it puts build
    state outside the repository, which this project's layout otherwise avoids;
    subst keeps every byte written inside build/ while still presenting a
    neutral path to the toolchain.

    THE TRADE, STATED RATHER THAN BURIED

    This mapping is per-session and per-machine: it must be established before
    vcpkg configures, including on CI. If it is missing, vcpkg fails because the
    buildtrees root does not exist -- it fails CLOSED, which is the right
    direction, but it does fail. The backstop for the opposite risk (a build
    that quietly succeeds against the profile path again) is
    tools/gate/check_package_account_name.py, which scans the installer payload
    and reddens on any unallowlisted occurrence. The fragile mechanism has a
    mechanical detector; that pairing is the point.

.PARAMETER DriveLetter
    Drive letter to map, without the colon. Default P.

.PARAMETER Target
    Directory to map it to. Defaults to <repo>/build/bt.

.PARAMETER Remove
    Remove the mapping instead of creating it.

.EXAMPLE
    pwsh -File scripts/Set-NeutralBuildRoot.ps1
    # then configure/build; pass the roots to vcpkg as P:\b and P:\p

.EXAMPLE
    pwsh -File scripts/Set-NeutralBuildRoot.ps1 -Remove
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z]$')]
    [string]$DriveLetter = 'P',

    [string]$Target,

    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$drive = $DriveLetter.ToUpperInvariant()
$spec  = "${drive}:"

function Get-SubstMap {
    # `subst` with no arguments lists current mappings as "P:\: => C:\path".
    $out = & cmd.exe /c subst 2>$null
    $map = @{}
    foreach ($line in $out) {
        if ($line -match '^([A-Za-z]):\\?:?\s*=>\s*(.+)$') {
            $map[$Matches[1].ToUpperInvariant()] = $Matches[2].Trim()
        }
    }
    return $map
}

if ($Remove) {
    $existing = Get-SubstMap
    if (-not $existing.ContainsKey($drive)) {
        Write-Host "${spec} is not a subst mapping; nothing to remove."
        exit 0
    }
    & cmd.exe /c subst $spec /D
    if ($LASTEXITCODE -ne 0) { throw "subst $spec /D failed ($LASTEXITCODE)" }
    Write-Host "Removed ${spec}"
    exit 0
}

if (-not $Target) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $Target = Join-Path $repoRoot 'build/bt'
}
$Target = [System.IO.Path]::GetFullPath($Target)

if (-not (Test-Path -LiteralPath $Target)) {
    New-Item -ItemType Directory -Path $Target -Force | Out-Null
    Write-Host "Created $Target"
}

$existing = Get-SubstMap

if ($existing.ContainsKey($drive)) {
    $current = [System.IO.Path]::GetFullPath($existing[$drive])
    if ($current -eq $Target) {
        Write-Host "${spec} already maps to $Target -- nothing to do."
        exit 0
    }
    # Do NOT silently re-point it. Another pipeline or another checkout may be
    # mid-build against this letter, and re-pointing it would corrupt that
    # build's output rather than fail it.
    throw ("${spec} is already mapped to $current, not $Target. " +
           "Refusing to re-point a drive another build may be using. " +
           "Choose a different -DriveLetter, or remove the existing mapping " +
           "deliberately with: subst ${spec} /D")
}

# A real (non-subst) drive at this letter must also not be clobbered.
if (Test-Path -LiteralPath "${spec}\") {
    throw ("${spec} exists but is not a subst mapping -- it looks like a real " +
           "volume. Choose a different -DriveLetter.")
}

& cmd.exe /c subst $spec $Target
if ($LASTEXITCODE -ne 0) { throw "subst $spec `"$Target`" failed ($LASTEXITCODE)" }

# Verify rather than assume: subst can report success and still not be visible.
$after = Get-SubstMap
if (-not $after.ContainsKey($drive)) {
    throw "subst reported success but ${spec} is not listed. Not usable."
}
if (-not (Test-Path -LiteralPath "${spec}\")) {
    throw "${spec} is listed by subst but not accessible. Not usable."
}

Write-Host "Mapped ${spec} -> $Target"
Write-Host ""
Write-Host "Pass these to vcpkg so no built binary can embed the account name:"
Write-Host "  --x-buildtrees-root=${spec}\b"
Write-Host "  --x-packages-root=${spec}\p"
Write-Host ""
Write-Host "Verify the result with:"
Write-Host "  python tools/gate/check_package_account_name.py <installer-payload>"
