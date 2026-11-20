#!/usr/bin/env bash
# Create or rotate the PDFE_HYGIENE_PROBE Actions secret (GitHub #407).
#
#   tools/ops/linux-ci-runner/set-hygiene-probe-secret.sh [--repo owner/name]
#
# check_tracker_hygiene.py's sensitivity control needs a string the needle
# inventory matches, to prove the scanner can see a real identifier before it
# reports "clean". On the Linux CI runner the container's own identity is
# deliberately not a needle, so the probe comes from this secret instead.
#
# The value is the build account's home path, built from the login of whoever
# runs this script on the build host, and piped straight into `gh secret set`.
# It is never printed, written to a file or passed on a command line. The script
# then confirms the value hits the needle inventory locally, without printing
# it, so a wrong value is caught here rather than as a red CI step.
set -euo pipefail
REPO="ipswitch8/PDFE"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo) REPO="${2:?--repo needs a value}"; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../../.." && pwd)"

probe() { printf 'built in %s/src here' "$HOME"; }

# Refuse to store a value the scanner would not detect.
if ! probe | python3 "$HERE/probe-hits-needle.py" "$ROOT"; then
    echo "error: this account's home path matches no needle; refusing to store it" >&2
    exit 3
fi
probe | gh secret set PDFE_HYGIENE_PROBE --repo "$REPO"
gh secret list --repo "$REPO" | grep -q '^PDFE_HYGIENE_PROBE' \
    && echo "PDFE_HYGIENE_PROBE is set on $REPO"
