# -*- coding: utf-8 -*-
"""
Generator for the DeviceCMYK / Separation raster IMAGE-XObject fixture that
closes gap A1-22 (adversarial hostile-class 5: "CMYK/Separation IMAGE (raster)"
— docs/audit/phase3-adversarial-corpus.md:27).

The pre-existing CMYK fixtures (adv_cmyk_fill.pdf, cmyk_identity.pdf) exercise
only CONTENT-STREAM FILL operators (`k`, `scn`).  Nothing in the suite pushed a
raster image whose *sample data* lives in a subtractive colour space through the
decode + colour-convert path, so a defect in the image colour pipeline (channel
swap/rotation, a dropped K plate, a Separation tint transform that is ignored)
had no oracle at all.

Output: cmyk_image_xobject.pdf — one 400x200 page, no text, two image XObjects,
both stored with **no /Filter** so the raw component bytes stay greppable and
the fixture is trivially auditable:

  /ImA  24x4  8-bit  /DeviceCMYK        — six authored patches, 4 px wide each
  /ImB  12x4  8-bit  [/Separation /SpotRed /DeviceCMYK <type-2 tint fn>]
                                        — three authored tints, 4 px wide each

AUTHORED GROUND TRUTH (this file is the source of truth; the C++ oracle in
tests/test_cmyk_image_xobject.cpp repeats these literals and derives its
expectations from them, never from PDFE output):

  /ImA patches, left -> right, (C, M, Y, K) as 8-bit component values:
    0  (255,   0,   0,   0)   pure cyan
    1  (  0, 255,   0,   0)   pure magenta
    2  (  0,   0, 255,   0)   pure yellow
    3  ( 26, 102, 179,  64)   mixed CMY *with* a K plate
    4  ( 26, 102, 179,   0)   the SAME CMY, K dropped   <- K-plate discriminator
    5  (  0,   0,   0, 255)   pure K (black)

  Patch 3 vs patch 4 is the whole point of the fixture: under ANY monotone
  colour transform (naive 255*(1-c)*(1-k), MuPDF's fast additive formula, or a
  fully ICC-managed conversion) adding ink to the K plate must make patch 3
  strictly DARKER than patch 4.  A pipeline that drops K makes them identical.
  Patch 3's three authored components are also pairwise well separated
  (R > G > B by a wide margin), so any channel swap or rotation inverts the
  authored ordering.

  /ImB tints, left -> right (1 component, /Decode default [0 1]):
    0  tint   0  -> CMYK(0, 0, 0, 0)         white
    1  tint 128  -> CMYK(0, .502, .502, 0)   mid pink
    2  tint 255  -> CMYK(0, 1, 1, 0)         saturated red

  The tint transform is a type-2 (exponential) function C0=[0 0 0 0],
  C1=[0 1 1 0], N=1 — i.e. the spot colourant is 100% M + 100% Y.  A pipeline
  that ignores the tint transform and renders the 1-component samples as grey
  produces R == G == B, which the oracle rejects.

PAGE GEOMETRY (PDF user space, y-up; the oracle converts to device y-down):
    /ImA  placed with `320 0 0 60 40 110 cm`  -> x 40..360, y 110..170
    /ImB  placed with `320 0 0 60 40  30 cm`  -> x 40..360, y  30.. 90

Raw byte assembly (same style as gen_cmyk_identity.py) so the content stream and
both sample arrays stay uncompressed and directly greppable.

Regenerate with:  python tests/fixtures/gen_cmyk_image_xobject.py
"""
import io
import os

PAGE_W = 400
PAGE_H = 200

# --- /ImA : DeviceCMYK -------------------------------------------------------
# Six authored patches, each PATCH_PX columns wide, IMG_A_H rows tall.
CMYK_PATCHES = [
    (255, 0, 0, 0),        # 0 pure cyan
    (0, 255, 0, 0),        # 1 pure magenta
    (0, 0, 255, 0),        # 2 pure yellow
    (26, 102, 179, 64),    # 3 mixed CMY with K
    (26, 102, 179, 0),     # 4 same CMY, K dropped
    (0, 0, 0, 255),        # 5 pure K
]
PATCH_PX = 4
IMG_A_H = 4
IMG_A_W = PATCH_PX * len(CMYK_PATCHES)

# --- /ImB : Separation "SpotRed" over DeviceCMYK -----------------------------
SEP_TINTS = [0, 128, 255]
IMG_B_H = 4
IMG_B_W = PATCH_PX * len(SEP_TINTS)

# Placement (user space).
IMG_X = 40
IMG_W = 320
IMG_A_Y = 110
IMG_B_Y = 30
IMG_DRAW_H = 60


def _cmyk_samples():
    """Raw 8-bit interleaved CMYK samples, row-major, no padding (8 bpc)."""
    row = bytearray()
    for c, m, y, k in CMYK_PATCHES:
        row += bytes([c, m, y, k]) * PATCH_PX
    assert len(row) == IMG_A_W * 4
    return bytes(row) * IMG_A_H


def _sep_samples():
    """Raw 8-bit single-component Separation tint samples."""
    row = bytearray()
    for t in SEP_TINTS:
        row += bytes([t]) * PATCH_PX
    assert len(row) == IMG_B_W
    return bytes(row) * IMG_B_H


CONTENT = (
    b"q\n%d 0 0 %d %d %d cm\n/ImA Do\nQ\n" % (IMG_W, IMG_DRAW_H, IMG_X, IMG_A_Y)
    + b"q\n%d 0 0 %d %d %d cm\n/ImB Do\nQ\n" % (IMG_W, IMG_DRAW_H, IMG_X, IMG_B_Y)
)


def _pdf():
    cmyk_data = _cmyk_samples()
    sep_data = _sep_samples()

    tint_fn = (
        b"<< /FunctionType 2 /Domain [0 1] "
        b"/C0 [0 0 0 0] /C1 [0 1 1 0] /N 1 >>"
    )

    resources = b"<< /XObject << /ImA 5 0 R /ImB 6 0 R >> >>"

    im_a = (
        b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        b"/ColorSpace /DeviceCMYK /BitsPerComponent 8 /Length %d >>\nstream\n"
        % (IMG_A_W, IMG_A_H, len(cmyk_data))
        + cmyk_data
        + b"\nendstream"
    )
    im_b = (
        b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        b"/ColorSpace [ /Separation /SpotRed /DeviceCMYK 7 0 R ] "
        b"/BitsPerComponent 8 /Length %d >>\nstream\n"
        % (IMG_B_W, IMG_B_H, len(sep_data))
        + sep_data
        + b"\nendstream"
    )

    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources "
        % (PAGE_W, PAGE_H) + resources + b" /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        im_a,
        im_b,
        tint_fn,
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
    out_path = os.path.join(here, "cmyk_image_xobject.pdf")
    with open(out_path, "wb") as f:
        f.write(_pdf())
    print("wrote", out_path)


if __name__ == "__main__":
    main()
