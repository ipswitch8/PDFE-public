# -*- coding: utf-8 -*-
"""Honest inventory of the committed visual baselines (gaps A1-66 / A1-100).

Why
===

Two separate claims about visual regression coverage were being read as one:

* ``tests/visual/test_visual_baselines.py`` exists and its five baseline PNGs
  are committed — but every one of its tests is gated behind
  ``REQUIRES_LIVE_UI`` plus a supported-DPI gate plus an ``xfail``, so on any
  headless lane the module contributes **zero assertions** while showing green.
* ``tests/fixtures/ui_screenshots/baseline/`` advertises 14 per-subsystem
  canvas baselines, of which **6 are empty directories holding only a
  ``.gitkeep``** (annotations, forms, image_edit, page_ops, redaction,
  text_edit). Those six subsystems have no visual guard at all.

This module states the true position mechanically, so the numbers in any
report come from the filesystem rather than from memory. It is pure and
headless — no PDFE, no display, no capture.

What is and is not seedable without the live UI
===============================================

``baseline/reference/*.png`` ARE deterministically generatable: they are drawn
from a fully-specified spec by the committed generator
``tests/fixtures/ui_screenshots/generate_reference_goldens.py`` (no screenshot,
no RNG). ``test_baseline_inventory.py`` regenerates them in a temp directory
and asserts byte-equality with the committed files — a real, executed check on
a headless lane.

The 14 per-subsystem ``canvas_baseline.png`` files and the five
``tests/fixtures/screenshots/*.png`` window captures are pictures of the
running application. They CANNOT be synthesised without launching PDFE, and
faking them would be worse than having none. For those, this module's job is
to make the absence explicit and countable rather than invisible.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List

REPO_ROOT = Path(__file__).resolve().parents[2]

#: Per-subsystem live-canvas baselines.
UI_SCREENSHOT_BASELINE_DIR = (REPO_ROOT / "tests" / "fixtures"
                              / "ui_screenshots" / "baseline")

#: Window/dialog captures asserted by tests/visual/test_visual_baselines.py.
VISUAL_BASELINE_DIR = REPO_ROOT / "tests" / "fixtures" / "screenshots"

#: Deterministically generated goldens — NOT live captures.
GENERATED_SUBSYSTEMS = frozenset({"reference"})

#: Not a subsystem canvas: the harness's own self-check baseline.
NON_SUBSYSTEM_DIRS = frozenset({"smoke"})

#: The five baselines tests/visual/test_visual_baselines.py compares against
#: (mirrors its BASELINE_PLAN; duplicated here so the inventory does not need
#: to import a module that pulls in pywinauto).
VISUAL_BASELINE_NAMES = (
    "startup_no_document",
    "document_open_page1",
    "zoom_in_200pct",
    "annotations_visible",
    "encryption_dialog_open",
)


@dataclass(frozen=True)
class SubsystemBaseline:
    subsystem: str
    seeded: bool
    files: tuple
    generated: bool

    @property
    def status(self) -> str:
        if not self.seeded:
            return "NOT-SEEDED"
        return "SEEDED-GENERATED" if self.generated else "SEEDED-CAPTURED"


def scan_subsystem_baselines(
        root: Path = UI_SCREENSHOT_BASELINE_DIR) -> List[SubsystemBaseline]:
    """Classify every ``baseline/<subsystem>/`` directory by what it holds.

    A directory containing only ``.gitkeep`` (or nothing) is NOT-SEEDED: it
    advertises a visual guard that does not exist.
    """
    out: List[SubsystemBaseline] = []
    if not root.is_dir():
        return out
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        if d.name in NON_SUBSYSTEM_DIRS:
            continue
        pngs = tuple(sorted(p.name for p in d.iterdir()
                            if p.suffix.lower() == ".png"))
        out.append(SubsystemBaseline(
            subsystem=d.name,
            seeded=bool(pngs),
            files=pngs,
            generated=d.name in GENERATED_SUBSYSTEMS,
        ))
    return out


def scan_visual_baselines(
        root: Path = VISUAL_BASELINE_DIR,
        names=VISUAL_BASELINE_NAMES) -> Dict[str, bool]:
    """{baseline name: present?} for the test_visual_baselines.py plan."""
    return {n: (root / f"{n}.png").is_file() for n in names}


def summarise() -> Dict[str, object]:
    subs = scan_subsystem_baselines()
    live = [s for s in subs if not s.generated]
    vis = scan_visual_baselines()
    return {
        "subsystem_total": len(live),
        "subsystem_seeded": sum(1 for s in live if s.seeded),
        "subsystem_not_seeded": sorted(s.subsystem for s in live
                                       if not s.seeded),
        "generated_subsystems": sorted(s.subsystem for s in subs
                                       if s.generated and s.seeded),
        "visual_plan_total": len(vis),
        "visual_plan_present": sorted(k for k, v in vis.items() if v),
        "visual_plan_missing": sorted(k for k, v in vis.items() if not v),
    }


def format_report() -> str:
    s = summarise()
    lines = ["Visual-baseline inventory (gaps A1-66 / A1-100)", ""]
    lines.append(
        f"Per-subsystem canvas baselines: "
        f"{s['subsystem_seeded']}/{s['subsystem_total']} seeded")
    for sub in scan_subsystem_baselines():
        if sub.generated:
            continue
        mark = "  OK " if sub.seeded else "  !! "
        lines.append(f"{mark}{sub.subsystem:18s} {sub.status}"
                     + (f"  [{', '.join(sub.files)}]" if sub.files else ""))
    if s["subsystem_not_seeded"]:
        lines.append("")
        lines.append(
            f"  {len(s['subsystem_not_seeded'])} subsystem(s) have NO visual "
            f"guard (directory holds only .gitkeep): "
            f"{', '.join(s['subsystem_not_seeded'])}")
        lines.append(
            "  These are live-canvas captures — they cannot be synthesised "
            "headlessly and are NOT covered.")
    lines.append("")
    lines.append(
        f"Deterministically generated goldens (headlessly checkable): "
        f"{', '.join(s['generated_subsystems']) or 'none'}")
    lines.append("")
    lines.append(
        f"test_visual_baselines.py plan: "
        f"{len(s['visual_plan_present'])}/{s['visual_plan_total']} PNGs "
        f"committed")
    if s["visual_plan_missing"]:
        lines.append(f"  MISSING: {', '.join(s['visual_plan_missing'])}")
    lines.append(
        "  NOTE: committed != asserted. Every test in that module is gated "
        "behind REQUIRES_LIVE_UI + a supported-DPI gate + xfail, so on a "
        "headless lane it asserts nothing. See the NOT-RUN report.")
    return "\n".join(lines)


if __name__ == "__main__":
    print(format_report())
