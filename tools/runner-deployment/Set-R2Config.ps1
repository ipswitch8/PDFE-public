<#
.SYNOPSIS
    Validate Cloudflare R2 credentials by a real round-trip, then set them at
    machine scope for the runner.

.DESCRIPTION
    The four PDFE_R2_* variables are the one part of runner provisioning that
    cannot be scripted end-to-end: creating a Cloudflare account needs email
    verification and acceptance of terms, and minting an R2 API token is done in
    the dashboard. What CAN be scripted is everything after you have the values
    -- and more importantly, PROVING they work before a CI job depends on them.

    WHY VALIDATE RATHER THAN JUST SET. Setting four environment variables always
    "succeeds". The failure shows up an hour later, inside a job, as a restore
    that could not reach the store -- and the operator then cannot tell a typo
    in the endpoint from a token with the wrong permissions from a bucket that
    does not exist. This does a real PUT / GET / DELETE against the actual
    bucket and reports which of those failed, which is the difference between a
    two-minute fix and an afternoon.

    IT DISTINGUISHES READ-ONLY FROM BROKEN. Consume-only machines are supposed
    to hold a READ-ONLY token (see docs/ci/r2-build-store.md) -- that is the
    control the store's integrity rests on. So a PUT that is refused while GET
    works is reported as CORRECT for -RunnerProfile consume, and as a failure
    for build. A checker that called read-only "broken" would push operators
    toward giving every machine write access, which is precisely the outcome
    the read-only rule exists to prevent.

    NOTHING IS WRITTEN until validation passes, so a failed run leaves the
    machine exactly as it was rather than half-configured -- the state
    ensure-build-tree refuses loudly and deliberately.

.PARAMETER EnvFile
    Path to a KEY=VALUE file supplying any of PDFE_R2_ACCOUNT_ID,
    PDFE_R2_ENDPOINT, PDFE_R2_BUCKET, PDFE_R2_ACCESS_KEY_ID,
    PDFE_R2_SECRET_ACCESS_KEY. Defaults to `.env` beside this script, then `.env`
    at the repository root. Explicit parameters override the file.

    The file MUST be git-ignored; this refuses to read one that is not, so a
    credential cannot end up one `git add` away from a commit. Start from
    `.env.example`.

.PARAMETER AccountId
    Cloudflare account id. The endpoint is derived from it as
    https://<AccountId>.r2.cloudflarestorage.com. Supply this OR -Endpoint.

.PARAMETER Endpoint
    Full S3 endpoint, if you would rather paste it than have it derived.

.PARAMETER Bucket
    R2 bucket name, e.g. pdfe-build-store.

.PARAMETER AccessKeyId
    R2 API token's Access Key ID.

.PARAMETER SecretAccessKey
    R2 API token's Secret Access Key. Prompted for securely if omitted, so it
    need not appear in shell history.

.PARAMETER RunnerProfile
    build (needs write) or consume (read-only is correct and expected).

.PARAMETER WhatIf
    Validate and report, but do not set anything.

.EXAMPLE
    .\Set-R2Config.ps1 -AccountId abc123 -Bucket pdfe-build-store `
                       -AccessKeyId <id> -RunnerProfile build
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$AccountId,
    [string]$Endpoint,
    [string]$Bucket,
    [string]$AccessKeyId,
    [string]$SecretAccessKey,
    [string]$EnvFile,
    [ValidateSet('build', 'consume')]
    [string]$RunnerProfile = 'build',
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Say { param($m, $c = 'Gray') Write-Host $m -ForegroundColor $c }

# ---------------------------------------------------------------------------
# .env loading
#
# The account id and access key id are per-machine values nobody wants to retype
# and nobody should paste into shell history. They live in a .env file that git
# ignores.
#
# THE FILE MUST BE IGNORED, AND THAT IS CHECKED AT RUNTIME rather than trusted.
# `.gitignore` had a real hole here: the blanket `.env` rule near the top does
# NOT survive the `!tools/runner-deployment/**` negation further down -- last
# match wins -- so a .env dropped beside this script was COMMITTABLE. It is
# fixed, but a rule in a file is not a guarantee about the file in front of you:
# somebody can copy this script elsewhere, or edit .gitignore. So this refuses
# to read a .env that git would track, which fails safe under any .gitignore.
# ---------------------------------------------------------------------------
function Import-DotEnv {
    param([string]$Path)

    $vals = @{}
    if (-not (Test-Path $Path)) { return $vals }

    # THE QUESTION IS "COULD THIS BE COMMITTED", NOT "IS GIT HAPPY".
    #
    # A file OUTSIDE any git repository cannot be committed at all, so it is
    # safe by construction. That is not a corner case -- it is the NORMAL case
    # on a provisioned runner, where the package sits at C:\pdfe-runner-deploy
    # and the repo may not be checked out on that machine at all.
    #
    # The first version of this check ran `git check-ignore` unconditionally.
    # Outside a repo that does not return "not ignored", it FAILS with
    # "is outside repository" -- so the safety check would have refused the
    # ordinary deployment and shown the operator a raw git error instead of a
    # reason. Found by running it against a path in TEMP.
    $dir = Split-Path -Parent $Path
    if (-not $dir) { $dir = '.' }

    # NATIVE STDERR IS NOT A POWERSHELL ERROR, but under
    # `$ErrorActionPreference = 'Stop'` PowerShell turns it into a TERMINATING
    # one. `git rev-parse` outside a repo writes "not a git repository" to
    # stderr and exits non-zero -- which is the answer we want, not a failure --
    # and the script died on it instead of reading the exit code. `2>&1` does
    # not help: it merges the streams but still produces an ErrorRecord.
    #
    # So the preference is relaxed around the native calls only, and restored
    # immediately. Without this the ordinary deployed case -- a .env beside the
    # package on a machine with no checkout -- aborted with a raw git error.
    $git = Get-Command git -ErrorAction SilentlyContinue
    $inRepo = $false
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($git) {
            & git -C $dir rev-parse --show-toplevel 2>&1 | Out-Null
            $inRepo = ($LASTEXITCODE -eq 0)
        }
        $ignored = $false
        if ($inRepo) {
            & git -C $dir check-ignore -q -- $Path 2>&1 | Out-Null
            $ignored = ($LASTEXITCODE -eq 0)
        }
    } finally {
        $ErrorActionPreference = $prevEap
    }

    if (-not $inRepo) {
        # Either git is absent or this path is not under version control.
        # Nothing can commit it, so there is nothing to protect it from.
        Say "  ($Path is not inside a git repository, so it cannot be committed)" 'DarkGray'
    } else {
        if (-not $ignored) {
            Write-Error @"
'$Path' is inside a git repository and is NOT ignored, so reading credentials from it would leave them one 'git add' away from being committed.

Add it to .gitignore first. If this is inside tools/runner-deployment/, note that the blanket '.env' rule does not reach here on its own -- the '!tools/runner-deployment/**' negation re-includes it, and an explicit re-ignore is required AFTER that negation.
"@
            exit 2
        }
    }

    foreach ($line in (Get-Content -LiteralPath $Path)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $eq = $t.IndexOf('=')
        if ($eq -lt 1) { continue }
        $k = $t.Substring(0, $eq).Trim()
        $v = $t.Substring($eq + 1).Trim()
        # Strip one matched pair of surrounding quotes. A secret containing '#'
        # is why comments are only stripped on WHOLE lines, never mid-value.
        if (($v.Length -ge 2) -and
            (($v[0] -eq '"' -and $v[-1] -eq '"') -or ($v[0] -eq "'" -and $v[-1] -eq "'"))) {
            $v = $v.Substring(1, $v.Length - 2)
        }
        if ($k) { $vals[$k] = $v }
    }
    return $vals
}

# Search order: an explicit -EnvFile, then beside this script, then the repo
# root. Beside-the-script first because that is where an operator provisioning a
# machine will put it -- the repo may not even be checked out there.
$envCandidates = @()
if ($EnvFile) { $envCandidates += $EnvFile }
else {
    $envCandidates += (Join-Path $PSScriptRoot '.env')
    $envCandidates += (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '.env')
}

$loaded = @{}
$loadedFrom = $null
foreach ($c in $envCandidates) {
    if (Test-Path $c) {
        $loaded = Import-DotEnv -Path $c
        $loadedFrom = $c
        break
    }
}
if ($EnvFile -and -not $loadedFrom) {
    Write-Error "-EnvFile '$EnvFile' does not exist."
    exit 2
}
if ($loadedFrom) {
    Say "reading configuration from $loadedFrom (git-ignored)" 'Gray'
} else {
    Say "no .env found; using parameters only. Copy .env.example to .env to avoid retyping." 'Gray'
}

# EXPLICIT PARAMETERS WIN over the file. A value typed on the command line is a
# deliberate override for this invocation, and silently preferring the file
# would make the override do nothing -- which is the kind of thing that is only
# discovered after it has already caused a wrong deployment.
function Resolve-Val {
    param([string]$Current, [string]$Key)
    if ($Current) { return $Current }
    if ($loaded.ContainsKey($Key)) { return $loaded[$Key] }
    return ''
}
$AccountId       = Resolve-Val $AccountId       'PDFE_R2_ACCOUNT_ID'
$Endpoint        = Resolve-Val $Endpoint        'PDFE_R2_ENDPOINT'
$Bucket          = Resolve-Val $Bucket          'PDFE_R2_BUCKET'
$AccessKeyId     = Resolve-Val $AccessKeyId     'PDFE_R2_ACCESS_KEY_ID'
$SecretAccessKey = Resolve-Val $SecretAccessKey 'PDFE_R2_SECRET_ACCESS_KEY'

# Mandatory-ness is enforced HERE rather than on the parameters, because a
# `[Parameter(Mandatory)]` prompts before the .env is ever read -- which would
# defeat the entire point of having one.
$missing = @()
if (-not $Bucket)      { $missing += 'Bucket (PDFE_R2_BUCKET)' }
if (-not $AccessKeyId) { $missing += 'AccessKeyId (PDFE_R2_ACCESS_KEY_ID)' }
if ($missing.Count -gt 0) {
    # NOT `??` -- that is PowerShell 7 only, and this package targets Windows
    # Server 2019 where Windows PowerShell 5.1 is what is present. The parser
    # caught it; on the target machine it would have been a runtime failure in
    # the error path, i.e. only when something had already gone wrong.
    $where = if ($loadedFrom) { $loadedFrom } else { 'a .env file' }
    Write-Error "Missing required value(s): $($missing -join ', '). Supply as a parameter or in $where."
    exit 2
}

# --- resolve the endpoint ---------------------------------------------------
if (-not $Endpoint) {
    if (-not $AccountId) {
        Write-Error "Supply -AccountId (the endpoint is derived from it) or -Endpoint."
        exit 2
    }
    $Endpoint = "https://$AccountId.r2.cloudflarestorage.com"
}
# A trailing slash, or a bucket accidentally appended to the endpoint, both
# produce confusing 403s rather than obvious errors. Catch them here.
$Endpoint = $Endpoint.TrimEnd('/')
if ($Endpoint -match "/$([regex]::Escape($Bucket))$") {
    Write-Error "The endpoint ends with the bucket name ('$Bucket'). R2's S3 endpoint is the ACCOUNT host only -- the bucket is a separate argument, and including it here yields 403s that look like a permissions problem."
    exit 2
}
if ($Endpoint -notmatch '^https://') {
    Write-Error "Endpoint must be https://. Got: $Endpoint"
    exit 2
}

if (-not $SecretAccessKey) {
    $sec = Read-Host -AsSecureString "R2 Secret Access Key"
    $SecretAccessKey = [Runtime.InteropServices.Marshal]::PtrToStringAuto(
        [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec))
}
if (-not $SecretAccessKey) {
    Write-Error "No secret supplied; refusing to configure a store that cannot authenticate."
    exit 2
}

Say "Cloudflare R2 configuration check" 'Cyan'
Say "  endpoint : $Endpoint"
Say "  bucket   : $Bucket"
Say "  key id   : $($AccessKeyId.Substring(0, [Math]::Min(6, $AccessKeyId.Length)))...  (secret never printed)"
Say "  profile  : $RunnerProfile"
Say ""
# Checked AFTER the summary on purpose: an operator who is about to be told a
# tool is missing should first see WHAT the script resolved, so a wrong bucket
# or endpoint is visible in the same output rather than one run later.
if (-not (Get-Command aws -ErrorAction SilentlyContinue)) {
    Write-Error "The AWS CLI is not on PATH. R2 is S3-compatible and both the restore and the crash-dump shipper drive ``aws s3``. Install it first: .\Install-PdfeRunner.ps1 -RunnerProfile $RunnerProfile (requirement id 'aws-cli')."
    exit 2
}


# Scope the credentials to this process only. Nothing is persisted unless every
# check that matters passes.
$env:AWS_ACCESS_KEY_ID     = $AccessKeyId
$env:AWS_SECRET_ACCESS_KEY = $SecretAccessKey
$env:AWS_DEFAULT_REGION    = 'auto'

$probeKey = "pdfe-preflight/$([guid]::NewGuid().ToString('N')).txt"
$tmp = Join-Path $env:TEMP "pdfe-r2-probe.txt"
"pdfe r2 preflight" | Out-File -FilePath $tmp -Encoding ascii

$canList = $false; $canRead = $false; $canWrite = $false
$listErr = ""; $writeErr = ""

# --- 1. can we reach the bucket at all? ------------------------------------
$out = aws s3api head-bucket --endpoint-url $Endpoint --bucket $Bucket 2>&1
if ($LASTEXITCODE -eq 0) {
    $canList = $true
    Say "  [ok]   bucket reachable and credentials accepted" 'Green'
} else {
    $listErr = "$out"
    Say "  [FAIL] cannot reach bucket '$Bucket'" 'Red'
    if ($listErr -match '(?i)404|NoSuchBucket') {
        Say "         The credentials worked but the bucket does not exist. Create it in the R2 dashboard, or check the name." 'Yellow'
    } elseif ($listErr -match '(?i)403|InvalidAccessKeyId|SignatureDoesNotMatch') {
        Say "         The bucket may exist but these credentials were refused. Check the Access Key ID / Secret, and that the token is scoped to this bucket." 'Yellow'
    } else {
        Say "         $listErr" 'Yellow'
    }
}

# --- 2. write, read back, clean up -----------------------------------------
if ($canList) {
    $out = aws s3 cp $tmp "s3://$Bucket/$probeKey" --endpoint-url $Endpoint --only-show-errors 2>&1
    if ($LASTEXITCODE -eq 0) {
        $canWrite = $true
        Say "  [ok]   write" 'Green'
    } else {
        $writeErr = "$out"
        Say "  [--]   write refused" 'Yellow'
    }

    if ($canWrite) {
        $back = Join-Path $env:TEMP "pdfe-r2-probe-back.txt"
        Remove-Item $back -Force -ErrorAction SilentlyContinue
        aws s3 cp "s3://$Bucket/$probeKey" $back --endpoint-url $Endpoint --only-show-errors 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path $back) -and
            ((Get-Content $back -Raw).Trim() -eq 'pdfe r2 preflight')) {
            $canRead = $true
            Say "  [ok]   read back, contents match" 'Green'
        } else {
            Say "  [FAIL] wrote an object but could not read it back intact" 'Red'
        }
        Remove-Item $back -Force -ErrorAction SilentlyContinue
        aws s3 rm "s3://$Bucket/$probeKey" --endpoint-url $Endpoint --only-show-errors 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { Say "  [ok]   probe object removed" 'Green' }
        else { Say "  [--]   could not delete the probe object ($probeKey) -- remove it by hand" 'Yellow' }
    } else {
        # A read-only token cannot write. Prove READ works using any existing
        # object, so we can tell "correctly read-only" from "entirely broken".
        $listing = aws s3api list-objects-v2 --endpoint-url $Endpoint --bucket $Bucket --max-keys 1 2>&1
        if ($LASTEXITCODE -eq 0) {
            $canRead = $true
            Say "  [ok]   read (list) works" 'Green'
        } else {
            Say "  [FAIL] neither write nor read succeeded" 'Red'
        }
    }
}
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

# --- verdict ---------------------------------------------------------------
Say ""
$ok = $false
if (-not $canList) {
    Say "RESULT: NOT configured. The bucket could not be reached." 'Red'
} elseif ($RunnerProfile -eq 'consume') {
    if ($canRead -and -not $canWrite) {
        Say "RESULT: correct for a CONSUME machine -- read works, write is refused." 'Green'
        Say "        Read-only is the intended state here: it is what stops a compromised" 'Gray'
        Say "        test machine poisoning the store every other runner restores from." 'Gray'
        $ok = $true
    } elseif ($canRead -and $canWrite) {
        Say "RESULT: WORKS, but this token can WRITE." 'Yellow'
        Say "        A consume-only machine should hold a READ-ONLY token. With write" 'Yellow'
        Say "        access, the marker check inside the store authenticates nothing" 'Yellow'
        Say "        against this machine -- see docs/ci/r2-build-store.md. Re-issue the" 'Yellow'
        Say "        token as read-only, or pass -Force to configure anyway." 'Yellow'
        $ok = [bool]$Force
    } else {
        Say "RESULT: NOT configured. Read did not work." 'Red'
    }
} else {
    if ($canWrite -and $canRead) {
        Say "RESULT: correct for a BUILD machine -- read and write both work." 'Green'
        $ok = $true
    } else {
        Say "RESULT: NOT configured. A build machine must be able to WRITE (it publishes" 'Red'
        Say "        the closure every other machine restores). Write error was:" 'Red'
        Say "        $writeErr" 'Yellow'
    }
}

if (-not $ok) {
    Say ""
    Say "Nothing was written to the machine environment. It is unchanged." 'Gray'
    exit 1
}

# --- persist ---------------------------------------------------------------
if (-not $PSCmdlet.ShouldProcess("machine environment", "set the four PDFE_R2_* variables")) {
    Say ""
    Say "DRY RUN -- validation passed; nothing was set." 'Cyan'
    exit 0
}

try {
    [Environment]::SetEnvironmentVariable('PDFE_R2_ENDPOINT',          $Endpoint,        'Machine')
    [Environment]::SetEnvironmentVariable('PDFE_R2_BUCKET',            $Bucket,          'Machine')
    [Environment]::SetEnvironmentVariable('PDFE_R2_ACCESS_KEY_ID',     $AccessKeyId,     'Machine')
    [Environment]::SetEnvironmentVariable('PDFE_R2_SECRET_ACCESS_KEY', $SecretAccessKey, 'Machine')
} catch {
    Write-Error "Could not write machine-scope environment variables (this needs an ELEVATED shell): $($_.Exception.Message)"
    exit 2
}

Say ""
Say "Set at MACHINE scope: PDFE_R2_ENDPOINT, PDFE_R2_BUCKET, PDFE_R2_ACCESS_KEY_ID, PDFE_R2_SECRET_ACCESS_KEY" 'Green'
Say ""
Say "RESTART THE RUNNER before this takes effect." 'Yellow'
Say "  The Actions runner reads its environment at PROCESS START, so a variable" 'Gray'
Say "  set after run.cmd launched is invisible to every job it runs -- which" 'Gray'
Say "  presents as 'R2 not configured' on a machine you have just configured." 'Gray'
Say ""
Say "Then confirm:  .\Test-PdfeRunner.ps1 -RunnerProfile $RunnerProfile -RepoRoot <checkout>" 'Gray'
exit 0
