# -*- coding: utf-8 -*-
"""Generate tests/fixtures/hybrid_xref.pdf — a GENUINE hybrid-reference PDF
(meta-audit A1-87: `/XRefStm` on the incremental-save path, zero fixtures).

A hybrid-reference file (ISO 32000-1 §7.5.8.4) carries BOTH cross-reference
forms in the same revision:

  * a classic cross-reference TABLE (`xref` … `trailer`), for PDF-1.4 readers, and
  * a cross-reference STREAM (`/Type /XRef`, `/W`, `/Index`, FlateDecode)
    pointed at by the trailer's `/XRefStm` entry, for PDF-1.5+ readers.

What makes THIS file a real hybrid rather than a decoration: **object 5 is not
listed in the classic table at all.** The classic table is written as two
subsections, `0 5` and `6 2`, which deliberately skip object number 5; only the
`/XRefStm` cross-reference stream carries an entry for it (`/Index [5 1]`).

Object 5 is the page's SECOND content stream, and it is the only thing that
draws the token "HYBRIDONLY". So:

  * a reader that honours /XRefStm resolves object 5 and the page shows both
    "TABLEBASE" (object 4, in the classic table) and "HYBRIDONLY";
  * a reader that ignores /XRefStm cannot resolve object 5 at all and the page
    shows "TABLEBASE" only.

That asymmetry is the oracle used by tests/test_hybrid_xref_incremental_save.cpp:
the presence of "HYBRIDONLY" PROVES the /XRefStm path was taken. (MuPDF's
pdf_read_new_xref_section only fills entries whose type is still 0 — an object
listed as *free* in the classic table would NOT be recoverable from the
/XRefStm, which is exactly why object 5 is omitted rather than marked free.)

AUTHORED GROUND TRUTH:

  object  where it is findable          draws
  ------  --------------------------    -----------------------------------
    4     classic xref table            "TABLEBASE"   at (72, 700), Helv 18
    5     /XRefStm ONLY                 "HYBRIDONLY"  at (72, 660), Helv 18
    6     classic xref table            the /XRefStm cross-reference stream
    7     classic xref table            /Helvetica

  Page 1, MediaBox [0 0 612 792], /Contents [4 0 R 5 0 R].

The file is assembled as raw bytes so byte offsets are exact and the result is
deterministic; the two content streams are UNCOMPRESSED and directly greppable.
Only the cross-reference stream is Flate-encoded (it must be, to be a realistic
/XRefStm).

Regenerate with:  python tests/fixtures/gen_hybrid_xref.py
"""

import io
import os
import sys
import zlib

# --- Authored content --------------------------------------------------------
TABLE_TOKEN = b"TABLEBASE"    # object 4 — reachable via the classic table
HYBRID_TOKEN = b"HYBRIDONLY"  # object 5 — reachable ONLY via /XRefStm

CONTENT_4 = b"BT\n/F1 18 Tf\n72 700 Td\n(" + TABLE_TOKEN + b") Tj\nET\n"
CONTENT_5 = b"BT\n/F1 18 Tf\n72 660 Td\n(" + HYBRID_TOKEN + b") Tj\nET\n"

# Object numbers. 5 is the hybrid-only object; 6 is the /XRefStm itself.
HYBRID_OBJ = 5
XREFSTM_OBJ = 6
SIZE = 8  # objects 0..7


def _stream_obj(num, body_dict, data):
    return (b"%d 0 obj\n" % num + body_dict + b"\nstream\n" + data
            + b"\nendstream\nendobj\n")


def _plain_obj(num, body):
    return b"%d 0 obj\n" % num + body + b"\nendobj\n"


def build_pdf():
    out = io.BytesIO()
    out.write(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")

    offsets = {}

    offsets[1] = out.tell()
    out.write(_plain_obj(1, b"<< /Type /Catalog /Pages 2 0 R >>"))

    offsets[2] = out.tell()
    out.write(_plain_obj(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"))

    offsets[3] = out.tell()
    out.write(_plain_obj(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 7 0 R >> >> "
        b"/Contents [4 0 R 5 0 R] >>"))

    offsets[4] = out.tell()
    out.write(_stream_obj(4, b"<< /Length %d >>" % len(CONTENT_4), CONTENT_4))

    # The hybrid-only object. Its offset is what the /XRefStm — and ONLY the
    # /XRefStm — records.
    offsets[HYBRID_OBJ] = out.tell()
    out.write(_stream_obj(HYBRID_OBJ, b"<< /Length %d >>" % len(CONTENT_5),
                          CONTENT_5))

    offsets[7] = out.tell()
    out.write(_plain_obj(
        7,
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        b"/Encoding /WinAnsiEncoding >>"))

    # --- The cross-reference STREAM (object 6) --------------------------------
    # /W [1 4 2]: 1-byte type, 4-byte offset, 2-byte generation.
    # /Index [5 1]: it describes exactly one object — the hybrid-only object 5.
    entry = (b"\x01"
             + offsets[HYBRID_OBJ].to_bytes(4, "big")
             + (0).to_bytes(2, "big"))
    xstm_data = zlib.compress(entry, 9)

    offsets[XREFSTM_OBJ] = out.tell()
    out.write(_stream_obj(
        XREFSTM_OBJ,
        b"<< /Type /XRef /Size %d /Index [%d 1] /W [1 4 2] /Root 1 0 R "
        b"/Filter /FlateDecode /Length %d >>" % (SIZE, HYBRID_OBJ,
                                                 len(xstm_data)),
        xstm_data))

    # --- The classic cross-reference TABLE ------------------------------------
    # Two subsections that SKIP object 5 entirely. A PDF-1.4 reader therefore
    # has no entry for it; only the /XRefStm supplies one.
    xref_pos = out.tell()
    out.write(b"xref\n")
    out.write(b"0 5\n")
    out.write(b"0000000000 65535 f \n")
    for num in (1, 2, 3, 4):
        out.write(b"%010d 00000 n \n" % offsets[num])
    out.write(b"6 2\n")
    for num in (6, 7):
        out.write(b"%010d 00000 n \n" % offsets[num])

    out.write(b"trailer\n")
    out.write(b"<< /Size %d /Root 1 0 R /XRefStm %d >>\n"
              % (SIZE, offsets[XREFSTM_OBJ]))
    out.write(b"startxref\n%d\n%%%%EOF\n" % xref_pos)

    return out.getvalue()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    target = os.path.join(here, "hybrid_xref.pdf")
    data = build_pdf()
    with open(target, "wb") as f:
        f.write(data)
    sys.stdout.reconfigure(encoding="utf-8")
    print("wrote %s (%d bytes)" % (target, len(data)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
