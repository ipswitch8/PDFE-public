# -*- coding: utf-8 -*-
"""Live diagnostic: report what the TextEditToolBar font combo actually
displays when the user clicks the title line on page 1 of the SpecimenA
Green PDF, before and after the first keystroke.

Why this exists
---------------
``tests/diagnose_pdf_fonts.py`` proved the PDF's actual MuPDF font names
are ``PalatinoLTPro-{Bold,Roman,Italic,BoldItalic}`` — no "Alef"
anywhere. ``tests/test_text_edit_toolbar.cpp`` proved the toolbar's
``setFontInfo`` / ``setSelectionFormatting`` codepaths display whatever
they are given verbatim. Putting those together: the toolbar should
display ``PalatinoLTPro-Bold`` on this PDF.

This script verifies that contract end-to-end against the live
PDFE.exe build. It is a diagnostic, not a regression test — its output
is the data we need to confirm "Alef" is no longer being shown.

Usage
-----
    python tests/diagnose_live_toolbar_font.py

Requires:
    - pywinauto + pyautogui + Pillow
    - An interactive Windows desktop (UIA needs a real session)
    - ``build/vs2022/bin/Debug/PDFE.exe`` (or the canonical
      ``build/windows-vs2022/Debug/PDFE.exe``)
    - The SpecimenA Green PDF (probed: env override, repo fixture,
      user's Desktop)

Output
------
Prints the font combo's displayed text at three checkpoints:
    1. After click into title (no keystroke)
    2. After typing 'X' (which is missing from the Palatino subset
       on this PDF — exactly the case the user originally reported)
    3. After clearing focus (sanity check)

A non-zero exit indicates a launch / locate failure.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))


def _resolve_pdfe_exe() -> Path | None:
    candidates = [
        REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "PDFE.exe",
        REPO_ROOT / "build" / "windows-vs2022" / "Debug" / "PDFE.exe",
        REPO_ROOT / "build" / "vs2022" / "bin" / "Release" / "PDFE.exe",
        REPO_ROOT / "build" / "windows-vs2022" / "Release" / "PDFE.exe",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def _find_specimen_a_pdf() -> Path | None:
    override = os.environ.get("PDFE_SPECIMEN_A_GREEN_PDF")
    if override and Path(override).exists():
        return Path(override)
    fixture = REPO_ROOT / "tests" / "fixtures" / "specimen_a_green.pdf"
    if fixture.exists():
        return fixture
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if home:
        desktop = Path(home) / "Desktop"
        if desktop.exists():
            for c in desktop.glob("SpecimenA*Green*.pdf"):
                return c
    return None


def _read_combo_text(combo):
    """Return the QFontComboBox displayed text from a cached reference.

    Reads the embedded Edit child first (Qt 6 exposes the line edit
    as an Edit subnode whose Name property is the displayed text);
    falls back to the combo's own window_text and legacy Value.
    """
    if combo is None:
        return None
    try:
        edit = combo.child_window(control_type="Edit")
        edit.wait("exists", timeout=2)
        t = edit.window_text()
        if t:
            return t
    except Exception:
        pass
    try:
        t = combo.window_text()
        if t:
            return t
    except Exception:
        pass
    try:
        return combo.legacy_properties().get("Value", "") or None
    except Exception:
        return None


def main() -> int:
    pdfe_exe = _resolve_pdfe_exe()
    if pdfe_exe is None:
        print("[diag] PDFE.exe not found in any known build layout",
              file=sys.stderr)
        return 2

    pdf = _find_specimen_a_pdf()
    if pdf is None:
        print("[diag] SpecimenA Green PDF not found "
              "(set $PDFE_SPECIMEN_A_GREEN_PDF or place under "
              "~/Desktop/SpecimenA*.pdf)", file=sys.stderr)
        return 2

    print(f"[diag] PDFE.exe : {pdfe_exe}")
    print(f"[diag] PDF      : {pdf}")

    try:
        from pywinauto import Application
        from tests.helpers.keys import typewrite_to, press_to  # type: ignore
        from tests.ui_automation.harness.canvas_click import (
            click_pdf_point,
        )
    except ImportError as exc:
        print(f"[diag] missing dependency: {exc}", file=sys.stderr)
        return 2

    proc = subprocess.Popen([str(pdfe_exe), str(pdf)])
    try:
        time.sleep(2.5)  # let MainWindow + first-page render settle
        app = Application(backend="uia").connect(
            process=proc.pid, timeout=15
        )
        win = app.window(title_re=r".*PDFE.*")
        win.wait("visible enabled", timeout=15)
        win.set_focus()
        time.sleep(0.3)

        # Activate the text-edit tool. The annotation toolbar exposes
        # tool-mode actions as checkable CheckBoxes (because each
        # QAction is checkable), with auto_id ending in
        # "ActionButton". Use click_input to physically dispatch the
        # button — UIA's toggle() on Qt CheckBoxes flips IsChecked but
        # may not fire QAction::triggered, so the tool wouldn't
        # actually activate.
        try:
            cb = win.child_window(
                auto_id="QApplication.mainWindow.annotationToolBar."
                        "textEditToolActionButton",
                control_type="CheckBox",
            )
            cb.wait("exists visible enabled", timeout=10)
            cb.click_input()
            print("[diag] activated text-edit tool via "
                  "textEditToolActionButton.click_input()")
        except Exception as exc:
            print(f"[diag] textEditToolActionButton.click_input() "
                  f"failed: {exc!r}", file=sys.stderr)
            return 3

        # The TextEditToolBar is hidden until MainWindow finishes its
        # font-detection / atlas-build pipeline, then setVisible(true)
        # at line 5064 in MainWindow.cpp. On a 51-page subsetted
        # vector PDF this can take several seconds. Poll for the
        # combo's appearance up to 30 s before giving up.
        deadline = time.time() + 30.0
        font_combo = None
        while time.time() < deadline:
            try:
                font_combo = win.child_window(
                    auto_id="QApplication.mainWindow.textEditToolBar."
                            "fontComboBox",
                    control_type="ComboBox",
                )
                font_combo.wait("exists", timeout=1)
                break
            except Exception:
                # Some Qt builds expose the combo without the dotted
                # path prefix; try the bare auto_id too.
                try:
                    font_combo = win.child_window(
                        auto_id="fontComboBox",
                        control_type="ComboBox",
                    )
                    font_combo.wait("exists", timeout=1)
                    break
                except Exception:
                    time.sleep(0.5)
        if font_combo is None or not font_combo.exists():
            print("[diag] TextEditToolBar.fontComboBox never became "
                  "visible — text-edit tool failed to fully activate",
                  file=sys.stderr)
            return 3
        print("[diag] TextEditToolBar visible; fontComboBox located")

        # Read the toolbar's "no span selected" default — this is
        # whatever clearFontInfo() set the combo to. If it's "Alef",
        # the bug is that clearFontInfo's setCurrentIndex(0) picks
        # the alphabetically-first installed system font.
        default_text = _read_combo_text(font_combo)
        print(f"[checkpoint 0: pre-click ]  fontComboBox = "
              f"{default_text!r}  (no span selected yet)")

        # Click the title line on page 1.  Page is 594x738pt; the
        # 50pt "Ka'amala's Greens" title is centred horizontally and
        # sits near the top of the page (PyMuPDF dump shows it as the
        # only 50pt run on page 1). Probe several Y positions across
        # the upper third of the page to make sure at least one click
        # lands inside a span.
        click_targets = [
            (297, 100),  # title-band 1
            (297, 130),  # title-band 2
            (297, 160),  # subtitle area
            (297, 220),  # author byline
            (200, 100),  # title-band, off-centre left
            (400, 100),  # title-band, off-centre right
        ]
        clicked_ok = False
        from tests.ui_automation.harness.canvas_click import canvas_region
        cr = canvas_region(win)
        print(f"[diag] canvas_region = {cr!r}")
        for px, py in click_targets:
            print(f"[diag] trying click at PDF point ({px}, {py})")
            ok = click_pdf_point(
                win, page_x=px, page_y=py,
                page_w_pt=594.0, page_h_pt=738.0,
            )
            if not ok:
                print(f"[diag]   canvas not resolvable")
                continue
            time.sleep(0.7)  # let selectSpan fire
            txt = _read_combo_text(font_combo)
            print(f"[diag]   after click: fontComboBox = {txt!r}")
            # If the combo's text changed from the default, a span
            # was selected.
            if txt and txt != default_text:
                print(f"[diag]   span selected — toolbar now shows "
                      f"{txt!r}")
                clicked_ok = True
                break
        if not clicked_ok:
            print("[diag] no click landed on a span — toolbar "
                  "stayed at default value. The 'Alef' you see "
                  "is the QFontComboBox's index-0 fallback from "
                  "clearFontInfo(), NOT a real PDF font name.")

        font_after_click = _read_combo_text(font_combo)
        print(f"[checkpoint 1: after click]  fontComboBox = "
              f"{font_after_click!r}")

        # Type 'X' — in the original bug report this triggered the
        # toolbar's font name to jump from "Alef" to "Tahoma".
        typewrite_to(win.handle, "X", interval=0.05)
        time.sleep(0.4)
        font_after_type = _read_combo_text(font_combo)
        print(f"[checkpoint 2: after type ]  fontComboBox = "
              f"{font_after_type!r}")

        # Press Escape to defocus the combo and re-read; a stable
        # final value is what the user actually sees.
        press_to(win.handle, "escape")
        time.sleep(0.3)
        font_after_esc = _read_combo_text(font_combo)
        print(f"[checkpoint 3: after esc  ]  fontComboBox = "
              f"{font_after_esc!r}")

        # Diagnosis.
        print()
        if font_after_click is None:
            print("[diag] could not read fontComboBox at all — "
                  "probably the text-edit tool didn't activate or the "
                  "click missed the title span")
            return 4

        looks_like_palatino = (
            "Palatino" in font_after_click
            or "PalatinoLTPro" in font_after_click
        )
        looks_like_alef = "Alef" in (font_after_click or "")
        looks_like_tahoma = "Tahoma" in (font_after_type or "")

        if looks_like_palatino and not looks_like_alef:
            print("[diag] PASS — toolbar shows the Palatino family "
                  "as expected. The 'Alef' bug from earlier is no "
                  "longer reproducible on the current build.")
            return 0
        if looks_like_alef:
            print("[diag] FAIL — toolbar still shows 'Alef'. The bug "
                  "is upstream of TextEditToolBar.setFontInfo (which "
                  "is unit-tested clean). Investigate "
                  "TextEditTool::selectSpan and TextExtractor's "
                  "fz_font_name path.")
            return 5
        if looks_like_tahoma and not looks_like_alef:
            print("[diag] PARTIAL — toolbar shows 'Tahoma' (Qt "
                  "QFontInfo substitute). The phase-7 setEditText "
                  "override may not be reaching this codepath.")
            return 5
        print("[diag] UNEXPECTED — none of Palatino / Alef / Tahoma "
              "matched. Inspect the values above and decide if this "
              "is a different bug.")
        return 1

    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
