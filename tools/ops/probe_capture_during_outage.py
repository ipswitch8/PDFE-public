#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Sample what a SCREEN CAPTURE actually returns across an RDP disconnect.

WHY
---
The desktop-outage work (#132/#133) established that `OpenInputDesktop` fails on
disconnect while the session survives, and that the SendInput canvas tests pass
straight through one. From that I concluded the false-failure risk was narrow.

That conclusion is contradicted by field experience: disconnects HAVE produced
bogus results here. So the mechanism is something the SendInput family does not
touch, and the obvious candidate is the screenshot oracles -- `capture_region`
in tests/ui_harness/core/screenshot.py is a `pyautogui.screenshot()`, i.e. a GDI
grab of the screen, and roughly a dozen live-UI files compare those captures
against committed baselines or against each other.

What a GDI grab returns from a session with no attached display is the question.
Black, a frozen last frame, an exception, or a correct image are all plausible,
and they have COMPLETELY different consequences:

  * black/frozen vs a committed baseline  -> SSIM collapses  -> bogus RED
  * black vs black in a "did it change?"  -> no diff detected -> bogus RED
  * black vs black in a "is it the same?" -> identical        -> bogus GREEN
  * exception                             -> error, honest

The last one is fine and the first two are already handled. A bogus GREEN is
not: a pass obtained while nothing could be observed is the worst outcome
available, and the reclassifier deliberately does not touch greens.

So: measure, do not reason. This is read-only -- it captures the screen and
records statistics, and writes nothing but its own log.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import io
import os
import sys
import time


def describe(img) -> str:
    """Compact, comparable description of one capture."""
    gray = img.convert("L")
    extrema = gray.getextrema()          # (min, max)
    colors = img.getcolors(maxcolors=256)  # None when > 256 distinct colours
    n_colors = "many" if colors is None else str(len(colors))
    hist = gray.histogram()
    total = sum(hist) or 1
    black_frac = hist[0] / total
    mean = sum(i * c for i, c in enumerate(hist)) / total
    uniform = extrema[0] == extrema[1]
    return ("size=%dx%d mean=%6.2f min=%3d max=%3d colors=%-5s black_frac=%.4f%s"
            % (img.width, img.height, mean, extrema[0], extrema[1], n_colors,
               black_frac, "  UNIFORM(degenerate)" if uniform else ""))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=os.path.join(
        os.environ.get("TEMP", "."), "pdfe-capture-probe.log"))
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()

    import pyautogui

    def emit(text):
        line = "%s  %s" % (_dt.datetime.now().strftime("%H:%M:%S.%f")[:-3], text)
        with io.open(args.log, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
        print(line, flush=True)

    emit("START pid=%d duration=%ds interval=%.1fs" % (os.getpid(), args.seconds, args.interval))
    deadline = time.time() + args.seconds
    prev = None
    while time.time() < deadline:
        try:
            img = pyautogui.screenshot()
            now = describe(img)
        except Exception as exc:              # noqa: BLE001 - the outcome IS the datum
            now = "EXCEPTION %s: %s" % (type(exc).__name__, exc)
        # Log every sample but mark transitions; a state change is the finding.
        emit(("CHANGE  " if now != prev else "        ") + now)
        prev = now
        time.sleep(args.interval)
    emit("END")
    return 0


if __name__ == "__main__":
    sys.exit(main())
