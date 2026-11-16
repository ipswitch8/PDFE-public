# -*- coding: utf-8 -*-
"""docs/releasing-linux.md stays coupled to the release scripts (GitHub #390).

The doc once described a host build (out/build-release) after the release
had moved into a container because host builds embed the build account's home
directory. A maintainer following the stale text would have shipped that leak.
These tests fail if the doc stops naming a release script, names one that no
longer exists, or goes back to describing a host release build.

Run: python3 -m pytest -q tests/packaging/test_releasing_linux_doc.py
"""
import io
import re

import pytest

from conftest import REPO

DOC = REPO / "docs" / "releasing-linux.md"

# Every script the release procedure depends on, and why the doc must name it.
RELEASE_SCRIPTS = [
    "tools/packaging/build-release-container.sh",   # the only sanctioned release build
    "tools/packaging/build-linux-packages.sh",      # the .deb and .rpm
    "tools/packaging/verify-linux-packages.sh",     # clean-container installs
    "tools/gate/check_release_asset_hygiene.py",    # the pre-upload scan
    "packaging/linux/install.sh",                   # the installer asset
]


def _doc():
    return io.open(DOC, encoding="utf-8").read()


@pytest.mark.parametrize("script", RELEASE_SCRIPTS)
def test_the_doc_names_each_release_script_and_it_exists(script):
    assert script in _doc(), "docs/releasing-linux.md no longer names %s" % script
    assert (REPO / script).is_file(), "%s is named in the doc but missing" % script


def test_every_tools_packaging_script_the_doc_names_exists():
    named = set(re.findall(r"tools/packaging/[A-Za-z0-9_.-]+\.sh", _doc()))
    assert named, "the doc names no tools/packaging script at all"
    missing = sorted(p for p in named if not (REPO / p).is_file())
    assert not missing, "the doc names scripts that do not exist: %s" % missing


def test_the_doc_does_not_describe_a_host_release_build():
    # The pre-container recipe configured the release tree on the host.
    assert "-B out/build-release" not in _doc(), (
        "docs/releasing-linux.md describes a host release build again; the "
        "release is built by tools/packaging/build-release-container.sh")


def test_the_doc_lists_all_seven_release_assets():
    doc = _doc()
    for asset in ("PDFE-<version>-x86_64.AppImage", "pdfe_<version>_amd64.deb",
                  "pdfe-<version>-1.x86_64.rpm", "install.sh"):
        assert asset in doc, "the doc's asset list lost %s" % asset
