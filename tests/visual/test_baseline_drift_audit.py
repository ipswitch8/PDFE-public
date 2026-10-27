# -*- coding: utf-8 -*-
"""Tests for `audit_baseline_drift.py` — the #279 mismatch classifier.

Headless. The classifier decides whether a visual-baseline mismatch is a stale
baseline or a real regression, and phase-8's criterion 1 requires every mismatch
to land in one of those two buckets. Two of the five baselines are whole-window
or dialog captures whose colour is palette-derived end to end, so they are
judged on STRUCTURE instead — and that structural measure is the part most able
to be quietly wrong, because an edge-map comparison that was not actually
palette-invariant would silently reclassify a theme change as a regression (or
the reverse) while looking plausible.

So the calibration is asserted here rather than having been run once by hand.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

_MOD_PATH = REPO_ROOT / "tests" / "visual" / "audit_baseline_drift.py"
BASELINE_DIR = REPO_ROOT / "tests" / "fixtures" / "screenshots"

#: The two palette-derived captures the structural path exists for.
CHROME_BASELINES = ("startup_no_document", "encryption_dialog_open")


@pytest.fixture(scope="module")
def mod():
    spec = importlib.util.spec_from_file_location("abd_under_test", _MOD_PATH)
    m = importlib.util.module_from_spec(spec)
    sys.modules["abd_under_test"] = m
    spec.loader.exec_module(m)
    return m


def test_the_modules_own_self_test_passes():
    import subprocess
    r = subprocess.run([sys.executable, str(_MOD_PATH), "--self-test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-4000:]


@pytest.mark.parametrize("name", CHROME_BASELINES)
def test_structural_discriminator_is_palette_invariant(mod, name):
    """A theme change must not move the structural score; a layout change must.

    This is the property the whole chrome classification rests on. If it did not
    hold, `startup_no_document` and `encryption_dialog_open` would be classified
    by something that is really just measuring colour again — which is the
    original #279 defect wearing a different hat.
    """
    from PIL import Image, ImageChops, ImageOps
    import numpy as np

    path = BASELINE_DIR / f"{name}.png"
    if not path.is_file():
        pytest.fail(f"missing baseline {path}; the calibration cannot be "
                    f"checked, and a skip here would read as coverage")
    img = Image.open(str(path)).convert("RGB")

    identical = mod.structural_agreement(img, img)
    assert identical >= 0.999, (
        f"{name}: an image does not structurally agree with itself "
        f"(IoU={identical:.4f}); the measure is broken")

    # A PURE PALETTE CHANGE. Inverting every pixel is the most extreme recolour
    # possible and must leave the edge map untouched.
    inverted = mod.structural_agreement(img, ImageOps.invert(img))
    assert inverted >= mod.CHROME_SAME_LAYOUT_IOU, (
        f"{name}: inverting every pixel moved the structural score to "
        f"{inverted:.4f}, below the {mod.CHROME_SAME_LAYOUT_IOU} same-layout "
        f"threshold. The measure is NOT palette-invariant, so it cannot "
        f"distinguish a theme change from a regression — which is exactly the "
        f"#279 defect it was built to avoid.")

    darkened = mod.structural_agreement(img, Image.eval(img, lambda p: int(p * 0.45)))
    assert darkened >= mod.CHROME_SAME_LAYOUT_IOU, (
        f"{name}: a 55% darkening moved the structural score to "
        f"{darkened:.4f}, below the same-layout threshold")

    # LAYOUT CHANGES must fall below the threshold, or the measure cannot fail.
    shifted = mod.structural_agreement(img, ImageChops.offset(img, 12, 12))
    assert shifted < mod.CHROME_SAME_LAYOUT_IOU, (
        f"{name}: a 12px shift of the whole window still scored "
        f"{shifted:.4f}, at or above the same-layout threshold. A measure that "
        f"cannot see the layout move is not discriminating anything.")

    blanked_arr = np.array(img).copy()
    blanked_arr[: img.size[1] // 4, :] = 128
    blanked = mod.structural_agreement(img, Image.fromarray(blanked_arr))
    assert blanked < mod.CHROME_SAME_LAYOUT_IOU, (
        f"{name}: blanking the top 25% of the capture still scored "
        f"{blanked:.4f}, at or above the same-layout threshold. This is the "
        f"case that caught the first threshold (0.55): IoU over the union "
        f"credits the surviving area as agreement, so the measure is weaker "
        f"against content REMOVED than against content MOVED.")


def test_an_edgeless_pair_is_not_perfect_agreement(mod):
    """Two featureless images must not read as a perfect structural match.

    A blank capture is a known failure mode of this harness (wrong window,
    black rectangle). If `structural_agreement` returned 1.0 for two images
    with no edges at all, a pair of blank captures would classify as
    STALE-BASELINE — a false green built out of nothing.
    """
    from PIL import Image

    flat_a = Image.new("RGB", (64, 64), (10, 10, 10))
    flat_b = Image.new("RGB", (64, 64), (200, 200, 200))
    assert mod.structural_agreement(flat_a, flat_b) == 0.0
    assert mod.structural_agreement(flat_a, flat_a) == 0.0


def test_every_classification_lands_in_a_criterion_bucket(mod):
    """Phase-8 criterion 1 names exactly two outcomes for a mismatch.

    The first version of this classifier emitted a third, `CHROME-ONLY-ORACLE`,
    for captures it judged unjudgeable — and the phase-8 validator gate failed
    the phase for it, correctly: the criterion reads "classified as
    stale-baseline **or** real-regression", which is an exhaustive dichotomy.
    This pins that no such bucket can come back for a capture that HAS a
    structural measurement.
    """
    allowed = {"MATCHES", "STALE-BASELINE", "REGRESSION-CANDIDATE"}
    for kind in ("window", "dialog"):
        for iou in (0.0, 0.3, 0.6, 0.89, 0.90, 0.95, 1.0):
            got = mod.classify(kind, 0.3, 0.50, 0.30, None, None, False, iou)
            assert got in allowed, (
                f"kind={kind} iou={iou} produced {got!r}, which is not one of "
                f"the buckets criterion 1 provides for")
    # Only a genuinely absent measurement may be UNCLASSIFIABLE.
    assert mod.classify("window", 0.3, 0.50, 0.30, None, None, False,
                        None) == "UNCLASSIFIABLE"
