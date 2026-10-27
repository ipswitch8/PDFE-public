# -*- coding: utf-8 -*-
"""
Provision the independent PDF/A conformance checker (veraPDF) into this
repository's documented default location.

meta-audit follow-up F14, gap PDF-10.

Before F14 the only independent PDF/A checker in the tree was reached through a
``VERAPDF_JAR`` environment variable that was set nowhere, so it had never run.
This script -- plus the ordered, variable-free resolution in
``tests/support/ConformanceChecker.cpp`` -- replaces that opt-in with a
deterministic one.

It downloads the official veraPDF greenfield installer, runs it HEADLESS via an
IzPack auto-install descriptor, and leaves the CLI jar at::

    <repo>/tools/conformance/verapdf/bin/cli-<version>.jar

which is the FIRST location ``resolveConformanceChecker()`` looks in.

The installed tree is gitignored (see ``tools/conformance/.gitignore``): a
~33 MB third-party Java distribution does not belong in the history. What is
committed is this script, so any machine can reproduce the installation.

Requirements: Python 3.9+, a JRE/JDK 11+ on PATH (or JAVA_HOME), outbound HTTPS
to software.verapdf.org.

Usage:
    python tools/conformance/fetch-verapdf.py [--force]
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
INSTALL_DIR = THIS_DIR / "verapdf"
INSTALLER_URL = "https://software.verapdf.org/releases/verapdf-installer.zip"

# IzPack automated-installation descriptor. Selects the CLI ("veraPDF Batch
# Files") pack plus the GUI pack that carries the shared jars, and nothing else.
AUTO_INSTALL_XML = """<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<AutomatedInstallation langpack="eng">
  <com.izforge.izpack.panels.htmlhello.HTMLHelloPanel id="welcome"/>
  <com.izforge.izpack.panels.target.TargetPanel id="install_dir">
    <installpath>{installpath}</installpath>
  </com.izforge.izpack.panels.target.TargetPanel>
  <com.izforge.izpack.panels.packs.PacksPanel id="sdk_pack_select">
    <pack index="0" name="veraPDF GUI" selected="true"/>
    <pack index="1" name="veraPDF Mac and *nix Scripts" selected="{nix}"/>
    <pack index="2" name="veraPDF Batch Files" selected="{win}"/>
    <pack index="3" name="veraPDF Validation model" selected="false"/>
    <pack index="4" name="veraPDF Documentation" selected="false"/>
    <pack index="5" name="veraPDF Sample Plugins" selected="false"/>
  </com.izforge.izpack.panels.packs.PacksPanel>
  <com.izforge.izpack.panels.install.InstallPanel id="install"/>
  <com.izforge.izpack.panels.finish.FinishPanel id="finish"/>
</AutomatedInstallation>
"""


def find_java() -> str:
    """Locate a java executable the same way ConformanceChecker.cpp does."""
    exe = shutil.which("java")
    if exe:
        return exe
    java_home = os.environ.get("JAVA_HOME", "")
    if java_home:
        cand = Path(java_home) / "bin" / ("java.exe" if os.name == "nt" else "java")
        if cand.exists():
            return str(cand)
    if os.name == "nt":
        for root in (
            Path("C:/Program Files/Java"),
            Path("C:/Program Files/Eclipse Adoptium"),
            Path("C:/Program Files/Microsoft/jdk"),
        ):
            if not root.is_dir():
                continue
            found = sorted(
                p for p in (d / "bin" / "java.exe" for d in root.iterdir()) if p.exists()
            )
            if found:
                return str(found[-1])
    raise SystemExit(
        "FATAL: no Java runtime found. Install a JRE/JDK 11+ and put `java` on "
        "PATH (or set JAVA_HOME), then re-run."
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true",
                    help="reinstall even if the CLI jar is already present")
    args = ap.parse_args()

    existing = sorted(INSTALL_DIR.glob("bin/cli-*.jar"))
    if existing and not args.force:
        print(f"already provisioned: {existing[-1]}")
        return 0

    java = find_java()
    print(f"java: {java}")

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        zip_path = tmpdir / "verapdf-installer.zip"
        print(f"downloading {INSTALLER_URL} ...")
        with urllib.request.urlopen(INSTALLER_URL, timeout=300) as resp:
            zip_path.write_bytes(resp.read())
        print(f"  {zip_path.stat().st_size} bytes")

        with zipfile.ZipFile(zip_path) as z:
            z.extractall(tmpdir)

        jars = sorted(tmpdir.glob("**/verapdf-izpack-installer-*.jar"))
        if not jars:
            raise SystemExit("FATAL: no IzPack installer jar inside the archive")
        installer = jars[-1]
        print(f"installer: {installer.name}")

        if INSTALL_DIR.exists() and args.force:
            shutil.rmtree(INSTALL_DIR)

        auto = tmpdir / "auto-install.xml"
        auto.write_text(
            AUTO_INSTALL_XML.format(
                installpath=str(INSTALL_DIR).replace("\\", "/"),
                win="true" if os.name == "nt" else "false",
                nix="false" if os.name == "nt" else "true",
            ),
            encoding="utf-8",
        )

        print(f"installing headless into {INSTALL_DIR} ...")
        proc = subprocess.run(
            [java, "-jar", str(installer), str(auto)],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        sys.stdout.write(proc.stdout or "")
        sys.stdout.write(proc.stderr or "")
        if proc.returncode != 0:
            raise SystemExit(f"FATAL: installer exited {proc.returncode}")

    jars = sorted(INSTALL_DIR.glob("bin/cli-*.jar"))
    if not jars:
        raise SystemExit(
            f"FATAL: install finished but no CLI jar under {INSTALL_DIR / 'bin'}"
        )
    print(f"OK: {jars[-1]}")

    banner = subprocess.run(
        [java, "-jar", str(jars[-1]), "--version"],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    sys.stdout.write(banner.stdout or "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
