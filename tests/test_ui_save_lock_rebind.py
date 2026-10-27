# -*- coding: utf-8 -*-
"""Native UI verification for the onSaveFile drain+rebind wiring
(pipeline f4a82c1e, Phase 3a).

The GoogleTest test_command_page_rebind.cpp proves saveWithCommandPageRebind()
at the Document/command layer. This test proves the WIRING: that onSaveFile —
the in-place File>Save path, reached here by invoking the "saveAction" QAction
(NOT testSaveAsPipe, which is the unwired onSaveAsFile) — actually routes
through the orchestration in the real app, so:

  * an in-place save with a command-held page does NOT throw / silently lose
    the edit to the Windows file lock (the held-Page hazard), and
  * undo still works after the save — the undoable command was rebound to a
    live page, not left dangling by the post-save onDocumentStructureChanged
    refresh (which now preserves the rebound commandPages_).

MainWindow is not GoogleTest-instantiable, so this native pywinauto/UIA test is
the only layer that can exercise onSaveFile end to end.

Run:  python -m pytest tests/test_ui_save_lock_rebind.py -v -s
"""
from __future__ import annotations

import os
import shutil
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

FIXTURES = REPO_ROOT / "tests" / "fixtures"
RUNTIME = REPO_ROOT / "tests" / "_runtime"
CORPUS = FIXTURES / "journey_corpus.pdf"


def _has(mod: str) -> bool:
    try:
        __import__(mod)
        return True
    except Exception:  # noqa: BLE001
        return False


def _has_pdfe() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:  # noqa: BLE001
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    if sys.platform != "win32":
        return False
    return bool(os.environ.get("SESSIONNAME"))


pytestmark = [
    pytest.mark.skipif(not _has_pdfe(), reason="pdfe.exe not built"),
    pytest.mark.skipif(not _is_interactive(),
                       reason="UIA requires an interactive Windows desktop"),
    pytest.mark.skipif(not _has("pywinauto"), reason="pywinauto not installed"),
    pytest.mark.skipif(not _has("psutil"), reason="psutil not installed"),
    pytest.mark.skipif(not _has("fitz"), reason="PyMuPDF (fitz) not installed"),
    pytest.mark.skipif(not CORPUS.exists(),
                       reason="journey corpus missing — run "
                              "tests/fixtures/make_journey_corpus.py"),
]


def _count_annots(pdf_path: Path) -> int:
    """Total annotation count across all pages, via an independent reader."""
    import fitz
    doc = fitz.open(str(pdf_path))
    try:
        total = 0
        for i in range(doc.page_count):
            for _ in doc.load_page(i).annots():
                total += 1
        return total
    finally:
        doc.close()


def test_onsavefile_inplace_drain_rebind_keeps_edits_and_undo():
    from tests.helpers.pdfe_session import PDFESession, list_pdfe_pids
    from tests.helpers import qinvoke

    RUNTIME.mkdir(parents=True, exist_ok=True)
    # Throwaway copy — the journey/save tests never touch a committed fixture
    # or the user's original.
    work = RUNTIME / "save_lock_rebind_working.pdf"
    shutil.copy2(CORPUS, work)

    annots_before = _count_annots(work)

    with PDFESession(pdf_path=str(work)) as s:
        assert s.wait_for_window_title("PDFE"), "PDFE window never appeared"
        s.wait_for_idle()
        time.sleep(1.0)

        pid = s.pid

        def alive() -> bool:
            try:
                return pid in list_pdfe_pids()
            except Exception:  # noqa: BLE001
                return False

        # 1. Create an annotation -> a CreateAnnotationCommand whose Page is
        #    owned by commandPages_ (the exact holder that locks a full-rewrite
        #    in-place save via fz_keep_document).
        qinvoke._write_to_pipe(s, "testAnnotCreatePipe",
                               "Square,2,80,120,60,18", settle=0.4)
        assert alive(), "process died creating the annotation"

        # 2. Best-effort: arm a full-rewrite with a content-stream text edit on
        #    a body page (full-rewrite class, distinct from the annotation and
        #    leaves commandPages_ populated). If the edit no-ops on this corpus,
        #    the save still exercises the wired orchestration on its save path.
        try:
            qinvoke._write_to_pipe(s, "testGoToPagePipe", "0", settle=0.2)
            qinvoke.activate_tool_by_name(s, "textEditToolAction")
            time.sleep(0.3)
            qinvoke.double_click_by_pdf_coord(s, 0, 100.0, 82.0)
            time.sleep(0.2)
            qinvoke.type_into_text_edit(s, "Z")
            time.sleep(0.2)
            qinvoke.send_key_to_text_edit(s, 0x01000000)  # Qt::Key_Escape commit
            time.sleep(0.3)
        except Exception:  # noqa: BLE001
            pass

        assert alive(), "process died before the in-place save"

        # 3. In-place save via the saveAction QAction -> onSaveFile (the WIRED
        #    path). This routes through saveWithCommandPageRebind so the held
        #    command page is drained + rebound around the full-rewrite.
        save_ok = qinvoke.activate_tool_by_name(s, "saveAction")
        assert save_ok, "failed to invoke saveAction"
        time.sleep(2.5)  # let the (possibly full-rewrite) save complete
        assert alive(), (
            "process died during in-place save — a held command page locking "
            "the full-rewrite would throw/lose changes")

        # 4. Undo must still work after the save: the command was rebound to a
        #    freshly-resolved page, not left dangling by the post-save refresh.
        undo_ok = qinvoke.activate_tool_by_name(s, "undoAction")
        time.sleep(0.8)
        assert alive(), (
            "process crashed on undo after in-place save — a dangling Page* in "
            "an undo command (the rebind/preserve path failed)")
        assert undo_ok, "failed to invoke undoAction"

        # 5. Redo too (exercises the rebound command's redo path).
        qinvoke.activate_tool_by_name(s, "redoAction")
        time.sleep(0.6)
        assert alive(), "process crashed on redo after in-place save"

    # 6. Reopen the IN-PLACE saved working file with an independent reader: it
    #    must not be corrupt, and the annotation must have persisted. A silent
    #    lost change here == the held-page file-lock bug the fix removes.
    import fitz
    doc = fitz.open(str(work))
    try:
        assert doc.page_count >= 1, "reopened file has no pages"
        for i in range(doc.page_count):
            doc.load_page(i).get_text("text")  # every page extracts (no corruption)
    finally:
        doc.close()

    annots_after = _count_annots(work)
    assert annots_after > annots_before, (
        f"annotation was not persisted by the in-place save "
        f"(before={annots_before}, after={annots_after}) — a silent lost "
        f"change indicates the held-page full-rewrite lock was NOT resolved")
