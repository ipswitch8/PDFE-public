# Hetzner Self-Hosted Runner Setup

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Purpose:** Provision a Hetzner Cloud VM as a GitHub Actions self-hosted
runner for extended ASan/UBSan/libFuzzer campaigns that exceed GitHub's
free-tier minute limits.

## Server Specification

| Use case | Server type | Cost | Specs |
|---|---|---|---|
| **Minimum** | CX31 | ~€8/month | 2 vCPU, 8 GB RAM, 80 GB disk |
| **Recommended** | CX41 | ~€16/month | 4 vCPU, 16 GB RAM, 160 GB disk |
| **Extended fuzz** | CX51 | ~€32/month | 8 vCPU, 32 GB RAM, 240 GB disk |

For 24-hour libFuzzer campaigns across 7 harnesses, the CX41 is the
sweet spot. ASan overhead is ~2-3x CPU + memory.

## Provisioning (Hetzner Cloud)

```bash
# Set API token
export HCLOUD_TOKEN="your-token-here"

# Create server
hcloud server create \
  --name pdfe-fuzz-runner \
  --type cx41 \
  --image ubuntu-22.04 \
  --location nbg1 \
  --ssh-key your-key-name

# Get IP
hcloud server ip pdfe-fuzz-runner
```

## Server Setup

SSH in as root and run:

```bash
#!/bin/bash
# Provision PDFE fuzz runner

set -euxo pipefail

# System update
apt-get update
apt-get upgrade -y

# Build tools
apt-get install -y \
  clang-15 lldb-15 lld-15 \
  ninja-build cmake git curl \
  pkg-config python3 python3-pip

# Qt6 dev libs
apt-get install -y \
  qt6-base-dev qt6-base-dev-tools \
  libqt6test6 libqt6opengl6-dev \
  libgl1-mesa-dev libfontconfig1-dev

# PDF library deps
apt-get install -y \
  libfreetype6-dev libharfbuzz-dev \
  libssl-dev libcurl4-openssl-dev \
  zlib1g-dev libpng-dev libjpeg-dev libtiff-dev \
  libopenjp2-7-dev libtesseract-dev libleptonica-dev \
  libpugixml-dev libsqlite3-dev

# Virtual display for offscreen rendering tests
apt-get install -y xvfb

# vcpkg
cd /opt
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
echo 'export VCPKG_ROOT=/opt/vcpkg' >> /etc/profile.d/vcpkg.sh
echo 'export PATH=$VCPKG_ROOT:$PATH' >> /etc/profile.d/vcpkg.sh

# Create runner user
useradd -m -s /bin/bash actions
usermod -aG sudo actions

# Install GitHub Actions runner
su - actions -c '
  mkdir -p ~/actions-runner && cd ~/actions-runner
  curl -o actions-runner.tar.gz -L \
    https://github.com/actions/runner/releases/latest/download/actions-runner-linux-x64-2.317.0.tar.gz
  tar xzf actions-runner.tar.gz
  rm actions-runner.tar.gz
'
```

## Register Runner with GitHub

On the Hetzner server, as the `actions` user:

```bash
cd ~/actions-runner

# Get registration token from:
# https://github.com/ipswitch8/PDFE/settings/actions/runners/new
# (Requires admin access to the repo)
export RUNNER_TOKEN="<paste-token-here>"

./config.sh \
  --url https://github.com/ipswitch8/PDFE \
  --token $RUNNER_TOKEN \
  --name pdfe-hetzner-fuzz \
  --labels self-hosted,linux,hetzner,sanitizer \
  --work _work \
  --unattended

# Install as systemd service
sudo ./svc.sh install
sudo ./svc.sh start
```

## Verify Runner Is Online

In GitHub:
1. Go to https://github.com/ipswitch8/PDFE/settings/actions/runners
2. Verify `pdfe-hetzner-fuzz` shows as "Idle" (green)

## Trigger Test Workflow

```bash
# From your local machine
gh workflow run hetzner-sanitizer.yml -F duration_minutes=10
```

Watch the run at https://github.com/ipswitch8/PDFE/actions

## Cost Management

The CX41 server is ~€16/month if always-on. To reduce costs:

### Option A: Schedule-based start/stop

```bash
# Start before scheduled run
hcloud server poweron pdfe-fuzz-runner

# Stop after run completes (or at end of week)
hcloud server poweroff pdfe-fuzz-runner
```

Wire this to a tiny GitHub Actions workflow that uses `hcloud-cli` to
manage server lifecycle.

### Option B: Ephemeral runners

Use [Hetzner's runner-on-demand pattern](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners-with-actions-runner-controller):
spawn a fresh runner per job, destroy after. More setup but pay-per-use.

## Security

- **Firewall:** Allow only outbound HTTPS (for GitHub Actions polling).
  No inbound except SSH (key-only).
- **Secrets:** Store HCLOUD_TOKEN as a GitHub Actions secret, not in
  the runner image.
- **Updates:** `unattended-upgrades` package keeps the OS patched.
- **Runner isolation:** The `actions` user runs the runner as
  non-root; jobs cannot escalate.

## Tearing Down

```bash
# Unregister from GitHub
su - actions -c 'cd ~/actions-runner && ./config.sh remove'

# Destroy server
hcloud server delete pdfe-fuzz-runner
```
