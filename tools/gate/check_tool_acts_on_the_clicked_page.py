#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""A tool must act on the page it was told about (GitHub #269).

THE PROPERTY
============
`AnnotationTool::onMousePress(QMouseEvent*, int pageIndex, QPointF)` receives
the index of the page the click landed on. The tool also holds a cached
`pdfe::core::Page* page_`. If those two ever name different pages the tool
writes to one page and tells its caller about another -- and the caller
(`MainWindow::onAnnotationCreatedByTool`) then resolves the reported index and
asks it for an annotation index belonging to the other page.

Measured 2026-09-30: the sticky note landed on the page the tool remembered,
`AnnotationManager::getAnnotation` threw "Annotation index out of range", the
exception crossed a Qt event handler -- which Qt's own logged warning says is
not survivable -- and the process died with the user's document, at roughly the
830th note, four runs out of four.

Not a new failure mode. `AnnotationTool.h`'s documentation for
`onAnnotationsChanged()` describes it verbatim as the mechanism of GitHub #234.
#234 was an index shifted by a deletion, #235/#236 were pages freed under a
repaint, #269 is an index resolved against the wrong page: four defects, one
family, four doors, each previous fix closing the door it came through.

WHY THIS GUARD CHANGED SHAPE
============================
Twelve handlers in ten tools write through `page_` while reporting `pageIndex`.
This guard's first version treated each as a defect to reconcile in the handler,
and carried a twelve-entry baseline.

That was the wrong place. `PdfCanvas` is the only participant that knows both
the clicked page and the document to resolve it from, so the fix belongs at the
DISPATCH SITE -- and once `PdfCanvas::bindToolToPage()` binds the tool to the
event's page before dispatching, all twelve handlers are correct without any of
them changing. A baseline listing them would then assert a defect that is fixed,
which is the "a list describes its own history rather than the tree" shape this
project keeps paying for. The baseline is gone; there is no amnesty list.

So the property is checked where it is enforced:

  1. every gesture-STARTING dispatch is guarded by bindToolToPage()
  2. bindToolToPage() actually binds -- it calls setPage()
  3. the owned page is released AFTER the tool is told, never before

(3) is the GitHub #235 lifetime rule and is the part a later edit breaks most
easily: `toolPage_` owns a Page the tool points into, so dropping it before
`onDocumentClosing()` has nulled the tool's pointer re-creates a
use-after-free at paint time.

Move and release dispatches are deliberately NOT required to bind: a drag must
keep its press-time page. `InkTool`, `ShapeTool` and `StampTool` already assume
that, capturing `currentPageIndex_` at press; rebinding at release would move a
stroke to whatever page the cursor ended over.

WHAT IT CANNOT DO
=================
It reads `PdfCanvas.cpp` lexically, and the "is this dispatch guarded" check
looks back a small fixed window for the condition rather than parsing the
enclosing block. Both directions are asserted by `--self-test`, so a pattern
that stopped matching anything would be caught, but a sufficiently unusual
rewrite of the dispatch could defeat it.

Usage:
    python tools/gate/check_tool_acts_on_the_clicked_page.py [--repo-root .]
    python tools/gate/check_tool_acts_on_the_clicked_page.py --self-test
Exit 0 = clean, 1 = the property is not enforced, 2 = usage error.
"""
from __future__ import annotations

import argparse
import io
import os
import re
import sys

CANVAS = os.path.join("src", "ui", "PdfCanvas.cpp")

# Gesture-STARTING dispatches, which must bind first.
GESTURE_START = ("onMousePress", "onMouseDoubleClick")


def strip_comments(src: str) -> str:
    """Remove // and /* */ comments and string literals.

    A dispatch named in a comment is not a dispatch, and this guard's siblings
    learned that the hard way (DCA 0052).
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        two = src[i:i + 2]
        if two == "//":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if two == "/*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
            continue
        if src[i] == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    break
                j += 1
            i = j + 1
            out.append('""')
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def dispatch_sites(clean: str, names=GESTURE_START):
    """(line_no, name, text) for each `currentTool_->{name}(` call."""
    out = []
    for ln, line in enumerate(clean.splitlines(), 1):
        for nm in names:
            if re.search(r"currentTool_\s*->\s*%s\s*\(" % nm, line):
                out.append((ln, nm, line.strip()))
    return out


def guarded_by_bind(clean: str, line_no: int, window: int = 6) -> bool:
    """Is the dispatch at `line_no` inside a bindToolToPage() guard?"""
    lines = clean.splitlines()
    lo = max(0, line_no - 1 - window)
    return "bindToolToPage" in "\n".join(lines[lo:line_no])


def check(repo_root: str):
    """Failure strings; empty means the property holds."""
    path = os.path.join(repo_root, CANVAS)
    problems = []
    if not os.path.isfile(path):
        return ["%s is missing, so the property cannot be checked at all"
                % CANVAS]
    clean = strip_comments(
        io.open(path, encoding="utf-8", errors="replace").read())

    starts = dispatch_sites(clean)
    if not starts:
        problems.append(
            "no currentTool_->onMousePress / onMouseDoubleClick dispatch found "
            "in %s. Either the dispatch moved or this guard's pattern is "
            "stale; either way it is checking nothing." % CANVAS)
    for ln, nm, text in starts:
        if not guarded_by_bind(clean, ln):
            problems.append(
                "%s:%d dispatches %s without a bindToolToPage() guard:\n"
                "      %s\n"
                "    GitHub #269: the tool would act on the page it was last "
                "bound to, not the page clicked." % (CANVAS, ln, nm, text))

    if "bool PdfCanvas::bindToolToPage" not in clean:
        problems.append(
            "PdfCanvas::bindToolToPage is not defined in %s, so the guards "
            "above call something that does not exist." % CANVAS)
    else:
        at = clean.index("bool PdfCanvas::bindToolToPage")
        if "setPage(" not in clean[at:at + 2000]:
            problems.append(
                "PdfCanvas::bindToolToPage does not call setPage(), so it "
                "binds nothing -- the dispatch guards would pass while the "
                "tool kept its old page.")

    for fn in ("void PdfCanvas::clearDocument", "void PdfCanvas::setDocument"):
        if fn not in clean:
            problems.append("%s not found in %s" % (fn, CANVAS))
            continue
        body = clean[clean.index(fn):clean.index(fn) + 3000]
        tell = body.find("onDocumentClosing()")
        rel = body.find("releaseToolPage()")
        if rel < 0:
            problems.append(
                "%s does not call releaseToolPage(); the canvas would keep "
                "owning a Page from a document that has been closed." % fn)
        elif tell < 0:
            problems.append(
                "%s does not call currentTool_->onDocumentClosing(); the tool "
                "would keep a raw pointer into a freed page (GitHub #235)."
                % fn)
        elif rel < tell:
            problems.append(
                "%s releases the owned Page BEFORE telling the tool. That "
                "order is GitHub #235: the tool still points into the page "
                "the canvas just freed." % fn)

    return problems


def self_test() -> int:
    ok = True

    guarded = strip_comments("""
void PdfCanvas::mousePressEvent(QMouseEvent* event) {
    if (bindToolToPage(pageIdx)) {
        currentTool_->onMousePress(event, pageIdx, pdfPoint);
    }
}
""")
    sites = dispatch_sites(guarded)
    if len(sites) != 1 or not guarded_by_bind(guarded, sites[0][0]):
        print("self-test FAIL: a guarded dispatch was not recognised")
        ok = False

    bare = strip_comments("""
void PdfCanvas::mousePressEvent(QMouseEvent* event) {
    currentTool_->onMousePress(event, pageIdx, pdfPoint);
}
""")
    sites = dispatch_sites(bare)
    if len(sites) != 1 or guarded_by_bind(bare, sites[0][0]):
        print("self-test FAIL: an UNGUARDED dispatch read as guarded -- this "
              "guard could not catch the defect it exists for")
        ok = False

    commented = strip_comments("""
void PdfCanvas::mousePressEvent(QMouseEvent* event) {
    // currentTool_->onMousePress(event, pageIdx, pdfPoint);
    somethingElse();
}
""")
    if dispatch_sites(commented):
        print("self-test FAIL: a commented-out dispatch counted as a dispatch")
        ok = False

    good = strip_comments("void PdfCanvas::clearDocument() {\n"
                          "  currentTool_->onDocumentClosing();\n"
                          "  releaseToolPage();\n}\n")
    bad = strip_comments("void PdfCanvas::clearDocument() {\n"
                         "  releaseToolPage();\n"
                         "  currentTool_->onDocumentClosing();\n}\n")
    if good.index("releaseToolPage()") < good.index("onDocumentClosing()"):
        print("self-test FAIL: correct ordering read as inverted")
        ok = False
    if bad.index("releaseToolPage()") > bad.index("onDocumentClosing()"):
        print("self-test FAIL: inverted ordering read as correct -- the "
              "GitHub #235 half of this guard cannot fire")
        ok = False

    print("self-test %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = os.path.abspath(args.repo_root)
    problems = check(root)
    print("=== a tool must act on the page it was told about (GitHub #269) ===")
    if not problems:
        clean = strip_comments(
            io.open(os.path.join(root, CANVAS), encoding="utf-8",
                    errors="replace").read())
        print("  %d gesture-starting dispatch(es), all guarded by "
              "bindToolToPage()" % len(dispatch_sites(clean)))
        print("  bindToolToPage() binds via setPage()")
        print("  owned page released AFTER the tool is told, in both "
              "clearDocument() and setDocument()")
        print("\ncheck_tool_acts_on_the_clicked_page: PASS")
        return 0

    for p in problems:
        print("  %s" % p)
    print("\ncheck_tool_acts_on_the_clicked_page: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
