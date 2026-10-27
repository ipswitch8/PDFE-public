# -*- coding: utf-8 -*-
"""Visual verification harness (Pipeline a7c2e541 phase-4).

Modules:
- capture.py            HWND / screen-region screenshots and DPI detection
- diff.py               SSIM + pixel diff, side-by-side visualization
- update_baselines.py   CLI to refresh committed baselines after manual review
- test_visual_baselines.py  pytest demonstration tests + fixtures

Baselines live at tests/fixtures/screenshots/{test_name}.png.
Failure artifacts (diff visualizations) land in tests/visual/_failure_artifacts/.
"""
