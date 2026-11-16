#!/usr/bin/env bash
# tools/packaging/verify-linux-packages.sh — install each Linux release asset
# in a clean container, run it, and remove it again (GitHub #390).
#
# Usage:
#   tools/packaging/verify-linux-packages.sh --version X.Y.Z
#       [--deb FILE] [--rpm FILE] [--appimage FILE --installer FILE]
#       [--log-dir DIR]
#
# What each check proves, in a container that has never seen PDFE:
#
#   .deb       ubuntu:24.04 and debian:trixie. `apt-get install ./file.deb`
#              resolves every Depends from the distribution's archive;
#              `pdfe --version` prints X.Y.Z from /usr/bin/pdfe; the desktop
#              entry and icon are in place; `apt-get purge` leaves nothing
#              under /opt/pdfe and no /usr/bin/pdfe.
#   .rpm       fedora:41. The same, with dnf.
#   AppImage   ubuntu:24.04, as root. install.sh --user installs the runtime
#              packages and the AppImage; the `pdfe` link it creates prints
#              X.Y.Z. A second run must succeed too (idempotent). There is no
#              FUSE in a container, so the AppImage runs with
#              APPIMAGE_EXTRACT_AND_RUN=1. A second container does the same
#              with --system (twice): /usr/local/bin/pdfe ->
#              /opt/pdfe/pdfe.AppImage plus the desktop entry.
#
# Each check's full transcript goes to --log-dir (default: the current
# directory) as deb-ubuntu.txt, deb-debian.txt, rpm-fedora.txt,
# appimage-install.txt and appimage-system.txt. The script exits non-zero if
# any check fails.
#
# The containers need network access for the distribution archives. Nothing
# is installed on the host.

set -euo pipefail

VERSION=""
DEB=""
RPM=""
APPIMAGE=""
INSTALLER=""
LOG_DIR="$PWD"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)   VERSION="${2:?}"; shift 2 ;;
        --deb)       DEB="$(readlink -f -- "${2:?}")"; shift 2 ;;
        --rpm)       RPM="$(readlink -f -- "${2:?}")"; shift 2 ;;
        --appimage)  APPIMAGE="$(readlink -f -- "${2:?}")"; shift 2 ;;
        --installer) INSTALLER="$(readlink -f -- "${2:?}")"; shift 2 ;;
        --log-dir)   LOG_DIR="$(readlink -m -- "${2:?}")"; shift 2 ;;
        -h|--help)   sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1' (see --help)" >&2; exit 2 ;;
    esac
done
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: --version X.Y.Z is required" >&2; exit 2; }
[[ -n "$DEB$RPM$APPIMAGE" ]] || { echo "error: nothing to verify (--deb, --rpm, --appimage)" >&2; exit 2; }
[[ -z "$APPIMAGE" || -n "$INSTALLER" ]] || { echo "error: --appimage needs --installer" >&2; exit 2; }
for f in "$DEB" "$RPM" "$APPIMAGE" "$INSTALLER"; do
    [[ -z "$f" || -f "$f" ]] || { echo "error: $f not found" >&2; exit 3; }
done
command -v podman >/dev/null 2>&1 || { echo "error: podman is required" >&2; exit 3; }
mkdir -p -- "$LOG_DIR"

FAILED=()

# run_check NAME IMAGE MOUNT_ARGS... -- SCRIPT
# The script is passed to bash -c inside the container; the transcript,
# with every command echoed (set -x), goes to $LOG_DIR/NAME.txt.
run_check() {
    local name="$1" image="$2"; shift 2
    local -a mounts=()
    while [[ "$1" != "--" ]]; do mounts+=("$1"); shift; done
    shift
    local script="$1" log="$LOG_DIR/$name.txt"
    {
        echo "# $name: $image ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
        podman run --rm --security-opt label=disable "${mounts[@]}" \
            -e PDFE_EXPECT_VERSION="$VERSION" -e QT_QPA_PLATFORM=offscreen \
            "$image" bash -c "set -euxo pipefail; $script"
    } > "$log" 2>&1
    local rc=$?
    echo "# exit=$rc" >> "$log"
    if (( rc == 0 )); then
        echo "PASS  $name  ($log)"
    else
        echo "FAIL  $name  ($log)"
        FAILED+=("$name")
    fi
}

# Shared assertions, run inside every container after installation. The
# ldd step comes first and lists EVERY unresolved library. The dynamic loader
# names only the first one it misses, so a missing dependency found by
# running the program would be found one library at a time.
# shellcheck disable=SC2016  # expanded inside the container, not here
CHECK_INSTALLED='
for b in /opt/pdfe/bin/pdfe /opt/pdfe/bin/pdfe_preflight; do
    missing="$(ldd "$b" | grep "not found" || true)"
    [ -z "$missing" ] || { echo "UNRESOLVED in $b:"; echo "$missing"; exit 1; }
done
out="$(pdfe --version)"; echo "pdfe --version -> $out"
[ "${out##* }" = "$PDFE_EXPECT_VERSION" ]
[ "$(readlink -f "$(command -v pdfe)")" = /opt/pdfe/bin/pdfe ]
[ -x /opt/pdfe/bin/pdfe_preflight ]
[ -f /usr/share/applications/pdfe.desktop ]
grep -qx "MimeType=application/pdf;" /usr/share/applications/pdfe.desktop
[ -f /usr/share/icons/hicolor/256x256/apps/pdfe.png ]
[ -s /opt/pdfe/share/pdfe/CORRESPONDING-SOURCE.txt ]
[ -s /opt/pdfe/share/pdfe/LICENSE ]
'
# The notices are checked under /opt/pdfe, not /usr/share/doc. The images
# tested here drop /usr/share/doc on purpose (Ubuntu's dpkg path-excludes,
# Fedora's tsflags=nodocs), and a user's minimal install may do the same.
# shellcheck disable=SC2016  # expanded inside the container, not here
CHECK_REMOVED='
[ ! -e /opt/pdfe ] || { find /opt/pdfe | head; exit 1; }
[ ! -e /usr/bin/pdfe ]
[ ! -e /usr/share/applications/pdfe.desktop ]
'

set +e
if [[ -n "$DEB" ]]; then
    deb_script="export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends /pkg/$(basename "$DEB")
$CHECK_INSTALLED
apt-get purge -y pdfe
$CHECK_REMOVED
echo VERIFIED"
    run_check deb-ubuntu docker.io/library/ubuntu:24.04 -v "$DEB:/pkg/$(basename "$DEB"):ro" -- "$deb_script"
    run_check deb-debian docker.io/library/debian:trixie -v "$DEB:/pkg/$(basename "$DEB"):ro" -- "$deb_script"
fi
if [[ -n "$RPM" ]]; then
    rpm_script="dnf -y -q install /pkg/$(basename "$RPM")
$CHECK_INSTALLED
dnf -y -q remove pdfe
$CHECK_REMOVED
echo VERIFIED"
    run_check rpm-fedora docker.io/library/fedora:41 -v "$RPM:/pkg/$(basename "$RPM"):ro" -- "$rpm_script"
fi
if [[ -n "$APPIMAGE" ]]; then
    ai="$(basename "$APPIMAGE")"
    # install.sh --user refuses to run as root, so this runs it as an
    # ordinary user with passwordless sudo, the way a desktop user would. The
    # missing-library check runs on the AppImage's own payload.
    ai_script="export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends sudo >/dev/null
useradd -m tester
echo 'tester ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/tester
install -o tester -m 0755 /pkg/$ai /pkg/install.sh /home/tester/
sudo -u tester -H env APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen \\
    PDFE_EXPECT_VERSION=\"\$PDFE_EXPECT_VERSION\" bash -euxo pipefail -c '
cd ~
./install.sh ./$ai --user
ls -l ~/.local/bin/pdfe ~/.local/opt/pdfe/pdfe.AppImage
test -f ~/.local/share/applications/pdfe.desktop
./$ai --appimage-extract >/dev/null
for b in squashfs-root/usr/bin/pdfe squashfs-root/usr/bin/pdfe_preflight; do
    missing=\$(LD_LIBRARY_PATH=squashfs-root/usr/lib ldd \$b | grep \"not found\" || true)
    test -z \"\$missing\" || { echo UNRESOLVED in \$b:; echo \"\$missing\"; exit 1; }
done
rm -rf squashfs-root
out=\$(~/.local/bin/pdfe --version); echo \"pdfe --version -> \$out\"
test \"\${out##* }\" = \"\$PDFE_EXPECT_VERSION\"
echo --- second run, must be idempotent
./install.sh ./$ai --user
out=\$(~/.local/bin/pdfe --version)
test \"\${out##* }\" = \"\$PDFE_EXPECT_VERSION\"
'
echo VERIFIED"
    run_check appimage-install docker.io/library/ubuntu:24.04 \
        -v "$APPIMAGE:/pkg/$ai:ro" -v "$INSTALLER:/pkg/install.sh:ro" -- "$ai_script"
    # The all-users install: the same ordinary user runs --system, and
    # install.sh re-runs itself with sudo. Then a second run (idempotent).
    ai_system_script="export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends sudo >/dev/null
useradd -m tester
echo 'tester ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/tester
install -o tester -m 0755 /pkg/$ai /pkg/install.sh /home/tester/
for run in 1 2; do
    echo \"--- --system run \$run\"
    sudo -u tester -H env APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen \\
        bash -euxo pipefail -c 'cd ~ && ./install.sh ./$ai --system'
    ls -l /usr/local/bin/pdfe /opt/pdfe/pdfe.AppImage
    test \"\$(readlink -f /usr/local/bin/pdfe)\" = /opt/pdfe/pdfe.AppImage
    test -f /usr/local/share/applications/pdfe.desktop
    out=\$(sudo -u tester -H env APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen /usr/local/bin/pdfe --version)
    echo \"pdfe --version -> \$out\"
    test \"\${out##* }\" = \"\$PDFE_EXPECT_VERSION\"
done
echo VERIFIED"
    run_check appimage-system docker.io/library/ubuntu:24.04 \
        -v "$APPIMAGE:/pkg/$ai:ro" -v "$INSTALLER:/pkg/install.sh:ro" -- "$ai_system_script"
fi
set -e

if (( ${#FAILED[@]} )); then
    echo "verification FAILED: ${FAILED[*]}" >&2
    exit 1
fi
echo "all checks passed"
