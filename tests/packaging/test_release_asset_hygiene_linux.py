# -*- coding: utf-8 -*-
"""The release-asset hygiene scanner opens the Linux release formats
(tools/gate/check_release_asset_hygiene.py, v0.5.3, GitHub #390).

Each format is built for real: a .deb and an .rpm by
tools/packaging/build-linux-packages.sh, and an AppImage as an ELF runtime
followed by a squashfs image (mksquashfs). A synthetic needle is planted
inside a compressed member.

For each format these tests check four things:
- the scanner finds the planted needle;
- a raw-bytes scan alone would NOT have found it, which is why extraction is
  needed at all;
- the clean twin passes;
- a corrupt copy fails closed.

The needle is synthetic (`zzsynth host`), so no real identifier appears in
the fixtures, the tests or their output.

Run: python3 -m pytest -q tests/packaging/test_release_asset_hygiene_linux.py
"""
import hashlib
import importlib.util
import io
import os
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from conftest import REPO, build_packages, make_appdir

NEEDLES = [{"label": "syn", "sha256": hashlib.sha256(b"zzsynth host").hexdigest()}]
LEAK = b"\x7fELF built in /home/zzsynth-host/src/lib.c\n"
CLEAN = b"\x7fELF built in /work/src/lib.c\n"


def _load(name):
    p = REPO / "tools" / "gate" / (name + ".py")
    spec = importlib.util.spec_from_file_location(name + "_lx", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rah = _load("check_release_asset_hygiene")
csr = _load("check_scrub_regressions")


def scan(path):
    """(files scanned, hits, extraction failures) for one asset."""
    hits, failures = [], []
    n = rah.scan_asset(str(path), Path(path).name, NEEDLES, csr, hits, failures)
    return n, hits, failures


def raw_only_hits(path):
    return rah.scan_bytes(Path(path).read_bytes(), "raw", NEEDLES, csr.scan_text)


@pytest.fixture(scope="module")
def leaky(tmp_path_factory):
    _appdir, out = build_packages(tmp_path_factory.mktemp("leaky"), library_bytes=LEAK)
    return out


@pytest.fixture(scope="module")
def clean(tmp_path_factory):
    _appdir, out = build_packages(tmp_path_factory.mktemp("clean"), library_bytes=CLEAN)
    return out


def make_appimage(tmp, library_bytes, symlink_target=None):
    appdir = make_appdir(tmp / "in", library_bytes=library_bytes)
    if symlink_target:
        os.symlink(symlink_target, appdir / "usr" / "lib" / "liblink.so")
    img = tmp / "image.squashfs"
    subprocess.run(["mksquashfs", str(appdir), str(img), "-quiet", "-noappend",
                    "-all-root", "-comp", "zstd"], check=True)
    runtime = Path(os.path.realpath(shutil.which("true"))).read_bytes()
    out = tmp / "PDFE-9.8.7-x86_64.AppImage"
    out.write_bytes(runtime + img.read_bytes())
    return out


# --- .deb ---------------------------------------------------------------------

def test_a_leak_inside_a_deb_is_found_in_the_payload(leaky):
    deb = leaky / "pdfe_9.8.7_amd64.deb"
    assert raw_only_hits(deb) == [], "the planted needle must be invisible raw"
    n, hits, failures = scan(deb)
    assert failures == []
    assert any("/payload/data.tar.xz/" in w and w.endswith("libstub.so.1 [latin-1]")
               for w, _lab in hits), hits
    assert n > 10


def test_a_clean_deb_passes_with_its_payload_counted(clean):
    n, hits, failures = scan(clean / "pdfe_9.8.7_amd64.deb")
    assert (hits, failures) == ([], [])
    assert n > 10, "the members must be counted, not just the outer file"


def test_a_truncated_deb_fails_closed(clean, tmp_path):
    data = (clean / "pdfe_9.8.7_amd64.deb").read_bytes()
    bad = tmp_path / "pdfe_cut_amd64.deb"
    bad.write_bytes(data[: len(data) // 2])
    _n, _hits, failures = scan(bad)
    assert failures and rah.asset_verdict(1, [], failures)[0] == "refuse"


def test_a_zstd_deb_is_read_too(tmp_path):
    root = tmp_path / "root"
    (root / "DEBIAN").mkdir(parents=True)
    (root / "opt").mkdir()
    (root / "opt" / "lib.so").write_bytes(LEAK)
    (root / "DEBIAN" / "control").write_text(
        "Package: z\nVersion: 1\nArchitecture: amd64\nMaintainer: t <t@t>\n"
        "Description: t\n", encoding="utf-8")
    deb = tmp_path / "z.deb"
    subprocess.run(["dpkg-deb", "--root-owner-group", "-Zzstd", "--build",
                    str(root), str(deb)], check=True, capture_output=True)
    _n, hits, failures = scan(deb)
    assert failures == [] and any("data.tar.zst/" in w for w, _ in hits)


def test_an_unknown_deb_compression_fails_closed(tmp_path):
    def ar(members):
        out = b"!<arch>\n"
        for name, body in members:
            out += ("%-16s%-12s%-6s%-6s%-8s%-10d`\n"
                    % (name, 0, 0, 0, 100644, len(body))).encode("ascii")
            out += body + (b"\n" if len(body) % 2 else b"")
        return out
    deb = tmp_path / "odd.deb"
    deb.write_bytes(ar([("debian-binary", b"2.0\n"), ("data.tar.lz4", b"\x04\x22M\x18")]))
    _n, _hits, failures = scan(deb)
    assert failures and "unsupported payload compression" in failures[0][1]


# --- .rpm -----------------------------------------------------------------------

def test_a_leak_inside_an_rpm_is_found_in_the_payload(leaky):
    rpm = leaky / "pdfe-9.8.7-1.x86_64.rpm"
    assert raw_only_hits(rpm) == [], "the planted needle must be invisible raw"
    n, hits, failures = scan(rpm)
    assert failures == []
    assert any(w.endswith("opt/pdfe/lib/libstub.so.1 [latin-1]") for w, _ in hits), hits
    assert n > 10


def test_a_clean_rpm_passes_with_its_payload_counted(clean):
    n, hits, failures = scan(clean / "pdfe-9.8.7-1.x86_64.rpm")
    assert (hits, failures) == ([], [])
    assert n > 10


def test_a_corrupt_rpm_payload_fails_closed(clean, tmp_path):
    data = bytearray((clean / "pdfe-9.8.7-1.x86_64.rpm").read_bytes())
    data[-4096:] = bytes(4096)          # wipe the end of the xz stream
    bad = tmp_path / "bad.rpm"
    bad.write_bytes(bytes(data))
    _n, _hits, failures = scan(bad)
    assert failures, "a damaged payload must not pass as clean"


def test_an_rpm_cut_inside_its_header_fails_closed(clean, tmp_path):
    data = (clean / "pdfe-9.8.7-1.x86_64.rpm").read_bytes()
    bad = tmp_path / "cut.rpm"
    bad.write_bytes(data[:100])        # the lead plus the start of a header
    _n, _hits, failures = scan(bad)
    assert failures and "rpm header" in failures[0][1]


def test_a_file_that_is_not_an_rpm_fails_closed(tmp_path):
    bad = tmp_path / "not.rpm"
    bad.write_bytes(b"\x00" * 200)
    _n, _hits, failures = scan(bad)
    assert failures and "bad lead" in failures[0][1]


# --- .AppImage ------------------------------------------------------------------

def test_a_leak_inside_an_appimage_is_found_in_the_squashfs(tmp_path):
    # Padded with compressible text: squashfs stores a block uncompressed
    # when compression would not shrink it, and a 45-byte file would sit in
    # the image in the clear -- which would make the raw-scan control below
    # meaningless rather than prove anything.
    ai = make_appimage(tmp_path, LEAK + b"lorem ipsum dolor sit amet " * 4000)
    assert raw_only_hits(ai) == [], "the planted needle must be invisible raw"
    n, hits, failures = scan(ai)
    assert failures == []
    assert any(w.endswith("usr/lib/libstub.so.1 [latin-1]") for w, _ in hits), hits
    assert n > 5


def test_a_clean_appimage_passes(tmp_path):
    n, hits, failures = scan(make_appimage(tmp_path, CLEAN))
    assert (hits, failures) == ([], []) and n > 5


def test_a_symlink_is_scanned_as_text_and_never_followed(tmp_path):
    """The target names the needle and does not exist. A scanner that
    followed links would fail to read it, or would read a file on this
    machine; reading the link text is the only correct behaviour."""
    ai = make_appimage(tmp_path, CLEAN, symlink_target="/nonexistent/zzsynth-host/lib.so")
    _n, hits, failures = scan(ai)
    assert failures == []
    assert any(w.endswith("liblink.so -> (link) [latin-1]") for w, _ in hits), hits


def test_an_appimage_whose_image_is_damaged_fails_closed(tmp_path):
    ai = make_appimage(tmp_path, CLEAN)
    data = ai.read_bytes()
    off = rah.appimage_squashfs_offset(data[:64])
    ai.write_bytes(data[: off + 200])
    _n, _hits, failures = scan(ai)
    assert failures, "a truncated squashfs must not pass as clean"


def test_an_elf_without_an_image_fails_closed(tmp_path):
    ai = tmp_path / "Fake.AppImage"
    ai.write_bytes(Path(os.path.realpath(shutil.which("true"))).read_bytes())
    _n, _hits, failures = scan(ai)
    assert failures and "no squashfs image" in failures[0][1]
