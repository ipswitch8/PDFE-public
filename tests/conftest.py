# -*- coding: utf-8 -*-
"""Session-scoped pytest fixtures enforcing the output / fixture invariants.

Two problems this file makes structurally impossible:

1. **Fixture corruption in place.**  E2E tests drive Save / Optimize /
   Encrypt / Batch / Sign dialogs.  If a test accepts the default
   filename, the output lands next to (and overwrites) the source
   fixture.  The ``_fixture_integrity_guard`` session fixture sha256-
   snapshots every file under ``tests/fixtures/`` at session start and
   re-snapshots at session end — any drift (mtime *or* content) aborts
   the session with a listing of the offenders.

2. **Disk-filling output accumulation.**  Pytest's ``tmp_path`` is kept
   for the last 3 sessions under ``%TEMP%/pytest-of-dev/`` by
   default.  Combined with stray screenshots under
   ``tests/visual/_failure_artifacts/`` (never cleaned) the working
   tree grew unbounded.  The ``_janitor`` session fixture runs first,
   sweeping anything under ``tests/_tmp/``, ``tests/fixtures/_runtime/``,
   ``tests/visual/_failure_artifacts/``, and ``%TEMP%/pytest-of-*/``
   that matches our per-run naming convention AND is older than 24 h.
   ``tests/visual/_failure_artifacts/`` is additionally capped to the
   20 most-recent files.

Both fixtures are session-scoped autouse so they run automatically — no
test has to opt in.  The ``run_id`` fixture is provided for convenience
when a test wants to embed the run id in a non-output context (log
filename, etc).
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import os
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import pytest

# Project root so helpers import regardless of where pytest was launched.
_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from tests.helpers import output_paths as _op  # noqa: E402


def pytest_configure(config):  # noqa: ARG001
    """Install the global PyAutoGUI click-safety net, suite-wide.

    Absolute-coordinate clicks computed from a stale / minimised / off-screen
    widget rect land outside PDFE — on the desktop, another app, or the Windows
    TASKBAR, which launches whatever icon is under the cursor. That has happened
    repeatedly on the self-hosted runner (a real desktop). Per-call-site guards
    kept missing sites (click sites are spread across tests/, tests/ui_automation/
    and tests/ui_harness/), so the guard is installed here — before any test runs
    — where no module can bypass it. See tests/helpers/click_safety.py.

    The per-process basetemp (GitHub #257) is installed by the REPO-ROOT
    conftest.py, not here: this file covers tests/ only, and the gate suites
    under tools/ need it too.
    """
    try:
        from tests.helpers.click_safety import (
            install_pyautogui_click_guard,
            install_pywinauto_click_guard,
            disable_active_window_tracking_for_session,
        )
        # Disable Windows "active window tracking" (X-Mouse: the window UNDER
        # THE CURSOR becomes active) for the duration of the UI suite. It is
        # fundamentally incompatible with mouse-driven UI automation — whenever
        # a test's cursor is not over PDFE, PDFE loses the foreground, so
        # keystrokes leak and hotkey_to's SetForegroundWindow fights the
        # tracking and fails ("could not bring HWND to foreground within 2.0s").
        # This caused a broad, hard-to-attribute UI-suite failure cascade.
        # Restored in pytest_unconfigure.
        disable_active_window_tracking_for_session()
        install_pyautogui_click_guard()
        # pywinauto MUST be guarded too: most clicks in this suite go through
        # wrapper.click_input() / pywinauto.mouse (its own SendInput path), which
        # never touches PyAutoGUI. An app was still being launched from the
        # taskbar with only the PyAutoGUI net installed.
        install_pywinauto_click_guard()
    except Exception:  # noqa: BLE001
        # pyautogui/pywinauto absent (headless/Linux CI) — nothing to guard.
        pass


def pytest_unconfigure(config):  # noqa: ARG001
    """Restore the active-window-tracking setting captured at session start."""
    try:
        from tests.helpers.click_safety import restore_active_window_tracking
        restore_active_window_tracking()
    except Exception:  # noqa: BLE001
        pass


# ---------------------------------------------------------------------------
# Constants — directories under the fixture tree that are NOT integrity-
# checked (their contents are expected to change between runs).
# ---------------------------------------------------------------------------

_FIXTURES_DIR = _TESTS_DIR / "fixtures"
_FIXTURES_RUNTIME = _FIXTURES_DIR / "_runtime"
_TESTS_TMP = _TESTS_DIR / "_tmp"
_VISUAL_ARTIFACTS = _TESTS_DIR / "visual" / "_failure_artifacts"

# Subdirectories under tests/fixtures/ that are intentionally mutable —
# generators write into them, tests read from them, contents are NOT
# part of the integrity snapshot.
_FIXTURE_MUTABLE_SUBDIRS = frozenset({
    "_runtime",         # this file's own staging area for E2E outputs
    "__pycache__",      # Python bytecode for fixture generator scripts
    "batch_output",     # legacy: BatchDialog target (gitignored)
    "ui_audit",         # ad-hoc UI audit output
    "ui_screenshots",   # ad-hoc UI screenshots
})

# File suffixes that are explicitly transient even when they appear
# under tests/fixtures/.  These are emitted by application code paths
# and must not trip the integrity guard.
#
# WIN-87 phase-57 -- ``.redact_tmp`` / ``.redact_out`` NO LONGER EXIST.
# RedactionManager used to stage its bake and its QPDF output at those two
# names, both derived from the user's own document path.  They are gone: the
# rewrite now runs entirely behind ``StagingFile`` exclusive handles at
# unpredictable names, and only the final output is ever published.  The two
# suffixes are KEPT here -- a developer switching branches, or an older build,
# can still have real ones on disk, and recognising a name that can no longer
# be produced costs nothing -- but they can no longer carry this guard, so the
# staging-residue names are recognised alongside them.
#
# The residue shapes are ``StagingFile``'s, spelled once in
# ``src/core/AtomicFileReplace.h`` (``kSuffixMarker`` = ``.pdfe_part.`` plus 32
# hex digits) and once as the full-rewrite temp ``.pdfe_tmp``.  This is the
# Python mirror of ``StagingFile::isStagingResidueName``; it is a mirror rather
# than a second matcher, and if the C++ shape changes this must move with it.
_FIXTURE_TRANSIENT_SUFFIXES = frozenset({
    ".redact_tmp",     # legacy: removed by WIN-87 phase-57
    ".redact_out",     # legacy: removed by WIN-87 phase-57
    ".pdfe_tmp",       # StagingFile::createFullRewriteTempFor
    ".tmp",
    ".bak",
})

# ``<anything>.pdfe_part.<32 hex digits>`` -- StagingFile::createBeside's
# unpredictable staging name (StagingFile::kSuffixMarker + 32 hex).  A suffix
# set cannot express it, because the 32 hex digits differ every time.
_STAGING_RESIDUE_RE = re.compile(r"\.pdfe_part\.[0-9a-fA-F]{32}$")

# Files under tests/fixtures/ that generators rewrite legitimately.
# These are skipped from the integrity check (they live in the fixture
# tree only because the generator + the data live together).
_FIXTURE_GENERATED_SIDECARS = frozenset({
    "validate_sidecars.py",
})

# Age threshold for janitor cleanup of cross-session leftovers.
_STALE_AGE_SECONDS = 24 * 3600

# Cap for visual failure artifacts (most-recent-by-mtime kept).
_VISUAL_ARTIFACT_KEEP = 20

#: How many RUN directories of visual artifacts to retain (GitHub #289).
#: Smaller than the per-file cap on purpose: each run directory can hold many
#: diffs, so 20 runs of a badly-failing suite would be a lot of disk, while the
#: investigations these exist for compare a handful of recent runs. The point of
#: the change is that a run cannot destroy another run's evidence -- not that
#: evidence is kept forever.
_VISUAL_ARTIFACT_RUNS_KEEP = 8


# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------

class FixtureCorruptionError(RuntimeError):
    """Raised at session teardown when fixture sha256 / mtime drift is
    detected.  Includes the list of mutated paths so the offending test
    can be identified by mtime correlation.
    """


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _is_excluded_fixture_path(rel_path: Path) -> bool:
    """Return True if ``rel_path`` (relative to tests/fixtures/) lives in
    a mutable subdirectory or matches a transient pattern.
    """
    parts = rel_path.parts
    if not parts:
        return True
    # First-level mutable subdir.
    if parts[0] in _FIXTURE_MUTABLE_SUBDIRS:
        return True
    # Match per-run output naming anywhere in the tree.
    if _op.is_our_output(Path(parts[-1])):
        return True
    # Transient suffixes.
    name = parts[-1]
    for suf in _FIXTURE_TRANSIENT_SUFFIXES:
        if name.endswith(suf):
            return True
    # StagingFile residue, whose tail is random and cannot be a fixed suffix.
    if _STAGING_RESIDUE_RE.search(name):
        return True
    # Generator sidecars.
    if name in _FIXTURE_GENERATED_SIDECARS:
        return True
    return False


def _hash_file(path: Path, _chunk: int = 1 << 16) -> str:
    h = hashlib.sha256()
    try:
        with path.open("rb") as f:
            while True:
                buf = f.read(_chunk)
                if not buf:
                    break
                h.update(buf)
    except OSError:
        return "<unreadable>"
    return h.hexdigest()


def _walk_fixture_tree() -> List[Path]:
    """Return every regular file under tests/fixtures/ that is NOT
    excluded from the integrity check.
    """
    if not _FIXTURES_DIR.exists():
        return []
    out: List[Path] = []
    for root, dirs, files in os.walk(_FIXTURES_DIR):
        root_p = Path(root)
        rel_root = root_p.relative_to(_FIXTURES_DIR)
        # Prune mutable subdirs in-place so os.walk doesn't descend.
        dirs[:] = [
            d for d in dirs
            if not _is_excluded_fixture_path(rel_root / d)
        ]
        for name in files:
            rel = rel_root / name
            if _is_excluded_fixture_path(rel):
                continue
            out.append(root_p / name)
    return out


def _snapshot_fixtures() -> Dict[Path, Tuple[str, float, int]]:
    """Return {path: (sha256, mtime, size)} for every tracked fixture."""
    snap: Dict[Path, Tuple[str, float, int]] = {}
    for p in _walk_fixture_tree():
        try:
            st = p.stat()
        except OSError:
            continue
        snap[p] = (_hash_file(p), st.st_mtime, st.st_size)
    return snap


def _format_drift(snap_before: Dict[Path, Tuple[str, float, int]],
                  snap_after: Dict[Path, Tuple[str, float, int]],
                  ) -> Optional[str]:
    """Return a multi-line diagnostic if anything drifted, else None."""
    before_keys = set(snap_before)
    after_keys = set(snap_after)

    new_files = sorted(after_keys - before_keys)
    deleted = sorted(before_keys - after_keys)
    mutated: List[Tuple[Path, str]] = []
    for p in before_keys & after_keys:
        b_h, b_mtime, b_size = snap_before[p]
        a_h, a_mtime, a_size = snap_after[p]
        if b_h != a_h:
            mutated.append((p, f"sha256 {b_h[:12]}.. -> {a_h[:12]}.."))
        elif b_size != a_size:
            mutated.append((p, f"size {b_size} -> {a_size}"))
        elif abs(b_mtime - a_mtime) > 1.0:
            # mtime drift without content change is suspicious but not
            # fatal on its own; record for visibility.
            mutated.append((p, f"mtime drift {b_mtime:.1f} -> {a_mtime:.1f}"))

    if not new_files and not deleted and not mutated:
        return None

    lines: List[str] = ["Fixture integrity drift detected:"]
    if mutated:
        lines.append(f"  MUTATED ({len(mutated)}):")
        for p, reason in mutated:
            lines.append(f"    {p.relative_to(_FIXTURES_DIR)}: {reason}")
    if new_files:
        lines.append(f"  NEW FILES ({len(new_files)}):")
        for p in new_files:
            lines.append(f"    {p.relative_to(_FIXTURES_DIR)}")
    if deleted:
        lines.append(f"  DELETED ({len(deleted)}):")
        for p in deleted:
            lines.append(f"    {p.relative_to(_FIXTURES_DIR)}")
    lines.append("")
    lines.append(
        "Every E2E test that writes output MUST use "
        "tests/helpers/output_paths.gen_out(tmp_path, ...) so output "
        "lands under tests/_tmp/ — never next to a fixture."
    )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Janitor — sweep stale outputs before integrity guard runs.
# ---------------------------------------------------------------------------

def _iter_stale_outputs() -> List[Path]:
    """Yield candidate stale output paths from every directory we track."""
    cands: List[Path] = []
    roots: List[Path] = [
        _TESTS_TMP,
        _FIXTURES_RUNTIME,
        _VISUAL_ARTIFACTS,
    ]
    # %TEMP%/pytest-of-* — pytest's default basetemp root.  We still
    # scan it in case a pre-pytest.ini test run left files there.
    sys_tmp = Path(tempfile.gettempdir())
    if sys_tmp.exists():
        try:
            for d in sys_tmp.iterdir():
                if d.is_dir() and d.name.startswith("pytest-of-"):
                    roots.append(d)
        except OSError:
            pass

    for root in roots:
        if not root.exists():
            continue
        try:
            for p in root.rglob("*"):
                if p.is_file():
                    cands.append(p)
        except OSError:
            continue
    return cands


def _run_janitor(reporter) -> int:
    """Sweep stale per-run outputs.  Returns count deleted."""
    deleted = 0
    now = time.time()
    for p in _iter_stale_outputs():
        try:
            # Only delete files that match our per-run naming (avoid
            # collateral damage on baseline images / configs).  Visual
            # _failure_artifacts have non-per-run names so we apply a
            # second rule there.
            is_ours = _op.is_our_output(p)
            if is_ours:
                if _op.age_seconds(p, now=now) > _STALE_AGE_SECONDS:
                    p.unlink(missing_ok=True)
                    deleted += 1
        except OSError:
            continue

    # Cap visual failure artifacts.
    #
    # TWO rules, because there are now two kinds of entry here (GitHub #289).
    #
    # Loose FILES are the writers that already named themselves per-run --
    # helpers/file_dialog.py has always used
    # "overwrite_prompt_<run_id>_<ts>.png", so it was never subject to the
    # overwrite defect and the mtime cap is the right bound for it.
    #
    # Run DIRECTORIES are new. The comment that used to sit here said "Visual
    # _failure_artifacts have non-per-run names so we apply a second rule
    # there" -- an accurate description of the defect #289 was filed about: a
    # path derived only from the baseline name, so a later run overwrote the
    # evidence an investigation was citing. Artifacts are now scoped by RUN_ID
    # (output_paths.visual_failure_dir), which fixes that and would ALSO have
    # silently disabled this cap: iterdir() + is_file() matches nothing inside
    # a subdirectory, so the artifacts would have grown without bound. Capping
    # whole run directories is the other half of the same change.
    if _VISUAL_ARTIFACTS.exists():
        try:
            items: List[Path] = [
                p for p in _VISUAL_ARTIFACTS.iterdir() if p.is_file()
            ]
            items.sort(key=lambda x: x.stat().st_mtime, reverse=True)
            for p in items[_VISUAL_ARTIFACT_KEEP:]:
                try:
                    p.unlink(missing_ok=True)
                    deleted += 1
                except OSError:
                    pass
        except OSError:
            pass
        try:
            run_dirs: List[Path] = [
                p for p in _VISUAL_ARTIFACTS.iterdir() if p.is_dir()
            ]
            # Newest first, so the slice drops the OLDEST. A run's directory
            # mtime moves when it writes, so "newest" is "most recently
            # producing evidence" rather than "most recently started".
            run_dirs.sort(key=lambda x: x.stat().st_mtime, reverse=True)
            for p in run_dirs[_VISUAL_ARTIFACT_RUNS_KEEP:]:
                try:
                    shutil.rmtree(p, ignore_errors=True)
                    deleted += 1
                except OSError:
                    pass
        except OSError:
            pass

    # Best-effort sweep of /tmp/p*_pytest*.log style files.  Tolerate
    # Windows-Linux path differences — Windows /tmp may not exist.
    legacy_log_root = Path("/tmp")
    if legacy_log_root.exists():
        try:
            logs = [
                p for p in legacy_log_root.glob("p*_pytest*.log")
                if p.is_file()
            ]
            logs.sort(key=lambda x: x.stat().st_mtime, reverse=True)
            for p in logs[5:]:
                try:
                    p.unlink(missing_ok=True)
                    deleted += 1
                except OSError:
                    pass
        except OSError:
            pass

    reporter.write_line(
        f"[pdfe-janitor] cleaned {deleted} stale output file(s) "
        f"(run_id={_op.RUN_ID})"
    )
    return deleted


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session", autouse=True)
def _janitor(request):
    """Session-scoped autouse: sweep stale outputs at session start.

    Runs BEFORE _fixture_integrity_guard (declaration order).  Yields
    immediately; no teardown work — the integrity guard handles the end-
    of-session report.
    """
    # Ensure target directories exist so subsequent code can rglob without
    # AttributeError on missing roots.
    _TESTS_TMP.mkdir(parents=True, exist_ok=True)
    _VISUAL_ARTIFACTS.mkdir(parents=True, exist_ok=True)

    reporter = request.config.pluginmanager.get_plugin("terminalreporter")

    class _NullReporter:
        def write_line(self, msg: str) -> None:
            print(msg)

    if reporter is None:
        reporter = _NullReporter()
    _run_janitor(reporter)
    yield


@pytest.fixture(scope="session", autouse=True)
def _fixture_integrity_guard(request, _janitor):
    """Session-scoped autouse: snapshot fixtures at start, verify at end.

    Depends on ``_janitor`` so the janitor's cleanup happens BEFORE the
    snapshot — otherwise stale files from prior runs would falsely
    appear as "deleted by this session."
    """
    snap_before = _snapshot_fixtures()
    reporter = request.config.pluginmanager.get_plugin("terminalreporter")

    class _NullReporter:
        def write_line(self, msg: str) -> None:
            print(msg)

    if reporter is None:
        reporter = _NullReporter()

    reporter.write_line(
        f"[pdfe-integrity] snapshot {len(snap_before)} fixture files "
        f"(run_id={_op.RUN_ID})"
    )

    yield

    snap_after = _snapshot_fixtures()
    drift = _format_drift(snap_before, snap_after)
    if drift is not None:
        # Raising at session teardown shows up in pytest's exitstatus and
        # the message is printed verbatim.  Tests that already passed
        # will still be reported as passed, but the session will fail.
        raise FixtureCorruptionError(drift)
    reporter.write_line(
        f"[pdfe-integrity] {len(snap_after)} fixtures verified clean "
        f"(run_id={_op.RUN_ID})"
    )


@pytest.fixture(scope="session")
def run_id() -> str:
    """Convenience: the session's run id (8 hex chars)."""
    return _op.RUN_ID


@pytest.fixture(autouse=True)
def _disable_pyautogui_failsafe():
    """Force PyAutoGUI's fail-safe OFF for every test.

    PyAutoGUI's fail-safe (``FAILSAFE=True``, the library default) is a
    HUMAN-abort mechanism: ``failSafeCheck()`` runs at the START of every
    pyautogui call and raises ``FailSafeException`` if the cursor is
    currently in a screen corner, so a person can slam the mouse to (0,0)
    to stop a runaway script. In headless/RDP CI there is no human, and
    the session legitimately starts with the cursor parked at (0,0) — so
    the check fires before the call can even move the mouse, producing
    false failures unrelated to the test. Three canvas-click tests failed
    exactly this way on validation run 27461377313 (Phase 7 sweep):
      - test_canvas_click_subsystems::test_scan_style_toggle_persist
      - test_canvas_click_subsystems::test_font_match_panose_top_candidate
      - test_canvas_click_tre::test_txt_tre_edit_size_change
    The traceback confirmed the exception originated in ``failSafeCheck()``
    at the head of ``pyautogui.click()`` (current cursor at a corner), not
    from the computed target coordinate.

    Disabling the fail-safe is PyAutoGUI's documented configuration for
    automated use. This is a function-scoped autouse fixture so it takes
    effect AFTER any module-level ``pyautogui.FAILSAFE = True`` left over
    in older harness modules: the flag is a process-wide global, whichever
    assignment runs last wins, and a fixture body runs after all imports.
    """
    try:
        import pyautogui  # type: ignore
        pyautogui.FAILSAFE = False
    except Exception:  # noqa: BLE001 - pyautogui absent in non-UI envs
        pass
    yield


@pytest.fixture(autouse=True)
def _clean_autosave_sidecars(request):
    """Belt-and-suspenders: remove any autosave / crash-recovery sidecar at the
    END of every test.

    A UI test that dirtied a document is then force-killed by the harness
    (PDFESession relaunches PDFE per test), so PDFE's clean-close
    ``clearRecovery()`` never runs and its recovery sidecar (the "temp save")
    survives in ``%APPDATA%\\PDFE\\PDFE\\recovery``. Left there, it fires the
    modal "Recover unsaved changes" prompt on the NEXT test's launch and wedges
    UIA teardown. PDFE already discards these on startup under the UI-test seam;
    this fixture is the independent OUTSIDE layer, cleaning the same location
    after each test regardless of how that test launched PDFE.

    EXEMPTION: a test that actually checks autosave / recovery functionality
    marks itself ``@pytest.mark.autosave`` (registered in pytest.ini) — such a
    test legitimately wants its sidecar to persist across steps, so cleanup is
    skipped for it. (No Python test currently does; the marker is provided so
    one can, without regressing this safeguard.)

    Function-scoped autouse so it runs after EVERY test, including
    ``unittest.TestCase`` methods. Best-effort: cleanup errors never fail a test.
    """
    yield
    if request.node.get_closest_marker("autosave") is not None:
        return  # test owns its recovery sidecars — do not disturb them
    try:
        from tests.helpers.autosave_cleanup import discard_recovery_sidecars
        discard_recovery_sidecars()
    except Exception:  # noqa: BLE001 - cleanup is best-effort, never fatal
        pass


# ---------------------------------------------------------------------------
# Phase F11 / gap WIN-45 — durable non-pass record for reachability blocks
# ---------------------------------------------------------------------------
# The reachability sites converted by phase F11 call
# ``tests.ui_automation.conftest.record_matrix_result(..., "blocked", ...)``
# before failing.  That module's own ``pytest_sessionfinish`` only runs when
# the session collected tests from ``tests/ui_automation/`` — but the majority
# of converted sites live in the top-level ``tests/test_ui_*.py`` live-UI
# files, which this ROOT conftest owns.  Without the hook below, a blocked
# record made from those files would exist only in memory and the matrix would
# once again show nothing: exactly the false-green shape WIN-45 is about.
#
# This writer is deliberately independent of ``_write_matrix_updates`` (which
# overlays the source-controlled CSV and drops ids that have no committed row).
# It dumps EVERY recorded id verbatim, so a blocked reachability record from a
# file with no matrix row is still visible after the run.

# ---------------------------------------------------------------------------
# Gaps A1-64 / A1-66 / A1-83 — NOT-RUN reporting
# ---------------------------------------------------------------------------
# pytest renders a skipped test as a green session. A suite that skips 100% of
# its assertions therefore reads identically to one that ran and passed them.
# These two hooks make the absence loud: every skip stamped by
# ``tests.notrun.skip_not_run`` is collected, printed in a dedicated terminal
# section, and written to tests/_runtime/not_run_report.json so CI can tell
# "ran and asserted" from "skipped clean" mechanically.

# ---------------------------------------------------------------------------
# Mid-test desktop loss (GitHub #132 follow-up)
# ---------------------------------------------------------------------------
# The watchdog takes the runner OFFLINE when the interactive desktop goes away,
# so queued jobs WAIT instead of running blind. That covers jobs that have not
# started. A job ALREADY RUNNING when an RDP session drops is the worse case: it
# does not merely fail, it fails dishonestly. Every UIA lookup misses, the suite
# goes red, and the red is indistinguishable from a genuine product defect.
#
# These two hooks reclassify such a failure from `red` to `blocked` -- the same
# vocabulary the WIN-45 reachability conversions use -- using the outage window
# journalled by runner_watchdog.ps1.
#
# It does NOT make the test pass. The product could not be driven, so a pass
# would be a lie in the other direction. It stops the failure lying about WHY.
#
# Wrapped in a bare except on purpose: this is bookkeeping, and bookkeeping that
# can break a test run is worse than none. With no journal present -- every
# developer machine, every CI box -- these are no-ops.

_TEST_START_TIMES = {}


def pytest_runtest_setup(item):  # noqa: D103
    try:
        _TEST_START_TIMES[item.nodeid] = _dt.datetime.now().astimezone()
    except Exception:  # noqa: BLE001
        pass


def _reclassify_if_desktop_was_gone(report):
    """Called on a FAILED report. Returns the outage description, or None."""
    started = _TEST_START_TIMES.get(report.nodeid)
    if started is None:
        return None
    from tests.helpers import desktop_outage as _outage
    window = _outage.outage_overlapping(started, _dt.datetime.now().astimezone())
    if window is None:
        return None
    from tests.ui_automation.conftest import (
        reclassify_blocked_by_outage, matrix_id_for_nodeid)
    reason = _outage.describe(window)
    test_id = matrix_id_for_nodeid(report.nodeid)
    reclassify_blocked_by_outage(test_id, reason)
    return reason


def pytest_runtest_logreport(report):  # noqa: D103
    try:
        from tests import notrun as _notrun
        _notrun.record_report(report)
    except Exception:  # noqa: BLE001 — reporting never masks results
        pass
    try:
        if report.when == "call" and report.failed:
            reason = _reclassify_if_desktop_was_gone(report)
            if reason:
                _DESKTOP_OUTAGE_FAILURES.append((report.nodeid, reason))
    except Exception:  # noqa: BLE001 — bookkeeping never masks results
        pass


#: Populated by the hook above; printed in the terminal summary so an outage is
#: impossible to miss. A reclassification nobody sees would just be a quieter
#: false failure.
_DESKTOP_OUTAGE_FAILURES = []


def pytest_terminal_summary(terminalreporter):  # noqa: D103
    try:
        from tests import notrun as _notrun
        _notrun.terminal_summary(terminalreporter)
    except Exception:  # noqa: BLE001
        pass
    try:
        if _DESKTOP_OUTAGE_FAILURES:
            terminalreporter.write_sep("=", "FAILURES DURING A DESKTOP OUTAGE")
            terminalreporter.write_line(
                "The interactive desktop was gone while these ran, so they could "
                "not drive the product. Recorded as 'blocked', NOT as evidence of "
                "a defect. They are still failures -- do not read them as passes.")
            for nodeid, reason in _DESKTOP_OUTAGE_FAILURES:
                terminalreporter.write_line("  %s\n      %s" % (nodeid, reason))
            terminalreporter.write_line(
                "Cause is almost always an RDP disconnect mid-run. See "
                "tools/ops/runner_watchdog.ps1 and GitHub #132.")
    except Exception:  # noqa: BLE001
        pass


def pytest_sessionfinish(session, exitstatus):  # noqa: ARG001
    """Persist MATRIX_RESULTS (all statuses) to tests/_runtime/ as JSON."""
    try:
        from tests.ui_automation.conftest import MATRIX_RESULTS
        if not MATRIX_RESULTS:
            return
        import json
        runtime_dir = _REPO_ROOT / "tests" / "_runtime"
        runtime_dir.mkdir(parents=True, exist_ok=True)
        stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        dest = runtime_dir / f"matrix_results_{stamp}.json"
        payload = {
            "recorded_at": _dt.datetime.now().isoformat(timespec="seconds"),
            "blocked_count": sum(
                1 for v in MATRIX_RESULTS.values() if v.get("status") == "blocked"),
            "red_count": sum(
                1 for v in MATRIX_RESULTS.values() if v.get("status") == "red"),
            "results": MATRIX_RESULTS,
        }
        dest.write_text(json.dumps(payload, indent=2, ensure_ascii=False),
                        encoding="utf-8")
        print(f"[conftest] matrix results written to {dest}", file=sys.stderr)
    except Exception as e:  # noqa: BLE001 - bookkeeping never masks results
        print(f"[conftest] matrix result dump failed: {e}", file=sys.stderr)
