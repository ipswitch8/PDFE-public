#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Is `check_win87_publish_shapes.py`'s SHAPES list complete? (GitHub #254)

THE GAP THIS CLOSES
-------------------
C-ENUM is this corpus's highest-weight test-gap class: "completeness asserted
from a hand-made enumeration". The population a control operates over is a list
somebody typed, so a subject ABSENT from the list is indistinguishable from one
that does not exist, and the control's green reads as "class closed" when it
means "the listed instances are closed".

`check_win87_publish_shapes.py` has already demonstrated that failure once, and
its own source records it: a shape was added to `SHAPES` after a security-audit
gate pointed out that only the `std::filesystem` spelling of copy was covered --
"and the RAW Win32 one is the shape that was actually EXPLOITED". The remedy
applied then was to type one more entry. The list grew; the property stayed
unmeasured.

#254 says plainly that "every API that publishes a file" is not lexically
enumerable without a model of what each API does, and that the issue exists to
track the limitation rather than to pretend it has an obvious fix. It also names
the partial improvement that IS available, and that nobody had scoped:

    a check that the shape list is complete with respect to some external
    reference, so that adding a new publish call to the tree without adding its
    shape is detectable.

This is that check.

WHAT IT ACTUALLY MEASURES, AND WHAT IT DOES NOT
-----------------------------------------------
It holds a REFERENCE SET of file-publishing APIs -- the Win32 file-management
surface plus the C++/Qt/CRT spellings -- and asks two questions:

  1. COVERAGE. For every reference API that actually appears in the scanned
     tree, is there a SHAPES entry that matches it? If a publish call is added
     to the tree in a spelling SHAPES does not know, this fails and names it.
     That is the C-ENUM hole, closed for the reference surface.

  2. LIVENESS. Does every SHAPES entry still correspond to a reference API? A
     shape that no longer matches anything in the reference set is either a typo
     or a dead entry, and a dead entry in a completeness list is how a list
     silently stops meaning what it says.

This does NOT make the population derived in the strong sense. The reference set
is itself typed, so the honest claim is narrower: the shape list is no longer
checked only against itself. A publish API outside the reference set remains
invisible to both -- and that residue is exactly what #254 says cannot be fixed
lexically. Stated here rather than in a document, so the next reader does not
have to re-derive the limit.

EXIT CODES
----------
  0  SHAPES covers every reference API present in the tree
  1  a reference API is used in the tree and no SHAPE matches it, or a SHAPE is
     dead with respect to the reference set
  2  usage / IO error
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

try:  # pragma: no cover
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "gate"))

import check_win87_publish_shapes as W  # noqa: E402

# THE REFERENCE SURFACE.
#
# Every entry is an API that can cause a file to appear at, or be replaced at, a
# destination path -- i.e. one that can PUBLISH. Grouped by where it comes from
# so a reviewer can check the group rather than the individual line.
#
# Deliberately includes APIs this tree does not currently use: the point is to
# notice the day one of them arrives. An entry that is never seen costs nothing;
# an entry that is missing costs a silent gap, which is the whole defect.
REFERENCE: Dict[str, str] = {
    # --- Win32 file management -------------------------------------------
    "CopyFile":            r"(?<![A-Za-z0-9_])CopyFile[WA]?\s*\(",
    "CopyFileEx":          r"(?<![A-Za-z0-9_])CopyFileEx[WA]?\s*\(",
    "CopyFile2":           r"(?<![A-Za-z0-9_])CopyFile2\s*\(",
    "MoveFile":            r"(?<![A-Za-z0-9_])MoveFile[WA]?\s*\(",
    "MoveFileEx":          r"(?<![A-Za-z0-9_])MoveFileEx[WA]?\s*\(",
    "MoveFileWithProgress": r"(?<![A-Za-z0-9_])MoveFileWithProgress[WA]?\s*\(",
    "ReplaceFile":         r"(?<![A-Za-z0-9_])ReplaceFile[WA]?\s*\(",
    "CreateHardLink":      r"(?<![A-Za-z0-9_])CreateHardLink[WA]?\s*\(",
    "CreateSymbolicLink":  r"(?<![A-Za-z0-9_])CreateSymbolicLink[WA]?\s*\(",
    "SHFileOperation":     r"(?<![A-Za-z0-9_])SHFileOperation[WA]?\s*\(",
    # --- C++ standard library --------------------------------------------
    "std::filesystem::rename":     r"\bstd\s*::\s*filesystem\s*::\s*rename\s*\(",
    "std::filesystem::copy_file":  r"\bstd\s*::\s*filesystem\s*::\s*copy_file\s*\(",
    "std::filesystem::copy":       r"\bstd\s*::\s*filesystem\s*::\s*copy\s*\(",
    "std::filesystem::create_hard_link":
        r"\bstd\s*::\s*filesystem\s*::\s*create_hard_link\s*\(",
    "std::filesystem::create_symlink":
        r"\bstd\s*::\s*filesystem\s*::\s*create_symlink\s*\(",
    # --- CRT ---------------------------------------------------------------
    "rename (CRT)":        r"(?<![A-Za-z0-9_:.>])_?w?rename\s*\(",
    # --- Qt ----------------------------------------------------------------
    "QFile::copy":         r"\bQFile\s*::\s*copy\s*\(",
    "QFile::rename":       r"\bQFile\s*::\s*rename\s*\(",
    # The instance spelling is a distinct TOKEN SEQUENCE for the same syscall, so
    # it needs its own reference entry: a SHAPE that only knows `QFile::rename`
    # is blind to `f.rename(dest)`, which is how the static-only list was blind
    # to LogHandler in the first place.
    "QFile instance rename": r"\.\s*rename\s*\(",
    "QSaveFile::commit":   r"\bcommit\s*\(\s*\)",   # QSaveFile publishes on commit
    # --- this repo's own sanctioned publisher ------------------------------
    "atomicReplaceFileWithRetry":
        r"(?<![A-Za-z0-9_])atomicReplaceFileWithRetry\s*\(",
}

# Reference APIs that are listed so the surface is complete, but whose absence
# from SHAPES is a MEASURED decision rather than an oversight. Each needs a
# reason here; an entry without one is the oversight it was meant to prevent.
#
#   QSaveFile::commit -- the regex would have to be `commit()`, which matches
#       many unrelated things. 4 files in src/ contain the token.
#   std::filesystem::copy -- `copy` is also `std::copy`'s spelling and the
#       directory-recursive form; the FILE form is covered by copy_file.
#
# `.copy(` on an instance is the third case, and it lives in the sweep's
# SOFT_SHAPES rather than here because it is not a distinct API -- see the long
# comment there for the 37-of-37 measurement.
REPORT_ONLY: Set[str] = {
    "QSaveFile::commit",
    "std::filesystem::copy",
}


# --- 1. which reference APIs does SHAPES cover? -----------------------
# A SHAPE covers a reference API when it matches a canonical call written in
# that API's spelling. Comparing regexes textually would be brittle; probing
# them with a sample is what the SHAPE will actually do to the tree.
PROBE: Dict[str, str] = {
    "CopyFile": "::CopyFileW(a, b, FALSE);",
    "CopyFileEx": "::CopyFileExW(a, b, 0, 0, 0, 0);",
    "CopyFile2": "::CopyFile2(a, b, 0);",
    "MoveFile": "::MoveFileW(a, b);",
    "MoveFileEx": "::MoveFileExW(a, b, 0);",
    "MoveFileWithProgress": "::MoveFileWithProgressW(a, b, 0, 0, 0);",
    "ReplaceFile": "::ReplaceFileW(a, b, 0, 0, 0, 0);",
    "CreateHardLink": "::CreateHardLinkW(a, b, 0);",
    "CreateSymbolicLink": "::CreateSymbolicLinkW(a, b, 0);",
    "SHFileOperation": "::SHFileOperationW(&op);",
    "std::filesystem::rename": "std::filesystem::rename(a, b);",
    "std::filesystem::copy_file": "std::filesystem::copy_file(a, b);",
    "std::filesystem::copy": "std::filesystem::copy(a, b);",
    "std::filesystem::create_hard_link": "std::filesystem::create_hard_link(a, b);",
    "std::filesystem::create_symlink": "std::filesystem::create_symlink(a, b);",
    "rename (CRT)": "rename(a, b);",
    "QFile::copy": "QFile::copy(a, b);",
    "QFile::rename": "QFile::rename(a, b);",
    "QFile instance rename": "f.rename(dest);",
    "QSaveFile::commit": "f.commit();",
    "atomicReplaceFileWithRetry": "atomicReplaceFileWithRetry(a, b, ec);",
}


def rel_to_repo(path: Path) -> str:
    """Repo-relative where possible, absolute otherwise.

    A probe root outside the repository is legitimate -- it is how the test
    reddens this guard without editing src/ -- so this must never raise. An
    exception on the display path would abort the scan and read as "no publish
    calls found", which is the vacuous green this file exists to prevent.
    """
    try:
        return path.relative_to(REPO).as_posix()
    except ValueError:
        return path.as_posix()


def iter_sources(root: Path) -> List[Path]:
    out: List[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in {"build", ".git", "_tmp"}]
        for name in filenames:
            if name.endswith((".cpp", ".h", ".hpp", ".cc")):
                out.append(Path(dirpath) / name)
    return sorted(out)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default="src",
                    help="tree to scan for reference-API usage (default: src)")
    ap.add_argument("--list", action="store_true",
                    help="print the reference surface and which SHAPES cover it")
    args = ap.parse_args(argv)

    root = (REPO / args.root) if not os.path.isabs(args.root) else Path(args.root)
    if not root.is_dir():
        print("no such root: %s" % root, file=sys.stderr)
        return 2

    shapes: Dict[str, re.Pattern] = dict(W.SHAPES)
    reference = {k: re.compile(v) for k, v in REFERENCE.items()}

    # Coverage is computed from PROBE (module level).
    covered_by: Dict[str, List[str]] = {}
    for ref_name in reference:
        probe = PROBE.get(ref_name, "")
        hits = [sh for sh, rx in shapes.items() if probe and rx.search(probe)]
        covered_by[ref_name] = hits

    # --- 2. which reference APIs actually appear in the tree? -------------
    used: Dict[str, List[str]] = {}
    unreadable: List[str] = []
    for path in iter_sources(root):
        rel = rel_to_repo(path)
        if rel in getattr(W, "EXEMPT", set()):
            continue
        try:
            raw = path.read_text(encoding="utf-8", errors="replace")
            body = W.strip_comments_and_literals(raw)
        except Exception as exc:  # noqa: BLE001
            unreadable.append("%s (%s)" % (rel, exc))
            continue
        for ref_name, rx in reference.items():
            if rx.search(body):
                used.setdefault(ref_name, []).append(rel)

    if args.list:
        print("reference surface: %d API(s)" % len(reference))
        for name in sorted(reference):
            shown = ", ".join(covered_by[name]) or "-- NOT COVERED --"
            seen = len(used.get(name, []))
            print("  %-36s covered_by=%-28s files_using=%d" % (name, shown, seen))
        return 0

    print("publish-shape completeness (GitHub #254)")
    print("  root                : %s" % rel_to_repo(root))
    print("  SHAPES entries      : %d" % len(shapes))
    print("  reference APIs      : %d" % len(reference))
    print("  reference APIs used : %d" % len(used))
    if unreadable:
        print("  UNREADABLE          : %d" % len(unreadable), file=sys.stderr)
        for u in unreadable:
            print("      %s" % u, file=sys.stderr)
        return 2

    ok = True

    # 1. COVERAGE -- a publish API used in the tree with no SHAPE to catch it.
    gaps: List[Tuple[str, List[str]]] = []
    for ref_name, files in sorted(used.items()):
        if ref_name in REPORT_ONLY:
            continue
        if not covered_by[ref_name]:
            gaps.append((ref_name, files))
    for ref_name, files in gaps:
        print("  UNCOVERED: %s is used in %d file(s) and no SHAPE matches it"
              % (ref_name, len(files)))
        for f in files[:5]:
            print("      %s" % f)
        ok = False

    # 2. LIVENESS -- a SHAPE that matches nothing in the reference surface.
    dead = [sh for sh in shapes
            if not any(sh in hits for hits in covered_by.values())]
    for sh in sorted(dead):
        print("  DEAD SHAPE: %r matches no API in the reference surface" % sh)
        ok = False

    if ok:
        print("OK -- every reference publish API used in the tree is covered by a "
              "SHAPE, and every SHAPE is live")
        return 0

    print("", file=sys.stderr)
    print("FAIL: the shape list is no longer complete with respect to the "
          "reference surface. Either add the missing shape to SHAPES in "
          "tools/gate/check_win87_publish_shapes.py, or -- if the call is not a "
          "publish -- say so here. This is the C-ENUM failure #254 exists to "
          "make visible: a list that grows one entry at a time whenever someone "
          "notices, with the property itself never measured.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
