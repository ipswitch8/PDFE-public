#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-3 AC-mandated entry point for follow-link tests.

The acceptance criteria require a file named `test_ui_follow_links.py`.
The actual test functions live in `test_follow_link_phase_post.py`
(historic name, kept for matrix-row backward compatibility). This
file re-exports them so pytest discovery against the AC name works.
"""
from tests.ui_automation.test_follow_link_phase_post import (  # noqa: F401
    pdfe_app,
    test_follow_goto_link_navigates_to_target_page,
    test_follow_named_nextpage_link_advances_one_page,
    test_follow_uri_link_dispatches_without_crashing,
    test_follow_launch_link_prompts_confirmation,
)
