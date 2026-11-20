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
    "tools/packaging/publish-linux-release.py",     # one release per version
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


SEVEN_ASSETS = [
    "PDFE-<version>-x86_64.AppImage",
    "PDFE-<version>-x86_64.AppImage.sha256",
    "pdfe_<version>_amd64.deb",
    "pdfe_<version>_amd64.deb.sha256",
    "pdfe-<version>-1.x86_64.rpm",
    "pdfe-<version>-1.x86_64.rpm.sha256",
    "install.sh",
]


def test_the_doc_lists_all_seven_release_assets():
    doc = _doc()
    assert len(SEVEN_ASSETS) == 7
    for asset in SEVEN_ASSETS:
        assert asset in doc, "the doc's asset list lost %s" % asset


def _publisher():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "pub", str(REPO / "tools" / "packaging" / "publish-linux-release.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_publish_helper_uploads_the_seven_assets_the_doc_lists():
    # The helper is what actually ships; an asset it does not upload is
    # missing from the release even if the doc lists it.
    names = _publisher().asset_names("<version>")
    assert names == SEVEN_ASSETS, names


def test_the_doc_publishes_with_the_helper_into_one_release():
    # One release per version: the front page's "Latest" release must hold
    # every platform's files. The doc must publish through the helper, and
    # must not go back to creating a separate Linux release or -linux tag.
    doc = _doc()
    start = doc.index("## 5. Publish")
    sec = doc[start:doc.index("\n## ", start + 1)]
    assert "tools/packaging/publish-linux-release.py --tag v<version> --apply" in sec
    assert "--repo ipswitch8/PDFE-public" in sec
    assert "gh release create" not in sec, \
        "section 5 creates a release by hand again; use the helper"
    assert "v<tag>" not in sec and 'for Linux"' not in sec, \
        "section 5 tags or titles a separate Linux release again"
    assert "One version tag never covers two source trees" in sec


# Commands the doc tells a maintainer to run: `<script> --flag ...`, possibly
# continued over `\`-joined lines inside a fenced block.
_COMMAND_RE = re.compile(
    r"((?:tools|packaging)/[\w./-]+\.(?:sh|py))((?:[^\n`]*\\\n)*[^\n`]*)")
_FLAG_RE = re.compile(r"(?<![\w-])(--[a-z][a-z0-9-]*)")


def _doc_commands():
    out = []
    for m in _COMMAND_RE.finditer(_doc()):
        flags = set(_FLAG_RE.findall(m.group(2)))
        if flags:
            out.append((m.group(1), flags))
    return out


def test_the_doc_passes_flags_to_its_release_scripts():
    # Guards the guard: if the regex stopped matching, every flag test below
    # would pass vacuously.
    scripts = {s for s, _f in _doc_commands()}
    for s in ("tools/packaging/build-release-container.sh",
              "tools/packaging/build-linux-packages.sh",
              "tools/packaging/verify-linux-packages.sh",
              "tools/gate/check_release_asset_hygiene.py",
              "tools/ops/publish_snapshot.py",
              "tools/packaging/publish-linux-release.py"):
        assert s in scripts, "no flagged command for %s found in the doc" % s


@pytest.mark.parametrize("script,flags", _doc_commands(),
                         ids=lambda v: v if isinstance(v, str) else None)
def test_every_flag_the_doc_passes_exists_in_the_script(script, flags):
    # GitHub #406: the doc's commands must keep working. A renamed or removed
    # option fails here instead of in a maintainer's terminal mid-release.
    src = io.open(REPO / script, encoding="utf-8").read()
    if script.endswith(".sh"):
        missing = sorted(f for f in flags
                         if not re.search(r"^\s*%s\)" % re.escape(f), src, re.M))
    else:
        missing = sorted(f for f in flags
                         if not re.search(r"add_argument\(\s*[\"']%s[\"']"
                                          % re.escape(f), src))
    assert not missing, "%s does not accept %s, but docs/releasing-linux.md " \
                        "passes it" % (script, missing)
