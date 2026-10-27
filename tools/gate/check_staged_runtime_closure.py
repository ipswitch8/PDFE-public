#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #281 — the staged DLL closure, checked against the real import tables.

THE CLAIM THIS MECHANISES. "A missing runtime DLL at load time raises a modal
dialog that blocks a CI job until the job's timeout." It is true and was
established by OBSERVATION only: on 2026-10-05 a Windows System Error modal

    pdfe_tests.exe - System Error
    The code execution cannot proceed because zlib1.dll was not found.

sat there until something killed the job. The `claim-check` gate raised it as an
unmechanised, load-bearing claim: nothing automated enforced the closure, and it
was the stated motivation for a commit.

WHY NOT THE EXISTING TEST. `tests/test_dll_dependencies.cpp` asserts that a
HAND-TYPED list of DLLs can be loaded -- Qt6Core, qpdf, tesseract, zlib and so
on, one `TEST` each. That is this corpus's highest-weight test-gap class,
**C-ENUM**: "completeness asserted from a hand-made enumeration". A dependency
absent from the list is indistinguishable from one that does not exist, so the
test's green means "the listed DLLs load", not "the closure is complete". The
DLL that actually caused the modal is only caught if somebody remembered to
type it.

WHAT THIS DOES INSTEAD. It reads the **import directory of the PE image** -- the
list the Windows loader itself walks -- and requires every imported name to be
either present in the staged directory or resolvable as a system DLL. The
population comes from the binary, so a new dependency is covered the moment it
is linked, with nobody remembering anything.

HOW "SYSTEM DLL" IS DECIDED, AND WHY NOT BY A LIST. A typed allow-list of
`KERNEL32.dll`, `USER32.dll`, ... would reintroduce exactly the defect being
removed one level down. Instead the question is answered by a **filesystem
probe** of the real system directories: a name found there is the loader's to
resolve, and anything else must be staged. That is measurable rather than
remembered, and it is why this script makes no claim to work off-Windows.

    python tools/gate/check_staged_runtime_closure.py --self-test
    python tools/gate/check_staged_runtime_closure.py \
        build/vs2022/bin/Debug/pdfe.exe build/vs2022/bin/Debug/pdfe_tests.exe
"""
from __future__ import annotations

import argparse
import io
import os
import struct
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

IMAGE_DIRECTORY_ENTRY_IMPORT = 1
IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT = 13


class PeError(Exception):
    """Raised when an image cannot be PARSED. Never caught into a pass.

    A closure check that treats an unreadable image as "no imports" would
    report a clean closure for a binary it never looked at, which is the
    false-green shape this whole pipeline exists to remove.
    """


def _u16(b: bytes, o: int) -> int:
    return struct.unpack_from("<H", b, o)[0]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def _cstr(data: bytes, off: int) -> str:
    end = data.find(b"\x00", off)
    if end < 0:
        raise PeError("unterminated import name at offset %d" % off)
    return data[off:end].decode("ascii", "replace")


def imported_dlls(path: Path) -> List[str]:
    """Every DLL named in the image's import and DELAY-import directories.

    Delay imports are included deliberately. A delay-loaded DLL that is absent
    does not fail at process start -- it fails at the first call, which is
    LATER and therefore harder to attribute. Leaving them out would make this
    check weakest exactly where the symptom is most confusing.
    """
    data = io.open(path, "rb").read()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise PeError("%s is not a PE image (no MZ header)" % path.name)
    pe = _u32(data, 0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise PeError("%s has no PE signature" % path.name)

    coff = pe + 4
    n_sections = _u16(data, coff + 2)
    opt_size = _u16(data, coff + 16)
    opt = coff + 20
    magic = _u16(data, opt)
    if magic not in (0x10B, 0x20B):
        raise PeError("%s: unrecognised optional-header magic 0x%X"
                      % (path.name, magic))
    dd_off = opt + (112 if magic == 0x20B else 96)
    n_dd = _u32(data, opt + (108 if magic == 0x20B else 92))

    sections = []
    st = opt + opt_size
    for i in range(n_sections):
        s = st + i * 40
        if s + 40 > len(data):
            raise PeError("%s: section table runs past end of file" % path.name)
        sections.append((_u32(data, s + 12), _u32(data, s + 8),
                         _u32(data, s + 20), _u32(data, s + 16)))

    def rva_to_off(rva: int) -> Optional[int]:
        for va, vsize, praw, sraw in sections:
            if va <= rva < va + max(vsize, sraw):
                delta = rva - va
                return praw + delta if delta < sraw else None
        return None

    names: List[str] = []
    for which, stride, name_field in ((IMAGE_DIRECTORY_ENTRY_IMPORT, 20, 12),
                                      (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
                                       32, 4)):
        if n_dd <= which:
            continue
        e = dd_off + which * 8
        rva, size = _u32(data, e), _u32(data, e + 4)
        if not rva or not size:
            continue
        base = rva_to_off(rva)
        if base is None:
            raise PeError("%s: import directory RVA 0x%X is in no section"
                          % (path.name, rva))
        for i in range(size // stride):
            ent = base + i * stride
            if ent + stride > len(data):
                break
            name_rva = _u32(data, ent + name_field)
            if not name_rva:
                break                      # the null terminator entry
            noff = rva_to_off(name_rva)
            if noff is None:
                continue
            names.append(_cstr(data, noff))
    # Case-insensitively unique, order preserved -- Windows filenames are.
    seen: Set[str] = set()
    out: List[str] = []
    for n in names:
        if n.lower() not in seen:
            seen.add(n.lower())
            out.append(n)
    return out


def system_dirs() -> List[Path]:
    """The directories the loader may resolve a system DLL from.

    Probed, not assumed: an absent directory is simply skipped, so this works
    the same on a host with no SysWOW64.
    """
    root = os.environ.get("SystemRoot") or os.environ.get("WINDIR") or ""
    cands = []
    if root:
        cands += [Path(root) / "System32", Path(root) / "SysWOW64",
                  Path(root)]
    return [p for p in cands if p.is_dir()]


def classify(name: str, staged: Path, sysdirs: Sequence[Path]) -> str:
    """`staged`, `system`, or `missing` for one imported DLL name.

    STAGED WINS over system on purpose. A DLL beside the executable is what the
    loader prefers for a non-KnownDLL, so if a name is both staged and present
    in System32 the staged copy is the one that will load, and reporting it as
    `system` would hide a staged-closure change.
    """
    # API SETS ARE NOT FILES. `api-ms-win-*` and `ext-ms-win-*` are virtual
    # contract names that the loader resolves through the API-set schema to a
    # real host library (ucrtbase, kernelbase, ...). They are never staged and
    # frequently have no file of that name anywhere on disk, so requiring them
    # to be staged or findable would report a missing dependency that cannot
    # exist. Measured: the first run of this check flagged
    # `api-ms-win-core-synch-l1-2-0.dll` against a staged directory that is
    # demonstrably complete, because `pdfe_tests.exe` runs.
    #
    # This is a PREFIX RULE over a documented, open-ended Windows convention,
    # not a list of specific names -- so a new API set is covered without
    # anyone editing anything. That distinction is the whole reason this file
    # exists; a typed roster of api-set names would reintroduce C-ENUM one
    # level down.
    low = name.lower()
    if low.startswith("api-ms-win-") or low.startswith("ext-ms-win-"):
        return "apiset"
    if (staged / name).is_file():
        return "staged"
    for d in sysdirs:
        try:
            if (d / name).is_file():
                return "system"
        except OSError:
            continue
    return "missing"


def check(images: Sequence[Path]) -> Tuple[int, List[str]]:
    lines: List[str] = []
    sysdirs = system_dirs()
    if os.name != "nt":
        return 0, ["[runtime-closure] SKIP: this check reads Windows PE import "
                   "tables and resolves system DLLs by filesystem probe; "
                   "neither is meaningful here."]
    if not sysdirs:
        return 1, ["[runtime-closure] ERROR: no system directory could be "
                   "located (SystemRoot/WINDIR unset or absent), so 'system' "
                   "cannot be distinguished from 'missing'. Refusing to "
                   "report a closure this check cannot actually judge."]

    total_missing: List[Tuple[str, str]] = []
    examined = 0
    for img in images:
        if not img.is_file():
            lines.append("[runtime-closure] ERROR: %s does not exist. Build it "
                         "rather than letting this check pass over an absent "
                         "image." % img)
            return 1, lines
        try:
            names = imported_dlls(img)
        except PeError as exc:
            lines.append("[runtime-closure] ERROR: %s" % exc)
            return 1, lines
        examined += 1
        staged = img.parent
        counts: Dict[str, int] = {"staged": 0, "system": 0,
                                  "apiset": 0, "missing": 0}
        for n in names:
            kind = classify(n, staged, sysdirs)
            counts[kind] += 1
            if kind == "missing":
                total_missing.append((img.name, n))
        lines.append("  %-18s imports=%-3d staged=%-3d system=%-3d "
                     "apiset=%-3d missing=%d"
                     % (img.name, len(names), counts["staged"],
                        counts["system"], counts["apiset"],
                        counts["missing"]))
        # An image with no imports at all is not a clean closure; it is a
        # parse that found nothing, and it must not read as a pass.
        if not names:
            lines.append("[runtime-closure] ERROR: %s declares NO imports, "
                         "which no linked Windows executable does. The import "
                         "directory was not found -- this is a parse failure "
                         "wearing a clean result." % img.name)
            return 1, lines

    lines.insert(0, "[runtime-closure] staged-closure check over %d image(s)"
                 % examined)
    if total_missing:
        lines.append("[runtime-closure] FAIL: %d import(s) resolve to neither "
                     "the staged directory nor a system directory. At load "
                     "time Windows raises a MODAL System Error dialog for "
                     "each, which on a CI runner blocks the job until its "
                     "timeout -- the failure mode GitHub #281 observed:"
                     % len(total_missing))
        for owner, name in total_missing:
            lines.append("    - %s imports %s, which is not staged" % (owner, name))
        return 1, lines

    lines.append("[runtime-closure] OK -- every imported DLL is staged beside "
                 "the image or resolvable as a system DLL.")
    return 0, lines


# --------------------------------------------------------------------------- #
# Self-test -- pins BOTH directions, and the refusals.
# --------------------------------------------------------------------------- #

def self_test() -> int:
    results: List[Tuple[str, bool, str]] = []

    def case(label: str, ok: bool, detail: str = "") -> None:
        results.append((label, bool(ok), detail))
        print(("  ok   " if ok else "  FAIL ") + label
              + (("  -- " + detail) if detail and not ok else ""))

    if os.name != "nt":
        print("[runtime-closure] self-test SKIP: Windows-only")
        return 0

    here = Path(__file__).resolve().parents[2]
    exe = None
    for cand in ("build/vs2022/bin/Debug/pdfe_tests.exe",
                 "build/vs2022/bin/Debug/pdfe.exe"):
        if (here / cand).is_file():
            exe = here / cand
            break

    # 1. A real image must yield a non-trivial import list. Without this the
    #    parser could return [] and every closure would look clean.
    if exe is None:
        case("a built image is available to parse", False,
             "neither pdfe.exe nor pdfe_tests.exe is built; the parser cannot "
             "be exercised against a real import table")
    else:
        names = imported_dlls(exe)
        case("a real PE image yields its import table",
             len(names) >= 5, "%d name(s): %s" % (len(names), names[:6]))
        case("the import list includes a staged (non-system) DLL",
             any(classify(n, exe.parent, system_dirs()) == "staged"
                 for n in names),
             "nothing resolved to the staged directory, so this check would "
             "pass even with an empty staged dir")

    # 2. A non-PE file must RAISE, not return [].
    with tempfile.TemporaryDirectory() as d:
        junk = Path(d) / "notapefile.exe"
        io.open(junk, "wb").write(b"this is not a PE image")
        try:
            imported_dlls(junk)
            case("a non-PE file is refused rather than read as import-free",
                 False, "no PeError raised")
        except PeError:
            case("a non-PE file is refused rather than read as import-free",
                 True)

    # 3. THE DIRECTION THAT MATTERS. A staged directory with a required DLL
    #    removed must FAIL. Built by copying the image alone into an empty
    #    directory: every non-system import is then missing by construction,
    #    which is the #281 condition exactly.
    if exe is not None:
        with tempfile.TemporaryDirectory() as d:
            lone = Path(d) / exe.name
            io.open(lone, "wb").write(io.open(exe, "rb").read())
            rc, out = check([lone])
            blob = "\n".join(out)
            case("an image with NOTHING staged beside it FAILS",
                 rc != 0 and "not staged" in blob, blob[-400:])
            case("...and the failure names the missing DLL rather than a count",
                 " imports " in blob, blob[-300:])

        # 4. SPECIFICITY. The real staged directory must pass, or the check
        #    reddens a healthy tree and gets switched off.
        rc, out = check([exe])
        case("the real staged directory passes", rc == 0, "\n".join(out))

    # 5. A missing image is an ERROR, not a pass.
    rc, out = check([Path("definitely-absent-image.exe")])
    case("an absent image ERRORS rather than passing",
         rc != 0 and "does not exist" in "\n".join(out), "\n".join(out))

    print("")
    failed = [r for r in results if not r[1]]
    if failed:
        print("[runtime-closure] SELF-TEST FAIL -- %d case(s)" % len(failed))
        return 1
    print("[runtime-closure] SELF-TEST PASS -- %d cases, both directions "
          "pinned" % len(results))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("images", nargs="*", help="PE images to check")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(list(argv) if argv is not None else None)
    if args.self_test:
        return self_test()
    if not args.images:
        ap.error("at least one image is required unless --self-test is given")
    rc, lines = check([Path(p) for p in args.images])
    for line in lines:
        print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
