# -*- coding: utf-8 -*-
"""Generate embedded_descriptor_style.pdf — a font whose italic+bold style is
encoded ONLY in the /FontDescriptor, with a NAME that carries no style keyword.

Why: the edit overlay + new-char paths historically detected italic/bold by
substring-scanning the font NAME. This fixture proves the descriptor-based path:
the embedded font is renamed to a neutral "Sutpenia Regular" (no
bold/italic/oblique substring), and its /FontDescriptor is forced (via pikepdf)
to carry /Flags Italic bit (64) + ForceBold bit (0x40000) and /ItalicAngle=-12.
So any italic/bold detection on this fixture can ONLY have come from the
descriptor, never the name.

Run from repo root:
  python tests/fixtures/make_descriptor_style_font.py
Writes tests/fixtures/embedded_descriptor_style.pdf (committed).
"""
from __future__ import annotations

import pathlib
import sys
import tempfile

import fitz          # PyMuPDF
import pikepdf

OUT = pathlib.Path(__file__).resolve().parent / "embedded_descriptor_style.pdf"

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\palabi.ttf",   # Palatino Linotype Bold Italic (serif italic)
    r"C:\Windows\Fonts\timesbi.ttf",
    r"C:\Windows\Fonts\georgiaz.ttf",
]

PAGE_W_PT, PAGE_H_PT = 594.0, 360.0
HEADING_TEXT = "A Tale of Two Collards"
HEADING_PT = 33.0
HEADING_BASELINE = (60.0, 140.0)

# A NEUTRAL name: no "bold"/"italic"/"oblique" substring anywhere.
NEUTRAL_FAMILY = "Sutpenia"
NEUTRAL_SUBFAMILY = "Regular"

FLAG_ITALIC = 1 << 6      # /Flags bit 7 (value 64)
FLAG_FORCEBOLD = 1 << 18  # /Flags bit 19 (value 0x40000)
FLAG_SERIF = 1 << 1       # /Flags bit 2 (Serif, value 2) — keep the serif identity


def _neutral_font(src_path: str) -> str:
    from fontTools.ttLib import TTFont
    f = TTFont(src_path)
    name = f["name"]
    recs = {1: NEUTRAL_FAMILY, 2: NEUTRAL_SUBFAMILY,
            4: f"{NEUTRAL_FAMILY} {NEUTRAL_SUBFAMILY}",
            6: f"{NEUTRAL_FAMILY}-{NEUTRAL_SUBFAMILY}",
            16: NEUTRAL_FAMILY, 17: NEUTRAL_SUBFAMILY}
    for nid, val in recs.items():
        name.setName(val, nid, 3, 1, 0x409)
        name.setName(val, nid, 1, 0, 0)
    tmp = pathlib.Path(tempfile.gettempdir()) / "_pdfe_neutral_descriptor.ttf"
    f.save(str(tmp))
    return str(tmp)


def _pick() -> str:
    for p in FONT_CANDIDATES:
        if pathlib.Path(p).exists():
            return p
    raise SystemExit("no serif italic source font found: " + ", ".join(FONT_CANDIDATES))


def build() -> None:
    font_path = _neutral_font(_pick())
    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)
    page.insert_text(HEADING_BASELINE, HEADING_TEXT, fontsize=HEADING_PT,
                     fontname="neutral", fontfile=font_path, render_mode=0)
    doc.subset_fonts()
    tmp_pdf = pathlib.Path(tempfile.gettempdir()) / "_pdfe_descstyle_tmp.pdf"
    doc.save(str(tmp_pdf), deflate=True, garbage=4)
    doc.close()

    # Force the descriptor flags so style lives ONLY in the descriptor.
    pdf = pikepdf.open(str(tmp_pdf))
    forced = 0
    for obj in pdf.objects:
        try:
            is_fd = isinstance(obj, pikepdf.Dictionary) and \
                str(obj.get("/Type", "")) == "/FontDescriptor"
        except Exception:
            is_fd = False
        if not is_fd:
            continue
        flags = int(obj.get("/Flags", 0))
        flags |= FLAG_ITALIC | FLAG_FORCEBOLD | FLAG_SERIF
        obj["/Flags"] = flags
        obj["/ItalicAngle"] = -12
        forced += 1
    if forced == 0:
        raise SystemExit("no /FontDescriptor found to force flags on")
    pdf.save(str(OUT))
    pdf.close()
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes); forced {forced} descriptor(s)")
    _self_check()


def _self_check() -> None:
    chk = fitz.open(str(OUT))
    pg = chk.load_page(0)
    assert "Collards" in pg.get_text("text"), "heading not extractable"
    fonts = pg.get_fonts(full=True)
    assert fonts, "no fonts"
    basefont = fonts[0][3]
    low = basefont.lower()
    assert "italic" not in low and "bold" not in low and "oblique" not in low, (
        f"BaseFont '{basefont}' must be style-NEUTRAL (no keyword) so the test "
        "proves descriptor-sourced detection")
    chk.close()

    pdf = pikepdf.open(str(OUT))
    seen = False
    for obj in pdf.objects:
        if isinstance(obj, pikepdf.Dictionary) and \
                str(obj.get("/Type", "")) == "/FontDescriptor":
            flags = int(obj.get("/Flags", 0))
            assert flags & FLAG_ITALIC, f"/Flags 0x{flags:x} missing Italic bit"
            assert flags & FLAG_FORCEBOLD, f"/Flags 0x{flags:x} missing ForceBold"
            assert int(obj.get("/ItalicAngle", 0)) != 0, "ItalicAngle is zero"
            seen = True
    pdf.close()
    assert seen, "no descriptor verified"
    print(f"self-check OK: BaseFont '{basefont}' is style-neutral; descriptor has "
          f"Italic+ForceBold flags + ItalicAngle=-12")


if __name__ == "__main__":
    try:
        build()
    except SystemExit as e:
        print(str(e), file=sys.stderr)
        raise
