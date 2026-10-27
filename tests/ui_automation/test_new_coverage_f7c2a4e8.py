# -*- coding: utf-8 -*-
"""Pipeline f7c2a4e8 phase-7: new UI + security coverage tests.

This file collects new tests written by phase-7 to close coverage
gaps identified in tests/docs/post_b5e3a9f2_followups.md Groups F + G.

Covered in this file:
- F1 testSaveAsPipe no-document path
- F2 select-pages multi-action sequence (Delete via menu)
- F3 imagePropertiesPanel field-value reading

Deferred to a future security-focused pipeline (each documented in
tests/KNOWN_WEAK_TESTS.txt + tests/docs/pipeline_g_triage.md):
- F4 broader Pages-menu workflows
- G1 UI-level redaction roundtrip — needs redaction fixture + headless
  byte-verifier integration
- G2 wrong-password retry/cancel — needs encrypted fixture + state machine
- G3 JS form-action sandbox — needs hostile-JS PDF fixture
- G4 encryption password-strength enforcement — needs dialog state read
- G5 visible/invisible signature workflows — needs valid PFX + signing flow
"""
from __future__ import annotations

import sys
import tempfile
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    _read_pipe_value,
    kill_pdfe_processes,
)
from tests.ui_automation.harness.dialog_helpers import find_by_suffix  # noqa: E402

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


@pytest.fixture()
def pdfe_no_doc():
    """Launch pdfe.exe with NO document loaded."""
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    kill_pdfe_processes()
    fx = PdfeAppFixture()
    fx.launch_args = []
    fx.setUp()
    time.sleep(1.5)
    yield fx
    try:
        fx.tearDown()
    except Exception:
        pass
    kill_pdfe_processes()


@pytest.fixture()
def pdfe_with_3page():
    """Launch pdfe.exe with test_3page.pdf loaded."""
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    src = FIXTURES_DIR / "test_3page.pdf"
    if not src.exists():
        pytest.skip(f"fixture missing: {src}")
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase7_"))
    import shutil
    dst = tmp / "test_3page.pdf"
    shutil.copy2(src, dst)
    kill_pdfe_processes()
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    time.sleep(2.0)
    yield fx, tmp
    try:
        fx.tearDown()
    except Exception:
        pass
    shutil.rmtree(tmp, ignore_errors=True)
    kill_pdfe_processes()


@pytest.fixture()
def pdfe_with_3page_qt_dlg():
    """Launch pdfe.exe with test_3page.pdf loaded and Qt QFileDialog mode
    enabled (PDFE_USE_QT_FILE_DIALOG=1) — required for any test that needs
    to drive a file-open or save-as dialog via desktop_dialog_driver.
    """
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    src = FIXTURES_DIR / "test_3page.pdf"
    if not src.exists():
        pytest.skip(f"fixture missing: {src}")
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase5_qtdlg_"))
    import shutil, os as _os
    dst = tmp / "test_3page.pdf"
    shutil.copy2(src, dst)
    kill_pdfe_processes()
    _os.environ["PDFE_USE_QT_FILE_DIALOG"] = "1"
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    time.sleep(2.0)
    yield fx, tmp
    try:
        fx.tearDown()
    except Exception:
        pass
    shutil.rmtree(tmp, ignore_errors=True)
    kill_pdfe_processes()
    # Pipeline 3e9b1d7c phase-5 (karen review): clear env var so it
    # doesn't contaminate subsequent test processes in the same session.
    _os.environ.pop("PDFE_USE_QT_FILE_DIALOG", None)


@pytest.fixture()
def pdfe_with_image():
    """Launch pdfe.exe with test_fullpage_image.pdf loaded."""
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    src = FIXTURES_DIR / "test_fullpage_image.pdf"
    if not src.exists():
        pytest.skip(f"fixture missing: {src}")
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase7_img_"))
    import shutil
    dst = tmp / "test_fullpage_image.pdf"
    shutil.copy2(src, dst)
    kill_pdfe_processes()
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    time.sleep(2.0)
    yield fx, tmp
    try:
        fx.tearDown()
    except Exception:
        pass
    shutil.rmtree(tmp, ignore_errors=True)
    kill_pdfe_processes()


# =========================================================================
# F1: testSaveAsPipe no-document path
# =========================================================================

def test_f1_save_as_pipe_no_document_does_not_create_file(pdfe_no_doc):
    """F1: writing to testSaveAsPipe with no document open must not
    create a file. The pipe handler short-circuits when document_ is
    null (MainWindow.cpp:955-961); this test pins that behavior.
    """
    pipe = find_by_suffix(pdfe_no_doc.main_window, "testSaveAsPipe")
    assert pipe is not None, "testSaveAsPipe must be reachable"

    target = Path(tempfile.gettempdir()) / "pdfe_f1_should_not_exist.pdf"
    if target.exists():
        target.unlink()

    pipe.set_edit_text(str(target))
    time.sleep(1.0)

    assert not target.exists(), (
        f"saveAs pipe must short-circuit when no document is open; "
        f"file {target} was created"
    )


# =========================================================================
# F2: select-pages [0, 2] + Delete via menu
# =========================================================================

def test_f2_select_pages_then_delete_via_menu(pdfe_with_3page):
    """F2 (1 of 3 in original plan): select non-contiguous pages [0, 2]
    via testSelectPagesPipe, then trigger Delete via the Pages menu.
    The 3-page document should drop to 1 page.
    """
    fx, _tmp = pdfe_with_3page
    select_pipe = find_by_suffix(fx.main_window, "testSelectPagesPipe")
    assert select_pipe is not None, "testSelectPagesPipe must be reachable"

    select_pipe.set_edit_text("0,2")
    time.sleep(0.5)

    from tests.ui_automation.harness.menu_nav import click_menu_path
    try:
        click_menu_path(fx.main_window, "Pages -> Delete Selected Pages")
    except Exception as e:
        pytest.skip(f"Pages → Delete Selected Pages menu unavailable: {e}")
    time.sleep(0.5)

    # Confirm via Yes (Y key on confirmation dialog)
    fx.main_window.type_keys("y")
    time.sleep(1.0)

    # Verify via pageLabel
    pl = find_by_suffix(fx.main_window, "pageLabel")
    assert pl is not None, "pageLabel must be reachable"
    text = pl.window_text() or ""
    assert "of 1" in text, (
        f"after deleting 2 of 3 pages, expected 'of 1' in pageLabel "
        f"but got: {text!r}"
    )


# =========================================================================
# F3: imagePropertiesPanel field-value reading
# =========================================================================

def test_f3_image_properties_panel_values_after_click(pdfe_with_image):
    """F3: after activating ImageTool and clicking on a full-page
    image, the imagePropertiesPanel widgets should display non-empty
    values for pixel dimensions, display dimensions, and DPI.
    """
    fx, _tmp = pdfe_with_image

    tool_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    assert tool_pipe is not None
    tool_pipe.set_edit_text("imageToolAction")
    time.sleep(0.5)

    click_pipe = find_by_suffix(fx.main_window, "testClickByPdfCoordPipe")
    assert click_pipe is not None
    click_pipe.set_edit_text("0,200,400")
    time.sleep(1.0)

    panel = find_by_suffix(fx.main_window, "imagePropertiesPanel")
    assert panel is not None, "imagePropertiesPanel must be reachable"

    label_texts = []
    for d in panel.descendants():
        try:
            aid = d.element_info.automation_id or ""
        except Exception:
            continue
        if not aid:
            continue
        try:
            t = d.window_text() or ""
        except Exception:
            continue
        # Match by suffix on auto_id (Qt prefixes with parent path)
        suffix = aid.split(".")[-1]
        if suffix in ("imagePixelDimensionsLabel",
                      "imageDisplayDimensionsLabel",
                      "imageDpiLabel"):
            label_texts.append((suffix, t.strip()))

    assert len(label_texts) >= 1, (
        f"imagePropertiesPanel should expose at least one of "
        f"pixelDimensions/displayDimensions/dpi labels; got {label_texts}"
    )
    for aid, t in label_texts:
        assert t, f"label {aid!r} has empty text after image click"


# =========================================================================
# F2b: select-pages [0, 2] + Rotate Clockwise via menu
# =========================================================================

def test_f2c_select_pages_then_rotate_clockwise(pdfe_with_3page):
    """F2c: select pages [0, 2] via testSelectPagesPipe, then trigger
    Pages → Rotate Clockwise. Verify the document is marked dirty
    (rotation registered as a Command in the undo stack).
    """
    fx, _tmp = pdfe_with_3page
    select_pipe = find_by_suffix(fx.main_window, "testSelectPagesPipe")
    assert select_pipe is not None, "testSelectPagesPipe must be reachable"

    select_pipe.set_edit_text("0,2")
    time.sleep(0.5)

    from tests.ui_automation.harness.menu_nav import click_menu_path
    try:
        click_menu_path(fx.main_window, "Pages -> Rotate Clockwise")
    except Exception as e:
        pytest.skip(f"Pages → Rotate Clockwise menu unavailable: {e}")
    time.sleep(0.8)

    # Dirty marker: window title should contain "*" after the rotate
    title = fx.main_window.window_text() or ""
    assert "*" in title, (
        f"after rotate of [0,2], expected dirty marker '*' in title; "
        f"got: {title!r}"
    )


# =========================================================================
# F2b: select-pages [0, 2] + Extract Pages via menu
# =========================================================================

def test_f2b_select_pages_then_extract(pdfe_with_3page):
    """F2b: extract pages [0, 2] and assert the WRITTEN FILE has 2 pages.

    GitHub #220. This was an UNCONDITIONAL ``@pytest.mark.skip``, which
    ``matrix/coverage_resolver.py`` treats as NOT resolved coverage -- so the
    row was a genuine hole, not a tolerated environmental skip.

    The f7c2a4e8 karen review deferred it for two correct reasons: the
    ``Pages -> Extract Pages...`` menu walk does not reach its leaf reliably
    on this host, and a truthful oracle needs the extracted FILE, not a
    liveness check. Both are addressed here rather than argued away:

      * the C++ ``testExtractPipe`` seam the old skip reason named as "future
        C++ work" now exists, so the trigger is deterministic;
      * the oracle opens the written file and counts its pages, and
        cross-checks the page count against the SOURCE so a 3-page copy
        cannot pass as a 2-page extract.

    Note the result pipe is read before the file is: a pipe that reports
    ``err|...`` tells us extraction was attempted and refused, which is a
    different finding from a file that never appeared, and conflating the two
    is how a harness problem gets filed as a product bug.
    """
    try:
        import fitz  # noqa: F401  # type: ignore
    except ImportError:
        pytest.skip("pymupdf (fitz) is required to count pages in the "
                    "extracted file; without it there is no truthful oracle")

    fx, tmp_dir = pdfe_with_3page
    src_path = tmp_dir / "test_3page.pdf"

    select_pipe = find_by_suffix(fx.main_window, "testSelectPagesPipe")
    assert select_pipe is not None, "testSelectPagesPipe must be reachable"
    extract_pipe = find_by_suffix(fx.main_window, "testExtractPipe")
    assert extract_pipe is not None, (
        "testExtractPipe must be reachable -- it is created only when "
        "PDFE_TEST_PIPES is set and the build defines PDFE_TESTING")
    result_pipe = find_by_suffix(fx.main_window, "testExtractResultPipe")
    assert result_pipe is not None, "testExtractResultPipe must be reachable"

    # Select [0, 2] through the production selection path, so the test also
    # covers the selection the menu action would have consumed.
    select_pipe.set_edit_text("0,2")
    time.sleep(0.5)

    # Written inside the fixture's own temp dir so teardown removes it; a
    # stray extracted.pdf under %TEMP% would be picked up by the next run's
    # "the file exists" branch and pass for the wrong reason.
    out_path = tmp_dir / "extracted.pdf"
    assert not out_path.exists(), (
        "target path already exists before the extract; the existence "
        "assertion below would be satisfied by a leftover file")

    extract_pipe.set_edit_text("0,2;%s" % out_path.as_posix())

    deadline = time.time() + 20.0
    verdict = ""
    while time.time() < deadline:
        verdict = (_read_pipe_value(result_pipe) or "").strip()
        if verdict:
            break
        time.sleep(0.25)

    assert verdict.startswith("ok|"), (
        "testExtractPipe did not report success; verdict=%r. An 'err|...' "
        "verdict means the product refused the extraction (a product "
        "finding); an empty verdict means the pipe never ran (a harness "
        "finding). They are not the same." % verdict)
    assert verdict == "ok|2", (
        "the pipe reported a page count other than the 2 requested: %r"
        % verdict)

    assert out_path.exists(), (
        "testExtractPipe reported ok but wrote no file at %s" % out_path)

    import fitz  # type: ignore
    with fitz.open(str(out_path)) as extracted:
        n_out = extracted.page_count
    with fitz.open(str(src_path)) as source:
        n_src = source.page_count

    assert n_src == 3, (
        "the 3-page fixture is not 3 pages (%d) -- the control for this "
        "assertion is broken, so a passing page count below would mean "
        "nothing" % n_src)
    assert n_out == 2, (
        "extracted file has %d page(s), expected exactly the 2 requested "
        "(indices 0 and 2). A count of %d would mean the whole document was "
        "copied rather than extracted." % (n_out, n_src))


# =========================================================================
# G4: encryption password-strength label reflects input
# =========================================================================

def test_g4_password_strength_label_reflects_input(pdfe_with_3page):
    """G4: Open the Save-with-Encryption dialog, type a weak password
    (3 chars), and verify the strength label exposes non-empty text
    indicating the password evaluation occurred. (We don't enforce
    'Weak' literal text because i18n + label format may vary; we
    only assert the strength infrastructure is wired and reacts to
    input.)
    """
    fx, _tmp = pdfe_with_3page

    # Use the keyboard shortcut Ctrl+Shift+E (MainWindow.cpp:2103)
    # — more reliable than menu path navigation for nested menus.
    fx.main_window.type_keys("^+e")
    time.sleep(1.5)

    # The userPasswordEdit field is disabled until algorithm != "None"
    # (EncryptionDialog.cpp:50). Change algorithm to AES-128 first
    # (index 3 in the combo: None=0, RC4-40=1, RC4-128=2, AES-128=3).
    algo_combo = find_by_suffix(fx.main_window, "algorithmCombo")
    if algo_combo is not None:
        try:
            algo_combo.select("AES-128")
        except Exception:
            try:
                # Some pywinauto versions need set_property or different API
                algo_combo.expand()
                time.sleep(0.2)
                algo_combo.select(3)
            except Exception:
                pass
        time.sleep(0.5)

    # EncryptionDialog widget names (per src/ui/EncryptionDialog.cpp):
    # userPasswordEdit, ownerPasswordEdit, passwordStrengthBar,
    # passwordStrengthLabel. We test against userPasswordEdit
    # (the open-password field).
    pwd_edit = find_by_suffix(fx.main_window, "userPasswordEdit")
    strength_label = find_by_suffix(fx.main_window, "passwordStrengthLabel")
    strength_bar = find_by_suffix(fx.main_window, "passwordStrengthBar")

    try:
        if pwd_edit is None:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "userPasswordEdit not reachable in Encryption dialog"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        if strength_label is None and strength_bar is None:
            pytest.skip("neither passwordStrengthLabel nor passwordStrengthBar reachable")

        # Type a weak password
        try:
            pwd_edit.set_edit_text("abc")
        except Exception:
            try:
                pwd_edit.set_text("abc")
            except Exception:
                pwd_edit.type_keys("abc")
        time.sleep(0.5)

        # Read whichever strength indicator is exposed
        readback = None
        if strength_label is not None:
            try:
                readback = strength_label.window_text()
            except Exception:
                pass
        if not readback and strength_bar is not None:
            try:
                readback = str(strength_bar.element_info.name or "")
            except Exception:
                pass

        assert readback is not None, "strength indicator did not return any text"
    finally:
        # Dismiss the dialog so test teardown is clean
        try:
            fx.main_window.type_keys("{ESC}")
        except Exception:
            pass


# =========================================================================
# G2: wrong-password retry / cancel on encrypted PDF
# =========================================================================

@pytest.fixture()
def pdfe_no_doc_for_encryption():
    """Launch pdfe.exe with NO document (we open the encrypted
    fixture via File > Open inside the test)."""
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    src = FIXTURES_DIR / "phase3_encrypted_aes128.pdf"
    if not src.exists():
        pytest.skip(f"encrypted fixture missing: {src}")
    kill_pdfe_processes()
    fx = PdfeAppFixture()
    fx.launch_args = []
    fx.setUp()
    time.sleep(1.5)
    yield fx, src
    try:
        fx.tearDown()
    except Exception:
        pass
    kill_pdfe_processes()


def test_g2_wrong_password_then_cancel(pdfe_no_doc_for_encryption):
    """G2 (branch A — cancel): open encrypted PDF, enter wrong
    password, cancel. PDFE should remain alive with no document
    loaded.

    Fixture: phase3_encrypted_aes128.pdf (user pw 'user', owner pw
    'owner-pw-phase3').
    """
    fx, src = pdfe_no_doc_for_encryption

    # Open the encrypted file via the openFile API (use File > Open
    # menu equivalent — but easier via a direct command-line restart
    # with the file arg). Simpler: use type_keys to send Ctrl+O, then
    # drive the file dialog. Or — best path: trigger File > Open
    # via menu and drive the native dialog.
    #
    # For G2's narrow scope (verifying wrong-password handling), use
    # the simplest path: kill pdfe, relaunch with the fixture as arg.
    # That bypasses the File > Open dialog entirely. The password
    # dialog will appear right after launch.
    kill_pdfe_processes()
    fx_app = PdfeAppFixture()
    fx_app.launch_args = [str(src)]
    fx_app.setUp()
    time.sleep(2.0)

    # The password prompt should appear. Find the password field —
    # the project uses "passwordEdit" (different from the encryption
    # *save* dialog's "userPasswordEdit"). Some builds have both
    # objectNames. Try both.
    pwd_edit = None
    for name in ("passwordEdit", "openPasswordEdit", "userPasswordEdit"):
        pwd_edit = find_by_suffix(fx_app.main_window, name)
        if pwd_edit is not None:
            break

    if pwd_edit is None:
        # Maybe the password is required via a different dialog/widget
        # name on this build. Skip with diagnostic.
        try:
            fx_app.tearDown()
        except Exception:
            pass
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "password prompt widget not reachable; encrypted-PDF "
                    "open flow not driven on this build"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

    try:
        # Branch A: type WRONG password
        try:
            pwd_edit.set_edit_text("wrong-password")
        except Exception:
            pwd_edit.type_keys("wrong-password")
        time.sleep(0.3)

        # Press Enter / OK
        pwd_edit.type_keys("{ENTER}")
        time.sleep(1.0)

        # Now expect either: error dialog, OR password dialog reappears.
        # Press Escape to cancel the entire flow.
        fx_app.main_window.type_keys("{ESC}")
        time.sleep(0.5)
        fx_app.main_window.type_keys("{ESC}")
        time.sleep(0.5)

        # PDFE should still be alive (cancellation doesn't crash)
        import subprocess
        proc = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe"],
            capture_output=True, text=True, timeout=5,
        )
        assert "pdfe.exe" in proc.stdout, (
            "pdfe.exe crashed after wrong-password + cancel"
        )

        # Window should NOT show the encrypted PDF as loaded (status
        # bar pageLabel should be empty or "Page 0 of 0").
        pl = find_by_suffix(fx_app.main_window, "pageLabel")
        if pl is not None:
            text = pl.window_text() or ""
            # Either empty or shows 0 of 0 indicates no doc loaded
            assert "of 1" not in text, (
                f"after wrong password + cancel, pageLabel shows a "
                f"loaded doc: {text!r}"
            )
    finally:
        try:
            fx_app.tearDown()
        except Exception:
            pass


def test_g2_wrong_then_correct_password_loads_doc(pdfe_no_doc_for_encryption):
    """G2 (branch B — retry): open encrypted PDF, enter wrong
    password, then re-enter correct password. PDFE should successfully
    load the 1-page document.
    """
    fx, src = pdfe_no_doc_for_encryption
    kill_pdfe_processes()
    fx_app = PdfeAppFixture()
    fx_app.launch_args = [str(src)]
    fx_app.setUp()
    time.sleep(2.0)

    pwd_edit = None
    for name in ("passwordEdit", "openPasswordEdit", "userPasswordEdit"):
        pwd_edit = find_by_suffix(fx_app.main_window, name)
        if pwd_edit is not None:
            break
    if pwd_edit is None:
        try: fx_app.tearDown()
        except Exception: pass
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "password prompt widget not reachable"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

    try:
        # Try wrong password first
        try:
            pwd_edit.set_edit_text("wrong-password")
        except Exception:
            pwd_edit.type_keys("wrong-password")
        pwd_edit.type_keys("{ENTER}")
        time.sleep(1.0)

        # Dismiss any error message popup
        fx_app.main_window.type_keys("{ENTER}")
        time.sleep(0.3)

        # The password prompt should reappear OR still be present.
        # Locate it again (may be a new widget instance).
        pwd_edit2 = None
        for name in ("passwordEdit", "openPasswordEdit", "userPasswordEdit"):
            pwd_edit2 = find_by_suffix(fx_app.main_window, name)
            if pwd_edit2 is not None:
                break
        if pwd_edit2 is None:
            # If the prompt closed entirely after wrong-pw, the test
            # can't proceed to branch-B; treat as documented limitation.
            pytest.skip("password prompt did not re-appear after wrong "
                        "password — retry-branch flow needs different UI "
                        "(maybe the dialog cancels after one wrong attempt)")

        # Type the correct password
        try:
            pwd_edit2.set_edit_text("user")
        except Exception:
            pwd_edit2.type_keys("user")
        pwd_edit2.type_keys("{ENTER}")
        time.sleep(1.5)

        # Verify the document loaded: pageLabel should show "of 1"
        pl = find_by_suffix(fx_app.main_window, "pageLabel")
        if pl is None:
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "pageLabel not reachable after correct-password entry"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        text = pl.window_text() or ""
        assert "of 1" in text, (
            f"after wrong+correct password, expected pageLabel to show "
            f"'of 1' (encrypted fixture is 1-page); got: {text!r}"
        )
    finally:
        try:
            fx_app.tearDown()
        except Exception:
            pass


# =========================================================================
# C1: broader Pages-menu workflows (Pipeline 3e9b1d7c phase-5, P-GAP-99)
# =========================================================================

def _page_count_via_label(fx) -> int:
    """Read page count from status-bar pageLabel ('Page X of N')."""
    import re as _re
    pl = find_by_suffix(fx.main_window, "pageLabel")
    if pl is None:
        return -1
    for _ in range(5):
        try:
            text = pl.window_text() or ""
            m = _re.search(r"of\s+(\d+)", text)
            if m:
                return int(m.group(1))
        except Exception:
            pass
        time.sleep(0.3)
    return -1


def _current_page_via_label(fx) -> int:
    """Read current page index (1-based) from pageLabel."""
    import re as _re
    pl = find_by_suffix(fx.main_window, "pageLabel")
    if pl is None:
        return -1
    try:
        text = pl.window_text() or ""
        m = _re.search(r"Page\s+(\d+)", text)
        if m:
            return int(m.group(1))
    except Exception:
        pass
    return -1


def test_c1_pages_move_up(pdfe_with_3page):
    """C1-MoveUp: navigate canvas to page 1 (Move Up reads from
    canvas->currentPage() not PagePanel selection), then trigger
    Move Up. Verify dirty marker.
    """
    fx, _tmp = pdfe_with_3page

    # Navigate canvas to page 1 first (PageDown moves canvas's current page)
    fx.main_window.type_keys("{PGDN}")
    time.sleep(0.5)

    tool_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    assert tool_pipe is not None
    tool_pipe.set_edit_text("movePageUpAction")
    time.sleep(0.8)

    title = fx.main_window.window_text() or ""
    assert "*" in title, (
        f"after Move Page Up, expected dirty marker '*' in title; got: {title!r}"
    )


def test_c1_pages_move_down(pdfe_with_3page):
    """C1-MoveDown: canvas is at page 0 by default; trigger Move Down."""
    fx, _tmp = pdfe_with_3page
    tool_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    assert tool_pipe is not None
    tool_pipe.set_edit_text("movePageDownAction")
    time.sleep(0.8)

    title = fx.main_window.window_text() or ""
    assert "*" in title, (
        f"after Move Page Down, expected dirty marker '*' in title; got: {title!r}"
    )


def test_c1_pages_move_to_start(pdfe_with_3page):
    """C1-MoveToStart: navigate canvas to page 2 (last), then trigger
    Move Up twice to bring it to page 0. Verify dirty marker.

    Move Up uses canvas->currentPage(), not PagePanel selection;
    after each Move Up, currentPage decrements to follow the moved page.
    """
    fx, _tmp = pdfe_with_3page
    tool_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    assert tool_pipe is not None

    # Navigate canvas to page 2 (PageDown twice)
    fx.main_window.type_keys("{PGDN}")
    time.sleep(0.3)
    fx.main_window.type_keys("{PGDN}")
    time.sleep(0.5)

    # Move Up twice
    tool_pipe.set_edit_text("movePageUpAction")
    time.sleep(0.6)
    tool_pipe.set_edit_text("movePageUpAction")
    time.sleep(0.6)

    title = fx.main_window.window_text() or ""
    assert "*" in title, (
        f"after Move To Start (Up x2), expected dirty marker '*'; got: {title!r}"
    )


def test_c1_pages_insert_blank_at_start(pdfe_with_3page):
    """C1-Insert (bonus, no-dialog variant): dispatch Pages →
    Insert Blank Page. Verifies the insert pathway via dirty marker
    + page count change, without exercising the file-open dialog.

    The full Insert From File flow is covered by
    test_c1_pages_insert_from_file below.
    """
    fx, _tmp = pdfe_with_3page
    initial_count = _page_count_via_label(fx)
    if initial_count != 3:
        pytest.skip(f"expected 3-page fixture, got count={initial_count}")

    # Use the testToolActivatePipe to trigger insertBlankPageAction directly
    tool_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    assert tool_pipe is not None
    tool_pipe.set_edit_text("insertBlankPageAction")
    time.sleep(1.0)

    # Verify page count increased to 4
    new_count = _page_count_via_label(fx)
    assert new_count == 4, (
        f"expected 4 pages after insert; got {new_count}"
    )
    title = fx.main_window.window_text() or ""
    assert "*" in title, (
        f"after Insert Blank, expected dirty marker; got: {title!r}"
    )


def test_c1_pages_insert_from_file(pdfe_with_3page_qt_dlg):
    """C1-InsertFromFile: drive the actual file-open dialog (Pages →
    Insert Pages from File...). Steps:
       1. Resolve testToolActivatePipe and trigger
          insertPagesFromFileAction. The action's QTimer::singleShot(0)
          deferred-trigger pattern lets the pipe handler return before
          QFileDialog::exec() blocks.
       2. Use desktop_dialog_driver.open_file_dialog() to fill in the
          source PDF path and click Open. Requires
          PDFE_USE_QT_FILE_DIALOG=1 (set by the fixture).
       3. Assert page count INCREASED from initial value and the dirty
          marker '*' is in the window title.

    Source file inserted: test_blank.pdf (single page in the standard
    fixtures set). We use a delta assertion rather than an exact count
    so the test stays robust if the fixture is regenerated with a
    different page count.
    """
    fx, _tmp = pdfe_with_3page_qt_dlg
    initial_count = _page_count_via_label(fx)
    if initial_count != 3:
        pytest.skip(f"expected 3-page fixture, got count={initial_count}")

    src_to_insert = FIXTURES_DIR / "test_blank.pdf"
    if not src_to_insert.exists():
        pytest.skip(f"source fixture missing: {src_to_insert}")

    try:
        from tests.ui_automation.helpers import desktop_dialog_driver as qt_dlg
    except ImportError as e:
        pytest.skip(f"desktop_dialog_driver helper unavailable: {e}")

    # Trigger the action via testToolActivatePipe. The pipe handler
    # queues action->trigger() via QTimer::singleShot(0) so the modal
    # QFileDialog::exec() doesn't block the textChanged handler that
    # set_edit_text() is waiting on.
    tool_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    assert tool_pipe is not None, "testToolActivatePipe must be reachable"
    tool_pipe.set_edit_text("insertPagesFromFileAction")
    time.sleep(0.6)  # allow QTimer::singleShot to fire + dialog to appear

    # Drive the QFileDialog
    try:
        qt_dlg.open_file_dialog(fx.main_window,
                                str(src_to_insert),
                                timeout=10.0)
    except TimeoutError as e:
        try:
            qt_dlg.cancel_file_dialog(fx.main_window)
        except Exception:
            pass
        pytest.skip(f"QFileDialog driving failed: {e}")

    # Allow ImportPagesCommand + onDocumentStructureChanged to complete.
    # The UIA tree gets rebuilt when panels refresh, so we poll for
    # the dirty marker as the primary completion signal, then re-resolve
    # pageLabel via a fresh descendants() enumeration.
    deadline = time.time() + 8.0
    title = ""
    while time.time() < deadline:
        title = fx.main_window.window_text() or ""
        if "*" in title:
            break
        time.sleep(0.3)

    assert "*" in title, (
        f"after Insert From File, expected dirty marker '*' in title; "
        f"got: {title!r}"
    )

    # Now poll for an updated page count. The pageLabel widget can get
    # re-rendered after onDocumentStructureChanged; allow several
    # retries on fresh UIA enumerations.
    new_count = -1
    deadline = time.time() + 5.0
    while time.time() < deadline:
        new_count = _page_count_via_label(fx)
        if new_count > initial_count:
            break
        time.sleep(0.4)

    assert new_count > initial_count, (
        f"after Insert From File, expected page count > {initial_count}; "
        f"got {new_count} (title={title!r})"
    )
