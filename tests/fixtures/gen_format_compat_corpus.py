# -*- coding: utf-8 -*-
"""
Generator for the C-04 FORMAT-COMPAT / MIGRATION corpus (coverage-expansion
pipeline phase-2).

WHY THIS CORPUS EXISTS
----------------------
register.json C-04: no test opens files written in OLDER / exotic-but-valid
on-disk PDF shapes and asserts they load + edit + save-roundtrip. A save-format
change could silently break opening documents produced by a prior PDFE release or
another producer — a real regression for existing users. This corpus commits one
minimal 2-page document rendered into each distinct on-disk shape, so
test_format_compat.cpp can prove every shape still opens, edits, and round-trips.

Every fixture carries the SAME logical content so the test's oracles are shared:
  page 0 text: "COMPATMARK ONE alpha <variant>"
  page 1 text: "COMPATMARK TWO beta  <variant>"
(<variant> distinguishes the files but the COMPATMARK/ONE/TWO tokens are the
invariant the roundtrip oracle checks.)

SHAPES (each a real, valid, but structurally distinct layout)
  compat_objstm_xref.pdf   PDF 1.5+: object streams (/ObjStm) + cross-reference
                           streams (/XRef). The modern compact layout — must open
                           as readily as classic xref tables.
  compat_incremental.pdf   A chained incremental-update file: base body, then TWO
                           appended incremental updates (each its own xref section
                           with a /Prev pointer). Exercises the multi-/Prev
                           reader path other producers emit for annotate-in-place.
  compat_linearized.pdf    A linearized ("fast web view") file: /Linearized dict +
                           hint stream + first-page-first object order.
  compat_rebuilt_xref.pdf  A classic xref file whose startxref offset + xref
                           table are DAMAGED (zeroed), forcing the reader to
                           reconstruct the cross-reference table by scanning the
                           body. Proves PDFE recovers a corrupted-but-recoverable
                           xref (the "repaired on open" class).
  compat_intact_xref.pdf   THE INTACT TWIN of the file above: byte-for-byte the
                           same document, written by the same call, WITHOUT the
                           xref damage. See gen_rebuilt_xref().

DETERMINISM
  Built with PyMuPDF; metadata is fixed and `no_new_id=1` keeps the /ID stable, so
  regeneration is reproducible. The rebuilt-xref damage is applied by a fixed byte
  transform.

Regenerate with:  python tests/fixtures/gen_format_compat_corpus.py
"""
import os
import re
import sys

import fitz  # PyMuPDF
import pikepdf  # libqpdf: linearization

HERE = os.path.dirname(os.path.abspath(__file__))

PAGE_W, PAGE_H = 480, 640


def _base_doc(variant):
    """A fresh 2-page document with the shared COMPATMARK content."""
    doc = fitz.open()
    p0 = doc.new_page(width=PAGE_W, height=PAGE_H)
    p0.insert_text((72, 96), "COMPATMARK ONE alpha %s" % variant, fontsize=18)
    p1 = doc.new_page(width=PAGE_W, height=PAGE_H)
    p1.insert_text((72, 96), "COMPATMARK TWO beta  %s" % variant, fontsize=18)
    doc.set_metadata({
        "title": "PDFE format-compat fixture",
        "author": "PDFE test corpus",
        "producer": "gen_format_compat_corpus.py",
        "creator": "gen_format_compat_corpus.py",
        "creationDate": "",
        "modDate": "",
    })
    return doc


def gen_objstm_xref():
    doc = _base_doc("objstm")
    out = os.path.join(HERE, "compat_objstm_xref.pdf")
    # use_objstms=1 => object streams + xref streams (PDF 1.5+ compact layout).
    doc.save(out, garbage=4, deflate=True, use_objstms=1, no_new_id=True)
    doc.close()
    # Sanity: the file must actually carry an /ObjStm.
    with open(out, "rb") as f:
        blob = f.read()
    if b"/ObjStm" not in blob:
        sys.exit("FATAL: compat_objstm_xref.pdf has no /ObjStm — objstm layout "
                 "not emitted")
    print("wrote compat_objstm_xref.pdf")


def gen_incremental():
    out = os.path.join(HERE, "compat_incremental.pdf")
    doc = _base_doc("incr")
    # Base body (classic xref).
    doc.save(out, garbage=4, deflate=True, no_new_id=True)
    doc.close()

    # Incremental update #1: add a Text annotation on page 0, append-only.
    doc = fitz.open(out)
    a1 = doc[0].add_text_annot((120, 140), "incr-update-1")
    a1.update()
    doc.save(out, incremental=True, encryption=fitz.PDF_ENCRYPT_KEEP)
    doc.close()

    # Incremental update #2: add another annotation on page 1, append-only. This
    # yields a SECOND /Prev-chained xref section (multi-/Prev reader path).
    doc = fitz.open(out)
    a2 = doc[1].add_text_annot((120, 140), "incr-update-2")
    a2.update()
    doc.save(out, incremental=True, encryption=fitz.PDF_ENCRYPT_KEEP)
    doc.close()

    with open(out, "rb") as f:
        blob = f.read()
    # A chained incremental file has >1 startxref (one per revision) and >=1 /Prev.
    if blob.count(b"startxref") < 2 or b"/Prev" not in blob:
        sys.exit("FATAL: compat_incremental.pdf is not a chained incremental "
                 "file (startxref=%d, /Prev present=%s)"
                 % (blob.count(b"startxref"), b"/Prev" in blob))
    print("wrote compat_incremental.pdf (startxref x%d)" % blob.count(b"startxref"))


def gen_linearized():
    # PyMuPDF 1.27 dropped linearization ("no longer supported"), so build the
    # body with fitz then linearize via pikepdf (libqpdf, which still supports
    # it). static_id keeps the output deterministic.
    out = os.path.join(HERE, "compat_linearized.pdf")
    tmp = os.path.join(HERE, "_compat_linear_tmp.pdf")
    doc = _base_doc("linear")
    doc.save(tmp, garbage=4, deflate=True, no_new_id=True)
    doc.close()
    with pikepdf.open(tmp) as pdf:
        pdf.save(out, linearize=True, static_id=True)
    os.remove(tmp)
    with open(out, "rb") as f:
        blob = f.read(4096)
    if b"/Linearized" not in blob:
        sys.exit("FATAL: compat_linearized.pdf has no /Linearized dict")
    print("wrote compat_linearized.pdf")


def gen_rebuilt_xref():
    """Classic xref file with a DAMAGED cross-reference so the reader must
    reconstruct it by scanning the body (the repaired-on-open class), PLUS its
    INTACT TWIN (issue #33 / PDF-86).

    WHY THE TWIN IS EMITTED HERE AND NOWHERE ELSE
    ---------------------------------------------
    `RebuiltXrefLoadsEditsRoundtrips` proves the damaged file round-trips. That
    is weaker than it sounds: a repair that recovered the WRONG content would
    still round-trip its own mistake. The stronger claim is EQUIVALENCE — what
    the reader reconstructs must equal what was there before the damage.

    That needs a reference the thing under test did not produce. The pristine
    bytes already exist in this function, one statement before the damage is
    applied; they were simply thrown away. Writing them out costs one file
    write and gives an oracle that is independent by construction: same
    generator, same `_base_doc("rebuilt")` content, same `doc.save(...)` call,
    differing ONLY by the byte transform below.

    Do NOT be tempted to produce the twin by re-saving the damaged file through
    a PDF reader. That compares the repair against itself and passes however
    wrong the repair is.
    """
    doc = _base_doc("rebuilt")
    out = os.path.join(HERE, "compat_rebuilt_xref.pdf")
    twin = os.path.join(HERE, "compat_intact_xref.pdf")
    # Force a classic xref TABLE (no object streams) so there is a plain xref to
    # damage.
    doc.save(out, garbage=4, deflate=True, use_objstms=0, no_new_id=True)
    doc.close()

    with open(out, "rb") as f:
        blob = bytearray(f.read())

    # The INTACT TWIN: the pristine bytes, before any damage is applied.
    with open(twin, "wb") as f:
        f.write(bytes(blob))
    print("wrote compat_intact_xref.pdf (intact twin, %d bytes)" % len(blob))

    # Damage strategy: point startxref at a bogus offset AND zero every 10-digit
    # offset in the xref table. MuPDF then cannot trust the xref and rebuilds it
    # by scanning "N 0 obj" markers in the body. The object bodies themselves are
    # untouched, so a correct rebuild recovers the full document.
    #   1) Corrupt the startxref value (last "startxref\n<num>").
    m = list(re.finditer(rb"startxref\s+(\d+)", blob))
    if not m:
        sys.exit("FATAL: no startxref in rebuilt-xref base file")
    last = m[-1]
    num_start, num_end = last.start(1), last.end(1)
    bogus = b"9999999999"[: (num_end - num_start)]
    blob[num_start:num_end] = bogus.rjust(num_end - num_start, b"0")

    #   2) Zero the in-use xref entry offsets ("0000000123 00000 n").
    def _zero_offset(mo):
        return b"0000000000" + mo.group(1) + mo.group(2) + b"n"
    damaged = re.sub(rb"\d{10}( )(\d{5} )n", _zero_offset, bytes(blob))

    with open(out, "wb") as f:
        f.write(damaged)

    # Confirm the damage is real yet recoverable: MuPDF must still open it and
    # report 2 pages (it rebuilds). If it can't, the fixture is over-damaged.
    d = fitz.open(out)
    if d.page_count != 2:
        sys.exit("FATAL: rebuilt-xref fixture over-damaged (page_count=%d)"
                 % d.page_count)
    d.close()

    # The twin must be the SAME document and must NOT carry the damage. Both
    # halves are checked: a twin identical to the damaged file would make the
    # equivalence test vacuous (it would compare the repair with itself), and a
    # twin whose content differs would make it wrong.
    t = fitz.open(twin)
    if t.page_count != 2:
        sys.exit("FATAL: intact twin has page_count=%d" % t.page_count)
    t.close()
    with open(twin, "rb") as f:
        twin_blob = f.read()
    if twin_blob == damaged:
        sys.exit("FATAL: intact twin is byte-identical to the damaged file — "
                 "the damage transform did nothing, so the equivalence test "
                 "would compare the repair with itself")
    if b"0000000000 " in twin_blob.split(b"xref", 1)[-1][:4096] and \
            twin_blob.count(b"0000000000 ") > 1:
        sys.exit("FATAL: intact twin appears to carry zeroed xref offsets")
    print("wrote compat_rebuilt_xref.pdf (xref damaged + reconstructable)")


def main():
    gen_objstm_xref()
    gen_incremental()
    gen_linearized()
    gen_rebuilt_xref()


if __name__ == "__main__":
    main()
