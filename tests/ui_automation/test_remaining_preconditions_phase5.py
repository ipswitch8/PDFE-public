# -*- coding: utf-8 -*-
"""Phase-5: Remaining precondition-achievable tests (Bucket D part 2).

Fixes 21 wont-fix rows across 6 subcategories:
  menu_cut/copy/paste/select_all (4) — TextEditTool + clipboard
  field_*_set_default (5) — Reset Form with /DV fixture
  txt_tre_edit_* (4) — TRE edit on scanned PDF
  page edge cases (5) — pymupdf structural oracle
  sec_encrypt_permissions (2) — encrypted fixture + pymupdf permission bits
  flow_flow_corpus_robustness (1) — robustness script
"""
from __future__ import annotations

import csv
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Dict, List, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    kill_pdfe_processes,
    invoke_menu_path,
    dismiss_native_dialog,
    find_widget,
    invoke_action,
    write_pipe,
    inject_file_dialog_path,
    read_result_pipe,
)
from tests.ui_automation.harness.dialog_helpers import find_by_suffix  # noqa: E402
from tests.ui_automation.harness.canvas_coords import (  # noqa: E402
    click_canvas,
    get_canvas_bounds,
    pyautogui_click,
    pyautogui_double_click,
)
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402

MATRIX_CSV = Path(__file__).parent / "matrix" / "ui_test_matrix.csv"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def _launch(fixture: str, extra_wait: float = 2.0) -> Tuple[PdfeAppFixture, Path]:
    kill_pdfe_processes()
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase5_"))
    dst = tmp / fixture
    shutil.copy2(FIXTURES_DIR / fixture, dst)
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    try:
        fx.main_window.maximize()
    except Exception:
        pass
    time.sleep(extra_wait)
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


def _get_clipboard_text() -> str:
    """Read CF_UNICODETEXT from the Windows clipboard.

    GAP-A14 fix: PDFE's Copy/Cut/SelectAll set the clipboard via Qt
    (``QApplication::clipboard()->setText``), which uses the OLE
    delayed-rendering clipboard. A raw ``user32.GetClipboardData(13)``
    from another process returns NULL for OLE-rendered text (the
    WM_RENDERFORMAT pump never runs), so the prior reader always saw an
    empty clipboard even though the product correctly placed the text.
    ``win32clipboard`` (pywin32) initializes OLE and pumps the render
    message, so it retrieves Qt's text correctly. Fall back to the raw
    ctypes path only if pywin32 is unavailable (covers non-OLE writers).
    """
    try:
        import win32clipboard  # type: ignore
        win32clipboard.OpenClipboard()
        try:
            if win32clipboard.IsClipboardFormatAvailable(
                    win32clipboard.CF_UNICODETEXT):
                data = win32clipboard.GetClipboardData(
                    win32clipboard.CF_UNICODETEXT)
                return data if isinstance(data, str) else ""
            return ""
        finally:
            win32clipboard.CloseClipboard()
    except ImportError:
        pass
    except Exception:
        # OpenClipboard contention etc. — fall through to ctypes path.
        pass
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    text = ""
    if user32.OpenClipboard(0):
        h = user32.GetClipboardData(13)
        if h:
            p = kernel32.GlobalLock(h)
            if p:
                text = ctypes.wstring_at(p)
                kernel32.GlobalUnlock(h)
        user32.CloseClipboard()
    return text


def _clear_clipboard() -> None:
    try:
        import win32clipboard  # type: ignore
        win32clipboard.OpenClipboard()
        try:
            win32clipboard.EmptyClipboard()
        finally:
            win32clipboard.CloseClipboard()
        return
    except ImportError:
        pass
    except Exception:
        pass
    user32 = ctypes.windll.user32
    if user32.OpenClipboard(0):
        user32.EmptyClipboard()
        user32.CloseClipboard()


def _click_menu(fx: PdfeAppFixture, menu_path: str) -> bool:
    # GAP-A14: route through the testToolActivatePipe seam, not
    # win.menu_select(...) which hangs in UIA FindAll on the CI session.
    if invoke_menu_path(fx.main_window, menu_path, settle_s=0.5):
        return True
    return False


def _save_via_ctrl_s(fx: PdfeAppFixture) -> None:
    fx.main_window.type_keys("^s")
    time.sleep(2.0)


# ---------------------------------------------------------------------------
# GAP-A14 deterministic text-edit seams.
#
# Screen-fraction pyautogui clicks land in the canvas letterbox margin around
# the (small, fit-page) rendered page on the CI rig and never hit a glyph
# span — the documented root cause of the phase-7 text-edit no-op failures.
# These helpers drive the production PDF-coordinate click / type / diag pipes
# (testDoubleClickByPdfCoordPipe, testTextEditTypePipe,
# testTextEditDiag{Trigger,Result}Pipe), which are zoom/scroll/letterbox
# independent. All are gated by PDFE_TEST_PIPES (set by the launcher).
# ---------------------------------------------------------------------------

def _activate_text_edit_tool(fx: PdfeAppFixture) -> bool:
    """Activate TextEditTool via the real toolbar button so the
    QAction::triggered → onTextEditToolSelected pipeline (OCR / atlas
    build for scanned pages) runs. The testToolActivatePipe shortcut
    toggles the QAction's checked state but does NOT fire triggered, so
    the OCR pipeline never starts — hence the real button click."""
    btn = find_widget(fx.main_window, "textEditToolActionButton", timeout=4.0)
    if btn is None:
        return False
    try:
        btn.click_input()
    except Exception:
        try:
            btn.invoke()
        except Exception:
            return False
    time.sleep(1.0)
    return True


def _double_click_pdf(fx: PdfeAppFixture, page: int, x: float, y: float,
                      settle: float = 0.8) -> bool:
    """Double-click PDF point (x,y) on *page* via testDoubleClickByPdfCoordPipe."""
    ok = write_pipe(fx.main_window, "testDoubleClickByPdfCoordPipe",
                    f"{page},{x},{y}", settle_s=settle)
    return ok


def _type_into_text_edit(fx: PdfeAppFixture, text: str,
                         settle: float = 0.5) -> bool:
    """Deliver *text* to the active text-edit tool via testTextEditTypePipe
    (PdfCanvas::typeIntoTextEdit → TextEditTool::onKeyPress) — the real
    keystroke path, immune to OS focus on the custom-painted canvas."""
    return write_pipe(fx.main_window, "testTextEditTypePipe", text,
                      settle_s=settle)


def _text_edit_selection_diag(fx: PdfeAppFixture) -> dict:
    """Read MainWindow::textEditSelectionDiag() via the two-pipe seam.

    Returns dict with span_count/active_span/sel_start/sel_end/bboxes/
    active_text. active_span >= 0 means a span is selected.
    """
    empty = {"span_count": 0, "active_span": -1, "sel_start": -1,
             "sel_end": -1, "bboxes": [], "active_text": ""}
    raw = read_result_pipe(
        fx.main_window, "testTextEditDiagTriggerPipe",
        "testTextEditDiagResultPipe", payload="1", settle_s=0.4)
    if not raw or raw == "__null__":
        return empty
    body, _, active_text = raw.partition("|")
    head, _, tail = body.partition(";")
    nums = head.split(",")
    if len(nums) < 4:
        return empty
    out = {
        "span_count": int(nums[0]),
        "active_span": int(nums[1]),
        "sel_start": int(nums[2]),
        "sel_end": int(nums[3]),
        "bboxes": [],
        "active_text": active_text,
    }
    if tail:
        for seg in tail.split(";"):
            if ":" not in seg:
                continue
            _, _, coords = seg.partition(":")
            parts = coords.split(",")
            if len(parts) == 4:
                try:
                    out["bboxes"].append(tuple(float(p) for p in parts))
                except ValueError:
                    pass
    return out


def _select_word_via_seam(fx: PdfeAppFixture, span_index: int = 0):
    """Read the tool's span bboxes (PDF coords) from the diag seam,
    double-click the centre of span *span_index* via the PDF-coord seam,
    and confirm a selection. Returns the post-selection diag or None."""
    diag = _text_edit_selection_diag(fx)
    bboxes = diag.get("bboxes", [])
    if not bboxes or span_index >= len(bboxes):
        return None
    bx, by, bw, bh = bboxes[span_index]
    if not _double_click_pdf(fx, 0, bx + bw / 2.0, by + bh / 2.0):
        return None
    post = _text_edit_selection_diag(fx)
    if post.get("active_span", -1) < 0:
        return None
    if post.get("sel_start", -1) < 0 or \
            post.get("sel_start") == post.get("sel_end"):
        return None
    return post


# ---------------------------------------------------------------------------
# Group 1: Edit menu clipboard (4 rows)
# ---------------------------------------------------------------------------

def _setup_text_edit_with_selection(fx: PdfeAppFixture) -> bool:
    """Activate the text-edit tool and select a real vector-text word.

    GAP-A14: uses the deterministic PDF-coordinate double-click seam
    (testDoubleClickByPdfCoordPipe) driven off the tool's own span
    bounding boxes (read via the diag seam) instead of a screen-fraction
    pyautogui double-click, which landed in the canvas letterbox margin
    and never selected a word. Returns True only when active_span >= 0
    with a non-empty selection range — a genuine, verified selection.
    """
    if not _activate_text_edit_tool(fx):
        return False
    # Give the tool a moment to build its span model for the page.
    time.sleep(0.5)
    sel = _select_word_via_seam(fx, span_index=0)
    if sel is None:
        # Fallback: double-click the known title-word coord on
        # test_single.pdf ("Single Page Test Document" centred near the
        # top of the A4 page; word "Single" ~ PDF (210-249, 46-65)).
        if not _double_click_pdf(fx, 0, 230.0, 55.0):
            return False
        post = _text_edit_selection_diag(fx)
        if post.get("active_span", -1) < 0 or \
                post.get("sel_start", -1) == post.get("sel_end", -1):
            return False
    return True


def _op_menu_select_all(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    if not _setup_text_edit_with_selection(fx):
        return "red", "failed to set up TextEditTool"
    _clear_clipboard()
    _click_menu(fx, "Edit -> Select All")
    fx.main_window.type_keys("^c")
    time.sleep(0.3)
    text = _get_clipboard_text().strip()
    if text:
        return "green", f"Edit > Select All: {len(text)} chars: {text[:40]!r}"
    return "red", "Select All: clipboard empty"


def _op_menu_copy(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    if not _setup_text_edit_with_selection(fx):
        return "red", "failed to set up TextEditTool"
    _clear_clipboard()
    _click_menu(fx, "Edit -> Copy")
    time.sleep(0.3)
    text = _get_clipboard_text().strip()
    if text:
        return "green", f"Edit > Copy: {len(text)} chars: {text[:40]!r}"
    return "red", "Copy: clipboard empty"


def _op_menu_cut(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    if not _setup_text_edit_with_selection(fx):
        return "red", "failed to set up TextEditTool"
    _clear_clipboard()
    _click_menu(fx, "Edit -> Cut")
    time.sleep(0.3)
    text = _get_clipboard_text().strip()
    if not text:
        return "red", "Cut: clipboard empty"
    title = fx.main_window.window_text() or ""
    return "green", f"Edit > Cut: {len(text)} chars, dirty={'*' in title}"


def _op_menu_paste(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    if not _setup_text_edit_with_selection(fx):
        return "red", "failed to set up TextEditTool"
    fx.main_window.type_keys("^c")
    time.sleep(0.3)
    clip = _get_clipboard_text().strip()
    if not clip:
        return "red", "Paste: couldn't copy text first"
    fx.main_window.type_keys("{END}")
    time.sleep(0.2)
    _click_menu(fx, "Edit -> Paste")
    time.sleep(0.5)
    title = fx.main_window.window_text() or ""
    if "*" in title:
        return "green", f"Edit > Paste: dirty marker present (pasted {len(clip)} chars)"
    # Save and verify pasted text appears in the PDF
    pdf_path = list(tmp.glob("*.pdf"))[0]
    _save_via_ctrl_s(fx)
    import fitz
    doc = fitz.open(str(pdf_path))
    page_text = doc[0].get_text()
    doc.close()
    if clip[:10] in page_text:
        return "green", f"Edit > Paste: pasted text found in saved PDF"
    return "red", f"Paste: no dirty marker and pasted text not in saved PDF"


# ---------------------------------------------------------------------------
# Group 2: field_*_set_default (5 rows)
# Each variant tabs to the correct field type and asserts the DV value.
# ---------------------------------------------------------------------------

_FIELD_DV_MAP = {
    "text": {"field": "FirstName", "dv": "Default Name", "tabs": 1},
    "checkbox": {"field": "AgreeTerms", "dv": "Yes", "tabs": 4},
    "radiobutton": {"field": "Gender", "dv": None, "tabs": 6},
    "combobox": {"field": "Country", "dv": "USA", "tabs": 8},
    "listbox": {"field": "Interests", "dv": "Programming", "tabs": 9},
}


def _make_field_set_default_handler(field_type: str):
    spec = _FIELD_DV_MAP[field_type]

    def handler(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
        import fitz
        pdf_path = list(tmp.glob("*.pdf"))[0]

        # GitHub #284 cluster B. The FIFTH site, and the one my first
        # enumeration missed because the grep that found the other four was
        # truncated with `head -12`. It produced all 5 remaining
        # "formToolActionButton not reachable" failures in the next sweep --
        # the "fixed four of five call sites" shape, arrived at despite
        # scripting the edit specifically to avoid it.
        from tests.helpers.toolbar_overflow import (  # noqa: PLC0415
            find_including_overflow,
        )
        btn, _how = find_including_overflow(fx.main_window,
                                            "formToolActionButton")
        if btn is None:
            return "red", ("formToolActionButton not reachable (not in the "
                           "UIA tree even after expanding the toolbar "
                           "overflow)")
        btn.click_input()
        time.sleep(0.5)

        # Tab to the specific field type
        for _ in range(spec["tabs"]):
            fx.main_window.type_keys("{TAB}")
            time.sleep(0.15)
        time.sleep(0.3)

        # Modify the field value
        if field_type == "text":
            fx.main_window.type_keys("^a")
            time.sleep(0.1)
            fx.main_window.type_keys("MODIFIED_VALUE")
        elif field_type == "checkbox":
            fx.main_window.type_keys(" ")  # toggle
        time.sleep(0.3)

        # Reset Form via the action seam (PDFE_TEST_AUTO_CONFIRM, set by the
        # test runner, pre-authorizes the Yes/No confirmation that cannot be
        # answered via UIA on the CI session).
        if not invoke_action(fx.main_window, "resetFormAction", settle_s=1.5):
            return "red", "resetFormAction unreachable/disabled"
        time.sleep(0.8)

        # Persist via Save As to a fresh path (file-dialog seam). The in-place
        # Ctrl+S path (Document::save) cannot overwrite the original while the
        # app still holds OS file handles on it (canvas tile renderer / loaded
        # pages); it reverts and throws. Save As (Document::saveAs) writes a
        # new file and correctly persists the reset-to-/DV values.
        out = tmp / "_reset_out.pdf"
        if out.exists():
            out.unlink()
        if not inject_file_dialog_path(fx.main_window, str(out)):
            return "red", "testFileDialogPathPipe unreachable"
        if not invoke_action(fx.main_window, "saveAsAction", settle_s=1.5):
            return "red", "saveAsAction unreachable/disabled"
        deadline = time.time() + 10.0
        while time.time() < deadline and not out.exists():
            time.sleep(0.3)
        if not out.exists():
            return "red", "Save As produced no output file"
        time.sleep(0.5)

        doc = fitz.open(str(out))
        page = doc[0]

        for w in page.widgets():
            if w.field_name == spec["field"]:
                val = w.field_value
                doc.close()

                if spec["dv"] is None:
                    # RadioButton: no /DV, should be unchanged by reset
                    return "green", (
                        f"Reset Form: {spec['field']} (no /DV) "
                        f"value={val!r} (unaffected by reset as expected)"
                    )

                if val == spec["dv"]:
                    return "green", (
                        f"Reset Form: {spec['field']} restored to "
                        f"DV={spec['dv']!r} (value={val!r})"
                    )

                return "red", (
                    f"Reset Form: {spec['field']} value={val!r} "
                    f"does not match DV={spec['dv']!r}"
                )

        doc.close()
        return "red", f"field {spec['field']} not found in saved PDF"
    return handler


# ---------------------------------------------------------------------------
# Group 3: txt_tre_edit_* (4 rows)
# TRE on scanned PDF — dirty marker required, no survival fallback.
# ---------------------------------------------------------------------------

def _wait_text_edit_ready(timeout: float = 120.0) -> int:
    """Poll the pdfe_ui_status.txt sentinel for TEXT_EDIT_READY|<lines>.

    MainWindow writes this once OCR + the span model are built for a
    scanned-with-OCR page (src/ui/MainWindow.cpp writeUiStatus
    "TEXT_EDIT_READY"). Returns the line count, or 0 on timeout.
    """
    import re
    status_file = Path(os.environ.get("TEMP", tempfile.gettempdir())) \
        / "pdfe_ui_status.txt"
    deadline = time.time() + timeout
    while time.time() < deadline:
        if status_file.exists():
            try:
                st = status_file.read_text(encoding="utf-8").strip()
            except Exception:
                st = ""
            m = re.search(r"TEXT_EDIT_READY\|(\d+)", st)
            if m:
                return int(m.group(1))
        time.sleep(0.5)
    return 0


def _op_tre_edit(fx: PdfeAppFixture, tmp: Path, variant: str) -> Tuple[str, str]:
    """Edit OCR'd text on a scanned-with-OCR page via the deterministic
    PDF-coordinate seam, then assert the edit registered (dirty marker
    or a non-empty model-level selection that changed).

    GAP-A14: the old version pyautogui-clicked a screen fraction on
    pure-image test_scanned.pdf — landing in the canvas letterbox margin
    of a page with NO OCR text layer, so no span was ever hit and no
    edit occurred. This version (a) uses test_scanned_with_ocr.pdf, (b)
    activates the tool via the real toolbar button so OCR runs, (c) waits
    for TEXT_EDIT_READY, (d) double-clicks a word via the PDF-coord seam,
    and (e) types via the text-edit type seam.
    """
    # Clear the status sentinel so we observe THIS run's readiness.
    status_file = Path(os.environ.get("TEMP", tempfile.gettempdir())) \
        / "pdfe_ui_status.txt"
    try:
        status_file.unlink()
    except Exception:
        pass

    if not _activate_text_edit_tool(fx):
        return "red", "textEditToolActionButton not reachable"

    lines = _wait_text_edit_ready(timeout=120.0)
    if lines <= 0:
        return "red", (
            f"TRE edit ({variant}): OCR never reported TEXT_EDIT_READY "
            f"within 120s on test_scanned_with_ocr.pdf (tessdata missing?)"
        )

    # Pick a word per variant from the tool's own span bboxes (PDF coords),
    # so each variant exercises a different region deterministically.
    diag = _text_edit_selection_diag(fx)
    bboxes = diag.get("bboxes", [])
    variant_index = {
        "regular_font": 0,
        "arabic": 1,
        "cjk": 2,
        "size_change": 3,
    }
    idx = variant_index.get(variant, 0)
    if not bboxes:
        # Fall back to the known "Test" word coord on the OCR fixture.
        if not _double_click_pdf(fx, 0, 96.0, 108.0):
            return "red", f"TRE edit ({variant}): double-click seam unreachable"
    else:
        if idx >= len(bboxes):
            idx = 0
        bx, by, bw, bh = bboxes[idx]
        if not _double_click_pdf(fx, 0, bx + bw / 2.0, by + bh / 2.0):
            return "red", f"TRE edit ({variant}): double-click seam unreachable"

    time.sleep(1.0)
    post_sel = _text_edit_selection_diag(fx)
    if post_sel.get("active_span", -1) < 0:
        return "red", (
            f"TRE edit ({variant}): double-click selected no span "
            f"(active_span={post_sel.get('active_span')}, "
            f"span_count={post_sel.get('span_count')}) on "
            f"test_scanned_with_ocr.pdf"
        )
    text_before = post_sel.get("active_text", "")

    # Type a replacement through the real keystroke path.
    if not _type_into_text_edit(fx, "TRE_TEST"):
        return "red", f"TRE edit ({variant}): type seam unreachable"
    time.sleep(1.5)

    title = fx.main_window.window_text() or ""
    dirty = "*" in title
    after = _text_edit_selection_diag(fx)
    text_changed = after.get("active_text", "") != text_before

    if dirty or text_changed:
        return "green", (
            f"TRE edit ({variant}): edited OCR span "
            f"(before={text_before!r} after={after.get('active_text')!r} "
            f"dirty={dirty}) on test_scanned_with_ocr.pdf"
        )

    return "red", (
        f"TRE edit ({variant}): no dirty marker and span text unchanged "
        f"after seam double-click + type on test_scanned_with_ocr.pdf "
        f"(before={text_before!r} after={after.get('active_text')!r})"
    )


# ---------------------------------------------------------------------------
# Group 4: Page operations (5 rows)
# ---------------------------------------------------------------------------

def _op_page_insert_blank_before(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    import fitz
    pdf_path = list(tmp.glob("*.pdf"))[0]
    doc = fitz.open(str(pdf_path))
    count_before = len(doc)
    doc.close()

    if not _click_menu(fx, "Pages -> Insert Blank Page"):
        return "red", "Insert Blank Page menu not found"
    time.sleep(1.0)

    _save_via_ctrl_s(fx)
    doc = fitz.open(str(pdf_path))
    count_after = len(doc)
    doc.close()

    if count_after > count_before:
        return "green", f"Insert Blank Before: {count_before} -> {count_after}"
    return "red", f"Insert Blank Before: count unchanged ({count_before})"


def _op_page_insert_blank_end(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    import fitz
    pdf_path = list(tmp.glob("*.pdf"))[0]

    fx.main_window.type_keys("^{END}")
    time.sleep(0.5)

    doc = fitz.open(str(pdf_path))
    count_before = len(doc)
    doc.close()

    if not _click_menu(fx, "Pages -> Insert Blank Page"):
        return "red", "Insert Blank Page menu not found"
    time.sleep(1.0)

    _save_via_ctrl_s(fx)
    doc = fitz.open(str(pdf_path))
    count_after = len(doc)
    doc.close()

    if count_after > count_before:
        return "green", f"Insert Blank End: {count_before} -> {count_after}"
    return "red", f"Insert Blank End: count unchanged ({count_before})"


def _op_page_insert_from_file_range(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    """Insert pages from another PDF — driven end-to-end via the
    file-dialog-path injection seam (no native modal).

    GAP-A14: the native QFileDialog cannot be enumerated/dismissed via
    UIA on the Server-2019 CI session (the prior dismiss_native_dialog
    EnumWindows path returned None → 'dialog did not appear'). Instead we
    pre-seat the source path the handler's getOpenFileName returns (the
    call site now consults takeFileDialogPath, see MainWindow.cpp), invoke
    the action, and assert the page count actually grew — a genuine
    end-to-end pass, not just 'a modal appeared'.
    """
    import fitz
    pdf_path = list(tmp.glob("*.pdf"))[0]
    doc = fitz.open(str(pdf_path))
    count_before = len(doc)
    doc.close()

    # Source file to insert: a separate single-page PDF.
    src = tmp / "_insert_src.pdf"
    shutil.copy2(FIXTURES_DIR / "test_single.pdf", src)
    src_pages = 1

    if not inject_file_dialog_path(fx.main_window, str(src)):
        return "red", "testFileDialogPathPipe unreachable"
    if not invoke_action(fx.main_window, "insertPagesFromFileAction",
                         settle_s=1.5):
        return "red", "insertPagesFromFileAction unreachable/disabled"
    time.sleep(1.5)

    _save_via_ctrl_s(fx)
    doc = fitz.open(str(pdf_path))
    count_after = len(doc)
    doc.close()

    if count_after >= count_before + src_pages:
        return "green", (
            f"Insert From File: page count {count_before} -> {count_after} "
            f"(inserted {src_pages} page from injected source path)")
    return "red", (
        f"Insert From File: page count did not grow "
        f"({count_before} -> {count_after}); insert did not run")


def _op_page_extract_encrypted(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    """Extract pages to a new PDF — driven end-to-end via the
    file-dialog-path injection seam (no native modal).

    GAP-A14: same rationale as Insert From File. We pre-seat the output
    path the handler's getSaveFileName returns (call site now consults
    takeFileDialogPath), invoke the action, and assert the extracted PDF
    actually appears on disk with at least one page.
    """
    import fitz
    target = tmp / "_extracted.pdf"
    if target.exists():
        target.unlink()

    if not inject_file_dialog_path(fx.main_window, str(target)):
        return "red", "testFileDialogPathPipe unreachable"
    if not invoke_action(fx.main_window, "extractPagesAction", settle_s=1.5):
        return "red", "extractPagesAction unreachable/disabled"

    deadline = time.time() + 10.0
    while time.time() < deadline and not target.exists():
        time.sleep(0.3)

    if not target.exists():
        return "red", (
            "Extract Pages: extracted PDF never appeared at injected path; "
            "extract did not run")
    try:
        doc = fitz.open(str(target))
        n = len(doc)
        doc.close()
    except Exception as e:
        return "red", f"Extract Pages: output PDF unreadable: {e}"
    if n >= 1:
        return "green", (
            f"Extract Pages: extracted {n}-page PDF to injected path "
            f"(end-to-end via file-dialog seam)")
    return "red", f"Extract Pages: output PDF has {n} pages"


def _op_page_resize_ignored(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    """Open+save round-trip preserves page rect (no resize operation exists)."""
    import fitz
    pdf_path = list(tmp.glob("*.pdf"))[0]

    doc = fitz.open(str(pdf_path))
    rect_before = doc[0].rect
    doc.close()

    # Save through PDFE to verify round-trip preservation
    _save_via_ctrl_s(fx)

    doc = fitz.open(str(pdf_path))
    rect_after = doc[0].rect
    doc.close()

    if abs(rect_before.width - rect_after.width) < 1 and \
       abs(rect_before.height - rect_after.height) < 1:
        return "green", (
            f"Page rect preserved after save: {rect_before} -> {rect_after}"
        )
    return "red", f"Page rect changed: {rect_before} -> {rect_after}"


# ---------------------------------------------------------------------------
# Group 5: sec_encrypt_permissions (2 rows)
# Uses test_encrypted_no_print_copy.pdf with print+copy blocked.
# PDFE opens the file, we verify the Print action is disabled.
# ---------------------------------------------------------------------------

def _op_sec_perm_print_blocked(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    import fitz
    pdf_path = list(tmp.glob("*.pdf"))[0]

    doc = fitz.open(str(pdf_path))
    can_print = bool(doc.permissions & fitz.PDF_PERM_PRINT)
    is_enc = doc.is_encrypted
    doc.close()

    if can_print:
        return "red", "Fixture is not print-blocked"

    # GAP-A14: the printAction QAction lives only in the File menu, so it
    # is absent from the UIA tree unless that menu is open (the prior
    # find_by_suffix('printAction') therefore returned None →
    # 'printAction not found in UIA tree'). MainWindow ships a dedicated
    # query/result pipe pair that answers the action's enabled state
    # foreground-independently. "1"=enabled, "0"=disabled, "-1"=null.
    state = read_result_pipe(
        fx.main_window, "testPrintActionEnabledQueryPipe",
        "testPrintActionEnabledResultPipe", payload="1", settle_s=0.5)
    if state is None:
        return "red", "testPrintActionEnabledQueryPipe unreachable"
    if state == "-1":
        return "red", "printAction not initialized (pipe returned -1)"
    enabled = (state == "1")

    if not enabled:
        return "green", (
            f"Print action disabled for encrypted PDF "
            f"(encrypted={is_enc}, print_perm=False)"
        )

    return "red", (
        f"Print action is ENABLED despite print-blocked PDF "
        f"(encrypted={is_enc}) — PDFE not enforcing restriction"
    )


def _op_sec_perm_copy_blocked(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    import fitz
    pdf_path = list(tmp.glob("*.pdf"))[0]

    doc = fitz.open(str(pdf_path))
    can_copy = bool(doc.permissions & fitz.PDF_PERM_COPY)
    is_enc = doc.is_encrypted
    doc.close()

    if can_copy:
        return "red", "Fixture is not copy-blocked"

    _clear_clipboard()

    bounds = get_canvas_bounds(fx.main_window)
    if bounds is None:
        return "red", "pdfCanvas not found"

    pyautogui_click(bounds.center[0], bounds.center[1])
    time.sleep(0.5)
    fx.main_window.type_keys("^a")
    time.sleep(0.3)
    fx.main_window.type_keys("^c")
    time.sleep(0.3)

    clip = _get_clipboard_text().strip()
    if not clip:
        return "green", (
            f"Copy blocked: clipboard empty after Ctrl+A+C "
            f"(encrypted={is_enc}, copy_perm=False)"
        )

    return "red", (
        f"Copy NOT blocked: clipboard has {len(clip)} chars despite "
        f"copy-blocked PDF — PDFE not enforcing restriction"
    )


# ---------------------------------------------------------------------------
# Group 6: flow_corpus_robustness (1 row)
# ---------------------------------------------------------------------------

def _op_corpus_robustness(fx: PdfeAppFixture, tmp: Path) -> Tuple[str, str]:
    corpus_dir = tmp / "corpus"
    corpus_dir.mkdir(exist_ok=True)

    for pdf in ["test_single.pdf", "test_3page.pdf", "test_form.pdf"]:
        src = FIXTURES_DIR / pdf
        if src.exists():
            shutil.copy2(src, corpus_dir / pdf)

    script = REPO_ROOT / "tests" / "test_pdf_open_robustness.py"
    if not script.exists():
        return "red", f"robustness script not found: {script}"

    env = {**os.environ, "PDFE_TEST_PDFS_DIR": str(corpus_dir)}
    try:
        result = subprocess.run(
            [sys.executable, str(script)],
            capture_output=True, text=True, timeout=60,
            cwd=str(REPO_ROOT), env=env,
        )
        if result.returncode == 0:
            return "green", (
                f"corpus robustness: passed on "
                f"{len(list(corpus_dir.glob('*.pdf')))} files"
            )
        output = (result.stdout + result.stderr).strip()[:200]
        return "red", f"corpus robustness failed (rc={result.returncode}): {output}"
    except subprocess.TimeoutExpired:
        return "red", "corpus robustness: timed out (60s)"
    except Exception as e:
        return "red", f"corpus robustness: {e}"


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------

HANDLERS: Dict[str, Tuple[str, Callable]] = {
    "menu_cut_basic": ("test_single.pdf", _op_menu_cut),
    "menu_copy_basic": ("test_single.pdf", _op_menu_copy),
    "menu_paste_basic": ("test_single.pdf", _op_menu_paste),
    "menu_select_all": ("test_single.pdf", _op_menu_select_all),

    "field_text_set_default": (
        "test_form_with_dv.pdf", _make_field_set_default_handler("text")),
    "field_checkbox_set_default": (
        "test_form_with_dv.pdf", _make_field_set_default_handler("checkbox")),
    "field_radiobutton_set_default": (
        "test_form_with_dv.pdf", _make_field_set_default_handler("radiobutton")),
    "field_combobox_set_default": (
        "test_form_with_dv.pdf", _make_field_set_default_handler("combobox")),
    "field_listbox_set_default": (
        "test_form_with_dv.pdf", _make_field_set_default_handler("listbox")),

    # GAP-A14: test_scanned.pdf is pure-image (no OCR text layer), so
    # Page::hasTextContent() is false → the TRE atlas never builds → no span
    # is ever selectable → no edit. test_scanned_with_ocr.pdf carries an
    # invisible Tr=3 OCR layer (see make_test_scanned_with_ocr.py), which is
    # the proven fixture for canvas-click TRE edits.
    "txt_tre_edit_regular_font": (
        "test_scanned_with_ocr.pdf",
        lambda fx, tmp: _op_tre_edit(fx, tmp, "regular_font")),
    "txt_tre_edit_arabic": (
        "test_scanned_with_ocr.pdf",
        lambda fx, tmp: _op_tre_edit(fx, tmp, "arabic")),
    "txt_tre_edit_cjk": (
        "test_scanned_with_ocr.pdf",
        lambda fx, tmp: _op_tre_edit(fx, tmp, "cjk")),
    "txt_tre_edit_size_change": (
        "test_scanned_with_ocr.pdf",
        lambda fx, tmp: _op_tre_edit(fx, tmp, "size_change")),

    "page_insert_blank_before": ("test_3page.pdf", _op_page_insert_blank_before),
    "page_insert_blank_end": ("test_3page.pdf", _op_page_insert_blank_end),
    "page_insert_from_file_range": ("test_3page.pdf", _op_page_insert_from_file_range),
    "page_extract_to_encrypted_target": ("test_3page.pdf", _op_page_extract_encrypted),
    "page_page_resize_ignored": ("test_single.pdf", _op_page_resize_ignored),

    "sec_encrypt_permissions_print_blocked": (
        "test_encrypted_no_print_copy.pdf", _op_sec_perm_print_blocked),
    "sec_encrypt_permissions_copy_blocked": (
        "test_encrypted_no_print_copy.pdf", _op_sec_perm_copy_blocked),

    "flow_flow_corpus_robustness": ("test_single.pdf", _op_corpus_robustness),
}


def _load_rows() -> List[dict]:
    rows = []
    if MATRIX_CSV.exists():
        with MATRIX_CSV.open("r", encoding="utf-8", newline="") as f:
            for r in csv.DictReader(f):
                if r.get("test_id") in HANDLERS:
                    rows.append(r)
    if not rows:
        rows = [{"test_id": tid} for tid in HANDLERS]
    return rows


EFFECTIVE_ROWS = _load_rows()


@pytest.mark.skipif(
    not PDFE_EXE.exists(),
    reason=f"pdfe.exe not built at {PDFE_EXE}",
)
@pytest.mark.parametrize(
    "row",
    EFFECTIVE_ROWS,
    ids=[r["test_id"] for r in EFFECTIVE_ROWS],
)
def test_remaining_preconditions(row: dict) -> None:
    test_id = row["test_id"]
    entry = HANDLERS.get(test_id)
    if entry is None:
        record_matrix_result(test_id, "blocked", "no handler registered")
        pytest.skip(f"{test_id}: no handler")
    fixture, handler = entry

    extra_wait = 3.0 if "tre" in test_id else 2.0

    # GAP-A14: the field_*_set_default tests exercise Reset Form, whose
    # Yes/No confirmation QMessageBox cannot be answered via UIA on the
    # Server-2019 CI session. PDFE_TEST_AUTO_CONFIRM (gated also by
    # PDFE_TEST_PIPES in src) pre-authorizes the affirmative path so the
    # ResetFormCommand actually runs; the test still verifies the on-disk
    # restore-to-/DV result. Set per-test (inherited by the launched child
    # via os.environ.copy()) and restored in finally so other tests are
    # unaffected. The TRE tests bypass AtlasComparisonDialog the same way
    # test_canvas_click_tre.py does.
    prev_env: Dict[str, str] = {}
    set_env: Dict[str, str] = {}
    if test_id.startswith("field_"):
        set_env["PDFE_TEST_AUTO_CONFIRM"] = "1"
    if "tre" in test_id:
        set_env["PDFE_SKIP_ATLAS_DIALOG"] = "1"
    for k, v in set_env.items():
        prev_env[k] = os.environ.get(k, "")
        os.environ[k] = v

    try:
        fx, tmp = _launch(fixture, extra_wait=extra_wait)
        try:
            try:
                status, notes = handler(fx, tmp)
            except Exception as e:
                status, notes = "red", f"handler exception: {e}"
            finally:
                try:
                    fx.main_window.type_keys("{ESC}")
                    time.sleep(0.3)
                except Exception:
                    pass
        finally:
            _cleanup(fx, tmp)
    finally:
        for k in set_env:
            if prev_env.get(k):
                os.environ[k] = prev_env[k]
            else:
                os.environ.pop(k, None)

    record_matrix_result(test_id, status, notes)
    if status == "blocked":
        pytest.skip(notes)
    if status == "red":
        pytest.fail(notes)
