# -*- coding: utf-8 -*-
"""Live-app reproduction attempt for the transparent-signature insert crash.

CONTEXT
-------
A user reported that inserting a tiny transparent (RGBA) signature PNG into a
large PDF crashes the real Qt application (pdfe.exe). The headless GoogleTest
model path (ImageManager::insertImage et al.) has already been proven CLEAN for
the same inputs, so if the crash is real it lives in the LIVE app: the Qt event
loop, the background TileRenderer threads that re-rasterize the page after the
insert, and/or the modal Insert-Image options dialog's lifetime. This test
drives that exact path end-to-end and watches for an abnormal exit or a crash
minidump.

This is phase-1 of pipeline ``transparency-insert-fix``: either REPRODUCE the
crash (and leave evidence a later phase can root-cause) or honestly record
NON-reproduction. It does not fabricate a fix.

SENSITIVE INPUTS COME FROM THE ENVIRONMENT, NEVER FROM THIS FILE
---------------------------------------------------------------
The two real inputs are supplied as env vars and are deliberately NOT written
here (no account path, no C:\\Users\\... segment, ever):

    PDFE_SIG_PNG   -> the transparent RGBA signature PNG to insert
    PDFE_LARGE_PDF -> the large PDF to insert it into

If either is unset the test SKIPs cleanly (it does not error), so
``pytest --collect-only`` and a bare ``pytest`` run both succeed on any machine.

HOW THE INSERT IS DRIVEN (follows the established project rules)
---------------------------------------------------------------
* The native file picker is bypassed via the ``testFileDialogPathPipe`` seam
  (a Qt app-modal picker cannot be driven on the CI/RDP session). The image
  path is pushed through that pipe, exactly as test_ui_insert_image_reposition.
* The modal InsertImageDialog is a Qt ``exec()`` loop, which is INVISIBLE to
  pywinauto UIA (see tests/helpers/modal_dialog.py). It is pre-armed to accept
  its defaults through its own ``testDialogActionPipe`` seam rather than
  enumerated through UIA.
* Canvas placement geometry is ASKED FOR, never guessed:
  tests.helpers.canvas_geometry.point_on_page gives a real on-page screen point
  at whatever zoom/scroll/DPI (the project rule; a hand-guessed offset routinely
  lands in the grey margin). We use it to drive a real drag of the just-inserted
  (auto-selected) image, which exercises the canvas + tile-render threads where
  a live-only crash would surface.

CRASH DETECTION
---------------
The CrashHandler writes a ``*.dmp`` minidump into PDFE_CRASH_DUMP_DIR (the UI
harness points that at ``<repo>/ui-out/crashdumps``) for an access violation,
pure-virtual call, invalid-parameter fault, or std::terminate, and under
automation it exits FAST (no modal) rather than blocking. We therefore detect a
crash two independent ways: a new minidump file appearing, and/or the process
vanishing with an abnormal exit code. Both are reported.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import List, Optional, Set, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


# ---------------------------------------------------------------------------
# Capability / environment gating -> clean SKIP (never an error)
# ---------------------------------------------------------------------------

def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401
        return True
    except Exception:  # noqa: BLE001
        return False


def _pdfe_exe() -> Path:
    config = os.environ.get("PDFE_BUILD_CONFIG", "Debug")
    return REPO_ROOT / "build" / "vs2022" / "bin" / config / "pdfe.exe"


def _has_pdfe_binary() -> bool:
    return _pdfe_exe().exists()


def _is_interactive_session() -> bool:
    # A pywinauto-driven GUI needs a real interactive desktop. SESSIONNAME is
    # set for both console and RDP sessions; a headless service session has it
    # absent. PDFE_UI_FORCE_HEADLESS forces the skip for deliberate headless CI.
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


def _sig_png() -> Optional[str]:
    return os.environ.get("PDFE_SIG_PNG")


def _large_pdf() -> Optional[str]:
    return os.environ.get("PDFE_LARGE_PDF")


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pywinauto() and _has_pdfe_binary() and _is_interactive_session()),
    reason="Live pdfe.exe + interactive Windows desktop (pywinauto) required.",
)


def _crash_dump_dir() -> Path:
    """The directory CrashHandler writes minidumps to under the UI harness.

    The harness sets PDFE_CRASH_DUMP_DIR to <repo>/ui-out/crashdumps via
    env.setdefault; honour an explicit override if the operator set one.
    """
    override = os.environ.get("PDFE_CRASH_DUMP_DIR")
    if override:
        return Path(override)
    return REPO_ROOT / "ui-out" / "crashdumps"


def _list_dumps(d: Path) -> Set[str]:
    try:
        return {p.name for p in d.glob("*.dmp")}
    except Exception:  # noqa: BLE001
        return set()


def _run_once(run_index: int) -> Tuple[str, List[str]]:
    """Drive one full insert attempt.

    Returns (outcome, new_dump_names) where outcome is one of:
        "alive"        -- process survived the whole flow (no crash)
        "crashed:<..>" -- process died abnormally and/or a new minidump appeared
    Raises on an environment/harness problem (not a product crash).
    """
    from tests.helpers.pdfe_session import PDFESession, list_pdfe_pids  # noqa: PLC0415
    from tests.helpers.qinvoke import activate_tool_by_name, _write_to_pipe  # noqa: PLC0415
    from tests.helpers.canvas_geometry import (  # noqa: PLC0415
        point_on_page,
        page_rect_on_canvas,
    )

    sig_png = _sig_png()
    large_pdf = _large_pdf()
    assert sig_png and large_pdf  # guaranteed by the caller's skip guard

    dump_dir = _crash_dump_dir()
    try:
        dump_dir.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass
    dumps_before = _list_dumps(dump_dir)

    new_dumps: List[str] = []
    outcome = "alive"

    # Open the (large) document. The 266 MB file can take a while to parse; the
    # window appears first, then the page loads asynchronously, so we poll the
    # page-geometry seam below to know the document is really ready.
    with PDFESession(pdf_path=large_pdf) as session:
        proc = session.proc

        # Wait until the document is actually loaded: the page-rect seam answers
        # only once page 0 is laid out on the canvas. Give the big file real
        # time. If it never answers we FAIL loudly rather than guess (project
        # rule) -- but distinguish "still loading" from "seam missing" by also
        # checking the process is alive.
        page_ready = False
        deadline = time.time() + 90.0
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            if page_rect_on_canvas(session, 0) is not None:
                page_ready = True
                break
            time.sleep(1.0)

        if proc.poll() is not None:
            # Died just opening the document -- that is itself a crash signal.
            time.sleep(1.0)
            new_dumps = sorted(_list_dumps(dump_dir) - dumps_before)
            return (f"crashed:on-open exit={proc.poll()}", new_dumps)

        assert page_ready, (
            "page_rect_on_canvas seam never answered for the large PDF within "
            "90s; cannot confirm the document loaded (refusing to guess canvas "
            "coordinates)."
        )

        # 1) Bypass the native file picker: push the signature image path.
        assert _write_to_pipe(
            session, "testFileDialogPathPipe", sig_png, settle=0.4
        ), "could not reach testFileDialogPathPipe seam"

        # 2) Pre-arm the modal InsertImageDialog to accept its defaults. A Qt
        #    app-modal exec() is invisible to UIA; drive it via its own seam.
        assert _write_to_pipe(
            session, "testDialogActionPipe", "insertImageDialog|accept",
            settle=0.4,
        ), "could not reach testDialogActionPipe seam"

        # 3) Trigger the real insert. This is the exact operation the user
        #    reported crashing: decode the RGBA PNG, build the fz_pixmap/SMask,
        #    commit it into the large document, then re-render the page (which
        #    spins up background TileRenderer threads).
        assert activate_tool_by_name(session, "insertImageAction", settle=0.6), (
            "insertImageAction could not be triggered"
        )

        # Give the insert + modal + re-render pipeline generous time to run to
        # completion, polling for an abnormal exit the whole while.
        insert_deadline = time.time() + 20.0
        while time.time() < insert_deadline:
            if proc.poll() is not None:
                break
            time.sleep(0.5)

        if proc.poll() is None:
            # 4) Placement / manipulation on the canvas, ASKING where the page
            #    is (never guessing). Drive a real drag near page centre to
            #    exercise the canvas interaction + tile threads against the
            #    freshly-inserted transparent image.
            pt = point_on_page(session, 0, 0.5, 0.5)
            if pt is not None:
                try:
                    from tests.helpers.win32_input import send_drag  # noqa: PLC0415
                    x, y = pt
                    send_drag(x, y, x + 25, y + 25, duration=0.3, steps=8)
                except Exception:  # noqa: BLE001
                    # A drag-guard refusal (point off PDFE) is not a product
                    # crash; ignore it and keep watching liveness.
                    pass
                time.sleep(1.5)

            # Nudge the tile-render threads once more with a view query.
            try:
                from tests.helpers.canvas_geometry import view_state  # noqa: PLC0415
                view_state(session)
            except Exception:  # noqa: BLE001
                pass
            time.sleep(1.5)

        # Settle, then take the crash verdict.
        time.sleep(1.0)
        new_dumps = sorted(_list_dumps(dump_dir) - dumps_before)
        exit_code = proc.poll()
        alive = exit_code is None and bool(list_pdfe_pids())

        if new_dumps:
            outcome = f"crashed:minidump exit={exit_code} dumps={new_dumps}"
        elif exit_code is not None:
            outcome = f"crashed:exit={exit_code}"
        elif not alive:
            outcome = "crashed:process-gone"
        else:
            outcome = "alive"

    return (outcome, new_dumps)


@REQUIRES_LIVE_UI
def test_insert_transparent_signature_into_large_pdf_does_not_crash():
    """Insert a transparent RGBA signature into a large PDF via the live app.

    SKIPs cleanly unless BOTH PDFE_SIG_PNG and PDFE_LARGE_PDF are set. When they
    are, drives the full Insert-Image flow and asserts the app neither exits
    abnormally nor writes a crash minidump. A failure here IS the reproduction
    the phase is looking for; the assertion message carries the evidence.
    """
    if not _sig_png() or not _large_pdf():
        pytest.skip(
            "PDFE_SIG_PNG and PDFE_LARGE_PDF must both be set to the real "
            "transparent-signature PNG and large PDF; skipping (no sensitive "
            "paths are embedded in this test)."
        )

    outcome, new_dumps = _run_once(run_index=0)

    assert not new_dumps, (
        "REPRODUCED: inserting the transparent signature wrote a crash "
        f"minidump: {new_dumps} (in {_crash_dump_dir()}). Analyse the .dmp to "
        "name the faulting site for phase-3."
    )
    assert outcome == "alive", (
        f"REPRODUCED: the live app did not survive the insert flow: {outcome}. "
        "This is the user-reported crash; feed the minidump/exit code to "
        "phase-3 root-causing."
    )


if __name__ == "__main__":
    # Manual driver: run the flow N times and print a per-run outcome table, so
    # the phase-1 evidence (run count + alive/crash per run) can be captured
    # without pytest. Honours PDFE_REPRO_RUNS (default 3).
    if not _sig_png() or not _large_pdf():
        print("SKIP: PDFE_SIG_PNG and PDFE_LARGE_PDF must both be set.")
        raise SystemExit(0)
    runs = int(os.environ.get("PDFE_REPRO_RUNS", "3"))
    print(f"crash-dump dir: {_crash_dump_dir()}")
    results = []
    for i in range(runs):
        print(f"--- run {i + 1}/{runs} ---", flush=True)
        try:
            outcome, dumps = _run_once(run_index=i)
        except Exception as exc:  # noqa: BLE001
            outcome, dumps = f"error:{exc}", []
        print(f"run {i + 1}: {outcome}", flush=True)
        results.append((i + 1, outcome, dumps))
    print("=== SUMMARY ===")
    for idx, outcome, dumps in results:
        print(f"run {idx}: {outcome}")
    crashed = [r for r in results if r[1].startswith("crashed")]
    print(f"crashed runs: {len(crashed)}/{runs}")
    raise SystemExit(1 if crashed else 0)
