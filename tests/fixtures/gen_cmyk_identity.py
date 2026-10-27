# -*- coding: utf-8 -*-
"""
Generator for a DeviceCMYK / DeviceGray / Separation-spot colour-IDENTITY
fixture, for the R3-06 colour-operator-identity oracle
(tests/test_cmyk_identity_preservation.cpp).

Unlike adv_spot_overprint.pdf (which pins document-level resource/spot
SURVIVAL), this fixture pins the EDITED span's colour-OPERATOR identity: after a
text edit + save, the injected replacement must be re-emitted with the ORIGINAL
colourspace operator (`k` for DeviceCMYK, `g` for DeviceGray, `/CS1 cs … scn`
for the Separation spot), NOT flattened to a visually-equivalent DeviceRGB `rg`.

Output: cmyk_identity.pdf (1 page, uncompressed / directly greppable content)
with five short text spans, each drawn under a distinct fill colour:

  * "PUREK"  — DeviceCMYK pure black  `0 0 0 1 k`
  * "RICHBK" — DeviceCMYK rich black  `0.2 0.2 0.2 1 k`
  * "SPOTA"  — Separation "PANTONE" spot  `/CS1 cs 1 scn`   (edited by the test)
  * "SPOTB"  — Separation "PANTONE" spot  `/CS1 cs 1 scn`   (kept, so /CS1 stays
                                                             referenced/alive)
  * "RGBRED" — DeviceRGB  `1 0 0 rg`  (regression control: RGB must stay rg)

Raw byte-assembly (same style as gen_spot_overprint_fixture.py) so the content
stream stays uncompressed and directly greppable.

Regenerate with:  python tests/fixtures/gen_cmyk_identity.py
"""
import io
import os

H = 220

# One BT/ET text object per span, each in its own q…Q so the fill colour is
# scoped and does not leak between spans.
CONTENT = (
    b"q\nBT\n/F1 18 Tf\n"
    b"0 0 0 1 k\n"                    # DeviceCMYK pure black
    b"60 180 Td\n(PUREK) Tj\n"
    b"ET\nQ\n"
    b"q\nBT\n/F1 18 Tf\n"
    b"0.2 0.2 0.2 1 k\n"              # DeviceCMYK rich black
    b"60 150 Td\n(RICHBK) Tj\n"
    b"ET\nQ\n"
    b"q\nBT\n/F1 18 Tf\n"
    b"/CS1 cs\n1 scn\n"               # Separation spot (edited)
    b"60 120 Td\n(SPOTA) Tj\n"
    b"ET\nQ\n"
    b"q\nBT\n/F1 18 Tf\n"
    b"/CS1 cs\n1 scn\n"               # Separation spot (kept alive)
    b"60 90 Td\n(SPOTB) Tj\n"
    b"ET\nQ\n"
    b"q\nBT\n/F1 18 Tf\n"
    b"1 0 0 rg\n"                     # DeviceRGB red (regression control)
    b"60 60 Td\n(RGBRED) Tj\n"
    b"ET\nQ\n"
)


def _pdf(content):
    # /Resources: /Font /F1 -> Helvetica; /ColorSpace /CS1 -> a Separation
    # "PANTONE" over DeviceCMYK with a Type-2 tint transform (tint 1 -> cyan).
    sep_fn = (
        b"<< /FunctionType 2 /Domain [0 1] "
        b"/C0 [0 0 0 0] /C1 [1 0 0 0] /N 1 >>"
    )
    resources = (
        b"<< /Font << /F1 5 0 R >> "
        b"/ColorSpace << /CS1 [ /Separation /PANTONE /DeviceCMYK "
        + sep_fn
        + b" ] >> "
        b">>"
    )
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 %d] "
        b"/Resources " % H + resources + b" /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
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
    out_path = os.path.join(here, "cmyk_identity.pdf")
    with open(out_path, "wb") as f:
        f.write(_pdf(CONTENT))
    print("wrote", out_path)


if __name__ == "__main__":
    main()
