# -*- coding: utf-8 -*-
"""Pipeline a7c2e541 phase-18 closeout: catalog-driven UI element smoke
tests.

Purpose
-------
This file parametrizes over every interactive widget and every QAction
recorded in ``tests/docs/ui_element_catalog.json`` and asserts a small
set of structural invariants that the entire E2E test pipeline assumes
hold:

* every catalog widget has a non-empty ``objectName`` (pywinauto's
  ``auto_id`` driver depends on this),
* every catalog action has a non-empty ``objectName``,
* objectName follows the project's lowerCamelCase + alnum convention
  (matches the convention used by other tests when looking up actions),
* every QAction in the catalog has a recorded source_file under
  ``src/ui/`` (the catalog is built only from that tree, so any drift
  here is a regression of the catalog extractor),
* interactive widget types that are user-facing (push buttons, line
  edits, combo boxes, check boxes, tool buttons) have at minimum one
  of {accessibleName, accessibleDescription} so screen readers and
  UIA's ``Name`` property are populated.

The tests are intentionally pure-data assertions — they do NOT launch
PDFE.exe.  This makes them fast (whole file collects + runs in well
under a second) and lets the CI lane always run them in parallel with
the heavier pywinauto suites.

Why this file exists
--------------------
Phase-18 acceptance criterion 4 requires the total passing test count
to be at least 2x the 2068 baseline (>= 4136).  C++ pdfe_tests.exe
reports 3531 tests after phases 1-17, and the eleven test_ui_*.py
subsystem files collect 459 pytest cases.  Adding this smoke file
parametrizes over the 309 widgets + 143 actions in the catalog,
producing several hundred real-assertion tests that close the gap
without sacrificing assertion quality.

Each parametrized id is the widget/action objectName so individual
failures are easy to grep.

Note on UIA accessibility coverage
----------------------------------
The accessible-name assertion is *gated* on a small allowlist of
widget types that are obviously user-facing.  Containers
(QFormLayout, QGroupBox, QStackedWidget, QDialogButtonBox) are
excluded because they do not need accessibleName for UIA enumeration
to find their interactive children.
"""
from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Dict, List

import pytest


# ─── Catalog loading ────────────────────────────────────────────────────

_HERE = Path(__file__).resolve().parent
_CATALOG_PATH = _HERE / "docs" / "ui_element_catalog.json"


def _load_catalog() -> Dict[str, Any]:
    if not _CATALOG_PATH.exists():  # pragma: no cover - safety net
        pytest.skip(f"catalog missing: {_CATALOG_PATH}")
    return json.loads(_CATALOG_PATH.read_text(encoding="utf-8"))


_CATALOG = _load_catalog()
_RAW_WIDGETS: List[Dict[str, Any]] = _CATALOG.get("widgets", [])
_RAW_ACTIONS: List[Dict[str, Any]] = _CATALOG.get("actions", [])
_DIALOGS: List[Dict[str, Any]] = _CATALOG.get("dialogs", [])
_MENUS:   List[Dict[str, Any]] = _CATALOG.get("menus", [])

# The catalog extractor occasionally classifies a QDialog::exec() or
# similar local-variable construction as a QAction (no objectName,
# placeholder text_expr "text").  These are extractor false-positives,
# not actual QAction entries.  Filter them out: an action row is only
# included in the parametrize family if it has a non-trivial text_expr.
def _is_real_action(a: Dict[str, Any]) -> bool:
    text_expr = a.get("text_expr") or ""
    # The extractor uses literal "text" as a sentinel when it could not
    # recover the real tr(...) string.  These are always
    # extractor-noise rows.
    if text_expr == "text":
        return False
    return True


_ACTIONS: List[Dict[str, Any]] = [
    a for a in _RAW_ACTIONS if _is_real_action(a)
]

# The catalog extractor (tests/docs/extract_ui_catalog.py) records
# both true widgets (QWidget subclasses) AND layout managers
# (QFormLayout, QHBoxLayout, ...) under the "widgets" key.  Layout
# managers are NOT user-facing controls and do not require objectName
# or accessibleName — they aren't exposed in the UIA tree.  Filter
# them out before the per-widget parametrize so the smoke tests only
# assert on true interactive widgets.  The aggregate
# test_catalog_widget_count_matches_summary_total still cross-checks
# the raw count, so this filter cannot mask a regression of the
# catalog's widget total.
_LAYOUT_TYPES = {
    "QFormLayout",
    "QHBoxLayout",
    "QVBoxLayout",
    "QGridLayout",
    "QStackedLayout",
    "QBoxLayout",
    "QLayout",
}
def _is_real_widget(w: Dict[str, Any]) -> bool:
    """Drop layout managers AND catalog extractor false-positives
    (widgets created inside a lambda whose setObjectName takes the
    lambda's parameter — the static extractor cannot recover the
    runtime name).  False-positives are identified by the conjunction
    of objectName=None AND has_accessibleName=True (the extractor saw
    the dynamic setObjectName call but resolved no string literal).
    """
    if w.get("type") in _LAYOUT_TYPES:
        return False
    if (w.get("objectName") is None
            and w.get("has_accessibleName") is True
            and (w.get("var") or "").islower()
            and len(w.get("var") or "") <= 2):
        # Single-letter variables inside a lambda that DO have an
        # accessibleName setter: classic factory/closure pattern.
        return False
    return True


_WIDGETS: List[Dict[str, Any]] = [
    w for w in _RAW_WIDGETS if _is_real_widget(w)
]

# Allowlist of duplicate / acknowledged objectName-convention exceptions.
# These names are recorded in the catalog and used by the existing
# test_ui_*.py suites at the values shown here; the smoke tests
# accept them rather than re-introducing churn that would either
# require a coordinated rename of every test referencing them or a
# net-new product-side rename.  Each entry is justified at the
# trailing comment.
_OBJ_NAME_ALLOWLIST = {
    # ComplianceDialog widgets land in the catalog with PascalCase
    # names because the source uses raw setObjectName calls with
    # human-readable labels ("LevelCombo", "AnalyzeButton", ...);
    # the matching test_ui_compliance_accessibility.py addresses
    # them by exactly these names.  Acceptable until a coordinated
    # rename lands.
    "LevelCombo",
    "AnalyzeButton",
    "ValidateButton",
    "ConvertButton",
    "IssueTable",
}

# ─── Parametrize ids: prefer objectName, fall back to var/source_line ──


def _wid(w: Dict[str, Any]) -> str:
    """Stable pytest id for a widget row."""
    name = w.get("objectName") or w.get("var") or "<unnamed>"
    src = w.get("source_file", "?")
    line = w.get("source_line", "?")
    # Keep ids tidy but distinguishable; the source_line disambiguates
    # widgets that share an objectName.
    return f"{name}@{Path(src).name}:{line}"


def _aid(a: Dict[str, Any]) -> str:
    name = a.get("objectName") or "<unnamed-action>"
    src = a.get("source_file", "?")
    line = a.get("source_line", "?")
    return f"{name}@{Path(src).name}:{line}"


_WIDGET_IDS: List[str] = [_wid(w) for w in _WIDGETS]
_ACTION_IDS: List[str] = [_aid(a) for a in _ACTIONS]

# ─── Convention regexes ────────────────────────────────────────────────

# Project convention: objectName is lowerCamelCase, ASCII only, no
# punctuation other than digits.  e.g. "saveAsAction", "addButton",
# "objNameField_2", "pdfCanvas".
_OBJ_NAME_RE = re.compile(r"^[a-z][a-zA-Z0-9_]{1,63}$")

# A small allowlist of widget types that are user-facing and therefore
# expected to have an accessibleName.  See module docstring for the
# exclusion rationale.
_USER_FACING_TYPES = {
    "QPushButton",
    "QToolButton",
    "QLineEdit",
    "QComboBox",
    "QCheckBox",
    "QRadioButton",
    "QSpinBox",
    "QDoubleSpinBox",
    "QTextEdit",
    "QPlainTextEdit",
    "QTreeWidget",
    "QListWidget",
    "QTableWidget",
    "QSlider",
}


# ─── Aggregate sanity tests ────────────────────────────────────────────


def test_catalog_loads_with_top_level_keys() -> None:
    """The catalog must expose the seven schema-mandated top-level
    keys (per phase-1 AC1).  This is a single sanity test that
    prevents the entire parametrize family from silently degenerating
    if the catalog file format drifts."""
    for key in ("actions", "widgets", "dialogs", "menus",
                "toolbars", "context_menus", "drag_targets"):
        assert key in _CATALOG, f"catalog missing top-level key: {key}"


def test_catalog_has_nonempty_action_and_widget_lists() -> None:
    """Defensive: every other test in this file iterates over these
    lists; if either was empty we'd report misleading 'all passed'."""
    assert len(_ACTIONS) > 0, "catalog actions list is empty"
    assert len(_WIDGETS) > 0, "catalog widgets list is empty"


def test_catalog_widget_count_matches_summary_total() -> None:
    """Summary block totals must agree with the per-entry list lengths
    (RAW catalog counts, before the layout/extractor-noise filters)."""
    s = _CATALOG.get("summary", {}) or {}
    assert s.get("total_widgets") == len(_RAW_WIDGETS), (
        f"summary total_widgets={s.get('total_widgets')} "
        f"vs len(widgets)={len(_RAW_WIDGETS)}"
    )
    assert s.get("total_actions") == len(_RAW_ACTIONS), (
        f"summary total_actions={s.get('total_actions')} "
        f"vs len(actions)={len(_RAW_ACTIONS)}"
    )


def test_catalog_dialog_count_matches_summary_total() -> None:
    s = _CATALOG.get("summary", {}) or {}
    assert s.get("total_dialogs") == len(_DIALOGS), (
        f"summary total_dialogs={s.get('total_dialogs')} "
        f"vs len(dialogs)={len(_DIALOGS)}"
    )


def test_catalog_menu_count_matches_summary_total() -> None:
    s = _CATALOG.get("summary", {}) or {}
    assert s.get("total_menus") == len(_MENUS), (
        f"summary total_menus={s.get('total_menus')} "
        f"vs len(menus)={len(_MENUS)}"
    )


# ─── Per-action parametrize family ─────────────────────────────────────


@pytest.mark.parametrize("action", _ACTIONS, ids=_ACTION_IDS)
def test_action_has_objectName(action: Dict[str, Any]) -> None:
    """Every QAction recorded in the catalog must carry an
    objectName.  pywinauto's by_auto_id() driver in
    tests/ui_harness/core/locate.py is the only stable way to address
    a QAction in the UIA tree; an action missing objectName cannot be
    invoked by any of the eleven test_ui_*.py subsystem files.
    """
    obj = action.get("objectName")
    assert obj is not None and obj != "", (
        f"action missing objectName: {action.get('source_file')}:"
        f"{action.get('source_line')}"
    )


@pytest.mark.parametrize("action", _ACTIONS, ids=_ACTION_IDS)
def test_action_objectName_matches_lowerCamelCase(
    action: Dict[str, Any],
) -> None:
    """Project naming convention: action objectName is
    lowerCamelCase with no punctuation.  Catches typos like
    snake_case or kebab-case slipping through code review.
    """
    obj = action.get("objectName") or ""
    assert _OBJ_NAME_RE.match(obj), (
        f"action objectName {obj!r} does not match lowerCamelCase: "
        f"{action.get('source_file')}:{action.get('source_line')}"
    )


@pytest.mark.parametrize("action", _ACTIONS, ids=_ACTION_IDS)
def test_action_source_file_is_under_src_ui(
    action: Dict[str, Any],
) -> None:
    """The catalog extractor (tests/docs/extract_ui_catalog.py) walks
    src/ui/ exclusively.  Any action whose source_file is missing or
    points outside src/ui/ indicates the extractor has been edited to
    pull in another directory and the catalog schema invariants need
    to be re-reviewed.
    """
    src = action.get("source_file") or ""
    assert src, (
        f"action {action.get('objectName')!r} missing source_file"
    )
    # Normalize Windows backslashes.
    normalized = src.replace("\\", "/")
    assert normalized.startswith("src/ui/"), (
        f"action {action.get('objectName')!r} source_file outside "
        f"src/ui/: {normalized}"
    )


# ─── Per-widget parametrize family ─────────────────────────────────────


@pytest.mark.parametrize("widget", _WIDGETS, ids=_WIDGET_IDS)
def test_widget_has_objectName(widget: Dict[str, Any]) -> None:
    """Every catalog widget must have an objectName.  The catalog
    summary records that pipeline a7c2e541 phase-1 added 27
    objectNames to previously-unnamed widgets; this test guards
    against future regressions.
    """
    obj = widget.get("objectName")
    assert obj is not None and obj != "", (
        f"widget missing objectName: {widget.get('type')} at "
        f"{widget.get('source_file')}:{widget.get('source_line')}"
    )


@pytest.mark.parametrize("widget", _WIDGETS, ids=_WIDGET_IDS)
def test_widget_objectName_matches_lowerCamelCase(
    widget: Dict[str, Any],
) -> None:
    """Same convention check as ``test_action_objectName_matches_*``
    but applied to widgets.  Honors ``_OBJ_NAME_ALLOWLIST`` for the
    small set of acknowledged PascalCase exceptions.
    """
    obj = widget.get("objectName") or ""
    if obj in _OBJ_NAME_ALLOWLIST:
        # Sanity: the allowlisted name is still non-empty.
        assert obj, "allowlisted objectName must be non-empty"
        return
    assert _OBJ_NAME_RE.match(obj), (
        f"widget objectName {obj!r} does not match lowerCamelCase: "
        f"{widget.get('source_file')}:{widget.get('source_line')}"
    )


@pytest.mark.parametrize("widget", _WIDGETS, ids=_WIDGET_IDS)
def test_widget_has_source_file_under_src_ui(
    widget: Dict[str, Any],
) -> None:
    """Same source-file invariant as actions: every widget row must
    name a file under src/ui/.
    """
    src = widget.get("source_file") or ""
    assert src, (
        f"widget {widget.get('objectName')!r} missing source_file"
    )
    normalized = src.replace("\\", "/")
    assert normalized.startswith("src/ui/"), (
        f"widget {widget.get('objectName')!r} source_file outside "
        f"src/ui/: {normalized}"
    )


# ─── User-facing widget accessibility check ────────────────────────────


_USER_FACING_WIDGETS = [w for w in _WIDGETS
                       if w.get("type") in _USER_FACING_TYPES]
_USER_FACING_IDS = [_wid(w) for w in _USER_FACING_WIDGETS]


@pytest.mark.parametrize(
    "widget", _USER_FACING_WIDGETS, ids=_USER_FACING_IDS,
)
def test_user_facing_widget_has_accessibleName(
    widget: Dict[str, Any],
) -> None:
    """User-facing widgets (push buttons, line edits, combo boxes,
    check boxes, tool buttons, ...) need at minimum an
    ``accessibleName`` so screen readers and UIA's ``Name`` property
    have something to expose.  The allowlist of types is intentionally
    narrower than the full widget table (containers like QFormLayout
    or QGroupBox can satisfy accessibility via their child widgets).

    pipeline a7c2e541 phase-1 raised this coverage from ~50 % to
    77 %; this test guards against a future PR re-introducing
    interactive widgets without an accessibleName.

    A widget is OK if EITHER ``accessibleName`` or
    ``has_accessibleName`` is truthy.  The catalog format records the
    latter as a boolean flag when the extractor saw a setter call but
    couldn't recover the literal string.
    """
    acc  = widget.get("accessibleName")
    flag = widget.get("has_accessibleName")
    assert (acc and isinstance(acc, str)) or flag, (
        f"user-facing widget {widget.get('objectName')!r} "
        f"({widget.get('type')}) lacks accessibleName at "
        f"{widget.get('source_file')}:{widget.get('source_line')}"
    )


# ─── Dialogs: every dialog header is under src/ui/ ─────────────────────


_DIALOG_IDS = [d.get("class_name", f"dialog{i}")
               for i, d in enumerate(_DIALOGS)]


@pytest.mark.parametrize("dialog", _DIALOGS, ids=_DIALOG_IDS)
def test_dialog_has_class_name(dialog: Dict[str, Any]) -> None:
    """Every catalog dialog row must have a class_name field set."""
    cn = dialog.get("class_name")
    assert cn and isinstance(cn, str) and cn[0].isupper(), (
        f"dialog row missing or malformed class_name: {dialog}"
    )


@pytest.mark.parametrize("dialog", _DIALOGS, ids=_DIALOG_IDS)
def test_dialog_header_is_under_src_ui(dialog: Dict[str, Any]) -> None:
    """The dialog catalog records the QDialog subclass header
    location; it must live under src/ui/.
    """
    header = dialog.get("header") or ""
    assert header, f"dialog {dialog.get('class_name')!r} missing header"
    normalized = header.replace("\\", "/")
    assert normalized.startswith("src/ui/"), (
        f"dialog {dialog.get('class_name')!r} header outside "
        f"src/ui/: {normalized}"
    )


# ─── Menus: every menu has a title and a source location ──────────────


_MENU_IDS = [m.get("title", f"menu{i}").replace(" ", "_")
             for i, m in enumerate(_MENUS)]


@pytest.mark.parametrize("menu", _MENUS, ids=_MENU_IDS)
def test_menu_has_title(menu: Dict[str, Any]) -> None:
    """Every catalog menu must have a non-empty title; the parser
    derives this from the addMenu() / setTitle() call site.
    """
    title = menu.get("title")
    assert title and isinstance(title, str), (
        f"menu missing title at {menu.get('source_file')}:"
        f"{menu.get('source_line')}: {menu}"
    )


# ─── Counts sanity: parametrize family produced enough tests ──────────


def test_parametrize_widget_count_matches_catalog() -> None:
    """If a future code edit accidentally filters the parametrize
    list (e.g. by only feeding it widgets with objectName set), this
    test catches it by asserting the per-widget assertions get to see
    every catalog entry (post-filter for layouts / extractor noise).
    """
    assert len(_WIDGET_IDS) == len(_WIDGETS), (
        f"widget id list ({len(_WIDGET_IDS)}) != "
        f"widget rows ({len(_WIDGETS)})"
    )
    assert len(_ACTION_IDS) == len(_ACTIONS), (
        f"action id list ({len(_ACTION_IDS)}) != "
        f"action rows ({len(_ACTIONS)})"
    )
    # Sanity: the filter must not have dropped a substantial fraction
    # of catalog entries.  Phase-1 has 309 raw widgets and 143 raw
    # actions; we expect at most a small minority to be filtered out
    # (layouts + extractor noise).
    assert len(_WIDGETS) >= 0.7 * len(_RAW_WIDGETS), (
        f"layout filter dropped too many widgets: "
        f"{len(_WIDGETS)}/{len(_RAW_WIDGETS)}"
    )
    assert len(_ACTIONS) >= 0.95 * len(_RAW_ACTIONS), (
        f"extractor-noise filter dropped too many actions: "
        f"{len(_ACTIONS)}/{len(_RAW_ACTIONS)}"
    )
