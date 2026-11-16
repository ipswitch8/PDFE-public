#!/usr/bin/env bash
# tools/packaging/build-linux-packages.sh — build the .deb and .rpm of PDFE
# from the AppImage's AppDir (GitHub #390).
#
# Usage:
#   tools/packaging/build-linux-packages.sh (--appdir DIR | --appimage FILE)
#       [--out-dir DIR] [--formats deb,rpm] [--rpm-image IMAGE]
#
#   --appdir     an AppDir as tools/packaging/build-appimage.sh leaves it
#                (<build-dir>/appimage/AppDir)
#   --appimage   a built PDFE AppImage; its squashfs image is unpacked with
#                unsquashfs (the AppImage file itself is not run). In both
#                modes the unpacked usr/bin/pdfe IS run, once, with
#                --version, to read the version, so only give this script
#                the project's own build output.
#   --out-dir    where the packages and their .sha256 files go (default: dist)
#   --formats    comma-separated subset of deb,rpm (default: deb,rpm)
#   --rpm-image  base image for rpmbuild (default: docker.io/library/fedora:41)
#
# Output: pdfe_<version>_amd64.deb and pdfe-<version>-1.x86_64.rpm, each with
# a .sha256 in sha256sum format.
#
# WHAT THE PACKAGES HOLD
#
# The AppDir's usr/ tree, unchanged, under /opt/pdfe. That's the same pdfe and
# pdfe_preflight binaries the AppImage ships, and the same bundled libraries.
# linuxdeploy already set every RUNPATH to $ORIGIN/../lib, so the tree works
# from any directory and needs no system copy of those libraries.
#
#   /opt/pdfe/bin/pdfe, /opt/pdfe/bin/pdfe_preflight   (side by side: the
#                                    preflight sandbox looks for its helper
#                                    next to the executable)
#   /opt/pdfe/lib/*.so*              the bundled libraries
#   /opt/pdfe/share/doc/*            their copyright files
#   /opt/pdfe/share/pdfe/{LICENSE,CORRESPONDING-SOURCE.txt}  (always
#                                    installed, unlike /usr/share/doc)
#   /usr/bin/pdfe -> ../../opt/pdfe/bin/pdfe  (relative)
#   /usr/share/applications/pdfe.desktop   (opens application/pdf)
#   /usr/share/icons/hicolor/256x256/apps/pdfe.png
#   /usr/share/doc/pdfe/{copyright,LICENSE,CORRESPONDING-SOURCE.txt}
#
# /usr/bin/pdfe is a symlink, not a wrapper script, because PDFE finds itself
# through /proc/self/exe. That resolves the link, so the executable directory
# is /opt/pdfe/bin and the preflight helper is found.
#
# Dependencies: the runtime packages packaging/linux/install.sh installs for
# the AppImage, minus FUSE, since nothing is mounted. tests/packaging pins the
# two lists together. There are NO maintainer scripts: Debian and Fedora
# refresh the desktop and icon caches with file triggers on their own, so
# installing PDFE runs no code as root.
#
# The .deb is built here with dpkg-deb. The .rpm is built with rpmbuild in a
# Fedora container (rootless podman), because rpmbuild is not a Debian/Ubuntu
# tool. In the .rpm, automatic dependency generation is off (the bundled
# libraries are private and must not appear as provides or requires), as are
# stripping and debuginfo extraction, so the binaries are byte-identical to
# the AppImage's. Both payloads are xz-compressed, which
# tools/gate/check_release_asset_hygiene.py reads with the standard library
# on any platform.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
APPDIR=""
APPIMAGE=""
OUT_DIR="$REPO/dist"
FORMATS="deb,rpm"
RPM_BASE_IMAGE="docker.io/library/fedora:41"

# Runtime dependencies. Keep in step with the apt-get and dnf lists in
# packaging/linux/install.sh (minus FUSE); tests/packaging checks that.
DEB_DEPENDS=(fontconfig fonts-liberation fonts-dejavu-core libxkbcommon-x11-0
             libxcb-cursor0 libgl1 libegl1 libcups2 libopengl0 libsm6 libice6)
RPM_REQUIRES=(fontconfig liberation-fonts dejavu-sans-fonts libxkbcommon-x11
              xcb-util-cursor mesa-libGL mesa-libEGL cups-libs libglvnd-opengl
              libSM libICE)

die() { echo "error: $*" >&2; exit "${2:-1}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --appdir)    APPDIR="$(readlink -f -- "${2:?--appdir needs a value}")"; shift 2 ;;
        --appimage)  APPIMAGE="$(readlink -f -- "${2:?--appimage needs a value}")"; shift 2 ;;
        --out-dir)   OUT_DIR="$(readlink -m -- "${2:?--out-dir needs a value}")"; shift 2 ;;
        --formats)   FORMATS="${2:?--formats needs a value}"; shift 2 ;;
        --rpm-image) RPM_BASE_IMAGE="${2:?--rpm-image needs a value}"; shift 2 ;;
        -h|--help)   sed -n '2,59p' "$0"; exit 0 ;;
        *) die "unknown argument '$1' (see --help)" 2 ;;
    esac
done

[[ -n "$APPDIR" || -n "$APPIMAGE" ]] || die "give --appdir or --appimage (see --help)" 2
[[ -z "$APPDIR" || -z "$APPIMAGE" ]] || die "give only one of --appdir and --appimage" 2
WANT_DEB=0; WANT_RPM=0
IFS=',' read -r -a _formats <<< "$FORMATS"
for f in "${_formats[@]}"; do
    case "$f" in
        deb) WANT_DEB=1 ;;
        rpm) WANT_RPM=1 ;;
        *) die "unknown format '$f' (deb, rpm)" 2 ;;
    esac
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/pdfe-pkg.XXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT

if [[ -n "$APPIMAGE" ]]; then
    [[ -f "$APPIMAGE" ]] || die "$APPIMAGE not found" 3
    command -v unsquashfs >/dev/null 2>&1 || die "unsquashfs is required for --appimage (squashfs-tools)" 3
    # The squashfs image starts where the ELF runtime ends:
    # e_shoff + e_shnum * e_shentsize, read from the ELF64 header.
    offset="$(python3 -c '
import struct, sys
h = open(sys.argv[1], "rb").read(64)
if h[:4] != b"\x7fELF" or h[4] != 2:
    sys.exit(1)
shoff, = struct.unpack_from("<Q", h, 0x28)
size, num = struct.unpack_from("<HH", h, 0x3A)
print(shoff + size * num)' "$APPIMAGE")" || die "$APPIMAGE is not an ELF64 AppImage" 3
    unsquashfs -q -no-xattrs -o "$offset" -d "$WORK/AppDir" "$APPIMAGE" >/dev/null \
        || die "could not unpack the squashfs image of $APPIMAGE" 3
    APPDIR="$WORK/AppDir"
fi
[[ -d "$APPDIR/usr/bin" ]] || die "$APPDIR is not an AppDir (no usr/bin)" 3
for f in pdfe pdfe_preflight; do
    [[ -x "$APPDIR/usr/bin/$f" ]] || die "$APPDIR/usr/bin/$f missing or not executable" 3
done
[[ -f "$APPDIR/usr/share/applications/pdfe.desktop" ]] || die "$APPDIR has no usr/share/applications/pdfe.desktop" 3
[[ -f "$APPDIR/usr/share/icons/hicolor/256x256/apps/pdfe.png" ]] || die "$APPDIR has no 256x256 pdfe.png icon" 3

# The version the binary reports, as build-appimage.sh does, so the package
# version cannot disagree with the program.
VERSION="$(QT_QPA_PLATFORM=offscreen "$APPDIR/usr/bin/pdfe" --version 2>/dev/null | awk '{print $NF; exit}')"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read a version from '$APPDIR/usr/bin/pdfe --version' (got '$VERSION')" 4

# --- the shared file tree ---------------------------------------------------
ROOT="$WORK/root"
mkdir -p "$ROOT/opt/pdfe" "$ROOT/usr/bin" "$ROOT/usr/share/applications" \
         "$ROOT/usr/share/icons/hicolor/256x256/apps" "$ROOT/usr/share/doc/pdfe"
cp -a -- "$APPDIR/usr/." "$ROOT/opt/pdfe/"
# The menu entry and icon live where the desktop looks for them; copies left
# under /opt/pdfe would only be a second, unused set.
mv -- "$ROOT/opt/pdfe/share/applications/pdfe.desktop" "$ROOT/usr/share/applications/pdfe.desktop"
mv -- "$ROOT/opt/pdfe/share/icons/hicolor/256x256/apps/pdfe.png" "$ROOT/usr/share/icons/hicolor/256x256/apps/pdfe.png"
rm -rf -- "$ROOT/opt/pdfe/share/applications" "$ROOT/opt/pdfe/share/icons" "$ROOT/opt/pdfe/share/pixmaps"
rmdir -- "$ROOT/opt/pdfe/share" 2>/dev/null || true
# A second copy of the licence and the corresponding-source notice goes under
# /opt/pdfe, because minimal installs often leave /usr/share/doc out: Ubuntu's
# container and cloud images path-exclude it in dpkg, and dnf with
# tsflags=nodocs (Fedora's container image) skips %doc files. Under AGPL-3.0
# these two notices have to reach every installation.
mkdir -p -- "$ROOT/opt/pdfe/share/pdfe"
install -m 0644 -- "$REPO/LICENSE" "$ROOT/opt/pdfe/share/pdfe/LICENSE"
install -m 0644 -- "$REPO/CORRESPONDING-SOURCE.txt" "$ROOT/opt/pdfe/share/pdfe/CORRESPONDING-SOURCE.txt"
ln -s ../../opt/pdfe/bin/pdfe "$ROOT/usr/bin/pdfe"
install -m 0644 -- "$REPO/LICENSE" "$ROOT/usr/share/doc/pdfe/LICENSE"
install -m 0644 -- "$REPO/CORRESPONDING-SOURCE.txt" "$ROOT/usr/share/doc/pdfe/CORRESPONDING-SOURCE.txt"
{
    echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    echo "Upstream-Name: PDFE"
    echo "Source: https://github.com/ipswitch8/PDFE"
    echo
    echo "Files: *"
    echo "Copyright: Itwerx Corp"
    echo "License: AGPL-3.0-only"
    echo " The full text is in /usr/share/doc/pdfe/LICENSE. Where to get the"
    echo " corresponding source is in /usr/share/doc/pdfe/CORRESPONDING-SOURCE.txt."
    echo " Both are also in /opt/pdfe/share/pdfe, which is installed even where"
    echo " documentation is excluded."
    echo " The libraries under /opt/pdfe/lib carry their own copyright files"
    echo " under /opt/pdfe/share/doc."
} > "$ROOT/usr/share/doc/pdfe/copyright"
# Nothing setuid, setgid or group/world-writable; executables stay executable.
chmod -R u+rwX,go+rX,go-w,ug-s -- "$ROOT"
find "$ROOT" -type d -exec chmod 0755 {} +

mkdir -p -- "$OUT_DIR"
SUMMARY="PDF editor: annotate, redact, sign, edit text and images"
DESCRIPTION="PDFE is a desktop PDF editor. It views, annotates, redacts, signs,
fills forms in, and edits the text and images of PDF documents."

write_sha256() {
    ( cd -- "$OUT_DIR" && sha256sum -- "$1" > "$1.sha256" )
}

# --- .deb ---------------------------------------------------------------------
if (( WANT_DEB )); then
    command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb is required for the .deb" 3
    DEB_ROOT="$WORK/deb"
    cp -a -- "$ROOT" "$DEB_ROOT"
    mkdir -p "$DEB_ROOT/DEBIAN"
    installed_kb="$(du -sk --apparent-size -- "$ROOT" | cut -f1)"
    depends="libc6 (>= 2.39)"
    for d in "${DEB_DEPENDS[@]}"; do depends+=", $d"; done
    {
        echo "Package: pdfe"
        echo "Version: $VERSION"
        echo "Architecture: amd64"
        echo "Maintainer: Itwerx Corp <pdfe@itwerx.net>"
        echo "Installed-Size: $installed_kb"
        echo "Depends: $depends"
        echo "Section: graphics"
        echo "Priority: optional"
        echo "Homepage: https://github.com/ipswitch8/PDFE"
        echo "Description: $SUMMARY"
        # Continuation lines of a control field start with a space.
        echo " ${DESCRIPTION//$'\n'/$'\n' }"
    } > "$DEB_ROOT/DEBIAN/control"
    ( cd -- "$DEB_ROOT" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' \
        | sort -z | xargs -0 md5sum ) > "$DEB_ROOT/DEBIAN/md5sums"
    chmod 0644 "$DEB_ROOT/DEBIAN/control" "$DEB_ROOT/DEBIAN/md5sums"
    DEB="pdfe_${VERSION}_amd64.deb"
    rm -f -- "$OUT_DIR/$DEB" "$OUT_DIR/$DEB.sha256"
    dpkg-deb --root-owner-group -Zxz --build "$DEB_ROOT" "$OUT_DIR/$DEB" >/dev/null
    write_sha256 "$DEB"
    echo "built: $OUT_DIR/$DEB"
fi

# --- .rpm ---------------------------------------------------------------------
if (( WANT_RPM )); then
    command -v podman >/dev/null 2>&1 || die "podman is required for the .rpm (rpmbuild runs in a Fedora container)" 3
    RPM_IMAGE="localhost/pdfe-rpmbuild:$(tr -c 'a-zA-Z0-9.\n' '-' <<< "${RPM_BASE_IMAGE##*/}")"
    if ! podman image exists "$RPM_IMAGE"; then
        printf 'FROM %s\nRUN dnf -y -q install rpm-build && dnf -q clean all\n' "$RPM_BASE_IMAGE" \
            | podman build -q -t "$RPM_IMAGE" -f - "$WORK" >/dev/null \
            || die "could not build the rpmbuild image from $RPM_BASE_IMAGE" 3
    fi
    requires="glibc >= 2.39"
    for r in "${RPM_REQUIRES[@]}"; do requires+=", $r"; done
    mkdir -p "$WORK/rpm"
    cat > "$WORK/rpm/pdfe.spec" <<EOF
Name:           pdfe
Version:        $VERSION
Release:        1
Summary:        $SUMMARY
License:        AGPL-3.0-only
URL:            https://github.com/ipswitch8/PDFE
ExclusiveArch:  x86_64
Requires:       $requires
AutoReqProv:    no

# The binaries are shipped exactly as the AppImage carries them: no stripping,
# no debuginfo split, no build-id links, no other post-install rewriting.
%global debug_package %{nil}
%global __os_install_post %{nil}
%global _build_id_links none
%global _binary_payload w6T.xzdio

%description
$DESCRIPTION

%install
cp -a /pkgroot/. %{buildroot}/

%files
/opt/pdfe
/usr/bin/pdfe
/usr/share/applications/pdfe.desktop
/usr/share/icons/hicolor/256x256/apps/pdfe.png
%doc /usr/share/doc/pdfe
EOF
    podman run --rm --security-opt label=disable \
        -v "$ROOT:/pkgroot:ro" -v "$WORK/rpm:/rpm" \
        "$RPM_IMAGE" rpmbuild -bb --quiet \
            --define "_topdir /rpm/top" --define "_rpmdir /rpm/out" \
            /rpm/pdfe.spec >/dev/null \
        || die "rpmbuild failed" 5
    RPM="pdfe-${VERSION}-1.x86_64.rpm"
    [[ -f "$WORK/rpm/out/x86_64/$RPM" ]] || die "rpmbuild produced no $RPM" 5
    rm -f -- "$OUT_DIR/$RPM" "$OUT_DIR/$RPM.sha256"
    cp -- "$WORK/rpm/out/x86_64/$RPM" "$OUT_DIR/$RPM"
    write_sha256 "$RPM"
    echo "built: $OUT_DIR/$RPM"
fi
