#!/usr/bin/env bash
# install.sh — PDFE installer for Linux (GitHub #390).
#
# Usage:
#   ./install.sh <path-to-PDFE-x.y.z-x86_64.AppImage> [--system|--user] [--keep-old-install]
#
# Installs the runtime packages an AppImage of PDFE needs from the host (FUSE
# for mounting the AppImage, fontconfig and the metric-compatible fonts PDFE
# substitutes for Arial/Times/Courier, the X11/xcb client libraries, CUPS for
# printing), places the AppImage under /opt/pdfe (system) or ~/.local/opt/pdfe
# (per-user), symlinks a `pdfe` command into PATH, and registers a freedesktop
# .desktop launcher and icon. Re-running is safe: present components are
# skipped and the placed files are replaced in place.
#
# Modelled on the CPSM installer (github.com/ipswitch8/CPSM, install.sh): the
# same two privilege modes, the same narrow shadowing cleanup, the same
# reasons, which are kept next to the code they explain.

set -euo pipefail

# ── Testing seams ──────────────────────────────────────────────────────────
# Only paths, never behaviour: the test suite points these at a scratch tree
# so a --system run can be exercised without touching the real /opt or /usr.
# Each must be absolute when set; anything else is refused below.
OS_RELEASE_FILE="${PDFE_INSTALL_OS_RELEASE:-/etc/os-release}"
SYSTEM_ROOT="${PDFE_INSTALL_SYSTEM_ROOT:-}"
HOST_GLIBC="${PDFE_INSTALL_HOST_GLIBC:-$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')}"

# The oldest glibc the AppImage runs on: the newest GLIBC_x.y symbol version any
# bundled ELF file needs, which is set by the distribution it was built on.
# tools/packaging/build-appimage.sh measures it and refuses to build if this
# line disagrees, so the two cannot drift apart.
MIN_GLIBC="2.39"

# ── Argument & environment validation ──────────────────────────────────────

KEEP_OLD=0
APPIMAGE_PATH=""
WANT_HELP=0
MODE_CHOICE=""   # "system" | "user" | "" (ask, or infer from EUID)
_extra=0
for _arg in "$@"; do
    case "$_arg" in
        --keep-old-install) KEEP_OLD=1 ;;
        --system)           MODE_CHOICE="system" ;;
        --user)             MODE_CHOICE="user" ;;
        -h|--help)          WANT_HELP=1 ;;
        -*)                 echo "Error: unknown option '$_arg' (see --help)" >&2; exit 2 ;;
        *)
            if [[ -z "$APPIMAGE_PATH" ]]; then APPIMAGE_PATH="$_arg"; else _extra=1; fi
            ;;
    esac
done

usage() {
    cat <<EOF
Usage: $0 <path-to-PDFE-x.y.z-x86_64.AppImage> [options]

Installs PDFE and the runtime packages it needs from this Linux system.

Modes:
  System-wide (--system, or run as root):
      /opt/pdfe/pdfe.AppImage, symlinked as /usr/local/bin/pdfe, with the
      launcher in /usr/local/share/applications so EVERY user gets it.
  Per-user (--user, no root needed):
      ~/.local/opt/pdfe/pdfe.AppImage, symlinked as ~/.local/bin/pdfe, with the
      launcher under ~/.local/share. Every file is owned by you; sudo is used
      ONLY to install missing system packages, and only if any are missing.

Run without --system/--user and it asks (non-interactive runs install per-user).

A system-wide install REMOVES an older per-user install (~/.local/opt/pdfe,
its ~/.local/bin/pdfe symlink and its launcher) if one is present: ~/.local/bin
comes first on a normal PATH, so the old copy would keep winning. Only PDFE's
own install layout is touched; anything else of that name is reported and left.

Options:
  --system             Install for all users (re-runs itself with sudo).
  --user               Install for the current user only; never asks.
  --keep-old-install   Do not remove a shadowing per-user install.
  -h, --help           Show this help.

Supports: Debian/Ubuntu/Mint/Pop!_OS, Fedora/RHEL/Rocky/Alma, Arch/Manjaro,
openSUSE, Alpine. Other distros get a list of what to install by hand.
Tested on Ubuntu; the other distributions' package lists are checked by the
installer's own tests but have not been installed on those systems.

Examples:
  ./install.sh PDFE-0.3.1-x86_64.AppImage            # per-user
  ./install.sh PDFE-0.3.1-x86_64.AppImage --system   # all users (asks for sudo)
EOF
}

if [[ "$WANT_HELP" -eq 1 ]]; then usage; exit 0; fi
if [[ -z "$APPIMAGE_PATH" ]]; then usage >&2; exit 2; fi
if [[ "$_extra" -eq 1 ]]; then
    echo "Error: more than one AppImage path given" >&2; exit 2
fi
if [[ ! -f "$APPIMAGE_PATH" ]]; then
    echo "Error: AppImage not found at '$APPIMAGE_PATH'" >&2; exit 2
fi
APPIMAGE_PATH="$(readlink -f -- "$APPIMAGE_PATH")"
# An AppImage is an ELF executable with the AppImage magic "AI\x02" at offset 8.
# Checking it refuses a mistyped path (a .zip, a script) before anything is
# copied anywhere or run.
if [[ "$(head -c 4 -- "$APPIMAGE_PATH" | od -An -c | tr -d ' ')" != '177ELF' ]] ||
   [[ "$(dd if="$APPIMAGE_PATH" bs=1 skip=8 count=3 2>/dev/null | od -An -tx1 | tr -d ' ')" != '414902' ]]; then
    echo "Error: '$APPIMAGE_PATH' is not a type-2 AppImage" >&2; exit 2
fi
for _p in "$OS_RELEASE_FILE" ${SYSTEM_ROOT:+"$SYSTEM_ROOT"}; do
    if [[ "$_p" != /* ]]; then echo "Error: test override '$_p' must be absolute" >&2; exit 2; fi
done

if [[ -t 1 ]]; then
    RED=$'\e[31m'; YEL=$'\e[33m'; GRN=$'\e[32m'; BLU=$'\e[34m'; RST=$'\e[0m'
else
    RED=''; YEL=''; GRN=''; BLU=''; RST=''
fi
info()  { echo "${BLU}info${RST}   $*"; }
warn()  { echo "${YEL}warn${RST}   $*" >&2; }
error() { echo "${RED}error${RST}  $*" >&2; }
ok()    { echo "${GRN}ok${RST}     $*"; }

# ── Host compatibility ─────────────────────────────────────────────────────

version_ge() {   # version_ge A B: is A >= B (dotted numeric)?
    [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]
}

check_glibc() {
    if [[ -z "$HOST_GLIBC" ]]; then
        warn "Could not read this system's glibc version; PDFE needs glibc $MIN_GLIBC or newer"
        return 0
    fi
    if ! version_ge "$HOST_GLIBC" "$MIN_GLIBC"; then
        error "This system has glibc $HOST_GLIBC; this PDFE AppImage needs glibc $MIN_GLIBC or newer"
        error "(Ubuntu 24.04, Debian 13, Fedora 40 or later). Nothing was installed."
        exit 7
    fi
    info "glibc $HOST_GLIBC (needs $MIN_GLIBC or newer)"
}

# ── Distro / package manager / GUI detection ───────────────────────────────

DISTRO_ID="unknown"
DISTRO_LIKE=""
DISTRO_NAME=""
PM=""
HAS_GUI=0

detect_distro() {
    if [[ ! -f "$OS_RELEASE_FILE" ]]; then
        error "$OS_RELEASE_FILE missing — cannot detect the distribution"
        exit 3
    fi
    # Parsed, not sourced: the installer may be running as root, and sourcing
    # executes whatever the file contains.
    local key value
    while IFS='=' read -r key value; do
        value="${value%\"}"; value="${value#\"}"
        case "$key" in
            ID)          DISTRO_ID="$value" ;;
            ID_LIKE)     DISTRO_LIKE="$value" ;;
            PRETTY_NAME) DISTRO_NAME="$value" ;;
        esac
    done < "$OS_RELEASE_FILE"
    DISTRO_NAME="${DISTRO_NAME:-$DISTRO_ID}"
    info "Distro: $DISTRO_NAME (id=$DISTRO_ID)"
}

detect_pm() {
    local candidate native="" like
    # The distribution's own package manager first: a host can have several on
    # PATH (apt on a Fedora box, say), and its own one is the one whose package
    # names map_packages uses.
    for like in "$DISTRO_ID" $DISTRO_LIKE; do
        case "$like" in
            debian|ubuntu|linuxmint|raspbian|pop|elementary|kali|zorin|neon) native=apt-get ;;
            fedora|rhel|centos|rocky|almalinux|ol) native=dnf ;;
            arch|manjaro|endeavouros|garuda|artix) native=pacman ;;
            opensuse*|suse|sles) native=zypper ;;
            alpine) native=apk ;;
        esac
        [[ -n "$native" ]] && break
    done
    if [[ "$native" == dnf ]] && ! command -v dnf >/dev/null 2>&1; then native=yum; fi
    if [[ -n "$native" ]] && command -v "$native" >/dev/null 2>&1; then
        PM="$native"
        info "Package manager: $PM"
        return 0
    fi
    for candidate in apt-get dnf yum pacman zypper apk; do
        if command -v "$candidate" >/dev/null 2>&1; then
            PM="$candidate"
            info "Package manager: $PM"
            return 0
        fi
    done
    PM=""
    warn "No supported package manager found (apt-get/dnf/yum/pacman/zypper/apk)"
}

detect_gui() {
    if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
        HAS_GUI=1
    else
        HAS_GUI=0
        info "Headless session — the launcher is still registered for later logins"
    fi
}

# ── Privilege model ────────────────────────────────────────────────────────

AS_ROOT=()         # (sudo --), but ONLY for installing system packages
INSTALL_TYPE=""
INSTALL_DIR=""
BIN_DIR=""
APPS_DIR=""
ICONS_DIR=""
SELF_PATH="$(readlink -f -- "$0")"
# $EUID is readonly; mirror it so the decision logic is testable.
IS_ROOT=0
[[ "$EUID" -eq 0 ]] && IS_ROOT=1
# Testing seam: exercise the root-only branches (no-sudo package step, icon
# extraction dropped to $SUDO_USER, shadow cleanup) as an ordinary user.
# Honoured ONLY together with PDFE_INSTALL_SYSTEM_ROOT. Under it, placement
# goes into that scratch root and the shadow cleanup acts on $HOME (the
# test's scratch home), never on the real home that getent reports for
# $SUDO_USER. It grants no privilege; it only changes which branch runs.
TEST_AS_ROOT=0
if [[ "${PDFE_INSTALL_TEST_AS_ROOT:-}" == 1 && -n "$SYSTEM_ROOT" ]]; then IS_ROOT=1; TEST_AS_ROOT=1; fi

ask_install_mode() {
    # Only ask when we can: a non-interactive run (CI, a pipe) must not block
    # on a prompt, so it takes the choice that needs no privilege.
    if [[ ! -t 0 || ! -t 1 ]]; then
        info "Non-interactive; installing for the current user only." >&2
        info "Pass --system to install for all users." >&2
        echo "user"
        return 0
    fi
    local reply
    {
        echo ""
        echo "Install PDFE for:"
        echo "  [1] this user only  ($HOME/.local) — no sudo needed"
        echo "  [2] all users       (/opt, /usr/local) — needs sudo"
        echo ""
    } >&2
    while true; do
        read -r -p "Choice [1/2, default 1]: " reply </dev/tty || { echo "user"; return 0; }
        case "${reply:-1}" in
            1|u|user)        echo "user";   return 0 ;;
            2|a|all|system)  echo "system"; return 0 ;;
            *) echo "Please answer 1 or 2." >&2 ;;
        esac
    done
}

decide_install_target() {
    # system: already root; files go under /opt and /usr/local.
    # user:   everything lands under $HOME and is owned by the user.
    # Placement never uses sudo. A per-user install that wrote with sudo would
    # leave root-owned files in the user's home that the user cannot replace,
    # which is how a stale AppImage gets stuck (CPSM learned this in the field).
    if [[ "$IS_ROOT" -eq 1 ]]; then
        if [[ "$MODE_CHOICE" == "user" ]]; then
            error "--user was given but this is running as root"
            error "Re-run without sudo to install for the current user only"
            exit 5
        fi
        INSTALL_TYPE="system"
    else
        [[ -z "$MODE_CHOICE" ]] && MODE_CHOICE="$(ask_install_mode)"
        if [[ "$MODE_CHOICE" == "system" && -z "$SYSTEM_ROOT" ]]; then
            command -v sudo >/dev/null 2>&1 || {
                error "A system-wide install needs root, and sudo is not available"
                error "Re-run as root, or choose the per-user install (--user)"
                exit 5
            }
            info "Re-running with sudo for a system-wide install ..."
            local -a again=("$SELF_PATH" "$APPIMAGE_PATH" --system)
            [[ "$KEEP_OLD" -eq 1 ]] && again+=(--keep-old-install)
            exec sudo -- "${again[@]}"
        fi
        INSTALL_TYPE="$MODE_CHOICE"
    fi

    if [[ "$INSTALL_TYPE" == "system" ]]; then
        AS_ROOT=()
        INSTALL_DIR="$SYSTEM_ROOT/opt/pdfe"
        BIN_DIR="$SYSTEM_ROOT/usr/local/bin"
        APPS_DIR="$SYSTEM_ROOT/usr/local/share/applications"
        ICONS_DIR="$SYSTEM_ROOT/usr/local/share/icons/hicolor"
    else
        if [[ "$IS_ROOT" -eq 0 ]] && command -v sudo >/dev/null 2>&1; then
            AS_ROOT=(sudo --)
        else
            AS_ROOT=()
        fi
        INSTALL_DIR="$HOME/.local/opt/pdfe"
        BIN_DIR="$HOME/.local/bin"
        APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
        ICONS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
    fi
    info "Install mode: $INSTALL_TYPE  (AppImage: $INSTALL_DIR  command: $BIN_DIR/pdfe)"
}

# ── Runtime packages ───────────────────────────────────────────────────────
# What the AppImage takes from the host rather than bundling: libfuse to mount
# the image, and the libraries the AppImage excludelist leaves to the system
# (fontconfig, X11/xcb, GL, CUPS). The fonts are the metric-compatible
# substitutes PDFE resolves Arial/Times/Courier to through fontconfig.
# The last three entries of each list (libOpenGL, libSM, libICE) are needed
# by pdfe directly and are also on that excludelist. A desktop usually has
# them already; a clean Ubuntu 24.04, Debian 13 or Fedora 41 does not, and
# pdfe would not start there (found by tools/packaging/verify-linux-packages.sh,
# v0.5.3).

PKGS=()
NEEDS=()

map_packages() {
    case "$DISTRO_ID" in
        debian|ubuntu|linuxmint|raspbian|pop|elementary|kali|zorin|neon)
            PKGS=(libfuse2t64 fontconfig fonts-liberation fonts-dejavu-core
                  libxkbcommon-x11-0 libxcb-cursor0 libgl1 libegl1 libcups2
                  libopengl0 libsm6 libice6)
            # Releases before the 64-bit time_t transition call it libfuse2.
            if ! apt_has_package libfuse2t64; then PKGS[0]="libfuse2"; fi
            # And libcups2 became libcups2t64. The old name still installs
            # (the new package provides it), but dpkg-query never reports it
            # as installed, so every run would reinstall it.
            if apt_has_package libcups2t64; then
                local i
                for i in "${!PKGS[@]}"; do
                    if [[ "${PKGS[$i]}" == libcups2 ]]; then PKGS[i]="libcups2t64"; fi
                done
            fi
            ;;
        fedora|rhel|centos|rocky|almalinux|ol)
            PKGS=(fuse-libs fontconfig liberation-fonts dejavu-sans-fonts
                  libxkbcommon-x11 xcb-util-cursor mesa-libGL mesa-libEGL cups-libs
                  libglvnd-opengl libSM libICE) ;;
        arch|manjaro|endeavouros|garuda|artix)
            PKGS=(fuse2 fontconfig ttf-liberation ttf-dejavu
                  libxkbcommon-x11 xcb-util-cursor libglvnd libcups libsm libice) ;;
        opensuse*|suse|sles)
            PKGS=(libfuse2 fontconfig liberation-fonts dejavu-fonts
                  libxkbcommon-x11-0 libxcb-cursor0 Mesa-libGL1 Mesa-libEGL1 libcups2
                  libOpenGL0 libSM6 libICE6) ;;
        alpine)
            PKGS=(fuse fontconfig font-liberation font-dejavu
                  libxkbcommon-x11 xcb-util-cursor mesa-gl mesa-egl cups-libs
                  libsm libice) ;;
        *)
            local like
            for like in $DISTRO_LIKE; do
                case "$like" in
                    debian|ubuntu|fedora|rhel|arch|suse|opensuse)
                        warn "Unknown distro '$DISTRO_ID' — treating it as '$like'"
                        DISTRO_ID="$like"; map_packages; return ;;
                esac
            done
            PKGS=()
            ;;
    esac
}

apt_has_package() {
    command -v apt-cache >/dev/null 2>&1 && apt-cache show "$1" >/dev/null 2>&1
}

pkg_installed() {
    local p="$1"
    case "$PM" in
        apt-get) dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" ;;
        dnf|yum|zypper) rpm -q "$p" >/dev/null 2>&1 ;;
        pacman)  pacman -Q "$p" >/dev/null 2>&1 ;;
        apk)     apk info -e "$p" >/dev/null 2>&1 ;;
        *)       return 1 ;;
    esac
}

check_deps() {
    NEEDS=()
    local p
    for p in "${PKGS[@]}"; do
        pkg_installed "$p" || NEEDS+=("$p")
    done
    if [[ ${#NEEDS[@]} -eq 0 ]]; then
        ok "Runtime packages already installed"
    else
        info "Will install: ${NEEDS[*]}"
    fi
}

install_packages() {
    if [[ ${#PKGS[@]} -eq 0 || -z "$PM" ]]; then
        warn "Cannot install runtime packages automatically on '$DISTRO_ID'."
        warn "Install by hand: FUSE 2 (libfuse.so.2), fontconfig, Liberation and"
        warn "DejaVu fonts, libxkbcommon-x11, xcb-util-cursor, OpenGL/EGL, libcups."
        return 0
    fi
    [[ ${#NEEDS[@]} -eq 0 ]] && return 0
    if [[ "$IS_ROOT" -eq 0 && ${#AS_ROOT[@]} -eq 0 ]]; then
        warn "Missing packages (${NEEDS[*]}) and no sudo to install them; install them by hand."
        return 0
    fi
    info "Installing via $PM: ${NEEDS[*]}"
    case "$PM" in
        apt-get)
            "${AS_ROOT[@]}" apt-get update -qq
            "${AS_ROOT[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y -- "${NEEDS[@]}" ;;
        dnf|yum) "${AS_ROOT[@]}" "$PM" install -y -- "${NEEDS[@]}" ;;
        pacman)  "${AS_ROOT[@]}" pacman -Sy --noconfirm --needed -- "${NEEDS[@]}" ;;
        zypper)  "${AS_ROOT[@]}" zypper --non-interactive install -- "${NEEDS[@]}" ;;
        apk)     "${AS_ROOT[@]}" apk add -- "${NEEDS[@]}" ;;
    esac
    ok "Package install complete"
}

# ── Placement ──────────────────────────────────────────────────────────────

place_appimage() {
    info "Installing AppImage to $INSTALL_DIR/pdfe.AppImage"
    mkdir -p -- "$INSTALL_DIR" "$BIN_DIR"
    # Copy to a temporary name and rename over the old one, so a running copy
    # (or a half-finished copy after a full disk) never leaves a truncated
    # AppImage behind under the real name.
    local tmp
    tmp="$(mktemp -- "$INSTALL_DIR/.pdfe.AppImage.XXXXXX")"
    cp -f -- "$APPIMAGE_PATH" "$tmp"
    chmod 0755 -- "$tmp"
    mv -f -- "$tmp" "$INSTALL_DIR/pdfe.AppImage"
    ln -sfn -- "$INSTALL_DIR/pdfe.AppImage" "$BIN_DIR/pdfe"
    ok "Installed: $BIN_DIR/pdfe -> $INSTALL_DIR/pdfe.AppImage"
}

# ── Desktop integration ────────────────────────────────────────────────────

register_desktop() {
    mkdir -p -- "$APPS_DIR" "$ICONS_DIR/256x256/apps"
    # The icon comes out of the AppImage itself, so the launcher always shows
    # the icon of the version installed. The real file is extracted: the
    # AppImage's root pdfe.png and .DirIcon are symlinks to it, and extracting
    # only a link leaves its target behind. --appimage-extract writes into the
    # CURRENT directory, hence the scratch dir.
    #
    # Getting the icon means RUNNING the AppImage (--appimage-extract), and an
    # installer running as root must not execute the program it is installing.
    # Under sudo it runs as the invoking user instead; as a real root login,
    # with nobody to drop to, the icon is skipped and the launcher shows a
    # generic one.
    local scratch icon rel="usr/share/icons/hicolor/256x256/apps/pdfe.png"
    local -a as_user=()
    if [[ "$IS_ROOT" -eq 1 ]]; then
        if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]] && command -v runuser >/dev/null 2>&1; then
            as_user=(runuser -u "$SUDO_USER" --)
        else
            warn "Running as root with no invoking user to drop to; not executing the AppImage to extract its icon"
            as_user=(false)
        fi
    fi
    scratch="$(mktemp -d)"
    [[ "$IS_ROOT" -eq 1 && "${as_user[0]}" == runuser ]] && chown -- "$SUDO_USER" "$scratch"
    if ( cd "$scratch" && "${as_user[@]}" "$INSTALL_DIR/pdfe.AppImage" --appimage-extract "$rel" >/dev/null 2>&1 ) &&
       icon="$scratch/squashfs-root/$rel" && [[ -f "$icon" && ! -L "$icon" ]]; then
        cp -f -- "$icon" "$ICONS_DIR/256x256/apps/pdfe.png"
        ok "Icon installed"
    else
        warn "Could not extract the icon from the AppImage; the launcher will use a generic icon"
    fi
    rm -rf -- "$scratch"

    # Exec= names the persistent symlink, never a path inside the mounted
    # AppImage: that mount vanishes the moment the AppImage exits.
    local entry="$APPS_DIR/pdfe.desktop" exec_path
    # Desktop Entry spec: a quoted argument escapes ", `, $ and \ with a
    # backslash, and the whole value then has its backslashes doubled again.
    # A home directory with a space in it otherwise splits Exec= in two.
    exec_path="$BIN_DIR/pdfe"
    exec_path="${exec_path//\\/\\\\}"
    exec_path="${exec_path//\"/\\\"}"
    exec_path="${exec_path//\`/\\\`}"
    exec_path="${exec_path//\$/\\\$}"
    exec_path="${exec_path//\\/\\\\}"
    cat > "$entry.tmp" <<EOF
[Desktop Entry]
Type=Application
Name=PDFE
GenericName=PDF Editor
Comment=Edit, annotate, redact and sign PDF documents
Exec="$exec_path" %F
TryExec=$BIN_DIR/pdfe
Icon=pdfe
Terminal=false
Categories=Office;Viewer;
MimeType=application/pdf;
Keywords=pdf;editor;annotate;redact;sign;
StartupWMClass=pdfe
EOF
    mv -f -- "$entry.tmp" "$entry"
    chmod 0644 -- "$entry"
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t "$ICONS_DIR" >/dev/null 2>&1 || true
    fi
    ok "Launcher registered: $entry"
}

# ── Shadowing cleanup ──────────────────────────────────────────────────────

# Remove *path*, reporting rather than aborting. Always returns 0: under set -e
# a failed rm would otherwise skip the launcher update the user ran this for.
try_remove() {
    local path="$1"
    if rm -rf -- "$path" 2>/dev/null || [[ ! -e "$path" && ! -L "$path" ]]; then
        ok "Removed $path"
    else
        warn "Could not remove $path; it will keep shadowing this install"
        warn "  remove it by hand: sudo rm -rf -- '$path'"
    fi
    return 0
}

cleanup_shadowing_install() {
    # Only a SYSTEM install can be shadowed by a per-user one (~/.local/bin
    # precedes /usr/local/bin on PATH, and a per-user .desktop outranks the
    # system one). Deliberately narrow: only PDFE's own layout is removed.
    [[ "$INSTALL_TYPE" == "system" ]] || return 0
    local user_home other_dir other_link target_real
    if [[ "$TEST_AS_ROOT" -eq 1 ]]; then
        user_home="$HOME"
    elif [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
        user_home="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    else
        user_home="$HOME"
    fi
    [[ -n "$user_home" ]] || return 0
    other_dir="$user_home/.local/opt/pdfe"
    other_link="$user_home/.local/bin/pdfe"

    # This runs as root on paths inside another user's home, so every
    # component must be what it claims to be: refuse if the install directory
    # is itself a symlink, or if any parent resolves somewhere else.
    local parent
    for parent in "$user_home/.local/opt" "$user_home/.local/bin"; do
        if [[ -e "$parent" && "$(readlink -f -- "$parent")" != "$(readlink -f -- "$user_home")/${parent#"$user_home"/}" ]]; then
            warn "Left the per-user install alone — $parent resolves outside $user_home"
            return 0
        fi
    done
    if [[ -L "$other_dir" ]]; then
        warn "Left $other_dir alone — it is a symlink, not PDFE's install layout"
        remove_stale_user_desktop "$user_home" "$other_dir"
        return 0
    fi

    if [[ ! -e "$other_dir" && ! -L "$other_link" ]]; then
        remove_stale_user_desktop "$user_home" "$other_dir"
        return 0
    fi
    info "Found an older per-user install that would shadow this one"
    if [[ "$KEEP_OLD" -eq 1 ]]; then
        warn "--keep-old-install given; leaving it in place"
        warn "Note: ~/.local/bin usually precedes $BIN_DIR on PATH, so the older copy may still run."
        return 0
    fi
    if [[ -L "$other_link" ]]; then
        target_real="$(readlink -f -- "$other_link" 2>/dev/null || true)"
        if [[ "$target_real" == "$other_dir"/* ]]; then
            try_remove "$other_link"
        else
            warn "Left $other_link alone — it does not point into $other_dir"
        fi
    elif [[ -e "$other_link" ]]; then
        warn "Left $other_link alone — not a symlink, so not ours to remove"
    fi
    if [[ -d "$other_dir" ]]; then
        if [[ -f "$other_dir/pdfe.AppImage" ]]; then
            try_remove "$other_dir"
        else
            warn "Left $other_dir alone — no pdfe.AppImage inside, not our layout"
        fi
    fi
    remove_stale_user_desktop "$user_home" "$other_dir"
}

# Remove the per-user launcher ONLY if the program its Exec= runs is the
# per-user install being removed. A launcher the user re-pointed is theirs.
remove_stale_user_desktop() {
    local user_home="$1" other_dir="$2"
    local entry="$user_home/.local/share/applications/pdfe.desktop"
    [[ -f "$entry" ]] || return 0
    local exec_line exec_bin
    exec_line="$(grep -m1 '^Exec=' -- "$entry" 2>/dev/null || true)"
    exec_bin="${exec_line#Exec=}"
    if [[ "$exec_bin" == \"* ]]; then
        # Quoted program (as this installer writes it): up to the closing quote.
        exec_bin="${exec_bin#\"}"
        exec_bin="${exec_bin%%\"*}"
    else
        exec_bin="${exec_bin%% *}"
    fi
    if [[ "$exec_bin" != "$other_dir/pdfe.AppImage" && "$exec_bin" != "$user_home/.local/bin/pdfe" ]]; then
        warn "Left $entry alone — it does not launch the per-user PDFE install"
        return 0
    fi
    info "Removing the per-user launcher that pointed at the old install"
    try_remove "$entry"
}

# ── Final report ───────────────────────────────────────────────────────────

report() {
    echo ""
    ok "PDFE installation complete"
    echo "  Run from a terminal:  pdfe [file.pdf]"
    if [[ "$HAS_GUI" -eq 1 ]]; then
        echo "  From the app menu:    PDFE (Office)"
    else
        echo "  No desktop session now; the PDFE menu entry appears at your next graphical login."
    fi
    if [[ ":$PATH:" != *":$BIN_DIR:"* ]]; then
        warn "$BIN_DIR is not on your PATH; add it to your shell profile, e.g.:"
        warn "    export PATH=\"$BIN_DIR:\$PATH\""
    fi
}

main() {
    check_glibc
    detect_distro
    detect_pm
    detect_gui
    decide_install_target
    map_packages
    check_deps
    install_packages
    place_appimage
    cleanup_shadowing_install || true
    register_desktop
    report
}

main
