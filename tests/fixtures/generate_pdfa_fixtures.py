# -*- coding: utf-8 -*-
"""Generate minimal PDF/A and PDF/UA test fixtures using pikepdf.

Creates:
  test_pdfa_1b.pdf — PDF/A-1b conforming (visual, sRGB ICC)
  test_pdfa_2b.pdf — PDF/A-2b conforming
  test_pdfa_2u.pdf — PDF/A-2u conforming (Unicode CMap)
  test_pdfa_2a.pdf — PDF/A-2a conforming (tagged, accessible)
  test_pdfa_3b.pdf — PDF/A-3b conforming (attachments allowed)
  test_pdfua.pdf  — PDF/UA-1 conforming (universal accessibility)

Each fixture starts from test_blank.pdf and adds the minimal metadata,
ICC output intent, and XMP conformance claim required for the target
level. These are NOT guaranteed to pass a strict preflight checker
like veraPDF — they are minimal claims sufficient for pdfe's
PdfAValidator/PdfUAValidator to report "conforming" on the fields
they check (XMP metadata + OutputIntent + font embedding +
StructTreeRoot for tagged PDFs).
"""
from __future__ import annotations

import sys
from pathlib import Path
from datetime import datetime

import pikepdf
from pikepdf import Name, String, Dictionary, Array, Stream

FIXTURES = Path(__file__).parent
BASE = FIXTURES / "test_blank.pdf"

LEVELS = {
    "1b": ("PDF/A-1b:2005", "1", "B"),
    "2b": ("PDF/A-2b:2011", "2", "B"),
    "2u": ("PDF/A-2u:2011", "2", "U"),
    "2a": ("PDF/A-2a:2011", "2", "A"),
    "3b": ("PDF/A-3b:2012", "3", "B"),
}

# Minimal sRGB ICC profile (2-byte stub — NOT a real ICC profile,
# but sufficient for pdfe's OutputIntent presence check. A real
# fixture-gen setup would embed the canonical sRGB profile.)
SRGB_ICC_STUB = (
    b"\x00\x00\x01\x30"  # profile size (304 bytes, minimal header)
    b"\x00\x00\x00\x00"  # preferred CMM
    b"\x02\x20\x00\x00"  # version 2.2
    b"mntr"               # device class: monitor
    b"RGB "               # color space
    b"XYZ "               # PCS
    + b"\x00" * 280       # rest of minimal header
)


# Fixed date constant — fixture generation must be idempotent
# (Pipeline J phase-4 AC). datetime.now() would change every run
# and produce different MD5 hashes.
FIXTURE_DATE = "2026-01-01T00:00:00+00:00"


def _xmp_for_level(part: str, conformance: str) -> bytes:
    return f"""<?xpacket begin="\xef\xbb\xbf" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
   xmlns:dc="http://purl.org/dc/elements/1.1/"
   xmlns:xmp="http://ns.adobe.com/xap/1.0/"
   xmlns:pdfaid="http://www.aiim.org/pdfa/ns/id/"
   xmlns:pdf="http://ns.adobe.com/pdf/1.3/">
   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Test PDF/A-{part}{conformance.lower()}</rdf:li></rdf:Alt></dc:title>
   <dc:creator><rdf:Seq><rdf:li>PDFE Test Fixtures</rdf:li></rdf:Seq></dc:creator>
   <xmp:CreateDate>{FIXTURE_DATE}</xmp:CreateDate>
   <xmp:ModifyDate>{FIXTURE_DATE}</xmp:ModifyDate>
   <pdfaid:part>{part}</pdfaid:part>
   <pdfaid:conformance>{conformance}</pdfaid:conformance>
   <pdf:Producer>pikepdf + PDFE test fixture generator</pdf:Producer>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>""".encode("utf-8")


def _xmp_for_pdfua() -> bytes:
    """XMP metadata block for PDF/UA-1 conformance."""
    return f"""<?xpacket begin="\xef\xbb\xbf" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
   xmlns:dc="http://purl.org/dc/elements/1.1/"
   xmlns:xmp="http://ns.adobe.com/xap/1.0/"
   xmlns:pdfuaid="http://www.aiim.org/pdfua/ns/id/"
   xmlns:pdf="http://ns.adobe.com/pdf/1.3/">
   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Test PDF/UA-1</rdf:li></rdf:Alt></dc:title>
   <dc:creator><rdf:Seq><rdf:li>PDFE Test Fixtures</rdf:li></rdf:Seq></dc:creator>
   <xmp:CreateDate>{FIXTURE_DATE}</xmp:CreateDate>
   <xmp:ModifyDate>{FIXTURE_DATE}</xmp:ModifyDate>
   <pdfuaid:part>1</pdfuaid:part>
   <pdf:Producer>pikepdf + PDFE test fixture generator</pdf:Producer>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>""".encode("utf-8")


def make_pdfua_fixture() -> Path:
    """Construct a minimal PDF/UA-1 conforming fixture.

    Pipeline J phase-4. Requirements per PDFE's PdfUAValidator:
      - StructTreeRoot with at least one StructElem
      - /MarkInfo /Marked true
      - ViewerPreferences /DisplayDocTitle true
      - Title set in /Info dict and dc:title in XMP
      - pdfuaid:part=1 in XMP
    """
    out = FIXTURES / "test_pdfua.pdf"
    pdf = pikepdf.open(BASE)

    # 1. XMP with pdfuaid:part=1
    xmp_data = _xmp_for_pdfua()
    xmp_stream = pdf.make_stream(xmp_data)
    xmp_stream[Name("/Type")] = Name("/Metadata")
    xmp_stream[Name("/Subtype")] = Name("/XML")
    pdf.Root[Name("/Metadata")] = xmp_stream

    # 2. Title in /Info dict (echoes XMP dc:title)
    if Name("/Info") not in pdf.trailer:
        info = pdf.make_indirect(pikepdf.Dictionary())
        pdf.trailer[Name("/Info")] = info
    pdf.trailer[Name("/Info")][Name("/Title")] = String("Test PDF/UA-1")

    # 3. ViewerPreferences with DisplayDocTitle=true (PDF/UA requires
    # that the viewer shows the document title, not the filename).
    vp = pikepdf.Dictionary()
    vp[Name("/DisplayDocTitle")] = True
    pdf.Root[Name("/ViewerPreferences")] = vp

    # 4. MarkInfo /Marked true (tagged PDF)
    mark_info = pikepdf.Dictionary()
    mark_info[Name("/Marked")] = True
    pdf.Root[Name("/MarkInfo")] = mark_info

    # 5. StructTreeRoot with a Document StructElem containing a
    # single P (Paragraph) element. Minimal but valid tag tree.
    p_elem = pikepdf.Dictionary()
    p_elem[Name("/Type")] = Name("/StructElem")
    p_elem[Name("/S")] = Name("/P")
    p_elem[Name("/Lang")] = String("en-US")

    doc_elem = pikepdf.Dictionary()
    doc_elem[Name("/Type")] = Name("/StructElem")
    doc_elem[Name("/S")] = Name("/Document")
    doc_elem[Name("/Lang")] = String("en-US")
    doc_elem[Name("/K")] = pdf.make_indirect(p_elem)

    struct_root = pikepdf.Dictionary()
    struct_root[Name("/Type")] = Name("/StructTreeRoot")
    struct_root[Name("/K")] = pdf.make_indirect(doc_elem)
    pt = pikepdf.Dictionary()
    pt[Name("/Nums")] = Array([])
    struct_root[Name("/ParentTree")] = pt
    pdf.Root[Name("/StructTreeRoot")] = pdf.make_indirect(struct_root)

    # 6. Document /Lang for screen reader pronunciation
    pdf.Root[Name("/Lang")] = String("en-US")

    pdf.save(out, deterministic_id=True)
    return out


def main() -> int:
    if not BASE.exists():
        print(f"ERROR: {BASE} not found", file=sys.stderr)
        return 1

    for level_id, (desc, part, conformance) in LEVELS.items():
        out = FIXTURES / f"test_pdfa_{level_id}.pdf"
        pdf = pikepdf.open(BASE)

        # 1. XMP metadata with pdfaid:part and pdfaid:conformance
        xmp_data = _xmp_for_level(part, conformance)
        xmp_stream = pdf.make_stream(xmp_data)
        xmp_stream[Name("/Type")] = Name("/Metadata")
        xmp_stream[Name("/Subtype")] = Name("/XML")
        pdf.Root[Name("/Metadata")] = xmp_stream

        # 2. OutputIntent with sRGB ICC profile
        icc_stream = pdf.make_stream(SRGB_ICC_STUB)
        icc_stream[Name("/N")] = 3  # RGB = 3 components
        oi = pikepdf.Dictionary()
        oi[Name("/Type")] = Name("/OutputIntent")
        oi[Name("/S")] = Name("/GTS_PDFA1")
        oi[Name("/OutputConditionIdentifier")] = String("sRGB IEC61966-2.1")
        oi[Name("/RegistryName")] = String("http://www.color.org")
        oi[Name("/Info")] = String("sRGB IEC61966-2.1")
        oi[Name("/DestOutputProfile")] = icc_stream
        pdf.Root[Name("/OutputIntents")] = Array([oi])

        # 3. Mark info for tagged PDFs (PDF/A-2a requires tagging)
        if conformance == "A":
            mark_info = pikepdf.Dictionary()
            mark_info[Name("/Marked")] = True
            pdf.Root[Name("/MarkInfo")] = mark_info
            # Add a minimal structure tree
            struct_root = pikepdf.Dictionary()
            struct_root[Name("/Type")] = Name("/StructTreeRoot")
            k_elem = pikepdf.Dictionary()
            k_elem[Name("/S")] = Name("/Document")
            k_elem[Name("/Type")] = Name("/StructElem")
            struct_root[Name("/K")] = k_elem
            pt = pikepdf.Dictionary()
            pt[Name("/Nums")] = Array([])
            struct_root[Name("/ParentTree")] = pt
            pdf.Root[Name("/StructTreeRoot")] = pdf.make_indirect(struct_root)

        pdf.save(out, deterministic_id=True)
        print(f"  {out.name}: {out.stat().st_size} bytes ({desc})")

    ua_out = make_pdfua_fixture()
    print(f"  {ua_out.name}: {ua_out.stat().st_size} bytes (PDF/UA-1)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
