#!/usr/bin/env bash
# Entry point of the PDFE Linux CI runner container.
#
# The runner's registration (.runner, .credentials*) lives in the /runner/state
# volume so it survives container restarts and image rebuilds; the job work
# directory is /runner/work. On first start, or after `config.sh remove`, the
# runner registers with RUNNER_REG_TOKEN (a one-hour registration token from
# `gh api -X POST repos/<repo>/actions/runners/registration-token`).
set -euo pipefail
: "${RUNNER_REPO_URL:?RUNNER_REPO_URL is required}"
: "${RUNNER_NAME:=pdfe-linux-01}"
: "${RUNNER_LABELS:=pdfe-linux}"
STATE=/runner/state
mkdir -p "$STATE" /runner/work
for f in .runner .credentials .credentials_rsaparams; do
    if [[ -f "$STATE/$f" ]]; then cp -f "$STATE/$f" "/opt/actions-runner/$f"; fi
done
if [[ ! -f /opt/actions-runner/.runner ]]; then
    : "${RUNNER_REG_TOKEN:?not registered yet and no RUNNER_REG_TOKEN given}"
    ./config.sh --unattended --url "$RUNNER_REPO_URL" --token "$RUNNER_REG_TOKEN" \
        --name "$RUNNER_NAME" --labels "$RUNNER_LABELS" --work /runner/work --replace
    for f in .runner .credentials .credentials_rsaparams; do
        cp -f "/opt/actions-runner/$f" "$STATE/$f"
    done
fi
unset RUNNER_REG_TOKEN
exec ./run.sh
