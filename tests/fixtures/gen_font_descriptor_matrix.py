# -*- coding: utf-8 -*-
"""
Generator for the /FontDescriptor style + /BaseFont untruncation matrix fixture
used by tests/test_core_font_name_util.cpp (gap A1-37, src/core/FontNameUtil).

FontNameUtil has two jobs and BOTH are name-independent by design:

  * buildFontStyleMap() must read italic/bold STRUCTURALLY from each font's
    /FontDescriptor -- /Flags bit 7 (Italic, 64), a non-zero /ItalicAngle,
    /Flags bit 19 (ForceBold, 0x40000) and a heavy /StemV -- never by
    keyword-scanning the font NAME. Every /BaseFont below is therefore named
    after the DESCRIPTOR FACT that must decide its flags, and several names
    deliberately LIE relative to a name-scan (e.g. "NotBoldStemV139" contains
    no keyword at all while "ItalicByAngle" carries /Flags 32, i.e. the italic
    bit CLEAR): a name-scanning implementation cannot pass this fixture.

  * buildBaseFontMap() must recover the untruncated /BaseFont for names past
    MuPDF's fixed char[32] fz_font.name buffer (31 usable chars), keyed both
    with and without the subset prefix, and must REFUSE to guess when two
    distinct /BaseFont values share the same 31-char prefix.

Authored ground truth (PDF 32000-1 sec. 9.8.1 Table 122 / sec. 9.7.4 for the
Type0 descendant):

  name (/BaseFont)                        Flags   ItalicAngle StemV  -> italic bold
  ItalicByFlagBit                            64             0    80      T     F
  ItalicByAngle                              32           -12    80      T     F
  BoldByForceBold                        262176             0    80      F     T
  BoldByStemV140                             32             0   140      F     T
  NotBoldStemV139                            32             0   139      F     F   (boundary)
  NoDescriptorFont                        (no /FontDescriptor)           F     F
  CidBoldItalicComposite                     64           -15   190      T     T   (Type0 -> descendant)

  SameFirstThirtyOneCharsPrefixXXAlpha  (36 chars)  |  identical first 31 chars
  SameFirstThirtyOneCharsPrefixXXBeta   (35 chars)  |  -> AMBIGUOUS, no guess
  BBBBBB+ShortPrefixedName              (24 chars, subset prefix, not truncated)
  AAAAAA+XObjectNestedLongFontNameItalic (38 chars, reachable ONLY through a
                                          Form XObject's own /Resources)

Raw byte-assembly (same style as gen_cmyk_identity.py) so the fixture is
uncompressed, directly greppable and byte-deterministic -- no font files are
embedded, since /FontDescriptor alone is what is under test and a non-embedded
simple font is perfectly legal PDF.

Regenerate with:  python tests/fixtures/gen_font_descriptor_matrix.py
"""
import io
import os

PAGE_W, PAGE_H = 300, 200

# The 31-character stem shared by the two colliding /BaseFont values. Its length
# is exactly MuPDF's usable fz_font.name capacity, so both names truncate to it.
COLLIDE_STEM = b"SameFirstThirtyOneCharsPrefixXX"
assert len(COLLIDE_STEM) == 31, len(COLLIDE_STEM)

# Drawn text is irrelevant to the resource walk, but a real content stream keeps
# the fixture a normally-openable document.
CONTENT = (
    b"q\nBT\n/F1 12 Tf\n0 0 0 rg\n"
    b"20 160 Td\n(FontDescriptor matrix) Tj\n"
    b"ET\nQ\n"
    b"q\n1 0 0 1 20 20 cm\n/Fx Do\nQ\n"
)

XOBJ_CONTENT = b"q\nBT\n/FN 10 Tf\n0 0 Td\n(nested) Tj\nET\nQ\n"


class _Objs:
    """Accumulates indirect objects and hands back their object numbers."""

    def __init__(self):
        self._body = []

    def add(self, obj):
        self._body.append(obj)
        return len(self._body)  # 1-based object number

    def add_stream(self, dict_prefix, data):
        return self.add(
            dict_prefix + b" /Length %d >>\nstream\n" % len(data)
            + data + b"endstream"
        )

    def all(self):
        return list(self._body)


def _descriptor(name, flags, italic_angle, stem_v):
    return (
        b"<< /Type /FontDescriptor /FontName /" + name
        + b" /Flags %d" % flags
        + b" /ItalicAngle %d" % italic_angle
        + b" /StemV %d" % stem_v
        + b" /Ascent 750 /Descent -250 /CapHeight 700"
        b" /FontBBox [-200 -250 1000 900] >>"
    )


def _pdf():
    o = _Objs()

    # 1/2/3/4 are reserved in that order so the catalog/pages/page/contents
    # numbering matches the sibling generators.
    catalog = o.add(b"")           # 1 - patched below
    pages = o.add(b"")             # 2 - patched below
    page = o.add(b"")              # 3 - patched below
    contents = o.add_stream(b"<<", CONTENT)  # 4

    fonts = []          # (resource name, object number)

    def simple_font(res_name, base_font, descriptor=None):
        if descriptor is None:
            num = o.add(
                b"<< /Type /Font /Subtype /Type1 /BaseFont /" + base_font + b" >>"
            )
        else:
            fd = o.add(descriptor)
            num = o.add(
                b"<< /Type /Font /Subtype /Type1 /BaseFont /" + base_font
                + b" /FontDescriptor %d 0 R >>" % fd
            )
        fonts.append((res_name, num))
        return num

    # --- descriptor-driven style matrix -----------------------------------
    simple_font(b"F1", b"ItalicByFlagBit",
                _descriptor(b"ItalicByFlagBit", 64, 0, 80))
    simple_font(b"F2", b"ItalicByAngle",
                _descriptor(b"ItalicByAngle", 32, -12, 80))
    simple_font(b"F3", b"BoldByForceBold",
                _descriptor(b"BoldByForceBold", 32 | 0x40000, 0, 80))
    simple_font(b"F4", b"BoldByStemV140",
                _descriptor(b"BoldByStemV140", 32, 0, 140))
    simple_font(b"F5", b"NotBoldStemV139",
                _descriptor(b"NotBoldStemV139", 32, 0, 139))
    simple_font(b"F6", b"NoDescriptorFont")

    # --- Type0 composite: the descriptor lives on the descendant CIDFont ---
    cid_fd = o.add(_descriptor(b"CidBoldItalicComposite", 64, -15, 190))
    cid_font = o.add(
        b"<< /Type /Font /Subtype /CIDFontType0"
        b" /BaseFont /CidBoldItalicComposite"
        b" /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity)"
        b" /Supplement 0 >>"
        b" /FontDescriptor %d 0 R /DW 1000 >>" % cid_fd
    )
    t0 = o.add(
        b"<< /Type /Font /Subtype /Type0 /BaseFont /CidBoldItalicComposite"
        b" /Encoding /Identity-H /DescendantFonts [%d 0 R] >>" % cid_font
    )
    fonts.append((b"F7", t0))

    # --- 31-char-prefix collision: two distinct names, one truncated key ---
    simple_font(b"F8", COLLIDE_STEM + b"Alpha")
    simple_font(b"F9", COLLIDE_STEM + b"Beta")

    # --- subset prefix on a name SHORT enough to survive untruncated -------
    simple_font(b"F10", b"BBBBBB+ShortPrefixedName")

    # --- font reachable only via a Form XObject's own /Resources ----------
    nested = o.add(
        b"<< /Type /Font /Subtype /Type1"
        b" /BaseFont /AAAAAA+XObjectNestedLongFontNameItalic >>"
    )
    xobj = o.add_stream(
        b"<< /Type /XObject /Subtype /Form /BBox [0 0 120 60]"
        b" /Resources << /Font << /FN %d 0 R >> >>" % nested,
        XOBJ_CONTENT,
    )

    font_res = b" ".join(
        b"/%s %d 0 R" % (name, num) for name, num in fonts
    )
    resources = (
        b"<< /Font << " + font_res + b" >>"
        b" /XObject << /Fx %d 0 R >> >>" % xobj
    )

    body = o.all()
    body[catalog - 1] = b"<< /Type /Catalog /Pages %d 0 R >>" % pages
    body[pages - 1] = (
        b"<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % page
    )
    body[page - 1] = (
        b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Resources "
        % (pages, PAGE_W, PAGE_H)
        + resources
        + b" /Contents %d 0 R >>" % contents
    )

    out = io.BytesIO()
    out.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, obj in enumerate(body, 1):
        offsets.append(out.tell())
        out.write(b"%d 0 obj\n" % i + obj + b"\nendobj\n")
    xref = out.tell()
    n = len(body) + 1
    out.write(b"xref\n0 %d\n" % n + b"0000000000 65535 f \n")
    for k in range(1, n):
        out.write(b"%010d 00000 n \n" % offsets[k])
    out.write(
        b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (n, catalog, xref)
    )
    return out.getvalue()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(here, "font_descriptor_matrix.pdf")
    data = _pdf()
    with open(out_path, "wb") as f:
        f.write(data)
    # Self-check: the two colliding names must really share a 31-char prefix,
    # and the nested name must really exceed the cap with its keyword past it.
    alpha = COLLIDE_STEM + b"Alpha"
    beta = COLLIDE_STEM + b"Beta"
    assert alpha[:31] == beta[:31] and alpha != beta
    nested = b"AAAAAA+XObjectNestedLongFontNameItalic"
    assert len(nested) > 31 and b"Italic" not in nested[:31]
    print("wrote", out_path, "(%d bytes)" % len(data))


if __name__ == "__main__":
    main()
