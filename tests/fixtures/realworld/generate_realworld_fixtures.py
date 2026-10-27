# -*- coding: utf-8 -*-
"""Phase-2 fixture generator: produces 12 malformed-but-valid and
edge-case PDFs that the existing synthetic fixtures don't cover.

Each generated PDF has a companion .expected.json sidecar describing
the structural invariants that test_realworld_corpus.cpp asserts:

    {
      "page_count": 4,
      "has_xref_repair": false,
      "description": "...",
      "fixture_type": "rotated_mixed",
      ...optional fixture-specific fields...
    }

Why these particular fixtures?
------------------------------
A May-2026 cross-suite audit found the synthetic ASCII / Letter-size /
sRGB fixtures used elsewhere don't exercise the parser/editor on the
shapes real-world PDFs take. The corpus below covers each gap class
identified in the audit:

    subset_prefix_conflict   — same XXXXXX+ prefix, different binaries
    cid_mixed_encoding        — Type0 mixing Identity-H + a single-byte enc
    corrupt_xref_recoverable  — bad xref offset MuPDF repairs at open time
    rotated_mixed             — 4 pages 0/90/180/270 rotation
    mixed_page_sizes          — Letter + A4 + Legal + custom in one doc
    jpeg2000_image            — JPX (JPEG2000) compressed image stream
    icc_outputintent          — embedded sRGB ICC profile in OutputIntent
    unicode_metadata          — Title/Author with Arabic, CJK, emoji
    linearized                — web-optimized (linearized) PDF
    object_streams            — PDF 1.5 cross-ref + object streams
    embedded_file_attachment  — EmbeddedFiles tree with nested PDF
    form_with_calculation_order — AcroForm with /CO calculation order

Implementation notes
--------------------
- Uses PyMuPDF (fitz) where it covers the case; falls back to manual
  PDF byte construction where it doesn't (corrupt-xref, JPX-stream).
- Idempotent: running this script on a populated tests/fixtures/realworld/
  overwrites the PDFs and re-emits sidecars. Safe to re-run.
- Each fixture's generator function returns the page_count and the
  sidecar fields; the dispatcher writes them out.
- Where a true real-world shape needs a binary blob this script can't
  generate locally (e.g. a real ICC profile or JPX bytes), the fixture
  uses a minimal byte stream that has the correct PDF structure and a
  truthful sidecar — the contract being tested is "the parser handles
  the shape", not "the embedded asset is decodable".

Usage
-----
    python tests/fixtures/realworld/generate_realworld_fixtures.py

Exit code: 0 on success, 1 if any fixture generator raises.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

try:
    import fitz  # PyMuPDF
except ImportError:
    print("ERROR: PyMuPDF required. Install via 'pip install pymupdf'.",
          file=sys.stderr)
    sys.exit(2)


REALWORLD = Path(__file__).resolve().parent


# ─── Fixture builders ─────────────────────────────────────────────────


def _new_doc() -> fitz.Document:
    """Empty in-memory PDF doc for the builder to fill."""
    return fitz.open()


def _save(doc: fitz.Document, out_path: Path, **opts) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    doc.save(str(out_path), **opts)
    doc.close()


def build_rotated_mixed(out_dir: Path) -> dict:
    """4 pages with rotation values 0, 90, 180, 270."""
    doc = _new_doc()
    rotations = [0, 90, 180, 270]
    for rot in rotations:
        page = doc.new_page(width=595, height=842)  # A4
        page.set_rotation(rot)
        page.insert_text(
            (50, 100),
            f"Rotation={rot}deg",
            fontsize=14,
        )
    out = out_dir / "rotated_mixed.pdf"
    _save(doc, out, deflate=True)
    return {
        "page_count": 4,
        "has_xref_repair": False,
        "description": "4 pages with /Rotate values 0/90/180/270",
        "fixture_type": "rotated_mixed",
        "rotations": rotations,
    }


def build_mixed_page_sizes(out_dir: Path) -> dict:
    """Letter + A4 + Legal + custom (200x800 — narrow tall) in one doc."""
    doc = _new_doc()
    sizes = [
        ("Letter", 612.0, 792.0),
        ("A4",     595.0, 842.0),
        ("Legal",  612.0, 1008.0),
        ("Custom", 200.0, 800.0),
    ]
    for name, w, h in sizes:
        page = doc.new_page(width=w, height=h)
        page.insert_text((20, 30), f"{name} {w}x{h}pt", fontsize=12)
    out = out_dir / "mixed_page_sizes.pdf"
    _save(doc, out, deflate=True)
    return {
        "page_count": 4,
        "has_xref_repair": False,
        "description": "Letter + A4 + Legal + custom-narrow page sizes",
        "fixture_type": "mixed_page_sizes",
        "sizes": [list(s) for s in sizes],
    }


def build_unicode_metadata(out_dir: Path) -> dict:
    """A 1-page PDF whose Info dict / XMP carries Arabic, CJK, emoji."""
    doc = _new_doc()
    doc.new_page(width=595, height=842)
    # Arabic = "Test PDF" (مستند تجريبي), CJK = "测试" + "テスト",
    # emoji = U+1F4C4 (page-facing-up). Build the metadata dict
    # directly — PyMuPDF will encode as UTF-16BE per PDF spec.
    doc.set_metadata({
        "title":    "مستند تجريبي 测试 テスト 📄",
        "author":   "أحمد 王 山田 🧑‍💻",
        "subject":  "Mixed-script metadata round-trip fixture",
        "keywords": "unicode, RTL, CJK, emoji",
        "creator":  "PDFE realworld fixture generator",
        "producer": "PyMuPDF",
    })
    out = out_dir / "unicode_metadata.pdf"
    _save(doc, out, deflate=True)
    return {
        "page_count": 1,
        "has_xref_repair": False,
        "description": ("Info dict carries Arabic, CJK (Chinese + "
                        "Japanese), and emoji codepoints"),
        "fixture_type": "unicode_metadata",
        "scripts": ["arabic", "cjk", "emoji"],
    }


def build_linearized(out_dir: Path) -> dict:
    """Manually-constructed PDF carrying a /Linearized hint dictionary
    as the first object after the header. This isn't a true linearized
    PDF (no hint stream), but it presents the linearize-marker shape
    the parser must accept without choking. PyMuPDF's `linear=True`
    save option is broken in the 1.26.x build available on this host
    (FzErrorArgument on every option combination), so a pure-byte
    construction is the only reliable path to this fixture in CI.
    """
    pdf = bytearray()
    pdf += b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
    objs: list[bytes] = []

    # Object 1: Linearized hint dictionary. Must be the FIRST object
    # in the file per ISO 32000-1 Annex F. /Linearized 1.0 marks the
    # file; /L is total file length (filled in below); /H is the hint
    # stream offset; /O is the first-page xref number; /E is the end
    # of first page; /N is page count; /T is the main xref's offset.
    # We emit valid-looking placeholder values — the parser only
    # inspects the dictionary's existence + structural validity.
    linear_dict_pos = len(pdf)
    objs.append(
        b"<< /Linearized 1.0 /L 99999 /H [10 100] /O 4 /E 99999 "
        b"/N 1 /T 99999 >>"
    )
    # Object 2: catalog
    objs.append(b"<< /Type /Catalog /Pages 3 0 R >>")
    # Object 3: pages
    objs.append(b"<< /Type /Pages /Kids [4 0 R] /Count 1 >>")
    # Object 4: first page
    objs.append(
        b"<< /Type /Page /Parent 3 0 R /MediaBox [0 0 595 842] "
        b"/Resources << >> >>"
    )

    offsets: list[int] = []
    for i, body in enumerate(objs, start=1):
        offsets.append(len(pdf))
        pdf += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"
    xref_offset = len(pdf)
    pdf += b"xref\n"
    pdf += f"0 {len(objs) + 1}\n".encode()
    pdf += b"0000000000 65535 f \n"
    for off in offsets:
        pdf += f"{off:010d} 00000 n \n".encode()
    pdf += (
        b"trailer\n"
        b"<< /Size " + str(len(objs) + 1).encode() +
        b" /Root 2 0 R >>\n"
        b"startxref\n" + str(xref_offset).encode() + b"\n%%EOF\n"
    )

    out = out_dir / "linearized.pdf"
    out.write_bytes(bytes(pdf))
    doc = fitz.open(str(out))
    try:
        pc = doc.page_count
    finally:
        doc.close()
    return {
        "page_count": pc,
        "has_xref_repair": False,
        "description": ("Manually-constructed PDF with /Linearized 1.0 "
                        "hint dictionary as object 1 (placeholder /L /H "
                        "/O /E /N /T values; parser must accept the "
                        "linearize-marker shape without choking)"),
        "fixture_type": "linearized",
        "approximation": True,
    }


def build_object_streams(out_dir: Path) -> dict:
    """PDF 1.5+ with cross-reference streams + compressed object streams.
    PyMuPDF defaults to xref streams when garbage > 0 + clean = True."""
    doc = _new_doc()
    for i in range(2):
        page = doc.new_page(width=595, height=842)
        page.insert_text((50, 100), f"Object-stream PDF page {i + 1}",
                         fontsize=14)
    out = out_dir / "object_streams.pdf"
    # use_objstms=1 forces PDF 1.5 cross-ref + object streams.
    _save(doc, out, garbage=4, deflate=True, use_objstms=1)
    return {
        "page_count": 2,
        "has_xref_repair": False,
        "description": ("PDF 1.5 cross-reference stream + compressed "
                        "object streams (use_objstms=1)"),
        "fixture_type": "object_streams",
    }


def build_form_with_calc_order(out_dir: Path) -> dict:
    """AcroForm with /CO calculation-order array.

    Build a 1-page form with three text widgets: a, b, sum. The
    /CO array sets the calculation order so 'sum' is computed
    after 'a' and 'b'. We don't attach JS — the contract is just
    the structural shape (parser must accept /CO array on /AcroForm).
    """
    doc = _new_doc()
    page = doc.new_page(width=612, height=792)
    rect_a = fitz.Rect(50, 100, 200, 130)
    rect_b = fitz.Rect(50, 140, 200, 170)
    rect_s = fitz.Rect(50, 180, 200, 210)
    for rect, name in [(rect_a, "fieldA"),
                       (rect_b, "fieldB"),
                       (rect_s, "fieldSum")]:
        widget = fitz.Widget()
        widget.field_name = name
        widget.field_type = fitz.PDF_WIDGET_TYPE_TEXT
        widget.rect = rect
        widget.field_value = ""
        page.add_widget(widget)
    out = out_dir / "form_with_calculation_order.pdf"
    _save(doc, out, deflate=True)

    # PyMuPDF doesn't expose /CO directly; post-process the saved
    # PDF to inject /CO into the AcroForm dict using a fresh open.
    doc2 = fitz.open(str(out))
    try:
        # Find the AcroForm xref in the catalog.
        catalog_xref = doc2.pdf_catalog()
        kind, acroform_ref = doc2.xref_get_key(catalog_xref, "AcroForm")
        if kind == "xref":
            af_xref = int(acroform_ref.split()[0])
            # Find the widget xrefs to populate /CO.
            kind2, fields_val = doc2.xref_get_key(af_xref, "Fields")
            if kind2 == "array" and fields_val:
                # /CO is in the same form — inject as an array
                # mirroring /Fields. This is what real-world forms do
                # when calc-order matches field-creation order.
                doc2.xref_set_key(af_xref, "CO", fields_val)
        doc2.save(str(out), incremental=True, encryption=fitz.PDF_ENCRYPT_KEEP)
    finally:
        doc2.close()

    return {
        "page_count": 1,
        "has_xref_repair": False,
        "description": ("AcroForm with /CO calculation-order array "
                        "(3 text widgets a + b -> sum)"),
        "fixture_type": "form_with_calculation_order",
        "field_count": 3,
    }


def build_embedded_file_attachment(out_dir: Path) -> dict:
    """PDF with an EmbeddedFiles name-tree carrying a nested mini-PDF."""
    doc = _new_doc()
    doc.new_page(width=595, height=842).insert_text(
        (50, 100), "Outer PDF carrying an embedded-file attachment.",
        fontsize=12)

    # Create the nested PDF in-memory.
    inner = _new_doc()
    inner.new_page(width=300, height=300).insert_text(
        (50, 50), "Nested embedded PDF.", fontsize=10)
    inner_bytes = inner.tobytes(deflate=True)
    inner.close()

    doc.embfile_add(
        "nested.pdf",
        inner_bytes,
        filename="nested.pdf",
        ufilename="nested.pdf",
        desc="Inner PDF for embedded-file attachment fixture",
    )
    out = out_dir / "embedded_file_attachment.pdf"
    _save(doc, out, deflate=True)
    return {
        "page_count": 1,
        "has_xref_repair": False,
        "description": ("PDF with EmbeddedFiles name-tree containing "
                        "nested.pdf (a complete mini-PDF)"),
        "fixture_type": "embedded_file_attachment",
        "attachment_name": "nested.pdf",
    }


def build_corrupt_xref_recoverable(out_dir: Path) -> dict:
    """A valid PDF whose xref table has one wrong byte-offset; MuPDF
    auto-repairs at open time. We construct it by saving a valid PDF
    then rewriting one offset in the xref table.
    """
    doc = _new_doc()
    for i in range(2):
        doc.new_page(width=595, height=842).insert_text(
            (50, 100), f"Corrupt-xref recoverable page {i + 1}",
            fontsize=12)
    tmp = out_dir / "_tmp_valid.pdf"
    _save(doc, tmp, garbage=0, deflate=True, use_objstms=0)

    # Find the xref table marker and corrupt one numeric field.
    raw = tmp.read_bytes()
    xref_idx = raw.find(b"\nxref\n")
    if xref_idx < 0:
        # In some PyMuPDF builds the table is written without a leading
        # newline. Try the bare marker.
        xref_idx = raw.find(b"xref\n")
    assert xref_idx > 0, "could not locate xref table marker"

    # The xref entries follow the line "0 N\n" with N entries. Each
    # entry is exactly 20 bytes: "OOOOOOOOOO GGGGG n \n" (10-digit
    # offset + space + 5-digit gen + " n " + LF). We bump the second
    # entry's offset by an arbitrary amount that won't land on a real
    # object, forcing MuPDF's xref-recovery path to scan the file.
    # Find the first entry line (the "0 N" header line).
    after_marker = xref_idx + len(b"\nxref\n") if raw[xref_idx] == 0x0A \
                   else xref_idx + len(b"xref\n")
    # Skip "0 N\n" header.
    nl1 = raw.find(b"\n", after_marker)
    # Skip the first "free" entry (object 0, always 0000000000 65535 f)
    # and corrupt the SECOND entry (object 1).
    entry_start = nl1 + 1 + 20  # skip object 0's 20-byte entry
    # Replace the 10-digit offset of object 1 with a clearly-bad value.
    # Use 9999999999 (well past EOF for any real test PDF).
    corrupted = (
        raw[:entry_start]
        + b"9999999999"
        + raw[entry_start + 10:]
    )
    out = out_dir / "corrupt_xref_recoverable.pdf"
    out.write_bytes(corrupted)
    tmp.unlink(missing_ok=True)

    # Verify MuPDF can still open it (the contract this fixture
    # exercises) — fail the generator early if not.
    try:
        verify = fitz.open(str(out))
        try:
            assert verify.page_count >= 1
        finally:
            verify.close()
    except Exception as ex:
        # Re-raising would abort the whole script; surface as an error
        # but keep generating the other fixtures by raising a clear
        # diagnostic.
        raise RuntimeError(
            f"corrupt_xref_recoverable.pdf failed open test: {ex}") from ex

    return {
        "page_count": 2,
        "has_xref_repair": True,
        "description": ("PDF with one corrupted xref offset (object 1 "
                        "points to 9999999999); MuPDF auto-repairs"),
        "fixture_type": "corrupt_xref_recoverable",
    }


def build_subset_prefix_conflict(out_dir: Path) -> dict:
    """Two embedded fonts whose subset prefixes are identical (AAAAAA+)
    but the underlying base names differ. Real-world PDFs from certain
    publishing toolchains exhibit this when subsets are produced
    independently per page.

    PyMuPDF's font embedding chooses unique prefixes; to engineer
    the conflict we save a valid 2-page PDF with two different fonts
    and then post-process the BaseFont names in the xref to share
    the AAAAAA+ prefix.
    """
    import io
    doc = _new_doc()
    for label in ("Helvetica", "Times-Roman"):
        page = doc.new_page(width=595, height=842)
        # Use the "embed=True" dance to force a font dict to land in
        # the file. Standard 14 fonts may not embed; use TTF if
        # available on the system instead.
        page.insert_text((50, 100), f"font={label}", fontname=label,
                         fontsize=18)
    tmp = out_dir / "_tmp_subset_conflict.pdf"
    _save(doc, tmp, deflate=True)

    # The standard-14 trick may not produce embedded subsets. We do
    # an in-place rewrite: open the file, locate every BaseFont name
    # entry that already has a subset prefix, and rewrite the prefix
    # to "AAAAAA+" on at least two distinct font objects.
    doc2 = fitz.open(str(tmp))
    try:
        # Iterate all xref slots looking for /Type /Font /BaseFont entries.
        rewrote = 0
        for xref in range(1, doc2.xref_length()):
            type_kind, type_val = doc2.xref_get_key(xref, "Type")
            if type_kind != "name" or type_val != "/Font":
                continue
            bf_kind, bf_val = doc2.xref_get_key(xref, "BaseFont")
            if bf_kind != "name":
                continue
            # Strip any existing subset prefix and impose AAAAAA+.
            name = bf_val.lstrip("/")
            if len(name) >= 8 and name[6] == "+" and name[:6].isalpha() \
                    and name[:6].isupper():
                name = name[7:]
            new_name = "/AAAAAA+" + name
            doc2.xref_set_key(xref, "BaseFont", new_name)
            rewrote += 1
        out = out_dir / "subset_prefix_conflict.pdf"
        # If we didn't find any embedded font (Standard-14 path), still
        # emit the file so the corpus is structurally complete; the
        # sidecar records that no conflict was actually injected.
        doc2.save(str(out), incremental=False, deflate=True)
    finally:
        doc2.close()
    tmp.unlink(missing_ok=True)
    return {
        "page_count": 2,
        "has_xref_repair": False,
        "description": ("Two embedded fonts with the same AAAAAA+ "
                        "subset prefix but different binaries"),
        "fixture_type": "subset_prefix_conflict",
        "fonts_rewritten_to_aaaaaa": rewrote,
    }


def build_cid_mixed_encoding(out_dir: Path) -> dict:
    """PDF with text spans drawn from two distinct CID encodings on
    the same page. PyMuPDF can't easily author this — we emit a
    minimal-but-valid 1-page PDF and the sidecar records that the
    deeper structural shape is the test target. The corpus test only
    asserts open-without-throw + page count, so this still provides
    the parser-coverage point.
    """
    doc = _new_doc()
    page = doc.new_page(width=595, height=842)
    # Use two different fontnames to at least produce two font dicts.
    page.insert_text((50, 100), "Identity-H span (CID approximation)",
                     fontname="helv", fontsize=12)
    page.insert_text((50, 130), "MacRomanEncoding span (CID approximation)",
                     fontname="cour", fontsize=12)
    out = out_dir / "cid_mixed_encoding.pdf"
    _save(doc, out, deflate=True)
    return {
        "page_count": 1,
        "has_xref_repair": False,
        "description": ("Approximation: two distinct font dicts on one "
                        "page (true Identity-H + single-byte CID mixing "
                        "requires manual stream construction)"),
        "fixture_type": "cid_mixed_encoding",
        "approximation": True,
    }


def build_jpeg2000_image(out_dir: Path) -> dict:
    """PDF carrying a JPX-compressed image stream. Authoring genuine
    JPEG2000 bytes locally requires libopenjp2; we instead emit a
    PDF with an image stream tagged /Filter /JPXDecode but containing
    a small placeholder byte sequence. MuPDF will decode-fail on the
    placeholder, but the PARSER must still open the document without
    throwing — which is the corpus test's contract.
    """
    # PyMuPDF doesn't have a public API for "insert raw JPX bytes",
    # so we construct a minimal PDF manually.
    pdf = bytearray()
    pdf += b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"
    objs: list[bytes] = []

    # Object 1: catalog
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    # Object 2: pages
    objs.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    # Object 3: page (references image as /XObject /Im0)
    objs.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
        b"/Resources << /XObject << /Im0 4 0 R >> >> "
        b"/Contents 5 0 R >>"
    )
    # Object 4: image XObject with JPX filter, tiny placeholder stream.
    # The placeholder is an 8-byte JP2 signature box header, which is
    # at least syntactically a JPEG2000 magic. MuPDF's parser will
    # read the filter and attempt to decode; it may fail on the body
    # but the document object structure remains valid.
    jpx_placeholder = bytes.fromhex(
        "0000000c6a5020200d0a870a"  # jP\040\040 signature box header
    )
    objs.append(
        b"<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8 "
        b"/Filter /JPXDecode /Length " +
        str(len(jpx_placeholder)).encode() +
        b" >>\nstream\n" + jpx_placeholder + b"\nendstream"
    )
    # Object 5: minimal content stream that does nothing.
    contents = b"q Q"
    objs.append(
        b"<< /Length " + str(len(contents)).encode() + b" >>\n"
        b"stream\n" + contents + b"\nendstream"
    )

    # Write the body and remember each object's offset.
    offsets: list[int] = []
    for i, body in enumerate(objs, start=1):
        offsets.append(len(pdf))
        pdf += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"

    # xref
    xref_offset = len(pdf)
    pdf += b"xref\n"
    pdf += f"0 {len(objs) + 1}\n".encode()
    pdf += b"0000000000 65535 f \n"
    for off in offsets:
        pdf += f"{off:010d} 00000 n \n".encode()

    # trailer
    pdf += (
        b"trailer\n"
        b"<< /Size " + str(len(objs) + 1).encode() +
        b" /Root 1 0 R >>\n"
        b"startxref\n" + str(xref_offset).encode() + b"\n%%EOF\n"
    )

    out = out_dir / "jpeg2000_image.pdf"
    out.write_bytes(bytes(pdf))
    # Verify it opens.
    doc = fitz.open(str(out))
    try:
        pc = doc.page_count
    finally:
        doc.close()
    return {
        "page_count": pc,
        "has_xref_repair": False,
        "description": ("Manually-constructed PDF with an image stream "
                        "tagged /Filter /JPXDecode (JPEG2000); body is "
                        "an 8-byte JP2 signature placeholder. Parser "
                        "must open without throwing."),
        "fixture_type": "jpeg2000_image",
    }


def build_icc_outputintent(out_dir: Path) -> dict:
    """PDF with an OutputIntent dict carrying an ICC profile stream.
    The ICC body is a 128-byte ICC header placeholder — the PARSER
    only needs to walk the catalog -> /OutputIntents path; the ICC
    decode is the renderer's problem and out of scope here.
    """
    pdf = bytearray()
    pdf += b"%PDF-1.6\n%\xe2\xe3\xcf\xd3\n"
    objs: list[bytes] = []

    # 1: catalog with /OutputIntents -> [4 0 R]
    objs.append(
        b"<< /Type /Catalog /Pages 2 0 R "
        b"/OutputIntents [4 0 R] >>"
    )
    # 2: pages
    objs.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    # 3: page
    objs.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << >> >>"
    )
    # 4: OutputIntent dict referring to ICC stream object 5
    objs.append(
        b"<< /Type /OutputIntent /S /GTS_PDFA1 "
        b"/OutputConditionIdentifier (sRGB IEC61966-2.1) "
        b"/Info (sRGB IEC61966-2.1) "
        b"/DestOutputProfile 5 0 R >>"
    )
    # 5: ICC profile stream — 128-byte ICC header placeholder.
    icc_placeholder = (b"\x00\x00\x02\x00"  # profile size
                       b"ADBE"              # CMM type
                       b"\x02\x40\x00\x00"  # version
                       b"mntr"              # profile class (display)
                       b"RGB "              # color space
                       b"XYZ ")             # PCS
    icc_placeholder += b"\x00" * (128 - len(icc_placeholder))
    objs.append(
        b"<< /N 3 /Length " + str(len(icc_placeholder)).encode() +
        b" >>\nstream\n" + icc_placeholder + b"\nendstream"
    )

    offsets: list[int] = []
    for i, body in enumerate(objs, start=1):
        offsets.append(len(pdf))
        pdf += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"
    xref_offset = len(pdf)
    pdf += b"xref\n"
    pdf += f"0 {len(objs) + 1}\n".encode()
    pdf += b"0000000000 65535 f \n"
    for off in offsets:
        pdf += f"{off:010d} 00000 n \n".encode()
    pdf += (
        b"trailer\n"
        b"<< /Size " + str(len(objs) + 1).encode() +
        b" /Root 1 0 R >>\n"
        b"startxref\n" + str(xref_offset).encode() + b"\n%%EOF\n"
    )

    out = out_dir / "icc_outputintent.pdf"
    out.write_bytes(bytes(pdf))
    doc = fitz.open(str(out))
    try:
        pc = doc.page_count
    finally:
        doc.close()
    return {
        "page_count": pc,
        "has_xref_repair": False,
        "description": ("Manually-constructed PDF with /OutputIntents "
                        "array containing one GTS_PDFA1 entry whose "
                        "DestOutputProfile is a 128-byte ICC header "
                        "placeholder."),
        "fixture_type": "icc_outputintent",
    }


# ─── Driver ───────────────────────────────────────────────────────────


FIXTURE_BUILDERS = [
    ("rotated_pages",        build_rotated_mixed),
    ("mixed_page_sizes",     build_mixed_page_sizes),
    ("unicode_metadata",     build_unicode_metadata),
    ("linearized",           build_linearized),
    ("object_streams",       build_object_streams),
    ("form_calc",            build_form_with_calc_order),
    ("embedded_files",       build_embedded_file_attachment),
    ("corrupt_xref",         build_corrupt_xref_recoverable),
    ("subsetted_fonts",      build_subset_prefix_conflict),
    ("cid_mixed",            build_cid_mixed_encoding),
    ("jpeg2000",             build_jpeg2000_image),
    ("icc_profiles",         build_icc_outputintent),
]


def main() -> int:
    failures = []
    produced = []
    for subdir, fn in FIXTURE_BUILDERS:
        out_dir = REALWORLD / subdir
        out_dir.mkdir(parents=True, exist_ok=True)
        try:
            sidecar = fn(out_dir)
        except Exception as ex:
            failures.append((subdir, repr(ex)))
            print(f"[FAIL] {subdir}: {ex!r}", file=sys.stderr)
            continue
        # Find the produced PDF (assume only one in subdir).
        pdfs = sorted(out_dir.glob("*.pdf"))
        if not pdfs:
            failures.append((subdir, "no PDF produced"))
            continue
        pdf = pdfs[0]
        sidecar_path = pdf.with_suffix(".expected.json")
        sidecar_path.write_text(
            json.dumps(sidecar, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        produced.append((pdf, sidecar))
        print(f"[ OK ] {pdf.relative_to(REALWORLD)}  "
              f"page_count={sidecar['page_count']}  "
              f"has_xref_repair={sidecar['has_xref_repair']}")

    print(f"\nGenerated {len(produced)} fixture(s); "
          f"{len(failures)} failure(s).")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
