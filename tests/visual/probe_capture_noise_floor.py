# -*- coding: utf-8 -*-
"""GitHub #279 — what is the run-to-run noise floor of a canvas capture?

A visual oracle's threshold should sit just above the noise it genuinely has to
tolerate, and no higher. `test_visual_baselines.py` currently uses

    SSIM_THRESHOLD  = 0.95
    PIXEL_THRESHOLD = 0.05      # i.e. tolerate 5% of pixels differing

Measured consequence of that pairing: pasting a **solid red block over 2% of the
page** into a baseline and comparing it against the unmodified baseline gives
SSIM 0.9721 and pixel fraction 0.0102 — and therefore **PASSES**. An oracle that
cannot see a 2% defacement of the document is not protecting much.

Tightening the numbers by guesswork would just trade a false-green for a
false-red, so this probe measures the thing a threshold actually has to survive:
capture the *same* state twice in a row and see how much it moves. That is the
floor. Anything above it is signal.

Usage:
    python tests/visual/probe_capture_noise_floor.py --repeats 3
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

TARGET = "document_open_page1"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--name", default=TARGET)
    args = ap.parse_args(argv)

    if not os.environ.get("SESSIONNAME"):
        print("ERROR: no interactive desktop session", file=sys.stderr)
        return 2

    from PIL import Image
    from tests.visual.diff import compare
    from tests.visual.test_visual_baselines import (
        BASELINE_PLAN, BASELINE_DIR, SSIM_THRESHOLD, PIXEL_THRESHOLD)

    plan = dict(BASELINE_PLAN)
    if args.name not in plan:
        print("unknown baseline %r" % args.name, file=sys.stderr)
        return 2

    shots = []
    for i in range(args.repeats):
        img = plan[args.name]()
        shots.append(img)
        print("capture %d: size=%s" % (i + 1, img.size), flush=True)

    print("\n--- consecutive captures of the SAME state (the noise floor) ---")
    worst_ssim, worst_pf = 1.0, 0.0
    for i in range(len(shots)):
        for j in range(i + 1, len(shots)):
            r = compare(shots[i], shots[j], ssim_threshold=SSIM_THRESHOLD,
                        pixel_threshold=PIXEL_THRESHOLD)
            print("  capture%d vs capture%d: ssim=%.6f pixel_fraction=%.6f"
                  % (i + 1, j + 1, r["ssim"], r["pixel_fraction"]))
            worst_ssim = min(worst_ssim, float(r["ssim"]))
            worst_pf = max(worst_pf, float(r["pixel_fraction"]))
    print("  WORST: ssim=%.6f pixel_fraction=%.6f" % (worst_ssim, worst_pf))

    bp = BASELINE_DIR / f"{args.name}.png"
    if bp.is_file():
        baseline = Image.open(str(bp)).convert("RGB")
        print("\n--- each capture vs the COMMITTED baseline ---")
        for i, s in enumerate(shots):
            r = compare(s, baseline, ssim_threshold=SSIM_THRESHOLD,
                        pixel_threshold=PIXEL_THRESHOLD)
            print("  capture%d: ssim=%.6f pixel_fraction=%.6f pass=%s"
                  % (i + 1, r["ssim"], r["pixel_fraction"], r["pass"]))

    print("\n--- what the floor implies ---")
    print("  current thresholds : ssim>=%.4f  pixel_fraction<=%.4f"
          % (SSIM_THRESHOLD, PIXEL_THRESHOLD))
    print("  measured floor     : ssim>=%.6f  pixel_fraction<=%.6f"
          % (worst_ssim, worst_pf))
    if worst_pf > 0:
        print("  headroom factor on pixel_fraction: %.0fx"
              % (PIXEL_THRESHOLD / worst_pf))
    else:
        print("  pixel_fraction floor is 0: consecutive captures are identical, "
              "so ANY differing pixel is signal and the 5%% tolerance is pure "
              "slack.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
