#!/usr/bin/env bash
# tools/packaging/build-appimage.sh — build PDFE-<version>-x86_64.AppImage
# from an existing Release build (GitHub #390).
#
# Usage:
#   tools/packaging/build-appimage.sh [--build-dir DIR] [--out-dir DIR] [--tools-dir DIR]
#
#   --build-dir  CMake build tree holding bin/Release/{pdfe,pdfe_preflight}
#                (default: build)
#   --out-dir    where the AppImage and its .sha256 are written (default: dist)
#   --tools-dir  where linuxdeploy-x86_64.AppImage and appimagetool-x86_64.AppImage
#                live (default: $PDFE_APPIMAGE_TOOLS or ~/.cache/pdfe-appimage-tools)
#
# Re-runnable: the AppDir is rebuilt from scratch on every run and the output
# is replaced. Nothing is downloaded; fetch the two tools yourself and check
# their published checksums (see docs/releasing-linux.md).
#
# What ends up inside:
#   usr/bin/pdfe, usr/bin/pdfe_preflight  (the preflight helper must sit next
#                                          to pdfe: PreflightOpen.cpp looks for
#                                          it in the executable's directory)
#   usr/lib/*                             every shared library linuxdeploy
#                                          does not leave to the host (its
#                                          excludelist: glibc, libGL, X11,
#                                          fontconfig, ...), e.g. libdbus-1
#   usr/share/applications/pdfe.desktop, usr/share/icons/.../pdfe.png
# Qt is linked statically (vcpkg x64-linux), including its platform, TLS,
# image-format and CUPS print plugins, so no Qt plugin deployment is needed.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO/build"
OUT_DIR="$REPO/dist"
TOOLS_DIR="${PDFE_APPIMAGE_TOOLS:-$HOME/.cache/pdfe-appimage-tools}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$(readlink -f -- "${2:?--build-dir needs a value}")"; shift 2 ;;
        --out-dir)   OUT_DIR="$(readlink -m -- "${2:?--out-dir needs a value}")"; shift 2 ;;
        --tools-dir) TOOLS_DIR="$(readlink -f -- "${2:?--tools-dir needs a value}")"; shift 2 ;;
        -h|--help)   sed -n '2,27p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1' (see --help)" >&2; exit 2 ;;
    esac
done

BIN="$BUILD_DIR/bin/Release"
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
for f in "$BIN/pdfe" "$BIN/pdfe_preflight"; do
    [[ -x "$f" ]] || { echo "error: $f not found; build the Release targets first" >&2; exit 3; }
done
for f in "$LINUXDEPLOY" "$APPIMAGETOOL"; do
    [[ -x "$f" ]] || { echo "error: $f not found or not executable (see docs/releasing-linux.md)" >&2; exit 3; }
done

# The version the binary reports is the version the file name carries, so the
# two cannot disagree.
VERSION="$(QT_QPA_PLATFORM=offscreen "$BIN/pdfe" --version 2>/dev/null | awk '{print $NF; exit}')"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.]+)?$ ]] || {
    echo "error: could not read a version from '$BIN/pdfe --version' (got '$VERSION')" >&2; exit 4; }

WORK="$BUILD_DIR/appimage"
APPDIR="$WORK/AppDir"
rm -rf -- "$WORK"
mkdir -p -- "$APPDIR/usr/bin" "$OUT_DIR"

install -m 0755 -- "$BIN/pdfe" "$APPDIR/usr/bin/pdfe"
install -m 0755 -- "$BIN/pdfe_preflight" "$APPDIR/usr/bin/pdfe_preflight"

# The FUSE-less extraction mode, so the build also works where /dev/fuse is
# unavailable (containers, CI).
export APPIMAGE_EXTRACT_AND_RUN=1
# linuxdeploy must not find libraries through the build's RUNPATH into the
# vcpkg tree and then leave that RUNPATH in place; it rewrites RUNPATH on
# everything it deploys to $ORIGIN-relative paths.
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/pdfe" \
    --executable "$APPDIR/usr/bin/pdfe_preflight" \
    --desktop-file "$REPO/packaging/linux/pdfe.desktop" \
    --icon-file "$REPO/packaging/linux/pdfe.png"

# The oldest glibc the bundle runs on is the newest GLIBC_x.y symbol version
# any ELF in it needs. install.sh refuses older systems with that number, so
# the two must agree; a newer build host changes it, and that has to be a
# deliberate edit to install.sh, not a surprise for users.
# `|| true`: a file with no GLIBC_ symbols (AppRun, a script) makes grep exit 1.
# Under pipefail that status would, if it came last, fail the assignment and
# set -e would end the build with no message.
NEEDED_GLIBC="$(find "$APPDIR" -type f -perm -u+x -o -type f -name '*.so*' | while read -r f; do
    objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' || true; done | sed 's/GLIBC_//' | sort -uV | tail -n1)"
INSTALLER_GLIBC="$(sed -n 's/^MIN_GLIBC="\(.*\)"$/\1/p' "$REPO/packaging/linux/install.sh")"
if [[ -z "$NEEDED_GLIBC" || "$NEEDED_GLIBC" != "$INSTALLER_GLIBC" ]]; then
    echo "error: the bundle needs glibc ${NEEDED_GLIBC:-?}, but packaging/linux/install.sh says MIN_GLIBC=\"${INSTALLER_GLIBC:-?}\"" >&2
    echo "       update MIN_GLIBC in install.sh (and the release notes) to match" >&2
    exit 5
fi
echo "glibc floor: $NEEDED_GLIBC (matches install.sh)"

OUT="$OUT_DIR/PDFE-$VERSION-x86_64.AppImage"
rm -f -- "$OUT" "$OUT.sha256"
ARCH=x86_64 VERSION="$VERSION" "$APPIMAGETOOL" --no-appstream "$APPDIR" "$OUT"
( cd -- "$OUT_DIR" && sha256sum -- "$(basename -- "$OUT")" > "$(basename -- "$OUT").sha256" )
echo "built: $OUT"
cat -- "$OUT.sha256"
