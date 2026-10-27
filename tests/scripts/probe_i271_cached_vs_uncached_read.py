#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #271 — the cost of resolving the result pipe UNCACHED vs CACHED.

Why this exists: the phase-10 record cited "4237 ms per arming" as the cost of
the first working implementation, which read the result pipe through
``session.find_optional`` (``_find_pipe``'s Strategy C -- a full descendants
scan) instead of through ``_find_pipe``'s ``(pid, auto_id)`` cache. The
``claim-check`` gate correctly objected (F4) that the number appeared in a live
test's failure message as a baseline a future engineer is told to act on, with
no committed capture behind it. Its original capture had been overwritten by
later probe runs.

So this measures both paths in one run, back to back on the same document, and
commits the result. A controlled comparison is better evidence than the
original single figure anyway.

Both arms call the SAME ``activate_tool_and_wait``; only the resolution strategy
behind ``read_tool_activate_ack`` differs, so the delta is attributable.

Run:  python tests/scripts/probe_i271_cached_vs_uncached_read.py [N]
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

N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
FIX = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"
DEST = REPO_ROOT / "docs" / "audit" / "p95-phase10"
CAP = DEST / "raw-cached-vs-uncached-read.txt"

_ORIGINAL_READ = qinvoke.read_tool_activate_ack


def _uncached_read(session):
    """The ORIGINAL implementation: Strategy C, a full descendants scan."""
    try:
        wid = session.find_optional(auto_id="testToolActivateResultPipe",
                                    timeout=1.5)
        return None if wid is None else wid.get_value()
    except Exception:  # noqa: BLE001
        return None


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    cap = io.open(CAP, "w", encoding="utf-8")

    def say(line: str) -> None:
        print(line, flush=True)
        cap.write(line + "\n")
        cap.flush()

    say("GitHub #271: result-pipe resolution, UNCACHED vs CACHED. N=%d" % N)
    if not FIX.exists():
        say("FAIL: fixture missing: %s" % FIX)
        return 2

    results = {}
    with PDFESession(pdf_path=str(FIX)) as s:
        if not s.wait_for_window_title(FIX.name, timeout=20):
            say("FAIL: PDFE did not open the document")
            return 2
        time.sleep(1.0)

        if _ORIGINAL_READ(s) is None:
            say("FAIL: testToolActivateResultPipe absent; nothing to measure")
            return 2

        for label, reader in (("uncached (find_optional)", _uncached_read),
                              ("cached (_find_pipe)", _ORIGINAL_READ)):
            qinvoke.read_tool_activate_ack = reader
            qinvoke.reset_ack_seq_cache()
            # Warm up: the first resolution of any widget pays a one-off cost
            # that no later iteration pays, and reporting it would overstate
            # the typical figure.
            for _ in range(2):
                qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                               timeout=30.0)
            times, fails = [], 0
            for _ in range(N):
                t0 = time.time()
                ok = qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                                    timeout=30.0)
                times.append(1000.0 * (time.time() - t0))
                if not ok:
                    fails += 1
            results[label] = (times, fails)
            say("%-26s mean %8.1f ms  median %8.1f  min %8.1f  max %8.1f  "
                "failures %d"
                % (label, statistics.mean(times), statistics.median(times),
                   min(times), max(times), fails))

    qinvoke.read_tool_activate_ack = _ORIGINAL_READ

    un = statistics.mean(results["uncached (find_optional)"][0])
    ca = statistics.mean(results["cached (_find_pipe)"][0])
    say("")
    say("ratio uncached/cached: %.1fx" % (un / ca if ca else float("nan")))
    say("")
    say("Measured on an EMPTY document, which is why these are smaller than")
    say("the 4237 ms originally recorded: that figure came from the stress")
    say("loop with 1000 annotations in the accessible tree, where every UIA")
    say("round trip is dearer. The ratio is the transferable part -- the")
    say("absolute cost scales with tree size, which is the whole lesson of")
    say("cost_probe_measured_the_wrong_code_path.")
    cap.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
