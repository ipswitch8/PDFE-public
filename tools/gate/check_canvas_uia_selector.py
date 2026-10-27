# -*- coding: utf-8 -*-
"""Fail a test that resolves the PdfCanvas by a FIXED AutomationId chain.

    python tools/gate/check_canvas_uia_selector.py
    python tools/gate/check_canvas_uia_selector.py --self-test

Exit 0 = clean, 1 = at least one hard-coded lookup, 2 = usage/IO error.

WHY THIS GUARD EXISTS (GitHub #138, then GitHub #198)

Qt builds a widget's UIA AutomationId from its objectName CHAIN, so the canvas's
id changes whenever anyone inserts a widget ABOVE it. One insertion --
``pdfCentralArea`` -- turned

    QApplication.mainWindow.pdfCanvas
into
    QApplication.mainWindow.pdfCentralArea.pdfCanvas

and every call site asking for the old literal stopped matching.

What makes this worth a guard rather than a comment is the SYMPTOM. Every caller
turned "not found" into the message

    PdfCanvas absent from UIA tree

which accuses the environment. During GitHub #138 that sent the investigation at
the runner, the display and the DPI before anyone suspected the selector, while
the canvas sat in the tree the whole time. A diagnostic that blames the
environment is the most expensive kind of wrong: it is believed, and it is
believed for a long time.

#138 fixed two of six sites. #198 fixed the remaining four. A string that has
already drifted once, whose failure mode misdirects, and which nothing prevents
from being retyped, is exactly what a cheap mechanical check is for.

THE RULE

A tracked file under ``tests/`` fails if it passes the fixed chain as a LOOKUP
ARGUMENT -- ``auto_id="QApplication.mainWindow.pdfCanvas"`` or the equivalent
positional/keyword spellings.

Prose is NOT a violation, and that distinction is load-bearing: the docstrings in
``canvas_click.py``, ``canvas_geometry.py``, ``smoke_test.py`` and
``known_issues.md`` quote the literal precisely in order to explain why it must
not be used. A guard that reddened on those would be reddening the fix, and a
guard that reddens on correct content trains people to ignore it.

THE FIX for a real violation is ``find_canvas()`` in
``tests/ui_automation/harness/canvas_click.py``, which matches on the TRAILING
segment and is immune to the next widget someone puts in the chain.
"""
from __future__ import annotations

import argparse
import io
import os
import re
import subprocess
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

#: The chain is assembled at run time rather than written as one literal, so
#: this guard does not itself become the next copy someone greps up and pastes
#: into a test.
_SEGMENTS = ("QApplication", "mainWindow", "pdfCanvas")
_CHAIN = ".".join(_SEGMENTS)

#: A LOOKUP, not a mention: the chain inside quotes, immediately preceded by
#: either an `auto_id=`/`automation_id=`/`name=` keyword or an opening paren of
#: a locator call. Requiring that context is what lets the explanatory
#: docstrings keep quoting the string.
_LOOKUP_RE = re.compile(
    r"(?:auto_id|automation_id|automationId|name)\s*=\s*[\"']"
    + re.escape(_CHAIN)
    + r"[\"']"
    r"|(?:child_window|find|find_optional|find_widget|window)\s*\(\s*[\"']"
    + re.escape(_CHAIN)
    + r"[\"']"
)

SCAN_EXT = {".py"}


def find_violations(text):
    """Return a list of ``(line_number, line_text)`` lookup violations."""
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if _LOOKUP_RE.search(line):
            out.append((n, line.strip()))
    return out


def tracked_test_files(root="."):
    try:
        proc = subprocess.run(
            ["git", "ls-files", "-z", "tests"],
            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            check=True)
    except (OSError, subprocess.CalledProcessError):
        return []
    names = [n for n in proc.stdout.decode("utf-8", "replace").split("\0") if n]
    return sorted(n for n in names
                  if os.path.splitext(n)[1].lower() in SCAN_EXT)


SELF_TESTS = [
    # (label, source line, expect_violation)
    ("keyword lookup -- THE BUG",
     'canvas = win.child_window(auto_id="%s")' % _CHAIN, True),
    ("positional lookup",
     'canvas = win.child_window("%s")' % _CHAIN, True),
    ("find_widget keyword lookup",
     'c = find_widget(win, auto_id="%s", timeout=3)' % _CHAIN, True),
    ("single-quoted keyword lookup",
     "c = session.find_optional(auto_id='%s')" % _CHAIN, True),
    # The explanatory prose that MUST stay green -- one per real file shape.
    ("docstring mention",
     '    ("%s"), so a bare ``auto_id="pdfCanvas"``' % _CHAIN, False),
    ("comment mention",
     "# AutomationId chain `%s` is stale -- Qt builds" % _CHAIN, False),
    ("rst-literal mention",
     "    ``%s``. Qt builds that id from the" % _CHAIN, False),
    ("the correct call",
     "canvas = find_canvas(win, timeout=3.0)", False),
    ("trailing-segment match",
     'if (info.automation_id or "").endswith(".pdfCanvas"):', False),
]


def self_test():
    failures = 0
    for label, line, expect in SELF_TESTS:
        got = bool(find_violations(line))
        ok = got == expect
        if not ok:
            failures += 1
        print("  [%s] %-34s violation=%-5s (expected %s)"
              % ("ok  " if ok else "FAIL", label, got, expect))
    print("self-test: %d/%d passed" % (len(SELF_TESTS) - failures,
                                       len(SELF_TESTS)))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Fail a test that resolves PdfCanvas by a fixed UIA "
                    "AutomationId chain (GitHub #138 / #198).")
    parser.add_argument("--root", default=".")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    paths = tracked_test_files(args.root)
    if not paths:
        print("check_canvas_uia_selector: no tracked test .py files found")
        return 2

    violations = []
    for rel in paths:
        full = os.path.join(args.root, rel)
        try:
            with io.open(full, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError as exc:
            print("  ERROR %s: %s" % (rel, exc))
            return 2
        for n, line in find_violations(text):
            violations.append((rel, n, line))

    if violations:
        print("check_canvas_uia_selector: FAIL -- %d hard-coded PdfCanvas UIA "
              "lookup(s):" % len(violations))
        for rel, n, line in violations:
            print("  %s:%d\n      %s" % (rel, n, line))
        print("\nFix: use find_canvas() from "
              "tests/ui_automation/harness/canvas_click.py, which matches the "
              "TRAILING AutomationId segment. The fixed chain breaks whenever a "
              "widget is inserted above the canvas, and every caller reports the "
              "miss as 'PdfCanvas absent from UIA tree' -- a message that blames "
              "the runner for a selector bug.")
        return 1

    print("check_canvas_uia_selector: PASS -- %d tracked test file(s), no "
          "fixed-chain canvas lookups" % len(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
