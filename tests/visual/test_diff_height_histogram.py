# -*- coding: utf-8 -*-
"""Unit tests for the height-histogram comparison mode (pipeline c4a9e271
phase-3) added to :mod:`tests.visual.diff`.

These use synthetic PIL images (no app launch), so they run anywhere the
test-runner gate runs. They lock the metric the per-line-font-size visual
assertions rely on: glyph-ink HEIGHT, and the ratio between two captures.
"""
from __future__ import annotations

from PIL import Image, ImageDraw

from tests.visual import diff


def _white(w=200, h=120):
    return Image.new("RGB", (w, h), (255, 255, 255))


def _bar(img, top, height, left=20, width=120, color=(0, 0, 0)):
    """Draw a solid black bar — a stand-in for a line of glyph ink."""
    d = ImageDraw.Draw(img)
    d.rectangle([left, top, left + width, top + height - 1], fill=color)
    return img


def test_glyph_ink_height_measures_bar_extent():
    img = _bar(_white(), top=30, height=40)
    h = diff.glyph_ink_height(img)
    # The measured span should match the drawn bar height within a pixel.
    assert abs(h - 40) <= 1, f"expected ~40px ink height, got {h}"


def test_glyph_ink_height_blank_image_is_zero():
    assert diff.glyph_ink_height(_white()) == 0


def test_ink_row_histogram_localizes_ink_rows():
    img = _bar(_white(), top=50, height=20, left=10, width=100)
    hist = diff.ink_row_histogram(img)
    assert hist.shape[0] == img.size[1]
    # Rows inside the bar have ~width dark pixels; rows outside have none.
    assert hist[60] >= 90, f"expected dense ink mid-bar, got {hist[60]}"
    assert hist[10] == 0 and hist[110] == 0, "rows outside the bar must be ink-free"


def test_height_histogram_compare_passes_for_taller_heading():
    heading = _bar(_white(), top=20, height=44)   # tall "heading" line
    body = _bar(_white(), top=20, height=13)       # short "body" line
    res = diff.height_histogram_compare(heading, body, min_ratio=1.5)
    assert res["pass"] is True
    assert res["taller_height"] > res["shorter_height"]
    assert res["ratio"] >= 1.5, res


def test_height_histogram_compare_fails_when_sizes_equal():
    # The font-size-bug scenario: both lines render at the SAME size. The mode
    # must NOT pass, distinguishing a real per-line difference from uniform size.
    a = _bar(_white(), top=20, height=16)
    b = _bar(_white(), top=70, height=16)
    res = diff.height_histogram_compare(a, b, min_ratio=1.5)
    assert res["pass"] is False
    assert abs(res["ratio"] - 1.0) < 0.2, res


def test_height_histogram_compare_fails_when_shorter_is_blank():
    heading = _bar(_white(), top=20, height=40)
    blank = _white()
    res = diff.height_histogram_compare(heading, blank, min_ratio=1.5)
    assert res["pass"] is False
    assert res["shorter_height"] == 0
    assert res["ratio"] == 0.0
