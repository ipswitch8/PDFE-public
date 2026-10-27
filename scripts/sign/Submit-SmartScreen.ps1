# -*- coding: utf-8 -*-
<#
.SYNOPSIS
    Prepare (and open) a Microsoft SmartScreen / WDSI submission for a signed installer.

.DESCRIPTION
    Microsoft's file-submission portal is INTERACTIVE: it requires signing in
    with a Microsoft account and uploading the file through the web form. There
    is no public unauthenticated API to submit arbitrary installers, so this
    script cannot fully automate the upload. What it DOES do:

      * Confirm the file carries a valid Authenticode signature (unsigned files
        should not be submitted -- sign first with Build-Release-Signed.ps1).
      * Gather everything the submission form asks for (SHA-256, signer subject,
        timestamp, product/company, size) into a copy-paste block and a JSON
        file next to the installer.
      * Open the submission portal in the default browser.

    Use the "Software developer" path on the portal. This channel is primarily
    for asking Microsoft to review a file you believe is being INCORRECTLY
    flagged by SmartScreen/Defender -- it is a dispute/allow-list request, not a
    general "pre-approve my app" service. SmartScreen reputation is built mainly
    by (a) code signing -- which you now do via Trusted Signing -- and (b)
    download volume accruing over time. Submit here if a signed release is still
    warned on after real-world downloads.

.PARAMETER Installer
    Path to the SIGNED installer to submit.

.PARAMETER SubmissionUrl
    Microsoft file-submission URL (default: the WDSI developer submission page).

.PARAMETER Open
    Open the submission portal in the browser (default: on).

.EXAMPLE
    ./scripts/sign/Submit-SmartScreen.ps1 -Installer build/vs2022-package/installer/PDFE-0.1.0-win64.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Installer,
    [string] $SubmissionUrl = "https://www.microsoft.com/en-us/wdsi/filesubmission",
    [string] $Company = "PDFE Project",
    [string] $ProductName = "PDFE",
    [switch] $NoOpen
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Installer)) { Write-Error "Installer not found: $Installer"; exit 1 }
$item = Get-Item $Installer

# --- Signature check (must be signed before submitting) ---------------------
$sig = Get-AuthenticodeSignature $Installer
if ($sig.Status -ne 'Valid') {
    Write-Error ("Installer is NOT validly signed (status=$($sig.Status)). " +
        "Sign it first with scripts/sign/Build-Release-Signed.ps1 before submitting to SmartScreen.")
    exit 1
}
$signer = $sig.SignerCertificate
$sha256 = (Get-FileHash $Installer -Algorithm SHA256).Hash
$sha1   = (Get-FileHash $Installer -Algorithm SHA1).Hash

$info = [ordered]@{
    product          = $ProductName
    company          = $Company
    fileName         = $item.Name
    sizeBytes        = $item.Length
    sha256           = $sha256
    sha1             = $sha1
    signatureStatus  = "$($sig.Status)"
    signerSubject    = "$($signer.Subject)"
    signerIssuer     = "$($signer.Issuer)"
    signerThumbprint = "$($signer.Thumbprint)"
    signedOn         = "$($sig.TimeStamperCertificate.NotBefore)"
    submissionUrl    = $SubmissionUrl
}

Write-Host "`n===== SmartScreen submission details (copy into the portal form) =====" -ForegroundColor Cyan
$info.GetEnumerator() | ForEach-Object { "{0,-16}: {1}" -f $_.Key, $_.Value } | Write-Host
Write-Host "=====================================================================" -ForegroundColor Cyan

$outJson = [System.IO.Path]::ChangeExtension($item.FullName, '.smartscreen-submission.json')
$info | ConvertTo-Json | Set-Content -Path $outJson -Encoding UTF8
Write-Host "Saved submission metadata: $outJson"

Write-Host "`nManual steps on the portal:" -ForegroundColor Yellow
Write-Host "  1. Sign in with your Microsoft account."
Write-Host "  2. Choose the 'Software developer' submission type."
Write-Host "  3. Upload: $($item.FullName)"
Write-Host "  4. Select 'Incorrectly detected as malware/blocked by SmartScreen' and reference the details above."
Write-Host "  5. Provide the public download URL (the GitHub release asset) so Microsoft can fetch it."

if (-not $NoOpen) {
    Write-Host "`nOpening submission portal..." -ForegroundColor Cyan
    Start-Process $SubmissionUrl
}
