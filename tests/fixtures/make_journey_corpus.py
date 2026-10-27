# -*- coding: utf-8 -*-
"""Generate tests/fixtures/journey_corpus.pdf — the committed, CI-portable
multi-page corpus for the cumulative single-session "journey" soak
(pipeline f4a82c1e Phase 2).

The corpus deliberately reproduces the salient structure of the real
"SpecimenA Green" document that the fresh-session suite could never exercise
cumulatively, so the journey harness (and the Phase 4 CI lane) always has a
non-skippable target even when the user's copyrighted PDF is absent:

  * NONZERO-ORIGIN MediaBox pages (like SpecimenA' [0 7.9 594 745.9]) — the
    ContentStreamEditor origin-offset path that made #4/#5 a no-op.
  * SMALL body text (8 pt) with a known, whole-word count of "the" — the
    Replace-All target that previously reported a count without landing.
  * /ActualText PREFIX spans (displays "ABC", /ActualText (A)) — the MuPDF
    1.26.10 extraction underflow that armed the #6 crash on all-page extract.
  * BLANK pages and IMAGE-ONLY pages — display-list/tile diversity.
  * Enough pages (default 64) to pressure the tile cache and the 256 MB MuPDF
    store into eviction across a long editing session.

Dependency-free (raw PDF, same idiom as make_multipage_smalltext_edit.py /
make_actualtext_fixture.py) so it runs on any CI Python without reportlab or
PyMuPDF. Writes the PDF plus journey_corpus_manifest.json describing every
page (type, marker token, per-page whole-word "the" count) so the harness
oracles assert against ground truth rather than magic numbers.

Regenerate:  python tests/fixtures/make_journey_corpus.py
"""
from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_PDF = os.path.join(HERE, "journey_corpus.pdf")
OUT_MANIFEST = os.path.join(HERE, "journey_corpus_manifest.json")

# Total pages — enough to force tile/store eviction across a long journey.
NPAGES = int(os.environ.get("JOURNEY_CORPUS_PAGES", "64"))

# Page-type cycle. Index i -> i % 5 selects the type.
TYPE_BODY = "body"                 # nonzero-origin, small "the"-bearing body
TYPE_ACTUALTEXT = "actualtext"     # /ActualText prefix span (crash-arming)
TYPE_BLANK = "blank"               # empty content stream
TYPE_IMAGE = "image"               # image-only (XObject Do)
TYPE_ORIGIN = "origin"             # large nonzero-origin MediaBox
TYPE_CYCLE = [TYPE_BODY, TYPE_ACTUALTEXT, TYPE_BLANK, TYPE_IMAGE, TYPE_ORIGIN]

# A small body sentence with a KNOWN whole-word "the" count. "the" as a whole
# word appears exactly 3 times below (the ... the ... the); "tender"/"then"
# style traps are avoided on purpose so the count is unambiguous.
_BODY_SENTENCE = "the greens simmer as the cook stirs the pot"
_THE_PER_BODY_LINE = 3
_BODY_LINES = 4  # 4 lines * 3 = 12 whole-word "the" per body page


def _whole_word_the(text: str) -> int:
    import re
    return len(re.findall(r"\bthe\b", text, flags=re.IGNORECASE))


def _pdf_escape(s: bytes) -> bytes:
    return s.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)")


def body_content(marker: str) -> bytes:
    # 18pt title marker (per-page identity for stale-tile checks) + small 8pt body.
    lines = [b"BT /F1 18 Tf 1 0 0 1 72 700 Tm (%s) Tj ET"
             % _pdf_escape(marker.encode())]
    y = 660
    for _ in range(_BODY_LINES):
        lines.append(b"BT /F1 8 Tf 1 0 0 1 72 %d Tm (%s) Tj ET"
                     % (y, _pdf_escape(_BODY_SENTENCE.encode())))
        y -= 16
    return b"\n".join(lines) + b"\n"


def actualtext_content(marker: str) -> bytes:
    # /ActualText (A) is a proper PREFIX of displayed "ABC" -> extraction
    # underflow (do_extract_within_actualtext). Plus a title marker.
    return (b"BT /F1 18 Tf 1 0 0 1 72 700 Tm (%s) Tj ET\n"
            b"BT /F1 12 Tf 1 0 0 1 72 660 Tm\n"
            b"/Span <</ActualText(A)>> BDC (ABC) Tj EMC ET\n"
            % _pdf_escape(marker.encode()))


def blank_content(marker: str) -> bytes:
    # Truly minimal — a single tiny marker so the page is identifiable but the
    # rendered surface is essentially empty (blank-page tile path).
    return b"BT /F1 6 Tf 1 0 0 1 20 20 Tm (%s) Tj ET\n" % _pdf_escape(marker.encode())


def image_content(marker: str) -> bytes:
    # Draw the shared image XObject scaled up, plus a small marker.
    return (b"q 400 0 0 400 100 200 cm /Im0 Do Q\n"
            b"BT /F1 6 Tf 1 0 0 1 20 20 Tm (%s) Tj ET\n"
            % _pdf_escape(marker.encode()))


def origin_content(marker: str) -> bytes:
    # Large nonzero-origin MediaBox; place text relative to the origin.
    return (b"BT /F1 16 Tf 1 0 0 1 90 780 Tm (%s) Tj ET\n"
            b"BT /F1 8 Tf 1 0 0 1 90 740 Tm (%s) Tj ET\n"
            % (_pdf_escape(marker.encode()), _pdf_escape(_BODY_SENTENCE.encode())))


def _image_xobject_bytes(w: int = 48, h: int = 48) -> bytes:
    # Deterministic RGB gradient, uncompressed (no filter dependency).
    data = bytearray()
    for y in range(h):
        for x in range(w):
            data.append((x * 5) & 0xFF)
            data.append((y * 5) & 0xFF)
            data.append(((x + y) * 3) & 0xFF)
    return bytes(data)


def build():
    img_w = img_h = 48
    img_data = _image_xobject_bytes(img_w, img_h)

    # Fixed object numbering:
    #   1 Catalog, 2 Pages, 3 Font, 4 Image XObject
    #   page i (0-based): page obj = 5 + 2*i, content obj = 6 + 2*i
    FONT_OBJ = 3
    IMG_OBJ = 4

    objs: list[bytes] = []
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")  # 1

    kids = b" ".join(b"%d 0 R" % (5 + 2 * i) for i in range(NPAGES))
    objs.append(b"<< /Type /Pages /Kids [%s] /Count %d >>" % (kids, NPAGES))  # 2

    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")  # 3
    objs.append(  # 4 — image XObject
        b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %d >>\n"
        b"stream\n%s\nendstream" % (img_w, img_h, len(img_data), img_data))

    manifest_pages = []
    total_the = 0

    for i in range(NPAGES):
        ptype = TYPE_CYCLE[i % len(TYPE_CYCLE)]
        marker = "PAGE_%03d_%s" % (i, ptype.upper())

        if ptype == TYPE_BODY:
            media = b"[0 7.9 594 745.9]"       # SpecimenA-like nonzero origin
            res = b"<< /Font << /F1 %d 0 R >> >>" % FONT_OBJ
            content = body_content(marker)
        elif ptype == TYPE_ACTUALTEXT:
            media = b"[0 0 612 792]"
            res = b"<< /Font << /F1 %d 0 R >> >>" % FONT_OBJ
            content = actualtext_content(marker)
        elif ptype == TYPE_BLANK:
            media = b"[0 0 612 792]"
            res = b"<< /Font << /F1 %d 0 R >> >>" % FONT_OBJ
            content = blank_content(marker)
        elif ptype == TYPE_IMAGE:
            media = b"[0 0 612 792]"
            res = (b"<< /Font << /F1 %d 0 R >> /XObject << /Im0 %d 0 R >> >>"
                   % (FONT_OBJ, IMG_OBJ))
            content = image_content(marker)
        else:  # TYPE_ORIGIN
            media = b"[50 50 662 842]"          # large nonzero origin
            res = b"<< /Font << /F1 %d 0 R >> >>" % FONT_OBJ
            content = origin_content(marker)

        the_count = _whole_word_the(content.decode("latin-1"))
        total_the += the_count
        manifest_pages.append({
            "index": i,
            "type": ptype,
            "marker": marker,
            "the_count": the_count,
            "media_box": media.decode("ascii"),
        })

        page_obj = (b"<< /Type /Page /Parent 2 0 R /MediaBox %s "
                    b"/Resources %s /Contents %d 0 R >>"
                    % (media, res, 6 + 2 * i))
        content_obj = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content)
        objs.append(page_obj)      # 5 + 2*i
        objs.append(content_obj)   # 6 + 2*i

    # Serialize with a classic xref table.
    pdf = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    offsets = []
    for idx, body in enumerate(objs, start=1):
        offsets.append(len(pdf))
        pdf += b"%d 0 obj\n%s\nendobj\n" % (idx, body)
    xref_pos = len(pdf)
    n = len(objs) + 1
    pdf += b"xref\n0 %d\n" % n
    pdf += b"0000000000 65535 f \n"
    for off in offsets:
        pdf += b"%010d 00000 n \n" % off
    pdf += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (n, xref_pos))

    with open(OUT_PDF, "wb") as f:
        f.write(pdf)

    manifest = {
        "pages": NPAGES,
        "type_cycle": TYPE_CYCLE,
        "body_sentence": _BODY_SENTENCE,
        "the_per_body_line": _THE_PER_BODY_LINE,
        "body_lines": _BODY_LINES,
        "total_whole_word_the": total_the,
        "page_details": manifest_pages,
        "note": "Regenerate via tests/fixtures/make_journey_corpus.py. "
                "total_whole_word_the is the case-insensitive whole-word count "
                "of 'the' across all pages (Replace-All ground truth).",
    }
    with open(OUT_MANIFEST, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    return pdf, manifest


if __name__ == "__main__":
    pdf, manifest = build()
    print("wrote", OUT_PDF, len(pdf), "bytes,", manifest["pages"], "pages,",
          manifest["total_whole_word_the"], "whole-word 'the'")
    print("wrote", OUT_MANIFEST)
