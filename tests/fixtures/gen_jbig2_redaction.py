# -*- coding: utf-8 -*-
"""
Generator for the JBIG2-decode + REDACTION fixture that closes gap A1-88
(docs/audit/meta-audit-2026-07/status-register.md, from
docs/audit/input-domain-audit.md:126,150 — "Runner-up: JBIG2 decode + redaction
path", evidence "zero fixtures for the decode/redact path").

Before this fixture the only JBIG2 mention in the suite was
tests/test_cve_replay.cpp:224-229 (a CVE replay), so neither the JBIG2 decode
path nor — far more importantly — the CONFIDENTIALITY of a redaction applied
over a JBIG2 image had any test at all.

Output: jbig2_redaction.pdf — one 300x300 page containing

  /ImJ  64x64  1-bit  /DeviceGray  /Filter /JBIG2Decode
        drawn with `200 0 0 200 50 50 cm` -> user-space x 50..250, y 50..250
  a "KEEPVISIBLE" text line at user (50, 270), OUTSIDE the image, as the
        must-survive control for the redaction test.

AUTHORED BITMAP GROUND TRUTH (image row 0 is the TOP row when placed, per the
PDF unit-square image convention).  The 64x64 bitmap is four 32x32 quadrants:

    +----------------+----------------+
    |  TL: all BLACK |  TR: all WHITE |     image rows  0..31
    +----------------+----------------+
    |  BL: speckle   |  BR: all BLACK |     image rows 32..63
    +----------------+----------------+
       cols 0..31        cols 32..63

  * TL and BR are the authored DARK patches the render oracle looks for.
  * TR is the authored LIGHT patch (proves the decode is not just "all black").
  * BL is a deterministic 50%-density speckle produced by a fixed 32-bit LCG
    (seed 0x5EED1234).  Its only job is ENTROPY: it makes the MMR-coded byte
    string long and unique, so the redaction test can scan the SAVED file for a
    long verbatim run of the original coded bytes and prove they are gone.
    No oracle asserts on BL's appearance.

JBIG2 ENCODING PATH (why this is reproducible without a JBIG2 encoder):
  A JBIG2 embedded stream is just a sequence of segments.  A generic region
  segment with the MMR flag set carries ITU-T T.6 (a.k.a. CCITT Group 4) coded
  data verbatim — exactly what a TIFF `compression="group4"` strip contains.
  So the coded bytes are obtained deterministically from Pillow, and this
  script only has to assemble the (fully specified, fixed-layout) segment
  headers around them:

    segment 0  type 48 (page information), 19 bytes of data
    segment 1  type 38 (immediate generic region), MMR=1

  Segment header layout (ITU-T T.88 section 7.2), all big-endian:
    4 bytes  segment number
    1 byte   flags: bits 0-5 = type, bit 6 = page-association size (0 => 1 byte)
    1 byte   referred-to segment count/retain flags (0x00 => zero referred-to)
    1 byte   page association
    4 bytes  data length
  Generic region segment data (T.88 section 7.4.6):
    17 bytes region segment information (w, h, x, y, 1 flags byte)
    1 byte   generic region flags; bit 0 = MMR  (0x01 => MMR, so no AT pixels)
    ...      the MMR-coded data

POLARITY: T.6 codes runs starting with WHITE, and JBIG2 defines 1 = black.  The
`_MMR_INVERT` switch below is set from an empirical round-trip through MuPDF
(the fixture was rendered and its quadrants compared against this authored
bitmap); flip it if a future MuPDF ever changes the convention.  The C++ oracle
in tests/test_jbig2_redaction.cpp asserts the authored quadrants directly, so a
polarity error there is loudly RED rather than silent.

Regenerate with:  python tests/fixtures/gen_jbig2_redaction.py
"""
import io
import os
import struct

from PIL import Image

# --- page / placement geometry (PDF user space, y-up) ------------------------
PAGE_W = 300
PAGE_H = 300
IMG_X = 50
IMG_Y = 50
IMG_DRAW_W = 200
IMG_DRAW_H = 200
TEXT_X = 50
TEXT_Y = 270
KEEP_TEXT = b"KEEPVISIBLE"

# --- authored bitmap ---------------------------------------------------------
BM_W = 64
BM_H = 64
HALF = 32
LCG_SEED = 0x5EED1234

# Pillow's mode-"1" TIFF/G4 encoder writes photometric=1 (BlackIsZero) and the
# libtiff fax coder treats a 0 bit as the "white" run colour, i.e. the coded
# runs are the complement of what JBIG2 wants (JBIG2: 1 = black).  Feeding the
# encoder the INVERTED bitmap therefore yields MMR data whose "black" runs are
# our authored black pixels.  Verified empirically end-to-end through MuPDF.
_MMR_INVERT = True


def _authored_bitmap():
    """Return a list of BM_H rows, each a list of BM_W ints (1 = black)."""
    rows = []
    state = LCG_SEED
    for y in range(BM_H):
        row = []
        for x in range(BM_W):
            if x < HALF and y < HALF:
                row.append(1)            # TL: black
            elif x >= HALF and y >= HALF:
                row.append(1)            # BR: black
            elif x >= HALF and y < HALF:
                row.append(0)            # TR: white
            else:
                # BL: deterministic 50% speckle (numerical-recipes LCG).
                state = (1664525 * state + 1013904223) & 0xFFFFFFFF
                row.append((state >> 16) & 1)
        rows.append(row)
    return rows


def _mmr_bytes(rows):
    """T.6 / Group-4 coded bytes for the authored bitmap, via Pillow TIFF."""
    img = Image.new("1", (BM_W, BM_H), 1)
    px = img.load()
    for y, row in enumerate(rows):
        for x, v in enumerate(row):
            black = bool(v)
            if _MMR_INVERT:
                black = not black
            px[x, y] = 0 if black else 1
    buf = io.BytesIO()
    # One strip for the whole image: G4 coding is stateful across rows, so a
    # multi-strip file would restart the coder and not be a single MMR stream.
    img.save(buf, format="TIFF", compression="group4",
             tiffinfo={278: BM_H})
    buf.seek(0)
    t = Image.open(buf)
    offsets = t.tag_v2[273]
    counts = t.tag_v2[279]
    if len(offsets) != 1 or len(counts) != 1:
        raise RuntimeError("expected a single TIFF strip, got %d" % len(offsets))
    raw = buf.getvalue()
    data = raw[offsets[0]:offsets[0] + counts[0]]
    if not data:
        raise RuntimeError("empty MMR strip")
    return data


def _segment(number, seg_type, page_assoc, data):
    """T.88 section 7.2 segment header (short forms) + segment data."""
    if not 0 <= seg_type <= 0x3F:
        raise ValueError("segment type out of range")
    return (struct.pack(">I", number)
            + bytes([seg_type & 0x3F])   # bit6=0 => 1-byte page association
            + bytes([0x00])              # zero referred-to segments
            + bytes([page_assoc])
            + struct.pack(">I", len(data))
            + data)


def _page_info_segment():
    """T.88 section 7.4.8 page information segment (19 bytes of data)."""
    data = (struct.pack(">I", BM_W)      # page width
            + struct.pack(">I", BM_H)    # page height
            + struct.pack(">I", 0)       # x resolution (unknown)
            + struct.pack(">I", 0)       # y resolution (unknown)
            + bytes([0x01])              # flags: bit0 = eventually lossless
            + struct.pack(">H", 0))      # striping information: not striped
    assert len(data) == 19
    return _segment(0, 48, 1, data)


def _generic_region_segment(mmr):
    """T.88 section 7.4.6 immediate generic region, MMR=1."""
    region_info = (struct.pack(">I", BM_W)   # region width
                   + struct.pack(">I", BM_H)  # region height
                   + struct.pack(">I", 0)     # region x
                   + struct.pack(">I", 0)     # region y
                   + bytes([0x00]))           # external combination op = OR
    assert len(region_info) == 17
    data = region_info + bytes([0x01]) + mmr  # 0x01 => MMR set, no AT pixels
    return _segment(1, 38, 1, data)


def jbig2_stream():
    mmr = _mmr_bytes(_authored_bitmap())
    return _page_info_segment() + _generic_region_segment(mmr), mmr


CONTENT_TEMPLATE = (
    b"q\n%d 0 0 %d %d %d cm\n/ImJ Do\nQ\n"
    % (IMG_DRAW_W, IMG_DRAW_H, IMG_X, IMG_Y)
    + b"BT\n/F1 14 Tf\n0 0 0 rg\n%d %d Td\n(" % (TEXT_X, TEXT_Y)
    + KEEP_TEXT
    + b") Tj\nET\n"
)


def _pdf(jbig2_data):
    resources = (b"<< /XObject << /ImJ 5 0 R >> "
                 b"/Font << /F1 6 0 R >> >>")
    im = (b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
          b"/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /JBIG2Decode "
          b"/Length %d >>\nstream\n" % (BM_W, BM_H, len(jbig2_data))
          + jbig2_data + b"\nendstream")
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources "
        % (PAGE_W, PAGE_H) + resources + b" /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(CONTENT_TEMPLATE)
        + CONTENT_TEMPLATE + b"endstream",
        im,
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = io.BytesIO()
    out.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, obj in enumerate(objs, 1):
        offsets.append(out.tell())
        out.write(b"%d 0 obj\n" % i + obj + b"\nendobj\n")
    xref = out.tell()
    n = len(objs) + 1
    out.write(b"xref\n0 %d\n" % n + b"0000000000 65535 f \n")
    for k in range(1, n):
        out.write(b"%010d 00000 n \n" % offsets[k])
    out.write(
        b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (n, xref)
    )
    return out.getvalue()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    stream, mmr = jbig2_stream()
    out_path = os.path.join(here, "jbig2_redaction.pdf")
    with open(out_path, "wb") as f:
        f.write(_pdf(stream))
    print("wrote", out_path)
    print("jbig2 stream bytes:", len(stream), " MMR-coded bytes:", len(mmr))
    print("MMR data begins at offset %d of the stream" % (len(stream) - len(mmr)))
    print("MMR hex:", mmr.hex())


if __name__ == "__main__":
    main()
