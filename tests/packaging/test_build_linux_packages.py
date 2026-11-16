# -*- coding: utf-8 -*-
"""Tests for tools/packaging/build-linux-packages.sh (GitHub #390).

The script is run on a stand-in AppDir (see conftest.py), and the .deb and
.rpm it produces are then read back: the deb with dpkg-deb, the rpm with rpm
in the same Fedora container that built it, and both payloads with the
release-asset hygiene scanner's own readers. Nothing is installed on the
host. Installing the real packages in clean containers is the job of the
release verification (docs/releasing-linux.md), not of these tests.

Run: python3 -m pytest -q tests/packaging/test_build_linux_packages.py
"""
import hashlib
import importlib.util
import io
import os
import re
import shutil
import stat
import subprocess
import tarfile
from pathlib import Path

import pytest

from conftest import (BUILD_PACKAGES, REPO, build_packages, make_appdir,
                      require_podman, run_build)

INSTALLER = REPO / "packaging" / "linux" / "install.sh"
RPM_IMAGE = "localhost/pdfe-rpmbuild:fedora-41"
VERSION = "9.8.7"
DEB = "pdfe_%s_amd64.deb" % VERSION
RPM = "pdfe-%s-1.x86_64.rpm" % VERSION


def _load_hygiene():
    p = REPO / "tools" / "gate" / "check_release_asset_hygiene.py"
    spec = importlib.util.spec_from_file_location("rah_pkg", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rah = _load_hygiene()

# Every path either package installs (directories excluded), with the mode
# it must carry. The symlink is checked separately.
EXPECTED_FILES = {
    "opt/pdfe/bin/pdfe": 0o755,
    "opt/pdfe/bin/pdfe_preflight": 0o755,
    "opt/pdfe/lib/libstub.so.1": 0o644,
    "opt/pdfe/share/doc/libstub1/copyright": 0o644,
    # Outside /usr/share/doc, so dpkg path-excludes and rpm nodocs keep them.
    "opt/pdfe/share/pdfe/LICENSE": 0o644,
    "opt/pdfe/share/pdfe/CORRESPONDING-SOURCE.txt": 0o644,
    "usr/share/applications/pdfe.desktop": 0o644,
    "usr/share/icons/hicolor/256x256/apps/pdfe.png": 0o644,
    "usr/share/doc/pdfe/LICENSE": 0o644,
    "usr/share/doc/pdfe/CORRESPONDING-SOURCE.txt": 0o644,
    "usr/share/doc/pdfe/copyright": 0o644,
}
SYMLINK = ("usr/bin/pdfe", "../../opt/pdfe/bin/pdfe")


# --- the dependency lists stay in step with the installer -------------------

def _bash_array(text, name, after, start=0):
    """Words of the first `name=( ... )` array assignment after `after`
    (searched from `start`)."""
    i = text.index(after, start)
    m = re.search(r"%s=\(([^)]*)\)" % re.escape(name), text[i:])
    assert m, "no %s=(...) after %r" % (name, after)
    return m.group(1).split()


def _installer_lists():
    t = io.open(INSTALLER, encoding="utf-8").read()
    start = t.index("map_packages() {")
    return (_bash_array(t, "PKGS", "debian|ubuntu|", start),
            _bash_array(t, "PKGS", "fedora|rhel|", start))


def _script_lists():
    t = io.open(BUILD_PACKAGES, encoding="utf-8").read()
    return _bash_array(t, "DEB_DEPENDS", "# Runtime dependencies"), \
        _bash_array(t, "RPM_REQUIRES", "# Runtime dependencies")


def test_package_dependencies_are_the_installers_list_minus_fuse():
    apt, dnf = _installer_lists()
    deb, rpm = _script_lists()
    assert apt[0].startswith("libfuse") and dnf[0] == "fuse-libs"
    assert deb == apt[1:], "deb Depends drifted from install.sh's apt list"
    assert rpm == dnf[1:], "rpm Requires drifted from install.sh's dnf list"


# --- argument handling and refusals -----------------------------------------

def test_no_input_is_a_usage_error(tmp_path):
    r = run_build(["--out-dir", tmp_path], check=False)
    assert r.returncode == 2 and "give --appdir or --appimage" in r.stderr


def test_unknown_format_is_a_usage_error(tmp_path):
    appdir = make_appdir(tmp_path)
    r = run_build(["--appdir", appdir, "--formats", "deb,snap"], check=False)
    assert r.returncode == 2 and "unknown format 'snap'" in r.stderr


def test_an_appdir_without_the_preflight_helper_is_refused(tmp_path):
    appdir = make_appdir(tmp_path)
    (appdir / "usr" / "bin" / "pdfe_preflight").unlink()
    r = run_build(["--appdir", appdir, "--out-dir", tmp_path / "o",
                   "--formats", "deb"], check=False)
    assert r.returncode == 3 and "pdfe_preflight missing" in r.stderr
    assert not (tmp_path / "o").exists() or not list((tmp_path / "o").iterdir())


def test_an_unreadable_version_is_refused(tmp_path):
    appdir = make_appdir(tmp_path, version="not-a-version")
    r = run_build(["--appdir", appdir, "--out-dir", tmp_path / "o",
                   "--formats", "deb"], check=False)
    assert r.returncode == 4 and "could not read a version" in r.stderr


# --- the .deb -----------------------------------------------------------------

def _dpkg_field(deb, field):
    return subprocess.run(["dpkg-deb", "-f", str(deb), field], check=True,
                          capture_output=True, text=True).stdout.strip()


def test_deb_control_fields(stub_packages):
    _appdir, out = stub_packages
    deb = out / DEB
    assert _dpkg_field(deb, "Package") == "pdfe"
    assert _dpkg_field(deb, "Version") == VERSION
    assert _dpkg_field(deb, "Architecture") == "amd64"
    apt, _dnf = _installer_lists()
    assert _dpkg_field(deb, "Depends") == ", ".join(["libc6 (>= 2.39)"] + apt[1:])


def test_deb_has_no_maintainer_scripts(stub_packages):
    _appdir, out = stub_packages
    members = subprocess.run(["dpkg-deb", "--ctrl-tarfile", str(out / DEB)],
                             check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(members)) as tf:
        names = sorted(n.lstrip("./") for n in tf.getnames() if n.strip("./"))
    assert names == ["control", "md5sums"]


def _deb_entries(deb):
    data = subprocess.run(["dpkg-deb", "--fsys-tarfile", str(deb)], check=True,
                          capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        return {ti.name.lstrip("./"): ti for ti in tf.getmembers()
                if ti.name.strip("./")}


def test_deb_file_list_modes_and_ownership(stub_packages):
    _appdir, out = stub_packages
    entries = _deb_entries(out / DEB)
    files = {n: ti for n, ti in entries.items() if ti.isfile()}
    assert {n: stat.S_IMODE(ti.mode) for n, ti in files.items()} == EXPECTED_FILES
    link = entries[SYMLINK[0]]
    assert link.issym() and link.linkname == SYMLINK[1]
    for n, ti in entries.items():
        assert (ti.uname, ti.gname, ti.uid, ti.gid) == ("root", "root", 0, 0), n
        assert not ti.mode & (stat.S_ISUID | stat.S_ISGID), n
        # A symlink's own mode is always 0777 and means nothing; what it
        # points to is checked above.
        if not ti.issym():
            assert not ti.mode & (stat.S_IWOTH | stat.S_IWGRP), n
        if ti.isdir():
            assert stat.S_IMODE(ti.mode) == 0o755, n


def test_deb_md5sums_cover_every_file_and_match(stub_packages):
    _appdir, out = stub_packages
    ctrl = subprocess.run(["dpkg-deb", "--ctrl-tarfile", str(out / DEB)],
                          check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(ctrl)) as tf:
        sums = tf.extractfile("./md5sums").read().decode("utf-8")
    listed = dict(reversed(line.split(None, 1)) for line in sums.splitlines())
    data = subprocess.run(["dpkg-deb", "--fsys-tarfile", str(out / DEB)],
                          check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        actual = {ti.name.lstrip("./"): hashlib.md5(tf.extractfile(ti).read()).hexdigest()
                  for ti in tf.getmembers() if ti.isfile()}
    assert listed == actual


def test_deb_payload_is_byte_identical_to_the_appdir(stub_packages):
    appdir, out = stub_packages
    members = dict(rah.deb_members((out / DEB).read_bytes()))
    for rel in ("bin/pdfe", "bin/pdfe_preflight", "lib/libstub.so.1"):
        name = [n for n in members if n.endswith("/opt/pdfe/" + rel)]
        assert len(name) == 1, rel
        assert members[name[0]] == (appdir / "usr" / rel).read_bytes(), rel


def test_the_usr_bin_link_runs_pdfe_from_an_unpacked_deb(stub_packages, tmp_path):
    """The command on PATH is the link; following it must reach the real
    executable with its preflight helper beside it."""
    _appdir, out = stub_packages
    subprocess.run(["dpkg-deb", "-x", str(out / DEB), str(tmp_path)], check=True)
    link = tmp_path / "usr" / "bin" / "pdfe"
    assert link.is_symlink()
    target = link.resolve()
    assert target == (tmp_path / "opt" / "pdfe" / "bin" / "pdfe").resolve()
    assert (target.parent / "pdfe_preflight").is_file()
    r = subprocess.run([str(link), "--version"], capture_output=True, text=True,
                       check=True)
    assert r.stdout.strip() == "pdfe " + VERSION


def test_checksum_files_verify(stub_packages):
    _appdir, out = stub_packages
    for name in (DEB, RPM):
        line = (out / (name + ".sha256")).read_text(encoding="utf-8").split()
        assert line[1] == name
        assert line[0] == hashlib.sha256((out / name).read_bytes()).hexdigest()


def test_the_desktop_entry_opens_pdfs_through_the_command_on_path(stub_packages):
    _appdir, out = stub_packages
    entries = _deb_entries(out / DEB)
    data = subprocess.run(["dpkg-deb", "--fsys-tarfile", str(out / DEB)],
                          check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        desktop = tf.extractfile(entries["usr/share/applications/pdfe.desktop"]) \
            .read().decode("utf-8")
    assert "\nExec=pdfe %F\n" in desktop
    assert "\nMimeType=application/pdf;\n" in desktop
    assert "\nIcon=pdfe\n" in desktop


# --- the .rpm -----------------------------------------------------------------

def _rpm_query(out, *args):
    require_podman()
    r = subprocess.run(["podman", "run", "--rm", "--security-opt", "label=disable",
                        "-v", "%s:/pkgs:ro" % out, RPM_IMAGE,
                        "rpm", "-qp"] + list(args) + ["/pkgs/" + RPM],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=600)
    assert r.returncode == 0, r.stderr
    return r.stdout


def test_rpm_name_version_and_arch(stub_packages):
    _appdir, out = stub_packages
    q = _rpm_query(out, "--qf", "%{NAME}|%{VERSION}|%{RELEASE}|%{ARCH}|%{LICENSE}")
    assert q == "pdfe|%s|1|x86_64|AGPL-3.0-only" % VERSION


def test_rpm_requires_are_exactly_the_runtime_list(stub_packages):
    _appdir, out = stub_packages
    reqs = [r.strip() for r in _rpm_query(out, "--requires").splitlines()
            if r.strip() and not r.startswith("rpmlib(")]
    _apt, dnf = _installer_lists()
    assert sorted(reqs) == sorted(["glibc >= 2.39"] + dnf[1:])


def test_rpm_does_not_provide_the_bundled_libraries(stub_packages):
    _appdir, out = stub_packages
    provides = [p.strip() for p in _rpm_query(out, "--provides").splitlines() if p.strip()]
    assert sorted(provides) == sorted(["pdfe = %s-1" % VERSION,
                                       "pdfe(x86-64) = %s-1" % VERSION])


def test_both_payloads_are_xz(stub_packages):
    """The header says both payloads are xz, which the release-asset scanner
    reads with the standard library on any platform, including the Windows
    runner that scans published releases. A zstd payload (Fedora's default)
    would need zstd or a 7-Zip that reads it, so this is what keeps the scan
    portable."""
    _appdir, out = stub_packages
    assert _rpm_query(out, "--qf", "%{PAYLOADCOMPRESSOR}") == "xz"
    ar = rah.ar_members((out / DEB).read_bytes())
    assert sorted(n for n, _b in ar if n.startswith(("control.tar", "data.tar"))) \
        == ["control.tar.xz", "data.tar.xz"]


def test_rpm_has_no_scriptlets(stub_packages):
    _appdir, out = stub_packages
    assert _rpm_query(out, "--scripts").strip() == ""


def test_rpm_file_list_and_modes(stub_packages):
    _appdir, out = stub_packages
    rows = _rpm_query(out, "--qf", "[%{FILEMODES:octal} %{FILEUSERNAME} "
                      "%{FILEGROUPNAME} %{FILENAMES} %{FILELINKTOS}\n]")
    files, links = {}, {}
    for row in rows.splitlines():
        parts = row.split()
        mode, user, group, name = int(parts[0], 8), parts[1], parts[2], parts[3]
        assert (user, group) == ("root", "root"), name
        assert not mode & (stat.S_ISUID | stat.S_ISGID), name
        rel = name.lstrip("/")
        if stat.S_ISREG(mode):
            files[rel] = stat.S_IMODE(mode)
        elif stat.S_ISLNK(mode):
            links[rel] = parts[4]
    assert files == EXPECTED_FILES
    assert links == {SYMLINK[0]: SYMLINK[1]}


def test_rpm_payload_is_byte_identical_to_the_appdir(stub_packages):
    appdir, out = stub_packages
    members = dict(rah.rpm_members((out / RPM).read_bytes()))
    for rel in ("bin/pdfe", "bin/pdfe_preflight", "lib/libstub.so.1"):
        name = [n for n in members if n.endswith("/opt/pdfe/" + rel)]
        assert len(name) == 1, rel
        assert members[name[0]] == (appdir / "usr" / rel).read_bytes(), rel


# --- --appimage input ---------------------------------------------------------

def test_an_appimage_input_gives_the_same_payload_as_its_appdir(tmp_path):
    """A stand-in type-2 AppImage: an ELF64 file whose section headers end the
    file, followed by a squashfs image of the AppDir. That is the layout the
    real runtime has, and the offset build-linux-packages.sh computes."""
    appdir = make_appdir(tmp_path / "in")
    elf = shutil.which("true")
    assert elf, "need an ELF executable to stand in for the AppImage runtime"
    img = tmp_path / "image.squashfs"
    subprocess.run(["mksquashfs", str(appdir), str(img), "-quiet", "-noappend",
                    "-all-root"], check=True)
    runtime = Path(os.path.realpath(elf)).read_bytes()
    assert rah.appimage_squashfs_offset(runtime[:64]) == len(runtime), \
        "the stand-in runtime's section headers must end the file"
    appimage = tmp_path / "PDFE-9.8.7-x86_64.AppImage"
    appimage.write_bytes(runtime + img.read_bytes())
    appimage.chmod(0o755)
    out = tmp_path / "out"
    run_build(["--appimage", appimage, "--out-dir", out, "--formats", "deb"])
    members = dict(rah.deb_members((out / DEB).read_bytes()))
    pdfe = [n for n in members if n.endswith("/opt/pdfe/bin/pdfe")]
    assert members[pdfe[0]] == (appdir / "usr" / "bin" / "pdfe").read_bytes()
