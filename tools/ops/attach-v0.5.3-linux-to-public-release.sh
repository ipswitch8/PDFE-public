#!/usr/bin/env bash
# tools/ops/attach-v0.5.3-linux-to-public-release.sh -- put the published
# v0.5.3 Linux packages on the public v0.5.3 release as well.
#
# WHY. The public mirror's front page links to its "Latest" release, v0.5.3,
# which holds only the Windows installer and SBOM. The Linux packages are on a
# separate v0.5.3-linux release that the front page never shows. Releases
# from now on carry every platform (docs/releasing-linux.md section 5); this
# one-off brings v0.5.3 into line.
#
# WHAT. It downloads the seven files exactly as published on the public
# v0.5.3-linux release; it does not use dist/, which has been rebuilt since.
# It checks each package against the SHA-256 printed in that release's notes,
# pinned below, so a changed download stops the run. Then it hands them to
# tools/packaging/publish-linux-release.py with --source-tag v0.5.3-linux:
#   * the helper checks that the local v0.5.3-linux tag is the commit given
#     by --built-from (also v0.5.3-linux);
#   * it uploads the seven files to v0.5.3;
#   * it adds a Linux section to v0.5.3's notes that says the files were
#     built from tag v0.5.3-linux, with that tag's tree as the corresponding
#     source.
# The Windows notes and assets stay as they are, and the v0.5.3-linux
# release is left in place. Nothing is deleted.
#
# Without --apply this is a dry run: it prints the uploads and the notes diff
# and changes nothing.
#
# Usage: tools/ops/attach-v0.5.3-linux-to-public-release.sh [--apply]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PUBLIC=ipswitch8/PDFE-public
APPLY=()
case "${1:-}" in
    --apply) APPLY=(--apply) ;;
    "") ;;
    *) echo "usage: $0 [--apply]" >&2; exit 2 ;;
esac

# From the public v0.5.3-linux release notes ("Checksums").
declare -A EXPECT=(
    [PDFE-0.5.3-x86_64.AppImage]=113af39b98a72293a9ec3b655ff4d4e33e3167b5920dec221f22f6f55ede3387
    [pdfe_0.5.3_amd64.deb]=79ce92673734189f7737e4f3c86adfc0fab30635cf01141b7dfb2e483ade574f
    [pdfe-0.5.3-1.x86_64.rpm]=7dd1374f03256be0631f8a00fbe3a96c1d03bd564e96e76724f92f7ea5384014
    [install.sh]=0d4414dace9c6303952626be3f3e50a18fb098c61514c533f80ebd4071caa7c0
)

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/pdfe-v053-linux.XXXXXX")"
trap 'rm -rf -- "$STAGE"' EXIT

gh release download v0.5.3-linux --repo "$PUBLIC" --dir "$STAGE" \
    --pattern 'PDFE-0.5.3-x86_64.AppImage*' --pattern 'pdfe_0.5.3_amd64.deb*' \
    --pattern 'pdfe-0.5.3-1.x86_64.rpm*' --pattern 'install.sh'
[[ "$(find "$STAGE" -type f | wc -l)" -eq 7 ]] \
    || { echo "ABORT: expected 7 files from v0.5.3-linux" >&2; exit 2; }

for f in "${!EXPECT[@]}"; do
    got="$(sha256sum -- "$STAGE/$f" | cut -d' ' -f1)"
    if [[ "$got" != "${EXPECT[$f]}" ]]; then
        echo "ABORT: $f is $got, but the v0.5.3-linux notes say ${EXPECT[$f]}" >&2
        exit 2
    fi
done
echo "downloaded: 7 files from $PUBLIC v0.5.3-linux; the 4 packages match its published SHA-256 values"

python3 "$REPO_ROOT/tools/packaging/publish-linux-release.py" \
    --tag v0.5.3 --source-tag v0.5.3-linux --built-from v0.5.3-linux \
    --repo "$PUBLIC" --dist "$STAGE" --installer "$STAGE/install.sh" \
    "${APPLY[@]}"
