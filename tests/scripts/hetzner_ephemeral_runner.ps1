<#
.SYNOPSIS
    Provision an ephemeral Hetzner GitHub Actions runner, run one
    workflow, and destroy the server.

.DESCRIPTION
    Creates a fresh Hetzner Cloud VM, installs build dependencies +
    vcpkg + the GitHub Actions runner, registers it to the PDFE repo,
    optionally dispatches a workflow and waits for completion, then
    unregisters the runner and deletes the server.

    The server is ALWAYS destroyed on exit — success, failure, or
    Ctrl+C — via a trap handler. No idle billing.

.PARAMETER ServerType
    Hetzner server type. Default: cpx42 (8 cores, 16 GB, 240 GB).
    Smaller types will OOM during Qt6 vcpkg build.

.PARAMETER Location
    Hetzner location. Default: nbg1 (Nuremberg).
    Other options: fsn1 (Falkenstein), hel1 (Helsinki), ash (Ashburn US),
    hil (Hillsboro US), sin (Singapore).

.PARAMETER Workflow
    Workflow file to dispatch after the runner is online.
    Use 'none' to skip dispatch (manual workflow_dispatch from another
    terminal). Default: 'sanitizer.yml'.

.PARAMETER FuzzDuration
    Per-harness fuzz duration in seconds (only used when Workflow is
    'fuzz.yml'). Default: 1800 (30 min).

.PARAMETER KeepOnFailure
    If set, do NOT destroy the server when the workflow fails — useful
    for debugging. The server will still be destroyed on success or
    Ctrl+C. Default: $false (always destroy).

.EXAMPLE
    .\hetzner_ephemeral_runner.ps1
    # Provisions cpx42, runs sanitizer.yml, destroys server.

.EXAMPLE
    .\hetzner_ephemeral_runner.ps1 -Workflow fuzz.yml -FuzzDuration 600
    # Runs 10-minute fuzz campaign, destroys server.

.EXAMPLE
    .\hetzner_ephemeral_runner.ps1 -Workflow none
    # Provisions + registers runner, waits for manual dispatch via gh.

.NOTES
    Requires: hcloud CLI (configured with `hcloud context create`),
    gh CLI (authenticated to ipswitch8 with workflow scope),
    SSH key at $env:USERPROFILE\.ssh\id_rsa.
#>

[CmdletBinding()]
param(
    [string]$ServerType = "cpx42",
    [string]$Location = "nbg1",
    [string]$Workflow = "sanitizer.yml",
    [int]$FuzzDuration = 1800,
    [switch]$KeepOnFailure
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# ----- Constants -----------------------------------------------------
$ServerName = "pdfe-ephemeral-runner"
$RunnerName = "pdfe-hetzner-eph-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$RunnerLabels = "self-hosted,linux,hetzner,sanitizer"
$GhRepo = "ipswitch8/PDFE"
$SshKeyName = "pdfe-key"
$HcloudExe = "$env:USERPROFILE\bin\hcloud.exe"

# Resolve gh — prefer the real exe over the .cmd shim, because the
# shim mangles --jq quoting on PowerShell, silently breaking API calls.
$GhExe = "$env:USERPROFILE\AppData\Local\Programs\gh\bin\gh.exe"
if (-not (Test-Path $GhExe)) {
    $candidate = (Get-Command gh -CommandType Application -ErrorAction SilentlyContinue |
                  Where-Object { $_.Source -like "*.exe" } | Select-Object -First 1).Source
    if ($candidate) { $GhExe = $candidate }
}

# ----- State tracking for cleanup -----------------------------------
$script:ServerCreated = $false
$script:RunnerRegistered = $false
$script:WorkflowFailed = $false
$script:CleanupDone = $false

# ----- Cleanup function (called by trap and finally) ----------------
function Invoke-Cleanup {
    param([bool]$WasFailure)

    if ($script:CleanupDone) { return }
    $script:CleanupDone = $true

    if ($script:RunnerRegistered) {
        Write-Host "[CLEANUP] Unregistering GitHub Actions runner..." -ForegroundColor Yellow
        try {
            # Get runner list as JSON, parse in PowerShell (avoids gh
            # --jq quoting issues when invoked via .cmd shim)
            $json = & $GhExe api "repos/$GhRepo/actions/runners"
            $runners = ($json | ConvertFrom-Json).runners |
                       Where-Object { $_.name -eq $RunnerName }
            foreach ($r in $runners) {
                & $GhExe api -X DELETE "repos/$GhRepo/actions/runners/$($r.id)" | Out-Null
                Write-Host "  Removed runner ID $($r.id) from GitHub" -ForegroundColor Gray
            }
            if (-not $runners) {
                Write-Warning "  No runner named '$RunnerName' found on GitHub (may have already been removed)"
            }
        } catch {
            Write-Warning "  Failed to unregister runner: $_"
        }
    }

    if ($script:ServerCreated) {
        if ($WasFailure -and $KeepOnFailure) {
            Write-Host "[CLEANUP] -KeepOnFailure set; leaving server '$ServerName' running for debug." -ForegroundColor Yellow
            Write-Host "  REMEMBER: hcloud server delete $ServerName  when done." -ForegroundColor Red
        } else {
            Write-Host "[CLEANUP] Deleting Hetzner server '$ServerName'..." -ForegroundColor Yellow
            & $HcloudExe server delete $ServerName 2>&1 | Out-Host
        }
    }
}

# Ctrl+C / unhandled exception → cleanup
trap {
    Write-Host ""
    Write-Host "[INTERRUPTED] $($_.Exception.Message)" -ForegroundColor Red
    Invoke-Cleanup -WasFailure $true
    exit 130
}

try {
    # ----- Preflight ------------------------------------------------
    Write-Host "[PREFLIGHT] Checking prerequisites..." -ForegroundColor Cyan

    if (-not (Test-Path $HcloudExe)) {
        throw "hcloud not found at $HcloudExe"
    }
    if (-not $GhExe -or -not (Test-Path $GhExe)) {
        throw "gh CLI not found"
    }
    if (-not (Test-Path "$env:USERPROFILE\.ssh\id_rsa")) {
        throw "SSH private key not found at $env:USERPROFILE\.ssh\id_rsa"
    }

    # Verify hcloud auth
    $existing = & $HcloudExe server list -o json | ConvertFrom-Json
    $stale = $existing | Where-Object { $_.name -eq $ServerName }
    if ($stale) {
        throw "Server '$ServerName' already exists (ID $($stale.id)). Delete it first: hcloud server delete $ServerName"
    }

    # Verify gh auth has workflow scope (needed for dispatch)
    if ($Workflow -ne "none") {
        $scopes = & $GhExe auth status 2>&1 | Select-String "Token scopes" | Select-Object -First 1
        if ($scopes -notmatch "workflow") {
            throw "gh CLI active account missing 'workflow' scope. Run: gh auth refresh -h github.com -s workflow"
        }
    }

    # Ensure SSH key is registered on Hetzner
    $hetznerKeys = & $HcloudExe ssh-key list -o json | ConvertFrom-Json
    if (-not ($hetznerKeys | Where-Object { $_.name -eq $SshKeyName })) {
        Write-Host "  Uploading SSH key '$SshKeyName' to Hetzner..." -ForegroundColor Gray
        & $HcloudExe ssh-key create --name $SshKeyName --public-key-from-file "$env:USERPROFILE\.ssh\id_rsa.pub" | Out-Host
    }

    # ----- Create server -------------------------------------------
    Write-Host "[CREATE] Provisioning $ServerType in $Location..." -ForegroundColor Cyan
    & $HcloudExe server create `
        --name $ServerName `
        --type $ServerType `
        --image ubuntu-22.04 `
        --location $Location `
        --ssh-key $SshKeyName | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "hcloud server create failed" }
    $script:ServerCreated = $true

    $serverIp = & $HcloudExe server ip $ServerName
    Write-Host "  Server IP: $serverIp" -ForegroundColor Green

    # ----- Wait for SSH --------------------------------------------
    Write-Host "[WAIT] Waiting for SSH (up to 90s)..." -ForegroundColor Cyan
    $sshReady = $false
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 3
        $test = ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes `
                    "root@$serverIp" "echo ready" 2>$null
        if ($test -eq "ready") { $sshReady = $true; break }
    }
    if (-not $sshReady) { throw "SSH never came up after 90s" }
    Write-Host "  SSH ready." -ForegroundColor Green

    # ----- Provision build deps ------------------------------------
    Write-Host "[PROVISION] Installing build deps (this takes ~3-5 min)..." -ForegroundColor Cyan
    $provisionScript = @'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# Add Kitware apt repo for current CMake (Ubuntu 22.04 ships 3.22,
# PDFE needs >= 3.25)
apt-get install -qq -y wget gnupg software-properties-common ca-certificates
wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
    | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main" \
    > /etc/apt/sources.list.d/kitware.list
apt-get update -qq

apt-get install -qq -y \
    clang-15 ninja-build cmake git curl zip unzip tar \
    autoconf autoconf-archive automake libtool \
    bison flex gperf perl \
    pkg-config python3 python3-pip python3-venv python3-jinja2 \
    qt6-base-dev qt6-base-dev-tools libqt6test6 \
    libfreetype-dev libharfbuzz-dev \
    libssl-dev libcurl4-openssl-dev \
    zlib1g-dev libpng-dev libjpeg-dev libtiff-dev \
    libopenjp2-7-dev libtesseract-dev libleptonica-dev \
    libpugixml-dev libsqlite3-dev \
    libgl1-mesa-dev libfontconfig1-dev xvfb \
    libxcb*-dev libx11-xcb-dev libglu1-mesa-dev \
    libxrender-dev libxi-dev libxkbcommon-dev \
    libxkbcommon-x11-dev libegl1-mesa-dev libdbus-1-dev \
    libsm-dev libice-dev libatspi2.0-dev libxext-dev \
    libxkbfile-dev libxss-dev libxcursor-dev libxinerama-dev \
    libxrandr-dev libxtst-dev libxv-dev libxxf86vm-dev \
    fonts-liberation fonts-liberation2 \
    fonts-crosextra-carlito fonts-crosextra-caladea \
    fonts-dejavu-core \
    > /tmp/apt.log 2>&1
# Deliberately NOT installing fonts-dejavu-extra: it contains
# "DejaVu Math TeX Gyre" which Qt6 was picking as its substitute
# for Arial / Helvetica / Times / etc. even with fontconfig aliases
# in place. Without that font present, Qt falls back to Liberation
# (the actual metric-compatible family) via the fontconfig alias.
# fonts-croscore is in newer releases; install separately so its
# absence doesn't fail the whole apt run
apt-get install -qq -y fonts-croscore >> /tmp/apt.log 2>&1 || \
    echo "fonts-croscore not available (OK, Liberation covers most)"

# Configure fontconfig aliases so Microsoft font names resolve to
# their metric-compatible Linux equivalents. Without these, Qt's
# font matcher picks "DejaVu Math TeX Gyre" as the fallback for
# unknown families, which breaks PDFE's font selection.
cat > /etc/fonts/conf.d/30-pdfe-msfont-aliases.conf <<'FONTCONF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <!-- Use match/edit assign for ABSOLUTE replacement before pattern
       matching, which is stronger than alias <prefer>. binding="same"
       prevents fontconfig from overriding it in later passes. -->
  <match target="pattern">
    <test name="family"><string>Arial</string></test>
    <edit name="family" mode="assign" binding="same"><string>Liberation Sans</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>Helvetica</string></test>
    <edit name="family" mode="assign" binding="same"><string>Liberation Sans</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>Times New Roman</string></test>
    <edit name="family" mode="assign" binding="same"><string>Liberation Serif</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>Times</string></test>
    <edit name="family" mode="assign" binding="same"><string>Liberation Serif</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>Courier New</string></test>
    <edit name="family" mode="assign" binding="same"><string>Liberation Mono</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>Courier</string></test>
    <edit name="family" mode="assign" binding="same"><string>Liberation Mono</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>Calibri</string></test>
    <edit name="family" mode="assign" binding="same"><string>Carlito</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>Cambria</string></test>
    <edit name="family" mode="assign" binding="same"><string>Caladea</string></edit>
  </match>
</fontconfig>
FONTCONF

# Rebuild the fontconfig cache so the aliases take effect.
# Run as both root and actions user — fontconfig caches per-user.
fc-cache -f >/dev/null 2>&1 || true
sudo -u actions fc-cache -f >/dev/null 2>&1 || true

# Diagnostics so we can see what actually got installed
echo "--- fontconfig diagnostic ---"
fc-list | grep -iE "liberation|arimo|carlito|caladea|cousine|tinos" | head -10 || true
echo "--- fc-match Arial ---"
fc-match Arial || true
echo "--- fc-match Helvetica ---"
fc-match Helvetica || true
echo "--- fc-match Times New Roman ---"
fc-match "Times New Roman" || true
echo "--- fc-match Calibri ---"
fc-match Calibri || true
echo "fontconfig aliases installed"
echo "apt install: done"

useradd -m -s /bin/bash actions || true
echo "actions user: ready"

# Install Rust toolchain (PDFE's Tantivy search bridge needs cargo)
sudo -u actions bash <<'RUST_INNER'
set -e
if [ ! -d ~/.cargo ]; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
fi
RUST_INNER
# Make cargo available system-wide for the workflow steps
ln -sf /home/actions/.cargo/bin/cargo /usr/local/bin/cargo
ln -sf /home/actions/.cargo/bin/rustc /usr/local/bin/rustc
echo "rust: ready ($(rustc --version 2>&1 || echo missing))"

# Pre-install vcpkg as actions user so workflows don't need
# lukka/run-vcpkg (which fails to load on self-hosted runners).
sudo -u actions bash <<'VCPKG_INNER'
set -e
if [ ! -d ~/vcpkg ]; then
    git clone --depth=1 https://github.com/microsoft/vcpkg.git ~/vcpkg
    cd ~/vcpkg
    git fetch --unshallow
    git checkout 15e5f3820f0370f1ba7150853762cec0688cd396
    ./bootstrap-vcpkg.sh -disableMetrics
fi
VCPKG_INNER
echo "vcpkg: ready at /home/actions/vcpkg"
'@
    $tmpScript = [System.IO.Path]::GetTempFileName()
    Set-Content -Path $tmpScript -Value $provisionScript -Encoding utf8
    scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR $tmpScript "root@${serverIp}:/tmp/provision.sh" | Out-Null
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR "root@$serverIp" "bash /tmp/provision.sh" 2>&1 | Out-Host
    Remove-Item $tmpScript -Force
    if ($LASTEXITCODE -ne 0) { throw "Provisioning failed" }

    # ----- Register GitHub Actions runner --------------------------
    Write-Host "[RUNNER] Registering GitHub Actions runner '$RunnerName'..." -ForegroundColor Cyan
    $regToken = (& $GhExe api -X POST "repos/$GhRepo/actions/runners/registration-token" --jq ".token")
    if (-not $regToken) { throw "Failed to fetch runner registration token" }

    $runnerScript = @"
set -euo pipefail
cd /home/actions
sudo -u actions bash <<'INNER'
set -e
mkdir -p ~/actions-runner && cd ~/actions-runner
if [ ! -f config.sh ]; then
    curl -fsSL -o runner.tar.gz https://github.com/actions/runner/releases/download/v2.319.1/actions-runner-linux-x64-2.319.1.tar.gz
    tar xzf runner.tar.gz
    rm runner.tar.gz
fi
./config.sh --unattended \
    --url https://github.com/$GhRepo \
    --token $regToken \
    --name $RunnerName \
    --labels $RunnerLabels \
    --work _work \
    --replace
INNER
cd /home/actions/actions-runner
./svc.sh install actions
# svc.sh start was flaky (printed usage instead of starting); call
# systemctl directly. Use bash `$(...)` for command substitution,
# escaped with backtick-dollar so PowerShell doesn't interpolate.
SVC_UNIT=`$(basename `$(ls /etc/systemd/system/actions.runner.*.service | tail -1))
systemctl daemon-reload
systemctl enable "`$SVC_UNIT"
systemctl start "`$SVC_UNIT"
systemctl is-active "`$SVC_UNIT" || (journalctl -u "`$SVC_UNIT" --no-pager -n 20; exit 1)
echo "Service started: `$SVC_UNIT"
"@
    $tmpRunner = [System.IO.Path]::GetTempFileName()
    Set-Content -Path $tmpRunner -Value $runnerScript -Encoding utf8
    scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR $tmpRunner "root@${serverIp}:/tmp/runner.sh" | Out-Null
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR "root@$serverIp" "bash /tmp/runner.sh" 2>&1 | Out-Host
    Remove-Item $tmpRunner -Force
    if ($LASTEXITCODE -ne 0) { throw "Runner registration failed" }
    $script:RunnerRegistered = $true
    Write-Host "  Runner '$RunnerName' online." -ForegroundColor Green

    # ----- Dispatch workflow (optional) ----------------------------
    if ($Workflow -eq "none") {
        Write-Host "[READY] Runner online. Dispatch a workflow manually:" -ForegroundColor Green
        Write-Host "  gh workflow run <name> -R $GhRepo" -ForegroundColor White
        Write-Host ""
        if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
            Write-Host "Press Enter when done to destroy the server..." -ForegroundColor Yellow
            $null = Read-Host
        } else {
            Write-Host "[NON-INTERACTIVE] Skipping wait; tearing down immediately." -ForegroundColor Yellow
        }
    } else {
        Write-Host "[DISPATCH] Triggering $Workflow..." -ForegroundColor Cyan
        $dispatchArgs = @("workflow", "run", $Workflow, "-R", $GhRepo)
        # Both fuzz.yml and hetzner-sanitizer.yml accept duration_seconds
        if ($Workflow -in @("fuzz.yml", "hetzner-sanitizer.yml")) {
            $dispatchArgs += @("-f", "duration_seconds=$FuzzDuration")
        }
        & $GhExe @dispatchArgs | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "gh workflow run failed" }

        # Find the run ID (most recent for this workflow file)
        Start-Sleep -Seconds 5
        $runId = & $GhExe run list --workflow $Workflow -R $GhRepo --limit 1 --json databaseId --jq ".[0].databaseId"
        Write-Host "  Run ID: $runId" -ForegroundColor Gray
        Write-Host "  Watching: https://github.com/$GhRepo/actions/runs/$runId" -ForegroundColor Gray

        # Poll until completion — parse JSON in PS, not --jq, to avoid
        # PowerShell-to-gh-to-jq string-quoting hell.
        Write-Host "[POLL] Waiting for workflow to finish..." -ForegroundColor Cyan
        $runConclusion = ""
        do {
            Start-Sleep -Seconds 30
            $runJson = & $GhExe run view $runId -R $GhRepo --json status,conclusion | ConvertFrom-Json
            $runStatus = $runJson.status
            $runConclusion = if ($runJson.conclusion) { $runJson.conclusion } else { "-" }
            Write-Host "  $runStatus / $runConclusion" -ForegroundColor Gray
        } while ($runStatus -in @("in_progress", "queued"))

        if ($runConclusion -in @("failure", "cancelled", "timed_out")) {
            $script:WorkflowFailed = $true
            Write-Warning "Workflow ended: $runStatus / $runConclusion"
        } else {
            Write-Host "[DONE] Workflow ended: $runStatus / $runConclusion" -ForegroundColor Green
        }
    }

} finally {
    Invoke-Cleanup -WasFailure $script:WorkflowFailed
}

if ($script:WorkflowFailed) { exit 1 }
exit 0
