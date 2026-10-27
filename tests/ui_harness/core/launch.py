# -*- coding: utf-8 -*-
"""Deterministic PDFE.exe launcher.

Determinism contract (see DETERMINISM.md):
- SetProcessDPIAware forces 96 DPI logical resolution.
- PDFE window is resized to exactly 1600 x 1000 logical pixels.
- Qt high-DPI env vars are set to 100% scaling before launch.
- Fit-page zoom is applied after launch by invoking the fitPageAction.
- Light theme is assumed (no dark-mode env override applied; the OS
  must be in light mode for deterministic icon rendering).

Usage
-----
    from tests.ui_harness.core.launch import PDFESession
    with PDFESession(pdf_path="tests/fixtures/test_single.pdf") as session:
        window = session.window          # pywinauto WindowSpecification
        pid    = session.pid

Environment requirements (from DETERMINISM.md):
- Windows 10/11 or Windows Server 2019 at 100% display scaling.
- No per-monitor DPI overrides active for pdfe.exe.
- Qt6 runtime present in the same directory as pdfe.exe.
"""

import ctypes
import os
import subprocess
import time
import psutil
import pywinauto

# ---------------------------------------------------------------------------
# Public constants
# ---------------------------------------------------------------------------

_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def pdfe_exe() -> str:
    """Absolute path to the application binary, honouring PDFE_BUILD_CONFIG.

    GitHub #270. This used to hardcode ``Debug`` and consult no environment
    variable at all, while the OTHER UI harness
    (``tests/ui_automation/harness/pdfe_app_fixture.py``) did honour
    ``PDFE_BUILD_CONFIG`` — and three of its callers set it to ``Release``. So
    the variable plainly selects the configuration, and this harness silently
    ignored it. ``tests/helpers/pdfe_session.py`` re-exports the constant and
    ``tests/test_ui_stress.py`` imports it from there, so **the whole stress
    suite launched the Debug binary no matter what the variable said**.

    The failure mode is the one this project has already paid for several times:
    a stale binary, with nothing reporting that the requested configuration was
    not the one under test. It is a silent no-op, not an error.

    Resolved at CALL time rather than import time, deliberately. A module-level
    constant is read once, so a test that sets the variable after this module is
    first imported — directly or transitively, and pytest imports are ordered by
    collection, not by intent — gets the value from whenever the first import
    happened. That is a second silent no-op of exactly the same shape, and
    avoiding it is why this is a function.
    """
    config = os.environ.get("PDFE_BUILD_CONFIG", "Debug")
    return os.path.join(_REPO_ROOT, "build", "vs2022", "bin", config, "pdfe.exe")


# Back-compat: ~274 references across ~91 files read PDFE_EXE directly, almost
# all of them to display a path or check existence. Renaming them is a far
# larger change than the defect warrants, so the name stays and resolves at
# import for those uses. Anything that LAUNCHES the application must call
# pdfe_exe() instead, so the environment is honoured — see launch_pdfe below.
PDFE_EXE = pdfe_exe()

# Target window size requested via SetWindowPos.
# The OS / Qt may adjust for window frame and minimum-size constraints; the
# actual achieved size is typically ~1584 x 961 on Windows Server 2019 at 96 DPI.
WINDOW_WIDTH = 1600
WINDOW_HEIGHT = 1000
WINDOW_TITLE_PREFIX = "PDFE"

# Seconds to wait for the main window to appear after launch.
LAUNCH_TIMEOUT = 30

# Seconds between readiness polls.
POLL_INTERVAL = 0.5


def _set_process_dpi_aware():
    """Mark THIS Python process as PER_MONITOR_AWARE_V2 DPI-aware.

    GAP-55 fix: SetProcessDpiAwareness(1) [SYSTEM_DPI_AWARE] still allows
    Windows to virtualize coordinates when the process is on a monitor
    whose DPI differs from the system DPI at process start, or when the
    OS interprets non-standard scaling (e.g. ~162%) by axis-swapping the
    reported screen geometry. On this rig the SYSTEM_AWARE level caused
    GetSystemMetrics(SM_CXSCREEN) to report 1154x1844 (apparent portrait)
    when the actual physical display is 1874x1124 (landscape), causing
    SetWindowPos(1600,...) to silently clamp the window to 1158px wide
    and cascading into ~190 width-dependent test failures.

    PER_MONITOR_AWARE_V2 (-4) reports real physical pixels on every
    monitor with no virtualization, regardless of system DPI mismatch.
    The fallback chain degrades to PER_MONITOR_AWARE, then
    SYSTEM_AWARE, then the legacy SetProcessDPIAware on older OSes.
    """
    try:
        # Win10 1703+: PER_MONITOR_AWARE_V2 (most accurate, handles
        # mixed-DPI multi-monitor and non-standard scaling correctly).
        ctypes.windll.user32.SetProcessDpiAwarenessContext(-4)
        return
    except (AttributeError, OSError):
        pass
    try:
        # Win8.1+: PER_MONITOR_AWARE (level 2).
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
        return
    except (AttributeError, OSError):
        pass
    try:
        # Win8.1+: SYSTEM_AWARE (level 1) — still vulnerable to the
        # GAP-55 axis-swap on non-standard scaling, but better than
        # unaware.
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
        return
    except (AttributeError, OSError):
        pass
    try:
        # Vista+: legacy system-aware.
        ctypes.windll.user32.SetProcessDPIAware()
    except (AttributeError, OSError):
        pass  # Pre-Vista fallback; proceed anyway.


def _qt_env_for_determinism():
    """Return env dict with Qt env vars pinned to 100% scaling / light theme."""
    env = os.environ.copy()
    # Force 1.0 (100%) device-pixel-ratio so all sizes are in logical pixels.
    env["QT_SCALE_FACTOR"] = "1"
    env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    env["QT_SCREEN_SCALE_FACTORS"] = "1"
    # Disable Qt's own DPI scaling; we rely on SetProcessDPIAware at the OS level.
    env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    # Pipeline b3f19d72 phase-4 (P7-BUG-2 fix): bypass the modal
    # AtlasComparisonDialog that opens on first span click on a
    # scanned-with-OCR page.  Without this seam the dialog blocks the
    # synchronous spanActivated → AtlasBuilder chain, so subsequent
    # keystrokes typed by the test are routed to the dialog (ignored)
    # instead of reaching TextEditTool::onKeyPress; the ImageReplacer
    # pipeline never runs and the canvas-change assertion fails.  See
    # MainWindow.cpp:4967 for the corresponding C++ seam.
    env["PDFE_SKIP_ATLAS_DIALOG"] = "1"
    # Crash observability (CrashHandler teardown-AV work): mark this as an
    # automated run so a crash writes a minidump and exits FAST instead of
    # popping a modal QMessageBox that blocks pywinauto until the per-test
    # timeout (the observed ~30-min UI-suite stalls). Route dumps into ui-out,
    # which the workflow uploads as artifacts, so an otherwise-invisible teardown
    # Access Violation leaves a stack behind for post-mortem.
    env["PDFE_UI_TEST"] = "1"
    _repo_root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    _dump_dir = os.path.join(_repo_root, "ui-out", "crashdumps")
    try:
        os.makedirs(_dump_dir, exist_ok=True)
    except OSError:
        pass
    env.setdefault("PDFE_CRASH_DUMP_DIR", _dump_dir)
    # Pipeline b3f19d72 phase-8 (P14-INFRA-1 + P13-INFRA-4 fix): suppress
    # native QPrinter/PrintEngine when PrintDialog is accepted during tests.
    # onPrint() still opens our own PrintDialog so keyboard tests can
    # Tab/Escape through it; only the OS print job is bypassed.  See
    # MainWindow.cpp onPrint() for the seam implementation.
    env["PDFE_TEST_DRYRUN_PRINT"] = "1"
    # Pipeline b3f19d72 phase-9 (P13-INFRA-4 ValidationRequestDialog fix):
    # lower requestValidationAction enable threshold from validCount to
    # signedCount so self-signed test certs (which fail chain validation)
    # still enable the "Request Validation Data..." action after Verify
    # Signatures is called.  The dialog itself is purely a consent UI;
    # no network calls are made unless the user clicks "Fetch" (the
    # default button is Cancel, so test Escape/Tab/Enter are all safe).
    env["PDFE_TEST_VALIDATION_REQUEST_UNLOCKED"] = "1"
    # Pipeline b3f19d72 phase-11 (test infrastructure): suppress the
    # "Unsaved changes — Save / Discard / Cancel" QMessageBox that appears
    # on close when undoManager_->isClean() == false.  Without this seam
    # the modal dialog blocks UIA teardown, contaminating screenshots and
    # orphaning PDFE.exe processes that cascade into "PDFE didn't appear
    # within 30s" failures in the next test.  See MainWindow.cpp
    # promptSaveOnClose() for the corresponding C++ seam.
    env["PDFE_TEST_SKIP_UNSAVED_PROMPT"] = "1"
    # Pipeline f2a8c3e1 phase-2 (P11-UIA-FLAKE-1 fix): enable the annotation
    # property pipe seams so Python tests can apply color / author / contents
    # changes directly without driving QColorDialog or UIA QLineEdit paths,
    # which race against the Qt event loop and cause intermittent failures.
    # Inert in production (the three hidden QLineEdit widgets are never created
    # when this env var is absent).
    env["PDFE_TEST_ANNOT_PIPES"] = "1"
    # Pipeline b3d7f942 phase-1 (GAP-15): enable the UI/form/click pipe seams
    # so Python tests can drive testToolActivatePipe, testClickByPdfCoordPipe,
    # testFontFamiliesQueryPipe, testFormFieldValueSetPipe, and
    # testFormFieldValueQueryPipe.  Distinct from PDFE_TEST_ANNOT_PIPES —
    # that gate covers annotation-property pipes only.  Inert in production
    # (the five hidden QLineEdit widgets are never created when this env var
    # is absent).
    env["PDFE_TEST_PIPES"] = "1"
    # Pipeline b3f19d72 phase-12 (P9-DEBT-1 fix): enable PDF JavaScript
    # for form calculations during UI tests.  The production default is
    # OFF (security hardening); UI tests that verify JS event side-effects
    # (validate/calculate/keystroke) need it ON.  PDFE_ENABLE_JS=1 causes
    # MainWindow to call jsEngine_->enable() at document load regardless
    # of the persistent QSettings value.  This env var does NOT alter
    # the stored preference.
    env["PDFE_ENABLE_JS"] = "1"
    return env


def _kill_orphan_pdfe():
    """Kill any existing pdfe.exe processes so they do not interfere."""
    for proc in psutil.process_iter(["name", "pid"]):
        try:
            if proc.info["name"] and proc.info["name"].lower() == "pdfe.exe":
                proc.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    time.sleep(0.3)


def launch_pdfe(pdf_path=None, kill_existing=True):
    """Launch PDFE.exe and return (subprocess.Popen, pywinauto.Application).

    Parameters
    ----------
    pdf_path : str or None
        If provided, passed as the first positional argument so PDFE opens the file.
    kill_existing : bool
        When True, any existing pdfe.exe processes are killed first.

    Returns
    -------
    proc : subprocess.Popen
    app  : pywinauto.Application (UIA backend, connected to the new window)
    """
    _set_process_dpi_aware()

    if kill_existing:
        _kill_orphan_pdfe()

    # GitHub #270: pdfe_exe(), NOT the PDFE_EXE constant. This is the one place
    # that decides which binary actually runs, so it must read the environment
    # now rather than at whenever this module was first imported.
    exe = pdfe_exe()
    if not os.path.exists(exe):
        # Fail with the configuration named. The old code's failure mode was a
        # missing-file error that said nothing about PDFE_BUILD_CONFIG, so a
        # Release run on a Debug-only tree looked like a broken checkout.
        raise FileNotFoundError(
            f"pdfe.exe not found at {exe} "
            f"(PDFE_BUILD_CONFIG={os.environ.get('PDFE_BUILD_CONFIG', 'Debug')!r}). "
            "Build that configuration, or unset PDFE_BUILD_CONFIG to use Debug."
        )

    cmd = [exe]
    if pdf_path:
        cmd.append(os.path.abspath(pdf_path))

    env = _qt_env_for_determinism()

    proc = subprocess.Popen(cmd, env=env)

    # Wait for the main window.
    app = pywinauto.Application(backend="uia")
    deadline = time.time() + LAUNCH_TIMEOUT
    while time.time() < deadline:
        try:
            app.connect(process=proc.pid, timeout=POLL_INTERVAL)
            win = app.window(title_re=r".*PDFE.*")
            win.wait("visible", timeout=5)
            break
        except Exception:  # noqa: BLE001
            time.sleep(POLL_INTERVAL)
    else:
        proc.kill()
        raise TimeoutError(
            f"PDFE main window did not appear within {LAUNCH_TIMEOUT}s."
        )

    # Resize and move to a deterministic position.
    _resize_window(app, proc.pid)

    return proc, app


#: Outcome of the most recent :func:`_resize_window` call, in this process.
#:
#: GitHub #284. The resize can silently land somewhere other than the requested
#: size, and callers had no way to find out -- the function returned None and
#: swallowed exceptions. Tests that care about toolbar overflow (pixel oracles,
#: UIA reachability of toolbar actions) can read this instead of assuming.
#:
#: Keys: ``requested`` (w, h); ``achieved`` (w, h) or None if the call raised;
#: ``exact`` bool; ``escalations`` int; ``error`` repr or None.
LAST_RESIZE: dict = {
    "requested": None, "achieved": None, "exact": False,
    "escalations": 0, "error": "not attempted",
}


def _resize_window(app, pid):
    """Move & resize the PDFE window to WINDOW_WIDTH x WINDOW_HEIGHT at (0, 0).

    Uses SetWindowPos directly via ctypes because the pywinauto UIA backend
    does not expose move_window on Win32 top-level windows.
    SWP_FRAMECHANGED (0x20) forces the window to recalculate its non-client area.

    GAP-28 fix: the bare except-pass pattern silently hid cases where the resize
    call landed at 1158px instead of the required >= 1400px (observed on Windows
    Server 2019 when the Python process's DPI-awareness context differs from the
    PDFE.exe process context, causing the OS to scale the coordinates).  The fix:
    (1) ensure DPI awareness is set on this Python process before making the call,
    (2) verify the achieved width after the call, and
    (3) retry once with SWP_NOACTIVATE | SWP_FRAMECHANGED if the first attempt
        falls short, using the logical pixel dimensions derived from the actual
        system DPI rather than assuming 96 dpi.
    """
    from ctypes import wintypes  # avoid top-level wintypes import on non-Windows

    escalations = 0
    achieved_w = achieved_h = -1
    try:
        # Re-apply DPI awareness here (belt-and-suspenders: launch_pdfe() also
        # calls _set_process_dpi_aware() but the ordering relative to connect()
        # can leave the context stale on some OS configurations).
        _set_process_dpi_aware()

        win = app.window(title_re=r".*PDFE.*")
        win.set_focus()
        hwnd = win.handle
        user32 = ctypes.windll.user32

        # SWP_FRAMECHANGED = 0x20; SWP_NOACTIVATE = 0x10
        SWP_FLAGS = 0x20 | 0x10
        user32.SetWindowPos(hwnd, 0, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, SWP_FLAGS)
        time.sleep(0.4)  # allow repaint + Qt geometry recalculation

        # Verify the achieved size via GetWindowRect (bypasses pywinauto's UIA
        # coordinate transformation, which can read scaled coordinates on
        # non-100%-DPI desktops).
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        achieved_w = rect.right - rect.left
        achieved_h = rect.bottom - rect.top

        if achieved_w < 1400:
            # Escalating retry — re-requesting the same WINDOW_WIDTH (1600)
            # would yield the same clamped result. Step up the requested width
            # to compensate for residual DPI / virtual-screen clamping. Stop as
            # soon as we cross the 1400px threshold or exhaust the escalation
            # ladder; if the screen is physically narrower than 1400 (e.g.
            # portrait-rotated desktop), no escalation can succeed and the
            # caller-side AC2 skip path in tests must handle it.
            for target_w in (1800, 2000, 2400):
                user32.SetWindowPos(hwnd, 0, 0, 0, target_w, WINDOW_HEIGHT, SWP_FLAGS)
                time.sleep(0.4)
                user32.GetWindowRect(hwnd, ctypes.byref(rect))
                escalations += 1
                if (rect.right - rect.left) >= 1400:
                    break
            achieved_w = rect.right - rect.left
            achieved_h = rect.bottom - rect.top

        # RECORD THE OUTCOME (GitHub #284). This function guarantees a LOWER
        # BOUND, not a value: the achieved width may be WINDOW_WIDTH, any rung
        # of the escalation ladder, or whatever the DPI / virtual-screen clamp
        # yields -- 1158 px has been observed here (GAP-28). Toolbars overflow
        # differently at different widths, so "at least 1400, probably" is not
        # a basis for a pixel oracle or for a UIA reachability assertion.
        #
        # Making it exact is not something this function can promise: the OS
        # has the last word. What it CAN do is stop being silent about it, so a
        # caller can assert on the real geometry instead of assuming one. The
        # bare `except: pass` below swallowed even the failures.
        LAST_RESIZE.update(
            requested=(WINDOW_WIDTH, WINDOW_HEIGHT),
            achieved=(achieved_w, achieved_h),
            exact=(achieved_w == WINDOW_WIDTH and achieved_h == WINDOW_HEIGHT),
            escalations=escalations,
            error=None,
        )
        if not LAST_RESIZE["exact"]:
            print(
                "[pdfe-harness] WINDOW SIZE IS NOT THE REQUESTED ONE: asked "
                f"{WINDOW_WIDTH}x{WINDOW_HEIGHT}, got {achieved_w}x{achieved_h} "
                f"after {escalations} escalation(s). Toolbar overflow, and "
                "therefore pixel oracles and UIA reachability, depend on this "
                "(GitHub #284).",
                flush=True,
            )
        if (os.environ.get("PDFE_TEST_REQUIRE_EXACT_WINDOW") == "1"
                and not LAST_RESIZE["exact"]):
            raise RuntimeError(
                f"PDFE_TEST_REQUIRE_EXACT_WINDOW=1 but the window is "
                f"{achieved_w}x{achieved_h}, not {WINDOW_WIDTH}x{WINDOW_HEIGHT}"
            )

    except Exception as exc:  # noqa: BLE001
        # Still non-fatal -- a rig whose window manager refuses the move should
        # run the non-visual tests rather than fail the whole session -- but no
        # longer SILENT. An unrecorded failure here looks identical to success
        # to every caller, which is how a 1158px window went unnoticed.
        if os.environ.get("PDFE_TEST_REQUIRE_EXACT_WINDOW") == "1":
            raise
        LAST_RESIZE.update(requested=(WINDOW_WIDTH, WINDOW_HEIGHT),
                           achieved=None, exact=False, error=repr(exc))
        print(f"[pdfe-harness] window resize failed: {exc!r}", flush=True)


def quit_pdfe(proc, app, timeout=10):
    """Close PDFE gracefully; fall back to kill if needed.

    Parameters
    ----------
    proc : subprocess.Popen
    app  : pywinauto.Application
    timeout : int  seconds to wait for graceful close before kill
    """
    try:
        win = app.window(title_re=r".*PDFE.*")
        win.close()
        proc.wait(timeout=timeout)
    except Exception:  # noqa: BLE001
        pass
    finally:
        try:
            if proc.poll() is None:
                proc.kill()
                proc.wait(2)
        except Exception:  # noqa: BLE001
            pass


def no_orphan_pdfe():
    """Return True if no pdfe.exe is running (for teardown verification)."""
    for proc in psutil.process_iter(["name"]):
        try:
            if proc.info["name"] and proc.info["name"].lower() == "pdfe.exe":
                return False
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return True


# ---------------------------------------------------------------------------
# Context manager wrapper
# ---------------------------------------------------------------------------

class PDFESession:
    """Context manager that launches PDFE and ensures clean teardown.

    Example
    -------
        with PDFESession(pdf_path="tests/fixtures/test_single.pdf") as s:
            s.window.menu_select("Help->About PDFE")
    """

    def __init__(self, pdf_path=None, kill_existing=True):
        self._pdf_path = pdf_path
        self._kill_existing = kill_existing
        self.proc = None
        self.app = None

    @property
    def window(self):
        """Return the pywinauto WindowSpecification for the main PDFE window."""
        return self.app.window(title_re=r".*PDFE.*")

    @property
    def pid(self):
        return self.proc.pid if self.proc else None

    def __enter__(self):
        self.proc, self.app = launch_pdfe(
            pdf_path=self._pdf_path,
            kill_existing=self._kill_existing,
        )
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        quit_pdfe(self.proc, self.app)
        return False  # do not suppress exceptions
