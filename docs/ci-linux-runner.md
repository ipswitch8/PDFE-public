# Self-hosted Linux CI runner

The `fast-checks` job of `.github/workflows/fast-checks.yml` runs on every push
and PR to master. It runs on a self-hosted Linux runner, inside a rootless
podman container on the Linux build host.

**Why self-hosted.** Since 2026-11-16, GitHub has refused to start jobs on its
hosted runners for this account. The job annotation reads "recent account
payments have failed or your spending limit needs to be increased".
Self-hosted runners are not billed. The Windows jobs are separate: they use the
Windows runners (`pdfe-windows`).

**Why a container.** The first version of this runner ran straight on the host.
Every path in its job logs was then under the build account's home directory,
so the account name appeared hundreds of times per log. GitHub keeps those
logs, and the lane's own check "Recent Actions run logs carry no scrubbed
identifier" exists to stop exactly that. Inside the container, paths are
`/home/runner` and `/runner`, the host name is `pdfe-ci-linux`, and a job
cannot read or write the host's files.

| | |
|---|---|
| Runner name | `pdfe-linux-01` |
| Labels | `self-hosted`, `Linux`, `X64`, `pdfe-linux` |
| Image | `localhost/pdfe-ci-runner:ubuntu-24.04`, built from `tools/ops/linux-ci-runner/Containerfile` |
| What the image adds | the Actions runner, pwsh, gh, git, Python 3.12, tesseract, cmake, ninja and g++. The runner, pwsh and gh downloads are pinned and checked against their publishers' SHA-256. |
| State | podman volume `pdfe-ci-runner`. It holds the registration (`/runner/state`) and the job work directory (`/runner/work`). |
| Limits | 4 CPUs and 6 GB of memory, so a CI job cannot starve a release or test build on the same host |
| Service | systemd **user** unit `pdfe-ci-runner.service`, which runs `tools/ops/linux-ci-runner/run-runner.sh`. Lingering is enabled, so it runs without a login session. |
| Scope | the private `ipswitch8/PDFE` repository only |

## Operating it

```
systemctl --user status pdfe-ci-runner       # state
journalctl --user -u pdfe-ci-runner -f       # live log
systemctl --user restart pdfe-ci-runner
```

The unit file, `~/.config/systemd/user/pdfe-ci-runner.service`:

```
[Unit]
Description=PDFE Linux CI runner (GitHub Actions, rootless podman container)
After=network-online.target

[Service]
ExecStart=%h/PDFE/tools/ops/linux-ci-runner/run-runner.sh
ExecStop=/usr/bin/podman stop -t 60 pdfe-ci-runner
Restart=always
RestartSec=15
TimeoutStopSec=90

[Install]
WantedBy=default.target
```

**Updating the runner, pwsh or gh.**

1. Change the version and SHA-256 `ARG`s in the `Containerfile`.
2. Run `tools/ops/linux-ci-runner/run-runner.sh --build` once, or stop the service
   and rebuild the image.
3. Restart the service.

The registration survives because it lives in the volume.

**Re-registering.** Stop the service and run `podman volume rm pdfe-ci-runner`.
Remove the old `pdfe-linux-01` entry under the repository's Settings → Actions →
Runners. Then start the service. `run-runner.sh` fetches a new registration token
with `gh` whenever the volume holds no registration.

## Hygiene scans on this runner (GitHub #407)

Two fast-checks steps scan text on GitHub against the hashed needle inventory:

- "Tracker text carries no scrubbed identifier" (`check_tracker_hygiene.py --tracker`);
- "Recent Actions run logs carry no scrubbed identifier" (`--logs 25`).

**The sensitivity control.** Before either scan may report "clean", it must
prove it can see a real protected identifier. On Windows the probe is the live
host name (`COMPUTERNAME`), which is in the inventory. This runner has no
`COMPUTERNAME`, and its container login (`runner`) is deliberately not a needle,
so until GitHub #407 both steps failed closed on every run.

**Options considered:**

| option | verdict |
|---|---|
| Probe from a masked Actions secret (`PDFE_HYGIENE_PROBE`) | **Chosen.** The probe is a real needle. GitHub masks it in every log, and nothing in the tree holds it. If the secret is missing or short, the check fails closed, so a misconfiguration can't pass. |
| A synthetic needle planted in a temporary inventory | Rejected. It proves the matcher works, which the self-tests already do, but not that the *real* inventory loaded and covers the real identifier. That is the half-loaded-inventory failure GitHub #146/#277 describe. |
| The runner's login or host name, as on the developer host | Rejected for CI. Inside the container these are deliberately neutral, so the control could never pass. It stays as the fallback for local runs on the build host, where the login *is* a needle. |
| Exempting the Linux job from the control | Rejected. An uncontrolled "clean" is not evidence. |

**How `check_tracker_hygiene.py` picks its probe** (`live_probe()`):

1. **Windows:** `COMPUTERNAME`, unchanged.
2. **Elsewhere, if `PDFE_HYGIENE_PROBE` is set:** that value. Set but shorter
   than 4 characters is a failure; it never quietly falls through to the next
   source.
3. **Otherwise:** the login name in a home-directory path. If that matches no
   needle, the control FAILS and says to supply the secret.

The probe is never printed. The tests pin this, along with every branch above.

**Threat model.** The secret holds the build account's home path, an identifier
the project treats as private.

- A GitHub repository secret is encrypted at rest, and only workflows of this
  private repo can read it.
- The runner masks it in logs, and the logs scan would flag a leak if masking
  ever failed.
- It is never in the tree. `set-hygiene-probe-secret.sh` builds the value from
  `$HOME` and pipes it straight to `gh secret set`. Before storing anything, it
  checks the value really hits the inventory (`probe-hits-needle.py`), so a
  useless value is refused there rather than turning CI red.

**Operator step,** once and whenever the account changes:

```
tools/ops/linux-ci-runner/set-hygiene-probe-secret.sh
```

## Security

The runner executes workflow code on the machine that builds releases. The
container keeps the job away from the host's files, but the following still
apply:

- **Private repositories only.** Never register it on a public repository,
  including `ipswitch8/PDFE-public`. A pull request from a fork would run
  arbitrary code here.
- **Short jobs only.** Keep the jobs it takes short and toolchain-free. Release
  builds still go through `tools/packaging/build-release-container.sh` by hand
  (see [releasing-linux.md](releasing-linux.md)), not through CI.
