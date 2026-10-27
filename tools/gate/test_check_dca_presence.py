#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DCA-presence gate tests (pipeline e5b9c231 Phase 5 — criterion 4 closure).

Proves that a defect-fix commit touching product/oracle logic REQUIRES a DCA
record, feature work does not, and the [skip-dca] escape hatch works. Exercises
the pure decision core (no git needed).

Run:
    python -m pytest tools/gate/test_check_dca_presence.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_dca_presence as cdp  # noqa: E402


def test_defect_fix_touching_src_requires_dca():
    assert cdp.requires_dca(
        ["fix: correct null deref in ContentStreamEditor"],
        ["src/textedit/ContentStreamEditor.cpp"],
    )


def test_defect_fix_touching_test_oracle_requires_dca():
    assert cdp.requires_dca(
        ["bug: oracle was blind to mirrored glyphs"],
        ["tests/test_content_stream_state_leak.cpp"],
    )


def test_feature_work_does_not_require_dca():
    """A feature commit (no defect marker) touching src is not a defect fix."""
    assert not cdp.requires_dca(
        ["feat: add PDF/A-4 conversion target"],
        ["src/compliance/Converter.cpp"],
    )


def test_defect_fix_touching_only_docs_does_not_require_dca():
    """A 'fix' that only edits docs is not a product/oracle defect fix."""
    assert not cdp.requires_dca(
        ["fix: typo in README"],
        ["README.md", "docs/audit/notes.md"],
    )


def test_skip_dca_escape_hatch_suppresses_requirement():
    assert not cdp.requires_dca(
        ["fix: rename local var [skip-dca] cosmetic, no behavior change"],
        ["src/core/Document.cpp"],
    )


def test_has_dca_detects_real_record_only():
    assert cdp.has_dca(["docs/audit/dca/0002-some-defect.md",
                        "src/core/Foo.cpp"])
    # README/TEMPLATE do not count as a record.
    assert not cdp.has_dca(["docs/audit/dca/README.md",
                           "docs/audit/dca/TEMPLATE.md"])
    assert not cdp.has_dca(["src/core/Foo.cpp"])


def test_defect_fix_with_dca_present_is_compliant():
    """The full contract: defect fix touching logic AND a DCA record present."""
    msgs = ["fix: state leak; add DCA"]
    changed = ["src/textedit/ContentStreamEditor.cpp",
               "docs/audit/dca/0003-state-leak.md"]
    assert cdp.requires_dca(msgs, changed)
    assert cdp.has_dca(changed)  # -> main() would return 0


def test_windows_path_separators_are_handled():
    assert cdp.requires_dca(
        ["regression: undo-after-save UAF"],
        ["src\\core\\CommandStack.cpp"],
    )
    assert cdp.has_dca(["docs\\audit\\dca\\0004-uaf.md"])
