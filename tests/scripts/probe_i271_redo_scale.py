#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Does a large REDO batch kill the application, and at what size?

Measured 2026-10-09 in the full stress test: create 1000 (350 s), undo 1010 ->
1000 performed (118 s), then **pdfe.exe exited during the redo batch** leaving no
minidump, while the caller sat polling a result pipe for a reply that could never
arrive. The test would have failed with "the redo-repeat seam gave no reply",
which reads as a harness fault and is not one -- the application died.

Two things need separating, and the full test is a poor instrument for it because
reaching the redo costs ~8 minutes of create loop first:

  * is it SIZE? redo works at small N and fails somewhere above it.
  * is it REENTRANCY in the seam? runTestUndoRedoRepeat calls processEvents
    between operations, which lets another pipe write or a deferred trigger run
    inside the batch. A guard is now in place; if that was the cause, the crash
    is gone at every N.

So: create N, undo N, redo N, through the acknowledged seams, at whatever N is
given. Reports how many operations the application says it performed at each
step and whether it is still alive afterwards.

Run:  python tests/scripts/probe_i271_redo_scale.py [N]
"""

from __future__ import annotations

import ctypes
import io
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers import qinvoke  # noqa: E402
from tests.helpers.canvas_geometry import canvas_rect, page_rect_on_canvas  # noqa: E402
from tests.helpers.click_safety import safe_set_cursor_pos  # noqa: E402
from tests.helpers.keys import press_to_own_foreground  # noqa: E402
from tests.helpers.pdfe_session import PDFESession, list_pdfe_pids  # noqa: E402

N = int(sys.argv[1]) if len(sys.argv) > 1 else 200
FIX = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"
DEST = REPO_ROOT / "docs" / "audit" / "p95-phase10"
CAP = DEST / ("raw-redo-scale-%d.txt" % N)


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    cap = io.open(CAP, "w", encoding="utf-8")

    def say(line: str) -> None:
        print(line, flush=True)
        cap.write(line + "\n")
        cap.flush()

    say("Redo-scale probe, N=%d" % N)
    if not FIX.exists():
        say("FAIL: fixture missing: %s" % FIX)
        return 2

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_redo_"))
    src = tmp / "redo_work.pdf"
    src.write_bytes(FIX.read_bytes())

    with PDFESession(pdf_path=str(src)) as s:
        if not s.wait_for_window_title(src.name, timeout=20):
            say("FAIL: PDFE did not open the document")
            return 2
        time.sleep(1.0)
        pid = s.pid

        depth = qinvoke.set_undo_depth(s, N + 100)
        say("undo depth set: %r" % depth)
        if depth != N + 100:
            say("FAIL: could not set the undo depth")
            return 2

        # Points from the application's own page geometry. CLAUDE.md: never
        # guess canvas coordinates. Spread so no click lands on an existing
        # icon, which would select rather than create.
        cr = canvas_rect(s)
        pr = page_rect_on_canvas(s, 0)
        if cr is None or pr is None:
            say("FAIL: canvas/page geometry seam did not answer; refusing to "
                "guess coordinates")
            return 2
        pxo, pyo, pw, ph = pr
        gx0, gy0 = cr.left + pxo, cr.top + pyo
        pitch = 16
        cols = max(1, int(pw // pitch) - 1)
        pts = []
        for i in range(N):
            cx = int(gx0 + pitch / 2 + (i % cols) * pitch)
            cy = int(gy0 + pitch / 2 + (i // cols) * pitch)
            if cy > gy0 + ph - pitch:
                break
            pts.append((cx, cy))
        if len(pts) < N:
            say("FAIL: page 0 holds only %d non-overlapping points, %d needed. "
                "Re-run with a smaller N rather than letting clicks overlap."
                % (len(pts), N))
            return 2

        t0 = time.time()
        armed = 0
        for i in range(N):
            if not qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                                  timeout=5.0):
                say("FAIL: arming %d not acknowledged" % i)
                return 2
            armed += 1
            safe_set_cursor_pos(*pts[i])
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.005)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.05)
            try:
                press_to_own_foreground(s.pid, "escape", pause=0.02)
            except Exception:  # noqa: BLE001
                pass
        say("created: %d armings acknowledged in %.1f s" % (armed, time.time() - t0))
        say("alive after create: %s" % (pid in list_pdfe_pids()))

        t0 = time.time()
        undone = qinvoke.undo_repeat(s, N + 10, timeout=900.0)
        say("undo: performed %r in %.1f s" % (undone, time.time() - t0))
        say("alive after undo: %s" % (pid in list_pdfe_pids()))
        if undone is None:
            say("RESULT: the UNDO batch gave no reply. If the process is dead "
                "above, the application died -- not a harness fault.")
            return 1

        t0 = time.time()
        redone = qinvoke.undo_repeat(s, N + 10, redo=True, timeout=900.0)
        dt = time.time() - t0
        alive = pid in list_pdfe_pids()
        say("redo: performed %r in %.1f s" % (redone, dt))
        say("alive after redo: %s" % alive)

        say("")
        if redone is None and not alive:
            say("RESULT at N=%d: the application DIED during redo. This is the "
                "full-test failure reproduced at a fraction of the cost." % N)
            return 1
        if redone is None:
            say("RESULT at N=%d: no reply but the process is ALIVE -- a stall "
                "or a lost reply, not a crash." % N)
            return 1
        say("RESULT at N=%d: redo survived, %r operations in %.1f s (%.0f ms "
            "each)." % (N, redone, dt, 1000.0 * dt / max(1, redone)))
    cap.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
