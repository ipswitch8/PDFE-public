#!/usr/bin/env bash
# tools/packaging/release-container-inner.sh — runs INSIDE the Ubuntu 24.04
# container started by build-release-container.sh. Not meant to be run on a
# host: it installs packages with apt-get and expects the mounts that script
# sets up (/work, /vcpkg, /rustup, /cargo, /appimage-tools, /dist).

set -euo pipefail

[[ -f /work/src/.release-commit ]] || {
    echo "error: run tools/packaging/build-release-container.sh, not this script" >&2; exit 2; }

JOBS="${JOBS:-3}"

# Build prerequisites. The same list as docs/building-linux.md section 1, plus
# what a bare image lacks that a desktop host already has:
#   binutils/file           build-appimage.sh's glibc-floor check and linuxdeploy
#   libdbus-1-dev           Qt's D-Bus support (the AppImage bundles libdbus-1)
#   bison flex gperf        vcpkg ports that generate parsers or hash tables
#   autopoint gettext       autotools ports that use gettext macros
#   libltdl-dev             libxcrypt's autoreconf (vcpkg asks for it by name)
#   python3-venv            vcpkg's meson-based ports set up a venv
#   ca-certificates         vcpkg and cargo download over HTTPS
#   libsm-dev libice-dev    X11 session management; qtbase forces its xcb_sm
#                           feature ON and fails to configure without them
#   libatspi2.0-dev and the remaining X11 headers: what the developer host
#                           that built the earlier AppImages had installed, so
#                           the release Qt gets the same features (the AT-SPI
#                           accessibility bridge in particular)
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl zip unzip tar \
    nasm libtool-bin autoconf autoconf-archive automake python3 python3-venv \
    bison flex gperf autopoint gettext libltdl-dev ca-certificates binutils file \
    '^libxcb.*-dev' libxcb-xinput-dev libx11-xcb-dev libxrender-dev libxi-dev \
    libxkbcommon-dev libxkbcommon-x11-dev libgl1-mesa-dev libglu1-mesa-dev \
    libegl1-mesa-dev libcups2-dev libdbus-1-dev \
    libsm-dev libice-dev libatspi2.0-dev libxext-dev libxfixes-dev libxrandr-dev \
    libxcursor-dev libxinerama-dev libxcomposite-dev libxdamage-dev libxtst-dev \
    libegl-dev libglx-dev libudev-dev \
    >/dev/null
echo "glibc: $(ldd --version | head -n1)"
echo "gcc:   $(gcc --version | head -n1)"
echo "cmake: $(cmake --version | head -n1)"

# vcpkg is the container's own clone (build-release-container.sh). rustup and
# cargo are the host's, READ-ONLY; cargo's registry cache goes under /work.
export VCPKG_ROOT=/work/vcpkg
export VCPKG_DOWNLOADS=/work/vcpkg-downloads
export RUSTUP_HOME=/rustup CARGO_HOME=/work/cargo-home
mkdir -p "$VCPKG_DOWNLOADS" "$CARGO_HOME"
export PATH="/cargo/bin:$PATH"
# A binary cache of the container's own. The host's cache holds packages built
# under the host's paths, and restoring one would put those paths back.
mkdir -p /work/vcpkg-cache
export VCPKG_BINARY_SOURCES="clear;files,/work/vcpkg-cache,readwrite"
export VCPKG_MAX_CONCURRENCY="$JOBS"
export VCPKG_DISABLE_METRICS=1
# Keep vcpkg's scratch trees off the shared vcpkg checkout, so a container
# build never touches the host's buildtrees/ or packages/.
VCPKG_OPTS="--x-buildtrees-root=/work/vcpkg-buildtrees;--x-packages-root=/work/vcpkg-packages;--clean-buildtrees-after-build;--clean-packages-after-build"

cd /work/src
echo "building commit $(cat .release-commit)"
# Always a fresh build tree: a release is configured from nothing, and a cache
# left by an earlier run can point at paths this run no longer mounts.
rm -rf /work/build-release
# x64-linux-release is vcpkg's own community triplet: x64-linux (static
# libraries, dynamic CRT) with VCPKG_BUILD_TYPE=release, so no debug variant
# of any dependency is built. The release links only release libraries, and
# the debug half would double a from-scratch dependency build. The host
# triplet is set too, or vcpkg resolves a second, x64-linux chain for the
# host dependencies (gtest).
cmake --preset linux-gcc -B /work/build-release \
    -DPDFE_TESTING=OFF \
    -DVCPKG_TARGET_TRIPLET=x64-linux-release \
    -DVCPKG_HOST_TRIPLET=x64-linux-release \
    -DVCPKG_INSTALLED_DIR=/work/vcpkg_installed \
    "-DVCPKG_INSTALL_OPTIONS=$VCPKG_OPTS"
cmake --build /work/build-release --config Release --target pdfe pdfe_preflight -j"$JOBS"

bash tools/packaging/build-appimage.sh \
    --build-dir /work/build-release --out-dir /dist --tools-dir /appimage-tools
