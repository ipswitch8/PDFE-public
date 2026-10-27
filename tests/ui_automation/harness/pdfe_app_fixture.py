# -*- coding: utf-8 -*-
"""Base class and helpers for launching pdfe.exe under pywinauto.

Every regression test in ``tests/ui_automation/`` should inherit
from :class:`PdfeAppFixture` or use :func:`launch_pdfe` /
:func:`kill_pdfe_processes` directly.

Key invariants this fixture enforces:

1. **``wait_for_idle=False``** — Qt apps hang ``WaitForInputIdle``
   under Windows if you call ``Application.start()`` with the
   default. Every launch goes through this fixture to guarantee
   the flag is set.

2. **Top-window resolved by class_name**, not title. The window
   title changes when a document is opened (``PDFE`` → ``foo.pdf –
   PDFE``), and test code that matches by title fails
   inconsistently. The fixture matches on ``pdfe::ui::MainWindow``
   which is stable.

3. **Teardown kills the process two ways** — first
   ``app.kill()``, then ``taskkill /F /IM pdfe.exe`` as a fallback
   for the case where ``app.kill()`` silently left the process
   alive (happens when the main window crashes before tests
   return).

4. **Startup cache flush** — before every launch, any lingering
   ``pdfe.exe`` from a previous crashed test is hard-killed so the
   fresh instance starts in a clean state.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

from pywinauto import Application
from pywinauto.timings import Timings


REPO_ROOT = Path(__file__).resolve().parents[3]
#: Build config — Debug by default, overridable via env for CI that
#: wants to run against Release or a custom tier without editing
#: source. Karen phase-1 review finding #1.
PDFE_BUILD_CONFIG = os.environ.get("PDFE_BUILD_CONFIG", "Debug")
PDFE_EXE = (
    REPO_ROOT / "build" / "vs2022" / "bin" / PDFE_BUILD_CONFIG / "pdfe.exe"
)
MAIN_WINDOW_CLASS = "pdfe::ui::MainWindow"


def kill_pdfe_processes() -> None:
    """Hard-kill every lingering ``pdfe.exe`` on the host.

    Called as the teardown fallback and as the pre-launch cleanup.
    Swallows errors — if no process is running, ``taskkill`` returns
    non-zero and that's fine.
    """
    try:
        subprocess.run(
            ["taskkill", "/F", "/IM", "pdfe.exe"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        # taskkill may not exist (non-Windows) or the host may be
        # under heavy load. Not a test failure — just best-effort.
        pass


def _wait_process_exit(name: str = "pdfe.exe", timeout: float = 5.0) -> None:
    """Poll until no process named *name* exists, or timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH"],
            capture_output=True, text=True, timeout=5,
        )
        if name.lower() not in result.stdout.lower():
            return
        time.sleep(0.3)
    # Timeout — proceed anyway (best effort)


def launch_pdfe(
    args: Optional[list] = None,
    visible_timeout_s: float = 15.0,
) -> tuple:
    """Launch ``pdfe.exe`` and return ``(app, main_window)``.

    :param args: Optional list of CLI args to append after the exe path.
                 Used by tests that want to open a specific document.
    :param visible_timeout_s: How long to wait for the main window to
                              become visible before giving up.
    :returns: Tuple of ``(pywinauto.Application, main_window)``.
    :raises RuntimeError: If ``pdfe.exe`` does not exist, fails to
                          launch, or the main window never appears.
    """
    if not PDFE_EXE.exists():
        raise RuntimeError(
            f"pdfe.exe not found at {PDFE_EXE} — "
            f"build via: cmake --build build/vs2022 --config Debug --target pdfe"
        )

    # Preflight: kill any stragglers so the new instance is clean.
    kill_pdfe_processes()
    _wait_process_exit("pdfe.exe", timeout=5.0)

    Timings.after_click_wait = 0.2

    # GAP-60 fix: enable PDFE_TEST_PIPES and friends BEFORE launching pdfe.exe
    # so the Q_INVOKABLE test pipes (testToolActivatePipe,
    # testClickByPdfCoordPipe, testAnnot*Pipe family, etc.) are created.
    # The ui_automation harness's launch_pdfe (this function) historically
    # did NOT set these env vars while tests/ui_harness/core/launch.py
    # (used by tests/test_ui_*.py) DID, which is why test_ui_annotations.py
    # could activate tools via pipe but tests/ui_automation/test_annotations_p1.py
    # could not.
    #
    # NB: pywinauto's Application.start() uses Win32 CreateProcess directly
    # which does NOT inherit os.environ in all cases. Spawn via subprocess
    # with explicit env, then connect via pywinauto.
    env = os.environ.copy()
    env["PDFE_TEST_ANNOT_PIPES"] = "1"
    env["PDFE_TEST_PIPES"] = "1"
    env["PDFE_ENABLE_JS"] = "1"
    # PDFE_TEST_DIALOG_AUTODISMISS is intentionally NOT forced here. It makes
    # EVERY registered dialog auto-reject on open, which is what a handful of
    # tests want (precondition_panels' AtlasComparisonDialog introspection
    # readback; test_ui_ltv's TrustAnchor/Validation modals) but which BREAKS
    # the many tests that open a dialog and inspect it while it stays up
    # (test_dialog_reachability_p0, test_remaining_preconditions_phase5,
    # test_security_compliance_p1, test_ui_dialog_coverage, the
    # BatchDialog/CompareDialog/Preferences/Compliance cases in
    # test_phase7_new_features — ~50 failures when forced global). Those few
    # tests that genuinely need auto-dismiss set os.environ[...] = "1"
    # themselves before calling launch_pdfe; because env starts as
    # os.environ.copy(), that per-test opt-in is inherited by the child while
    # the default (unset) keeps dialogs OPEN for inspection. The keyboard tests
    # use the PDFESession launcher (which never set AUTODISMISS), so they are
    # unaffected either way.
    # Phase 7 (GAP-A14): lower the "Request Validation Data..." gate from
    # validCount>0 to signedCount>0 so the self-signed test fixtures on the
    # rig can exercise the ValidationRequestDialog. Strictly a test seam —
    # NEVER set in production; inert when absent (see onVerifySignatures()).
    env["PDFE_TEST_VALIDATION_REQUEST_UNLOCKED"] = "1"
    # GAP-60 fix: Qt high-DPI scaling env vars matching the ui_harness
    # launch_pdfe. Without these, Qt may treat widget coordinates and
    # visibility differently, which on this rig caused the QTreeWidget
    # in CommentPanel to not expose its child items via UIA (Qt's
    # accessibility bridge dropped them when high-DPI scaling was
    # active even though the items existed in the QObject tree).
    env["QT_SCALE_FACTOR"] = "1"
    env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    env["QT_SCREEN_SCALE_FACTORS"] = "1"
    env["QT_ENABLE_HIGHDPI_SCALING"] = "0"

    exe_args = [str(PDFE_EXE)] + ([str(a) for a in args] if args else [])
    try:
        proc = subprocess.Popen(exe_args, env=env)
    except Exception as e:
        raise RuntimeError(f"subprocess.Popen(pdfe.exe) failed: {e}") from e

    try:
        app = Application(backend="uia").connect(
            process=proc.pid, timeout=visible_timeout_s,
        )
    except Exception as e:
        try:
            proc.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_pdfe_processes()
        raise RuntimeError(f"Application.connect failed: {e}") from e

    # The window title changes when a doc is opened, so match by
    # class_name which is stable.
    deadline = time.time() + visible_timeout_s
    last_err: Optional[Exception] = None
    main_window = None
    while time.time() < deadline:
        try:
            main_window = app.window(class_name=MAIN_WINDOW_CLASS)
            main_window.wait("visible", timeout=1.0)
            break
        except Exception as e:  # pywinauto may raise various things
            last_err = e
            time.sleep(0.2)

    if main_window is None:
        # Clean up the orphan process before raising.
        try:
            app.kill()
        except Exception:
            pass
        kill_pdfe_processes()
        raise RuntimeError(
            f"pdfe.exe main window did not become visible within "
            f"{visible_timeout_s}s: {last_err}"
        )

    return app, main_window


class PdfeAppFixture:
    """Base class for every PDFE UI regression test.

    Usage::

        class MyTest(PdfeAppFixture):
            def test_something(self):
                self.setUp()
                try:
                    # self.app and self.main_window are valid here
                    ...
                finally:
                    self.tearDown()

    Or via a context manager::

        with PdfeAppFixture() as fx:
            # fx.app and fx.main_window are valid
            ...
    """

    app = None           # type: Optional[Application]
    main_window = None   # type: Optional[object]

    #: Optional list of CLI args passed to pdfe.exe at launch.
    #: Subclasses override to pre-open a document.
    launch_args: Optional[list] = None

    def setUp(self) -> None:
        """Launch pdfe.exe and wait for the main window."""
        self.app, self.main_window = launch_pdfe(
            args=self.launch_args,
        )

    def tearDown(self) -> None:
        """Kill the app two ways and swallow any errors.

        Errors during teardown must never mask a test's real
        failure. The ``taskkill`` fallback runs even when
        ``app.kill()`` raises, so no orphaned process is left
        behind.
        """
        if self.app is not None:
            try:
                self.app.kill()
            except Exception:
                pass
        kill_pdfe_processes()
        self.app = None
        self.main_window = None

    # Context manager support for test functions that prefer
    # ``with`` over explicit setUp/tearDown.

    def __enter__(self) -> "PdfeAppFixture":
        self.setUp()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.tearDown()
        return None


def assert_no_orphan_pdfe() -> None:
    """Raise if any pdfe.exe is still running. Test post-condition."""
    try:
        result = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe"],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return  # best effort
    if "pdfe.exe" in result.stdout:
        raise RuntimeError(
            "Orphan pdfe.exe detected after test teardown:\n"
            + result.stdout
        )


def find_widget(parent, leaf_id: str, timeout: float = 5.0,
                require_visible: bool = False):
    """Find a Qt widget by its **leaf** ``objectName``.

    pywinauto's UIA backend reports Qt widgets' ``automation_id`` as
    the full QObject parent-chain path, e.g.
    ``QApplication.mainWindow.attachmentPanelDock.attachmentPanel``,
    not just the leaf ``objectName`` we set in C++. Calling
    ``child_window(auto_id="attachmentPanel")`` therefore times out
    even though the widget exists.

    This helper walks ``parent.descendants()`` and matches by
    ``automation_id == leaf_id`` or ``automation_id.endswith("." +
    leaf_id)``. Returns the wrapper or raises ``TimeoutError``.

    :param parent: pywinauto wrapper to search under.
    :param leaf_id: The Qt ``objectName`` set in C++ source.
    :param timeout: How long to retry the descendants scan.
    :param require_visible: If True, also require ``is_visible()``
                            (skips hidden dock widgets).
    """
    suffix = "." + leaf_id
    deadline = time.time() + timeout
    last_err: Optional[Exception] = None

    def _scan(root):
        """Suffix-match a Qt leaf objectName under *root*'s UIA descendants.
        Returns the wrapper or None. Qt6 reports automation_id as the full
        parent-chain path (``QApplication.mainWindow.<objectName>``), so we
        match the trailing ``.<leaf>`` component."""
        for d in root.descendants():
            try:
                aid = d.element_info.automation_id or ""
            except Exception:
                continue
            if aid == leaf_id or aid.endswith(suffix):
                if require_visible:
                    try:
                        if not d.is_visible():
                            continue
                    except Exception:
                        continue
                return d
        return None

    # Resolve the PID once for the fresh-connection strategy below.
    pid = None
    for getter in (lambda: parent.process_id(),
                   lambda: parent.element_info.process_id):
        try:
            pid = getter()
            if pid:
                break
        except Exception:
            continue

    while time.time() < deadline:
        # Strategy 1: scan the existing wrapper (fast when its UIA cache is good).
        try:
            found = _scan(parent)
            if found is not None:
                return found
        except Exception as e:
            last_err = e
        # Strategy 2: scan a FRESH ``Application().connect(process=pid)``.
        # The existing wrapper's cached UIA tree can be stale/incomplete on the
        # Windows-Server-2019 RDP runner session — its descendants() walk
        # (IUIAutomationElement::FindAll) intermittently returns a partial tree
        # that MISSES the zero-size hidden test-pipe QLineEdits, so the existing
        # batch suites recorded "testToolActivatePipe unreachable" in CI even
        # though the pipe exists and the app is healthy (runs 28186683294 /
        # 28250606897 / 28277763938). A fresh connection rebuilds the proxy tree
        # from the live PID and sees the pipes. Mirrors the proven
        # qinvoke._find_pipe Strategy B (see GAP-A14 / the 0x80040155
        # REGDB_E_CLASSNOTREG note in test_ui_batch3_preferences.py).
        if pid:
            try:
                import pywinauto  # local import: keep module headless-importable
                fresh = pywinauto.Application(backend="uia")
                fresh.connect(process=pid, timeout=2)
                fwin = fresh.window(title_re=r".*PDFE.*")
                found = _scan(fwin)
                if found is not None:
                    return found
            except Exception as e:
                last_err = e
        time.sleep(0.2)
    raise TimeoutError(
        f"widget with leaf objectName {leaf_id!r} not found under "
        f"{parent} within {timeout}s (last err: {last_err})"
    )


def invoke_action(win, action_object_name: str, settle_s: float = 0.5,
                  timeout: float = 4.0) -> bool:
    """Trigger a MainWindow ``QAction`` by objectName via the hidden
    ``testToolActivatePipe`` QLineEdit, bypassing pywinauto's
    ``menu_select`` / UIA ``FindAll``.

    Phase 7 (GAP-A14): ``win.menu_select("Menu->Item")`` drives
    pywinauto's UIA menu walk, whose ``IUIAutomationElement::FindAll``
    intermittently **blocks indefinitely** on the Windows-Server / RDP CI
    session — pytest-timeout then hard-kills the whole file (rc=-1 / rc=1
    with no per-test record). PDFE ships a test seam for exactly this:
    writing a QAction objectName to ``testToolActivatePipe`` calls
    ``MainWindow::activateToolByObjectName``, which triggers the action
    via ``QTimer::singleShot(0, ...)`` so a modal ``QDialog::exec`` cannot
    block the pipe's ``textChanged`` handler. ``launch_pdfe`` already sets
    ``PDFE_TEST_PIPES=1`` so the pipe exists.

    :param win: the pywinauto main-window wrapper from ``launch_pdfe``.
    :param action_object_name: e.g. ``"verifySignaturesAction"``.
    :param settle_s: pause after writing so the queued trigger fires.
    :returns: True if the objectName was written to the pipe. A False/raise
              means the pipe was unreachable; callers decide whether that
              is a skip. Note: the C++ seam returns without effect if the
              action is *disabled* — callers that depend on the action's
              effect (e.g. a dialog opening) must still verify that effect.
    """
    # Qt6 prefixes the UIA AutomationId with the full widget path (e.g.
    # "QApplication.mainWindow.testToolActivatePipe"), so an exact
    # child_window(auto_id="testToolActivatePipe") match MISSES it. Use the
    # harness's suffix matcher with require_visible=False (the pipe is a
    # zero-size NoFocus QLineEdit, intentionally invisible to users).
    try:
        pipe = find_widget(win, "testToolActivatePipe", timeout=timeout,
                           require_visible=False)
    except Exception as e:
        # Surface the real lookup error (UIA/COM) so a CI per-file log shows
        # WHY the pipe was unreachable, not just a bare "unreachable".
        import sys as _sys
        print(f"[invoke_action] testToolActivatePipe lookup failed for "
              f"{action_object_name!r}: {e!r}", file=_sys.stderr)
        return False
    try:
        pipe.set_edit_text(action_object_name)
    except Exception:
        try:
            # Some pywinauto/UIA builds expose only the ValuePattern.
            pipe.set_value(action_object_name)
        except Exception:
            return False
    time.sleep(settle_s)
    return True


def write_pipe(win, object_name: str, value: str, settle_s: float = 0.3,
               timeout: float = 4.0) -> bool:
    """Write *value* to a hidden write-only MainWindow pipe by objectName.

    Generic counterpart to ``invoke_action`` for the GAP-A14 / SKIP=FAIL
    write-only pipes (``testFileDialogPathPipe``, ``testDialogActionPipe``).
    Uses the suffix matcher (Qt6 prefixes the UIA AutomationId) with
    ``require_visible=False`` since the pipe is a zero-size NoFocus QLineEdit.
    Returns True if the value was written.
    """
    try:
        pipe = find_widget(win, object_name, timeout=timeout,
                           require_visible=False)
    except Exception:
        return False
    try:
        pipe.set_edit_text(value)
    except Exception:
        try:
            pipe.set_value(value)
        except Exception:
            return False
    time.sleep(settle_s)
    return True


def inject_file_dialog_path(win, path: str, settle_s: float = 0.3) -> bool:
    """Pre-seat the path the NEXT QFileDialog save/open should return.

    GAP-A14 / SKIP=FAIL: each getSaveFileName/getOpenFileName/
    getExistingDirectory call site consults
    ``pdfe::ui::testseam::takeFileDialogPath()`` first, so writing here makes
    the next file dialog return *path* without showing a native modal. Call
    while MainWindow is non-modal (before the consuming dialog opens).
    """
    return write_pipe(win, "testFileDialogPathPipe", path, settle_s=settle_s)


def enqueue_dialog_action(win, spec: str, settle_s: float = 0.3) -> bool:
    """Queue a modal-dialog action script, replayed when the dialog opens.

    GAP-A14 / SKIP=FAIL. *spec* is ``"<dialogObjectName>|verb|verb|..."`` with
    verbs ``combo:Name=Value`` / ``edit:Name=Text`` / ``spin:Name=N`` /
    ``check:Name=1`` / ``click:Name`` / ``accept`` / ``reject``. Call BEFORE
    opening the dialog (while MainWindow is non-modal). ``registerDialogIntro-
    spection`` replays it against the real widgets on the dialog's own loop.
    """
    return write_pipe(win, "testDialogActionPipe", spec, settle_s=settle_s)


def attachment_names(win, settle_s: float = 0.4, timeout: float = 4.0) -> list:
    """Return the attachment-panel's attachment names (UIA-free).

    Drives ``testAttachmentQueryPipe`` / ``testAttachmentResultPipe`` with the
    ``names`` command. Returns ``[]`` when the panel has no attachments or is
    unreachable.
    """
    raw = read_result_pipe(win, "testAttachmentQueryPipe",
                           "testAttachmentResultPipe", payload="names",
                           settle_s=settle_s, timeout=timeout)
    if not raw or raw.startswith("err:"):
        return []
    return [p for p in raw.split("|") if p]


def attachment_select(win, name: str, settle_s: float = 0.4,
                      timeout: float = 4.0) -> bool:
    """Select the attachment row named *name* in the panel (UIA-free).
    Returns True when the row was found and selected.
    """
    res = read_result_pipe(win, "testAttachmentQueryPipe",
                           "testAttachmentResultPipe", payload=f"select:{name}",
                           settle_s=settle_s, timeout=timeout)
    return res == "1"


def attachment_trigger(win, which: str, settle_s: float = 0.6,
                       timeout: float = 4.0) -> bool:
    """Run the panel's real Add (``which="add"``) or Extract
    (``which="extract"``) handler. Seat the file/dir path via
    :func:`inject_file_dialog_path` first. Returns True when it ran.
    """
    if which not in ("add", "extract"):
        return False
    res = read_result_pipe(win, "testAttachmentQueryPipe",
                           "testAttachmentResultPipe", payload=which,
                           settle_s=settle_s, timeout=timeout)
    return res == "ok"


#: GAP-A14: human menu path → MainWindow QAction objectName. pywinauto's
#: ``win.menu_select("Menu->Item")`` drives a UIA menu walk whose
#: ``FindAll`` blocks indefinitely on the Windows-Server / RDP CI session,
#: so tests route menu invocations through ``invoke_menu_path`` (which calls
#: ``invoke_action`` on the mapped objectName) instead. Keys are the
#: normalized path produced by ``_normalize_menu_path`` (lowercased, ``&``
#: and ``...`` stripped, ``->`` collapsed to ``>``).
_MENU_PATH_TO_ACTION = {
    "file>print": "printAction",
    "file>save as": "saveAsAction",
    "edit>select all": "selectAllAction",
    "edit>copy": "copyAction",
    "edit>cut": "cutAction",
    "edit>paste": "pasteAction",
    "edit>replace": "replaceAction",
    "edit>preferences": "preferencesAction",
    "security>sign document": "signDocumentAction",
    "security>verify signatures": "verifySignaturesAction",
    "security>request validation data": "requestValidationAction",
    "forms>export form data": "exportFormDataAction",
    "forms>reset form": "resetFormAction",
    "forms>tab order": "tabOrderAction",
    "forms>submit form": "submitFormAction",
    "pages>rotate clockwise": "rotatePageCWAction",
    "pages>rotate counter-clockwise": "rotatePageCCWAction",
    "pages>move page down": "movePageDownAction",
    "pages>move page up": "movePageUpAction",
    "pages>insert blank page": "insertBlankPageAction",
    "pages>insert from file": "insertPagesFromFileAction",
    "pages>extract pages": "extractPagesAction",
    "tools>compliance": "complianceAction",
    "tools>ocr current page": "ocrAction",
    "tools>text reconstruction": "treAction",
    "tools>accessibility check": "accessibilityAction",
    "tools>batch process": "batchAction",
    "tools>compare documents": "compareAction",
    "view>attachments panel": "toggleAttachmentPanelAction",
    "view>toggle dark theme": "themeToggleAction",
}


def _normalize_menu_path(path: str) -> str:
    """Normalize a human menu path for ``_MENU_PATH_TO_ACTION`` lookup.

    Lowercases, removes accelerator ``&`` and trailing ``...``, collapses
    ``->`` to ``>``, and strips whitespace around each segment. A trailing
    submenu segment is kept (e.g. ``View -> Rotate -> Rotate View
    Clockwise`` → ``view>rotate>rotate view clockwise``).
    """
    p = path.replace("->", ">").replace("&", "").replace("...", "")
    parts = [seg.strip().lower() for seg in p.split(">")]
    return ">".join(x for x in parts if x)


def invoke_menu_path(win, menu_path: str, settle_s: float = 0.5,
                    timeout: float = 4.0) -> bool:
    """Activate a menu item by its human path via the action seam.

    GAP-A14: never calls ``win.menu_select`` (which hangs in UIA
    ``FindAll`` on the CI session). Looks the path up in
    ``_MENU_PATH_TO_ACTION`` and triggers the action through
    ``invoke_action``. Returns True if a mapping existed and the
    objectName was written to the pipe; False if the path is unmapped or
    the pipe was unreachable (caller decides whether that is a skip).
    """
    action = _MENU_PATH_TO_ACTION.get(_normalize_menu_path(menu_path))
    if action is None:
        return False
    return invoke_action(win, action, settle_s=settle_s, timeout=timeout)


def _read_pipe_value(pipe) -> Optional[str]:
    """Read a result-pipe QLineEdit's current text via UIA.

    Tries the UIA ValuePattern (``iface_value.CurrentValue``) first
    because a zero-size NoFocus QLineEdit does not always expose its
    text through ``window_text()``; falls back to ``window_text()`` and
    ``get_value()``. Returns ``None`` if every accessor raises.
    """
    try:
        return pipe.iface_value.CurrentValue
    except Exception:
        pass
    try:
        txt = pipe.window_text()
        if txt:
            return txt
    except Exception:
        pass
    try:
        return pipe.get_value()
    except Exception:
        return None


def read_dialog_introspection(win, dialog_object_name: str,
                             settle_s: float = 0.5,
                             timeout: float = 4.0) -> Optional[str]:
    """Read a modal dialog's recorded child-widget inventory via the
    generic introspection seam (TestDialogSeam.h).

    GAP-A14: after a dialog is opened (e.g. via ``invoke_action``) with
    ``PDFE_TEST_DIALOG_AUTODISMISS=1`` set so the modal exec() returns, this
    writes the dialog's objectName to ``testDialogIntrospectionQueryPipe``
    and reads the recorded inventory ("<dialogObjectName>|name1,name2,...")
    from ``testDialogIntrospectionResultPipe``. Returns ``"__null__"`` if
    the dialog was never opened, or ``None`` if the pipe is unreachable.
    """
    return read_result_pipe(
        win, "testDialogIntrospectionQueryPipe",
        "testDialogIntrospectionResultPipe",
        payload=dialog_object_name, settle_s=settle_s, timeout=timeout)


def dismiss_native_dialog(pid: int, timeout: float = 4.0,
                         class_name: str = "#32770") -> Optional[str]:
    """Detect and close a native Win32 dialog owned by *pid* — no UIA.

    GAP-A14: the native modal QFileDialog (Insert From File, Extract Pages,
    Save As) blocks pdfe's UI thread; the common test pattern
    ``Desktop(backend="uia").window(title_re=...).wait("visible")`` drives a
    UIA Desktop enumeration that hangs in ``FindAll`` on the Windows-Server
    CI session. This helper instead uses ``EnumWindows`` (raw Win32) to find
    a window of *class_name* (``#32770`` = standard Windows dialog) belonging
    to *pid*, reads its title, and posts ``WM_CLOSE`` to dismiss it. Returns
    the dialog title on success (the modal is confirmed open and closed), or
    ``None`` if no matching dialog appeared within *timeout*.
    """
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    WM_CLOSE = 0x0010
    EnumWindowsProc = ctypes.WINFUNCTYPE(
        wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    deadline = time.time() + timeout
    found = {"hwnd": None, "title": None}

    def _cb(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid:
            return True
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value != class_name:
            return True
        tbuf = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(hwnd, tbuf, 512)
        found["hwnd"] = hwnd
        found["title"] = tbuf.value
        return False  # stop enumeration

    cb = EnumWindowsProc(_cb)
    while time.time() < deadline:
        found["hwnd"] = None
        user32.EnumWindows(cb, 0)
        if found["hwnd"]:
            user32.PostMessageW(found["hwnd"], WM_CLOSE, 0, 0)
            time.sleep(0.3)
            return found["title"] or ""
        time.sleep(0.2)
    return None


def read_result_pipe(win, query_object_name: str, result_object_name: str,
                     payload: str = "1", settle_s: float = 0.5,
                     timeout: float = 4.0) -> Optional[str]:
    """Drive a PDFE query/result pipe pair and return the result text.

    Phase 7 (GAP-A14): the LTV UI surfaces (the SignaturePanel LTV label
    and the two modal signature dialogs) cannot be read or enumerated via
    UIA on the Windows-Server-2019 CI session. PDFE ships hidden
    query/result ``QLineEdit`` pairs (gated by ``PDFE_TEST_PIPES``): write
    any non-empty token to ``<X>QueryPipe`` and the MainWindow handler
    writes the asserted state into ``<X>ResultPipe``, which we read back
    here via the UIA ValuePattern. This is the same readback seam
    ``tests/helpers/qinvoke.py`` uses for AltText / Customize / form-field
    values — a genuine assertion path, not a skip.

    :param win: the pywinauto main-window wrapper from ``launch_pdfe``.
    :param query_object_name: e.g. ``"testLtvStatusQueryPipe"``.
    :param result_object_name: e.g. ``"testLtvStatusResultPipe"``.
    :param payload: token written to the query pipe (any non-empty value).
    :param settle_s: pause after writing so the handler populates result.
    :returns: the result-pipe text (handler writes ``"__null__"`` for an
              empty/absent value), or ``None`` if either pipe was
              unreachable.
    """
    try:
        query = find_widget(win, query_object_name, timeout=timeout,
                            require_visible=False)
    except Exception:
        return None
    try:
        query.set_edit_text(payload)
    except Exception:
        try:
            query.set_value(payload)
        except Exception:
            return None
    time.sleep(settle_s)
    try:
        result = find_widget(win, result_object_name, timeout=timeout,
                            require_visible=False)
    except Exception:
        return None
    return _read_pipe_value(result)
