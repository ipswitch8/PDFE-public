# -*- coding: utf-8 -*-
"""
Generator for a CMYK / Separation-spot / overprint colour fixture, for a
colour-preservation oracle.

Output: adv_spot_overprint.pdf (1 page) with a content stream that draws two
short text spans using NON-RGB colour:

  * span "EDITME" filled with DeviceCMYK via the `k` operator (`1 0 0 0 k` =
    pure cyan). An editor that degrades this to RGB is detectable.
  * span "SPOTME" filled via a Separation colourspace named "PANTONE" (tint
    transform to DeviceCMYK), selected with `/CS1 cs` + `1 scn`.

An /ExtGState with overprint (`/OP true /op true`) is referenced via `/GS1 gs`
before the CMYK span.

The page /Resources carries the /ColorSpace Separation entry and the /ExtGState
entry so a saved-file inspector can find the Separation colourspace name and the
`k` / `scn` operators in the decompressed (uncompressed here) content.

Raw byte-assembly (same style as gen_adversarial_ambient_corpus.py) so the
content stream stays uncompressed and directly greppable.

Regenerate with:  python tests/fixtures/gen_spot_overprint_fixture.py
"""
import io
import os

H = 200

# Content stream: overprint gstate + CMYK cyan span, then Separation spot span.
CONTENT = (
    b"q\n"
    b"/GS1 gs\n"                       # overprint ExtGState (OP/op true)
    b"BT\n/F1 18 Tf\n"
    b"1 0 0 0 k\n"                     # DeviceCMYK cyan fill via `k`
    b"60 120 Td\n(EDITME) Tj\n"
    b"ET\n"
    b"Q\n"
    b"q\n"
    b"BT\n/F1 18 Tf\n"
    b"/CS1 cs\n"                       # select Separation colourspace
    b"1 scn\n"                         # full tint (=> cyan via tint transform)
    b"60 80 Td\n(SPOTME) Tj\n"
    b"ET\n"
    b"Q\n"
)


def _pdf(content):
    # Object 3 (Page) /Resources embeds:
    #   /Font      /F1 -> obj 5
    #   /ColorSpace /CS1 -> [ /Separation /PANTONE /DeviceCMYK <Type-2 tint fn> ]
    #   /ExtGState  /GS1 -> << /Type /ExtGState /OP true /op true /OPM 1 >>
    # The Type-2 tint transform maps tint t in [0,1] to CMYK, C1 = cyan.
    sep_fn = (
        b"<< /FunctionType 2 /Domain [0 1] "
        b"/C0 [0 0 0 0] /C1 [1 0 0 0] /N 1 >>"
    )
    resources = (
        b"<< /Font << /F1 5 0 R >> "
        b"/ColorSpace << /CS1 [ /Separation /PANTONE /DeviceCMYK "
        + sep_fn
        + b" ] >> "
        b"/ExtGState << /GS1 << /Type /ExtGState /OP true /op true /OPM 1 >> >> "
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
    out_path = os.path.join(here, "adv_spot_overprint.pdf")
    with open(out_path, "wb") as f:
        f.write(_pdf(CONTENT))
    print("wrote", out_path)


if __name__ == "__main__":
    main()
