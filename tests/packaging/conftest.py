# -*- coding: utf-8 -*-
"""Shared fixtures for the Linux packaging tests (GitHub #390).

`make_appdir` builds a small stand-in for the AppDir that build-appimage.sh
produces: shell-script stand-ins for pdfe and pdfe_preflight (pdfe answers
--version), one "bundled library", the real desktop file and icon. That is
enough for tools/packaging/build-linux-packages.sh to build a real .deb and a
real .rpm from it in seconds. The real binaries would take minutes to
compress.

`build_packages` runs that script. The .rpm needs rootless podman, because
rpmbuild runs in a Fedora container. There is deliberately no skip when
podman is missing: the test fails and says why, since a skipped rpm test
would let the rpm path go unverified while the run still reports green.
"""
import io
import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BUILD_PACKAGES = REPO / "tools" / "packaging" / "build-linux-packages.sh"
DESKTOP = REPO / "packaging" / "linux" / "pdfe.desktop"
ICON = REPO / "packaging" / "linux" / "pdfe.png"

PDFE_STUB = """#!/bin/sh
if [ "$1" = "--version" ]; then echo "pdfe {version}"; exit 0; fi
echo "stub pdfe $*"
"""
PREFLIGHT_STUB = "#!/bin/sh\necho preflight\n"


def make_appdir(root, version="9.8.7", library_bytes=b"\x7fELF stub library\n"):
    """Write a stand-in AppDir under `root` and return its path."""
    appdir = Path(root) / "AppDir"
    (appdir / "usr" / "bin").mkdir(parents=True)
    (appdir / "usr" / "lib").mkdir()
    (appdir / "usr" / "share" / "applications").mkdir(parents=True)
    icons = appdir / "usr" / "share" / "icons" / "hicolor" / "256x256" / "apps"
    icons.mkdir(parents=True)
    (appdir / "usr" / "share" / "doc" / "libstub1").mkdir(parents=True)
    pdfe = appdir / "usr" / "bin" / "pdfe"
    io.open(pdfe, "w", encoding="utf-8").write(PDFE_STUB.format(version=version))
    pdfe.chmod(0o755)
    pre = appdir / "usr" / "bin" / "pdfe_preflight"
    io.open(pre, "w", encoding="utf-8").write(PREFLIGHT_STUB)
    pre.chmod(0o755)
    lib = appdir / "usr" / "lib" / "libstub.so.1"
    lib.write_bytes(library_bytes)
    lib.chmod(0o644)
    io.open(appdir / "usr" / "share" / "doc" / "libstub1" / "copyright", "w",
            encoding="utf-8").write("stub library copyright\n")
    shutil.copy(DESKTOP, appdir / "usr" / "share" / "applications" / "pdfe.desktop")
    shutil.copy(ICON, icons / "pdfe.png")
    # What linuxdeploy also leaves at the AppDir root.
    os.symlink("usr/bin/pdfe", appdir / "AppRun")
    return appdir


def run_build(args, check=True):
    r = subprocess.run(["bash", str(BUILD_PACKAGES)] + [str(a) for a in args],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=1800)
    if check and r.returncode != 0:
        raise AssertionError("build-linux-packages.sh exit %d\nstdout:\n%s\nstderr:\n%s"
                             % (r.returncode, r.stdout, r.stderr))
    return r


def require_podman():
    if shutil.which("podman") is None:
        pytest.fail("podman is required: the .rpm is built and inspected in a "
                    "Fedora container (tools/packaging/build-linux-packages.sh)")


def build_packages(workdir, version="9.8.7", library_bytes=b"\x7fELF stub library\n",
                   formats="deb,rpm"):
    """Build packages from a fresh stand-in AppDir. Returns (appdir, outdir)."""
    if "rpm" in formats:
        require_podman()
    workdir = Path(workdir)
    appdir = make_appdir(workdir / "in", version=version, library_bytes=library_bytes)
    out = workdir / "out"
    run_build(["--appdir", appdir, "--out-dir", out, "--formats", formats])
    return appdir, out


@pytest.fixture(scope="session")
def stub_packages(tmp_path_factory):
    """One clean deb + rpm pair, built once per session from the stand-in."""
    return build_packages(tmp_path_factory.mktemp("pkgs"))
