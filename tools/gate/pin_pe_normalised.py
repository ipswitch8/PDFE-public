#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pin_pe_normalised.py -- pin PE images whose bytes legitimately move.

GitHub #298 (PDFE's own binary) and GitHub #177 (six self-timestamping shipped
DLLs). Both are the same problem: an artifact worth pinning that cannot be
pinned by a plain SHA-256, because a few of its bytes change on every build for
reasons that have nothing to do with what the code does.

WHY A PLAIN HASH IS NOT AN OPTION, MEASURED RATHER THAN ASSUMED
===============================================================

`tools/gate/vcpkg_cache_manifest.py` pins 7,364 installed dependency files and
EXCLUDES 274, in six measured categories (docs/security/vcpkg-cache-unpinned.json).
Two of those categories are what this script addresses:

  * `static-archive` (22 files) -- lib.exe has no /Brepro equivalent. #298's
    point is that a static archive is a LINK-TIME INPUT to pdfe.exe, so for that
    subset the cache-substitution risk does not vanish, it TRANSFERS to an
    artifact nothing pins. MuPDF, the largest static dependency here, is exactly
    that case.
  * `self-timestamping-port` (21 paths, 6 distinct libraries) -- the library
    embeds its OWN build timestamp above the toolchain layer, so no linker flag
    removes it. Measured in libcrypto-3-x64.dll: ONE string,
    `built on: Sat Oct  3 15:44:52 2026 UTC` at offset 4,165,600, and exactly one
    date-like run in the whole 5.3 MB image.

And for pdfe.exe itself, measured 2026-10-14 on this host, two links of
IDENTICAL objects:

    before any determinism flags   6,164 of 70,291,968 bytes differed
    with /Brepro /PDBALTPATH:%_PDB% /INCREMENTAL:NO
                                      69 of 57,795,072 bytes differed

Those 69 are not scattered noise. They are seven runs, and every one is a
debug-identity field:

    @288         4 bytes   the PE header COFF TimeDateStamp
    @20188628    4 bytes   debug directory entry TimeDateStamp
    @20188656    4 bytes   debug directory entry TimeDateStamp
    @20188684    4 bytes   debug directory entry TimeDateStamp
    @20188712    4 bytes   debug directory entry TimeDateStamp
    @20417636   17 bytes   `RSDS` + the CodeView PDB GUID
    @20418900   32 bytes   the same GUID again, plus the timestamp, in the
                           debug-info table

`/Brepro` derives the timestamp from content, so once the GUID moves the
timestamp follows -- which is why five timestamp slots differ for one underlying
cause. The GUID pairs the image with its PDB, and a PDB is not reproducible: the
same exclusion list records 142 `msvc-pdb` files differing between two otherwise
byte-identical builds, with the reason that /Brepro "makes the IMAGE
deterministic, not the symbol file".

WHAT THIS SCRIPT PINS INSTEAD
=============================

A SHA-256 over the image with the fields that legitimately move MASKED TO ZERO,
every one located by PARSING THE PE STRUCTURES -- never by a hard-coded offset,
because an offset is only true for one build and a pin keyed to one build is not
a pin. Masked:

  1. the COFF header's TimeDateStamp;
  2. the optional header's CheckSum (recomputed from content by the linker, so it
     follows the timestamp);
  3. every IMAGE_DEBUG_DIRECTORY entry's TimeDateStamp;
  4. the CodeView (RSDS) GUID and Age inside every CODEVIEW debug entry;
  5. the IMAGE_DEBUG_TYPE_REPRO payload -- the linker's own content hash, a
     derived identity of the image that moves with the GUID;
  6. optionally (`--mask-build-strings`), ASCII date-like runs of the form
     `built on: <Day Mon DD HH:MM:SS YYYY>` -- the libcrypto case. This is OFF by
     default, because masking a date-shaped byte run is a bigger hammer than
     masking a structurally located field, and it must be an explicit choice.

EVERYTHING ELSE IS HASHED. That is the property that makes this a security
control rather than a formality: code, data, resources, imports and the embedded
manifest are all covered, so a swapped DLL or a poisoned static archive changes
the normalised hash. `test_pe_normalised_pin.py` asserts exactly that in both
directions -- a byte flipped OUTSIDE the masked regions MUST change the hash, and
a forged timestamp MUST NOT.

WHAT THIS DOES NOT CLAIM
========================

Cross-machine reproducibility. `__FILE__` embeds the absolute source path, so two
builds under different roots differ even with all of the above; the triplet's
`/d1trimfile` treatment is the known remedy and is not applied to our own
compile. So the pin is valid for "the same tree, the same toolchain, repeated
builds" -- which is the property #298 asks for ("byte-identical output across
repeated builds") and no more.

ENTRIES ARE KEYED BY PATH, NOT BY FILENAME
==========================================

The same library ships at several paths with DIFFERENT content.
docs/security/vcpkg-cache-unpinned.json lists 21 paths for 6 distinct
self-timestamping libraries, and insists on being "EXPLICIT AND LITERAL --
no globs" for exactly this reason. A first version of this script keyed by
basename; its own test caught the consequence within minutes, reporting
jpeg62.dll and libcrypto-3-x64.dll as CHANGED when nothing had changed --
the verifier had found the copies deployed next to the application and
compared them against pins taken from the installed tree. A pin that fails
in the direction that looks like tampering is worse than no pin.

So keys are repo-relative paths with forward slashes, and --verify checks
each at its own path.

Usage:
    python tools/gate/pin_pe_normalised.py --hash <file> [--mask-build-strings]
    python tools/gate/pin_pe_normalised.py --write <manifest.json> <file>...
    python tools/gate/pin_pe_normalised.py --verify <manifest.json>
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# A `built on:` timestamp as libcrypto embeds it. Fixed WIDTH per field, so the
# masked span is the same length as what it replaces and the hash stays aligned.
_BUILT_ON = re.compile(
    rb"built on: (?:Mon|Tue|Wed|Thu|Fri|Sat|Sun) "
    rb"(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) "
    rb"[ 0-9]\d \d\d:\d\d:\d\d \d{4}")

IMAGE_DIRECTORY_ENTRY_DEBUG = 6
IMAGE_DEBUG_TYPE_CODEVIEW = 2
# IMAGE_DEBUG_TYPE_REPRO carries /Brepro's own 32-byte content hash. It is
# a DERIVED identity of the image -- including its debug identity -- not content
# of its own, so it moves whenever the CodeView GUID moves and has to be masked
# with it. Found by PARSING the debug directory of a real pair of links: the one
# uncovered run (32 bytes at file offset 20418900) sat inside entry [3], type 16,
# raw 20418896..20418932. Masking that by offset would have 'worked' and been
# wrong for every other build.
IMAGE_DEBUG_TYPE_REPRO = 16


def _repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))


def _repo_rel(p):
    """A stable, repo-relative, forward-slashed key for a pinned file."""
    ap = os.path.abspath(p)
    root = _repo_root()
    if ap.lower().startswith(root.lower() + os.sep):
        ap = ap[len(root) + 1:]
    return ap.replace(os.sep, "/")


class PeError(RuntimeError):
    pass


def _u16(b, o): return struct.unpack_from("<H", b, o)[0]
def _u32(b, o): return struct.unpack_from("<I", b, o)[0]


def masked_regions(data: bytes, mask_build_strings: bool = False):
    """Return [(offset, length, why)] for every field that legitimately moves.

    Raises PeError rather than guessing if the file is not a PE we understand: a
    pin computed over a misparsed image would be stable and meaningless.
    """
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise PeError("not a PE image (no MZ signature)")
    pe = _u32(data, 0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise PeError("not a PE image (no PE signature at e_lfanew)")

    coff = pe + 4
    n_sections = _u16(data, coff + 2)
    opt_size = _u16(data, coff + 16)
    opt = coff + 20

    out = [(coff + 4, 4, "coff-timedatestamp")]

    magic = _u16(data, opt)
    if magic == 0x20B:       # PE32+
        checksum_off, dd_off = opt + 64, opt + 112
    elif magic == 0x10B:     # PE32
        checksum_off, dd_off = opt + 64, opt + 96
    else:
        raise PeError("unknown optional-header magic 0x%X" % magic)
    out.append((checksum_off, 4, "optional-header-checksum"))

    n_dd = _u32(data, opt + (108 if magic == 0x20B else 92))
    if n_dd <= IMAGE_DIRECTORY_ENTRY_DEBUG:
        dbg_rva = dbg_size = 0
    else:
        e = dd_off + IMAGE_DIRECTORY_ENTRY_DEBUG * 8
        dbg_rva, dbg_size = _u32(data, e), _u32(data, e + 4)

    # Section table, to map the debug directory's RVA to a file offset.
    sections = []
    st = opt + opt_size
    for i in range(n_sections):
        s = st + i * 40
        if s + 40 > len(data):
            raise PeError("section table runs past end of file")
        sections.append((_u32(data, s + 12), _u32(data, s + 8),
                         _u32(data, s + 20), _u32(data, s + 16)))

    def rva_to_off(rva):
        for va, vsize, praw, sraw in sections:
            if va <= rva < va + max(vsize, sraw):
                delta = rva - va
                return praw + delta if delta < sraw else None
        return None

    if dbg_rva and dbg_size:
        base = rva_to_off(dbg_rva)
        if base is None:
            raise PeError("debug directory RVA 0x%X is not inside any section"
                          % dbg_rva)
        for i in range(dbg_size // 28):
            ent = base + i * 28
            if ent + 28 > len(data):
                raise PeError("debug directory entry runs past end of file")
            out.append((ent + 4, 4, "debug-entry-timedatestamp"))
            dtype = _u32(data, ent + 12)
            dsize = _u32(data, ent + 16)
            draw = _u32(data, ent + 24)
            if dtype == IMAGE_DEBUG_TYPE_REPRO and draw and dsize:
                out.append((draw, dsize, "repro-content-hash"))
            if dtype == IMAGE_DEBUG_TYPE_CODEVIEW and draw:
                if data[draw:draw + 4] == b"RSDS":
                    # RSDS | GUID(16) | Age(4) | PdbPath(asciiz).
                    # The PDB PATH is deliberately NOT masked: /PDBALTPATH
                    # reduces it to a basename, which is stable, and masking it
                    # would hide a changed PDB name.
                    out.append((draw + 4, 20, "codeview-guid-and-age"))

    if mask_build_strings:
        for m in _BUILT_ON.finditer(data):
            out.append((m.start(), m.end() - m.start(), "built-on-string"))

    return sorted(out)


def codeview_pdb_name(data: bytes):
    """The PDB name recorded in the image's CodeView (RSDS) debug entry.

    GitHub #304. Located STRUCTURALLY -- debug directory -> CODEVIEW entry ->
    RSDS -> skip GUID(16) and Age(4) -> NUL-terminated name -- not by scanning
    the file for the bytes `RSDS`. A scan can land in a string literal or in
    embedded data and would then report a name the loader never sees, which
    makes it useless as evidence about what the linker actually wrote.

    Returns None when the image carries no CodeView entry, which a caller must
    treat as "could not determine" rather than as a clean answer.
    """
    name = [None]

    def capture(off):
        end = data.find(b"\x00", off)
        if end < 0:
            raise PeError("CodeView PDB name is not NUL-terminated")
        name[0] = data[off:end].decode("utf-8", "replace")

    # masked_regions() already does the full structural walk and yields the
    # CodeView GUID+Age region at `draw + 4` with length 20; the name begins
    # immediately after it. Reusing that walk keeps ONE parser rather than a
    # second copy that could disagree about where the record is.
    for off, length, label in masked_regions(data):
        if label == "codeview-guid-and-age" and length == 20:
            capture(off + 20)
            break
    return name[0]


def normalised_sha256(path: str, mask_build_strings: bool = False):
    data = bytearray(io.open(path, "rb").read())
    regions = masked_regions(bytes(data), mask_build_strings)
    for off, ln, _why in regions:
        if off + ln > len(data):
            raise PeError("masked region @%d+%d past end of file" % (off, ln))
        data[off:off + ln] = b"\0" * ln
    return hashlib.sha256(bytes(data)).hexdigest(), regions


def _toolchain():
    """Recorded alongside the hashes, because a normalised hash is only
    comparable across builds with the SAME compiler -- the manifest for the
    vcpkg tree records the same thing for the same reason."""
    import platform
    import subprocess
    cl = ""
    try:
        p = subprocess.run(["cl"], capture_output=True, text=True)
        cl = ((p.stderr or "") + (p.stdout or "")).splitlines()[0].strip()
    except Exception:  # noqa: BLE001
        cl = "unavailable"
    return {"platform": platform.platform(), "cl": cl}


def cmd_hash(args):
    digest, regions = normalised_sha256(args.hash, args.mask_build_strings)
    print("%s  %s" % (digest, args.hash))
    total = sum(ln for _o, ln, _w in regions)
    print("masked %d region(s), %d byte(s):" % (len(regions), total))
    for off, ln, why in regions:
        print("  @%-10d %-4d %s" % (off, ln, why))
    return 0


def cmd_write(args):
    entries = {}
    for f in args.files:
        digest, regions = normalised_sha256(f, args.mask_build_strings)
        entries[_repo_rel(f)] = {
            "normalised_sha256": digest,
            "size": os.path.getsize(f),
            "masked_regions": [{"offset": o, "length": l, "why": w}
                               for o, l, w in regions],
            "mask_build_strings": bool(args.mask_build_strings),
        }
        print("pinned %s -> %s" % (f, digest[:16]))
    doc = {
        "_comment": (
            "Normalised SHA-256 pins for PE images whose bytes legitimately "
            "move. Produced by tools/gate/pin_pe_normalised.py; see that file "
            "for which fields are masked and why, and for the measurement "
            "showing a plain hash cannot work here. Valid for repeated builds "
            "of the same tree with the same toolchain -- NOT across machines, "
            "because __FILE__ embeds the absolute source path."),
        "toolchain": _toolchain(),
        "files": entries,
    }
    io.open(args.write, "w", encoding="utf-8").write(
        json.dumps(doc, indent=2, sort_keys=True) + "\n")
    print("wrote %s (%d file(s))" % (args.write, len(entries)))
    return 0


def cmd_verify(args):
    doc = json.load(io.open(args.verify, encoding="utf-8"))
    files = doc.get("files", {})
    if not files:
        print("FAIL: manifest %s pins nothing -- a vacuous pass is worse than "
              "no pin" % args.verify)
        return 1
    bad = missing = 0
    for name, rec in sorted(files.items()):
        # `name` IS the path, relative to the repo root. Searching for a
        # basename is what made a first version compare the wrong file.
        path = name if os.path.isfile(name) else None
        if path is None:
            cand = os.path.join(_repo_root(), name)
            path = cand if os.path.isfile(cand) else None
        if path is None:
            print("MISSING  %s" % name)
            missing += 1
            continue
        digest, _ = normalised_sha256(path, rec.get("mask_build_strings", False))
        if digest == rec["normalised_sha256"]:
            print("OK       %s" % name)
        else:
            print("CHANGED  %s\n  expected %s\n  actual   %s"
                  % (name, rec["normalised_sha256"], digest))
            bad += 1
    print("[pin-pe-normalised] %d pinned, %d changed, %d missing"
          % (len(files), bad, missing))
    if bad:
        print("FAIL: a pinned image's content changed outside the fields that "
              "legitimately move. That is the case this pin exists to catch.")
        return 1
    if missing and not args.allow_missing:
        print("FAIL: a pinned image was not found. Pass --allow-missing only if "
              "a partial install is expected.")
        return 1
    print("PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--hash", metavar="FILE")
    g.add_argument("--write", metavar="MANIFEST")
    g.add_argument("--verify", metavar="MANIFEST")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--search", action="append",
                    help="directory to look for pinned files in (--verify)")
    ap.add_argument("--mask-build-strings", action="store_true",
                    help="also mask `built on: <date>` runs (the libcrypto case)")
    ap.add_argument("--allow-missing", action="store_true")
    args = ap.parse_args(argv)
    try:
        if args.hash:
            return cmd_hash(args)
        if args.write:
            if not args.files:
                ap.error("--write needs at least one file")
            return cmd_write(args)
        return cmd_verify(args)
    except PeError as e:
        print("FAIL: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
