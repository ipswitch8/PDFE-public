# -*- coding: utf-8 -*-
"""
Generate the PDF *version-handling* corpus (meta-audit follow-up F14, PDF-01 /
PDF-07 / PDF-08).

Every file is a hand-assembled, minimal, single-page PDF with a correct classic
cross-reference table. They are deliberately tiny (< 1 KB each) and fully
described by this script -- no opaque blobs.

The corpus pins the three things the product must get right:

  1. the *header* version (``%PDF-x.y``) is reported as authored,
  2. ``/Root/Version`` OVERRIDES the header when it is LATER
     (ISO 32000-1 s7.5.5 / ISO 32000-2 s7.5.5),
  3. ``/Root/Version`` is IGNORED when it is EARLIER than the header
     (the spec's override is one-way -- it may only raise).

Usage:  python gen_pdf_version_corpus.py [--outdir DIR]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent

# A tiny uncompressed content stream: one line of Helvetica text.
CONTENT = b"BT /F1 24 Tf 72 700 Td (PDFE version fixture) Tj ET\n"


def build_pdf(header: str, catalog_version: str | None, title: str) -> bytes:
    """Assemble a minimal 1-page PDF with a classic xref table.

    :param header:          e.g. ``"1.4"`` -- emitted as ``%PDF-1.4``.
    :param catalog_version: e.g. ``"2.0"`` -> ``/Version /2.0`` in the catalog;
                            ``None`` omits the key entirely.
    :param title:           /Title string, so each fixture self-identifies.
    """
    ver_key = f"/Version /{catalog_version}".encode("ascii") if catalog_version else b""

    objects: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R " + ver_key + b" >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length " + str(len(CONTENT)).encode("ascii") + b" >>\nstream\n"
        + CONTENT + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Title (" + title.encode("ascii") + b") >>",
    ]

    # %PDF header + a binary comment line (>= 4 bytes >= 0x80), as the spec
    # recommends so byte-mode transfer detection works.
    out = bytearray()
    out += f"%PDF-{header}\n".encode("ascii")
    out += b"%\xe2\xe3\xcf\xd3\n"

    offsets: list[int] = []
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode("ascii") + body + b"\nendobj\n"

    startxref = len(out)
    n = len(objects) + 1
    out += f"xref\n0 {n}\n".encode("ascii")
    out += b"0000000000 65535 f \n"
    for off in offsets:
        out += f"{off:010d} 00000 n \n".encode("ascii")
    out += (
        b"trailer\n<< /Size " + str(n).encode("ascii")
        + b" /Root 1 0 R /Info 6 0 R >>\nstartxref\n"
        + str(startxref).encode("ascii") + b"\n%%EOF\n"
    )
    return bytes(out)


# name -> (header version, /Root/Version, expected EFFECTIVE version, title)
CORPUS: dict[str, tuple[str, str | None, str, str]] = {
    # --- plain header versions, no catalog override -------------------------
    "version_header_1_4.pdf": ("1.4", None, "1.4", "PDFE version fixture 1.4"),
    "version_header_1_7.pdf": ("1.7", None, "1.7", "PDFE version fixture 1.7"),
    "version_header_2_0.pdf": ("2.0", None, "2.0", "PDFE version fixture 2.0"),
    # --- catalog /Version LATER than the header: it wins (raises) -----------
    "version_catalog_raises_2_0.pdf": (
        "1.4", "2.0", "2.0", "PDFE version fixture catalog-raises",
    ),
    # --- catalog /Version EARLIER than the header: it is IGNORED ------------
    "version_catalog_older_1_4.pdf": (
        "1.7", "1.4", "1.7", "PDFE version fixture catalog-older",
    ),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=str(THIS_DIR))
    args = ap.parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    for name, (header, cat, effective, title) in CORPUS.items():
        data = build_pdf(header, cat, title)
        (outdir / name).write_bytes(data)
        print(
            f"wrote {name:34s} {len(data):5d} bytes  "
            f"header={header} catalog={cat or '-'} effective={effective}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
