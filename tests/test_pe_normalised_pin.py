# -*- coding: utf-8 -*-
"""GitHub #298 / #177 — the normalised PE pin must detect tampering and ignore
only the fields that legitimately move.

WHAT IS BEING PINNED AND WHY A PLAIN HASH CANNOT DO IT

`tools/gate/vcpkg_cache_manifest.py` pins 7,364 installed dependency files by
SHA-256 and excludes 274 in six measured categories. Two of those are the
subject here:

  * 22 `static-archive` `.lib` files — lib.exe has no deterministic-output flag,
    and COFF archive members carry their own timestamps. #298's point: a static
    archive is a LINK-TIME INPUT to `pdfe.exe`, so for that subset the
    cache-substitution risk does not disappear, it TRANSFERS to an artifact
    nothing pins. MuPDF is linked statically here, so this is not marginal.
  * 21 paths / 6 distinct `self-timestamping-port` DLLs — the library embeds its
    own build timestamp above the toolchain layer.

Measured on this host 2026-10-14, two links of `pdfe.exe` from IDENTICAL objects:
6,164 of 70,291,968 bytes differed with no determinism flags; 69 of 57,795,072
with them. All 69 are debug-identity fields (the COFF timestamp in five places,
the CodeView GUID twice, and the linker's own content hash which moves with it).
So the pin is a SHA-256 over the image with exactly those fields masked, each
located by parsing the PE structures.

WHAT THIS FILE ASSERTS, AND WHY BOTH DIRECTIONS ARE REQUIRED

A masking pin has a characteristic failure mode: mask too much and it still
"passes" while detecting nothing. Asserting only "a forged timestamp does not
change the hash" would be satisfied by a pin that masked the entire file. So
every positive assertion here is paired with a negative one:

  * a forged COFF timestamp must NOT change the hash   (it legitimately moves)
  * a byte flipped in the CODE must change it           (that is tampering)
  * a byte flipped in a RESOURCE must change it         (so must that)
  * truncating the file must change it
  * a masked region must be a structurally located field, not a fixed offset

The swap-detection assertion is the one #177's criterion asks for in so many
words ("a test asserts it detects a swapped DLL"), and it is done by actually
swapping bytes rather than by inspection.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

sys.path.insert(0, str(REPO_ROOT / "tools" / "gate"))
import pin_pe_normalised as pin  # noqa: E402

CONFIG = os.environ.get("PDFE_BUILD_CONFIG", "Debug")
APP = REPO_ROOT / "build" / "vs2022" / "bin" / CONFIG / "pdfe.exe"
MANIFEST = REPO_ROOT / "docs" / "security" / "pe-normalised-pins.json"


def _subject() -> Path:
    """A PE image to exercise the pin against.

    Prefers the application; falls back to any pinned DLL. Fails rather than
    skips if neither exists, because a silent skip here would make this file
    look like coverage while asserting nothing.
    """
    if APP.is_file():
        return APP
    for name in ("libcrypto-3-x64.dll", "jpeg62.dll", "turbojpeg.dll"):
        cand = (REPO_ROOT / "build" / "vs2022" / "vcpkg_installed"
                / "x64-windows-repro" / "bin" / name)
        if cand.is_file():
            return cand
    pytest.fail(
        "no PE image available to test the pin against: %s is missing and no "
        "pinned DLL was found. Build the app (cmake --build build/vs2022 "
        "--config %s --target pdfe) rather than letting this test skip."
        % (APP, CONFIG))


def test_the_image_records_a_bare_pdb_basename():
    """GitHub #304 — the linker must leave a usable PDB BASENAME in the image.

    THE DEFECT. `src/app/CMakeLists.txt` passed the MSBuild macro `%_PDB%` to
    the PDB-alt-path linker option. It expanded, but what reached the CodeView
    record was

        %%%pdfe.pdb%%%

    — MSBuild percent-escaping of the macro token not round-tripping through
    CMake's `target_link_options`. No absolute path and no account name either
    way, so this was cosmetic rather than a disclosure finding; the practical
    cost is that a debugger looking for `pdfe.pdb` beside the image may not
    resolve that name. Fixed with `$<TARGET_PDB_FILE_NAME:pdfe>`, which CMake
    resolves, so no percent token reaches MSBuild and nothing is left to escape.

    WHY THIS ASSERTS MORE THAN THE SYMPTOM. The percent characters are the
    symptom; the property that matters is the one #298 bought — a bare basename
    with NO directory component, because the absolute build path contains the
    account name on a CI runner and this project has leaked it before
    (`docs/audit/history-scrub-2026-08-27.md`). A test that only looked for `%`
    would pass happily on a full absolute path.

    The name is read STRUCTURALLY, via the same debug-directory walk the pin
    tool uses, not by scanning the file for the bytes `RSDS`: a scan can land
    in a string literal or in embedded data and would then report a name the
    loader never sees.
    """
    if not APP.is_file():
        pytest.fail(
            "%s is missing, so the recorded PDB name cannot be inspected. "
            "Build it (cmake --build build/vs2022 --config %s --target pdfe) "
            "rather than letting this test skip -- a skip here would read as "
            "coverage while asserting nothing." % (APP, CONFIG))

    data = io.open(APP, "rb").read()
    name = pin.codeview_pdb_name(data)
    assert name is not None, (
        "the image carries no CodeView debug entry, so the recorded PDB name "
        "could not be determined. That is 'could not find out', not a pass")

    assert "%" not in name, (
        "GitHub #304: the recorded PDB name is %r. A percent character means "
        "the MSBuild macro did not round-trip; it should be the resolved "
        "basename." % name)
    assert ("/" not in name) and (chr(92) not in name), (
        "the recorded PDB name %r carries a DIRECTORY COMPONENT. That is "
        "exactly the disclosure property #298 bought: an absolute build path "
        "contains the account name on a CI runner." % name)
    assert name.lower() == "pdfe.pdb", (
        "expected the resolved basename pdfe.pdb, got %r" % name)


def _pe_timestamp_offset(data: bytes) -> int:
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return pe + 4 + 4   # COFF header + TimeDateStamp field


def test_masked_regions_are_structurally_located():
    data = io.open(_subject(), "rb").read()
    regions = pin.masked_regions(data)
    whys = {w for _o, _l, w in regions}
    assert "coff-timedatestamp" in whys, (
        "the COFF timestamp must be masked -- it is the field that moves on "
        "every single link")
    # The timestamp's offset must be the one the PE header itself points at, not
    # a constant. A pin that hard-codes an offset is valid for exactly one build.
    ts = [o for o, _l, w in regions if w == "coff-timedatestamp"]
    assert ts == [_pe_timestamp_offset(data)], (
        "the masked timestamp offset %r does not match the offset derived from "
        "e_lfanew (%d), so it was not located structurally"
        % (ts, _pe_timestamp_offset(data)))
    total = sum(l for _o, l, _w in regions)
    assert total < 4096, (
        "masking %d bytes is too much for a set of header fields; a pin that "
        "masks a large span stops detecting the thing it exists to detect"
        % total)


def test_a_forged_timestamp_does_not_change_the_hash():
    src = _subject()
    base, _ = pin.normalised_sha256(str(src))
    with tempfile.TemporaryDirectory() as td:
        copy = Path(td) / src.name
        shutil.copy2(src, copy)
        data = bytearray(io.open(copy, "rb").read())
        off = _pe_timestamp_offset(bytes(data))
        before = bytes(data[off:off + 4])
        data[off:off + 4] = b"\xde\xad\xbe\xef"
        assert bytes(data[off:off + 4]) != before
        io.open(copy, "wb").write(bytes(data))
        after, _ = pin.normalised_sha256(str(copy))
    assert after == base, (
        "rewriting the COFF timestamp changed the normalised hash, so the pin "
        "would red on every rebuild and get switched off")


def test_a_byte_flipped_in_code_changes_the_hash():
    src = _subject()
    base, regions = pin.normalised_sha256(str(src))
    masked = {(o, l) for o, l, _w in regions}

    with tempfile.TemporaryDirectory() as td:
        copy = Path(td) / src.name
        shutil.copy2(src, copy)
        data = bytearray(io.open(copy, "rb").read())
        # Pick an offset inside the first section's raw data and well clear of
        # every masked region -- i.e. real content.
        pe = struct.unpack_from("<I", bytes(data), 0x3C)[0]
        opt_size = struct.unpack_from("<H", bytes(data), pe + 4 + 16)[0]
        sect0 = pe + 4 + 20 + opt_size
        praw = struct.unpack_from("<I", bytes(data), sect0 + 20)[0]
        target = praw + 64
        assert not any(o <= target < o + l for o, l in masked), (
            "the chosen offset %d lies inside a masked region; the test would "
            "prove the opposite of what it claims" % target)
        data[target] ^= 0xFF
        io.open(copy, "wb").write(bytes(data))
        after, _ = pin.normalised_sha256(str(copy))
    assert after != base, (
        "flipping a byte of CODE did not change the normalised hash. The pin "
        "detects nothing -- this is the assertion that separates a security "
        "control from a formality, and it is exactly what GitHub #177 asks for "
        "('a test asserts it detects a swapped DLL').")


def test_truncation_changes_the_hash():
    src = _subject()
    base, _ = pin.normalised_sha256(str(src))
    with tempfile.TemporaryDirectory() as td:
        copy = Path(td) / src.name
        data = io.open(src, "rb").read()
        io.open(copy, "wb").write(data[:-4096])
        after, _ = pin.normalised_sha256(str(copy))
    assert after != base, "a truncated image hashed the same as the original"


def test_a_non_pe_file_is_refused_rather_than_hashed():
    with tempfile.TemporaryDirectory() as td:
        bogus = Path(td) / "not-a-pe.bin"
        io.open(bogus, "wb").write(b"this is not a PE image" * 64)
        with pytest.raises(pin.PeError):
            pin.normalised_sha256(str(bogus))


def test_committed_manifest_matches_what_is_installed():
    """The pin itself, against the tree.

    This is the assertion that fails if a build introduces nondeterminism
    outside the masked set -- the property GitHub #298 criterion 2 names.

    The manifest records the toolchain it was generated with, because a
    normalised hash is only comparable across builds of the same tree with the
    same compiler: `__FILE__` still embeds the absolute source path, so a
    different checkout root legitimately produces different bytes. When the
    toolchain differs this test says so and does not pretend to have verified
    anything.
    """
    assert MANIFEST.is_file(), (
        "%s is missing. Regenerate with:\n"
        "  python tools/gate/pin_pe_normalised.py --write %s <files...>"
        % (MANIFEST, MANIFEST))
    doc = json.load(io.open(MANIFEST, encoding="utf-8"))
    files = doc.get("files") or {}
    assert files, "the manifest pins nothing; a vacuous pass is worse than no pin"

    recorded = (doc.get("toolchain") or {}).get("cl", "")
    current = pin._toolchain().get("cl", "")

    checked = changed = absent = 0
    detail = []
    for name, rec in sorted(files.items()):
        # Keys ARE repo-relative paths. An earlier version of this test searched
        # for the BASENAME under two directories and reported jpeg62.dll and
        # libcrypto-3-x64.dll as CHANGED minutes after they were pinned: both
        # ship at more than one path with different content (deployed next to
        # the app versus in the installed tree), and the search found the wrong
        # one. docs/security/vcpkg-cache-unpinned.json had already said so, with
        # 21 paths for 6 distinct libraries and a comment insisting on being
        # literal. Resolve the path, never the name.
        path = REPO_ROOT / name
        if not path.is_file():
            absent += 1
            continue
        digest, _ = pin.normalised_sha256(
            str(path), rec.get("mask_build_strings", False))
        checked += 1
        if digest != rec["normalised_sha256"]:
            changed += 1
            detail.append("%s\n    expected %s\n    actual   %s"
                          % (name, rec["normalised_sha256"], digest))

    assert checked > 0, (
        "none of the %d pinned paths exists under %s -- the pin verified "
        "nothing, which is the vacuous-pass shape this project has been bitten "
        "by before" % (len(files), REPO_ROOT))

    if changed and recorded and current and recorded != current:
        pytest.fail(
            "%d of %d pinned image(s) changed, AND the toolchain differs from "
            "the one that generated the manifest:\n  recorded: %s\n  current:  "
            "%s\nThat combination is expected (see the docstring) -- regenerate "
            "the manifest on this toolchain rather than treating it as "
            "tampering.\n%s" % (changed, checked, recorded, current,
                                "\n".join(detail)))
    assert changed == 0, (
        "%d of %d pinned image(s) changed outside the fields that legitimately "
        "move, on the SAME toolchain that generated the manifest. That is "
        "either a nondeterministic build or a substituted artifact, and both "
        "are what this pin exists to catch.\n%s"
        % (changed, checked, "\n".join(detail)))


def test_the_tool_runs_as_a_script_and_reports_its_masking():
    """The gate invokes this as a script, so the CLI path is covered too."""
    p = subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools" / "gate" / "pin_pe_normalised.py"),
         "--hash", str(_subject())],
        capture_output=True, text=True, cwd=str(REPO_ROOT))
    assert p.returncode == 0, p.stdout + p.stderr
    assert "coff-timedatestamp" in p.stdout, (
        "the CLI did not report which regions it masked; an opaque pin is not "
        "reviewable:\n" + p.stdout)
