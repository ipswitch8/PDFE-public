# -*- coding: utf-8 -*-
"""Pins for check_release_asset_hygiene.py's decision logic.

Offline: synthetic needle, no network, no 7-Zip. What these keep true: the
guard cannot report a clean release while an archive payload went unscanned,
while nothing was scanned at all, or while the release body fails to name the
corresponding source (GitHub #200 item 5) — and the wide-decode path genuinely
detects a UTF-16LE needle, since a control that only proves the narrow path
would leave half the scan vacuous.
"""
from __future__ import annotations

import hashlib
import importlib.util
import os

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(_HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rah = _load("check_release_asset_hygiene")
csr = _load("check_scrub_regressions")

NEEDLES = [{"label": "syn",
            "sha256": hashlib.sha256(b"zzsynth host").hexdigest(),
            "n": len("zzsynth host")}]


# --- scan_bytes: both decode paths are real ----------------------------------

def test_an_ascii_needle_is_found_via_the_latin1_decode():
    hits = rah.scan_bytes(b"path to zzsynth-host here", "w", NEEDLES,
                          csr.scan_text)
    assert ("w [latin-1]", "syn") in hits


def test_a_utf16le_needle_is_found_via_the_wide_decode():
    raw = "resource zzsynth_host string".encode("utf-16-le")
    hits = rah.scan_bytes(raw, "w", NEEDLES, csr.scan_text)
    assert ("w [utf-16-le]", "syn") in hits


def test_a_utf16le_needle_is_invisible_to_the_narrow_decode_alone():
    # The reason the wide path exists: NUL-interleaved bytes tokenise to
    # nothing under latin-1. If this ever starts matching, the wide decode can
    # be deleted; until then, removing it silently halves coverage.
    raw = "resource zzsynth_host string".encode("utf-16-le")
    hits = rah.scan_bytes(raw, "w", NEEDLES, csr.scan_text)
    assert ("w [latin-1]", "syn") not in hits


def test_clean_binaryish_bytes_do_not_hit():
    assert not rah.scan_bytes(b"\x00\x01PE\x00\x00 nothing", "w", NEEDLES,
                              csr.scan_text)


# --- the AGPL statement check -------------------------------------------------

def test_source_statement_needs_phrase_and_url_together():
    assert rah.source_statement_ok(
        "## Corresponding source\n"
        "The complete corresponding source for this release is "
        "https://github.com/o/r/tree/v1.0", "o/r")
    assert not rah.source_statement_ok("https://github.com/o/r", "o/r")
    assert not rah.source_statement_ok("the corresponding source exists",
                                       "o/r")
    assert not rah.source_statement_ok("", "o/r")
    assert not rah.source_statement_ok(None, "o/r")


def test_source_statement_is_case_insensitive_but_repo_exact():
    assert rah.source_statement_ok(
        "CORRESPONDING SOURCE: HTTPS://GITHUB.COM/O/R", "o/r")
    # a DIFFERENT repo's URL must not satisfy the offer
    assert not rah.source_statement_ok(
        "corresponding source: https://github.com/other/repo", "o/r")


# --- asset_verdict: the never-vacuous promise ---------------------------------

def test_verdict_refuses_zero_assets():
    assert rah.asset_verdict(0, [], [])[0] == "refuse"


def test_verdict_refuses_an_unextracted_payload():
    v, why = rah.asset_verdict(5, [], [("installer.exe", "7z exit 2")])
    assert v == "refuse"
    assert "UNSCANNED" in why


def test_verdict_leak_and_clean():
    assert rah.asset_verdict(5, [("a", "syn")], [])[0] == "leak"
    assert rah.asset_verdict(5, [], [])[0] == "clean"


# --- the three claim-check findings, pinned -----------------------------------

def test_a_partial_download_is_refused_not_scanned_smaller():
    # The branch that decides whether a missing asset silently shrinks the
    # scan population. Was inline glue in main() that no test reached.
    assert not rah.download_complete(3, 2)
    assert not rah.download_complete(3, 0)
    assert rah.download_complete(3, 3)
    assert not rah.download_complete(0, 0)   # a 0-asset "download" is never OK


def test_the_release_workflow_shape_is_pinned():
    good = ("on:\n"
            "  release:\n"
            "    types: [published, edited]\n"
            "  workflow_dispatch:\n"
            "jobs:\n"
            "  scan:\n"
            "    steps:\n"
            "      - run: python tools/gate/check_release_asset_hygiene.py "
            "--release \"$TAG\"\n")
    assert rah.workflow_shape_ok(good)
    # stripping the release trigger must be rejected
    assert not rah.workflow_shape_ok(good.replace(
        "  release:\n    types: [published, edited]\n", ""))
    # a workflow that fires but no longer runs the guard must be rejected
    assert not rah.workflow_shape_ok(good.replace(
        "check_release_asset_hygiene.py", "echo_skip.py"))
    assert not rah.workflow_shape_ok("")
    assert not rah.workflow_shape_ok(None)


def test_the_real_workflow_file_satisfies_the_shape():
    # The ratchet half: the shape check must hold on the REAL file, so a
    # deletion or trigger change reddens the guard's push-time self-test.
    import io as _io
    wf = os.path.join(os.path.dirname(os.path.dirname(_HERE)),
                      ".github", "workflows", "release-asset-hygiene.yml")
    assert rah.workflow_shape_ok(_io.open(wf, encoding="utf-8").read())


def test_the_corresponding_source_install_rule_is_pinned():
    assert rah.source_file_installed(
        'install(FILES "${CMAKE_SOURCE_DIR}/CORRESPONDING-SOURCE.txt" '
        'DESTINATION .)')
    assert not rah.source_file_installed(
        'install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION .)')
    assert not rah.source_file_installed("")
    assert not rah.source_file_installed(None)


def test_the_real_cmakelists_still_installs_the_source_offer():
    import io as _io
    cm = os.path.join(os.path.dirname(os.path.dirname(_HERE)),
                      "CMakeLists.txt")
    assert rah.source_file_installed(_io.open(cm, encoding="utf-8").read())


# --- the fast screen is never blinder than scan_text (v0.5.3) -----------------

def _screen_agrees(text, needles=NEEDLES):
    """The one property that makes the screen safe: scan_text never finds a
    needle in text the screen called clean."""
    found = bool(csr.scan_text(text, needles))
    screened = rah.could_hold_needle(text, needles)
    return (not found) or screened


def test_the_screen_passes_text_scan_text_would_flag(monkeypatch):
    import random
    rnd = random.Random(7)
    alphabet = "abcdefghijklmnopqrstuvwxyz0123456789 /\\-_.\n\r\t\x00\xe9"
    monkeypatch.setattr(rah, "_SCREEN_CHUNK", 37)   # force many chunk edges
    for _ in range(400):
        body = "".join(rnd.choice(alphabet) for _ in range(rnd.randint(0, 300)))
        cut = rnd.randint(0, len(body))
        sep = rnd.choice(["-", " ", "/", "\\", ".", "_", "\x00"])
        text = body[:cut] + " zzsynth" + sep + "HOST " + body[cut:]
        assert _screen_agrees(text), repr(text)
        assert rah.could_hold_needle(text, NEEDLES), repr(text)
        assert _screen_agrees(body), repr(body)


def test_the_screen_says_clean_for_clean_text():
    assert not rah.could_hold_needle("nothing here at all\n" * 1000, NEEDLES)
    assert not rah.could_hold_needle("", NEEDLES)
    # A token welded into a longer word is not a needle for scan_text, and is
    # not one for the screen either: the two tokenise the same way.
    assert not rah.could_hold_needle("xzzsynth hosty", NEEDLES)


def test_the_screen_finds_a_pair_split_across_a_chunk_edge(monkeypatch):
    monkeypatch.setattr(rah, "_SCREEN_CHUNK", 8)
    assert rah.could_hold_needle("aaaa zzsynth host bbbb", NEEDLES)


def test_scan_bytes_still_reports_line_hits_after_the_screen():
    assert rah.scan_bytes(b"one\ntwo zzsynth host\n", "w", NEEDLES,
                          csr.scan_text) == [("w [latin-1]", "syn")]


# --- Linux package readers: synthetic bytes, no tools needed ----------------

def _ar(members):
    out = b"!<arch>\n"
    for name, body in members:
        out += ("%-16s%-12s%-6s%-6s%-8s%-10d`\n"
                % (name, 0, 0, 0, 100644, len(body))).encode("ascii")
        out += body + (b"\n" if len(body) % 2 else b"")
    return out


def _tar_xz(files, links=()):
    import io as _io
    import lzma
    import tarfile
    buf = _io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for name, body in files:
            ti = tarfile.TarInfo(name)
            ti.size = len(body)
            tf.addfile(ti, _io.BytesIO(body))
        for name, target in links:
            ti = tarfile.TarInfo(name)
            ti.type = tarfile.SYMTYPE
            ti.linkname = target
            tf.addfile(ti)
    return lzma.compress(buf.getvalue())


def test_deb_members_opens_control_and_data_and_reads_links_as_text():
    deb = _ar([("debian-binary", b"2.0\n"),
               ("control.tar.xz", _tar_xz([("./control", b"Package: t\n")])),
               ("data.tar.xz", _tar_xz([("./opt/lib.so", b"x zzsynth host y")],
                                       [("./usr/bin/t", "../zzsynth-host/t")]))])
    members = dict(rah.deb_members(deb))
    assert members["data.tar.xz/./opt/lib.so"] == b"x zzsynth host y"
    assert members["data.tar.xz/./usr/bin/t -> (link)"] == b"../zzsynth-host/t"
    assert members["control.tar.xz/./control"] == b"Package: t\n"


def test_a_deb_without_data_is_refused():
    with pytest.raises(rah.PayloadError):
        rah.deb_members(_ar([("debian-binary", b"2.0\n")]))


def test_a_truncated_ar_member_is_refused():
    deb = _ar([("data.tar.xz", b"0123456789")])
    with pytest.raises(rah.PayloadError):
        rah.ar_members(deb[:-6])


def _cpio(entries):
    out = b""
    for name, mode, body in entries + [("TRAILER!!!", 0, b"")]:
        nb = name.encode("utf-8") + b"\0"
        hdr = b"070701" + b"".join(b"%08X" % v for v in (
            0, mode, 0, 0, 1, 0, len(body), 0, 0, 0, 0, len(nb), 0))
        rec = hdr + nb
        rec += b"\0" * ((4 - len(rec) % 4) % 4)
        rec += body
        rec += b"\0" * ((4 - len(rec) % 4) % 4)
        out += rec
    return out


def test_cpio_members_reads_files_and_link_targets():
    import stat as _st
    data = _cpio([("./a.so", _st.S_IFREG | 0o644, b"zzsynth host"),
                  ("./d", _st.S_IFDIR | 0o755, b""),
                  ("./l", _st.S_IFLNK | 0o777, b"../zzsynth-host")])
    assert rah.cpio_members(data) == [("./a.so", b"zzsynth host"),
                                      ("./l -> (link)", b"../zzsynth-host")]


def test_a_cpio_without_a_trailer_is_refused():
    import stat as _st
    data = _cpio([("./a", _st.S_IFREG | 0o644, b"abc")])
    with pytest.raises(rah.PayloadError):
        rah.cpio_members(data[: data.index(b"TRAILER") - 110])


def _rpm_header(strings):
    """An rpm header structure holding only string tags."""
    import struct
    store, index = b"", b""
    for tag, value in strings:
        index += struct.pack(">IIII", tag, 6, len(store), 1)
        store += value.encode("utf-8") + b"\0"
    return (b"\x8e\xad\xe8\x01" + b"\0" * 4
            + struct.pack(">II", len(strings), len(store)) + index + store)


def _rpm(payload, compressor):
    lead = b"\xed\xab\xee\xdb" + b"\0" * 92
    sig = _rpm_header([(1000, "s")])
    sig += b"\0" * ((8 - (len(lead) + len(sig)) % 8) % 8)
    hdr = _rpm_header([(1124, "cpio"), (1125, compressor)])
    return lead + sig + hdr + payload


def test_rpm_members_follows_the_header_to_the_named_compressor():
    import lzma
    import stat as _st
    cpio = _cpio([("./opt/a.so", _st.S_IFREG | 0o644, b"zzsynth host")])
    members = rah.rpm_members(_rpm(lzma.compress(cpio), "xz"))
    assert members == [("./opt/a.so", b"zzsynth host")]


def test_an_rpm_with_an_unknown_compressor_is_refused():
    with pytest.raises(rah.PayloadError) as e:
        rah.rpm_members(_rpm(b"????", "lz77-of-the-future"))
    assert "unsupported payload compression" in str(e.value)


def test_gzip_and_bzip2_payloads_are_decompressed():
    import bz2
    import gzip
    body = b"x zzsynth host y" * 10
    assert rah.decompress(gzip.compress(body), "gzip") == body
    assert rah.decompress(gzip.compress(body), "gz") == body
    assert rah.decompress(bz2.compress(body), "bzip2") == body
    assert rah.decompress(bz2.compress(body), "bz2") == body
    assert rah.decompress(body, "none") == body


def test_corrupt_gzip_and_bzip2_payloads_are_refused():
    for kind in ("gzip", "bzip2"):
        with pytest.raises(rah.PayloadError):
            rah.decompress(b"definitely not compressed", kind)


def test_a_corrupt_xz_payload_is_refused_not_passed():
    with pytest.raises(rah.PayloadError):
        rah.decompress(b"\xfd7zXZ\x00garbage", "xz")


def test_the_appimage_offset_is_where_the_elf_section_headers_end():
    import struct
    head = bytearray(64)
    head[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", head, 0x28, 1000)
    struct.pack_into("<HH", head, 0x3A, 64, 3)
    assert rah.appimage_squashfs_offset(bytes(head)) == 1000 + 64 * 3
    assert rah.appimage_squashfs_offset(b"MZ" + bytes(62)) is None
    head[4] = 1                      # ELF32 is not a type-2 x86_64 AppImage
    assert rah.appimage_squashfs_offset(bytes(head)) is None


# --- the sensitivity control off Windows ------------------------------------

def test_the_windows_probe_is_the_host_name():
    probe, what = rah.live_identity_probe({"COMPUTERNAME": "ZZHOSTNAME"},
                                          platform="win32")
    assert what == "COMPUTERNAME" and "ZZHOSTNAME" in probe
    assert rah.live_identity_probe({}, platform="win32")[0] is None


def test_the_linux_probe_is_the_login_name_in_a_home_path():
    probe, what = rah.live_identity_probe({}, login="zzsynth-host",
                                          platform="linux")
    assert what == "login name"
    assert csr.scan_text(probe, NEEDLES), \
        "the probe must carry the login the way a Linux build leaks it"
    assert rah.live_identity_probe({}, login="", platform="linux")[0] is None


def test_the_control_refuses_when_the_inventory_does_not_know_the_login(
        monkeypatch):
    real = rah.live_identity_probe

    def probe_for(login):
        return lambda: real({}, login=login, platform="linux")
    monkeypatch.setattr(rah, "live_identity_probe", probe_for("zzother-acct"))
    assert rah.run_controls(csr, NEEDLES) is False
    monkeypatch.setattr(rah, "live_identity_probe", probe_for("zzsynth-host"))
    assert rah.run_controls(csr, NEEDLES) is True
