# -*- coding: utf-8 -*-
"""GitHub #279 — classify each visual-baseline mismatch: STALE or REGRESSION?

WHY THIS EXISTS
===============
`tests/visual/test_visual_baselines.py`'s five baseline comparisons all carry a
blanket `xfail(strict=False)` whose reason asserts:

    "baseline drift from rendering / Qt theme / OS DPI variations since
     baselines were captured. SSIM 0.27-0.67 vs 0.95 threshold.
     NOT a regression from any product pipeline."

**That last sentence is a claim, not a measurement.** It is exactly the claim
#279 says cannot be made from an SSIM number: `PdfCanvas::paintEvent` fills the
viewport backdrop from `palette().color(QPalette::Dark)`, so the surround's
colour is a property of the *session theme*. A theme-derived diff and a genuine
rendering regression both present as a collapsed SSIM over the whole frame.
Telling them apart requires asking **where** the pixels differ, not how many.

And because the marker is `strict=False`, a passing oracle is an XPASS rather
than a signal — so these five cannot redden *or* announce that they have
started matching. They assert nothing in either direction.

THE DISCRIMINATOR
=================
`diff.crop_to_page_content()` isolates the rendered white page block from the
palette-derived surround and toolbar. So for each baseline:

* compare **full frame**  -> reproduces the number the xfail marker quotes;
* compare **page content only** -> asks whether the *document* changed.

Full frame differs while the page is identical  => STALE-BASELINE (theme/chrome).
Page content differs as well                    => REGRESSION-CANDIDATE.

THE TRAP THIS AVOIDS, STATED BECAUSE IT WOULD BE SILENT
=======================================================
`crop_to_page_content` returns the image **UNCHANGED** when it finds no clear
page block — a deliberate "never worse than no crop" fallback. If that happens,
the "page-content" comparison is byte-for-byte the same computation as the
full-frame one, and classifying on it would be circular.

**A no-op crop has two OPPOSITE causes, and the first version of this file
conflated them.** Measured on the committed baselines:

    zoom_in_200pct          near_white=0.983  crop_kept=0.991   <- all page
    encryption_dialog_open  near_white=0.042  crop_kept=1.000   <- no page

At 200% zoom the page *fills the viewport*, so there is no grey margin to crop
away and the full-frame comparison already **is** a page comparison. A dialog
capture has no page at all. Same symptom, opposite meaning — calling both
"UNCLASSIFIABLE" is wrong in the first case and calling both classifiable is
wrong in the second. So the near-white fraction is measured and used to tell
them apart.

The first version also set the "did the crop localise anything" bar at 2% of
frame area, which was far too lenient: `startup_no_document` cropped just 11.7%
onto a white block that is not a page (no document is open), and that bogus
localisation produced a REGRESSION-CANDIDATE verdict for a capture containing no
document to regress. The bar is now meaningful, and the capture's KIND is a
declared fact taken from the plan rather than inferred from pixels.

This is the same failure this project has already paid for twice: an oracle that
measured the grey margin instead of its own region, and a crop that missed the
region it was named for.

Usage
-----
    python tests/visual/audit_baseline_drift.py            # audit, write JSON
    python tests/visual/audit_baseline_drift.py --self-test

Requires an interactive desktop: it launches PDFE to capture each view.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

#: A page-content diff at or below this is "the document is unchanged".
#: Deliberately not 0: resizing two page crops to common dimensions (which
#: `ssim`/`pixel_diff` do, so residual fit-zoom is a pure scale) resamples, and
#: resampling perturbs edge pixels. Measured noise floor on identical content is
#: well under 1%.
PAGE_IDENTICAL_PIXEL_FRACTION = 0.01

#: ...and the page SSIM must still be high. Both conditions, because a small
#: pixel fraction with a collapsed SSIM would mean a few pixels changed a lot.
PAGE_IDENTICAL_SSIM = 0.95

#: A crop must remove a real amount of frame to count as having separated page
#: from chrome. The first version used 0.02, which let `startup_no_document`'s
#: 11.7% crop onto a non-page white block count as a successful localisation.
CROP_MUST_REMOVE_AREA_FRACTION = 0.15

#: At or above this near-white fraction the page FILLS the frame: there is no
#: chrome in the capture, so the full-frame comparison already is a page
#: comparison. Measured: zoom_in_200pct is 0.983.
PAGE_FILLS_FRAME_WHITE_FRACTION = 0.95

#: Below this near-white fraction a *canvas* capture contains no page block
#: worth cropping to. Measured: encryption_dialog_open is 0.042 (a dialog),
#: startup_no_document is 0.319 (no document open), while the three genuine
#: page-bearing canvas captures are 0.657-0.983.
PAGE_PRESENT_MIN_WHITE_FRACTION = 0.50

#: Which capture each baseline is. A DECLARED FACT read off the plan in
#: test_visual_baselines.py, not inferred from the pixels:
#:   _capture_window_image  -> the whole PDFE main window
#:   _capture_canvas_image  -> the PdfCanvas widget area
#: A window or dialog capture is palette-derived chrome essentially end to end,
#: so "did the document change" is not a question that can be asked of it.
CAPTURE_KIND = {
    "startup_no_document": "window",   # _capture_window_image, no document open
    "document_open_page1": "canvas",
    "zoom_in_200pct": "canvas",
    "annotations_visible": "canvas",
    "encryption_dialog_open": "dialog",  # the dialog itself
}


def _area(img) -> int:
    w, h = img.size
    return w * h


def _identical(ssim_val, pf) -> bool:
    """Both conditions. A small pixel fraction with a collapsed SSIM means a
    few pixels changed a great deal, which is not 'identical'."""
    return (ssim_val is not None and pf is not None
            and ssim_val >= PAGE_IDENTICAL_SSIM
            and pf <= PAGE_IDENTICAL_PIXEL_FRACTION)


def classify(kind: str, white_fraction: float,
             full_ssim: float, full_pf: float,
             page_ssim, page_pf, cropped_ok: bool,
             structure_iou=None) -> str:
    """Pure classification, unit-testable without a display.

    ``kind`` is the declared capture kind ("canvas" / "window" / "dialog").
    ``page_ssim``/``page_pf`` may be None when no page comparison was possible.
    ``structure_iou`` is the palette-invariant edge-map agreement, required for
    a window/dialog capture.

    EVERY MISMATCH LANDS IN ONE OF THE TWO BUCKETS phase-8's criterion 1 names:

      MATCHES              the baseline already agrees with the current capture
      STALE-BASELINE       the baseline is out of date -- for a canvas capture
                           the DOCUMENT is identical and only chrome moved; for
                           a chrome capture the LAYOUT is identical and only
                           palette-derived colour moved
      REGRESSION-CANDIDATE the thing the oracle defends actually changed
      UNCLASSIFIABLE       retained only for a canvas capture in which no page
                           could be located at all, which is a broken input
                           rather than a judgement

    The earlier `CHROME-ONLY-ORACLE` verdict is GONE. It was a third bucket for
    "cannot be judged", and phase-8's validator gate correctly failed the phase
    for it: criterion 1 says "classified as stale-baseline **or**
    real-regression", and that is an exhaustive dichotomy. A chrome capture IS
    judgeable -- not from colour, but from structure. See `edge_map`.
    """
    if _identical(full_ssim, full_pf):
        return "MATCHES"

    if kind in ("window", "dialog"):
        # Palette-derived chrome end to end, so colour says nothing. Ask whether
        # the LAYOUT moved. Same layout + different colour = the baseline was
        # captured under another theme, i.e. stale.
        if structure_iou is None:
            return "UNCLASSIFIABLE"
        return ("STALE-BASELINE" if structure_iou >= CHROME_SAME_LAYOUT_IOU
                else "REGRESSION-CANDIDATE")

    # Canvas capture. The page fills the frame -> the full-frame comparison IS
    # the page comparison, and the absent crop is expected rather than a gap.
    if white_fraction >= PAGE_FILLS_FRAME_WHITE_FRACTION:
        return "REGRESSION-CANDIDATE"

    if white_fraction < PAGE_PRESENT_MIN_WHITE_FRACTION:
        return "UNCLASSIFIABLE"

    if not cropped_ok:
        return "UNCLASSIFIABLE"

    return "STALE-BASELINE" if _identical(page_ssim, page_pf) \
        else "REGRESSION-CANDIDATE"


def edge_map(img, rel_threshold: float = 0.25):
    """A binary edge map, normalised so a global palette change does not move it.

    Why this exists (GitHub #279, phase-8 validator FAIL): criterion 1 requires
    EVERY mismatch classified as stale-baseline or real-regression. A whole-window
    or dialog capture is palette-derived chrome end to end, so the page-crop
    discriminator has nothing to work with -- and the first version of this
    auditor answered "cannot be judged", which is a third bucket the criterion
    does not provide for. The validator was right to refuse it.

    But the question IS answerable, just not from colour. For a chrome capture:

      * if the widget LAYOUT is unchanged and only palette-derived colours
        differ, the baseline was captured under a different theme -> STALE;
      * if edges have MOVED, appeared or vanished, the window itself changed
        -> a real regression.

    So compare STRUCTURE. A Sobel magnitude thresholded RELATIVE to its own
    maximum is invariant to the affine lightness/contrast change a palette swap
    applies: inverting or re-shading a flat region leaves its boundaries where
    they were. That is the property being exploited, and it is why an absolute
    threshold would not do.
    """
    import numpy as np

    g = np.asarray(img.convert("L"), dtype=np.float64)
    if g.size == 0 or g.shape[0] < 3 or g.shape[1] < 3:
        return np.zeros_like(g, dtype=bool)
    gx = np.zeros_like(g)
    gy = np.zeros_like(g)
    # Central differences: a 3x3 Sobel without a scipy dependency.
    gx[:, 1:-1] = g[:, 2:] - g[:, :-2]
    gy[1:-1, :] = g[2:, :] - g[:-2, :]
    mag = np.hypot(gx, gy)
    peak = float(mag.max())
    if peak <= 0:
        return np.zeros_like(g, dtype=bool)
    return mag >= (rel_threshold * peak)


def structural_agreement(a, b) -> float:
    """Intersection-over-union of two edge maps, resampled to a common size.

    1.0 means the two captures have their edges in the same places -- same
    layout, whatever the colours. Near 0 means the structure moved.
    """
    import numpy as np
    from PIL import Image

    tw = min(a.size[0], b.size[0])
    th = min(a.size[1], b.size[1])
    if tw < 3 or th < 3:
        return 0.0
    ra = a if a.size == (tw, th) else a.resize((tw, th), Image.LANCZOS)
    rb = b if b.size == (tw, th) else b.resize((tw, th), Image.LANCZOS)
    ea = edge_map(ra)
    eb = edge_map(rb)
    union = np.logical_or(ea, eb).sum()
    if union == 0:
        # Neither capture has any edge at all. That is not agreement; it is two
        # featureless images, and calling it 1.0 would read a blank capture as a
        # perfect structural match.
        return 0.0
    return float(np.logical_and(ea, eb).sum()) / float(union)


#: Edge-map IoU at or above which two chrome captures are the SAME LAYOUT, so a
#: mismatch is attributable to the palette rather than to the product.
#:
#: CALIBRATED ON THE REAL BASELINES against deliberately-broken controls, not
#: guessed. Measured on `startup_no_document` (1584x961) and
#: `encryption_dialog_open` (466x639):
#:
#:     transformation                    startup   dialog
#:     identical                          1.0000   1.0000
#:     inverted (a pure palette change)   1.0000   1.0000
#:     darkened 55% (ditto)               0.9979   0.9949
#:     top 25% blanked (layout change)    0.4528   0.6447
#:     shifted 12px (layout change)       0.0701   0.0617
#:
#: The invariance is the point: inverting every pixel leaves the edge map
#: IDENTICAL, so a theme swap cannot move this number, while moving or removing
#: widgets does.
#:
#: The first value tried was 0.55, chosen off the shifted-12px controls alone.
#: The blanked-25% control exposed it as a FALSE NEGATIVE: the dialog scored
#: 0.6447 with a quarter of it erased, and would have been called "stale". IoU
#: over the union still credits the surviving 75% as agreement, so this measure
#: is inherently weaker against content REMOVED than against content MOVED.
#: 0.90 separates all three classes with the palette controls still at 0.9949+.
#:
#: `test_structural_discriminator_is_palette_invariant` in
#: tests/visual/test_baseline_drift_audit.py asserts this table, so the
#: calibration is checked on every run rather than having been done once.
CHROME_SAME_LAYOUT_IOU = 0.90


def near_white_fraction(img) -> float:
    """Fraction of pixels that are near-white, i.e. page rather than chrome.

    Uses the same 230 threshold `crop_to_page_content` uses, so this measurement
    and that crop cannot disagree about what counts as page.
    """
    import numpy as np

    arr = np.asarray(img.convert("RGB"), dtype=np.int16)
    if arr.size == 0:
        return 0.0
    return float((arr >= 230).all(axis=-1).mean())


def audit_one(name: str, actual, baseline, kind: str = None) -> dict:
    """Measure and classify one baseline. Pure given two images."""
    from tests.visual.diff import compare, crop_to_page_content

    if kind is None:
        kind = CAPTURE_KIND.get(name, "canvas")

    full = compare(actual, baseline, ssim_threshold=PAGE_IDENTICAL_SSIM,
                   pixel_threshold=PAGE_IDENTICAL_PIXEL_FRACTION)

    # The near-white fraction is taken from the BASELINE, because that is the
    # committed artefact whose structure we are reasoning about, and it does not
    # move run to run.
    white = near_white_fraction(baseline)

    a_crop = crop_to_page_content(actual)
    b_crop = crop_to_page_content(baseline)

    # Did the crop actually separate page from chrome, on BOTH sides? A no-op
    # crop makes the page comparison identical to the full-frame one -- see the
    # module docstring for why that has two opposite causes.
    a_removed = 1.0 - (_area(a_crop) / max(1, _area(actual)))
    b_removed = 1.0 - (_area(b_crop) / max(1, _area(baseline)))
    cropped_ok = (a_removed >= CROP_MUST_REMOVE_AREA_FRACTION
                  and b_removed >= CROP_MUST_REMOVE_AREA_FRACTION)

    page = None
    if cropped_ok:
        page = compare(a_crop, b_crop, ssim_threshold=PAGE_IDENTICAL_SSIM,
                       pixel_threshold=PAGE_IDENTICAL_PIXEL_FRACTION)

    # Palette-invariant structure, used to judge chrome captures. Computed for
    # every capture so the number is on the record either way.
    s_iou = structural_agreement(actual, baseline)

    verdict = classify(kind, white,
                       full["ssim"], full["pixel_fraction"],
                       None if page is None else page["ssim"],
                       None if page is None else page["pixel_fraction"],
                       cropped_ok, s_iou)

    return {
        "name": name,
        "kind": kind,
        "baseline_near_white_fraction": round(white, 4),
        "structure_edge_iou": round(s_iou, 4),
        "structure_same_layout_threshold": CHROME_SAME_LAYOUT_IOU,
        "actual_size": list(actual.size),
        "baseline_size": list(baseline.size),
        "full_ssim": round(float(full["ssim"]), 4),
        "full_pixel_fraction": round(float(full["pixel_fraction"]), 6),
        "crop_localised": cropped_ok,
        "actual_crop_size": list(a_crop.size),
        "baseline_crop_size": list(b_crop.size),
        "actual_area_removed": round(a_removed, 4),
        "baseline_area_removed": round(b_removed, 4),
        "page_ssim": None if page is None else round(float(page["ssim"]), 4),
        "page_pixel_fraction": (None if page is None
                                else round(float(page["pixel_fraction"]), 6)),
        "verdict": verdict,
    }


def run_audit(out_path: Path) -> int:
    from PIL import Image
    from tests.visual.test_visual_baselines import BASELINE_PLAN, BASELINE_DIR

    rows = []
    for name, capture_fn in BASELINE_PLAN:
        baseline_path = BASELINE_DIR / f"{name}.png"
        if not baseline_path.is_file():
            rows.append({"name": name, "verdict": "NO-BASELINE",
                         "note": f"no committed baseline at {baseline_path}"})
            print(f"{name:26s} NO-BASELINE", flush=True)
            continue
        try:
            actual = capture_fn()
        except Exception as exc:  # noqa: BLE001
            rows.append({"name": name, "verdict": "CAPTURE-FAILED",
                         "note": repr(exc)})
            print(f"{name:26s} CAPTURE-FAILED {exc!r}", flush=True)
            continue
        baseline = Image.open(str(baseline_path)).convert("RGB")
        row = audit_one(name, actual, baseline)
        rows.append(row)
        print("%-26s %-21s full(ssim=%.4f pf=%.4f) page(%s) crop_localised=%s"
              % (name, row["verdict"], row["full_ssim"],
                 row["full_pixel_fraction"],
                 "n/a" if row["page_ssim"] is None
                 else "ssim=%.4f pf=%.4f" % (row["page_ssim"],
                                             row["page_pixel_fraction"]),
                 row["crop_localised"]), flush=True)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    io.open(out_path, "w", encoding="utf-8").write(
        json.dumps({"baselines": rows}, indent=2) + "\n")
    print(f"\nwrote {out_path}", flush=True)

    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("summary: " + ", ".join("%s=%d" % kv for kv in sorted(counts.items())))
    return 0


def _self_test() -> int:
    """The classification logic, exercised without a display."""
    W_PAGE = 0.67       # a page-bearing canvas capture (measured 0.657-0.671)
    W_FILLS = 0.983     # zoom_in_200pct: the page fills the frame
    W_NONE = 0.042      # encryption_dialog_open: no page at all

    cases = [
        # (label, kind, white, full_ssim, full_pf, page_ssim, page_pf,
        #  cropped_ok, expected)
        ("already matching full-frame", "canvas", W_PAGE, 0.99, 0.001,
         None, None, False, "MATCHES"),
        ("chrome-only diff, page clean", "canvas", W_PAGE, 0.40, 0.50,
         0.99, 0.0005, True, "STALE-BASELINE"),
        ("page differs too", "canvas", W_PAGE, 0.40, 0.50, 0.60, 0.20, True,
         "REGRESSION-CANDIDATE"),
        ("page pf ok but ssim collapsed", "canvas", W_PAGE, 0.40, 0.50,
         0.50, 0.001, True, "REGRESSION-CANDIDATE"),
        ("page ssim ok but pf high", "canvas", W_PAGE, 0.40, 0.50,
         0.99, 0.30, True, "REGRESSION-CANDIDATE"),
        ("borderline: exactly at thresholds is identical", "canvas", W_PAGE,
         0.40, 0.50, PAGE_IDENTICAL_SSIM, PAGE_IDENTICAL_PIXEL_FRACTION, True,
         "STALE-BASELINE"),

        # THE TWO OPPOSITE CAUSES OF A NO-OP CROP, which the first version
        # of this classifier conflated.
        ("page FILLS frame -> full frame IS the page", "canvas", W_FILLS,
         0.72, 0.0088, None, None, False, "REGRESSION-CANDIDATE"),
        ("no page in a canvas capture -> unclassifiable", "canvas", W_NONE,
         0.72, 0.30, None, None, False, "UNCLASSIFIABLE"),

        # A whole-window / dialog capture is judged on STRUCTURE, because its
        # colour is palette-derived. Same layout -> the baseline is stale.
        ("window, same layout -> STALE", "window", 0.319, 0.5653, 0.2849,
         None, None, False, "STALE-BASELINE", 1.0000),   # measured: invert -> 1.0000
        ("dialog, same layout -> STALE", "dialog", W_NONE, 0.8951, 0.0157,
         None, None, False, "STALE-BASELINE", 0.9949),  # measured: darkened -> 0.9949
        # ...and a layout that MOVED is a real regression, not a theme change.
        ("window, layout moved -> REGRESSION", "window", 0.319, 0.5653, 0.2849,
         None, None, False, "REGRESSION-CANDIDATE", 0.0701),  # measured: 12px shift
        ("dialog, layout moved -> REGRESSION", "dialog", W_NONE, 0.8951,
         0.0157, None, None, False, "REGRESSION-CANDIDATE", 0.6447),  # measured: 25% blanked
        # Exactly at the threshold counts as the same layout.
        ("window, exactly at the layout threshold -> STALE", "window", 0.319,
         0.5653, 0.2849, None, None, False, "STALE-BASELINE",
         CHROME_SAME_LAYOUT_IOU),
        # No structure measurement at all must not silently become a verdict.
        ("window with no structure measurement", "window", 0.319, 0.5653,
         0.2849, None, None, False, "UNCLASSIFIABLE", None),
        # ...but a window capture that AGREES on pixels is still a match.
        ("matching window capture is MATCHES", "window",
         0.319, 0.99, 0.001, None, None, False, "MATCHES", 0.99),

        # The circularity guard: page numbers handed in alongside a failed crop
        # ARE the full-frame numbers and must not be trusted.
        ("no-op crop with page numbers present", "canvas", W_PAGE, 0.40, 0.50,
         0.40, 0.50, False, "UNCLASSIFIABLE"),

        # The regression that motivated raising the crop bar: startup_no_document
        # cropped 11.7% onto a non-page white block and was called a regression.
        ("11.7% crop must not localise", "canvas", 0.319, 0.5653, 0.2849,
         0.6867, 0.2005, False, "UNCLASSIFIABLE"),
    ]
    failures = 0
    for case in cases:
        # Cases carrying a 10th element supply structure_iou; the rest are
        # canvas cases for which it is irrelevant.
        if len(case) == 10:
            label, kind, white, fs, fp, ps, pp, ok, expected, siou = case
        else:
            label, kind, white, fs, fp, ps, pp, ok, expected = case
            siou = 1.0
        got = classify(kind, white, fs, fp, ps, pp, ok, siou)
        if got != expected:
            failures += 1
            print("FAIL %-48s expected %-21s got %s" % (label, expected, got))
        else:
            print("ok   %-48s %s" % (label, expected))

    # A MATCHES verdict must not depend on the crop having worked.
    if classify("canvas", W_PAGE, 0.99, 0.0, None, None, False,
                1.0) != "MATCHES":
        failures += 1
        print("FAIL a full-frame match must not require a successful crop")
    else:
        print("ok   a full-frame match does not require a successful crop")

    # Every name in the plan must have a declared kind, or it silently defaults.
    try:
        from tests.visual.test_visual_baselines import BASELINE_PLAN
        planned = {n for n, _ in BASELINE_PLAN}
    except Exception:  # noqa: BLE001 — needs pywinauto; skip that cross-check
        planned = set()
    if planned:
        undeclared = sorted(planned - set(CAPTURE_KIND))
        if undeclared:
            failures += 1
            print("FAIL baselines with no declared CAPTURE_KIND: %r "
                  "(they would silently default to 'canvas')" % (undeclared,))
        else:
            print("ok   every planned baseline has a declared CAPTURE_KIND")

    print("\nself-test: %d failure(s)" % failures)
    return 1 if failures else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--out", default=str(
        REPO_ROOT / "docs" / "audit" / "p97-phase8" / "i279-baseline-audit.json"))
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    if not os.environ.get("SESSIONNAME"):
        print("ERROR: no interactive desktop session; this audit launches PDFE "
              "and captures real windows. Refusing to report anything rather "
              "than producing an audit of nothing.", file=sys.stderr)
        return 2
    return run_audit(Path(args.out))


if __name__ == "__main__":
    sys.exit(main())
