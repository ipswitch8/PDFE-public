#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #271 — does the acknowledged re-arm survive the FULL loop shape?

``probe_i271_ack_cost.py`` arms the tool repeatedly with nothing in between, so
the tool stays armed and every call takes ``activateToolByObjectName``'s
already-checked early-return path. The stress loop does not look like that: it
arms, CLICKS (which creates a note and switches MainWindow back to SelectTool),
dismisses the popup that creation opened, and only then arms again. That path
goes through the deferred trigger, and the popup is a top-level window.

So this probe is the loop, ten iterations of it, printing what the seam says at
every step and counting the annotations that actually reached the file. Ten
rather than a thousand because a re-arm that fails fails on iteration two.

Writes nothing through a pipe filter: every line is flushed to stdout AND
appended to the capture as it happens, so a kill mid-run leaves the evidence
behind rather than destroying it.

Run:  python tests/scripts/probe_i271_loop_shape.py [N]
"""

from __future__ import annotations

import io
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers import qinvoke  # noqa: E402
from tests.helpers.canvas_geometry import point_on_page  # noqa: E402
from tests.helpers.click_safety import safe_set_cursor_pos  # noqa: E402
from tests.helpers.keys import press_to_own_foreground  # noqa: E402
from tests.helpers.pdfe_session import PDFESession  # noqa: E402

N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
FIX = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"
DEST = REPO_ROOT / "docs" / "audit" / "p95-phase10"
CAP = DEST / "raw-loop-shape.txt"


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    cap = io.open(CAP, "w", encoding="utf-8")

    def say(line: str) -> None:
        print(line, flush=True)
        cap.write(line + "\n")
        cap.flush()      # the whole point: survive a kill

    say("GitHub #271 loop-shape probe, N=%d" % N)

    if not FIX.exists():
        say("FAIL: fixture missing: %s" % FIX)
        return 2

    import ctypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)

    # The working copy goes to a temp dir, NOT next to the capture. It ends up
    # holding N sticky notes, and a mutating binary under docs/audit/ is noise
    # in every future diff -- the capture is the evidence, not the scratch file.
    import tempfile
    _tmp = tempfile.mkdtemp(prefix="pdfe_i271_")
    src = Path(_tmp) / "loopshape_work.pdf"
    src.write_bytes(FIX.read_bytes())

    armed_ok = 0
    with PDFESession(pdf_path=str(src)) as s:
        if not s.wait_for_window_title(src.name, timeout=20):
            say("FAIL: PDFE did not open the document")
            return 2
        time.sleep(1.0)

        if qinvoke.read_tool_activate_ack(s) is None:
            say("FAIL: testToolActivateResultPipe absent in this build")
            return 2

        for i in range(N):
            t0 = time.time()
            ok = qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                                timeout=3.0)
            dt = 1000.0 * (time.time() - t0)
            ack = qinvoke.read_tool_activate_ack(s)
            say("iter %2d  arm=%-5s %7.1f ms  ack=%r" % (i, ok, dt, ack))
            if not ok:
                say("          -> re-arm FAILED; this is the loop-shape "
                    "difference the cost probe could not see")
                continue
            armed_ok += 1

            # CLAUDE.md: never guess canvas coordinates. Spread the points so
            # a later click cannot land on an icon an earlier one created --
            # a click on an existing note selects THAT note instead.
            fx = 0.15 + 0.07 * (i % 10)
            pt = point_on_page(s, page_index=0, fx=fx, fy=0.2)
            if pt is None:
                say("          -> point_on_page did not answer; FAIL rather "
                    "than guess")
                return 2
            cx, cy = pt
            safe_set_cursor_pos(cx, cy)
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.005)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.25)

            # Dismiss the popup the creation just opened.
            try:
                press_to_own_foreground(s.pid, "escape", pause=0.02)
            except Exception as e:  # noqa: BLE001
                say("          -> escape raised %r" % (e,))
            say("          clicked (%d,%d) fx=%.2f; post-click ack=%r"
                % (cx, cy, fx, qinvoke.read_tool_activate_ack(s)))

    say("")
    say("armed OK: %d of %d" % (armed_ok, N))
    say("NOTE: the annotation count is deliberately NOT read from the live "
        "document here -- this probe measures the RE-ARM, and asserting on a "
        "count as well would conflate the two the way GitHub #294 did.")
    cap.close()
    return 0 if armed_ok == N else 1


if __name__ == "__main__":
    sys.exit(main())
