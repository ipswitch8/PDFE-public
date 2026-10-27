# -*- coding: utf-8 -*-
"""Generate the F18 backlog corpus — the phase-2 synthesis backlog fixtures that
the meta-audit recorded as *zero-coverage* (gap ids A3-33, A3-48, A3-49, A3-51,
A3-52, A3-53) plus a search ground-truth fixture for A3-16.

Every file is assembled from RAW BYTES so byte offsets are exact, the result is
byte-for-byte deterministic, and the AUTHORED GROUND TRUTH below is the literal
truth of the file — not a description of what PDFE happens to produce. The
consuming oracles in ``tests/test_f18_backlog_corpus.cpp`` assert against these
literals.

AUTHORED GROUND TRUTH
=====================

f18_search_ground_truth.pdf   (A3-16 — search oracle anchored to authored counts)
  3 pages, MediaBox [0 0 612 792], /Helvetica 12pt, uncompressed streams.
    page 0: "NEEDLE" x2, "needle" x1, "HAYSTACK" x3
    page 1: "NEEDLE" x3, "needle" x0, "HAYSTACK" x2
    page 2: "NEEDLE" x1, "needle" x1, "HAYSTACK" x1
  => case-INSENSITIVE "needle" total = 8   (2+1 , 3+0 , 1+1)
     case-SENSITIVE   "NEEDLE" total = 6   (2, 3, 1)
     case-SENSITIVE   "needle" total = 2   (1, 0, 1)
     "HAYSTACK" total = 6, "ABSENTTOKEN" total = 0
  Each drawn token is its own Tj on its own line so no token is split across
  show-text operators and no two tokens are adjacent (no accidental overlap).

f18_no_tounicode.pdf          (A3-48 — redaction-by-search false-negative class)
  1 page, MediaBox [0 0 300 200]. **TWO Type 3 fonts that share the very same six
  glyph procedures and the very same /Widths array**, so they lay down IDENTICAL
  ink; the ONLY difference between them is the glyph NAMES in their
  /Encoding /Differences. Neither font carries a /ToUnicode — that absence is
  the point of the fixture.

    /T3  codes 1..6 -> /Ssmall /Esmall /Csmall /Rsmall /Esmall /Tsmall
         The Adobe Expert small-caps names. They ARE in the Adobe Glyph List, but
         they map to the Corporate Use Subarea (U+F773, U+F765, ...), so an
         unaided reader extracts six PRIVATE-USE codepoints: text that looks
         extracted but that no search can ever match. PDFE's glyph-name recovery
         (src/core/ToUnicodeRecovery.cpp) strips the Expert `small` suffix and
         recovers S, E, C, R, E, T — the letters the reader actually sees.
         Drawn at Tm (20,100).

    /T4  codes 1..6 -> /gx1 /gx2 /gx3 /gx4 /gx5 /gx6
         Opaque, producer-invented names carrying NO Unicode information of any
         kind. Nothing — not MuPDF, not PDFE, not any other reader — can recover
         letters from them, so this run stays unsearchable and keeps the A3-48
         false-negative class alive and characterised after the recovery landed.
         Drawn at Tm (20,160).

  The page ALSO draws, with ordinary /Helvetica, the token "VISIBLEMARK" — a
  control proving the page's other text IS searchable, so a miss on a Type 3 run
  is attributable to that run's encoding and not to the page being unreadable.
    searchable-by-text : "VISIBLEMARK"  (Helvetica, 1 occurrence)
                         "SECRET"       (/T3, 1 occurrence, via glyph-name recovery)
    NOT searchable     : the 6 /T4 glyph codes. Under PDFE's stext options they
                         surface as six U+FFFD REPLACEMENT CHARACTERs; under
                         MuPDF's whitespace-preserving default they surface as
                         the raw codes \001..\006. Either way: not letters.
    ink present        : 6 filled rectangles inside [20 100 200 140]  (/T3)
                         6 filled rectangles inside [20 160 200 200]  (/T4)

f18_zero_page.pdf             (A3-49 / A3-33 — 0-page document)
  A structurally VALID PDF whose page tree is empty: /Type /Pages /Count 0
  /Kids []. The catalog is well-formed and the xref is exact. Authored page
  count = 0.

f18_incremental_shadow.pdf    (A3-51 — latest-object-wins /Prev chain)
  Revision 1: object 4 is the page content stream drawing "ORIGINALSECRET".
  Revision 2 (incremental, trailer /Prev -> revision 1's xref): object 4 is
  REDEFINED to draw "UPDATEDPUBLIC". Both byte sequences are present in the
  file; only the revision-2 definition is reachable through the xref chain.
    reachable text  : "UPDATEDPUBLIC"
    shadowed text   : "ORIGINALSECRET"  (present in the bytes, unreachable)

f18_deeply_nested_array.pdf   (A3-52 — deep-recursion parse boundary)
  1 page. Object 6 is an /F18Nest entry in the page dictionary holding an array
  nested NEST_DEPTH levels deep with the integer 42 at the bottom. The page also
  draws "NESTHOST" so the page remains renderable and identifiable.

f18_10000page.pdf             (A3-53 / A3-33 — 10k-page scale boundary)
  Exactly 10000 pages. Every page shares one Resources dict and one of two
  content streams: page 0 draws "FIRSTPAGE", page 9999 draws "LASTPAGE", all
  other pages share a single "FILLERPAGE" stream (so the file stays ~1 MB).

Regenerate with:  python tests/fixtures/gen_f18_backlog_corpus.py
"""

from __future__ import annotations

import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# --- authored literals (mirrored verbatim in the C++ oracle) ------------------
SEARCH_PAGES = [
    # (NEEDLE count, needle count, HAYSTACK count)
    (2, 1, 3),
    (3, 0, 2),
    (1, 1, 1),
]
NEST_DEPTH = 200
TEN_K_PAGES = 10000


# ---------------------------------------------------------------------------
# Minimal raw-PDF writer: objects are byte blobs, offsets computed exactly.
# ---------------------------------------------------------------------------
class RawPdf:
    """Assemble a single-revision PDF with a classic xref table."""

    def __init__(self, header: bytes = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"):
        self.buf = io.BytesIO()
        self.buf.write(header)
        self.offsets = {}  # obj number -> byte offset

    def add(self, num: int, body: bytes) -> None:
        self.offsets[num] = self.buf.tell()
        self.buf.write(b"%d 0 obj\n" % num + body + b"\nendobj\n")

    def add_stream(self, num: int, dict_body: bytes, data: bytes) -> None:
        d = dict_body.rstrip()
        assert d.endswith(b">>")
        d = d[:-2] + b" /Length %d >>" % len(data)
        self.offsets[num] = self.buf.tell()
        self.buf.write(b"%d 0 obj\n" % num + d + b"\nstream\n" + data
                       + b"\nendstream\nendobj\n")

    def finish(self, root: int, size: int) -> bytes:
        xref_at = self.buf.tell()
        lines = [b"xref\n", b"0 %d\n" % size, b"0000000000 65535 f \n"]
        for n in range(1, size):
            off = self.offsets.get(n)
            if off is None:
                lines.append(b"0000000000 65535 f \n")
            else:
                lines.append(b"%010d 00000 n \n" % off)
        self.buf.write(b"".join(lines))
        self.buf.write(b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
                       % (size, root, xref_at))
        return self.buf.getvalue()


def _write(name: str, data: bytes) -> None:
    path = os.path.join(HERE, name)
    with open(path, "wb") as fh:
        fh.write(data)
    print("wrote %s (%d bytes)" % (name, len(data)))


# ---------------------------------------------------------------------------
# A3-16 — search ground truth
# ---------------------------------------------------------------------------
def build_search_ground_truth() -> bytes:
    pdf = RawPdf()
    n_pages = len(SEARCH_PAGES)
    # 1 catalog, 2 pages-node, 3 font, 4..(3+n) page dicts, (4+n).. contents
    page_first = 4
    cont_first = 4 + n_pages
    kids = b" ".join(b"%d 0 R" % (page_first + i) for i in range(n_pages))
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Count %d /Kids [%s] >>" % (n_pages, kids))
    pdf.add(3, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    for i, (upper, lower, hay) in enumerate(SEARCH_PAGES):
        pdf.add(page_first + i,
                b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
                b" /Resources << /Font << /F1 3 0 R >> >>"
                b" /Contents %d 0 R >>" % (cont_first + i))
    for i, (upper, lower, hay) in enumerate(SEARCH_PAGES):
        rows = []
        y = 720
        for _ in range(upper):
            rows.append((y, b"NEEDLE"))
            y -= 24
        for _ in range(lower):
            rows.append((y, b"needle"))
            y -= 24
        for _ in range(hay):
            rows.append((y, b"HAYSTACK"))
            y -= 24
        body = b"BT\n/F1 12 Tf\n"
        for yy, tok in rows:
            body += b"1 0 0 1 72 %d Tm\n(" % yy + tok + b") Tj\n"
        body += b"ET\n"
        pdf.add_stream(cont_first + i, b"<< >>", body)
    return pdf.finish(root=1, size=cont_first + n_pages)


# ---------------------------------------------------------------------------
# A3-48 — Type 3 font with NO /ToUnicode
# ---------------------------------------------------------------------------
def build_no_tounicode() -> bytes:
    pdf = RawPdf()
    # Six Type 3 glyph procedures. Each one is a filled rectangle of a distinct
    # width so the "word" has authored, measurable ink. Glyph space is scaled by
    # /FontMatrix [0.001 0 0 0.001 0 0], so 600 units == 0.6 * font size.
    glyph_widths = [600, 520, 560, 540, 520, 500]
    proc_first = 10
    for idx, w in enumerate(glyph_widths):
        body = (b"%d 0 d0\n" % w
                + b"60 60 %d 620 re f\n" % (w - 120))
        pdf.add_stream(proc_first + idx, b"<< >>", body)
    # /T3: Adobe Expert small-caps names spelling SECRET. /Esmall appears at
    # codes 2 AND 5, which is exactly why glyph_widths[1] == glyph_widths[4] ==
    # 520: the two codes share one glyph procedure, so the authored ink is
    # unchanged by the two codes naming the same glyph.
    expert_names = [b"Ssmall", b"Esmall", b"Csmall", b"Rsmall", b"Esmall", b"Tsmall"]
    expert_procs = {}
    for idx, nm in enumerate(expert_names):
        expert_procs.setdefault(nm, proc_first + idx)
    charprocs_expert = b" ".join(b"/%s %d 0 R" % (nm, obj)
                                 for nm, obj in expert_procs.items())
    diffs_expert = b" ".join(b"/%s" % nm for nm in expert_names)
    # /T4: opaque names over THE SAME six procedures — identical ink, zero
    # recoverable Unicode. The residual false-negative class.
    charprocs_opaque = b" ".join(b"/gx%d %d 0 R" % (i + 1, proc_first + i)
                                 for i in range(len(glyph_widths)))
    diffs_opaque = b" ".join(b"/gx%d" % (i + 1) for i in range(len(glyph_widths)))

    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>")
    pdf.add(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200]"
               b" /Resources << /Font << /T3 5 0 R /F1 9 0 R /T4 16 0 R >> >>"
               b" /Contents 4 0 R >>")
    content = (b"BT\n/T3 24 Tf\n1 0 0 1 20 100 Tm\n"
               b"<010203040506> Tj\nET\n"
               b"BT\n/T4 24 Tf\n1 0 0 1 20 160 Tm\n"
               b"<010203040506> Tj\nET\n"
               b"BT\n/F1 10 Tf\n1 0 0 1 20 40 Tm\n(VISIBLEMARK) Tj\nET\n")
    pdf.add_stream(4, b"<< >>", content)
    # NOTE: no /ToUnicode entry anywhere in EITHER font. That absence is the point.
    pdf.add(5, b"<< /Type /Font /Subtype /Type3"
               b" /FontBBox [0 0 700 700] /FontMatrix [0.001 0 0 0.001 0 0]"
               b" /CharProcs 6 0 R /Encoding 7 0 R"
               b" /FirstChar 1 /LastChar 6 /Widths 8 0 R"
               b" /Resources << >> >>")
    pdf.add(6, b"<< " + charprocs_expert + b" >>")
    pdf.add(7, b"<< /Type /Encoding /Differences [1 " + diffs_expert + b"] >>")
    pdf.add(8, b"[" + b" ".join(b"%d" % w for w in glyph_widths) + b"]")
    pdf.add(9, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    pdf.add(16, b"<< /Type /Font /Subtype /Type3"
                b" /FontBBox [0 0 700 700] /FontMatrix [0.001 0 0 0.001 0 0]"
                b" /CharProcs 17 0 R /Encoding 18 0 R"
                b" /FirstChar 1 /LastChar 6 /Widths 8 0 R"
                b" /Resources << >> >>")
    pdf.add(17, b"<< " + charprocs_opaque + b" >>")
    pdf.add(18, b"<< /Type /Encoding /Differences [1 " + diffs_opaque + b"] >>")
    return pdf.finish(root=1, size=19)


# ---------------------------------------------------------------------------
# A3-49 / A3-33 — 0-page document
# ---------------------------------------------------------------------------
def build_zero_page() -> bytes:
    pdf = RawPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Count 0 /Kids [] >>")
    return pdf.finish(root=1, size=3)


# ---------------------------------------------------------------------------
# A3-51 — incremental update shadowing a base object
# ---------------------------------------------------------------------------
ORIGINAL_TOKEN = b"ORIGINALSECRET"
UPDATED_TOKEN = b"UPDATEDPUBLIC"


def build_incremental_shadow() -> bytes:
    base = RawPdf()
    base.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    base.add(2, b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>")
    base.add(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
                b" /Resources << /Font << /F1 5 0 R >> >>"
                b" /Contents 4 0 R >>")
    base.add_stream(4, b"<< >>",
                    b"BT\n/F1 18 Tf\n1 0 0 1 72 700 Tm\n("
                    + ORIGINAL_TOKEN + b") Tj\nET\n")
    base.add(5, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    rev1 = base.finish(root=1, size=6)
    rev1_xref = rev1.rindex(b"xref\n0 6\n")

    # Revision 2 — redefine object 4 only.
    out = io.BytesIO()
    out.write(rev1)
    new_off = out.tell()
    new_body = (b"BT\n/F1 18 Tf\n1 0 0 1 72 700 Tm\n("
                + UPDATED_TOKEN + b") Tj\nET\n")
    out.write(b"4 0 obj\n<< /Length %d >>\nstream\n" % len(new_body)
              + new_body + b"\nendstream\nendobj\n")
    xref2 = out.tell()
    out.write(b"xref\n0 1\n0000000000 65535 f \n"
              b"4 1\n%010d 00000 n \n" % new_off)
    out.write(b"trailer\n<< /Size 6 /Root 1 0 R /Prev %d >>\nstartxref\n%d\n%%%%EOF\n"
              % (rev1_xref, xref2))
    return out.getvalue()


# ---------------------------------------------------------------------------
# A3-52 — deeply nested array
# ---------------------------------------------------------------------------
def build_deeply_nested_array() -> bytes:
    pdf = RawPdf()
    nest = b"[" * NEST_DEPTH + b"42" + b"]" * NEST_DEPTH
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>")
    pdf.add(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
               b" /Resources << /Font << /F1 5 0 R >> >>"
               b" /F18Nest 6 0 R /Contents 4 0 R >>")
    pdf.add_stream(4, b"<< >>",
                   b"BT\n/F1 18 Tf\n1 0 0 1 72 700 Tm\n(NESTHOST) Tj\nET\n")
    pdf.add(5, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    pdf.add(6, nest)
    return pdf.finish(root=1, size=7)


# ---------------------------------------------------------------------------
# A3-53 / A3-33 — 10000-page document
# ---------------------------------------------------------------------------
def build_10000page() -> bytes:
    pdf = RawPdf()
    n = TEN_K_PAGES
    # 1 catalog, 2 pages, 3 font, 4 filler content, 5 first content,
    # 6 last content, 7.. page dicts
    page_first = 7
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    kids = b" ".join(b"%d 0 R" % (page_first + i) for i in range(n))
    pdf.add(2, b"<< /Type /Pages /Count %d /Kids [%s] >>" % (n, kids))
    pdf.add(3, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    for num, token in ((4, b"FILLERPAGE"), (5, b"FIRSTPAGE"), (6, b"LASTPAGE")):
        pdf.add_stream(num, b"<< >>",
                       b"BT\n/F1 18 Tf\n1 0 0 1 72 700 Tm\n("
                       + token + b") Tj\nET\n")
    for i in range(n):
        cont = 5 if i == 0 else (6 if i == n - 1 else 4)
        pdf.add(page_first + i,
                b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
                b" /Resources << /Font << /F1 3 0 R >> >>"
                b" /Contents %d 0 R >>" % cont)
    return pdf.finish(root=1, size=page_first + n)


def main() -> int:
    _write("f18_search_ground_truth.pdf", build_search_ground_truth())
    _write("f18_no_tounicode.pdf", build_no_tounicode())
    _write("f18_zero_page.pdf", build_zero_page())
    _write("f18_incremental_shadow.pdf", build_incremental_shadow())
    _write("f18_deeply_nested_array.pdf", build_deeply_nested_array())
    _write("f18_10000page.pdf", build_10000page())
    return 0


if __name__ == "__main__":
    sys.exit(main())
