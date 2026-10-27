#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the PDFE-SEC-MUPDF-RECURSION-DOS reproducer corpus.

GitHub #139. See `PDFE-SEC-MUPDF-RECURSION-DOS` in tests/docs/known_issues.md.

WHAT THE FINDING IS
-------------------
The `fuzz_document_open` libFuzzer harness drove MuPDF 1.26.10 into
effectively-unbounded recursion on a crafted page graph and exhausted the
thread stack -- an unrecoverable crash (stack overflow -> access violation)
that `fz_try`/`fz_catch` cannot trap, because it is setjmp-based and the
overflow has already corrupted both the stack and the allocator state. PDFE
contains it at the application layer by opening every document in the
disposable `pdfe_preflight` child first.

WHY THIS FILE EXISTS
--------------------
The finding's resolution path is "(2) on a MuPDF bump, re-run
`fuzz_document_open` to confirm". Nothing made that happen, and the original
libFuzzer crash artifact is not in the tree (the windows-fuzz run's artifacts
are long past retention). So the finding was a paragraph of prose that no
later reviewer could re-run -- precisely the gap
`tools/gate/verify_vcpkg_overlays.py` was written to close for the overlay
hashes.

This file re-derives the described input SHAPES from the finding's own
description, so that "re-run the reproducer" is a command rather than an
intention. `tools/gate/verify_mupdf_recursion_recheck.py` runs them and
records the outcome against the pinned libMuPDF version.

HONEST LIMITATION, STATED HERE AND IN THE RECORD
------------------------------------------------
These shapes are RECONSTRUCTED from the finding's prose ("a chain of thousands
of /Pages nodes each /Kids-referencing the next, with matching /Parent
back-links"), not recovered from the original artifact. As of the recorded run
NONE of them reproduces the crash against the pinned MuPDF through
`pdfe_preflight`. That is reported as a weaker result rather than presented as
a clean bill of health: this corpus is a standing CONTROL that must be re-run
and re-recorded on every MuPDF bump, not proof that the class is gone. The
strong reproducer remains the fuzz campaign itself.

Generated, never committed: a crash reproducer that lands in a repository is a
thing people accidentally open.

Usage:
    python tests/fixtures/gen_mupdf_recursion_dos.py --all --out-dir <dir>
    python tests/fixtures/gen_mupdf_recursion_dos.py --shape chain --out f.pdf
    python tests/fixtures/gen_mupdf_recursion_dos.py --self-test
"""
from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path


def _assemble(objs: list[tuple[int, bytes]], root: int = 1,
              break_startxref: bool = False) -> bytes:
    """Serialise numbered objects into a PDF with a classic xref table.

    `break_startxref` points startxref at nothing, forcing MuPDF down its
    xref-REPAIR path -- one of the three code paths the finding names.
    """
    out = io.BytesIO()
    out.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    off: dict[int, int] = {}
    for num, body in objs:
        off[num] = out.tell()
        out.write(b"%d 0 obj\n" % num)
        out.write(body)
        out.write(b"\nendobj\n")
    total = max(off)
    startxref = out.tell()
    out.write(b"xref\n0 %d\n" % (total + 1))
    out.write(b"0000000000 65535 f \n")
    for num in range(1, total + 1):
        out.write(b"%010d 00000 n \n" % off.get(num, 0))
    out.write(b"trailer\n<< /Size %d /Root %d 0 R >>\n" % (total + 1, root))
    out.write(b"startxref\n%d\n%%%%EOF\n"
              % (999999999 if break_startxref else startxref))
    return out.getvalue()


def shape_chain(depth: int = 60000, break_startxref: bool = False) -> bytes:
    """The shape the finding describes literally: a chain of `depth` /Pages
    nodes, each /Kids-referencing the next, each with a /Parent back-link, one
    /Page leaf at the bottom. Every node is individually legal."""
    catalog, contents = 1, 2
    chain = list(range(3, 3 + depth))
    leaf = 3 + depth
    objs = [(catalog, b"<< /Type /Catalog /Pages %d 0 R >>" % chain[0])]
    stream = b"0 0 0 rg 10 10 10 10 re f\n"
    objs.append((contents,
                 b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream)))
    for i, num in enumerate(chain):
        child = chain[i + 1] if i + 1 < len(chain) else leaf
        if i == 0:
            objs.append((num, b"<< /Type /Pages /Kids [ %d 0 R ] /Count 1 >>" % child))
        else:
            objs.append((num, b"<< /Type /Pages /Parent %d 0 R /Kids [ %d 0 R ] "
                              b"/Count 1 >>" % (chain[i - 1], child)))
    objs.append((leaf, b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 100 100] "
                       b"/Resources << >> /Contents %d 0 R >>"
                 % (chain[-1], contents)))
    return _assemble(objs, break_startxref=break_startxref)


def shape_cyclic(depth: int = 20000) -> bytes:
    """The 'effectively cyclic' variant the finding concludes it must be: the
    deepest node's /Kids points back at the ROOT, so there is no bottom, and
    every node claims a /Count large enough that a walker cannot short-circuit
    on the count alone."""
    objs = [(1, b"<< /Type /Catalog /Pages 2 0 R >>")]
    for i in range(depth):
        num = 2 + i
        child = 2 + ((i + 1) % depth)
        parent = 2 + ((i - 1) % depth)
        objs.append((num, b"<< /Type /Pages /Parent %d 0 R /Kids [ %d 0 R ] "
                          b"/Count 1000000 >>" % (parent, child)))
    return _assemble(objs)


def shape_self_kids() -> bytes:
    """The minimal cycle: one /Pages node whose /Kids contains ITSELF. Tiny,
    so a walker with no loop detection diverges on 343 bytes."""
    return _assemble([
        (1, b"<< /Type /Catalog /Pages 2 0 R >>"),
        (2, b"<< /Type /Pages /Count 1000000 /Kids [ 2 0 R 3 0 R ] >>"),
        (3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>"),
    ])


def shape_nested_kids(depth: int = 100000) -> bytes:
    """PARSER recursion rather than page-tree recursion: /Kids is `depth`
    nested arrays deep. pdf_parse_array is the classically recursive one."""
    kids = b"[" * depth + b"3 0 R" + b"]" * depth
    return _assemble([
        (1, b"<< /Type /Catalog /Pages 2 0 R >>"),
        (2, b"<< /Type /Pages /Count 1 /Kids " + kids + b" >>"),
        (3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>"),
    ])


# name -> (builder, one-line description of the code path it aims at)
SHAPES: dict[str, tuple] = {
    "chain": (lambda: shape_chain(60000),
              "60000-deep /Pages chain with /Parent back-links (page-tree walk)"),
    "chain-repair": (lambda: shape_chain(60000, break_startxref=True),
                     "the same chain reached through the xref-REPAIR path"),
    "cyclic": (lambda: shape_cyclic(20000),
               "20000-node /Pages ring, no leaf, inflated /Count"),
    "self-kids": (shape_self_kids,
                  "minimal cycle: a /Pages node whose /Kids contains itself"),
    "nested-kids": (lambda: shape_nested_kids(100000),
                    "100000 nested arrays as /Kids (parser recursion)"),
}


def self_test() -> int:
    problems: list[str] = []

    d = shape_chain(10)
    if not d.startswith(b"%PDF-1.7"):
        problems.append("chain: missing %PDF header")
    if d.count(b"/Type /Pages") != 10:
        problems.append(f"chain: expected a 10-node chain, got {d.count(b'/Type /Pages')}")
    if d.count(b"/Type /Page ") != 1:
        problems.append("chain: expected exactly one /Page leaf")
    # 9 chain back-links + the leaf's: the back-links are half of what makes
    # the graph pathological rather than merely deep.
    if d.count(b"/Parent") != 10:
        problems.append(f"chain: expected 10 /Parent back-links, got {d.count(b'/Parent')}")
    if shape_chain(10) != d:
        problems.append("chain: output is not deterministic")

    rep = shape_chain(10, break_startxref=True)
    if b"startxref\n999999999" not in rep:
        problems.append("chain-repair: startxref was not broken")
    if rep == d:
        problems.append("chain-repair: identical to chain -- the flag did nothing")

    cyc = shape_cyclic(5)
    if cyc.count(b"/Type /Pages") != 5:
        problems.append("cyclic: wrong node count")
    if b"/Kids [ 2 0 R ]" not in cyc:
        problems.append("cyclic: the ring does not close back on the root")

    sk = shape_self_kids()
    if b"/Kids [ 2 0 R 3 0 R ]" not in sk:
        problems.append("self-kids: the node does not reference itself")

    nk = shape_nested_kids(7)
    if b"[[[[[[[3 0 R]]]]]]]" not in nk:
        problems.append("nested-kids: nesting depth is wrong")

    if set(SHAPES) != {"chain", "chain-repair", "cyclic", "self-kids", "nested-kids"}:
        problems.append("SHAPES table drifted from the self-test's expectations")

    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    print(f"OK: gen_mupdf_recursion_dos self-test passed ({len(SHAPES)} shapes, "
          "deterministic).")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--shape", choices=sorted(SHAPES), default="")
    ap.add_argument("--out", default="", help="output path for --shape")
    ap.add_argument("--all", action="store_true", help="write every shape")
    ap.add_argument("--out-dir", default="", help="output directory for --all")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.all:
        if not args.out_dir:
            ap.error("--all requires --out-dir")
        d = Path(args.out_dir)
        d.mkdir(parents=True, exist_ok=True)
        for name, (build, _desc) in sorted(SHAPES.items()):
            data = build()
            p = d / f"recursion-dos-{name}.pdf"
            with io.open(p, "wb") as fh:
                fh.write(data)
            print(f"[recursion-dos] wrote {p} ({len(data)} bytes)")
        return 0

    if not args.shape or not args.out:
        ap.error("give --self-test, or --all --out-dir, or --shape X --out Y")
    data = SHAPES[args.shape][0]()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with io.open(out, "wb") as fh:
        fh.write(data)
    print(f"[recursion-dos] wrote {out} ({len(data)} bytes, shape {args.shape})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
