# -*- coding: utf-8 -*-
"""Generate embedded_longname_italic_serif.pdf — the >31-char /BaseFont proof
fixture for the untruncated-/BaseFont change (pipeline b91f4e83 follow-up).

Why this fixture exists: MuPDF stores a font's name in a fixed char[32] buffer,
so fz_font_name() truncates any /BaseFont longer than 31 characters. The sibling
fixture embedded_italic_serif_subset.pdf uses a name that is EXACTLY 31 chars
(SSTISF+PalatinoLTPro BoldItalic) — faithful to the live SpecimenA defect, but it
cannot demonstrate the truncation bug because nothing is clipped.

This fixture deliberately uses a 36-char /BaseFont with the style keyword at the
TAIL:  XXXXXX+Palatino Linotype Bold Italic
Truncated to 31 chars that becomes "XXXXXX+Palatino Linotype Bold I" — the word
"Italic" is lost. So:
  * BEFORE the fix, TextExtractor reports the 31-char truncated name and the edit
    overlay's name-parse sees no "italic" -> renders upright over slanted text.
  * AFTER the fix (TextExtractor resolves the untruncated /BaseFont), the span's
    fontName is the full 36-char name with "Italic" intact, so buildOverlayFont
    sets italic.

The gtest test_textedit_font_identity.cpp asserts exactly that contrast.

Run from the repo root:
  python tests/fixtures/make_embedded_longname_italic_serif.py
Writes tests/fixtures/embedded_longname_italic_serif.pdf (committed).
"""
from __future__ import annotations

import io
import pathlib
import sys
import tempfile

import fitz  # PyMuPDF

OUT = pathlib.Path(__file__).resolve().parent / "embedded_longname_italic_serif.pdf"

# Same serif bold-italic source faces as the sibling fixture.
FONT_CANDIDATES = [
    (r"C:\Windows\Fonts\palabi.ttf", "Palatino Linotype Bold Italic"),
    (r"C:\Windows\Fonts\timesbi.ttf", "Times New Roman Bold Italic"),
    (r"C:\Windows\Fonts\georgiaz.ttf", "Georgia Bold Italic"),
    (r"C:\Windows\Fonts\cambriaz.ttf", "Cambria Bold Italic"),
]

PAGE_W_PT, PAGE_H_PT = 594.0, 360.0
HEADING_TEXT = "A Tale of Two Collards"   # NO 'z'
HEADING_PT = 33.0
HEADING_BASELINE = (60.0, 140.0)

# Deliberately LONG: "family subfamily" = "Palatino Linotype Bold Italic" (29) →
# with a 7-char "XXXXXX+" subset prefix the /BaseFont is 36 chars, so MuPDF's
# 31-char fz_font.name clips the trailing "talic" / "Italic". The spaced
# subfamily ("Bold Italic", two words) is what pushes it past the cap.
TARGET_PS_NAME = "PalatinoLinotype-BoldItalic"
TARGET_FAMILY = "Palatino Linotype"
TARGET_SUBFAMILY = "Bold Italic"


def _renamed_serif_font(src_path: str) -> str:
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
    tmp = pathlib.Path(tempfile.gettempdir()) / "_pdfe_longname_serif_italic.ttf"
    f.save(str(tmp))
    return str(tmp)


def _pick_font() -> tuple[str, str]:
    for path, label in FONT_CANDIDATES:
        if pathlib.Path(path).exists():
            return path, label
    raise SystemExit(
        "No serif italic system font found; looked for: "
        + ", ".join(p for p, _ in FONT_CANDIDATES))


def build() -> None:
    src_path, font_label = _pick_font()
    font_path = _renamed_serif_font(src_path)
    print(f"embedding serif italic face: {font_label}  ({src_path})")

    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)
    page.insert_text(HEADING_BASELINE, HEADING_TEXT,
                     fontsize=HEADING_PT, fontname="serifbi",
                     fontfile=font_path, render_mode=0)
    doc.subset_fonts()
    doc.save(str(OUT), deflate=True, garbage=4)
    doc.close()

    print(f"wrote {OUT}  ({OUT.stat().st_size} bytes)")
    _self_check()


def _self_check() -> None:
    from fontTools.ttLib import TTFont

    chk = fitz.open(str(OUT))
    pg = chk.load_page(0)

    txt = pg.get_text("text")
    assert "Collards" in txt, "heading text not extractable"

    fonts = pg.get_fonts(full=True)
    assert fonts, "fixture has no fonts"
    xref, _ext, ftype, basefont = (fonts[0][0], fonts[0][1],
                                   fonts[0][2], fonts[0][3])
    assert "+" in basefont, f"BaseFont '{basefont}' has no subset prefix"
    assert ftype == "Type0", f"expected Type0, got {ftype}"
    # The WHOLE point of this fixture: the name MUST exceed MuPDF's 31-char cap
    # AND carry the style keyword past char 31 so truncation hides it.
    assert len(basefont) > 31, (
        f"BaseFont '{basefont}' is only {len(basefont)} chars; this fixture must "
        "exceed MuPDF's 31-char cap to exercise the truncation recovery")
    low = basefont.lower()
    assert "italic" in low and "bold" in low, (
        f"BaseFont '{basefont}' must carry 'Bold' and 'Italic'")
    assert "italic" not in basefont[:31].lower(), (
        f"truncation of '{basefont}' to 31 chars ('{basefont[:31]}') must hide "
        "'Italic' — otherwise the fixture does not prove untruncation")

    _name, _ext2, _type2, buffer = chk.extract_font(xref)
    chk.close()
    f = TTFont(io.BytesIO(buffer))
    os2, head = f["OS/2"], f["head"]
    serif = os2.panose.bFamilyType == 2 and 2 <= os2.panose.bSerifStyle <= 10
    assert serif, f"embedded face not serif (PANOSE {os2.panose.bSerifStyle})"
    assert bool(head.macStyle & 0x01) and bool(head.macStyle & 0x02), (
        f"embedded face not bold+italic (macStyle={head.macStyle})")
    print(f"self-check OK: BaseFont '{basefont}' ({len(basefont)} chars > 31), "
          f"truncates to '{basefont[:31]}' (Italic hidden), serif bold-italic")


if __name__ == "__main__":
    try:
        build()
    except SystemExit as e:
        print(str(e), file=sys.stderr)
        raise
