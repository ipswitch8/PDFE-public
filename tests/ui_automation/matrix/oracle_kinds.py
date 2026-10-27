# -*- coding: utf-8 -*-
"""Quantify what KIND of oracle each matrix row actually asserts (gap A1-67).

UI-ORACLE-01 says "~632 matrix rows assert STRUCTURE, not pixels; the
render-proof lane is the only pixel lane". That number came from the ``tool``
column (``pywinauto`` = 632 rows), which is a statement about the DRIVER, not
about the oracle. A pywinauto-driven test can perfectly well end in a pixel
assertion, and a ``gtest`` row asserts the model, not the UI at all.

This module answers the question from the tests themselves: it resolves each
row to the test that backs it (see :mod:`coverage_resolver`) and classifies
that test by the strongest oracle it actually uses.

Kinds
=====

``pixel``
    The backing test captures the canvas and asserts on the rendered image —
    ``render_oracle`` / ``render_drive`` (``assert_matches_golden``,
    ``assert_canvas_changed``, ``assert_not_blank``, the font-identity
    oracles), ``tests.visual.diff``/``compare``, ``compare_to_golden``, or a
    PyAutoGUI screenshot comparison.
``structural``
    The backing test asserts UIA/Qt structure: control existence, enabled
    state, text values, object names, window titles. Real assertions — but a
    blank or mirrored render passes them.
``model``
    The row is backed by a C++ GoogleTest: the document model / byte layer,
    with no UI render involved.
``unresolved``
    No backing test could be found at all (that is gap A1-70's finding, not
    this one, but it is reported here so the percentages have a denominator
    that adds up).
"""
from __future__ import annotations

import re
from collections import Counter
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from tests.ui_automation.matrix import coverage_resolver as cr

#: Any of these appearing in a backing test means it asserts on PIXELS.
PIXEL_MARKERS: Tuple[str, ...] = (
    "render_oracle",
    "render_drive",
    "assert_matches_golden",
    "assert_canvas_changed",
    "assert_not_blank",
    "matches_golden",
    "canvas_changed",
    "region_is_blank",
    "assert_slant_matches",
    "assert_overlay_font_class",
    "assert_overlay_alignment",
    "assert_glyph_class",
    "compare_to_golden",
    "pixel_diff_ratio",
    "tests.visual.diff",
    "from tests.visual import diff",
    "save_diff_visualization",
    "pyautogui.screenshot",
    "capture_hwnd",
    "capture_screen_region",
)


def classify_source(text: str, path: str) -> str:
    """Classify one backing-test source by its strongest oracle."""
    if path.endswith(".cpp"):
        return "model"
    if any(m in text for m in PIXEL_MARKERS):
        return "pixel"
    return "structural"


def _evidence_path(res: cr.Resolution) -> Optional[str]:
    """Best-effort path of the file backing a resolution."""
    if not res.kind:
        return None
    if res.kind == "param":
        return res.evidence.split("::", 1)[0]
    if res.kind == "source":
        return res.evidence
    if res.kind == "covered-by":
        return res.evidence.split("::", 1)[0].strip()
    if res.kind == "notes-gtest":
        name = res.evidence.split("::", 1)[0]
        return f"tests/{name}"
    return None


def classify_rows(rows: Iterable[Dict[str, str]],
                  index: cr.TestIndex) -> List[Tuple[str, str, str]]:
    """Return ``[(test_id, status, kind), ...]`` for every row."""
    out: List[Tuple[str, str, str]] = []
    for row in rows:
        res = cr.resolve_row(row, index)
        path = _evidence_path(res)
        if path is None:
            out.append((res.test_id, res.status, "unresolved"))
            continue
        text = index.sources.get(path)
        if text is None:
            # notes-gtest paths may live elsewhere under tests/
            match = [p for p in index.sources
                     if Path(p).name == Path(path).name]
            text = index.sources[match[0]] if match else None
            path = match[0] if match else path
        if text is None:
            out.append((res.test_id, res.status, "unresolved"))
            continue
        out.append((res.test_id, res.status, classify_source(text, path)))
    return out


def summarise(classified: Iterable[Tuple[str, str, str]]) -> Dict[str, int]:
    return dict(sorted(Counter(k for _t, _s, k in classified).items()))


def format_report(classified: List[Tuple[str, str, str]],
                  tool_counts: Optional[Dict[str, int]] = None) -> str:
    total = len(classified)
    counts = summarise(classified)
    lines = ["Matrix oracle kinds (gap A1-67 / UI-ORACLE-01)", ""]
    lines.append(f"rows: {total}")
    for k, v in counts.items():
        pct = (100.0 * v / total) if total else 0.0
        lines.append(f"  {k:12s} {v:4d}  ({pct:5.1f}%)")
    green = [c for c in classified if c[1] == "green"]
    if green:
        gc = summarise(green)
        lines.append("")
        lines.append(f"green rows only: {len(green)}")
        for k, v in gc.items():
            lines.append(f"  {k:12s} {v:4d}  "
                         f"({100.0 * v / len(green):5.1f}%)")
    if tool_counts:
        lines.append("")
        lines.append("for contrast, the 'tool' column (driver, not oracle):")
        for k, v in sorted(tool_counts.items(), key=lambda kv: -kv[1]):
            lines.append(f"  {k:22s} {v:4d}")
    return "\n".join(lines)


def main() -> int:
    import csv
    csv_path = Path(__file__).resolve().parent / "ui_test_matrix.csv"
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    index = cr.build_index()
    classified = classify_rows(rows, index)
    tools = Counter(r.get("tool", "") for r in rows)
    print(format_report(classified, dict(tools)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
