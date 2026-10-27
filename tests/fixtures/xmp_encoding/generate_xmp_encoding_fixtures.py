# -*- coding: utf-8 -*-
"""Generate the F36 XMP encoding fixture corpus.

Meta-audit 2026-07 follow-up, phase F36.

Emits three byte-for-byte transcodes of ONE logical XMP packet:

    xmp_pdfa2b_utf8.xmp      UTF-8,    leading U+FEFF BOM (EF BB BF)
    xmp_pdfa2b_utf16le.xmp   UTF-16LE, leading U+FEFF BOM (FF FE)
    xmp_pdfa2b_utf16be.xmp   UTF-16BE, leading U+FEFF BOM (FE FF)

They are transcodes of the SAME source string, which is what makes
`XmpEncodingTest.FixtureCorpusIsAnExactTranscodeSet` a real integrity check
rather than a restatement: if `core.autocrlf` (or anything else) rewrites one
file's bytes on checkout, the three no longer decode to the same string and
that test fails loudly. See ../../../docs/audit/meta-audit-2026-07/followup/
phase-36-dispositions.md.

Run from anywhere:  python generate_xmp_encoding_fixtures.py
It rewrites the three .xmp files in ITS OWN directory.
"""

import io
import os

# The packet body. Deliberately contains:
#   * the ISO 16684-1 xpacket header, whose `begin` attribute carries U+FEFF
#     encoded in the packet's OWN encoding (so the UTF-16 files have the BOM
#     twice: once at offset 0, once inside the header);
#   * a non-ASCII BMP character (U+00DC, U+00EF) and an ASTRAL character
#     (U+1D11E MUSICAL SYMBOL G CLEF) so a surrogate-pair transcode bug cannot
#     hide behind an all-ASCII fixture;
#   * pdfaid:part / pdfaid:conformance as CHILD ELEMENTS, which is the branch
#     XmpMetadataWriter::detectPdfALevel takes for packets it did not write.
PACKET = (
    '<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>\n'
    '<x:xmpmeta xmlns:x="adobe:ns:meta/">\n'
    '  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">\n'
    '    <rdf:Description rdf:about=""\n'
    '        xmlns:pdfaid="http://www.aiim.org/pdfa/ns/id/"\n'
    '        xmlns:dc="http://purl.org/dc/elements/1.1/"\n'
    '        xmlns:xmp="http://ns.adobe.com/xap/1.0/"\n'
    '        xmlns:pdf="http://ns.adobe.com/pdf/1.3/">\n'
    '      <pdfaid:part>2</pdfaid:part>\n'
    '      <pdfaid:conformance>B</pdfaid:conformance>\n'
    '      <dc:title>\n'
    '        <rdf:Alt>\n'
    '          <rdf:li xml:lang="x-default">'
    'Ünicode Tïtle \U0001d11e F36</rdf:li>\n'
    '        </rdf:Alt>\n'
    '      </dc:title>\n'
    '      <xmp:CreatorTool>PDFE F36 fixture generator</xmp:CreatorTool>\n'
    '      <pdf:Producer>PDFE</pdf:Producer>\n'
    '    </rdf:Description>\n'
    '  </rdf:RDF>\n'
    '</x:xmpmeta>\n'
    '<?xpacket end="w"?>'
)

# NOTE: no trailing newline, and '\n' only (never '\r\n'). The corpus is
# committed with a `binary` .gitattributes rule precisely so these bytes are
# what a fresh checkout produces.

OUTPUTS = {
    "xmp_pdfa2b_utf8.xmp": "utf-8-sig",
    "xmp_pdfa2b_utf16le.xmp": "utf-16-le",
    "xmp_pdfa2b_utf16be.xmp": "utf-16-be",
}

BOMS = {
    "xmp_pdfa2b_utf8.xmp": b"",          # utf-8-sig already writes EF BB BF
    "xmp_pdfa2b_utf16le.xmp": b"\xff\xfe",
    "xmp_pdfa2b_utf16be.xmp": b"\xfe\xff",
}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, enc in OUTPUTS.items():
        data = BOMS[name] + PACKET.encode(enc)
        path = os.path.join(here, name)
        with io.open(path, "wb") as f:
            f.write(data)
        print("wrote {} ({} bytes, {})".format(name, len(data), enc))


if __name__ == "__main__":
    main()
