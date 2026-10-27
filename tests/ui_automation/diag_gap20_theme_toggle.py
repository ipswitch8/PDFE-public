# -*- coding: utf-8 -*-
"""DIAGNOSTIC (throwaway): characterise the gap20 theme-toggle XPASS flake.

gap20 (test_render_view_nav.py::test_theme_toggle_does_not_repaint_canvas_gap20)
is a strict-xfail asserting the canvas does NOT repaint on theme toggle (the
canvas is theme-independent). It xfails deterministically in isolation but
XPASSes (-> strict failure) intermittently inside the full render sweep. This
script reproduces the load and, on EVERY iteration, records the exact metrics
the test's oracle uses (SSIM, pixel-diff fraction at thr=10) PLUS where the diff
sits, so we can decide whether a tripping instance is a transient capture
artifact (diff localised in the page-content region = initial render completing
during the racy `before` capture) or a real intermittent theme repaint
(diff spread across the gray viewport margin / whole canvas).

NOT a pytest. Run via the Bash tool:
  PDFE_BUILD_CONFIG=Release PDFE_TEST_PIPES=1 python tests/ui_automation/diag_gap20_theme_toggle.py [iterations]

Artifacts -> scratchpad/gap20_diag/  (before/after/diff PNGs for tripping runs,
plus a metrics.csv for every run).
"""
from __future__ import annotations

import os
import sys
import time
import pathlib

os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")
os.environ.setdefault("PDFE_TEST_PIPES", "1")

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness import render_drive as d   # noqa: E402
from tests.ui_automation.harness import render_oracle as ro  # noqa: E402
from tests.visual.diff import ssim as _ssim, pixel_diff as _pixel_diff  # noqa: E402

OUT = pathlib.Path(
    os.environ["LOCALAPPDATA"]
) / "Temp" / "2" / "claude" / "C--Users-dev-source-PDFE" / \
    "c7b7bd44-c552-494c-8d0b-711d5c4d2873" / "scratchpad" / "gap20_diag"
OUT.mkdir(parents=True, exist_ok=True)


def _np():
    import numpy as n
    return n


def band_breakdown(before, after):
    """Fraction of differing pixels (thr 10) in each of three horizontal bands
    and three vertical bands, to localise WHERE the change is.

    A real theme repaint changes the gray viewport MARGIN (the outer bands,
    around the white page). An initial-render race changes the CENTRE (page
    content). Returns a dict of band -> diff fraction."""
    np = _np()
    a = np.asarray(before.convert("RGB"), dtype=np.int16)
    b = np.asarray(after.convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return {"shape_mismatch": f"{a.shape} vs {b.shape}"}
    mask = np.abs(a - b).max(axis=-1) > 10
    h, w = mask.shape
    out = {}
    yth = [0, h // 3, 2 * h // 3, h]
    for i, name in enumerate(("top", "mid", "bot")):
        out[f"row_{name}"] = round(float(mask[yth[i]:yth[i + 1], :].mean()), 6)
    xth = [0, w // 3, 2 * w // 3, w]
    for i, name in enumerate(("left", "cmid", "right")):
        out[f"col_{name}"] = round(float(mask[:, xth[i]:xth[i + 1]].mean()), 6)
    # Outer frame (margin) vs inner 60% (page content) split.
    iy0, iy1 = int(h * 0.2), int(h * 0.8)
    ix0, ix1 = int(w * 0.2), int(w * 0.8)
    inner = mask[iy0:iy1, ix0:ix1]
    inner_count = int(inner.sum())
    total_count = int(mask.sum())
    out["inner_frac_of_total"] = round(
        inner_count / total_count, 4) if total_count else 0.0
    out["margin_frac_of_total"] = round(
        (total_count - inner_count) / total_count, 4) if total_count else 0.0
    return out


def one_iteration(idx: int):
    with d.app_session(d.MULTISPAN) as win:
        before = d.capture(win)               # the racy plain capture
        before_stable = d.capture_stable(win)  # what the committed fix uses
        d.activate(win, "themeToggleAction", settle_s=1.0)
        after = d.capture(win)
        after_stable = d.capture_stable(win)

    # Metrics on BOTH the plain (racy) and stable capture pairs.
    s_plain = _ssim(before, after)
    f_plain, vis_plain = _pixel_diff(before, after, threshold_per_pixel=10)
    s_stab = _ssim(before_stable, after_stable)
    f_stab, _vis_stab = _pixel_diff(before_stable, after_stable,
                                    threshold_per_pixel=10)

    changed_plain = (s_plain < ro.SSIM_CHANGED_THRESHOLD) or \
        (f_plain >= ro.PIXEL_DIFF_THRESHOLD)
    changed_stab = (s_stab < ro.SSIM_CHANGED_THRESHOLD) or \
        (f_stab >= ro.PIXEL_DIFF_THRESHOLD)
    bands = band_breakdown(before, after)

    rec = {
        "iter": idx,
        "ssim_plain": round(s_plain, 5),
        "frac_plain": round(f_plain, 6),
        "changed_plain(XPASS)": changed_plain,
        "ssim_stable": round(s_stab, 5),
        "frac_stable": round(f_stab, 6),
        "changed_stable": changed_stab,
        **bands,
    }
    # Dump artifacts whenever EITHER pair "changed" (a tripping instance).
    if changed_plain or changed_stab:
        before.save(OUT / f"trip{idx:03d}_before_plain.png")
        after.save(OUT / f"trip{idx:03d}_after_plain.png")
        vis_plain.save(OUT / f"trip{idx:03d}_diff_plain.png")
        before_stable.save(OUT / f"trip{idx:03d}_before_stable.png")
        after_stable.save(OUT / f"trip{idx:03d}_after_stable.png")
    return rec


def main():
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    csv = OUT / "metrics.csv"
    rows = []
    trips_plain = trips_stable = 0
    print(f"gap20 diagnostic: {iters} iterations -> {OUT}")
    for i in range(1, iters + 1):
        try:
            rec = one_iteration(i)
        except BaseException as exc:  # noqa: BLE001  (incl. pytest.skip's Skipped)
            print(f"  iter {i}: SKIP/ERROR {type(exc).__name__}: {exc}")
            try:
                from tests.ui_automation.harness.pdfe_app_fixture import (
                    kill_pdfe_processes)
                kill_pdfe_processes()
            except Exception:  # noqa: BLE001
                pass
            continue
        rows.append(rec)
        if rec["changed_plain(XPASS)"]:
            trips_plain += 1
        if rec["changed_stable"]:
            trips_stable += 1
        print(f"  iter {i}: plain ssim={rec['ssim_plain']} "
              f"frac={rec['frac_plain']} XPASS={rec['changed_plain(XPASS)']} "
              f"| stable ssim={rec['ssim_stable']} frac={rec['frac_stable']} "
              f"chg={rec['changed_stable']} | inner_frac="
              f"{rec.get('inner_frac_of_total')} margin_frac="
              f"{rec.get('margin_frac_of_total')}")

    if rows:
        keys = list(rows[0].keys())
        with open(csv, "w", encoding="utf-8") as fh:
            fh.write(",".join(keys) + "\n")
            for r in rows:
                fh.write(",".join(str(r.get(k, "")) for k in keys) + "\n")
    print(f"\nSUMMARY: {len(rows)} runs; "
          f"plain XPASS (racy capture) = {trips_plain}/{len(rows)}; "
          f"stable-capture changed = {trips_stable}/{len(rows)}")
    print(f"metrics -> {csv}")


if __name__ == "__main__":
    main()
