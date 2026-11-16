# -*- coding: utf-8 -*-
"""Tests for packaging/linux/install.sh (GitHub #390).

Every run is confined to a scratch HOME and, for --system, a scratch root
(PDFE_INSTALL_SYSTEM_ROOT). Package-manager commands (sudo, apt-get, apt-cache,
dpkg-query) are stubs on PATH that only LOG what they were asked to do, so no
test installs anything on the host. The AppImage is a stub carrying the real
type-2 magic (ELF, "AI\\x02" at offset 8), which is what the installer checks.

Run: python3 -m pytest -q tests/packaging/test_install_sh.py
"""
import io
import os
import shutil
import stat
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
INSTALLER = REPO / "packaging" / "linux" / "install.sh"

OS_RELEASE_UBUNTU = 'ID=ubuntu\nID_LIKE=debian\nPRETTY_NAME="Ubuntu 24.04 LTS"\n'

STUB_LOGGER = """#!/bin/sh
printf '%s %s\\n' "$(basename "$0")" "$*" >> "$STUB_LOG"
"""


def _write(path, text, mode=0o755):
    path.parent.mkdir(parents=True, exist_ok=True)
    io.open(path, "w", encoding="utf-8").write(text)
    path.chmod(mode)


@pytest.fixture
def env(tmp_path):
    """A scratch HOME, a stub bin dir first on PATH, an os-release file and a
    stub AppImage. `installed` controls what the stub dpkg-query reports."""
    home = tmp_path / "home"
    home.mkdir()
    stubs = tmp_path / "stubs"
    log = tmp_path / "stub.log"
    log.write_text("", encoding="utf-8")
    # sudo: log, then run the command (which is itself a stub).
    _write(stubs / "sudo", STUB_LOGGER + '[ "$1" = "--" ] && shift\nexec "$@"\n')
    for tool in ("apt-get", "apt-cache"):
        _write(stubs / tool, STUB_LOGGER + "exit 0\n")
    # dpkg-query reports a package installed iff its name is in $STUB_INSTALLED.
    _write(stubs / "dpkg-query", STUB_LOGGER + """
pkg="$(eval echo \\${$#})"
case " $STUB_INSTALLED " in
  *" $pkg "*|*" ALL "*) printf 'install ok installed'; exit 0 ;;
esac
exit 1
""")
    osr = tmp_path / "os-release"
    osr.write_text(OS_RELEASE_UBUNTU, encoding="utf-8")
    appimage = tmp_path / "PDFE-9.9.9-x86_64.AppImage"
    appimage.write_bytes(b"\x7fELF\x02\x01\x01\x00AI\x02" + b"\x00" * 4096)
    appimage.chmod(0o755)
    e = {
        "HOME": str(home),
        "PATH": f"{stubs}:{os.environ['PATH']}",
        "STUB_LOG": str(log),
        "STUB_INSTALLED": "ALL",
        "PDFE_INSTALL_OS_RELEASE": str(osr),
        "LANG": "C.UTF-8",
    }
    return {"tmp": tmp_path, "home": home, "env": e, "log": log,
            "appimage": appimage, "osr": osr}


def run(env, *args, extra=None, stdin=subprocess.DEVNULL):
    e = dict(env["env"])
    if extra:
        e.update(extra)
    return subprocess.run(["bash", str(INSTALLER), *map(str, args)], env=e,
                          stdin=stdin, capture_output=True, text=True,
                          encoding="utf-8", timeout=120)


def user_layout(home):
    return {
        "appimage": home / ".local/opt/pdfe/pdfe.AppImage",
        "link": home / ".local/bin/pdfe",
        "desktop": home / ".local/share/applications/pdfe.desktop",
    }


# ── Arguments ──────────────────────────────────────────────────────────────

def test_help_exits_zero_and_documents_every_option(env):
    r = run(env, "--help")
    assert r.returncode == 0, r.stderr
    for opt in ("--system", "--user", "--keep-old-install", "-h, --help"):
        assert opt in r.stdout
    r2 = run(env, "-h")
    assert r2.returncode == 0 and r2.stdout == r.stdout


def test_missing_appimage_argument_is_a_usage_error(env):
    r = run(env)
    assert r.returncode == 2
    assert "Usage:" in r.stderr


def test_unknown_option_is_refused(env):
    r = run(env, env["appimage"], "--frobnicate")
    assert r.returncode == 2
    assert "unknown option" in r.stderr


def test_nonexistent_path_is_refused(env):
    r = run(env, env["tmp"] / "nope.AppImage", "--user")
    assert r.returncode == 2
    assert "not found" in r.stderr


def test_a_file_that_is_not_an_appimage_is_refused_before_anything_is_written(env):
    bogus = env["tmp"] / "notes.txt"
    bogus.write_text("hello", encoding="utf-8")
    r = run(env, bogus, "--user")
    assert r.returncode == 2
    assert "not a type-2 AppImage" in r.stderr
    assert not (env["home"] / ".local").exists()


def test_two_appimage_paths_are_refused(env):
    r = run(env, env["appimage"], env["appimage"], "--user")
    assert r.returncode == 2


def test_a_relative_test_override_is_refused(env):
    r = run(env, env["appimage"], "--user", extra={"PDFE_INSTALL_OS_RELEASE": "os-release"})
    assert r.returncode == 2


# ── Per-user install ───────────────────────────────────────────────────────

def test_user_install_places_appimage_symlink_and_launcher(env):
    r = run(env, env["appimage"], "--user")
    assert r.returncode == 0, r.stdout + r.stderr
    lay = user_layout(env["home"])
    assert lay["appimage"].read_bytes() == env["appimage"].read_bytes()
    assert stat.S_IMODE(lay["appimage"].stat().st_mode) == 0o755
    assert lay["link"].is_symlink()
    assert lay["link"].resolve() == lay["appimage"].resolve()
    text = lay["desktop"].read_text(encoding="utf-8")
    assert f'Exec="{lay["link"]}" %F' in text
    assert "MimeType=application/pdf;" in text
    if shutil.which("desktop-file-validate"):
        v = subprocess.run(["desktop-file-validate", str(lay["desktop"])],
                           capture_output=True, text=True)
        assert v.returncode == 0, v.stdout + v.stderr
    # Nothing escaped the scratch HOME.
    assert not list(env["tmp"].glob("opt"))


def test_user_install_is_idempotent(env):
    first = run(env, env["appimage"], "--user")
    assert first.returncode == 0, first.stderr
    lay = user_layout(env["home"])
    snapshot = {k: (p.read_bytes() if not p.is_symlink() else os.readlink(p))
                for k, p in lay.items()}
    second = run(env, env["appimage"], "--user")
    assert second.returncode == 0, second.stderr
    again = {k: (p.read_bytes() if not p.is_symlink() else os.readlink(p))
             for k, p in lay.items()}
    assert again == snapshot
    # No temporary copies left behind by the atomic replace.
    assert sorted(p.name for p in lay["appimage"].parent.iterdir()) == ["pdfe.AppImage"]


def test_a_newer_appimage_replaces_the_installed_one(env):
    assert run(env, env["appimage"], "--user").returncode == 0
    newer = env["tmp"] / "PDFE-10.0.0-x86_64.AppImage"
    newer.write_bytes(b"\x7fELF\x02\x01\x01\x00AI\x02" + b"\x01" * 4096)
    newer.chmod(0o755)
    assert run(env, newer, "--user").returncode == 0
    assert user_layout(env["home"])["appimage"].read_bytes() == newer.read_bytes()


def test_non_interactive_run_without_a_mode_installs_per_user(env):
    r = run(env, env["appimage"])
    assert r.returncode == 0, r.stderr
    assert "Non-interactive" in r.stderr
    assert user_layout(env["home"])["appimage"].exists()


# ── Runtime packages ───────────────────────────────────────────────────────

def test_present_packages_are_not_reinstalled(env):
    r = run(env, env["appimage"], "--user")
    assert r.returncode == 0
    assert "apt-get install" not in env["log"].read_text(encoding="utf-8")


def test_missing_packages_are_installed_through_the_package_manager(env):
    r = run(env, env["appimage"], "--user", extra={"STUB_INSTALLED": "fontconfig libcups2t64"})
    assert r.returncode == 0, r.stderr
    log = env["log"].read_text(encoding="utf-8")
    install = [l for l in log.splitlines() if l.startswith("apt-get install")]
    assert install, log
    line = install[-1]
    for pkg in ("libfuse2t64", "fonts-liberation", "fonts-dejavu-core", "libxkbcommon-x11-0",
                "libopengl0", "libsm6", "libice6"):
        assert pkg in line
    assert "fontconfig" not in line and "libcups2" not in line
    # sudo is used for the package step, and only for it.
    assert any(l.startswith("sudo -- env DEBIAN_FRONTEND=noninteractive apt-get install")
               for l in log.splitlines()), log


def test_an_unknown_distro_still_installs_and_says_what_to_install(env):
    env["osr"].write_text('ID=plan9\nPRETTY_NAME="Plan 9"\n', encoding="utf-8")
    r = run(env, env["appimage"], "--user")
    assert r.returncode == 0, r.stderr
    assert "Install by hand" in r.stderr
    assert user_layout(env["home"])["appimage"].exists()


def test_a_derivative_is_mapped_through_id_like(env):
    env["osr"].write_text('ID=myos\nID_LIKE="ubuntu debian"\n', encoding="utf-8")
    r = run(env, env["appimage"], "--user", extra={"STUB_INSTALLED": ""})
    assert r.returncode == 0, r.stderr
    assert "treating it as 'ubuntu'" in r.stderr
    assert "apt-get install" in env["log"].read_text(encoding="utf-8")


def test_os_release_is_parsed_not_executed(env):
    marker = env["tmp"] / "executed"
    env["osr"].write_text(f'ID=ubuntu\nPRETTY_NAME="x"; touch {marker}\n$(touch {marker})\n',
                          encoding="utf-8")
    r = run(env, env["appimage"], "--user")
    assert r.returncode == 0, r.stderr
    assert not marker.exists(), "the installer executed os-release content"


# ── System install and shadowing cleanup ───────────────────────────────────

def system_layout(root):
    return {
        "appimage": root / "opt/pdfe/pdfe.AppImage",
        "link": root / "usr/local/bin/pdfe",
        "desktop": root / "usr/local/share/applications/pdfe.desktop",
    }


def test_system_install_uses_the_system_layout(env):
    root = env["tmp"] / "root"
    r = run(env, env["appimage"], "--system", extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root)})
    assert r.returncode == 0, r.stdout + r.stderr
    lay = system_layout(root)
    assert lay["appimage"].read_bytes() == env["appimage"].read_bytes()
    assert lay["link"].resolve() == lay["appimage"].resolve()
    assert f'Exec="{lay["link"]}" %F' in lay["desktop"].read_text(encoding="utf-8")
    assert not user_layout(env["home"])["appimage"].exists()


def _plant_user_install(env):
    assert run(env, env["appimage"], "--user").returncode == 0
    return user_layout(env["home"])


def test_system_install_removes_a_shadowing_user_install(env):
    old = _plant_user_install(env)
    root = env["tmp"] / "root"
    r = run(env, env["appimage"], "--system", extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root)})
    assert r.returncode == 0, r.stderr
    assert not old["appimage"].exists()
    assert not old["link"].is_symlink()
    assert not old["desktop"].exists()
    assert not old["appimage"].parent.exists()


def test_keep_old_install_leaves_the_user_install(env):
    old = _plant_user_install(env)
    root = env["tmp"] / "root"
    r = run(env, env["appimage"], "--system", "--keep-old-install",
            extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root)})
    assert r.returncode == 0, r.stderr
    assert old["appimage"].exists() and old["link"].is_symlink() and old["desktop"].exists()
    assert "--keep-old-install given" in r.stderr


def test_cleanup_never_touches_files_that_are_not_pdfe_layout(env):
    home = env["home"]
    other = home / "elsewhere/pdfe-real"
    other.parent.mkdir(parents=True)
    other.write_text("not ours", encoding="utf-8")
    (home / ".local/bin").mkdir(parents=True)
    (home / ".local/bin/pdfe").symlink_to(other)
    stray = home / ".local/opt/pdfe"
    stray.mkdir(parents=True)
    (stray / "readme.txt").write_text("not an install", encoding="utf-8")
    apps = home / ".local/share/applications"
    apps.mkdir(parents=True)
    (apps / "pdfe.desktop").write_text("[Desktop Entry]\nExec=/usr/bin/other %F\n", encoding="utf-8")

    root = env["tmp"] / "root"
    r = run(env, env["appimage"], "--system", extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root)})
    assert r.returncode == 0, r.stderr
    assert (home / ".local/bin/pdfe").is_symlink()
    assert other.read_text(encoding="utf-8") == "not ours"
    assert (stray / "readme.txt").exists()
    assert (apps / "pdfe.desktop").exists()
    assert "Left" in r.stderr


# ── Host compatibility ─────────────────────────────────────────────────────

def test_a_glibc_older_than_the_appimage_needs_is_refused_before_anything_is_written(env):
    r = run(env, env["appimage"], "--user", extra={"PDFE_INSTALL_HOST_GLIBC": "2.35"})
    assert r.returncode == 7, r.stdout + r.stderr
    assert "needs glibc" in r.stderr
    assert not (env["home"] / ".local").exists()


def test_the_glibc_floor_matches_what_the_build_script_enforces():
    text = INSTALLER.read_text(encoding="utf-8")
    assert 'MIN_GLIBC="' in text
    build = (REPO / "tools" / "packaging" / "build-appimage.sh").read_text(encoding="utf-8")
    assert "MIN_GLIBC" in build and "install.sh" in build


def test_the_report_reflects_whether_a_desktop_session_is_present(env):
    headless = run(env, env["appimage"], "--user")
    assert "next graphical login" in headless.stdout
    gui = run(env, env["appimage"], "--user", extra={"DISPLAY": ":99"})
    assert "From the app menu" in gui.stdout


# ── Hardening (phase-7 security review) ────────────────────────────────────

def test_a_home_with_a_space_still_yields_a_valid_quoted_launcher(env):
    spaced = env["tmp"] / "home with space"
    spaced.mkdir()
    r = run(env, env["appimage"], "--user", extra={"HOME": str(spaced)})
    assert r.returncode == 0, r.stderr
    desktop = spaced / ".local/share/applications/pdfe.desktop"
    text = desktop.read_text(encoding="utf-8")
    assert f'Exec="{spaced}/.local/bin/pdfe" %F' in text
    if shutil.which("desktop-file-validate"):
        v = subprocess.run(["desktop-file-validate", str(desktop)], capture_output=True, text=True)
        assert v.returncode == 0, v.stdout + v.stderr


def test_cleanup_refuses_a_symlinked_install_directory(env):
    home = env["home"]
    victim = env["tmp"] / "victim"
    victim.mkdir()
    (victim / "pdfe.AppImage").write_text("not ours", encoding="utf-8")
    (home / ".local/opt").mkdir(parents=True)
    (home / ".local/opt/pdfe").symlink_to(victim)
    root = env["tmp"] / "root"
    r = run(env, env["appimage"], "--system", extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root)})
    assert r.returncode == 0, r.stderr
    assert (home / ".local/opt/pdfe").is_symlink()
    assert (victim / "pdfe.AppImage").read_text(encoding="utf-8") == "not ours"
    assert "is a symlink" in r.stderr


def test_cleanup_refuses_when_a_parent_directory_resolves_elsewhere(env):
    home = env["home"]
    elsewhere = env["tmp"] / "elsewhere-opt"
    (elsewhere / "pdfe").mkdir(parents=True)
    (elsewhere / "pdfe/pdfe.AppImage").write_text("not ours", encoding="utf-8")
    (home / ".local").mkdir()
    (home / ".local/opt").symlink_to(elsewhere)
    root = env["tmp"] / "root"
    r = run(env, env["appimage"], "--system", extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root)})
    assert r.returncode == 0, r.stderr
    assert (elsewhere / "pdfe/pdfe.AppImage").exists()
    assert "resolves outside" in r.stderr


# ── Root-only branches, sudo re-exec and the other package managers ────────
# (phase-7 claim-check: each was asserted in --help/docs with no mechanism).

def _logging_stub(path, body="exit 0\n"):
    _write(path, STUB_LOGGER + body)


def test_system_mode_reexecs_itself_under_sudo_with_its_arguments(env):
    # A stub sudo that only RECORDS its argv: no SYSTEM_ROOT, not root, so the
    # script must hand itself to sudo and stop.
    stubs = Path(env["env"]["PATH"].split(":")[0])
    _logging_stub(stubs / "sudo")
    r = run(env, env["appimage"], "--system", "--keep-old-install")
    assert r.returncode == 0, r.stdout + r.stderr
    log = env["log"].read_text(encoding="utf-8").splitlines()
    calls = [l for l in log if l.startswith("sudo ")]
    assert calls == [f"sudo -- {INSTALLER} {env['appimage']} --system --keep-old-install"], log
    assert not (env["home"] / ".local").exists()


def test_as_root_the_icon_is_extracted_as_the_sudo_user_never_as_root(env):
    stubs = Path(env["env"]["PATH"].split(":")[0])
    _logging_stub(stubs / "runuser", "exit 1\n")
    root = env["tmp"] / "root"
    me = os.environ.get("USER") or subprocess.run(["id", "-un"], capture_output=True,
                                                  text=True).stdout.strip()
    r = run(env, env["appimage"], extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root),
                                         "PDFE_INSTALL_TEST_AS_ROOT": "1",
                                         "SUDO_USER": me})
    assert r.returncode == 0, r.stdout + r.stderr
    log = env["log"].read_text(encoding="utf-8").splitlines()
    runs = [l for l in log if l.startswith("runuser ")]
    assert runs, "the AppImage was not run through runuser"
    installed = root / "opt/pdfe/pdfe.AppImage"
    assert runs[0].startswith(f"runuser -u {me} -- {installed} --appimage-extract "), runs
    # As root there is no sudo: the package step runs the package manager directly.
    assert not any(l.startswith("sudo ") for l in log), log


def test_as_a_real_root_login_the_appimage_is_not_executed_for_its_icon(env):
    stubs = Path(env["env"]["PATH"].split(":")[0])
    _logging_stub(stubs / "runuser", "exit 1\n")
    root = env["tmp"] / "root"
    e = {"PDFE_INSTALL_SYSTEM_ROOT": str(root), "PDFE_INSTALL_TEST_AS_ROOT": "1",
         "SUDO_USER": ""}
    r = run(env, env["appimage"], extra=e)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "not executing the AppImage" in r.stderr
    assert not any(l.startswith("runuser ")
                   for l in env["log"].read_text(encoding="utf-8").splitlines())
    assert (root / "opt/pdfe/pdfe.AppImage").exists()


def test_the_root_seam_is_ignored_without_a_scratch_root(env):
    r = run(env, env["appimage"], "--user", extra={"PDFE_INSTALL_TEST_AS_ROOT": "1"})
    assert r.returncode == 0, r.stderr
    assert "Install mode: user" in r.stdout


@pytest.mark.parametrize("distro,pm,query,expected", [
    ("fedora", "dnf", "rpm",
     "dnf install -y -- fuse-libs fontconfig liberation-fonts dejavu-sans-fonts "
     "libxkbcommon-x11 xcb-util-cursor mesa-libGL mesa-libEGL cups-libs "
     "libglvnd-opengl libSM libICE"),
    ("arch", "pacman", None,
     "pacman -Sy --noconfirm --needed -- fuse2 fontconfig ttf-liberation ttf-dejavu "
     "libxkbcommon-x11 xcb-util-cursor libglvnd libcups libsm libice"),
    ("opensuse-tumbleweed", "zypper", "rpm",
     "zypper --non-interactive install -- libfuse2 fontconfig liberation-fonts dejavu-fonts "
     "libxkbcommon-x11-0 libxcb-cursor0 Mesa-libGL1 Mesa-libEGL1 libcups2 "
     "libOpenGL0 libSM6 libICE6"),
    ("alpine", "apk", None,
     "apk add -- fuse fontconfig font-liberation font-dejavu libxkbcommon-x11 "
     "xcb-util-cursor mesa-gl mesa-egl cups-libs libsm libice"),
])
def test_each_package_manager_gets_its_own_package_names_and_command(env, distro, pm, query,
                                                                     expected):
    # The names are the script's mapping; they were taken from each
    # distribution's repositories but are not installed on those systems here.
    stubs = Path(env["env"]["PATH"].split(":")[0])
    # Nothing is installed: every query stub reports "missing".
    for tool in {pm, query} - {None}:
        _logging_stub(stubs / tool, "exit 1\n" if tool in ("rpm",) else "exit 0\n")
    if pm == "pacman":
        _logging_stub(stubs / "pacman", '[ "$1" = "-Q" ] && exit 1\nexit 0\n')
    if pm == "apk":
        _logging_stub(stubs / "apk", '[ "$1" = "info" ] && exit 1\nexit 0\n')
    env["osr"].write_text(f'ID={distro}\nPRETTY_NAME="{distro}"\n', encoding="utf-8")
    r = run(env, env["appimage"], "--user")
    assert r.returncode == 0, r.stdout + r.stderr
    assert f"Package manager: {pm}" in r.stdout, r.stdout
    log = env["log"].read_text(encoding="utf-8")
    assert f"sudo -- {expected}" in log, log
    assert "apt-get install" not in log


def test_debian_before_the_time64_transition_gets_libfuse2(env):
    stubs = Path(env["env"]["PATH"].split(":")[0])
    _logging_stub(stubs / "apt-cache", "exit 100\n")
    r = run(env, env["appimage"], "--user", extra={"STUB_INSTALLED": ""})
    assert r.returncode == 0, r.stderr
    line = [l for l in env["log"].read_text(encoding="utf-8").splitlines()
            if "apt-get install" in l][-1]
    assert " libfuse2 " in f"{line} " and "libfuse2t64" not in line
    assert " libcups2 " in f"{line} " and "libcups2t64" not in line


def test_after_the_time64_transition_an_installed_libcups2t64_is_not_reinstalled(env):
    """On Ubuntu 24.04 / Debian 13 the package is libcups2t64. Asking
    dpkg-query about the old name reports it missing every time, so each run
    used to reinstall it (seen in a clean-container install, v0.5.3)."""
    r = run(env, env["appimage"], "--user", extra={"STUB_INSTALLED": "ALL"})
    assert r.returncode == 0, r.stderr
    assert "apt-get install" not in env["log"].read_text(encoding="utf-8")
    r = run(env, env["appimage"], "--user", extra={"STUB_INSTALLED": "libcups2t64"})
    line = [l for l in env["log"].read_text(encoding="utf-8").splitlines()
            if l.startswith("apt-get install")][-1]
    assert "libcups2" not in line, line


def test_the_shipped_desktop_file_is_valid():
    shipped = REPO / "packaging" / "linux" / "pdfe.desktop"
    text = shipped.read_text(encoding="utf-8")
    assert "MimeType=application/pdf;" in text and "Exec=pdfe %F" in text
    if shutil.which("desktop-file-validate"):
        v = subprocess.run(["desktop-file-validate", str(shipped)], capture_output=True, text=True)
        assert v.returncode == 0, v.stdout + v.stderr


def test_under_the_root_seam_the_shadow_cleanup_acts_on_the_scratch_home_only(env):
    # SUDO_USER names the REAL current user, whose real home getent would
    # return. The seam must keep the destructive cleanup inside the scratch
    # HOME: the per-user install planted there is removed, and the cleanup
    # reports the scratch path, never the real home.
    old = _plant_user_install(env)
    me = os.environ.get("USER") or subprocess.run(["id", "-un"], capture_output=True,
                                                  text=True).stdout.strip()
    real_home = subprocess.run(["getent", "passwd", me], capture_output=True,
                               text=True).stdout.split(":")[5]
    assert real_home and Path(real_home).resolve() != env["home"].resolve()
    stubs = Path(env["env"]["PATH"].split(":")[0])
    _logging_stub(stubs / "runuser", "exit 1\n")
    root = env["tmp"] / "root"
    r = run(env, env["appimage"], extra={"PDFE_INSTALL_SYSTEM_ROOT": str(root),
                                         "PDFE_INSTALL_TEST_AS_ROOT": "1", "SUDO_USER": me})
    assert r.returncode == 0, r.stdout + r.stderr
    assert not old["appimage"].exists() and not old["link"].is_symlink()
    out = r.stdout + r.stderr
    assert str(env["home"]) in out
    assert f"{real_home}/.local" not in out
