# -*- coding: utf-8 -*-
"""Phase-9: P2 Text Editing Rich Formatting.

Exercises the TextEditToolBar format controls — bold/italic/underline/
strikethrough toggle buttons, font combo, size spinner. Uses the
controls' UIA state as the oracle: toggle_state for CheckBox
buttons, selection for ComboBox, value for Spinner.

No canvas click is required to exercise the toolbar controls
themselves — activating the Text Edit tool reveals the toolbar,
and the format widgets are controllable via pywinauto without
needing a live editing session. This sidesteps the phase-5
canvas-click harness gap for the format-UI layer.

What phase-9 does NOT cover:
  * Applying formatting to actual text spans on the canvas — that
    requires the canvas-click gap to be resolved (phase-5 scope).
  * Color picker — opens a QColorDialog; color selection is phase-10
    polish work.
  * Alignment — no alignment widgets exist in TextEditToolBar (phase-0
    matrix debt).
  * TRE edit on image-dominated pages — requires a live text span
    click, blocked on the canvas-click gap.
"""
from __future__ import annotations

import csv
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    kill_pdfe_processes,
)
from tests.ui_automation.harness.dialog_helpers import (  # noqa: E402
    find_by_suffix,
    select_combo_item,
)
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402

MATRIX_CSV = Path(__file__).parent / "matrix" / "ui_test_matrix.csv"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def _load_phase9_rows() -> List[dict]:
    if not MATRIX_CSV.exists():
        return []
    with MATRIX_CSV.open("r", encoding="utf-8", newline="") as f:
        return [r for r in csv.DictReader(f) if r.get("owner_phase") == "phase-9"]


PHASE9_ROWS = _load_phase9_rows()


def _launch(fixture: str) -> tuple[PdfeAppFixture, Path]:
    kill_pdfe_processes()
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase9_"))
    dst = tmp / fixture
    shutil.copy2(FIXTURES_DIR / fixture, dst)
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    try:
        fx.main_window.maximize()
    except Exception:
        pass
    time.sleep(1.5)
    return fx, tmp


def _cleanup(fx: PdfeAppFixture, tmp: Path) -> None:
    try:
        fx.tearDown()
    except Exception:
        pass
    try:
        shutil.rmtree(tmp, ignore_errors=True)
    except Exception:
        pass


def _pdfe_alive() -> bool:
    try:
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe"],
            capture_output=True, text=True, timeout=10,
        )
        return "pdfe.exe" in r.stdout
    except Exception:
        return False


def _activate_text_edit(fx: PdfeAppFixture) -> bool:
    """Click the textEditToolActionButton to reveal the format toolbar."""
    btn = find_by_suffix(fx.main_window, "textEditToolActionButton")
    if btn is None:
        return False
    try:
        btn.click_input()
    except Exception:
        return False
    time.sleep(0.5)
    # Verify the format toolbar is reachable post-activation
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if find_by_suffix(fx.main_window, "fontComboBox") is not None:
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------------------
# Op handlers — pure toolbar-state oracles
# ---------------------------------------------------------------------------

def _toggle_op(suffix: str, want_on: bool):
    """Generic toggle-button handler that reads back toggle_state."""
    def _run(fx, tmp) -> tuple[str, str]:
        if not _activate_text_edit(fx):
            return "red", "text edit tool did not activate"
        btn = find_by_suffix(fx.main_window, suffix)
        if btn is None:
            return "red", f"{suffix} not reachable"
        # Make sure we start at the opposite state before toggling.
        try:
            initial = btn.get_toggle_state()
        except Exception as e:
            return "red", f"get_toggle_state failed: {e}"
        target = 1 if want_on else 0
        if initial == target:
            # Toggle twice to end at the target state.
            try:
                btn.click_input()
                time.sleep(0.2)
                btn.click_input()
                time.sleep(0.2)
            except Exception as e:
                return "red", f"click_input failed: {e}"
        else:
            try:
                btn.click_input()
                time.sleep(0.2)
            except Exception as e:
                return "red", f"click_input failed: {e}"
        try:
            final = btn.get_toggle_state()
        except Exception as e:
            return "red", f"get_toggle_state (after) failed: {e}"
        if final != target:
            return "red", f"{suffix}: want toggle={target}, got {final}"
        return "green", f"{suffix}: toggled to {final}"
    return _run


def _font_select(font_family: str):
    """Type font name into QFontComboBox Edit child; read back to verify."""
    def _run(fx, tmp) -> tuple[str, str]:
        if not _activate_text_edit(fx):
            return "red", "text edit tool did not activate"
        combo = find_by_suffix(fx.main_window, "fontComboBox")
        if combo is None:
            return "red", "fontComboBox not reachable"
        # Type into the Edit child — .select() is unreliable on QFontComboBox
        typed = False
        for e in combo.descendants(control_type="Edit"):
            try:
                e.set_edit_text(font_family)
                time.sleep(0.2)
                e.type_keys("{ENTER}")
                time.sleep(0.3)
                typed = True
                break
            except Exception:
                continue
        if not typed:
            # Pipeline I phase-3 audit: combo.select() doesn't update
            # Qt's currentIndex; the read-back via combo.selected_text()
            # below would surface the UIA-only update as a false-green.
            # Use the click_input-on-ListItem helper instead.
            if not select_combo_item(combo, font_family,
                                     parent=fx.main_window,
                                     timeout_s=3.0):
                return "blocked", f"{font_family}: input failed"
            time.sleep(0.3)
        # Read back via selected_text or Edit child
        current = None
        try:
            current = combo.selected_text()
        except Exception:
            pass
        if not current:
            for e in combo.descendants(control_type="Edit"):
                try:
                    current = e.window_text()
                    break
                except Exception:
                    continue
        if not current:
            try:
                current = combo.window_text()
            except Exception:
                current = ""
        if font_family.lower() not in (current or "").lower():
            if not (current or "").lower().startswith(font_family.lower()):
                return "red", f"{font_family}: selected={current!r}"
        return "green", f"{font_family}: selected {current!r}"
    return _run


def _size_set(size_value: int):
    """Set fontSizeSpinBox to a given integer value; read back."""
    def _run(fx, tmp) -> tuple[str, str]:
        if not _activate_text_edit(fx):
            return "red", "text edit tool did not activate"
        spin = find_by_suffix(fx.main_window, "fontSizeSpinBox")
        if spin is None:
            return "red", "fontSizeSpinBox not reachable"
        # Two approaches: set_value() if the wrapper supports it, else
        # clear + type the value.
        try:
            if hasattr(spin, "set_value"):
                spin.set_value(float(size_value))
                time.sleep(0.2)
        except Exception:
            pass
        # Type-in fallback — select all + replace
        try:
            spin.set_focus()
            spin.type_keys("^a")
            time.sleep(0.1)
            spin.type_keys(str(size_value))
            spin.type_keys("{ENTER}")
            time.sleep(0.3)
        except Exception as e:
            return "red", f"spin input failed: {e}"
        # Read back via window_text
        try:
            current = spin.window_text() or ""
        except Exception:
            current = ""
        # QDoubleSpinBox shows e.g. "12.00 pt" or "12"
        if str(size_value) not in current:
            return "red", f"size {size_value}: read back {current!r}"
        return "green", f"size {size_value}: {current!r}"
    return _run


def _combo_toggles(specs: List[tuple[str, bool]]):
    """Apply multiple toggles in sequence, assert each lands correctly."""
    def _run(fx, tmp) -> tuple[str, str]:
        if not _activate_text_edit(fx):
            return "red", "text edit tool did not activate"
        applied = []
        for suffix, want_on in specs:
            btn = find_by_suffix(fx.main_window, suffix)
            if btn is None:
                return "red", f"{suffix} not reachable"
            target = 1 if want_on else 0
            try:
                cur = btn.get_toggle_state()
            except Exception as e:
                return "red", f"{suffix} get_toggle_state: {e}"
            if cur != target:
                try:
                    btn.click_input()
                    time.sleep(0.2)
                except Exception as e:
                    return "red", f"{suffix} click: {e}"
            try:
                final = btn.get_toggle_state()
            except Exception as e:
                return "red", f"{suffix} final: {e}"
            if final != target:
                return "red", f"{suffix} want={target} got={final}"
            applied.append(f"{suffix}={final}")
        return "green", "combo: " + ", ".join(applied)
    return _run


def _color_pick(target_hex: str):
    """Click textColorButton, drive QColorDialog hex edit, click OK."""
    def _run(fx, tmp) -> tuple[str, str]:
        if not _activate_text_edit(fx):
            return "red", "text edit tool did not activate"
        btn = find_by_suffix(fx.main_window, "textColorButton")
        if btn is None:
            return "red", "textColorButton not reachable"
        try:
            btn.click_input()
        except Exception as e:
            return "red", f"textColorButton click failed: {e}"
        time.sleep(0.8)
        # Find QColorDialog
        dlg = None
        for d in fx.main_window.descendants(control_type="Window"):
            if "Color" in (d.window_text() or ""):
                dlg = d
                break
        if dlg is None:
            return "red", "QColorDialog did not appear"
        # Find hex edit
        hex_edit = None
        for e in dlg.descendants(control_type="Edit"):
            try:
                aid = e.element_info.automation_id or ""
                if "colorname_lineedit" in aid:
                    hex_edit = e
                    break
            except Exception:
                continue
        if hex_edit is None:
            return "red", "QColorDialog hex edit not found"
        initial = hex_edit.window_text() or ""
        try:
            hex_edit.set_edit_text(target_hex)
            time.sleep(0.3)
        except Exception as e:
            return "red", f"hex edit set_edit_text failed: {e}"
        # Click OK
        ok_btn = None
        for b in dlg.descendants(control_type="Button"):
            if b.window_text() == "OK":
                ok_btn = b
                break
        if ok_btn is None:
            return "red", "OK button not found"
        try:
            ok_btn.click_input()
        except Exception as e:
            return "red", f"OK click failed: {e}"
        time.sleep(0.5)
        return "green", f"color {initial} -> {target_hex}"
    return _run


def _op_blocked(reason: str):
    def _run(fx, tmp) -> tuple[str, str]:
        return "blocked", reason
    return _run


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------

HANDLERS: Dict[str, tuple[str, Callable]] = {
    # Toggle on/off × 4 decorations = 8 tests
    "txt_bold_on":       ("test_single.pdf", _toggle_op("boldButton", True)),
    "txt_bold_off":      ("test_single.pdf", _toggle_op("boldButton", False)),
    "txt_italic_on":     ("test_single.pdf", _toggle_op("italicButton", True)),
    "txt_italic_off":    ("test_single.pdf", _toggle_op("italicButton", False)),
    "txt_underline_on":  ("test_single.pdf", _toggle_op("underlineButton", True)),
    "txt_underline_off": ("test_single.pdf", _toggle_op("underlineButton", False)),
    "txt_strike_on":     ("test_single.pdf", _toggle_op("strikethroughButton", True)),
    "txt_strike_off":    ("test_single.pdf", _toggle_op("strikethroughButton", False)),

    # Font select × 4. QFontComboBox.select() requires the exact
    # system font name. "Helvetica" may not be installed on Windows
    # Server (it's a Mac font). Fallback names provided.
    "txt_font_arial":     ("test_single.pdf", _font_select("Arial")),
    "txt_font_times":     ("test_single.pdf", _font_select("Times New Roman")),
    "txt_font_courier":   ("test_single.pdf", _font_select("Courier New")),
    "txt_font_helvetica": ("test_single.pdf", _font_select("Helvetica")),

    # Size set × 5
    "txt_size_8":  ("test_single.pdf", _size_set(8)),
    "txt_size_12": ("test_single.pdf", _size_set(12)),
    "txt_size_18": ("test_single.pdf", _size_set(18)),
    "txt_size_24": ("test_single.pdf", _size_set(24)),
    "txt_size_36": ("test_single.pdf", _size_set(36)),

    # Combo toggles × 3
    "txt_combo_bold_italic": ("test_single.pdf", _combo_toggles([
        ("boldButton", True), ("italicButton", True),
    ])),
    "txt_combo_italic_underline": ("test_single.pdf", _combo_toggles([
        ("italicButton", True), ("underlineButton", True),
    ])),
    "txt_combo_all_decor": ("test_single.pdf", _combo_toggles([
        ("boldButton", True),
        ("italicButton", True),
        ("underlineButton", True),
        ("strikethroughButton", True),
    ])),

    # Color — textColorButton opens QColorDialog; set via hex edit
    "txt_color_red": ("test_single.pdf", _color_pick("#ff0000")),
    "txt_color_blue": ("test_single.pdf", _color_pick("#0000ff")),
    "txt_color_green": ("test_single.pdf", _color_pick("#00ff00")),
    "txt_color_custom": ("test_single.pdf", _color_pick("#ff8800")),

    # Pipeline K phase-3 stale-stub audit (2026-05-05): txt_align_*
    # rows are covered by concrete drivers in test_features_phase7.py
    # (_op_txt_align_left/_center/_right/_justify; alignButton_ wired
    # in TextEditToolBar.cpp:167-198). Removed here to prevent stale
    # 'blocked' from demoting the green records.

    # Pipeline K phase-4 (2026-05-05): two of the four TRE edit rows
    # are now covered by canvas-click drivers in
    # tests/ui_automation/test_canvas_click_tre.py:
    #   txt_tre_edit_regular_font → test_canvas_click_tre.py
    #   txt_tre_edit_size_change → test_canvas_click_tre.py
    # The Arabic and CJK variants stay blocked: they need both the
    # canvas-click harness AND content-specific fixtures (test_scanned
    # is English-only). Adding those fixtures is fixture-gen work
    # outside the test-harness layer.
    "txt_tre_edit_arabic": ("test_scanned.pdf", _op_blocked(
        "needs Arabic-content scanned fixture; canvas-click harness "
        "ready (tests/ui_automation/harness/canvas_click.py) but no "
        "fixture with Arabic OCR text exists")),
    "txt_tre_edit_cjk": ("test_scanned.pdf", _op_blocked(
        "needs CJK-content scanned fixture; canvas-click harness "
        "ready (tests/ui_automation/harness/canvas_click.py) but no "
        "fixture with CJK OCR text exists")),
}


# ---------------------------------------------------------------------------
# Parametrized test
# ---------------------------------------------------------------------------

@pytest.mark.skipif(
    not PDFE_EXE.exists(),
    reason=f"pdfe.exe not built at {PDFE_EXE}",
)
@pytest.mark.parametrize(
    "row",
    PHASE9_ROWS,
    ids=[r["test_id"] for r in PHASE9_ROWS],
)
def test_p2_text_formatting(row: dict) -> None:
    test_id = row["test_id"]
    entry = HANDLERS.get(test_id)
    if entry is None:
        record_matrix_result(test_id, "blocked", "no handler registered")
        pytest.skip(f"{test_id}: no handler")
    fixture, handler = entry

    fx, tmp = _launch(fixture)
    try:
        try:
            status, notes = handler(fx, tmp)
        except Exception as e:
            status, notes = "red", f"handler exception: {e}"
    finally:
        _cleanup(fx, tmp)

    record_matrix_result(test_id, status, notes)
    if status == "blocked":
        pytest.skip(notes)
    if status == "red":
        pytest.fail(notes)
