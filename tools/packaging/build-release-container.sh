#!/usr/bin/env bash
# tools/packaging/build-release-container.sh — build the Linux release binaries
# and the AppImage inside a clean Ubuntu 24.04 container (rootless podman).
#
# Usage:
#   tools/packaging/build-release-container.sh [--work-dir DIR] [--tools-dir DIR] [--jobs N]
#
#   --work-dir   host directory for the container's build state: the exported
#                source, vcpkg buildtrees, the installed tree, the binary cache
#                and the release build tree
#                (default: ~/.cache/pdfe-release-container)
#   --tools-dir  directory holding linuxdeploy-x86_64.AppImage and
#                appimagetool-x86_64.AppImage (default: $PDFE_APPIMAGE_TOOLS or
#                ~/.cache/pdfe-appimage-tools)
#   --jobs       parallel compile jobs (default: 3)
#
# Output: dist/PDFE-<version>-x86_64.AppImage and its .sha256, built by
# tools/packaging/build-appimage.sh inside the container from the committed
# HEAD. Uncommitted changes are not built. The full build transcript is
# written to <work-dir>/release-build.log. The finished AppImage is then
# scanned with tools/gate/check_release_asset_hygiene.py --paths, and the
# script fails if any build-host path or account name is found in it.
#
# WHY A CONTAINER, NOT THE DEVELOPER'S MACHINE
#
# 1. Build paths end up in the binary. Static libraries carry __FILE__ from
#    their assertion macros (tesseract, libarchive, libjpeg-turbo, ...), and
#    OpenSSL compiles its install prefix in as ENGINESDIR / MODULESDIR. A host
#    build measured 199 strings in `pdfe` naming the build account's home
#    directory (v0.5.3, GitHub #390). In the container everything lives under
#    /work and /vcpkg, so those strings name nothing about who built it. The
#    vcpkg binary cache is separate (/work/vcpkg-cache), because a package
#    restored from the host's cache would bring the host's paths back with it.
# 2. The glibc floor is the build system's. Ubuntu 24.04 ships glibc 2.39, so
#    the bundle's floor is 2.39 by construction rather than because nothing
#    happened to use a 2.40 symbol. build-appimage.sh still measures it and
#    refuses to disagree with install.sh.
# 3. Every package installed is listed in release-container-inner.sh. A host
#    build quietly picks up whatever the host has, which is how
#    libjpeg-turbo8-dev became a hidden link dependency once (GitHub #388).
#
# The container builds the COMMITTED tree, exported fresh on every run, so the
# release is the commit and not whatever the working tree held. The export also
# gives the cargo target directory (rust/tantivy_bridge/target) a home of its
# own instead of sharing the developer build's.
#
# vcpkg: the container gets its own clone of the host's vcpkg checkout, at the
# same commit, in --work-dir. vcpkg takes a lock file in its root, so it can't
# run from a read-only mount, and a read-write mount would let the build write
# into the developer's checkout.
# The Rust toolchain is mounted READ-ONLY at /rustup and /cargo, and cargo's
# registry goes under /work, so the build never writes into the developer's
# trees.
# src/search/CMakeLists.txt remaps CARGO_HOME out of the Rust panic paths, so
# that location doesn't reach the binary either.
#
# Re-runnable: the vcpkg binary cache persists in --work-dir, so a second run
# rebuilds only what changed.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
WORK_DIR="$HOME/.cache/pdfe-release-container"
TOOLS_DIR="${PDFE_APPIMAGE_TOOLS:-$HOME/.cache/pdfe-appimage-tools}"
JOBS=3
IMAGE="docker.io/library/ubuntu:24.04"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --work-dir)  WORK_DIR="$(readlink -m -- "${2:?--work-dir needs a value}")"; shift 2 ;;
        --tools-dir) TOOLS_DIR="$(readlink -f -- "${2:?--tools-dir needs a value}")"; shift 2 ;;
        --jobs)      JOBS="${2:?--jobs needs a value}"; shift 2 ;;
        -h|--help)   sed -n '2,58p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1' (see --help)" >&2; exit 2 ;;
    esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "error: --jobs must be a positive integer" >&2; exit 2; }

VCPKG_ROOT_HOST="${VCPKG_ROOT:-$HOME/vcpkg}"
RUSTUP_HOST="${RUSTUP_HOME:-$HOME/.rustup}"
CARGO_HOST="${CARGO_HOME:-$HOME/.cargo}"
for d in "$VCPKG_ROOT_HOST" "$RUSTUP_HOST" "$CARGO_HOST"; do
    [[ -d "$d" ]] || { echo "error: $d not found" >&2; exit 3; }
done
[[ -x "$VCPKG_ROOT_HOST/vcpkg" ]] || { echo "error: $VCPKG_ROOT_HOST/vcpkg is not bootstrapped" >&2; exit 3; }
for f in linuxdeploy-x86_64.AppImage appimagetool-x86_64.AppImage; do
    [[ -x "$TOOLS_DIR/$f" ]] || { echo "error: $TOOLS_DIR/$f not found or not executable" >&2; exit 3; }
done
command -v podman >/dev/null 2>&1 || { echo "error: podman is required" >&2; exit 3; }

VCPKG_COMMIT="$(git -C "$VCPKG_ROOT_HOST" rev-parse HEAD)"
if [[ ! -d "$WORK_DIR/vcpkg/.git" ]]; then
    git clone --quiet --no-checkout -- "$VCPKG_ROOT_HOST" "$WORK_DIR/vcpkg"
fi
git -C "$WORK_DIR/vcpkg" fetch --quiet -- "$VCPKG_ROOT_HOST" "$VCPKG_COMMIT"
git -C "$WORK_DIR/vcpkg" checkout --quiet --force "$VCPKG_COMMIT"
# The bootstrapped vcpkg tool is a static binary; reuse the host's. vcpkg.cmake
# re-bootstraps whenever scripts/vcpkg-tool-metadata.txt is newer than the
# tool, and the checkout above just rewrote that file, so the copy is touched
# after it.
cp -- "$VCPKG_ROOT_HOST/vcpkg" "$WORK_DIR/vcpkg/vcpkg"
touch -- "$WORK_DIR/vcpkg/vcpkg"

COMMIT="$(git -C "$REPO" rev-parse HEAD)"
if [[ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]]; then
    echo "note: the working tree has uncommitted changes; building HEAD ($COMMIT) without them" >&2
fi
mkdir -p -- "$WORK_DIR" "$REPO/dist"
rm -rf -- "$WORK_DIR/src"
mkdir -p -- "$WORK_DIR/src"
git -C "$REPO" archive --format=tar "$COMMIT" | tar -x -C "$WORK_DIR/src"
printf '%s\n' "$COMMIT" > "$WORK_DIR/src/.release-commit"

# Rootless podman maps the container's root to the invoking user, so what the
# container writes into the mounted directories is owned by that user.
# The recipe (the inner script) comes from beside this script; the source it
# builds is the export above.
cp -- "$SCRIPT_DIR/release-container-inner.sh" "$WORK_DIR/release-container-inner.sh"
# The container is removed on exit (--rm), which takes `podman logs` with it,
# so the full transcript is kept here. pipefail makes the build's exit status
# the script's, not tee's.
LOG="$WORK_DIR/release-build.log"
echo "build log: $LOG" >&2
podman run --rm \
    --security-opt label=disable \
    -v "$WORK_DIR:/work" \
    -v "$RUSTUP_HOST:/rustup:ro" \
    -v "$CARGO_HOST:/cargo:ro" \
    -v "$TOOLS_DIR:/appimage-tools:ro" \
    -v "$REPO/dist:/dist" \
    -e JOBS="$JOBS" \
    -w /work/src \
    "$IMAGE" bash ../release-container-inner.sh 2>&1 | tee -- "$LOG"

# The point of the container is that no build-host path reaches the release,
# so prove it here rather than trusting the recipe: the hygiene scanner unpacks
# the AppImage and fails (non-zero, and set -e stops us) on any hit for the
# needle inventory or the live login name.
APPIMAGE_NAME="$(sed -n 's#^built: /dist/##p' -- "$LOG" | tail -n 1)"
[[ -n "$APPIMAGE_NAME" && -f "$REPO/dist/$APPIMAGE_NAME" ]] \
    || { echo "error: the build log names no AppImage in dist/" >&2; exit 4; }
python3 "$REPO/tools/gate/check_release_asset_hygiene.py" --paths \
    "$REPO/dist/$APPIMAGE_NAME" "$REPO/dist/$APPIMAGE_NAME.sha256"
