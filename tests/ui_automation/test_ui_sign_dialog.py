#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-3 AC-mandated entry point for sign-dialog tests.

The acceptance criteria require a file named `test_ui_sign_dialog.py`.
The actual test functions live in `test_misc_phase_post.py` alongside
unrelated tests (portfolio, preflight). This file re-exports just the
sign-dialog ones so pytest discovery against the AC name works.
"""
from tests.ui_automation.test_misc_phase_post import (  # noqa: F401
    test_sign_dialog_exposes_invisible_checkbox,
    test_sign_dialog_exposes_certification_combo,
)
