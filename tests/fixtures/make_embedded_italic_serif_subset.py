# -*- coding: utf-8 -*-
"""Generate the embedded ITALIC SERIF SUBSET fixture for pipeline b91f4e83
phase 6 — embedded_italic_serif_subset.pdf.

Why this fixture exists (phase-4 gap analysis §3.3): NO fixture in the corpus
was an embedded + subsetted ITALIC SERIF font, so the three text-edit defects
(1b overlay drops italic, 2 geometry, 3 new-char serif->sans) could not be
exercised by any test. The only subset heading fixture, invoice_vector_heading
(`HDJKHA+Arial,Bold`), is sans / non-italic and therefore structurally blind to
serif->sans and italic-drop.

This fixture reproduces the defect PDF's span profile
(`BAAAAA+PalatinoLTPro-BoldItalic`, embedded subset, serif, ItalicAngle<0,
bold+italic) using a system serif italic face embedded then SUBSET so that
characters not present in the text — notably ``z`` — are genuinely absent from
the embedded font's cmap. That makes ``z`` the canonical "new character absent
from the embedded subset" probe for the defect-3 resolver tests.

Two text lines at different sizes (a large heading + a smaller body line) so a
per-line font-size assertion has real differing line heights to check (AC g).
Neither line contains the letter ``z``.

The committed PDF embeds the font, so CI does NOT need the source face
installed to OPEN it. The defect-3 resolver tests DO need a serif face
installed to resolve the *fallback* for ``z`` — Palatino Linotype and Times New
Roman both ship with Windows, so that holds on the Server-2019 CI image.

Run from the repo root:  python tests/fixtures/make_embedded_italic_serif_subset.py
Writes tests/fixtures/embedded_italic_serif_subset.pdf (committed).
"""
from __future__ import annotations

import io
import pathlib
import sys
import tempfile

import fitz  # PyMuPDF

OUT = pathlib.Path(__file__).resolve().parent / "embedded_italic_serif_subset.pdf"

# Serif ITALIC faces, in preference order. palabi = Palatino Linotype Bold
# Italic — the closest match to the defect's PalatinoLTPro-BoldItalic profile
# (serif, bold, italic). Fallbacks are also serif bold-italic faces shipping
# with Windows.
FONT_CANDIDATES = [
    (r"C:\Windows\Fonts\palabi.ttf", "Palatino Linotype Bold Italic"),
    (r"C:\Windows\Fonts\timesbi.ttf", "Times New Roman Bold Italic"),
    (r"C:\Windows\Fonts\georgiaz.ttf", "Georgia Bold Italic"),
    (r"C:\Windows\Fonts\cambriaz.ttf", "Cambria Bold Italic"),
]

PAGE_W_PT, PAGE_H_PT = 594.0, 360.0

HEADING_TEXT = "A Tale of Two Collards"   # 22 glyphs, NO 'z'
BODY_TEXT = "Original Recipe by Tuvache Betton"  # NO 'z'
HEADING_PT = 33.0                          # matches the defect span size
BODY_PT = 13.0
HEADING_BASELINE = (60.0, 140.0)
BODY_BASELINE = (60.0, 210.0)

PROBE_ABSENT_CHAR = "z"                    # must NOT appear in either line

# The embedded BaseFont is renamed to this so it matches the LIVE defect font
# (BAAAAA+PalatinoLTPro-BoldItalic, 31 chars) rather than the source system
# face's long name. MuPDF's fz_font.name is a hard 31-char buffer; the live
# defect name fits exactly, so the overlay's name-parsed italic/bold detection
# works on it. Using the source face's untouched name (e.g. 'Palatino Linotype
# Bold Italic' -> >31 chars) would TRUNCATE off 'talic' and defeat italic
# detection — an artifact the real defect does not have. The renamed face's
# serif/bold/italic identity (PANOSE, macStyle, weight) is unchanged; only the
# name records differ.
# fitz forms the BaseFont as "<family> <subfamily>", so family(13)+' '+sub must
# be <= 24 to leave room for the 7-char 'XXXXXX+' subset prefix under MuPDF's
# 31-char cap. 'PalatinoLTPro BoldItalic' = 24 -> BaseFont = 31, exactly the
# live defect length. 'BoldItalic' (one word) still contains both 'bold' and
# 'italic' for the overlay's name parse.
TARGET_PS_NAME = "PalatinoLTPro-BoldItalic"
TARGET_FAMILY = "PalatinoLTPro"
TARGET_SUBFAMILY = "BoldItalic"


def _renamed_serif_font(src_path: str) -> str:
    """Rewrite the face's name table to the short defect-matching name and
    return a temp .ttf path. Identity tables (OS/2, head) are untouched."""
    from fontTools.ttLib import TTFont

    f = TTFont(src_path)
    name = f["name"]
    records = {
        1: TARGET_FAMILY, 2: TARGET_SUBFAMILY,
        4: f"{TARGET_FAMILY} {TARGET_SUBFAMILY}", 6: TARGET_PS_NAME,
        16: TARGET_FAMILY, 17: TARGET_SUBFAMILY,
    }
    for nid, val in records.items():
        name.setName(val, nid, 3, 1, 0x409)   # Windows / Unicode BMP / en-US
        name.setName(val, nid, 1, 0, 0)        # Mac / Roman / en
    tmp = pathlib.Path(tempfile.gettempdir()) / "_pdfe_serif_italic_renamed.ttf"
    f.save(str(tmp))
    return str(tmp)


def _pick_font() -> tuple[str, str]:
    for path, label in FONT_CANDIDATES:
        if pathlib.Path(path).exists():
            return path, label
    raise SystemExit(
        "No serif italic system font found; cannot build the italic-serif "
        "subset fixture. Looked for: "
        + ", ".join(p for p, _ in FONT_CANDIDATES))


def build() -> None:
    assert PROBE_ABSENT_CHAR not in HEADING_TEXT, "probe char leaked into heading"
    assert PROBE_ABSENT_CHAR not in BODY_TEXT, "probe char leaked into body"

    src_path, font_label = _pick_font()
    font_path = _renamed_serif_font(src_path)
    print(f"embedding serif italic face: {font_label}  ({src_path})")
    print(f"renamed BaseFont -> {TARGET_PS_NAME} (matches the live defect length)")

    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)

    # One embedded font resource, two lines at different sizes. Visible text
    # (render_mode 0) so the span is a real editable vector run.
    page.insert_text(HEADING_BASELINE, HEADING_TEXT,
                     fontsize=HEADING_PT, fontname="serifbi",
                     fontfile=font_path, render_mode=0)
    page.insert_text(BODY_BASELINE, BODY_TEXT,
                     fontsize=BODY_PT, fontname="serifbi",
                     fontfile=font_path, render_mode=0)

    # Subset the embedded font to only the glyphs actually used, so 'z' (and
    # every other unused codepoint) is removed from the embedded cmap. This is
    # what makes the file a genuine *subset* and 'z' genuinely absent.
    doc.subset_fonts()
    doc.save(str(OUT), deflate=True, garbage=4)
    doc.close()

    print(f"wrote {OUT}  ({OUT.stat().st_size} bytes)")
    _self_check()


def _self_check() -> None:
    """Validate the fixture actually has the properties the tests rely on.

    Note on the embedded font: fitz emits a Type0/Identity-H CIDFontType2 and
    its subset pass strips the Unicode ``cmap`` (CID == GID, so glyph lookup is
    by index, not codepoint). Text stays *extractable* via the /ToUnicode map,
    but the embedded font program is cmap-less — exactly how the real defect PDF
    (PalatinoLTPro-BoldItalic, Identity-H) behaves. The C++ resolver loads this
    program in FreeType and calls ``FT_Get_Char_Index``; with no cmap, any
    codepoint NOT placed by the content stream — notably ``z`` — returns gid 0
    (unencodable) and is routed to the system serif fallback. That is precisely
    the defect-3 path under test.
    """
    import io
    from fontTools.ttLib import TTFont

    chk = fitz.open(str(OUT))
    pg = chk.load_page(0)

    # 1. Both text lines extractable (the spans the editor will click).
    # fitz inserts non-breaking spaces between words, so match single words.
    txt = pg.get_text("text")
    assert "Collards" in txt, "heading text not extractable"
    assert "Betton" in txt, "body text not extractable"

    # 2. The embedded font carries a subset prefix (6 uppercase letters + '+').
    fonts = pg.get_fonts(full=True)
    assert fonts, "fixture has no fonts"
    xref, _ext, ftype, basefont = fonts[0][0], fonts[0][1], fonts[0][2], fonts[0][3]
    assert "+" in basefont, (
        f"embedded font BaseFont '{basefont}' has no subset prefix")
    assert ftype == "Type0", f"expected Type0 CID font, got {ftype}"
    # MuPDF's fz_font.name is a 31-char (+NUL) buffer; the overlay's name-parsed
    # italic/bold detection reads that truncated string. The name must fit so
    # 'Italic' survives — exactly as the live defect font does.
    assert len(basefont) <= 31, (
        f"BaseFont '{basefont}' is {len(basefont)} chars > MuPDF's 31-char cap; "
        "the overlay would receive a truncated name with 'Italic' cut off")
    low = basefont.lower()
    assert "bold" in low and "italic" in low, (
        f"BaseFont '{basefont}' must carry both 'Bold' and 'Italic' so the "
        "overlay name-parse sets both flags")
    print(f"subset BaseFont = {basefont}  ({ftype}, {len(basefont)} chars)")

    # 3. The embedded face IDENTITY matches the defect profile: serif + bold +
    #    italic. Read OS/2 PANOSE and head.macStyle straight from the embedded
    #    program (this is the byte-level ground truth the C++ tests assert on).
    _name, _ext2, _type2, buffer = chk.extract_font(xref)
    chk.close()
    f = TTFont(io.BytesIO(buffer))
    os2, head = f["OS/2"], f["head"]
    serif = os2.panose.bFamilyType == 2 and 2 <= os2.panose.bSerifStyle <= 10
    bold = bool(head.macStyle & 0x01)
    italic = bool(head.macStyle & 0x02)
    cmap = f.getBestCmap() if "cmap" in f else {}
    z_encodable = ord("z") in cmap
    assert serif, (
        f"embedded face is not serif (PANOSE bSerifStyle="
        f"{os2.panose.bSerifStyle}); fixture must reproduce the serif profile")
    assert bold and italic, (
        f"embedded face is not bold+italic (macStyle={head.macStyle}); "
        "fixture must reproduce the BoldItalic profile")
    assert not z_encodable, (
        "embedded font cmap encodes 'z' — the defect-3 probe char must be "
        "unencodable so the resolver routes it to the serif fallback")
    print(f"self-check OK: Type0 serif bold-italic subset "
          f"(PANOSE serif={os2.panose.bSerifStyle}, weight={os2.usWeightClass}, "
          f"macStyle={head.macStyle}), 'z' unencodable, both lines extractable")


if __name__ == "__main__":
    try:
        build()
    except SystemExit as e:
        print(str(e), file=sys.stderr)
        raise
