#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Offline guards for the OCR-layer fixtures and their generator (#250/#218).

Why these exist, in the words of the gate that demanded them
------------------------------------------------------------
Phase-3 of pipeline p93 regenerated `test_scanned_with_ocr.pdf` and
`mixed_dpi_page.pdf` to carry a genuinely glyphless OCR text layer, and claimed
three things the claim-check gate found had **no enforcing mechanism**:

1. *"Tesseract discovery is repeatable."* Nothing referenced `_TESSERACT_GLOBS`
   or `find_tesseract()` outside the generator itself, so re-hardcoding a vcpkg
   triplet -- the original defect -- would be caught by nothing.

2. *"Both fixtures carry a glyphless font."* The only consumers are two
   `xfail(strict=True)` probes with no `raises=`. A silent regression to a
   non-glyphless font reproduces the ORIGINAL blocker (the recomposite is never
   reached) and still registers as a matching XFAIL -- indistinguishable from
   the current, different blocker. The claim rested on a marker that cannot
   tell the two apart.

3. *"The font is glyphless in fact, not merely in name."* Untested, and inert
   on the live path: `render_mode=3` makes the text invisible whatever the
   outlines contain, so a renamed ordinary font would behave identically today.
   The distinction only bites when something inspects the outlines -- so
   something here does.

These run offline in under a second: no UI, no Tesseract, no app binary.
"""
from __future__ import annotations

import io
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = REPO / "tests" / "fixtures"
sys.path.insert(0, str(FIXTURES))

fitz = pytest.importorskip("fitz", reason="pymupdf is required to read fixture fonts")
ttLib = pytest.importorskip("fontTools.ttLib", reason="fontTools is required to inspect glyph outlines")

from glyphless_font import build_glyphless_ttf  # noqa: E402

OCR_FIXTURES = ["test_scanned_with_ocr.pdf", "mixed_dpi_page.pdf"]

# tests/ui_automation/harness/render_drive.py::THUMB_REGION_BBOX. Duplicated as
# a literal ON PURPOSE: importing the harness drags in pywinauto and a UI stack
# this offline test must not need. The duplication is checked below rather than
# trusted -- see test_the_thumb_region_bbox_literal_matches_the_harness.
THUMB_REGION_BBOX = (80.0, 470.0, 224.0, 614.0)


def _page0(name: str):
    path = FIXTURES / name
    assert path.exists(), f"fixture missing: {path}"
    return fitz.open(str(path))[0]


@pytest.mark.parametrize("name", OCR_FIXTURES)
def test_fixture_carries_a_glyphless_font_name(name: str) -> None:
    """The product gate is a 'glyphless' substring on the font name."""
    fonts = _page0(name).get_fonts()
    assert fonts, f"{name} has NO font resources at all -- there is no OCR span to edit"
    names = [f[3] for f in fonts]
    assert any("glyphless" in n.lower() for n in names), (
        f"{name} carries {names!r}, none containing 'glyphless'. "
        "TextSpan::isOcrInvisibleLayer() will be false for every span, the "
        "recomposite will never engage, and the two mixed-DPI probes will keep "
        "reporting xfail for a DIFFERENT reason than their marker claims.")


@pytest.mark.parametrize("name", OCR_FIXTURES)
def test_the_embedded_font_really_has_no_outlines(name: str) -> None:
    """Glyphless in FACT, not just in name.

    Without this, 'glyphless' is a naming convention: render_mode=3 hides the
    text whatever the outlines contain, so a renamed ordinary font would pass
    every other check here.
    """
    doc = fitz.open(str(FIXTURES / name))
    page = doc[0]
    # Selected up front rather than with a `continue` inside the loop: a
    # skip-shaped branch is what check_reachability_skip_shape.py flags as the
    # WIN-45 false green, and it is right to -- a loop that quietly filters
    # everything away measures nothing. The assert below is the non-vacuity
    # guard either way.
    glyphless = [f for f in page.get_fonts() if "glyphless" in f[3].lower()]
    assert glyphless, (
        f"{name}: no glyphless font to inspect -- "
        f"carries {[f[3] for f in page.get_fonts()]!r}")
    checked = 0
    for xref, ext, _type, basefont, _ref, _enc in glyphless:
        name_, ext_, _t, buf = doc.extract_font(xref)
        assert buf, f"{name}: glyphless font {basefont} has no embedded file"
        font = ttLib.TTFont(io.BytesIO(buf))
        assert "glyf" in font, f"{name}: {basefont} has no glyf table"
        glyf = font["glyf"]
        non_empty = [g for g in font.getGlyphOrder()
                     if glyf[g].numberOfContours not in (0, -1)]
        assert not non_empty, (
            f"{name}: {basefont} draws real outlines for {non_empty[:5]} -- it "
            "is glyphless in name only")
        # A zero-advance font would collapse every span to zero width and
        # silently break the hit-testing these fixtures exist to support.
        widths = {font["hmtx"][g][0] for g in font.getGlyphOrder()}
        assert widths != {0}, (
            f"{name}: {basefont} advances are all zero; spans would have no "
            "width and hit-testing would fail")
        checked += 1
    assert checked, f"{name}: no glyphless font was inspected -- the loop found nothing"


def test_the_generated_font_is_glyphless_and_advances() -> None:
    """Guard the generator itself, not only its committed output."""
    font = ttLib.TTFont(io.BytesIO(build_glyphless_ttf()))
    glyf = font["glyf"]
    assert not [g for g in font.getGlyphOrder()
                if glyf[g].numberOfContours not in (0, -1)]
    assert {font["hmtx"][g][0] for g in font.getGlyphOrder()} != {0}


def test_mixed_dpi_has_a_span_inside_the_thumb_region() -> None:
    """The #2 proof edits a span over the 150-DPI region; it must be there."""
    page = _page0("mixed_dpi_page.pdf")
    x0, y0, x1, y1 = THUMB_REGION_BBOX
    inside = []
    for block in page.get_text("dict")["blocks"]:
        if block["type"] != 0:
            continue
        for line in block["lines"]:
            for span in line["spans"]:
                bx0, by0, bx1, by1 = span["bbox"]
                if bx0 >= x0 and by0 >= y0 and bx1 <= x1 and by1 <= y1:
                    inside.append(span["text"])
    assert inside, (
        "no text span lies inside THUMB_REGION_BBOX "
        f"{THUMB_REGION_BBOX} -- the mixed-DPI probe has nothing to edit over "
        "the 150-DPI region")


def test_the_thumb_region_bbox_literal_matches_the_harness() -> None:
    """The duplicated literal above must not drift from the harness."""
    src = (REPO / "tests" / "ui_automation" / "harness"
           / "render_drive.py").read_text(encoding="utf-8", errors="replace")
    import re
    m = re.search(r"^THUMB_REGION_BBOX\s*=\s*\(([^)]*)\)", src, re.M)
    assert m, "THUMB_REGION_BBOX not found in render_drive.py"
    harness = tuple(float(v.strip()) for v in m.group(1).split(","))
    assert harness == THUMB_REGION_BBOX, (
        f"harness says {harness}, this file says {THUMB_REGION_BBOX}")


# ---------------------------------------------------------------------------
# Tesseract discovery. The original defect was a hardcoded vcpkg triplet:
# the lookup named `x64-windows` while this tree builds `x64-windows-repro`,
# so it found none of the five tesseract binaries under build/.
# ---------------------------------------------------------------------------
def _reload_generator(monkeypatch, repo_root: pathlib.Path):
    import importlib
    import make_test_scanned_with_ocr as gen
    importlib.reload(gen)
    monkeypatch.setattr(gen, "REPO", repo_root)
    return gen


def _make_fake_tesseract(root: pathlib.Path, triplet: str) -> pathlib.Path:
    p = (root / "build" / "somepreset" / "vcpkg_installed" / triplet
         / "tools" / "tesseract" / "tesseract.exe")
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"not a real binary")
    return p


@pytest.mark.parametrize("triplet", ["x64-windows", "x64-windows-repro",
                                     "arm64-windows", "x64-windows-static"])
def test_discovery_is_triplet_agnostic(tmp_path, monkeypatch, triplet) -> None:
    """Any triplet directory must be found -- that is the whole defect."""
    gen = _reload_generator(monkeypatch, tmp_path)
    expected = _make_fake_tesseract(tmp_path, triplet)
    monkeypatch.delenv("TESSERACT_EXE", raising=False)
    assert gen.find_tesseract() == str(expected)


def test_discovery_falls_back_to_path_when_nothing_is_built(tmp_path,
                                                            monkeypatch) -> None:
    gen = _reload_generator(monkeypatch, tmp_path)
    monkeypatch.delenv("TESSERACT_EXE", raising=False)
    monkeypatch.setattr(gen.pathlib.Path, "exists", lambda self: False)
    assert gen.find_tesseract() == "tesseract"


def test_an_explicit_missing_path_is_rejected_not_ignored(tmp_path,
                                                          monkeypatch) -> None:
    """A typo'd --tesseract must fail loudly, not silently fall through."""
    gen = _reload_generator(monkeypatch, tmp_path)
    with pytest.raises(SystemExit):
        gen.find_tesseract(str(tmp_path / "nope" / "tesseract.exe"))


def test_the_env_var_is_honoured(tmp_path, monkeypatch) -> None:
    gen = _reload_generator(monkeypatch, tmp_path)
    exe = _make_fake_tesseract(tmp_path, "x64-windows")
    other = tmp_path / "elsewhere.exe"
    other.write_bytes(b"x")
    monkeypatch.setenv("TESSERACT_EXE", str(other))
    assert gen.find_tesseract() == str(other), "TESSERACT_EXE was ignored"
    assert exe.exists()  # the glob candidate existed and was still overridden


def test_tessdata_resolves_to_the_committed_directory() -> None:
    """The committed traineddata is what a clean checkout has."""
    import make_test_scanned_with_ocr as gen
    import importlib
    importlib.reload(gen)
    resolved = gen.find_tessdata()
    assert resolved, "no tessdata directory resolved at all"
    assert (pathlib.Path(resolved) / "eng.traineddata").exists(), (
        f"resolved tessdata {resolved!r} has no eng.traineddata")
