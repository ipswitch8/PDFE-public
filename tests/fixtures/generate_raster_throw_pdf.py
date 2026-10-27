# -*- coding: utf-8 -*-
"""Generate test_r205_render.pdf — the renderable fixture behind the TileRenderer
raster-leak oracle (remediation R2-05).

This is a minimal, fully valid PDF whose single page draws large text with a
base-14 font (Helvetica, no embedding). Rasterizing it allocates well beyond the
tile pixmap.

How the raster *throw* is produced (see tests/test_tile_renderer_leak.cpp):
    A corrupt embedded IMAGE does NOT throw — MuPDF's image decoders treat a bad
    stream as a recoverable FORMAT error and keep rendering. So the oracle
    instead wires a custom counting fz allocator that injects a one-shot
    OUT-OF-MEMORY fault mid-rasterization (OOM is NOT recovered — it propagates
    out of fz_run_display_list). The fault is armed only AFTER the tile pixmap +
    draw device are allocated, which is exactly the R2-05 leak window.

The C++ oracle builds an identical PDF at runtime (hermetic — it cannot silently
skip). This generator emits a committed, inspectable copy of the same fixture
for manual analysis.

Run:  python generate_raster_throw_pdf.py
"""

import io

OUT = "test_r205_render.pdf"


def build_pdf() -> bytes:
    content = (
        b"BT /F1 300 Tf 20 40 Td (Leak R2-05) Tj ET\n"
        b"BT /F1 300 Tf 20 360 Td (Test AAA) Tj ET"
    )
    content_dict = b"<< /Length " + str(len(content)).encode("ascii") + b" >>"

    buf = io.BytesIO()
    buf.write(b"%PDF-1.5\n%\xE2\xE3\xCF\xD3\n")
    offsets = [0] * 6

    def emit_obj(n: int, dictionary: bytes, stream: bytes | None) -> None:
        offsets[n] = buf.tell()
        buf.write(str(n).encode("ascii") + b" 0 obj\n" + dictionary + b"\n")
        if stream is not None:
            buf.write(b"stream\n")
            buf.write(stream)
            buf.write(b"\nendstream\n")
        buf.write(b"endobj\n")

    emit_obj(1, b"<< /Type /Catalog /Pages 2 0 R >>", None)
    emit_obj(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>", None)
    emit_obj(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        None,
    )
    emit_obj(4, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>", None)
    emit_obj(5, content_dict, content)

    xref_off = buf.tell()
    buf.write(b"xref\n0 6\n")
    buf.write(b"0000000000 65535 f \n")
    for n in range(1, 6):
        buf.write(("%010d 00000 n \n" % offsets[n]).encode("ascii"))
    buf.write(
        b"trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n"
        + str(xref_off).encode("ascii")
        + b"\n%%EOF\n"
    )
    return buf.getvalue()


def main() -> None:
    data = build_pdf()
    with open(OUT, "wb") as fh:
        fh.write(data)
    print("wrote %s (%d bytes)" % (OUT, len(data)))


if __name__ == "__main__":
    main()
