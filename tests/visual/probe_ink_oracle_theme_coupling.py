# -*- coding: utf-8 -*-
"""GitHub #279 — why is the ink-fraction oracle red at 0.478?

`test_canvas_pyautogui_regression::_assert_canvas_renders_document` asserts

    0.002 <= ink <= 0.45

where `ink` is the fraction of pixels darker than 130 **inside the page crop**.
It currently measures **0.478** and fails, standalone and in-suite alike, so it
is not an ordering artefact.

That oracle was written specifically to ESCAPE theme coupling: its docstring
says a pixel-SSIM baseline "could not hold on the CI runner" and that the
ink/background ratio "IS stable across rigs/sessions/DPI". It crops to the page
first precisely so the grey viewport margin cannot influence the number.

So the question worth answering is not "what is the number" but **"did the crop
work"**. If `crop_to_page_content` fails to locate the page under the current
dark palette, the oracle measures the dark SURROUND as ink and the ratio it
relies on stops being a property of the document at all — which would make this
a second instance of #279 in an oracle built to be immune to it.

Prints, for a fresh capture taken through the same path the test uses:

  * near-white fraction (does a page block exist to find?)
  * whether the crop localised, and by how much
  * the ink/bright numbers full-frame vs page-crop

If ink is over the ceiling full-frame but under it on a correct page crop, the
surround is the cause and the fix is the crop, not the threshold.
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

INK_LO, INK_HI = 0.002, 0.45


def describe(label: str, img) -> dict:
    import numpy as np

    g = np.asarray(img.convert("L"))
    rgb = np.asarray(img.convert("RGB"), dtype=np.int16)
    d = {
        "label": label,
        "size": img.size,
        "near_white": float((rgb >= 230).all(axis=-1).mean()),
        "ink": float((g < 130).mean()),
        "bright": float((g >= 200).mean()),
    }
    print("  %-14s size=%-12s near_white=%.4f ink=%.4f bright=%.4f  ink in band=%s"
          % (label, "x".join(map(str, img.size)), d["near_white"], d["ink"],
             d["bright"], INK_LO <= d["ink"] <= INK_HI))
    return d


def main() -> int:
    if not os.environ.get("SESSIONNAME"):
        print("ERROR: no interactive desktop session", file=sys.stderr)
        return 2

    from tests.visual.diff import crop_to_page_content
    from tests.visual.test_canvas_pyautogui_regression import (
        _capture_canvas_via_pyautogui, FIXTURE_SINGLE)
    from tests.ui_harness.core.launch import PDFESession

    with PDFESession(pdf_path=str(FIXTURE_SINGLE)) as session:
        time.sleep(2.0)
        actual = _capture_canvas_via_pyautogui(session)

    print("capture taken through the test's own path:")
    full = describe("full frame", actual)
    page = crop_to_page_content(actual)
    kept = (page.size[0] * page.size[1]) / max(1, actual.size[0] * actual.size[1])
    crop = describe("page crop", page)
    print("\n  crop kept %.1f%% of the frame -> %s"
          % (kept * 100,
             "NO-OP, the page was NOT located" if kept > 0.98
             else "localised"))

    print("\n--- reading ---")
    if kept > 0.98 and full["ink"] > INK_HI:
        print("  The crop did not locate a page, so the oracle is measuring the "
              "whole frame. Its ink figure therefore includes the "
              "palette-derived surround rather than only document glyphs, which "
              "is GitHub #279's coupling appearing in an oracle written to be "
              "immune to it. The fix belongs in the crop (or in what region is "
              "measured), NOT in raising the 0.45 ceiling -- raising it would "
              "accept a dark surround as 'a correctly rendered text page'.")
    elif crop["ink"] <= INK_HI < full["ink"]:
        print("  The crop DID locate the page and the page's ink is within "
              "band; the full frame is not. So the surround is the cause and "
              "the oracle's own crop is the remedy.")
    elif crop["ink"] > INK_HI:
        print("  The PAGE ITSELF measures above the ceiling, so this is not "
              "surround contamination -- the rendered document really is "
              "darker/denser than the band allows. That is a product question, "
              "not an oracle one.")
    else:
        print("  ink is within band here; the failure did not reproduce in this "
              "capture. Do not conclude anything from a single non-reproduction "
              "-- re-run before treating it as resolved.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
