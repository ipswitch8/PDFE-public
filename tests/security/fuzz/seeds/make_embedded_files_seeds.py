#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate structure-hostile seeds for the fuzz_embedded_files harness.

WHY THIS EXISTS (see docs/audit/dca/0035). The harness header comment has
claimed since phase-4 that it targets "cyclic name-tree refs". It never found
one, because its entire seed corpus was `test_portfolio.pdf`, whose
/EmbeddedFiles name tree is FLAT — no /Kids node anywhere. Byte mutation does
not invent a cycle: that needs two mutually-referencing indirect objects plus a
catalog pointing at them, which is a coordinated addition, not a flipped byte.

The corpus was the whole gap, and this is the cheap half of closing it.

`NameTreeShapeTest.ACycleSurvivesXrefDamageSoASeededCorpusWouldFindIt` measures
the property these seeds depend on: MuPDF's repair path rebuilds the object
graph by scanning for `N 0 obj` headers, so a cyclic tree survives having its
xref offsets destroyed. A mutated cyclic seed therefore stays cyclic through
essentially anything libFuzzer does to it — which is exactly what makes seeding
worth more here than a longer campaign.

Deterministic: same bytes every run, so re-running does not churn the corpus.

  python tests/security/fuzz/seeds/make_embedded_files_seeds.py
"""

import io
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "fuzz_embedded_files")


class PdfBuilder(object):
    """Minimal PDF writer with a computed cross-reference table.

    The C++ twin of this lives in tests/test_attachment_hardening_phase71.cpp.
    Both compute real offsets: a placeholder xref sends MuPDF down the repair
    path, and while repair happens to preserve the object graph (measured, see
    above), starting from a well-formed file keeps the seed's meaning obvious to
    whoever reads the corpus next.
    """

    def __init__(self):
        self.buf = bytearray(b"%PDF-1.7\n")
        self.off = [0]

    def obj(self, num, content):
        while len(self.off) <= num:
            self.off.append(0)
        self.off[num] = len(self.buf)
        self.buf += ("%d 0 obj\n%s\nendobj\n" % (num, content)).encode("ascii")

    def finish(self, root_num):
        xref_pos = len(self.buf)
        n = len(self.off)
        self.buf += ("xref\n0 %d\n" % n).encode("ascii")
        self.buf += b"0000000000 65535 f \n"
        for i in range(1, n):
            self.buf += ("%010d 00000 n \n" % self.off[i]).encode("ascii")
        self.buf += ("trailer\n<< /Size %d /Root %d 0 R >>\n"
                     "startxref\n%d\n%%%%EOF\n"
                     % (n, root_num, xref_pos)).encode("ascii")
        return bytes(self.buf)


def skeleton(b, ef_root):
    b.obj(1, "<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles %d 0 R >> >>"
          % ef_root)
    b.obj(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.obj(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>")
    b.obj(90, "<< /Type /Filespec /F (leaf.txt) /UF (leaf.txt) "
              "/EF << /F 91 0 R >> >>")
    b.obj(91, "<< /Type /EmbeddedFile /Length 5 >>\nstream\nhello\nendstream")


def cyclic_kids():
    """4 -> 5 -> 4. The shape the harness header has always claimed to cover."""
    b = PdfBuilder()
    skeleton(b, 4)
    b.obj(4, "<< /Names [(leaf.txt) 90 0 R] /Kids [5 0 R] >>")
    b.obj(5, "<< /Kids [4 0 R] >>")
    return b.finish(1)


def deep_kids():
    """A 200-link /Kids chain with the leaf below the depth cap."""
    b = PdfBuilder()
    skeleton(b, 10)
    first, last = 10, 209
    for i in range(first, last):
        b.obj(i, "<< /Kids [%d 0 R] >>" % (i + 1))
    b.obj(last, "<< /Names [(leaf.txt) 90 0 R] >>")
    return b.finish(1)


def nested_kids():
    """An ORDINARY nested tree. Not hostile — and that is the point.

    A corpus of nothing but pathological inputs teaches the mutator to explore
    pathology and nothing else. This seed is what a real producer emits once a
    document has more attachments than fit in one node, and it is the shape a
    fix that simply refuses to descend /Kids would break.
    """
    b = PdfBuilder()
    skeleton(b, 4)
    b.obj(4, "<< /Kids [5 0 R] >>")
    b.obj(5, "<< /Kids [6 0 R] >>")
    b.obj(6, "<< /Names [(leaf.txt) 90 0 R] >>")
    return b.finish(1)


SEEDS = {
    "nametree_cyclic_kids.pdf": cyclic_kids,
    "nametree_deep_kids.pdf": deep_kids,
    "nametree_nested_kids.pdf": nested_kids,
}


def main():
    if not os.path.isdir(OUT_DIR):
        print("seed dir missing:", OUT_DIR)
        return 2
    for name, fn in sorted(SEEDS.items()):
        path = os.path.join(OUT_DIR, name)
        data = fn()
        existing = None
        if os.path.exists(path):
            with io.open(path, "rb") as fh:
                existing = fh.read()
        if existing == data:
            print("unchanged: %s (%d bytes)" % (name, len(data)))
            continue
        with io.open(path, "wb") as fh:
            fh.write(data)
        print("wrote:     %s (%d bytes)" % (name, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
