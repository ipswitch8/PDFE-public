# -*- coding: utf-8 -*-
<#
.SYNOPSIS
    Sign Windows binaries with Azure Trusted Signing.

.DESCRIPTION
    Signs one or more files (pdfe.exe, the NSIS installer, etc.) using Azure
    Trusted Signing via the `sign` .NET global tool (https://github.com/dotnet/sign).
    Trusted Signing applies a Microsoft-issued Authenticode certificate AND an
    RFC-3161 timestamp from Microsoft's timestamp authority in one step.

    Authentication uses Azure DefaultAzureCredential, in this order:
      1. `az login` (interactive desktop / dev box)  <-- run this first locally
      2. Service-principal env vars in CI:
         AZURE_TENANT_ID / AZURE_CLIENT_ID / AZURE_CLIENT_SECRET
      3. Managed identity (Azure-hosted runners)

    NO secrets are stored in this repo. The account/profile/endpoint are passed
    as parameters or environment variables.

.PARAMETER Files
    One or more file paths or globs to sign.

.PARAMETER Endpoint
    Trusted Signing region endpoint, e.g. https://eus.codesigning.azure.net/
    (defaults to $env:TRUSTED_SIGNING_ENDPOINT).

.PARAMETER Account
    Trusted Signing account name (defaults to $env:TRUSTED_SIGNING_ACCOUNT).

.PARAMETER CertProfile
    Certificate profile name (defaults to $env:TRUSTED_SIGNING_PROFILE).

.EXAMPLE
    $env:TRUSTED_SIGNING_ENDPOINT = "https://eus.codesigning.azure.net/"
    $env:TRUSTED_SIGNING_ACCOUNT  = "my-ts-account"
    $env:TRUSTED_SIGNING_PROFILE  = "my-cert-profile"
    az login
    ./scripts/sign/Sign-Artifacts.ps1 -Files build/vs2022/bin/Release/pdfe.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]] $Files,
    [string] $Endpoint    = $env:TRUSTED_SIGNING_ENDPOINT,
    [string] $Account     = $env:TRUSTED_SIGNING_ACCOUNT,
    [string] $CertProfile = $env:TRUSTED_SIGNING_PROFILE,
    [string] $Description  = "PDFE - PDF Editor",
    [string] $DescriptionUrl = "https://github.com/ipswitch8/PDFE",
    # Azure credential to use: azure-cli | azure-powershell | managed-identity |
    # workload-identity. Empty => the sign tool's DefaultAzureCredential chain
    # (use '' in CI when authenticating via AZURE_* service-principal env vars).
    [string] $CredentialType = 'azure-cli',
    [switch] $SkipVerify
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

function Fail($msg) { Write-Error $msg; exit 1 }

# --- Validate configuration -------------------------------------------------
$missing = @()
if (-not $Endpoint)    { $missing += 'Endpoint (TRUSTED_SIGNING_ENDPOINT)' }
if (-not $Account)     { $missing += 'Account (TRUSTED_SIGNING_ACCOUNT)' }
if (-not $CertProfile) { $missing += 'CertProfile (TRUSTED_SIGNING_PROFILE)' }
if ($missing.Count) {
    Fail ("Missing Trusted Signing config: " + ($missing -join ', ') + "`n" +
          "Set them as parameters or environment variables. See docs/code-signing.md.")
}

# --- Resolve files ----------------------------------------------------------
$resolved = @()
foreach ($f in $Files) {
    $items = Get-ChildItem -Path $f -File -ErrorAction SilentlyContinue
    if (-not $items) { Fail "No file matched: $f" }
    $resolved += $items.FullName
}
$resolved = $resolved | Sort-Object -Unique
Write-Host "Files to sign ($($resolved.Count)):" -ForegroundColor Cyan
$resolved | ForEach-Object { Write-Host "  $_" }

# --- Ensure the `sign` .NET tool is available -------------------------------
$toolsDir = Join-Path $env:USERPROFILE '.dotnet\tools'
if ($env:PATH -notlike "*$toolsDir*") { $env:PATH = "$env:PATH;$toolsDir" }
if (-not (Get-Command sign -ErrorAction SilentlyContinue)) {
    Write-Host "Installing the 'sign' .NET global tool..." -ForegroundColor Yellow
    # The dotnet/sign tool ships as a prerelease NuGet package -- --prerelease is required.
    dotnet tool install --global sign --prerelease 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "Failed to install the 'sign' tool. Requires dotnet SDK + nuget.org access. See docs/code-signing.md for the signtool+dlib alternative."
    }
    if ($env:PATH -notlike "*$toolsDir*") { $env:PATH = "$env:PATH;$toolsDir" }
}

# --- Sign -------------------------------------------------------------------
foreach ($file in $resolved) {
    Write-Host "`nSigning: $file" -ForegroundColor Cyan
    # NOTE: the `sign` tool renamed the Trusted Signing verb to `artifact-signing`
    # (the old `trusted-signing` is a deprecated alias). Flags are --artifact-signing-*.
    $signArgs = @(
        'code', 'artifact-signing', $file,
        '--artifact-signing-endpoint',            $Endpoint,
        '--artifact-signing-account',             $Account,
        '--artifact-signing-certificate-profile', $CertProfile,
        '--description',                          $Description,
        '--description-url',                      $DescriptionUrl,
        '--verbosity', 'information'
    )
    if ($CredentialType) { $signArgs += @('--azure-credential-type', $CredentialType) }
    & sign @signArgs
    if ($LASTEXITCODE -ne 0) { Fail "Signing failed for $file (exit $LASTEXITCODE). If auth failed, run 'az login' (dev, default -CredentialType azure-cli) or set AZURE_* SP env vars + pass -CredentialType ''." }
}

# --- Verify -----------------------------------------------------------------
if (-not $SkipVerify) {
    $signtool = (Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*x64*' } | Sort-Object FullName -Descending | Select-Object -First 1).FullName
    if (-not $signtool) { Write-Warning "signtool.exe not found; skipping verification."; }
    else {
        foreach ($file in $resolved) {
            Write-Host "`nVerifying: $file" -ForegroundColor Cyan
            & $signtool verify /pa /v $file
            if ($LASTEXITCODE -ne 0) { Fail "Verification FAILED for $file" }
        }
    }
}

Write-Host "`nAll files signed and verified." -ForegroundColor Green
