# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/recovery_verbatim_streams.pdf

A single-page PDF whose content stream AND image stream are both stored
VERBATIM (no /Filter, no Flate). It backs the no-recompress recovery-copy
oracle for mutants M08 and M16 in Document::writeRecoveryCopy:

  * M16: `opts.do_compress = 0;` -> `1`  (re-deflates general streams)
  * M08: `opts.do_compress_images = 0;` -> `1`  (re-encodes image streams)

writeRecoveryCopy must serialize a fast, verbatim copy (both flags 0), so:
  * the ASCII content marker "RECOVERYVERBATIMMARKER" survives in the raw
    recovery bytes (proves the content stream was NOT re-deflated); and
  * a long run of the sentinel byte 0xAB from the raw image survives in the
    raw recovery bytes (proves the image stream was NOT re-encoded).

Both payloads are large and highly compressible, so under either mutation the
corresponding stream IS deflated and its sentinel disappears from the raw bytes
— failing the oracle and killing the mutant.

Regenerate with:  python tests/fixtures/gen_recovery_verbatim_streams.py
"""
import os

W, H = 612, 792
IMG_W, IMG_H = 32, 32                       # 32*32*3 = 3072 raw RGB bytes
IMG_DATA = b"\xab" * (IMG_W * IMG_H * 3)    # constant 0xAB sentinel run

# Large, highly-compressible content stream carrying a unique ASCII marker.
# The filler makes deflate strictly smaller, so do_compress=1 WILL compress it
# (MuPDF keeps a stream uncompressed only when deflate would not shrink it).
_FILLER = b"0 0 0 rg 1 0 0 1 0 0 cm\n" * 400
CONTENT = (
    b"q %d 0 0 %d 100 500 cm /Im0 Do Q\n" % (IMG_W * 4, IMG_H * 4)
    + _FILLER
    + b"BT /F1 24 Tf 100 700 Td (RECOVERYVERBATIMMARKER) Tj ET\n"
)


def _obj(num, body):
    return b"%d 0 obj\n" % num + body + b"\nendobj\n"


def build():
    out = bytearray()
    out += b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    offsets = {}
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
         b"/Resources << /Font << /F1 5 0 R >> /XObject << /Im0 6 0 R >> >> "
         b"/Contents 4 0 R >>" % (W, H)),
        # Object 4: content stream — UNCOMPRESSED (no /Filter).
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        # Object 5: font.
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        # Object 6: image XObject — UNCOMPRESSED raw RGB (no /Filter).
        (b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
         b"/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %d >>\nstream\n"
         % (IMG_W, IMG_H, len(IMG_DATA))) + IMG_DATA + b"\nendstream",
    ]
    for i, body in enumerate(objs, 1):
        offsets[i] = len(out)
        out += _obj(i, body)

    xref = len(out)
    n = len(objs) + 1
    out += b"xref\n0 %d\n" % n
    out += b"0000000000 65535 f \n"
    for k in range(1, n):
        out += b"%010d 00000 n \n" % offsets[k]
    out += (b"trailer\n<< /Size %d /Root 1 0 R >>\n"
            b"startxref\n%d\n%%%%EOF\n" % (n, xref))
    return bytes(out)


def main():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "recovery_verbatim_streams.pdf")
    with open(path, "wb") as f:
        f.write(build())
    print("wrote", path)


if __name__ == "__main__":
    main()
