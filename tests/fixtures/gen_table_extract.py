# -*- coding: utf-8 -*-
"""
Generator for a TABLE-STRUCTURE text-extraction fidelity fixture (R3-07).

Output: table_extract.pdf (1 page, US Letter 612x792).

Writes a deterministic 3-row x 3-column grid of text cells at KNOWN absolute
positions. Each cell's glyph string encodes its (row, col) as `R<r>C<c>`
(e.g. `R0C2` = row 0, column 2) so a fidelity oracle can assert the
reconstructed reading order token-by-token with zero ambiguity.

Layout (PDF user space, origin BOTTOM-left; larger y == physically HIGHER):

        col0 x=100      col1 x=280      col2 x=460
  row0  R0C0            R0C1            R0C2          y=700  (top)
  row1  R1C0            R1C1            R1C2          y=650
  row2  R2C0            R2C1            R2C2          y=600  (bottom)

Rows are 50pt apart in y (well-separated y-bands so MuPDF groups each visual
row into ONE stext line); columns are 180pt apart in x. All cells use the
base-14 Helvetica at 12pt, so extraction is plain ASCII by construction and
needs no embedded font or ToUnicode map.

The correct, defensible table reading contract (verified empirically against
MuPDF stext — see tests/test_fidelity_r3_07_text_extraction.cpp) is:
  * cells in the same visual ROW share a y-band and extract LEFT-TO-RIGHT in
    column order (R<r>C0, R<r>C1, R<r>C2);
  * the whole document reads ROW-MAJOR top-to-bottom: every cell of row 0
    precedes every cell of row 1, which precedes every cell of row 2.

Written as RAW PDF BYTES (no reportlab / no external dependency) so a clean
checkout or CI can regenerate it deterministically:

    python tests/fixtures/gen_table_extract.py
"""
import os

# ---- Grid definition (single source of truth; mirrored by the C++ oracle) ----
COLS_X = [100, 280, 460]          # x baseline of each column (left -> right)
ROWS_Y = [700, 650, 600]          # y baseline of each row (top -> bottom)
FONT_SIZE = 12


def _cell_token(r, c):
    return "R%dC%d" % (r, c)


def _build_content_stream():
    """Return the page content-stream bytes drawing every cell with Tj."""
    parts = []
    for r, y in enumerate(ROWS_Y):
        for c, x in enumerate(COLS_X):
            token = _cell_token(r, c)
            # Each cell is its own text object at an absolute position.
            parts.append(
                "BT\n/F1 %d Tf\n%d %d Td\n(%s) Tj\nET\n" % (FONT_SIZE, x, y, token)
            )
    return "".join(parts).encode("ascii")


def _build_pdf():
    """Assemble a minimal, valid single-page PDF with a byte-accurate xref."""
    content = _build_content_stream()

    objects = []
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    objects.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"
    )
    objects.append(
        b"<< /Length " + str(len(content)).encode("ascii") + b" >>\n"
        b"stream\n" + content + b"\nendstream"
    )
    objects.append(
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        b"/Encoding /WinAnsiEncoding >>"
    )

    out = bytearray()
    out += b"%PDF-1.7\n"
    out += b"%\xe2\xe3\xcf\xd3\n"  # binary marker

    offsets = []
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += str(i).encode("ascii") + b" 0 obj\n"
        out += body
        out += b"\nendobj\n"

    xref_pos = len(out)
    n = len(objects) + 1
    out += b"xref\n"
    out += ("0 %d\n" % n).encode("ascii")
    out += b"0000000000 65535 f \n"
    for off in offsets:
        out += ("%010d 00000 n \n" % off).encode("ascii")
    out += b"trailer\n"
    out += ("<< /Size %d /Root 1 0 R >>\n" % n).encode("ascii")
    out += b"startxref\n"
    out += (str(xref_pos) + "\n").encode("ascii")
    out += b"%%EOF\n"
    return bytes(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(here, "table_extract.pdf")
    with open(out_path, "wb") as f:
        f.write(_build_pdf())
    print("wrote", out_path)


if __name__ == "__main__":
    main()
