#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate a 100k-object page-tree fixture for the resource-bomb sandbox.

GitHub #51 (meta-audit SEC-35).

WHY THIS IS GENERATED AND NOT COMMITTED
---------------------------------------
The artifact is ~10 MB of near-incompressible ASCII whose entire content is
mechanically derivable from this file. Committing it would put a
multi-megabyte binary in every clone forever to express one integer. It is
therefore produced as a BUILD STEP (see tests/CMakeLists.txt) into the build
tree, and the test that consumes it FAILS -- never skips -- if it is absent,
because "the fixture wasn't there" and "the product handled the fixture" must
not look the same from the outside.

WHAT THE FIXTURE IS
-------------------
A structurally VALID PDF whose page tree carries `--pages` (default 100,000)
`/Page` objects. It is not corrupt and it is not a parser trap: the point is
volume, not malformation. A reader that materialises one in-memory node per
page, or that walks the tree recursively per page, or that builds an O(n^2)
`/Parent` chain, blows a memory cap on this file while a reader with bounded
per-page cost does not. That distinction is exactly what the sandbox's other
three oracles measure for a decompression bomb, in the dimension of OBJECT
COUNT rather than decompressed BYTES.

The tree is BALANCED rather than one flat `/Kids` array of 100,000 refs:
`/Kids` arrays are capped at `--fanout` (default 64) entries, so the tree is
three levels deep for 100k pages. A flat array would test one specific
degenerate shape; the balanced form is what real large documents look like and
is the harder case for a walker that recurses.

Determinism: byte-identical output for identical arguments. No timestamps, no
random ids, no /ID entry derived from the clock.

Usage:
    python tests/fixtures/gen_page_tree_bomb.py --out <path> [--pages 100000]
"""
from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path


def build_pdf(num_pages: int, fanout: int) -> bytes:
    """Return the complete PDF bytes for a `num_pages`-page balanced page tree.

    Object numbering, assigned before any bytes are written so that every
    reference is known up front:

        1                     /Catalog
        2                     shared /Contents stream (one, referenced by all
                              pages -- the fixture is about the page TREE, not
                              about content-stream volume)
        3 .. 2+P              the P /Page leaves
        3+P .. 3+P+I-1        the I internal /Pages nodes, ROOT LAST
    """
    if num_pages < 1:
        raise ValueError("--pages must be >= 1")
    if fanout < 2:
        raise ValueError("--fanout must be >= 2")

    catalog_num = 1
    contents_num = 2
    first_page_num = 3
    page_nums = list(range(first_page_num, first_page_num + num_pages))

    # Build the internal nodes bottom-up. `level` starts as the leaf object
    # numbers; each round groups them into parent nodes until one root remains.
    next_obj = first_page_num + num_pages
    # internal node number -> list of child object numbers
    internal: dict[int, list[int]] = {}
    # child object number -> parent object number
    parent_of: dict[int, int] = {}

    level = page_nums
    while len(level) > 1:
        parents: list[int] = []
        for i in range(0, len(level), fanout):
            group = level[i:i + fanout]
            node = next_obj
            next_obj += 1
            internal[node] = group
            for c in group:
                parent_of[c] = node
            parents.append(node)
        level = parents
    root = level[0]

    if root not in internal:
        # Degenerate single-page case: synthesise a root so the catalog has a
        # /Pages to point at.
        node = next_obj
        next_obj += 1
        internal[node] = [root]
        parent_of[root] = node
        root = node

    total_objects = next_obj - 1

    # ---- serialise -------------------------------------------------------
    out = io.BytesIO()
    # PDF 1.7. The header comment line with high bytes is the conventional
    # "this file is binary" marker; harmless and standard.
    out.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")

    # offsets[obj_num] = byte offset of that object's definition
    offsets: dict[int, int] = {}

    def begin(num: int) -> None:
        offsets[num] = out.tell()
        out.write(b"%d 0 obj\n" % num)

    def end() -> None:
        out.write(b"endobj\n")

    # 1: catalog
    begin(catalog_num)
    out.write(b"<< /Type /Catalog /Pages %d 0 R >>\n" % root)
    end()

    # 2: the one shared content stream
    stream = b"0.5 0.5 0.5 rg 10 10 80 80 re f\n"
    begin(contents_num)
    out.write(b"<< /Length %d >>\nstream\n" % len(stream))
    out.write(stream)
    out.write(b"\nendstream\n")
    end()

    # leaves
    for num in page_nums:
        begin(num)
        out.write(
            b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 100 100] "
            b"/Resources << >> /Contents %d 0 R >>\n"
            % (parent_of[num], contents_num)
        )
        end()

    # Internal nodes, in object-number order. /Count is accumulated bottom-up
    # (children always have lower object numbers than their parent) rather than
    # by recursion: the fixture is meant to stress the READER, not this script.
    counts: dict[int, int] = {}
    for node in sorted(internal):          # bottom-up: children always lower
        counts[node] = sum(
            counts[c] if c in counts else 1 for c in internal[node]
        )

    for node in sorted(internal):
        begin(node)
        kids = b" ".join(b"%d 0 R" % c for c in internal[node])
        if node == root:
            out.write(
                b"<< /Type /Pages /Kids [ %s ] /Count %d >>\n"
                % (kids, counts[node])
            )
        else:
            out.write(
                b"<< /Type /Pages /Parent %d 0 R /Kids [ %s ] /Count %d >>\n"
                % (parent_of[node], kids, counts[node])
            )
        end()

    # ---- xref ------------------------------------------------------------
    startxref = out.tell()
    out.write(b"xref\n0 %d\n" % (total_objects + 1))
    out.write(b"0000000000 65535 f \n")
    for num in range(1, total_objects + 1):
        out.write(b"%010d 00000 n \n" % offsets[num])
    out.write(b"trailer\n<< /Size %d /Root %d 0 R >>\n"
              % (total_objects + 1, catalog_num))
    out.write(b"startxref\n%d\n%%%%EOF\n" % startxref)

    return out.getvalue()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, help="output .pdf path")
    ap.add_argument("--pages", type=int, default=100000,
                    help="number of /Page objects (default 100000)")
    ap.add_argument("--fanout", type=int, default=64,
                    help="max /Kids per internal /Pages node (default 64)")
    ap.add_argument("--self-test", action="store_true",
                    help="build a small tree and verify its structure, then exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    data = build_pdf(args.pages, args.fanout)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # Binary mode: no newline translation, so the byte offsets in the xref
    # table stay correct on Windows. Writing this file in text mode would
    # produce a PDF whose every offset is wrong.
    with io.open(out, "wb") as fh:
        fh.write(data)
    print(f"[page-tree-bomb] wrote {out} "
          f"({len(data)} bytes, {args.pages} pages, fanout {args.fanout})")
    return 0


def self_test() -> int:
    """Structural checks on a small tree; the shape must be right before the
    100k one is trusted."""
    problems: list[str] = []

    data = build_pdf(200, 8)
    if not data.startswith(b"%PDF-1.7"):
        problems.append("missing %PDF header")
    if b"%%EOF" not in data:
        problems.append("missing %%EOF")
    if data.count(b"/Type /Page ") + data.count(b"/Type /Page\n") != 200:
        problems.append(
            "expected exactly 200 /Page objects, got "
            f"{data.count(b'/Type /Page ') + data.count(b'/Type /Page')}")
    # 200 pages at fanout 8 => 25 + 4 + 1 = 30 internal nodes.
    if data.count(b"/Type /Pages") != 30:
        problems.append(
            f"expected 30 /Pages nodes at fanout 8, got {data.count(b'/Type /Pages')}")
    # The root's /Count must be the page total, or every reader disagrees with
    # the tree it is walking.
    if b"/Count 200" not in data:
        problems.append("root /Count 200 not present")
    # Determinism.
    if build_pdf(200, 8) != data:
        problems.append("output is not deterministic across two builds")
    # No CR bytes outside the binary marker line: an accidental text-mode write
    # anywhere would corrupt the xref offsets.
    body = data.split(b"\n", 2)[2]
    if b"\r" in body:
        problems.append("CR byte in body -- offsets would be wrong")

    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    print("OK: gen_page_tree_bomb self-test passed "
          "(200 pages, 30 /Pages nodes, deterministic, LF-only).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
