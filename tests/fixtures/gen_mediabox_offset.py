# -*- coding: utf-8 -*-
"""Generate tests/fixtures/mediabox_offset.pdf for the R3-12 origin-crop test.

A single-page PDF whose MediaBox has a NON-ZERO origin: [100 100 700 900]
(600 x 800 pt extent, origin at (100,100)). On a white page it paints two
solid, saturated colour blocks at KNOWN PDF user-space (Y-up) coordinates:

  * RED  block  1 0 0 rg : x[140..280], y[640..760]   (upper-left region)
  * BLUE block  0 0 1 rg : x[420..560], y[190..310]   (lower-right region)

These two blocks are well separated on BOTH axes so a crop of one can be
asserted to contain that colour and NOT the other. The test selects a smaller
rect fully inside each block (with a >=20pt margin) so that pixel rounding and
2x anti-aliasing at the block edges never leak the white background into the
crop.

The PDF is written as raw bytes (no pikepdf/reportlab dependency) so it is
deterministic and reproducible on any machine. Run this file with python to
(re)generate the .pdf; both this generator and the generated .pdf are committed.
"""

import os
import sys


def build_pdf() -> bytes:
    # Content stream: white background across the whole MediaBox, then the two
    # colour blocks. All coordinates are PDF user space (Y-up), i.e. they carry
    # the MediaBox origin (100,100).
    content = (
        b"1 1 1 rg\n"          # white
        b"100 100 600 800 re\n"
        b"f\n"
        b"1 0 0 rg\n"          # pure red
        b"140 640 140 120 re\n"
        b"f\n"
        b"0 0 1 rg\n"          # pure blue
        b"420 190 140 120 re\n"
        b"f\n"
    )

    objects = []
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    objects.append(
        b"<< /Type /Page /Parent 2 0 R "
        b"/MediaBox [100 100 700 900] "
        b"/Resources << >> "
        b"/Contents 4 0 R >>"
    )
    objects.append(
        b"<< /Length " + str(len(content)).encode("ascii") + b" >>\n"
        b"stream\n" + content + b"endstream"
    )

    out = bytearray()
    out += b"%PDF-1.7\n"
    # Binary marker so tools treat the file as binary.
    out += b"%\xe2\xe3\xcf\xd3\n"

    offsets = [0]  # object 0 is the free head
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += str(i).encode("ascii") + b" 0 obj\n"
        out += body + b"\n"
        out += b"endobj\n"

    xref_pos = len(out)
    n = len(objects) + 1  # includes the free object 0
    out += b"xref\n"
    out += b"0 " + str(n).encode("ascii") + b"\n"
    out += b"0000000000 65535 f \n"
    for off in offsets[1:]:
        out += ("%010d 00000 n \n" % off).encode("ascii")

    out += b"trailer\n"
    out += b"<< /Size " + str(n).encode("ascii") + b" /Root 1 0 R >>\n"
    out += b"startxref\n"
    out += str(xref_pos).encode("ascii") + b"\n"
    out += b"%%EOF\n"
    return bytes(out)


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    target = os.path.join(here, "mediabox_offset.pdf")
    data = build_pdf()
    with open(target, "wb") as f:
        f.write(data)
    # Report so a human running the generator sees confirmation.
    sys.stdout.reconfigure(encoding="utf-8")
    print("wrote %s (%d bytes)" % (target, len(data)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
