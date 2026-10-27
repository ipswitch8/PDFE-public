#requires -Version 5.1
<#
.SYNOPSIS
  Supplies `Get-FileHash` via .NET when the Utility module does not
  auto-load, so install_tools.ps1's PINNED-DIGEST VERIFICATION can run.

.DESCRIPTION
  install_tools.ps1 verifies every downloaded scanner against a SHA-256 pinned
  in that script (R2-27) and DELETES a file whose digest does not match. On
  2026-10-20, on the PDFE build host, that verification could not run at all:

      The term 'Get-FileHash' is not recognized as the name of a cmdlet...

  The installer therefore reported `failed` for gitleaks, trufflehog and
  osv-scanner. **That was correct behaviour and must not be "fixed" by skipping
  the check** -- a scanner installed without digest verification is exactly the
  supply-chain hole the pin exists to close.

  This script restores the missing primitive instead. It computes SHA-256 with
  System.Security.Cryptography, which needs no module auto-loading, and exposes
  it under the expected cmdlet name and output shape (`.Hash`, uppercase hex)
  so install_tools.ps1 runs UNMODIFIED. The pinned comparison is still the
  installer's own.

  WHY THIS IS COMMITTED. The `karen` gate's phase-6 review observed that the
  claim "the shim was known-answer tested" had nothing inspectable behind it:
  a replacement hashing function could, in principle, have been written to
  return whatever digest the caller expected, and no artifact would show
  otherwise. So the shim lives here, in the repository, where it can be read --
  and it self-tests before it is trusted.

.PARAMETER SkipKnownAnswerTest
  Do not run the known-answer test. Provided only so the test itself can be
  exercised; using it defeats the point.

.EXAMPLE
  . .\scripts\security\get_filehash_shim.ps1
  .\scripts\security\install_tools.ps1 -OnlyTools 'gitleaks,trufflehog'
#>
param([switch]$SkipKnownAnswerTest)

if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
    Write-Host '[filehash-shim] Get-FileHash resolves natively; no shim installed.'
    return
}

function global:Get-FileHash {
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [Alias('LiteralPath')]
        [string]$Path,
        [ValidateSet('SHA1','SHA256','SHA384','SHA512','MD5')]
        [string]$Algorithm = 'SHA256'
    )
    $full = (Resolve-Path -LiteralPath $Path).Path
    $alg  = [System.Security.Cryptography.HashAlgorithm]::Create($Algorithm)
    if (-not $alg) { throw "[filehash-shim] unsupported algorithm '$Algorithm'" }
    $fs = [System.IO.File]::OpenRead($full)
    try { $bytes = $alg.ComputeHash($fs) } finally { $fs.Dispose(); $alg.Dispose() }
    [pscustomobject]@{
        Algorithm = $Algorithm.ToUpperInvariant()
        Hash      = (-join ($bytes | ForEach-Object { $_.ToString('x2') })).ToUpperInvariant()
        Path      = $full
    }
}
Write-Host '[filehash-shim] installed Get-FileHash backed by .NET SHA-256.'

if ($SkipKnownAnswerTest) {
    Write-Warning '[filehash-shim] known-answer test SKIPPED -- the shim is unverified.'
    return
}

# KNOWN-ANSWER TEST, against published digests rather than self-consistency.
# Two vectors: the empty string, and the canonical "abc". A shim that returned
# a caller-pleasing value, or that hashed the path instead of the contents,
# fails both.
$vectors = @(
    @{ Bytes = @();                      Expect = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855' }
    @{ Bytes = [byte[]]@(0x61,0x62,0x63); Expect = 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad' }
)
foreach ($v in $vectors) {
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        [System.IO.File]::WriteAllBytes($tmp, $v.Bytes)
        $got = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($got -ne $v.Expect) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            throw ("[filehash-shim] KNOWN-ANSWER TEST FAILED for a {0}-byte input: got {1}, expected {2}. Refusing to let a pinned-digest check run through an unverified hash function." -f $v.Bytes.Count, $got, $v.Expect)
        }
    } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
}
Write-Host '[filehash-shim] known-answer test passed (2 published SHA-256 vectors).'
