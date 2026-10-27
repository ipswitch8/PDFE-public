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
