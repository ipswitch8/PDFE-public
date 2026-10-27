# -*- coding: utf-8 -*-
"""
Generator for the ADVERSARIAL-AMBIENT-STATE corpus (content-stream-continuity
pipeline, Phase 1 DISCOVERY).

One minimal, hand-built PDF per hostile ambient graphics/text-state operator.
Each page:

  * draws a SAFE "KEEPME" neighbour first, inside its own q...Q with an explicit
    black fill, so the neighbour is immune to the hostile ambient state and must
    render identically no matter what an injector does elsewhere on the page;
  * then leaves ONE hostile ambient operator LIVE at the top level (never reset
    / never restored) so it is still active at the END of the base content
    stream. PDF concatenates the /Contents array's streams with NO graphics-state
    reset, so any injector that APPENDS a fragment (AlignmentInjector,
    HeaderFooterEngine, BatchEngine watermark, TRE PdfContentWriter) inherits it
    (the inbound-leak scenario);
  * the TEXT family also draws an "EDITME" span UNDER the live hostile state
    (font + position only, no local colour/scale reset) so the inline
    ContentStreamEditor path is exercised against the same hostile state.

Two families are produced:

  TEXT  family  aa_<op>.pdf      -> ContentStreamEditor / Alignment / HeaderFooter
                                    / Watermark / TRE writeTextBlock
  IMAGE family  aa_img_<op>.pdf  -> ImageManager insert + ImageContentEditor
                                    resize / rotate / crop
                (carries a distinct solid-BLUE Im0 XObject placed correctly as a
                 neighbour, so the geometry oracle can measure a leaked CTM)

Hostile operators (one fixture each):
  neg_tz      -100 Tz            (mirror)
  tc          2 Tc               (char spacing)
  ts          5 Ts               (text rise)
  tr          3 Tr               (invisible render mode)
  white       1 1 1 rg           (white fill -> invisible on white paper)
  cm          0.5 0 0 0.5 0 0 cm (imposed / half-scale CTM)
  dangling_q  q + 0.6 0.6 0.6 rg (unbalanced q -> leaked grey gstate)
  cmyk        1 0 0 0 k          (CMYK cyan fill / colourspace)
  shear       1 0 0.5 1 0 0 cm  (horizontal SKEW / shear CTM)     <-- R-33
  rotate      15deg rotation cm  (rotate CTM)                     <-- R-33

In addition to the per-op fixtures above, one MEDIABOX-OFFSET fixture is emitted:

  mediabox    MediaBox [100 100 712 892] + half-scale cm ambient (imposition/N-up)

R-34 (validator gap-closure round 2): the imposition / N-up pattern places page
content on a MediaBox whose origin is NOT (0,0). An injector that computes its
placement in a naive 0-origin page space (ignoring the MediaBox origin) drops the
injected element off the visible page. `aa_mediabox.pdf` / `aa_img_mediabox.pdf`
carry a non-zero MediaBox origin [100 100 712 892] AND leave the half-scale
"0.5 0 0 0.5 0 0 cm" ambient live, so an on-page-placement oracle can prove an
injected edit lands ON the offset page (origin added) rather than shifted off by
the origin. The base KEEPME/EDITME/Im0 content is offset by the origin so it too
lands on the visible page.

R-33 (adversarial-ambient audit, phase-2): the SHEARED/SKEWED-CTM class was the
sharpest zero-coverage gap — the untested sibling of the -100 Tz mirror bug. A
pure shear preserves the glyph's x/y scale (determinant == 1) so the half-size
CTM-reset oracle is BLIND to it; a leaked shear turns each glyph quad into a
PARALLELOGRAM (its left edge is no longer vertical). The `shear` / `rotate`
fixtures let a parallelogram-aware glyph-quad oracle prove an injected edit lands
upright and un-skewed despite the hostile ambient transform.

Regenerate with:  python tests/fixtures/gen_aa_corpus.py
"""
import io
import os

W, H = 612, 792

# R-34: MediaBox origin for the imposition / N-up mediabox-offset fixture. The
# visible box is [OX OY OX+W OY+H]; base content + injected placements must add
# this origin to land on the page.
MBOX_OX, MBOX_OY = 100, 100

# The hostile ambient operator left LIVE at the end of the base content stream.
OPS = {
    "neg_tz":     b"-100 Tz\n",
    "tc":         b"2 Tc\n",
    "ts":         b"5 Ts\n",
    "tr":         b"3 Tr\n",
    "white":      b"1 1 1 rg\n",
    "cm":         b"0.5 0 0 0.5 0 0 cm\n",
    "dangling_q": b"q\n0.6 0.6 0.6 rg\n",   # unbalanced q; leaked grey fill
    "cmyk":       b"1 0 0 0 k\n",
    # R-33: hostile SHEAR / ROTATE CTMs left live. A pure shear (det==1) keeps
    # the glyph's height+width unchanged, so it slips past the half-size CTM
    # oracle; it manifests only as a skewed (parallelogram) glyph quad. The
    # rotate CTM (15 deg) likewise leaves x/y scale ~unchanged but tilts the run.
    "shear":      b"1 0 0.5 1 0 0 cm\n",
    "rotate":     b"0.9659258 0.2588190 -0.2588190 0.9659258 0 0 cm\n",  # +15 deg
}


def _emit(objs):
    """Serialise a list of object bodies (1-based) into a minimal PDF."""
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
    out.write(b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
              % (n, xref))
    return out.getvalue()


def _text_family(hostile, ox=0, oy=0):
    """KEEPME (safe) + hostile-live + EDITME (under hostile). 18pt everywhere.

    (ox, oy) offsets the base content by the MediaBox origin so it lands on the
    visible page of a non-zero-origin (imposition/N-up) fixture; the MediaBox
    itself is [ox oy ox+W oy+H]. With ox==oy==0 the output is byte-identical to
    the historical 0-origin fixtures.
    """
    content = (
        # Safe neighbour: own q...Q, explicit black fill.
        b"q\nBT\n/F1 18 Tf\n0 0 0 rg\n%d %d Td\n(KEEPME) Tj\nET\nQ\n"
        % (72 + ox, 750 + oy)
        # Hostile ambient operator, left LIVE (never reset / restored).
        + hostile
        # EDITME under the live hostile state: font + position only, no local
        # colour/scale reset, so it inherits the hostile ambient state. This is
        # the inline ContentStreamEditor target. The hostile op remains live
        # after this ET (BT/ET does not reset Tz/Tc/Tw/Ts/Tr/colour/CTM), so an
        # APPENDING injector after it also inherits the hostile state.
        + b"BT\n/F1 18 Tf\n%d %d Td\n(EDITME) Tj\nET\n" % (72 + ox, 700 + oy)
    )
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [%d %d %d %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"
        % (ox, oy, ox + W, oy + H),
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    return _emit(objs)


def _image_family(hostile, ox=0, oy=0):
    """Solid-blue Im0 placed correctly (neighbour) + KEEPME + hostile-live.

    (ox, oy) offsets the base content + MediaBox by the origin (see _text_family).
    """
    # 8x8 solid blue (0,0,200) uncompressed RGB image data.
    img = bytes([0, 0, 200]) * (8 * 8)
    content = (
        # Neighbour image placed correctly in its own q...Q: 120pt square at
        # (200,500)+(ox,oy) in PDF (Y-up) space.
        b"q\n120 0 0 120 %d %d cm\n/Im0 Do\nQ\n" % (200 + ox, 500 + oy)
        # Safe neighbour text.
        + b"q\nBT\n/F1 18 Tf\n0 0 0 rg\n%d %d Td\n(KEEPME) Tj\nET\nQ\n"
        % (72 + ox, 750 + oy)
        # Hostile ambient operator, left LIVE.
        + hostile
    )
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [%d %d %d %d] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Im0 6 0 R >> >> "
        b"/Contents 4 0 R >>" % (ox, oy, ox + W, oy + H),
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /XObject /Subtype /Image /Width 8 /Height 8 "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %d >>\nstream\n"
        % len(img) + img + b"\nendstream",
    ]
    return _emit(objs)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    outdir = os.path.join(here, "adversarial_ambient")
    os.makedirs(outdir, exist_ok=True)
    for op, hostile in OPS.items():
        for prefix, builder in (("aa_", _text_family),
                                ("aa_img_", _image_family)):
            name = "%s%s.pdf" % (prefix, op)
            with open(os.path.join(outdir, name), "wb") as f:
                f.write(builder(hostile))
            print("wrote", os.path.join("adversarial_ambient", name))

    # R-34: MEDIABOX-OFFSET imposition/N-up fixtures. Non-zero MediaBox origin
    # [100 100 712 892] + the half-scale "0.5 0 0 0.5 0 0 cm" ambient left live.
    mbox_hostile = OPS["cm"]
    for prefix, builder in (("aa_", _text_family), ("aa_img_", _image_family)):
        name = "%smediabox.pdf" % prefix
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(builder(mbox_hostile, ox=MBOX_OX, oy=MBOX_OY))
        print("wrote", os.path.join("adversarial_ambient", name))


if __name__ == "__main__":
    main()
