# -*- coding: utf-8 -*-
"""Refresh committed baseline PNGs after manual review.

Usage
-----
    python tests/visual/update_baselines.py
        Capture all baselines, refusing to overwrite existing PNGs.

    python tests/visual/update_baselines.py --force
        Overwrite existing PNGs.

    python tests/visual/update_baselines.py --filter zoom
        Only refresh baselines whose name matches the regex.

The script uses the same capture-fixtures as ``test_visual_baselines.py``
so the produced baselines are exactly what the tests will assert against.

This is the *human-supervised* path: the human is expected to visually
inspect the resulting PNGs (e.g. via ``git diff --stat`` plus an image
viewer) before committing them.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Make the test fixtures importable when this script is run directly.
_HERE = Path(__file__).resolve().parent
_REPO_ROOT = _HERE.parent.parent
sys.path.insert(0, str(_REPO_ROOT))

# pylint: disable=wrong-import-position
from tests.visual import test_visual_baselines as scenarios  # type: ignore


BASELINE_DIR = _REPO_ROOT / "tests" / "fixtures" / "screenshots"


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Refresh PDFE visual-test baselines.",
    )
    parser.add_argument(
        "--filter",
        default=None,
        help="Regex applied to baseline names; only matches refresh.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing baselines (default refuses).",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List the configured baselines and exit.",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    BASELINE_DIR.mkdir(parents=True, exist_ok=True)

    plan = scenarios.BASELINE_PLAN
    if args.list:
        for name, _fn in plan:
            print(name)
        return 0

    pattern = re.compile(args.filter) if args.filter else None

    refreshed: list[str] = []
    skipped: list[str] = []
    failed: list[tuple[str, str]] = []

    for name, capture_fn in plan:
        if pattern is not None and not pattern.search(name):
            continue
        out_path = BASELINE_DIR / f"{name}.png"
        if out_path.exists() and not args.force:
            print(f"[skip] {name}: exists (use --force to overwrite)")
            skipped.append(name)
            continue

        try:
            print(f"[capture] {name} -> {out_path}")
            img = capture_fn()
            img.save(str(out_path))
            refreshed.append(name)
        except Exception as exc:  # noqa: BLE001
            print(f"[fail] {name}: {exc}", file=sys.stderr)
            failed.append((name, str(exc)))

    print()
    print(f"refreshed: {len(refreshed)}")
    print(f"skipped:   {len(skipped)}")
    print(f"failed:    {len(failed)}")
    for n, msg in failed:
        print(f"  - {n}: {msg}")

    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
