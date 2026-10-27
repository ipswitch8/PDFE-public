# -*- coding: utf-8 -*-
"""
Phase-3 fixture generator (Pipeline a7c2e541).

Produces fixtures that fill coverage-matrix gaps NOT covered by the
existing 71 PDFs in tests/fixtures/ (root + realworld/ + image_pdf/).

Gap coverage targets:
  - PDF version 1.4 stamp (v14_basic.pdf)
  - PDF version 2.0 stamp (v20_minimal.pdf)
  - AES-128 encryption (encrypted_aes128.pdf)
  - All-field-types AcroForm (form_all_field_types.pdf)
  - Multi-type annotations (annotations_all_types.pdf)
  - CJK text body (cjk_text.pdf)
  - RTL text body / Arabic (rtl_arabic.pdf)
  - UTF-16 string in metadata (utf16_metadata.pdf)
  - Latin-extended / Cyrillic / Greek text (cyrillic_greek.pdf)
  - Large fixture > 5 MB but < 50 MB (large_100page_image.pdf)
  - 1-page minimal (covered already; skip)

All generators are deterministic — fixed metadata dates, fixed RNG
seed, no time.time() — so re-running produces byte-identical output
(or near-identical, modulo PyMuPDF library version).

Each PDF gets a corresponding <name>.expected.json sidecar.

Run:
    python generate_phase3_fixtures.py
"""
from __future__ import annotations

import io
import json
import os
import random
import struct
import sys
import zlib
from pathlib import Path

try:
    import fitz  # PyMuPDF
except ImportError:
    print("ERROR: PyMuPDF required. Install via 'pip install pymupdf'.",
          file=sys.stderr)
    sys.exit(2)


# Deterministic context --------------------------------------------------------
# Fix the RNG seed and use the same fake CreationDate / ModDate for everything.
random.seed(0xA7C2E541)
FIXED_DATE = "D:20260101000000Z"  # frozen for determinism
FIXED_AUTHOR = "PDFE phase-3 generator"
FIXED_PRODUCER = "PDFE phase-3 (PyMuPDF)"


THIS_DIR = Path(__file__).resolve().parent
OUT_DIR = THIS_DIR  # phase-3 fixtures live in tests/fixtures/ root


def _set_metadata(doc: fitz.Document, title: str, subject: str = "phase-3 fixture") -> None:
    doc.set_metadata({
        "title": title,
        "author": FIXED_AUTHOR,
        "subject": subject,
        "keywords": "phase3,coverage,fixture",
        "creator": FIXED_PRODUCER,
        "producer": FIXED_PRODUCER,
        "creationDate": FIXED_DATE,
        "modDate": FIXED_DATE,
    })


def _save(doc: fitz.Document, path: Path, **kw) -> None:
    """Save with deterministic options (no incremental, no garbage).

    PyMuPDF 1.26 does not accept a `version=` kwarg on save(). To
    emit a specific PDF header version we save with default settings
    and patch the leading %PDF-X.Y bytes (see _patch_header)."""
    # Strip any 'version=' caller passed in (we patch post-save instead).
    kw.pop("version", None)
    # no_new_id=1 suppresses regeneration of the trailer /ID — without it
    # every save() emits a fresh random ID array which breaks byte-identical
    # determinism on re-run.
    defaults = dict(garbage=0, deflate=True, clean=False, linear=False, no_new_id=1)
    defaults.update(kw)
    doc.save(str(path), **defaults)


def _patch_header(path: Path, version: str) -> None:
    """Rewrite the %PDF-X.Y header in-place. Used to mark fixtures as
    1.4 or 2.0 even though PyMuPDF wrote them with a different default."""
    blob = path.read_bytes()
    if not blob.startswith(b"%PDF-"):
        return
    # Find the original "X.Y" — always 3 ASCII chars after %PDF-
    new_header = b"%PDF-" + version.encode("ascii")
    blob = new_header + blob[len(new_header):]
    path.write_bytes(blob)


def _write_sidecar(pdf_path: Path, **fields) -> None:
    """Write <pdf_path>.expected.json with the standard schema fields."""
    sidecar = pdf_path.with_suffix(".expected.json")
    # Inspect the file we just wrote for the canonical attributes
    sz = pdf_path.stat().st_size
    base = {
        "pageCount": fields.get("pageCount"),
        "isEncrypted": fields.get("isEncrypted", False),
        "hasForm": fields.get("hasForm", False),
        "hasForms": fields.get("hasForm", False),
        "hasAnnotations": fields.get("hasAnnotations", False),
        "hasImages": fields.get("hasImages", False),
        "pdfVersion": fields.get("pdfVersion"),
        "fileSize": sz,
        "origin": "phase3-generator",
        "category_tags": fields.get("category_tags", []),
        "description": fields.get("description", ""),
    }
    # Optional extras
    for opt in ("encryptionAlgo", "annotationTypes", "fieldTypes", "scripts",
                "approximation", "contentKind"):
        if opt in fields:
            base[opt] = fields[opt]
    with open(sidecar, "w", encoding="utf-8") as f:
        json.dump(base, f, indent=2, ensure_ascii=False)
        f.write("\n")


# Generators -------------------------------------------------------------------
def gen_pdf14_basic() -> Path:
    """PDF 1.4 with explicit version stamp, plain text body."""
    doc = fitz.open()
    page = doc.new_page()
    page.insert_text((72, 90), "PDF 1.4 fixture", fontsize=14, fontname="helv")
    page.insert_text((72, 110), "Version stamp test for parser fallback paths.",
                     fontsize=10, fontname="helv")
    _set_metadata(doc, "PDF 1.4 basic")
    out = OUT_DIR / "phase3_v14_basic.pdf"
    _save(doc, out)
    doc.close()
    _patch_header(out, "1.4")
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.4",
        category_tags=["pdf-1.4", "vector-text", "ascii-only", "no-encryption"],
        scripts=["ascii"], contentKind="vector-text",
        description="Minimal PDF 1.4 doc with ASCII body for version-fallback tests."
    )
    return out


def gen_pdf20_minimal() -> Path:
    """Minimal PDF claiming version 2.0 in header. PyMuPDF cannot
    write 2.0 directly so we patch the header byte after save.
    Sidecar marks approximation: true."""
    doc = fitz.open()
    page = doc.new_page()
    page.insert_text((72, 90), "PDF 2.0 fixture (header-patched)", fontsize=12)
    _set_metadata(doc, "PDF 2.0 minimal")
    out = OUT_DIR / "phase3_v20_minimal.pdf"
    _save(doc, out)
    doc.close()
    _patch_header(out, "2.0")
    _write_sidecar(out,
        pageCount=1, pdfVersion="2.0",
        category_tags=["pdf-2.0", "vector-text", "ascii-only", "no-encryption"],
        scripts=["ascii"], contentKind="vector-text",
        approximation=True,
        description=("Header claims PDF 2.0; body is PDF 1.7-compliant. "
                     "Approximation: true — exercises version-string parsing only.")
    )
    return out


def gen_aes128_encrypted() -> Path:
    """AES-128 (V=4 R=4) encrypted single-page PDF. User password 'user'."""
    doc = fitz.open()
    page = doc.new_page()
    page.insert_text((72, 90), "AES-128 encrypted body", fontsize=12)
    _set_metadata(doc, "AES-128 encrypted fixture")
    out = OUT_DIR / "phase3_encrypted_aes128.pdf"
    # NOTE: AES encryption regenerates a fresh random 16-byte IV on every
    # save (cryptographic requirement) so the file bytes will differ between
    # runs even with no_new_id=1. The structural attributes (V/R/keyLength,
    # page count, content) are deterministic; only the ciphertext drifts.
    _save(doc, out,
        encryption=fitz.PDF_ENCRYPT_AES_128,
        owner_pw="owner-pw-phase3",
        user_pw="user",
        permissions=int(fitz.PDF_PERM_PRINT | fitz.PDF_PERM_COPY),
    )
    doc.close()
    _patch_header(out, "1.6")
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.6", isEncrypted=True,
        category_tags=["aes-128", "encrypted", "ascii-only", "vector-text"],
        encryptionAlgo="AES-128", scripts=["ascii"], contentKind="vector-text",
        description="AES-128 (V=4 R=4) encryption. user pw='user', owner pw='owner-pw-phase3'."
    )
    return out


def gen_form_all_field_types() -> Path:
    """AcroForm with one of each major field type via PyMuPDF widgets."""
    doc = fitz.open()
    page = doc.new_page(width=595, height=842)
    page.insert_text((72, 60), "All field types form", fontsize=14)

    # Text field
    rc = fitz.Rect(72, 90, 300, 110)
    w = fitz.Widget()
    w.field_name = "txt_name"
    w.field_type = fitz.PDF_WIDGET_TYPE_TEXT
    w.rect = rc
    w.field_value = ""
    page.add_widget(w)

    # Checkbox
    w = fitz.Widget()
    w.field_name = "chk_subscribe"
    w.field_type = fitz.PDF_WIDGET_TYPE_CHECKBOX
    w.rect = fitz.Rect(72, 130, 90, 148)
    w.field_value = False
    page.add_widget(w)

    # ComboBox (a.k.a. dropdown)
    w = fitz.Widget()
    w.field_name = "cbo_country"
    w.field_type = fitz.PDF_WIDGET_TYPE_COMBOBOX
    w.rect = fitz.Rect(72, 170, 300, 190)
    w.choice_values = ["USA", "UK", "Japan"]
    w.field_value = "USA"
    page.add_widget(w)

    # ListBox
    w = fitz.Widget()
    w.field_name = "lst_colors"
    w.field_type = fitz.PDF_WIDGET_TYPE_LISTBOX
    w.rect = fitz.Rect(72, 210, 300, 270)
    w.choice_values = ["red", "green", "blue", "yellow"]
    w.field_value = "green"
    page.add_widget(w)

    # Multi-line text
    w = fitz.Widget()
    w.field_name = "txt_comments"
    w.field_type = fitz.PDF_WIDGET_TYPE_TEXT
    w.text_maxlen = 400
    w.rect = fitz.Rect(72, 290, 500, 360)
    w.field_value = ""
    page.add_widget(w)

    # Signature placeholder
    w = fitz.Widget()
    w.field_name = "sig_signoff"
    w.field_type = fitz.PDF_WIDGET_TYPE_SIGNATURE
    w.rect = fitz.Rect(72, 380, 300, 430)
    page.add_widget(w)

    _set_metadata(doc, "All field types form")
    out = OUT_DIR / "phase3_form_all_field_types.pdf"
    _save(doc, out)
    doc.close()
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.7", hasForm=True,
        category_tags=["acroform", "all-field-types", "vector-text"],
        fieldTypes=["text", "checkbox", "combobox", "listbox", "text-multiline", "signature"],
        scripts=["ascii"], contentKind="vector-text",
        description="AcroForm with text, checkbox, combobox, listbox, multiline, and signature fields."
    )
    return out


def gen_annotations_all_types() -> Path:
    """One PDF with one of each major annotation subtype."""
    doc = fitz.open()
    page = doc.new_page(width=595, height=842)
    page.insert_text((72, 60), "Annotations sampler page", fontsize=14)
    page.insert_text((72, 90),
                     "highlight me, strike me, underline me, squiggle me",
                     fontsize=11, fontname="helv")

    # Sticky-note (Text annotation)
    page.add_text_annot(fitz.Point(420, 90), "Sticky note text",
                        icon="Note")

    # Highlight on the line
    rect = fitz.Rect(72, 84, 300, 100)
    page.add_highlight_annot(rect)

    # Strike-out
    page.add_strikeout_annot(fitz.Rect(72, 102, 200, 116))
    # Underline
    page.add_underline_annot(fitz.Rect(202, 102, 290, 116))
    # Squiggly
    page.add_squiggly_annot(fitz.Rect(72, 118, 290, 132))

    # Ink (free-hand) — list of strokes, each stroke a list of (x,y) tuples
    points = [
        [(72.0, 200.0), (80.0, 210.0), (120.0, 220.0), (160.0, 200.0)]
    ]
    page.add_ink_annot(points)

    # Circle / Square / Line
    page.add_circle_annot(fitz.Rect(72, 250, 152, 320))
    page.add_rect_annot(fitz.Rect(180, 250, 260, 320))
    page.add_line_annot(fitz.Point(290, 260), fitz.Point(420, 320))

    # FreeText
    fr = fitz.Rect(72, 360, 300, 400)
    page.add_freetext_annot(fr, "FreeText annotation",
                            text_color=(0, 0, 0), fill_color=(1, 1, 0.6))

    # Stamp
    page.add_stamp_annot(fitz.Rect(330, 360, 480, 410), stamp=0)  # 0 = Approved

    _set_metadata(doc, "All annotation types")
    out = OUT_DIR / "phase3_annotations_all_types.pdf"
    _save(doc, out)
    doc.close()
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.7", hasAnnotations=True,
        category_tags=["annotations", "all-major-types", "vector-text"],
        annotationTypes=["Text", "Highlight", "StrikeOut", "Underline", "Squiggly",
                         "Ink", "Circle", "Square", "Line", "FreeText", "Stamp"],
        scripts=["ascii"], contentKind="vector-text",
        description="One annotation of each major subtype on a single page."
    )
    return out


def gen_cjk_text() -> Path:
    """CJK (Japanese + Chinese) text body using a built-in CJK font."""
    doc = fitz.open()
    page = doc.new_page()
    # PyMuPDF's "japan" / "china-s" CJK font (Adobe CJK base) avoids needing a
    # local TrueType file. font="japan" maps to KozMinPro which can render CJK.
    page.insert_text((72, 90), "CJK fixture (mixed scripts)", fontsize=12)
    page.insert_text((72, 120), "日本語: こんにちは世界", fontsize=14, fontname="japan")
    page.insert_text((72, 150), "中文: 你好世界", fontsize=14, fontname="china-s")
    page.insert_text((72, 180), "한국어: 안녕 세상", fontsize=14, fontname="korea")
    _set_metadata(doc, "CJK text fixture")
    out = OUT_DIR / "phase3_cjk_text.pdf"
    _save(doc, out)
    doc.close()
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.7",
        category_tags=["cjk", "japanese", "chinese", "korean", "vector-text"],
        scripts=["cjk", "ascii"], contentKind="vector-text",
        description="Body text in Japanese (Hiragana+Kanji), Simplified Chinese, and Korean (Hangul)."
    )
    return out


def gen_rtl_arabic() -> Path:
    """Right-to-left text body in Arabic + Hebrew via insert_htmlbox,
    which uses a CSS-styled font fallback chain (covers Arabic/Hebrew)."""
    doc = fitz.open()
    page = doc.new_page()
    page.insert_text((72, 60), "RTL fixture (Arabic + Hebrew)", fontsize=12)
    arabic = "مرحبا بالعالم"   # "hello world" in Arabic
    hebrew = "שלום עולם"        # "hello world" in Hebrew
    rect = fitz.Rect(72, 90, 522, 250)
    page.insert_htmlbox(
        rect,
        f'<p>Arabic: {arabic}</p><p>Hebrew: {hebrew}</p>'
    )
    _set_metadata(doc, f"RTL fixture: {arabic} / {hebrew}",
                  subject=f"{arabic} / {hebrew}")
    out = OUT_DIR / "phase3_rtl_arabic.pdf"
    _save(doc, out)
    doc.close()
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.7",
        category_tags=["rtl", "arabic", "hebrew", "metadata-unicode"],
        scripts=["arabic", "hebrew", "rtl", "ascii"],
        contentKind="vector-text",
        description=("RTL text in Arabic and Hebrew, rendered via insert_htmlbox "
                     "(font fallback covers both scripts). Metadata Title/Subject "
                     "also carry the Unicode strings for extraction tests. "
                     "Note: Arabic shaping may convert codepoints to presentation "
                     "forms during extraction — both forms are still in the "
                     "Arabic Unicode block.")
    )
    return out


def gen_cyrillic_greek() -> Path:
    """Cyrillic + Greek + Latin Extended text body via insert_htmlbox."""
    doc = fitz.open()
    page = doc.new_page()
    page.insert_text((72, 60), "Cyrillic / Greek / Latin Ext fixture", fontsize=12)
    rect = fitz.Rect(72, 90, 522, 300)
    page.insert_htmlbox(
        rect,
        '<p>Russian: Привет мир</p>'
        '<p>Greek: Γειά σου κόσμε</p>'
        '<p>Latin Ext: Café—naïve—über</p>'
    )
    _set_metadata(doc, "Cyrillic Greek Latin-Ext fixture")
    out = OUT_DIR / "phase3_cyrillic_greek.pdf"
    _save(doc, out)
    doc.close()
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.7",
        category_tags=["cyrillic", "greek", "latin-extended", "vector-text"],
        scripts=["cyrillic", "greek", "latin-extended", "ascii"],
        contentKind="vector-text",
        description="Body text in Cyrillic (Russian), Greek, and Latin Extended (with diacritics)."
    )
    return out


def gen_utf16_metadata() -> Path:
    """ASCII body but metadata Title/Author contain Unicode that PyMuPDF
    will write as UTF-16BE PDFDocEncoding strings (the spec-canonical
    way to express non-ASCII strings in the Info dict)."""
    doc = fitz.open()
    page = doc.new_page()
    page.insert_text((72, 90), "UTF-16 metadata fixture", fontsize=12)
    page.insert_text((72, 110), "Body is plain ASCII; metadata is Unicode.",
                     fontsize=10)
    doc.set_metadata({
        "title": "Title with Unicode: 日本語 + Ω + naïve",
        "author": "Author мир — αβγ",
        "subject": "Subject تجربة",
        "keywords": "phase3,unicode-metadata,utf-16",
        "creator": FIXED_PRODUCER,
        "producer": FIXED_PRODUCER,
        "creationDate": FIXED_DATE,
        "modDate": FIXED_DATE,
    })
    out = OUT_DIR / "phase3_utf16_metadata.pdf"
    _save(doc, out)
    doc.close()
    _write_sidecar(out,
        pageCount=1, pdfVersion="1.7",
        category_tags=["utf-16", "metadata-unicode", "vector-text", "ascii-body"],
        scripts=["ascii", "metadata-unicode"],
        contentKind="vector-text",
        description="ASCII body. Info dict Title/Author/Subject contain CJK/Greek/Cyrillic/Arabic — written as UTF-16BE per PDF spec."
    )
    return out


def _make_noise_png(W: int, H: int, rng: random.Random) -> bytes:
    """Build a non-compressible RGB noise PNG deterministically."""
    raw = bytearray()
    for y in range(H):
        raw.append(0)  # filter byte
        for x in range(W):
            raw.append(rng.randint(0, 255))
            raw.append(rng.randint(0, 255))
            raw.append(rng.randint(0, 255))
    compressed = zlib.compress(bytes(raw), 6)

    def png_chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data)
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    png = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)  # 8-bit RGB
    png += png_chunk(b"IHDR", ihdr)
    png += png_chunk(b"IDAT", compressed)
    png += png_chunk(b"IEND", b"")
    return bytes(png)


def gen_large_100page_image() -> Path:
    """100-page document with a unique noise image per page so the
    file does NOT collapse via XObject deduplication. Aims for the
    'medium' (5–50 MB) size gap.

    Each image is 320x240 random-noise RGB (~230 KB compressed each;
    100 of them = roughly 22-25 MB total)."""
    W, H = 320, 240
    rng = random.Random(0xA7C2E541)

    doc = fitz.open()
    for i in range(100):
        page = doc.new_page(width=595, height=842)
        page.insert_text((72, 60), f"Phase-3 large fixture page {i + 1}", fontsize=12)
        rect = fitz.Rect(72, 80, 522, 680)
        # Per-page unique noise image — defeats XObject dedup
        img_bytes = _make_noise_png(W, H, rng)
        page.insert_image(rect, stream=img_bytes)
    _set_metadata(doc, "Phase-3 large 100-page image fixture")
    out = OUT_DIR / "phase3_large_100page_image.pdf"
    _save(doc, out, deflate=True)
    doc.close()

    sz = out.stat().st_size
    _write_sidecar(out,
        pageCount=100, pdfVersion="1.7", hasImages=True,
        category_tags=["medium", "large-corpus", "100-page", "image-heavy"],
        scripts=["ascii"], contentKind="mixed",
        description=f"100 pages, each with a 320x240 deterministic noise PNG. File size {sz} bytes."
    )
    return out


# Driver -----------------------------------------------------------------------
GENERATORS = [
    gen_pdf14_basic,
    gen_pdf20_minimal,
    gen_aes128_encrypted,
    gen_form_all_field_types,
    gen_annotations_all_types,
    gen_cjk_text,
    gen_rtl_arabic,
    gen_cyrillic_greek,
    gen_utf16_metadata,
    gen_large_100page_image,
]


def main() -> int:
    print(f"Generating phase-3 fixtures into {OUT_DIR}")
    failures: list[tuple[str, str]] = []
    paths: list[Path] = []
    for g in GENERATORS:
        try:
            p = g()
            paths.append(p)
            print(f"  OK {p.name} ({p.stat().st_size:,} bytes)")
        except Exception as e:
            failures.append((g.__name__, repr(e)))
            print(f"  FAIL {g.__name__}: {e}", file=sys.stderr)
    if failures:
        print("\nFailures:", file=sys.stderr)
        for name, msg in failures:
            print(f"  {name}: {msg}", file=sys.stderr)
        return 1
    print(f"\nGenerated {len(paths)} fixtures.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
