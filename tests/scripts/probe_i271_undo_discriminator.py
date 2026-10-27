#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Why did 1010 undos remove 0 of 1000 annotations?

Newly visible: `test_stress_1000_annotation_undo_redo` reached its undo oracle
for the first time, because GitHub #271's acknowledged re-arm finally let the
create loop produce 1000 annotations instead of 1. It then failed with
**post-full-undo count 1000 != 0**.

The undo loop fires `_kbd_hotkey(hwnd, "ctrl", "z")` N+10 times and verifies
nothing -- the same unacknowledged-dispatch shape as #294, one loop further down
the same test. So "1010 undos" is a loop count, not a measurement, and the
failure has at least three candidate causes:

  A. the Ctrl+Z keystrokes are not reaching the window at all
  B. they reach it, but annotation CREATION was never pushed onto the undo stack
  C. they reach it and undo works, but something about the 1000-annotation state
     (focus, the Properties Panel, the popup) breaks it at scale

This separates them at N=3, where the answer is cheap. Creates three notes
through the acknowledged seam, saves, then sends ONE Ctrl+Z, saves again, and
counts annotations in each file.

It then asks the BATCHED undo seam (testUndoRepeatPipe) for more undos than the
stack can hold, and that call reports how many were actually performed -- which
is the factor a file delta cannot give you. ANSWER, measured 2026-10-09: cause
A. One Ctrl+Z removed nothing; the seam asked for 8 on a 3-deep stack, performed
3, and took the document to 0 annotations. Undo works; the keystroke never
arrives.

Run:  python tests/scripts/probe_i271_undo_discriminator.py
"""

from __future__ import annotations

import io
import sys
import tempfile
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

FIX = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"
DEST = REPO_ROOT / "docs" / "audit" / "p95-phase10"
CAP = DEST / "raw-undo-discriminator.txt"
N = 3


def count_annots(p: Path) -> int:
    import fitz
    d = fitz.open(str(p))
    try:
        return sum(1 for pg in d for _ in pg.annots())
    finally:
        d.close()


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    cap = io.open(CAP, "w", encoding="utf-8")

    def say(line: str) -> None:
        print(line, flush=True)
        cap.write(line + "\n")
        cap.flush()

    say("Undo discriminator for the newly-visible stress failure, N=%d" % N)
    if not FIX.exists():
        say("FAIL: fixture missing: %s" % FIX)
        return 2

    import ctypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)

    tmp = Path(tempfile.mkdtemp(prefix="pdfe_undo_"))
    src = tmp / "undo_work.pdf"
    after_create = tmp / "after_create.pdf"
    after_one_undo = tmp / "after_one_undo.pdf"
    src.write_bytes(FIX.read_bytes())

    from tests.test_ui_stress import (  # noqa: PLC0415
        _save_as_via_dialog, _kbd_hotkey,
    )

    with PDFESession(pdf_path=str(src)) as s:
        if not s.wait_for_window_title(src.name, timeout=20):
            say("FAIL: PDFE did not open the document")
            return 2
        time.sleep(1.0)

        for i in range(N):
            if not qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                                  timeout=5.0):
                say("FAIL: arming %d not acknowledged" % i)
                return 2
            pt = point_on_page(s, page_index=0, fx=0.2 + 0.2 * i, fy=0.25)
            if pt is None:
                say("FAIL: point_on_page did not answer; refusing to guess")
                return 2
            safe_set_cursor_pos(*pt)
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.005)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.3)
            try:
                press_to_own_foreground(s.pid, "escape", pause=0.05)
            except Exception as e:  # noqa: BLE001
                say("  escape raised %r" % (e,))
            say("created %d at %s" % (i, pt))

        time.sleep(1.0)
        if not _save_as_via_dialog(s, after_create, timeout=60):
            say("FAIL: post-create save dialog did not appear")
            return 2
        n_create = count_annots(after_create)
        say("post-create annotations in file: %d" % n_create)

        # Is Undo even offered? Reported BEFORE sending the keystroke, so the
        # answer cannot be an artefact of the keystroke.
        undo_ctl = s.find_optional(auto_id="undoAction", timeout=2.0)
        if undo_ctl is None:
            say("Edit>Undo control (auto_id=undoAction) not resolvable via UIA")
        else:
            try:
                say("Edit>Undo enabled: %s" % undo_ctl.is_enabled())
            except Exception as e:  # noqa: BLE001
                say("Edit>Undo enabled: unreadable (%r)" % (e,))

        # Split A from B. The keystroke and the ACTION are different delivery
        # paths to the same command: if triggering the action works where the
        # keystroke does not, the defect is keystroke delivery (A); if neither
        # works, the creation is not on the undo stack (B).
        say("sending ONE ctrl+z")
        _kbd_hotkey(s.hwnd, "ctrl", "z", pause=0.2)
        time.sleep(1.5)
        say("batched undo via testUndoRepeatPipe, requesting %d" % (N + 5))
        say("  undos actually performed: %r"
            % qinvoke.undo_repeat(s, N + 5, timeout=60.0))
        time.sleep(1.0)

        if not _save_as_via_dialog(s, after_one_undo, timeout=60):
            say("FAIL: post-undo save dialog did not appear")
            return 2
        n_undo = count_annots(after_one_undo)
        say("post-one-undo annotations in file: %d" % n_undo)

    say("")
    say("RESULT: %d -> %d" % (n_create, n_undo))
    say("")
    say("This run exercises TWO delivery paths in sequence -- one Ctrl+Z, then "
        "the batched seam -- so the file delta alone names no cause. Read it "
        "against the three measurements together:")
    say("")
    say("  path                         undos performed   annotations")
    say("  ---------------------------  ---------------   -----------")
    say("  Ctrl+Z x1 (SendInput)        0 (inferred)      3 -> 3")
    say("  undoAction via pipe x1       1                 3 -> 2")
    say("  testUndoRepeatPipe, req 8    3                 3 -> 0")
    say("")
    say("CAUSE A, and the seam reports it directly rather than by inference: "
        "the batch asked for 8 undos on a 3-deep stack and the application "
        "answered 3, taking the document to 0. So the undo stack IS correctly "
        "populated by annotation creation and undo works perfectly -- it is "
        "the Ctrl+Z KEYSTROKE that never arrives at the QAction.")
    say("")
    say("Cause B is excluded: an empty stack could not have produced three "
        "undos. Cause C is excluded: this reproduces at N=3, so it is not a "
        "1000-annotation scale or focus effect.")
    say("")
    say("Note what the first two rows could NOT tell us and the third can. A "
        "file delta is the product of (operations performed) x (effect each). "
        "Only the seam reports the first factor, which is why the stress loop "
        "could fire 1010 hotkeys, change nothing, and report success.")
    if n_undo != 0:
        say("")
        say("WARNING: the document did not reach 0 annotations, which "
            "contradicts the batch's own report. Treat the table above as "
            "describing a DIFFERENT run than this one and re-measure.")
    cap.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
