# -*- coding: utf-8 -*-
"""Deterministic boundary fixtures for the "Create PDF from Images" wizard
(gap A1-31 — tests/test_image_to_pdf_wizard.cpp was happy-path-only).

Produces two NON-PDF image fixtures (so the FixtureCorpus PDF guards are
unaffected):

  image_wizard_corrupt.png
      A valid 8-byte PNG signature + a valid, CRC-correct IHDR for a small
      64x64 RGB image, followed by an IDAT chunk that is TRUNCATED mid-stream
      and no IEND. Every PNG sniffer accepts the magic; every PNG *decoder*
      must fail. This is the "corrupt / truncated image" boundary case: the
      file is not simply missing (already covered by the pre-existing
      UnreadableImageSkipped* tests) — it is present, plausible and undecodable.

  image_wizard_oversized.png
      A valid 8-byte PNG signature + a CRC-correct IHDR declaring
      65535 x 65535 RGBA (~17 GB of pixels), followed by the IDAT/IEND of a
      real 1x1 image. Qt's PNG handler allocates the destination QImage from
      the IHDR dimensions via QImageIOHandler::allocateImage(), which enforces
      QImageReader::allocationLimit() (256 MiB by default), so decoding must
      be refused BEFORE any 17 GB allocation is attempted. This is the
      "oversized image" boundary case in its purely declarative form: the file
      on disk stays ~100 bytes, so the fixture itself costs nothing.

Both files are byte-for-byte reproducible: no timestamps, no randomness.

Run:  python tests/fixtures/gen_image_wizard_boundary.py
"""

import io
import os
import struct
import zlib

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _chunk(chunk_type: bytes, payload: bytes) -> bytes:
    """Build a complete, CRC-correct PNG chunk."""
    return (
        struct.pack(">I", len(payload))
        + chunk_type
        + payload
        + struct.pack(">I", zlib.crc32(chunk_type + payload) & 0xFFFFFFFF)
    )


def _ihdr(width: int, height: int, bit_depth: int, color_type: int) -> bytes:
    payload = struct.pack(
        ">IIBBBBB",
        width,
        height,
        bit_depth,
        color_type,
        0,  # compression: deflate
        0,  # filter: adaptive
        0,  # interlace: none
    )
    return _chunk(b"IHDR", payload)


def _real_png_bytes(width: int, height: int, color: tuple) -> bytes:
    """Encode a real PNG with Pillow, deterministically."""
    img = Image.new("RGB", (width, height), color)
    buf = io.BytesIO()
    # optimize=False keeps zlib output stable across Pillow versions' heuristics.
    img.save(buf, format="PNG", optimize=False, compress_level=6)
    return buf.getvalue()


def _split_chunks(png: bytes):
    """Yield (chunk_type, whole_chunk_bytes) for a well-formed PNG."""
    assert png[:8] == PNG_SIGNATURE, "not a PNG"
    pos = 8
    while pos < len(png):
        (length,) = struct.unpack(">I", png[pos : pos + 4])
        ctype = png[pos + 4 : pos + 8]
        end = pos + 12 + length
        yield ctype, png[pos:end]
        pos = end


def write_corrupt(path: str) -> None:
    """Valid signature + valid IHDR + truncated IDAT, no IEND."""
    real = _real_png_bytes(64, 64, (200, 30, 30))
    idat = b""
    for ctype, whole in _split_chunks(real):
        if ctype == b"IDAT":
            idat = whole
            break
    assert idat, "expected an IDAT chunk in the Pillow output"

    out = bytearray()
    out += PNG_SIGNATURE
    out += _ihdr(64, 64, 8, 2)  # 8-bit RGB, matches what we truncate
    # Keep the IDAT length/type/CRC header claim intact but cut the deflate
    # payload in half so the zlib stream ends mid-block. No IEND follows.
    keep = 12 + max(1, (len(idat) - 12) // 2)
    out += idat[:keep]

    with open(path, "wb") as fh:
        fh.write(bytes(out))


def write_oversized(path: str) -> None:
    """Valid signature + IHDR declaring 65535x65535 RGBA + a real 1x1 body."""
    real = _real_png_bytes(1, 1, (10, 10, 10))

    out = bytearray()
    out += PNG_SIGNATURE
    out += _ihdr(65535, 65535, 8, 6)  # 8-bit RGBA => 65535*65535*4 ~= 17 GB
    for ctype, whole in _split_chunks(real):
        if ctype in (b"IDAT", b"IEND"):
            out += whole

    with open(path, "wb") as fh:
        fh.write(bytes(out))


def main() -> None:
    corrupt = os.path.join(HERE, "image_wizard_corrupt.png")
    oversized = os.path.join(HERE, "image_wizard_oversized.png")
    write_corrupt(corrupt)
    write_oversized(oversized)

    with io.open(os.devnull, "w", encoding="utf-8"):
        pass  # explicit-encoding discipline for any text IO added later

    for p in (corrupt, oversized):
        print(u"wrote {0} ({1} bytes)".format(os.path.basename(p), os.path.getsize(p)))


if __name__ == "__main__":
    main()
