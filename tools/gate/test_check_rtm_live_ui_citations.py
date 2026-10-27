#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_rtm_live_ui_citations.py.

Run explicitly -- `pytest.ini` sets `testpaths = tests`, so a file under
`tools/gate/` is NOT auto-collected by a bare `pytest`. It runs because it is
named in the "Gate self-tests" step of `.github/workflows/fast-checks.yml`.

THE TWO CONTROLS
----------------
1. PLANTED-FILE control: a converted live-UI file cited by no row makes the
   checker FAIL; citing it makes the checker PASS. This proves the checker
   reacts to CITATION, not to file existence.

2. PROSE-ONLY control -- the discriminating one: a file mentioned ONLY in a
   prose paragraph is still reported UNCITED. Control 1 alone cannot catch the
   bug this checker exists to avoid, because a *planted* file has no prose
   mention either, so a naive substring checker fails on it too and looks
   correct. Only control 2 separates "reads the table" from "greps the file".
   This is not hypothetical: before F29,
   `grep -n "test_ui_annotations.py" docs/audit/rtm-coverage.md` returned a hit,
   and the hit was the F25 sentence saying the file was uncited.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_rtm_live_ui_citations import (  # noqa: E402
    cited_filenames,
    evaluate,
    extract_requirement_test_cells,
)

# The canonical F25 converted shape, verbatim (double-quoted "blocked" included).
# It must be exact: these bodies are written into tmp_path, which pytest.ini roots
# at tests/_tmp, and ReachabilitySkipGuard.ConvertedSitesStillRecordBlockedBefore\
# Failing in tests/test_doc_drift_guards.cpp walks tests/ looking for `_reach = (`.
# F29 excluded /_tmp/ from that walk, but writing a faithful sample costs nothing
# and keeps this file from being a trap for the next person who edits the guard.
CONVERTED_BODY = (
    "# -*- coding: utf-8 -*-\n"
    "import pytest\n"
    "def test_thing(session):\n"
    "    if not session.find('x'):\n"
    "        _reach = (\n"
    '            "x not reachable."\n'
    "        )\n"
    '        record_matrix_result(current_test_id(), "blocked", _reach)\n'
    "        pytest.fail(\n"
    '            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")\n'
)

RTM_HEAD = """# RTM

### Document lifecycle

| Requirement | Source | Test(s) | Class |
|---|---|---|---|
| Open / view / close | README | test_document, test_page | COVERED |
| Save | README | test_save | COVERED |
"""


def _make_repo(tmp_path: Path, rtm_body: str, ui_files: dict) -> Path:
    root = tmp_path
    (root / "docs" / "audit").mkdir(parents=True)
    # newline="" so the exact bytes we pass are what lands on disk; the checker
    # is responsible for CRLF normalisation, and one of the tests below relies
    # on that being true.
    with open(root / "docs" / "audit" / "rtm-coverage.md", "w", encoding="utf-8", newline="") as f:
        f.write(rtm_body)
    (root / "tests" / "ui_automation").mkdir(parents=True)
    for name, body in ui_files.items():
        target = root / "tests" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, "w", encoding="utf-8", newline="") as f:
            f.write(body)
    return root


# ---------------------------------------------------------------------------
# CONTROL 1 -- planted uncited file fails, citing it passes
# ---------------------------------------------------------------------------


def test_control1_planted_uncited_converted_file_is_reported(tmp_path):
    root = _make_repo(tmp_path, RTM_HEAD, {"test_ui_planted.py": CONVERTED_BODY})
    result = evaluate(root, recursive=False)
    assert result["uncited"] == ["tests/test_ui_planted.py"]


def test_control1_same_file_passes_once_cited_in_a_requirement_row(tmp_path):
    rtm = RTM_HEAD + (
        "| Planted requirement | README | test_document, "
        "**test_ui_planted.py** (drives the thing) | COVERED |\n"
    )
    root = _make_repo(tmp_path, rtm, {"test_ui_planted.py": CONVERTED_BODY})
    result = evaluate(root, recursive=False)
    assert result["uncited"] == []


# ---------------------------------------------------------------------------
# CONTROL 2 -- the discriminating one. Prose is not a citation.
# ---------------------------------------------------------------------------


def test_control2_file_named_only_in_prose_is_still_reported_uncited(tmp_path):
    """A substring checker passes here. This one must not.

    The paragraph below is the shape of the real F25 text: it names the file
    precisely in order to say the file is NOT cited.
    """
    rtm = RTM_HEAD + (
        "\nThe finding this re-read surfaces about the RTM itself:\n\n"
        "`test_ui_planted.py` (55 converted sites) is cited by **no requirement "
        "row at all**, while the adjacent rows are marked COVERED on the "
        "strength of headless tests only.\n"
    )
    root = _make_repo(tmp_path, rtm, {"test_ui_planted.py": CONVERTED_BODY})

    # The trap, made explicit: substring presence says "cited".
    raw = (root / "docs" / "audit" / "rtm-coverage.md").read_text(encoding="utf-8")
    assert "test_ui_planted.py" in raw, "precondition: the prose does name the file"

    result = evaluate(root, recursive=False)
    assert result["uncited"] == ["tests/test_ui_planted.py"], (
        "a file named only in prose must still be UNCITED -- this is the check "
        "that separates table parsing from grepping"
    )


def test_control2_non_requirement_table_columns_do_not_count_as_citations(tmp_path):
    """The F25 re-read table's `after` column quotes filenames. It is not a citation.

    Its header is `| row | before | after | why |`, so the structural parser must
    ignore it entirely.
    """
    rtm = RTM_HEAD + (
        "\n### Re-read log\n\n"
        "| row | before | after | why |\n"
        "|---|---|---|---|\n"
        "| Zoom | `test_ui_planted.py` | `test_ui_planted.py (now fails)` | narrative |\n"
    )
    root = _make_repo(tmp_path, rtm, {"test_ui_planted.py": CONVERTED_BODY})
    result = evaluate(root, recursive=False)
    assert result["uncited"] == ["tests/test_ui_planted.py"]


# ---------------------------------------------------------------------------
# scope derivation
# ---------------------------------------------------------------------------


def test_scope_excludes_live_ui_files_with_no_conversion_marker(tmp_path):
    """Scope is 'contains reachability-converted sites', not 'is a UI test'."""
    root = _make_repo(
        tmp_path,
        RTM_HEAD,
        {
            "test_ui_unconverted.py": "# -*- coding: utf-8 -*-\ndef test_x():\n    assert True\n",
            "test_ui_converted.py": CONVERTED_BODY,
        },
    )
    result = evaluate(root, recursive=False)
    assert result["in_scope"] == ["tests/test_ui_converted.py"]
    assert result["uncited"] == ["tests/test_ui_converted.py"]


def test_recursive_flag_widens_scope_to_subdirectories(tmp_path):
    root = _make_repo(
        tmp_path, RTM_HEAD, {"ui_automation/test_ui_nested.py": CONVERTED_BODY}
    )
    assert evaluate(root, recursive=False)["in_scope"] == []
    assert evaluate(root, recursive=True)["in_scope"] == [
        "tests/ui_automation/test_ui_nested.py"
    ]


def test_citation_matches_on_basename_so_a_path_qualified_mention_counts(tmp_path):
    rtm = RTM_HEAD + (
        "| Nested requirement | README | "
        "**tests/ui_automation/test_ui_nested.py** | COVERED |\n"
    )
    root = _make_repo(
        tmp_path, rtm, {"ui_automation/test_ui_nested.py": CONVERTED_BODY}
    )
    assert evaluate(root, recursive=True)["uncited"] == []


# ---------------------------------------------------------------------------
# CRLF -- core.autocrlf=true makes this a real failure mode, not a formality
# ---------------------------------------------------------------------------


def test_crlf_rtm_parses_identically_to_lf(tmp_path):
    rtm_lf = RTM_HEAD + "| Planted | README | **test_ui_planted.py** | COVERED |\n"
    lf_root = _make_repo(tmp_path / "lf", rtm_lf, {"test_ui_planted.py": CONVERTED_BODY})
    crlf_root = _make_repo(
        tmp_path / "crlf",
        rtm_lf.replace("\n", "\r\n"),
        {"test_ui_planted.py": CONVERTED_BODY.replace("\n", "\r\n")},
    )
    assert evaluate(lf_root, recursive=False)["uncited"] == []
    assert evaluate(crlf_root, recursive=False)["uncited"] == []


def test_crlf_conversion_marker_is_found(tmp_path):
    """The `_reach = (` marker must be found in a CRLF-on-disk file."""
    root = _make_repo(
        tmp_path, RTM_HEAD, {"test_ui_planted.py": CONVERTED_BODY.replace("\n", "\r\n")}
    )
    assert evaluate(root, recursive=False)["in_scope"] == ["tests/test_ui_planted.py"]


# ---------------------------------------------------------------------------
# parser unit-level behaviour
# ---------------------------------------------------------------------------


def test_extractor_returns_only_the_test_column_of_requirement_tables():
    cells = extract_requirement_test_cells(
        RTM_HEAD + "| X | src_hint.py | test_a.py | COVERED |\n"
    )
    assert cells == ["test_document, test_page", "test_save", "test_a.py"]


def test_source_column_filenames_are_not_treated_as_citations():
    """Only column index 2 is read; a .py name in the Source column is not a citation."""
    md = RTM_HEAD + "| X | conftest_hint.py | test_a.py | COVERED |\n"
    assert "conftest_hint.py" not in cited_filenames(md)
    assert "test_a.py" in cited_filenames(md)


def test_missing_rtm_raises_rather_than_silently_passing(tmp_path):
    (tmp_path / "tests").mkdir(parents=True)
    with pytest.raises(FileNotFoundError):
        evaluate(tmp_path, recursive=False)
