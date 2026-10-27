#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #271 — measure the arming-acknowledgement handshake's per-call cost.

#271 rules out the per-iteration routes it found, because they cost 55-314 ms
each and the loop that needs them runs 1000 times. A replacement asserted to be
"cheap" without a number is the same claim with no evidence behind it, so this
measures it.

Reports, for N arming round trips:
  * the acknowledged seam: write + poll until the sequence number changes
  * the bare write alone, for the difference the acknowledgement actually costs

Run:  python tests/scripts/probe_i271_ack_cost.py [N]
"""

from __future__ import annotations

import io
import statistics
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers import qinvoke  # noqa: E402
from tests.helpers.pdfe_session import PDFESession  # noqa: E402

N = int(sys.argv[1]) if len(sys.argv) > 1 else 30
FIX = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"


def main() -> int:
    if not FIX.exists():
        print("FAIL: fixture missing: %s" % FIX)
        return 2

    out = io.StringIO()

    def say(line: str) -> None:
        print(line, flush=True)
        out.write(line + "\n")

    say("GitHub #271 arming-acknowledgement cost, N=%d" % N)
    say("fixture: %s" % FIX.name)

    acked_ms, write_ms = [], []
    failures = 0

    with PDFESession(pdf_path=str(FIX)) as s:
        if not s.wait_for_window_title(FIX.name, timeout=20):
            say("FAIL: PDFE did not open the document")
            return 2
        time.sleep(1.0)

        if qinvoke.read_tool_activate_ack(s) is None:
            say("FAIL: testToolActivateResultPipe absent -- build has no test "
                "pipes, so there is nothing to measure")
            return 2

        # Warm up. The first UIA resolution of each pipe widget pays a
        # descendants scan that no later iteration pays, and reporting that as
        # the typical cost would overstate it by an order of magnitude.
        for _ in range(3):
            qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                           timeout=5.0)

        for i in range(N):
            t0 = time.time()
            ok = qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                                timeout=5.0)
            acked_ms.append(1000.0 * (time.time() - t0))
            if not ok:
                failures += 1

        # The unacknowledged write, for the delta. Measured SECOND so the
        # acknowledged figure above cannot be flattered by a warmer cache than
        # the thing it is compared against.
        for i in range(N):
            t0 = time.time()
            qinvoke.activate_tool_by_name(s, "stickyNoteToolAction")
            write_ms.append(1000.0 * (time.time() - t0))

    def stats(label: str, xs: list) -> None:
        say("%-26s n=%d  mean %6.1f ms  median %6.1f  min %6.1f  max %6.1f"
            % (label, len(xs), statistics.mean(xs), statistics.median(xs),
               min(xs), max(xs)))

    say("")
    stats("acknowledged (write+poll)", acked_ms)
    stats("bare write only", write_ms)
    delta = statistics.mean(acked_ms) - statistics.mean(write_ms)
    say("acknowledgement overhead:  %+.1f ms per call (mean)" % delta)
    say("arming failures:           %d of %d" % (failures, N))
    say("")
    say("At N=1000 the acknowledged route costs %.1f s of handshake in total."
        % (N and statistics.mean(acked_ms) * 1000.0 / 1000.0))
    say("#271's rejected routes cost 55-314 ms each, i.e. 55-314 s at N=1000.")

    dest = REPO_ROOT / "docs" / "audit" / "p95-phase10"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "raw-ack-cost.txt").write_text(out.getvalue(), encoding="utf-8")
    say("written: docs/audit/p95-phase10/raw-ack-cost.txt")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
