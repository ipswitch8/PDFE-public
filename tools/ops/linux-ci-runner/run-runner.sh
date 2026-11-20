#!/usr/bin/env bash
# Run the PDFE Linux CI runner container in the foreground (systemd user unit
# pdfe-ci-runner.service calls this; see docs/ci-linux-runner.md).
#
#   tools/ops/linux-ci-runner/run-runner.sh [--build]
#
# --build rebuilds the image from tools/ops/linux-ci-runner/Containerfile first.
# The registration lives in the podman volume pdfe-ci-runner; a registration
# token is fetched with gh only when that volume holds none yet.
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_SLUG="${PDFE_CI_REPO:-ipswitch8/PDFE}"
IMAGE="localhost/pdfe-ci-runner:ubuntu-24.04"
VOLUME="pdfe-ci-runner"
NAME="pdfe-ci-runner"

if [[ "${1:-}" == "--build" ]] || ! podman image exists "$IMAGE"; then
    podman build -q -t "$IMAGE" -f "$HERE/Containerfile" "$HERE"
fi
podman volume exists "$VOLUME" || podman volume create "$VOLUME" >/dev/null

# Registered already? Look inside the volume without starting the runner.
token_env=()
if ! podman run --rm -v "$VOLUME:/runner" --entrypoint /bin/test "$IMAGE" -f /runner/state/.runner; then
    token="$(gh api -X POST "repos/$REPO_SLUG/actions/runners/registration-token" --jq .token)"
    token_env=(-e "RUNNER_REG_TOKEN=$token")
fi

podman rm -f "$NAME" >/dev/null 2>&1 || true
# Limits keep a CI job from starving a release or test build on the same host.
exec podman run --rm --name "$NAME" --hostname pdfe-ci-linux \
    --memory 6g --cpus 4 \
    -v "$VOLUME:/runner" \
    -e RUNNER_REPO_URL="https://github.com/$REPO_SLUG" \
    -e RUNNER_NAME=pdfe-linux-01 -e RUNNER_LABELS=pdfe-linux \
    "${token_env[@]}" \
    "$IMAGE"
