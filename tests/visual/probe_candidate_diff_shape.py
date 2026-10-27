# -*- coding: utf-8 -*-
"""GitHub #279 — is a REGRESSION-CANDIDATE a content change or a geometric one?

`audit_baseline_drift.py` classifies `zoom_in_200pct` and `annotations_visible`
as REGRESSION-CANDIDATE. Before either is regenerated — or filed as a rendering
defect — the diff needs a shape, because two very different causes produce a
"document differs" verdict:

* **content change** — differing pixels CONCENTRATED in a region (a glyph, an
  annotation, a colour), leaving the rest of the page identical;
* **geometric shift** — differing pixels SPREAD across the whole page, because
  a sub-pixel scroll/zoom/DPI difference moved everything slightly. SSIM is
  structural and collapses hard on a 1px shift while the above-threshold pixel
  count stays small, which is exactly `zoom_in_200pct`'s signature (pf=0.0088
  with ssim=0.7210).

Re-aligning the two images over a small offset search tells them apart: if a
shift materially reduces the differing-pixel count, the difference is geometric.
That is the same technique GitHub #284 used to *reject* a one-pixel-shift
hypothesis — there, re-aligning over dx,dy in [-3,+3] did not reduce the count
at all, which is what proved it was content. Applied here it can answer either
way, which is the point.

Usage:
    python tests/visual/probe_candidate_diff_shape.py          # live capture
    python tests/visual/probe_candidate_diff_shape.py --self-test
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

CANDIDATES = ("zoom_in_200pct", "annotations_visible")
SEARCH = range(-3, 4)
#: Per-channel difference above which a pixel counts as differing. Matches
#: diff.pixel_diff's default so this probe and the oracle agree on "differing".
PIXEL_TOL = 16


#: Page crops whose dimensions differ by more than this are not the same page
#: at the same scale, and a pixel comparison between them measures geometry
#: rather than content.
SCALE_MISMATCH_TOLERANCE = 0.02


def size_mismatch(a, b) -> float:
    """Largest relative dimension difference between two crops."""
    aw, ah = a.size
    bw, bh = b.size
    return max(abs(aw - bw) / max(1, max(aw, bw)),
               abs(ah - bh) / max(1, max(ah, bh)))


def to_common_size(a, b):
    """Resample the larger crop down to the smaller, as ssim/pixel_diff do.

    WITHOUT this, comparing a 1111px-wide crop against an 846px-wide one
    aligned at the origin compares *different parts of the page* and reports
    the resulting misalignment as content. The first version of this probe did
    exactly that and produced a confident "CONTENT" verdict for
    `zoom_in_200pct`, whose two crops differ in width by 31%.
    """
    from PIL import Image

    tw = min(a.size[0], b.size[0])
    th = min(a.size[1], b.size[1])
    ra = a if a.size == (tw, th) else a.resize((tw, th), Image.LANCZOS)
    rb = b if b.size == (tw, th) else b.resize((tw, th), Image.LANCZOS)
    return ra, rb


def differing_count(a, b, tol: int = PIXEL_TOL) -> int:
    import numpy as np

    aa = np.asarray(a.convert("RGB"), dtype=np.int16)
    bb = np.asarray(b.convert("RGB"), dtype=np.int16)
    h = min(aa.shape[0], bb.shape[0])
    w = min(aa.shape[1], bb.shape[1])
    d = np.abs(aa[:h, :w] - bb[:h, :w]).max(axis=-1)
    return int((d > tol).sum())


def best_alignment(a, b, search=SEARCH, tol: int = PIXEL_TOL):
    """(best_dx, best_dy, best_count, baseline_count) over an offset search."""
    import numpy as np

    aa = np.asarray(a.convert("RGB"), dtype=np.int16)
    bb = np.asarray(b.convert("RGB"), dtype=np.int16)
    h = min(aa.shape[0], bb.shape[0])
    w = min(aa.shape[1], bb.shape[1])
    aa = aa[:h, :w]
    bb = bb[:h, :w]

    base = int((np.abs(aa - bb).max(axis=-1) > tol).sum())
    best = (0, 0, base)
    pad = max(abs(min(search)), abs(max(search)))
    if h <= 2 * pad or w <= 2 * pad:
        return best + (base,)

    core = aa[pad:h - pad, pad:w - pad]
    for dy in search:
        for dx in search:
            shifted = bb[pad + dy:h - pad + dy, pad + dx:w - pad + dx]
            if shifted.shape != core.shape:
                continue
            n = int((np.abs(core - shifted).max(axis=-1) > tol).sum())
            # Scale to full-frame terms so it is comparable with `base`.
            n_full = int(n * (h * w) / max(1, core.shape[0] * core.shape[1]))
            if n_full < best[2]:
                best = (dx, dy, n_full)
    return best + (base,)


def spread(a, b, tol: int = PIXEL_TOL):
    """How concentrated are the differing pixels? Returns (rows_frac, cols_frac).

    Fraction of rows / columns that contain at least one differing pixel. A
    geometric shift touches nearly every row and column; a localised content
    change touches few.
    """
    import numpy as np

    aa = np.asarray(a.convert("RGB"), dtype=np.int16)
    bb = np.asarray(b.convert("RGB"), dtype=np.int16)
    h = min(aa.shape[0], bb.shape[0])
    w = min(aa.shape[1], bb.shape[1])
    d = np.abs(aa[:h, :w] - bb[:h, :w]).max(axis=-1) > tol
    if d.size == 0:
        return 0.0, 0.0
    return float(d.any(axis=1).mean()), float(d.any(axis=0).mean())


def interpret(base: int, best: int, rows_frac: float, cols_frac: float) -> str:
    reduction = 0.0 if base == 0 else 1.0 - (best / base)
    spread_high = rows_frac >= 0.5 and cols_frac >= 0.5
    if reduction >= 0.5:
        return ("GEOMETRIC — re-alignment removes %.0f%% of the differing "
                "pixels, so the content is the same picture in a different "
                "place" % (reduction * 100))
    if spread_high:
        return ("GEOMETRIC-OR-GLOBAL — alignment does not help (%.0f%%) but the "
                "diff touches %.0f%% of rows and %.0f%% of columns, so it is a "
                "global change (scale, colour, AA) rather than a local edit"
                % (reduction * 100, rows_frac * 100, cols_frac * 100))
    return ("CONTENT — alignment does not help (%.0f%%) and the diff is "
            "confined to %.0f%% of rows / %.0f%% of columns, so something in "
            "the document itself rendered differently"
            % (reduction * 100, rows_frac * 100, cols_frac * 100))


def _self_test() -> int:
    from PIL import Image
    import numpy as np

    failures = 0

    # A pure 2px shift of real-ish content must read GEOMETRIC.
    rng = np.random.RandomState(7)
    base_img = np.full((120, 160, 3), 255, dtype=np.uint8)
    base_img[30:90, 40:120] = rng.randint(0, 80, size=(60, 80, 3))
    a = Image.fromarray(base_img)
    shifted = np.full_like(base_img, 255)
    shifted[32:92, 42:122] = base_img[30:90, 40:120]
    b = Image.fromarray(shifted)
    dx, dy, best, base = best_alignment(a, b)
    rf, cf = spread(a, b)
    verdict = interpret(base, best, rf, cf)
    if not verdict.startswith("GEOMETRIC —"):
        failures += 1
        print("FAIL pure 2px shift should read GEOMETRIC, got: %s" % verdict)
    else:
        print("ok   pure 2px shift -> GEOMETRIC (dx=%d dy=%d %d->%d)"
              % (dx, dy, base, best))

    # A small local blob change must read CONTENT.
    c = base_img.copy()
    c[50:58, 60:68] = 0
    dx, dy, best, base = best_alignment(a, Image.fromarray(c))
    rf, cf = spread(a, Image.fromarray(c))
    verdict = interpret(base, best, rf, cf)
    if not verdict.startswith("CONTENT"):
        failures += 1
        print("FAIL local blob should read CONTENT, got: %s" % verdict)
    else:
        print("ok   local blob -> CONTENT (rows=%.2f cols=%.2f)" % (rf, cf))

    # The flaw the first version of this probe had: different-sized crops
    # compared at the origin, which measures misalignment and calls it content.
    # size_mismatch must flag it; to_common_size must equalise it.
    wide = Image.fromarray(
        np.kron(base_img, np.ones((1, 2, 1), dtype=np.uint8)).astype(np.uint8))
    mm = size_mismatch(a, wide)
    if mm <= SCALE_MISMATCH_TOLERANCE:
        failures += 1
        print("FAIL a 2x-wide crop should flag a scale mismatch, got %.3f" % mm)
    else:
        print("ok   2x-wide crop flagged as scale mismatch (%.2f)" % mm)
    ra, rb = to_common_size(a, wide)
    if ra.size != rb.size:
        failures += 1
        print("FAIL to_common_size left %r vs %r" % (ra.size, rb.size))
    else:
        print("ok   to_common_size equalises to %r" % (ra.size,))

    # Identical images: nothing differs, and that must not crash or claim a shift.
    dx, dy, best, base = best_alignment(a, a)
    if base != 0 or best != 0:
        failures += 1
        print("FAIL identical images report %d/%d differing" % (base, best))
    else:
        print("ok   identical images -> 0 differing")

    print("\nself-test: %d failure(s)" % failures)
    return 1 if failures else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return _self_test()

    if not os.environ.get("SESSIONNAME"):
        print("ERROR: no interactive desktop session", file=sys.stderr)
        return 2

    from PIL import Image
    from tests.visual.diff import crop_to_page_content
    from tests.visual.test_visual_baselines import BASELINE_PLAN, BASELINE_DIR

    plan = dict(BASELINE_PLAN)
    for name in CANDIDATES:
        bp = BASELINE_DIR / f"{name}.png"
        if not bp.is_file():
            print("%s: no baseline" % name)
            continue
        actual = plan[name]()
        baseline = Image.open(str(bp)).convert("RGB")
        a = crop_to_page_content(actual)
        b = crop_to_page_content(baseline)

        print("\n=== %s ===" % name)
        print("  page crops: actual=%s baseline=%s" % (a.size, b.size))

        mismatch = size_mismatch(a, b)
        print("  crop size mismatch: %.1f%% of the larger dimension "
              "(actual %dx%d vs baseline %dx%d; actual is %.1f%% wider than "
              "baseline)"
              % (mismatch * 100, a.size[0], a.size[1], b.size[0], b.size[1],
                 100.0 * (a.size[0] - b.size[0]) / max(1, b.size[0])))
        if mismatch > SCALE_MISMATCH_TOLERANCE:
            print("  => SCALE/GEOMETRY. The page is rendered at a materially "
                  "different pixel size in the two captures, so the baseline "
                  "was taken at a different canvas width or zoom. A content "
                  "comparison first requires resampling, which itself "
                  "perturbs every edge pixel -- so this baseline is not "
                  "reproducible rather than being a rendering regression.")

        # Compare at a common size regardless, so the number is about content
        # and not about the size difference just reported.
        ra, rb = to_common_size(a, b)
        dx, dy, best, base = best_alignment(ra, rb)
        rf, cf = spread(ra, rb)
        print("  after resampling to %s:" % (ra.size,))
        print("    differing pixels: %d -> %d at (dx=%d, dy=%d)"
              % (base, best, dx, dy))
        print("    diff touches rows=%.1f%% cols=%.1f%%" % (rf * 100, cf * 100))
        print("    => %s" % interpret(base, best, rf, cf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
