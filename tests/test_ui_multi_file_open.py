# -*- coding: utf-8 -*-
"""GitHub #241 — launching PDFE with several PDFs must open all of them.

WHY THIS TEST EXISTS AT THE UI LAYER

The GoogleTest suite covers the two layers the fix touches — ``WindowManager``
(one window per document, the same file never twice) and ``SingleInstanceGuard``
(N terminated lines on the wire). It cannot cover the third: ``main()`` is not
linked into ``pdfe_tests``, so the loop over ``parser.positionalArguments()`` —
the exact line the issue names, ``main.cpp:163`` — has no unit test by
construction. Claiming the fix was verified without this would be citing the
wrong layer.

So this one launches the real ``pdfe.exe`` with two paths on the command line
and counts the windows that appear.

WHY IT COUNTS WINDOWS VIA RAW WIN32

``tests/helpers/modal_dialog.list_process_windows`` uses ``EnumWindows``
directly. UIA is not used anywhere here: it cannot reliably enumerate this
application's windows (see that module's header for the measured four-minute
blind spot), and a window count is precisely the kind of question a blind
enumerator answers wrongly and confidently.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.modal_dialog import close_window, list_process_windows  # noqa: E402

EXE = REPO_ROOT / "build" / "vs2022" / "bin" / "Debug" / "pdfe.exe"
FIXTURES = REPO_ROOT / "tests" / "fixtures"


def _editor_windows(pid: int):
    """Top-level windows of `pid` that look like an editor window.

    Filters out the zero-size helper/message windows Qt parents to the
    application, which are real top-level HWNDs but not documents.
    """
    return [(hwnd, title) for hwnd, title in list_process_windows(pid) if title]


def _wait_for_window_count(pid: int, expected: int, timeout_s: float = 45.0):
    """Wait until `pid` has `expected` titled windows; return what it had."""
    deadline = time.time() + timeout_s
    seen = []
    while time.time() < deadline:
        seen = _editor_windows(pid)
        if len(seen) >= expected:
            # Settle briefly and re-read, so a count that is still climbing is
            # not mistaken for the final answer.
            time.sleep(1.0)
            return _editor_windows(pid)
        time.sleep(0.5)
    return seen


@pytest.fixture
def launched():
    """Launch pdfe.exe with the given args; guarantee teardown."""
    procs = []

    def _launch(*args):
        env = dict(os.environ)
        # F63 — partition the single-instance domain so this test can never
        # forward into (or be captured by) a PDFE the operator has open, and so
        # concurrent test processes do not collide on the per-user name.
        env["PDFE_INSTANCE_DOMAIN"] = f"p94-multiopen-{os.getpid()}"
        proc = subprocess.Popen([str(EXE), *[str(a) for a in args]], env=env)
        procs.append(proc)
        return proc

    yield _launch

    for proc in procs:
        for hwnd, _ in _editor_windows(proc.pid):
            try:
                close_window(hwnd)
            except Exception:
                pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            # An orphaned pdfe.exe holds the single-instance discriminator and
            # wedges the next run, so teardown must be certain.
            proc.kill()
            proc.wait(timeout=15)


pytestmark = pytest.mark.skipif(
    not EXE.exists(), reason=f"pdfe.exe not built at {EXE}"
)


def test_two_files_on_the_command_line_open_two_windows(launched):
    """The reported defect: arguments 2..N were discarded at main.cpp:163."""
    a = FIXTURES / "test_single.pdf"
    b = FIXTURES / "test_multipage.pdf"
    assert a.exists() and b.exists(), "fixtures missing"

    proc = launched(a, b)
    windows = _wait_for_window_count(proc.pid, 2)

    assert len(windows) == 2, (
        f"expected one window per file, got {len(windows)}: {windows}. "
        "Only the first positional argument reached a document."
    )


def test_the_same_file_named_twice_opens_one_window(launched):
    """R3-02 must survive the fix.

    Two editors on one file can each save it, and whichever saves last silently
    clobbers the other's edits. Opening N documents must never become opening N
    editors on one document.
    """
    a = FIXTURES / "test_single.pdf"
    assert a.exists(), "fixture missing"

    proc = launched(a, a)
    # Give a would-be second window as long as a real one would need, so this
    # is a measurement rather than a race that happens to pass.
    windows = _wait_for_window_count(proc.pid, 2, timeout_s=20.0)

    assert len(windows) == 1, (
        f"the same file was opened in {len(windows)} windows: {windows}. "
        "Two editors on one document is the R3-02 data-loss window."
    )


def test_a_single_file_still_opens_exactly_one_window(launched):
    """The N=1 case must be unchanged by the widening."""
    a = FIXTURES / "test_single.pdf"
    assert a.exists(), "fixture missing"

    proc = launched(a)
    windows = _wait_for_window_count(proc.pid, 1)

    assert len(windows) == 1, f"expected exactly one window, got {windows}"
