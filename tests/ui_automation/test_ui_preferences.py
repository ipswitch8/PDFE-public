#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-3 AC-mandated entry point for preferences tests.

The acceptance criteria require a file named `test_ui_preferences.py`.
The actual test function lives in `test_broader_phase_post.py`. This
file re-exports it so pytest discovery against the AC name works.
"""
from tests.ui_automation.test_broader_phase_post import (  # noqa: F401
    test_preferences_ocr_language_change,
)
