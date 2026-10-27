# -*- coding: utf-8 -*-
"""GitHub #91 (WIN-60): the File > Open leg must refuse a UNC path, say so, and
leave the open document alone.

WHY THIS IS NOT ALREADY COVERED
-------------------------------
``tests/test_engine_unc_gate.cpp`` proves the ENGINE leg —
``Document::open`` / the optimize engine refuse a UNC and their message names
UNC. That is a different function from the one a user reaches. The path a user
types into File > Open arrives at ``MainWindow::openFile``, and before this test
existed that function's first act on the string was ``QFileInfo::exists()``:

  * an UNREACHABLE UNC blocked the GUI thread on SMB name resolution and was
    then reported as "could not be found" — the word UNC never appeared, so the
    user had no way to know PDFE will not open network locations at all;
  * a REACHABLE UNC got past ``exists()`` and was refused further down by the
    engine, worded for a DOCUMENT: "malformed or crafted to exhaust resources".
    Wrong and alarming, for a file whose only problem is where it lives.

So the two legs genuinely differ, and only the UI leg is under test here.

TWO CASES, BOTH REQUIRED
------------------------
``test_native_open_dialog_typed_unc_is_refused`` is the literal scenario in the
issue: the REAL native picker, opened by the real ``openAction``, with the UNC
written into its real filename box and its real Open button clicked. It needs a
UNC that the SHELL can resolve, because a native ``IFileOpenDialog`` validates
existence itself and never hands a bad path to the application — so it uses the
machine's own ``\\\\127.0.0.1\\C$`` admin share and is gated on that share being
present (a filesystem-existence probe, the one legitimate skip shape).

``test_open_leg_refuses_unreachable_unc_via_dialog_seam`` covers the same leg
with a UNC that names a host that does not exist, injected through
``takeFileDialogPath()`` — the seam the getOpenFileName call site consults
first. It needs no share and therefore runs everywhere, which is the point: the
coverage of the refusal itself must not be contingent on this machine's SMB
configuration.

READING THE ERROR
-----------------
UIA cannot enumerate a Qt modal ``exec()`` (see tests/helpers/modal_dialog.py),
and a QMessageBox body is a QLabel with no HWND, so a Win32 child walk cannot
see it either. Asserting on the window CAPTION would prove a dialog appeared
without proving it said why. ``MainWindow::openFile``'s reporter therefore calls
``registerDialogIntrospection()`` on the box, and the assertion below reads the
real label text (``qt_msgbox_label``) through the dialog-child pipe.

Run:  python -m pytest tests/test_ui_open_dialog_unc.py -v -s
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
CORPUS = FIXTURES / "test_3page.pdf"

# The loopback admin share. Present by default on Windows Server for a local
# administrator; absent on a hardened or non-admin box. Probed, never assumed.
LOOPBACK_SHARE_ROOT = r"\\127.0.0.1\C$"

# A host that cannot exist, so the case needs no share at all.
UNREACHABLE_UNC = r"\\pdfe-no-such-host.invalid\share\nope.pdf"


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
    pytest.mark.skipif(not CORPUS.exists(),
                       reason="tests/fixtures/test_3page.pdf missing"),
]


# ---------------------------------------------------------------------------
# Shared oracle helpers
# ---------------------------------------------------------------------------

def _page_count(session) -> int:
    """Pages in the currently-open document, via the seam. -1 when unanswered.

    -1 is deliberately distinct from 0: "the seam did not answer" must never be
    mistaken for "no document is open", or an unreachable seam would satisfy a
    "nothing loaded" assertion on its own.
    """
    from tests.helpers import qinvoke
    if not qinvoke._write_to_pipe(session, "testPageCountQueryPipe", "1",
                                  settle=0.25):
        return -1
    raw = qinvoke._read_result_pipe(session, "testPageCountResultPipe")
    try:
        return int((raw or "").strip())
    except (TypeError, ValueError):
        return -1


def _unc_error_text(session, timeout: float = 20.0) -> str:
    """Body text of the open-failure modal, read through the dialog-child pipe.

    Waits for the box by title with the Win32 helper first (UIA cannot see a
    modal), then reads the label. Raises if the box never appears — a missing
    error dialog is a FAILURE of this test, never a reason to pass quietly.
    """
    from tests.helpers.modal_dialog import wait_for_window
    from tests.helpers import qinvoke

    hwnd = wait_for_window(session.pid, "Cannot Open Document",
                           timeout=timeout, poll=0.25)
    assert hwnd, (
        "no 'Cannot Open Document' modal appeared within %.0fs. The UNC was "
        "either accepted, or refused silently -- both are failures of this "
        "criterion." % timeout
    )
    return qinvoke.dialog_widget_text(session, "qt_msgbox_label")


def _dismiss_error(session) -> None:
    from tests.helpers import qinvoke
    try:
        qinvoke.dialog_press_default(session)
    except Exception:  # noqa: BLE001
        pass
    time.sleep(0.6)


def _assert_names_unc(body: str, where: str) -> None:
    assert "UNC" in body, (
        "the %s error does not name UNC. A user who is told only that the "
        "file 'could not be found' cannot know PDFE refuses network "
        "locations by policy. Body text was: %r" % (where, body)
    )


def _assert_document_unchanged(session, title_before: str, pages_before: int,
                               unc: str) -> None:
    """No document loaded: the one that WAS open is still the one that is."""
    title_after = session.window.window_text() or ""
    assert title_after == title_before, (
        "the refused UNC %r changed the open document: title was %r, now %r. "
        "A refusal must not close, replace or dirty the current document."
        % (unc, title_before, title_after)
    )
    pages_after = _page_count(session)
    assert pages_after == pages_before > 0, (
        "page count moved from %d to %d after the refused UNC %r -- something "
        "was loaded or unloaded" % (pages_before, pages_after, unc)
    )


def _open_working_copy(name: str) -> Path:
    RUNTIME.mkdir(parents=True, exist_ok=True)
    work = RUNTIME / name
    shutil.copy2(CORPUS, work)
    return work


def _settle_open(session):
    assert session.wait_for_window_title("PDFE"), "PDFE window never appeared"
    session.wait_for_idle()
    time.sleep(1.2)
    title = session.window.window_text() or ""
    pages = _page_count(session)
    assert pages > 0, (
        "the local fixture did not open (page-count seam said %d), so this "
        "test has no baseline to prove 'nothing loaded' against" % pages
    )
    return title, pages


# ---------------------------------------------------------------------------
# Case 1 -- the real native picker (issue #91 verbatim)
# ---------------------------------------------------------------------------

def _loopback_unc_for(local: Path) -> str:
    """`C:\\a\\b` -> `\\\\127.0.0.1\\C$\\a\\b`."""
    resolved = local.resolve()
    drive = resolved.drive          # "C:"
    assert len(drive) == 2 and drive[1] == ":", (
        "expected a drive-letter path, got %r" % str(resolved))
    return "%s\\%s$%s" % ("\\\\127.0.0.1", drive[0],
                          str(resolved)[2:])


@pytest.mark.skipif(
    not os.path.isdir(LOOPBACK_SHARE_ROOT),
    reason="loopback admin share %s is not reachable on this machine; the "
           "native picker validates existence itself and will not hand an "
           "unreachable UNC to the application" % LOOPBACK_SHARE_ROOT,
)
def test_native_open_dialog_typed_unc_is_refused():
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers import qinvoke
    from tests.helpers.file_dialog import set_open_dialog_filename

    work = _open_working_copy("unc_open_dialog_local.pdf")
    target = _open_working_copy("unc_open_dialog_target.pdf")
    unc = _loopback_unc_for(target)
    assert os.path.isfile(unc), (
        "the UNC spelling %r does not resolve to the file we just wrote; "
        "without a resolvable path the native picker would reject it before "
        "PDFE ever sees it, and this test would prove nothing" % unc
    )

    with PDFESession(pdf_path=str(work)) as s:
        title_before, pages_before = _settle_open(s)

        # Real QAction -> real MainWindow::onOpenFile -> real native picker.
        assert qinvoke.activate_tool_by_name(s, "openAction"), \
            "failed to trigger the openAction QAction"
        dlg = s.wait_for_top_level_window("Open PDF File", timeout=20)
        assert dlg, "the native Open dialog never appeared"

        # Type the UNC into the filename box and press Open, in the dialog.
        set_open_dialog_filename(dlg, unc, settle_after_click=2.0)

        body = _unc_error_text(s)
        _assert_names_unc(body, "native Open dialog")
        _dismiss_error(s)
        _assert_document_unchanged(s, title_before, pages_before, unc)


# ---------------------------------------------------------------------------
# Case 2 -- same leg, no share required
# ---------------------------------------------------------------------------

def test_open_leg_refuses_unreachable_unc_via_dialog_seam():
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers import qinvoke

    work = _open_working_copy("unc_open_seam_local.pdf")

    with PDFESession(pdf_path=str(work)) as s:
        title_before, pages_before = _settle_open(s)

        # Pre-seat what the Open dialog will yield, then drive the same
        # openAction the menu item drives. The path reaches openFile() by the
        # identical route a typed path does.
        assert qinvoke.inject_file_dialog_path(s, UNREACHABLE_UNC), \
            "could not pre-seat the file-dialog path"
        assert qinvoke.activate_tool_by_name(s, "openAction"), \
            "failed to trigger the openAction QAction"

        started = time.monotonic()
        body = _unc_error_text(s)
        elapsed = time.monotonic() - started
        _assert_names_unc(body, "File > Open")
        # The refusal is a string test, so it must not have cost an SMB
        # round-trip. A gate placed AFTER QFileInfo::exists() would still name
        # UNC eventually while freezing the GUI thread on name resolution.
        assert elapsed < 10.0, (
            "the refusal took %.1fs -- the UNC was probed on the filesystem "
            "before being refused" % elapsed
        )
        _dismiss_error(s)
        _assert_document_unchanged(s, title_before, pages_before,
                                   UNREACHABLE_UNC)
