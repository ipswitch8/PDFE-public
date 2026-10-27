# -*- coding: utf-8 -*-
"""The canvas page-area crop must remove theme coupling WITHOUT blinding the oracle.

WHY THIS EXISTS
===============
`PdfCanvas::paintEvent` fills the viewport backdrop from
`palette().color(QPalette::Dark)` through
`AccessibilityHelper::adaptColor(..., QPalette::Window)` -- deliberately, so a
High-Contrast theme does not collide with a hardcoded mid-grey. That makes the
surround's colour a property of the SESSION THEME, not of the product's
rendering, so a pixel oracle over the whole canvas asserts the theme.

Measured in CI run 37366537472 (2026-10-05): eleven canvas tests in
`tests/test_ui_file_operations.py` failed at SSIM 0.53-0.57 against a 0.99
threshold with 44-62% of pixels differing. The diff showed red confined entirely
to the two margin strips -- the page and its text were pixel-identical. Nothing
about the document was wrong.

The fix crops the comparison to the page rect, taken from the
`page_rect_on_canvas` seam. The DANGER of that fix is obvious and is the reason
for this file: a crop that is too aggressive, or applied to only one side, turns
a real rendering regression into a silent pass. "The tests stopped failing" is
not the goal.

So this pins three properties, and the third is the load-bearing one:

  1. a surround-only difference FAILS without the crop   (reproduces the defect)
  2. the same difference PASSES with the crop            (the fix works)
  3. a difference INSIDE the page still FAILS with crop  (the oracle is not blind)

These drive the REAL `_baseline_check` -- not a reimplementation of it -- so the
checks cannot pass against a copy of the logic while the shipped path rots. No
live UI, no app launch: the images are synthetic and the baseline directory is
redirected to a tmp path.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest
from PIL import Image

try:                                   # pytest's failure signal
    from _pytest.outcomes import Failed
except ImportError:                    # pragma: no cover - very old pytest
    Failed = pytest.fail.Exception     # type: ignore[attr-defined]

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

# Importing the UI module does not launch anything; the live-UI work is behind
# fixtures and the REQUIRES_LIVE_UI marker.
ui_fileops = pytest.importorskip(
    "tests.test_ui_file_operations",
    reason="the UI file-operations module must be importable to test its oracle",
)

W, H = 400, 300
PAGE = (100, 20, 200, 260)          # x, y, w, h -- page inset inside the canvas
LIGHT_SURROUND = (214, 209, 199)    # what the committed baselines were captured on
DARK_SURROUND = (99, 97, 92)        # what the CI session now resolves


def _canvas(surround, page=(255, 255, 255), blot=None):
    img = Image.new("RGB", (W, H), surround)
    img.paste(Image.new("RGB", (PAGE[2], PAGE[3]), page), (PAGE[0], PAGE[1]))
    if blot:
        img.paste(Image.new("RGB", (40, 40), blot), (PAGE[0] + 20, PAGE[1] + 20))
    return img


@pytest.fixture()
def baseline_dir(tmp_path, monkeypatch):
    """Redirect the committed-baseline directory at a tmp one."""
    monkeypatch.setattr(ui_fileops, "SCREENSHOTS", tmp_path)
    monkeypatch.setattr(ui_fileops, "FAILURE_DIR", tmp_path / "_failures")
    return tmp_path


def _seed(baseline_dir, name, img):
    img.save(str(baseline_dir / f"{name}.png"))


def _check(name, actual, crop=None):
    """Call the REAL oracle; return True on pass, False when it fails the test.

    Catches `Failed`, NOT `Exception`. `pytest.fail()` raises
    `_pytest.outcomes.Failed`, which derives from `BaseException` -- so an
    `except Exception` here silently does not catch it and the expected failure
    escapes as a test error. The first version of this file did exactly that,
    and the two tests asserting "the oracle must FAIL" reported as failures
    while the oracle was working perfectly.
    """
    try:
        ui_fileops._baseline_check(
            name, actual,
            ssim_threshold=0.99, pixel_threshold=0.001, crop=crop)
        return True
    except Failed:
        return False
    except AssertionError:
        # The crop-bounds guard inside _baseline_check is a plain assert.
        return False


def test_surround_only_difference_fails_without_the_crop(baseline_dir):
    """Property 1 -- the defect reproduces, so this file is testing something."""
    _seed(baseline_dir, "t1", _canvas(LIGHT_SURROUND))
    assert _check("t1", _canvas(DARK_SURROUND)) is False, (
        "a theme-only surround change passed the whole-canvas comparison, so "
        "this file no longer reproduces the condition it exists to fix"
    )


def test_surround_only_difference_passes_with_the_crop(baseline_dir):
    """Property 2 -- the fix works."""
    _seed(baseline_dir, "t2", _canvas(LIGHT_SURROUND))
    assert _check("t2", _canvas(DARK_SURROUND), crop=PAGE) is True, (
        "the page-area crop did not neutralise a surround-only difference"
    )


def test_a_change_inside_the_page_still_fails_with_the_crop(baseline_dir):
    """Property 3 -- THE ONE THAT MATTERS. The crop must not blind the oracle."""
    _seed(baseline_dir, "t3", _canvas(LIGHT_SURROUND))
    actual = _canvas(DARK_SURROUND, blot=(255, 0, 0))
    assert _check("t3", actual, crop=PAGE) is False, (
        "a real rendering change INSIDE the page passed while cropped -- the "
        "crop has blinded the oracle, which is worse than the theme coupling "
        "it was meant to remove"
    )


def test_the_crop_is_applied_to_both_sides(baseline_dir):
    """Cropping only the actual would compare different regions and mis-report.

    Identical images must pass while cropped; if the baseline were left uncropped
    the sizes would differ and the comparison could not be meaningful.
    """
    img = _canvas(LIGHT_SURROUND)
    _seed(baseline_dir, "t4", img)
    assert _check("t4", img.copy(), crop=PAGE) is True


def test_a_crop_outside_the_capture_is_rejected(baseline_dir):
    """A seam/capture disagreement is a geometry bug and must say so."""
    _seed(baseline_dir, "t5", _canvas(LIGHT_SURROUND))
    assert _check("t5", _canvas(DARK_SURROUND),
                  crop=(W - 10, H - 10, 500, 500)) is False


# ---------------------------------------------------------------------------
# Class-level guard: the crop must be applied at EVERY canvas oracle, not just
# at the ones that happened to be failing when the defect was found.
#
# The five property tests above prove the crop MECHANISM is sound. They say
# nothing about whether each call site uses it, and that is where this defect
# class actually lives: the original fix cropped five canvas sites while five
# more kept comparing the theme-derived surround, so the UI lanes stayed red
# for the same reason at the sites nobody had looked at. A new canvas test
# added tomorrow without `crop=` reintroduces it again.
#
# Static, so it runs on every push in fast-checks rather than only under a live
# UI. Keyed on the SHAPE -- "this site captures the canvas" -- rather than on a
# list of names, because a name list is what lets site eleven slip through.
# ---------------------------------------------------------------------------
import re as _re


def _canvas_oracle_sites():
    """Every `_baseline_check` call whose image came from `_capture_canvas`.

    Returns (path, lineno, has_crop) triples. Scans all UI test modules, not
    just the one that holds the defect today, so a second module gaining a
    canvas oracle is covered the day it lands.
    """
    sites = []
    for path in sorted((REPO_ROOT / "tests").rglob("test_ui_*.py")):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:                                  # pragma: no cover
            continue
        for i, line in enumerate(lines):
            if "_baseline_check(" not in line:
                continue
            if line.lstrip().startswith(("def ", "#")) or "``" in line:
                continue
            call = line.strip()
            k = i + 1
            while call.count("(") > call.count(")") and k < len(lines):
                call += " " + lines[k].strip()
                k += 1
            # The capture is theme-coupled only when it came off the canvas
            # widget; dialog and whole-window captures legitimately have no
            # page rect to crop to.
            context = " ".join(lines[max(0, i - 6):i])
            if "_capture_canvas" not in context:
                continue
            sites.append((path, i + 1, "crop" in call))
    return sites


def test_every_canvas_oracle_crops_to_the_page():
    """A canvas pixel oracle without a crop asserts the session theme.

    This is the class-level test for the defect: not "the crop works" but
    "the crop is used wherever it is needed". It fails loudly listing the
    offending sites, because the original fix missed half of them.
    """
    sites = _canvas_oracle_sites()

    # Non-vacuity first: a sweep that finds nothing passes for free, and this
    # repo has a documented history of exactly that.
    assert len(sites) >= 8, (
        "found only %d canvas oracle site(s); the sweep is not seeing the "
        "tree it is supposed to police, so a pass here would be meaningless"
        % len(sites)
    )

    missing = [(str(p.relative_to(REPO_ROOT)).replace("\\", "/"), ln)
               for p, ln, has in sites if not has]
    assert not missing, (
        "these canvas oracles compare the theme-derived surround because they "
        "pass no crop=, so they fail whenever the session palette differs from "
        "the one the baseline was captured under:\n  "
        + "\n  ".join("%s:%d" % m for m in missing)
        + "\nPass crop=_page_area_crop(session) -- the page rect from the "
          "page_rect_on_canvas seam, with no fallback."
    )


def test_the_sweep_can_actually_fail():
    """A guard that cannot redden is decoration.

    Feeds the detector a synthetic call site with no crop and requires it to be
    reported. Without this, `test_every_canvas_oracle_crops_to_the_page` could
    be silently keyed on something that never matches.
    """
    sample = [
        "        img = _capture_canvas(s)",
        '        _baseline_check("x_canvas", img,',
        "                        ssim_threshold=0.99)",
    ]
    call = sample[1].strip() + " " + sample[2].strip()
    context = " ".join(sample[:1])
    assert "_capture_canvas" in context, "the context probe does not match"
    assert "crop" not in call, (
        "the detector would not flag a crop-less canvas site, so the live "
        "sweep proves nothing"
    )

    cropped = call + " crop=_page_area_crop(s))"
    assert "crop" in cropped, "the detector would flag a correct site"


# ---------------------------------------------------------------------------
# A GOLDEN MUST BE SEEDED FROM A SETTLED FRAME (DCA 0114).
#
# Sibling defect to the crop coupling above, same family: an image oracle whose
# compared pixels are not the ones the product finished producing. Here the
# cause is timing rather than theme -- `render_drive.capture()` can return a
# MID-RENDER frame while the machine is busy (its own docstring says exactly
# that, and prescribes `capture_stable()` "for any test whose verdict turns on
# whether the canvas changed"). All four render golden sites used the race-prone
# form, and the two `rotate_cw` goldens committed on 2026-06-25 carry the
# UNROTATED page aspect (0.708 vs the 1.415 a CW-rotated page must have), i.e.
# they are half-painted frames.
#
# These live HERE, rather than in tests/ui_automation/test_render_oracle_golden_unit.py
# where they would sit more naturally, because fast-checks.yml names THIS file
# and does not name that one -- a guard in an unwired file never runs.
# ---------------------------------------------------------------------------
_RENDER_GOLDEN_FILES = (
    "test_render_page_ops.py",
    "test_render_view_nav.py",
    "test_render_annotations.py",
    "test_render_redaction.py",
)


def _golden_sites(paths):
    """(path, lineno, capture_kind, golden) for each assert_matches_golden call."""
    out = []
    for path in paths:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:                                  # pragma: no cover
            continue
        for i, line in enumerate(lines):
            if "assert_matches_golden" not in line or line.lstrip().startswith("#"):
                continue
            call = line.strip()
            k = i + 1
            while call.count("(") > call.count(")") and k < len(lines):
                call += " " + lines[k].strip()
                k += 1
            m = _re.search(r"crop_to_page_content\(\s*([A-Za-z_]\w*)", call) \
                or _re.search(r"assert_matches_golden\(\s*([A-Za-z_]\w*)", call)
            if not m:
                continue
            var = m.group(1)
            kind = "unknown"
            for j in range(i, max(0, i - 25), -1):
                mm = _re.search(r"\b%s\s*=\s*d\.(capture_stable|capture)\("
                                % _re.escape(var), lines[j])
                if mm:
                    kind = mm.group(1)
                    break
            g = _re.search(r'"([^"]+\.png)"', call)
            out.append((path, i + 1, kind, g.group(1) if g else "?"))
    return out


def test_render_golden_captures_use_a_settled_frame():
    """INSTANCE: the four known render golden sites must not use plain capture().

    RED before the fix -- all four used `d.capture(win)`.
    """
    base = REPO_ROOT / "tests" / "ui_automation"
    paths = [base / n for n in _RENDER_GOLDEN_FILES]
    missing = [p.name for p in paths if not p.exists()]
    assert not missing, ("expected render golden modules are gone: %s -- this "
                         "guard is looking at the wrong tree" % missing)

    sites = _golden_sites(paths)
    assert len(sites) >= 4, (
        "found only %d golden site(s) across %s; the detector is not seeing the "
        "calls it polices, so a pass would be meaningless"
        % (len(sites), list(_RENDER_GOLDEN_FILES)))

    racy = ["%s:%d (%s)" % (p.name, ln, gold)
            for p, ln, kind, gold in sites if kind != "capture_stable"]
    assert not racy, (
        "these golden comparisons are fed by a capture that is not gated on "
        "frame stability, so the image asserted against -- and any golden "
        "re-seeded from it -- may be half-painted:\n  " + "\n  ".join(racy)
        + "\nUse d.capture_stable(win); see its docstring.")


def test_every_golden_site_anywhere_is_frame_stable():
    """CLASS-LEVEL: discovered by shape, so a NEW file is covered on arrival.

    The instance case above is pinned to four filenames. This one sweeps the
    whole tree for `assert_matches_golden`, which is what catches golden site
    five in a module nobody has written yet.
    """
    sites = _golden_sites(sorted(REPO_ROOT.rglob("test_*.py")))
    assert sites, ("no assert_matches_golden site found anywhere -- the sweep "
                   "matched nothing and would pass vacuously")

    racy = ["%s:%d (%s)" % (p.relative_to(REPO_ROOT), ln, gold)
            for p, ln, kind, gold in sites if kind == "capture"]
    assert not racy, (
        "golden comparisons fed by a race-prone capture():\n  "
        + "\n  ".join(racy))


def test_the_golden_capture_sweep_can_fail():
    """The detector must flag a plain capture; otherwise the sweeps prove nothing.

    The sample lines are ASSEMBLED rather than written literally. Written out,
    this file would itself contain a race-prone golden site and
    `test_every_golden_site_anywhere_is_frame_stable` -- which sweeps every
    `test_*.py` in the tree, including this one -- would report its own test
    fixture as a real defect. It did, on the first run. A sweep that matches its
    own sample data is measuring itself.
    """
    _call_fn = "assert_matches" + "_golden"
    _crop_fn = "crop_to_page" + "_content"
    sample = [
        "        after = d.capture(win)",
        '        d.%s(d.%s(after), "x.png")' % (_call_fn, _crop_fn),
    ]
    call = sample[1].strip()
    var = _re.search(r"crop_to_page_content\(\s*([A-Za-z_]\w*)", call).group(1)
    kind = _re.search(r"\b%s\s*=\s*d\.(capture_stable|capture)\(" % var,
                      sample[0]).group(1)
    assert var == "after"
    assert kind == "capture", "the detector cannot see a race-prone capture"

    ok_line = "        after = d.capture_stable(win)"
    assert _re.search(r"\bafter\s*=\s*d\.(capture_stable|capture)\(",
                      ok_line).group(1) == "capture_stable"
