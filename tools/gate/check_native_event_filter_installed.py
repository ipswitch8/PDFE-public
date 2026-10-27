#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Pin the install CALL for each QAbstractNativeEventFilter, in CODE not comments.

WHY THIS EXISTS. The `claim-check` gate failed pipeline p95 phase-11 on this, and
it demonstrated the hole rather than arguing it. On a temp copy of the tree it
mutated `src/main.cpp` three ways and ran the existing controls:

    mutation                                      check_engine_wiring  17 power tests
    baseline                                      rc=0                 green
    delete BOTH lines 208-209                     rc=1 (names it)      green
    delete ONLY the installNativeEventFilter(...) rc=0  GREEN          green

(That table is the gate's measurement, taken on a temp copy of
`src/ tests/ tools/gate/`. I tried twice to reproduce the middle column myself
and both attempts were confounded by an incomplete copy -- one missing most of
`src/`, one missing `tests/` -- each of which makes the reachability walk fail
for unrelated reasons, and a third attempt with the full tree exceeded a
two-minute budget. So I am neither independently confirming nor disputing its
`rc=0`. It does not matter to this guard's value: what IS verified below, twice
and against the real `src/main.cpp`, is that NOTHING in the tree pinned the call
and that this guard does.)

`check_engine_wiring.py` walks reachability from `main.cpp` over CLASS NAMES, so
the orphaned declaration

    static pdfe::app::PowerEventFilter powerEventFilter;

satisfies it on its own. Delete the install CALL and power events silently stop
reaching users -- while `docs/audit/p95-phase11/`, commit `63e3dd70` and GitHub
comment `6080895571` all keep saying "installed at src/main.cpp:208-209".

That is the gap: the claim is about the CALL, every mechanism read the NAME.
And the 17 power-event tests cannot close it, because they construct
`PowerEventFilter` directly -- they never go through the application's
installation path.

So this guard asserts the call exists, for every native event filter the
application is documented to install. A declaration alone does not satisfy it.

WHAT IT READS, AND ITS ONE REMAINING HOLE. The first version of this guard
regexed the raw file, and the `claim-check` gate flipped the real `src/main.cpp`
five ways to show what that bought:

    mutation                                  v1 (raw text)   now
    unmutated                                 green           green
    call DELETED                              red             red
    call COMMENTED OUT, declaration kept      GREEN  <-- bad  red
    call wrapped in `#if 0`                   GREEN  <-- bad  GREEN (see below)
    call deleted, named in a why-not comment  GREEN  <-- bad  red

Three of five defeated it, and none is exotic: commenting a line out is the
ordinary way a call gets disabled, and "deleted but still named in the comment
explaining why" is this estate's most repeated false green -- the header of
`check_engine_wiring.py`, the workflow step immediately before this one, records
that at the moment WIN-87 was actually closed a plain grep returned five hits
and every one was a COMMENT describing the defect that had just been removed.

So the input is now passed through
`check_win87_publish_shapes.strip_comments_and_literals`, which blanks comments
and string literals while preserving line structure. Both matchers use it: the
install-call search and the declaration scan.

RESIDUAL, stated rather than left to be discovered. This guard pins the call
against DELETION and against COMMENT-DISABLING. It does NOT catch exclusion that
needs preprocessing or control-flow analysis, measured: a call inside `#if 0`
(the lexer does not evaluate the preprocessor), inside `if (false) { ... }`, or
moved below `app.exec()` so it never runs. Closing those needs a real
preprocessor pass or a link-time check, which is more than this claim warrants;
none of the three is a plausible way to disable a call, whereas deleting or
commenting one out is exactly how it happens. The limit belongs here rather than
in a reader's assumptions.

WHY THE FIRST SELF-TEST MISSED ALL OF THIS: every one of its four directions
mutated by DELETION. The guard was tested against the single mutation class it
already handled. The direction list below now includes comment-disabling and the
named-in-a-comment case, and records `#if 0` as a KNOWN-GREEN so the hole is
asserted rather than merely admitted.

WHAT IS OUT OF SCOPE, BY CONSTRUCTION. This is a lexical guard over one named
file per filter, so:

  * It cannot see a filter installed from a DIFFERENT translation unit. The
    EXPECTED table names the file each filter is expected to be installed from,
    which bounds this rather than leaving it unknown -- but a green result says
    nothing about an install call living elsewhere, and nobody should read it as
    if it did.
  * It cannot see exclusion that needs preprocessing or control-flow analysis.
    Measured green: a call inside `#if 0`, inside `if (false) { ... }`, or moved
    below `app.exec()` so it never runs.
  * It identifies filter classes by PascalCase naming. A filter class named in
    camelCase would not be recognised by the completeness check.

Each of those is a disclosed limit with a measurement behind it, not an
assumption. The limits are repeated in
`docs/audit/p95-phase11/213-power-events-already-implemented.md` and on
GitHub #213 so they travel with the claim.

Usage:
    python tools/gate/check_native_event_filter_installed.py --repo-root .
    python tools/gate/check_native_event_filter_installed.py --self-test

Exit 0 = every expected install call is present. Exit 1 = one is missing.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_win87_publish_shapes import (  # noqa: E402
    strip_comments_and_literals,
)

# (filter class, instance identifier, the file that must install it).
#
# This IS a hand-made enumeration, which is this project's highest-weight
# test-gap class (C-ENUM), so it carries a reverse direction: every CLASS whose
# name ends in `EventFilter` and appears anywhere in the scanned file's code must
# have a row here, or the guard fails.
#
# Keyed on the CLASS NAME, not on the declaration's shape. The first version
# required the literal `static` keyword, and the claim-check gate measured what
# that bought -- a new filter with no row here passed as
#     pdfe::app::ThemeEventFilter themeEventFilter;              (non-static)
#     auto* f = new pdfe::app::ThemeEventFilter();                (heap)
# and reddened only for the `static` form. `static` at main.cpp:208 is a
# lifetime choice for outliving app.exec(); a plain local in main() does that
# too, and `installNativeEventFilter(new MyFilter)` is the form Qt's own
# examples use. So the next person had no reason to copy the keyword the guard
# silently depended on.
#
# Name-keyed is fail-closed: a mention of an EventFilter class with no row is an
# error even if it turns out to be benign, because the fix is one line (add the
# row, or add it with a stated exclusion) and the failure it prevents is #213's
# original defect reproduced for a different filter -- one that exists, is
# tested directly, and is never installed.
EXPECTED = [
    ("PowerEventFilter", "powerEventFilter", "src/main.cpp"),
]

# Any class whose name ends in EventFilter, however it is spelled at the point
# of use: `static ns::FooEventFilter f;`, `ns::FooEventFilter f;`,
# `new ns::FooEventFilter()`, or a bare mention. Deliberately NOT anchored on a
# declaration shape -- see the EXPECTED comment above.
# Upper-case first letter: class names here are PascalCase, methods are
# camelCase. That distinction is load-bearing -- without it this pattern
# matches `installNativeEventFilter` itself, which also ends in
# "EventFilter", and the guard then fails on its own baseline. Caught by
# this file's baseline self-test direction, which is exactly why a green
# direction is asserted rather than assumed.
_FILTER_CLASS_RX = re.compile(r"\b([A-Z]\w*EventFilter)\b")

# Not filters the application installs: Qt's base class, plus the method
# names guarded in case someone later capitalises one or adds a wrapper.
_FILTER_CLASS_IGNORE = {
    "QAbstractNativeEventFilter",
    "InstallNativeEventFilter",
    "RemoveNativeEventFilter",
}


def _install_call_present(text: str, instance: str) -> bool:
    """Is there an installNativeEventFilter call taking &<instance>?

    Whitespace-tolerant and receiver-agnostic: `app.installNativeEventFilter`,
    `qApp->installNativeEventFilter`, or a bare call all count. What must be
    present is the CALL passing this instance's address.
    """
    rx = re.compile(
        r"installNativeEventFilter\s*\(\s*&\s*" + re.escape(instance) + r"\s*\)")
    return rx.search(text) is not None


def check(repo_root: Path) -> int:
    rc = 0
    for cls, instance, rel in EXPECTED:
        path = repo_root / rel
        if not path.is_file():
            print("FAIL: %s does not exist (expected to install %s)" % (rel, cls))
            rc = 1
            continue
        # CODE, not raw text -- see the header. A commented-out call must not
        # satisfy this, and neither must the name appearing in a comment that
        # explains why the call was removed.
        try:
            text = strip_comments_and_literals(
                path.read_text(encoding="utf-8", errors="replace"))
        except ValueError as exc:
            print("FAIL: %s could not be lexed (%s). A file this cannot read is "
                  "a file it must not report clean." % (rel, exc))
            rc = 1
            continue

        if not _install_call_present(text, instance):
            print("FAIL: %s declares or names %s but does NOT call "
                  "installNativeEventFilter(&%s)." % (rel, cls, instance))
            print("      A declaration is not an installation. "
                  "check_engine_wiring.py cannot catch this: it walks class")
            print("      NAMES, so an orphaned `static %s %s;` satisfies it "
                  "while the filter never" % (cls, instance))
            print("      receives a single event. The power-event tests cannot "
                  "catch it either -- they construct")
            print("      the filter directly and never use the application's "
                  "install path.")
            rc = 1
        else:
            print("OK: %s installs %s via installNativeEventFilter(&%s)"
                  % (rel, cls, instance))

    # Reverse direction: an EventFilter CLASS named in the code with no row here.
    known_classes = {cls for cls, _, _ in EXPECTED}
    for _, _, rel in EXPECTED:
        path = repo_root / rel
        if not path.is_file():
            continue
        try:
            code = strip_comments_and_literals(
                path.read_text(encoding="utf-8", errors="replace"))
        except ValueError:
            continue  # already reported by the forward check
        seen = {c for c in _FILTER_CLASS_RX.findall(code)
                if c not in _FILTER_CLASS_IGNORE}
        for cls in sorted(seen - known_classes):
            print("FAIL: %s names the filter class `%s`, which has no row in "
                  "this guard's EXPECTED table." % (rel, cls))
            print("      Add one, so its install CALL is pinned too -- or add "
                  "it with a stated reason if it is")
            print("      deliberately not installed. Keyed on the class name "
                  "rather than the declaration's")
            print("      shape, because `static` is a lifetime choice and a "
                  "non-static or heap-allocated")
            print("      filter would otherwise slip past with no control at "
                  "all.")
            print("")
            print("      NOTE: if the filter is installed as a heap temporary "
                  "-- installNativeEventFilter(new")
            print("      %s()) with no named instance -- adding a row is NOT "
                  "sufficient on its own." % cls)
            print("      The forward matcher looks for `&<instance>`, so it has "
                  "no address to match. That is")
            print("      deliberate: a new install SHAPE should have to be "
                  "taught to this guard explicitly")
            print("      rather than silently accepted. Give the filter a named "
                  "instance, or extend")
            print("      _install_call_present and add a self-test direction "
                  "for the shape.")
            rc = 1
    return rc


_BASE = """
#include "app/PowerEvents.h"
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    static pdfe::app::PowerEventFilter powerEventFilter;
    app.installNativeEventFilter(&powerEventFilter);
    return app.exec();
}
"""

# The exact mutation check_engine_wiring.py misses: the declaration stays, the
# call goes.
_CALL_DELETED = _BASE.replace(
    "    app.installNativeEventFilter(&powerEventFilter);\n", "")

# Both lines gone -- check_engine_wiring.py DOES catch this one, and so must we.
_BOTH_DELETED = _CALL_DELETED.replace(
    "    static pdfe::app::PowerEventFilter powerEventFilter;\n", "")

# A filter added without a table row -- in each of the three shapes a developer
# would plausibly reach for. v1 reddened only for the first.
_UNKNOWN_FILTER = _BASE.replace(
    "    return app.exec();",
    "    static pdfe::app::ThemeEventFilter themeEventFilter;\n"
    "    return app.exec();")

_UNKNOWN_NONSTATIC = _BASE.replace(
    "    return app.exec();",
    "    pdfe::app::ThemeEventFilter themeEventFilter;\n"
    "    return app.exec();")

_UNKNOWN_HEAP = _BASE.replace(
    "    return app.exec();",
    "    auto* themeEventFilter = new pdfe::app::ThemeEventFilter();\n"
    "    return app.exec();")

_UNKNOWN_INLINE_NEW = _BASE.replace(
    "    return app.exec();",
    "    app.installNativeEventFilter(new pdfe::app::ThemeEventFilter());\n"
    "    return app.exec();")

# The three NON-DELETION mutations the first version of this guard let through.
_CALL_COMMENTED = _BASE.replace(
    "    app.installNativeEventFilter(&powerEventFilter);",
    "    // app.installNativeEventFilter(&powerEventFilter);")

_CALL_BLOCK_COMMENTED = _BASE.replace(
    "    app.installNativeEventFilter(&powerEventFilter);",
    "    /* app.installNativeEventFilter(&powerEventFilter); */")

# Deleted, but still named in the comment explaining why -- this estate's most
# repeated false green.
_CALL_NAMED_IN_WHY_NOT = _CALL_DELETED.replace(
    "    static pdfe::app::PowerEventFilter powerEventFilter;",
    "    static pdfe::app::PowerEventFilter powerEventFilter;\n"
    "    // Removed app.installNativeEventFilter(&powerEventFilter) pending\n"
    "    // a decision on resume-time handle revalidation.")

# KNOWN GREEN. strip_comments_and_literals does not evaluate the preprocessor,
# so this survives. Asserted as green so the hole is pinned rather than
# rediscovered -- if a future change makes it red, this direction fails and the
# header's residual note gets revisited deliberately.
_CALL_IF_ZERO = _BASE.replace(
    "    app.installNativeEventFilter(&powerEventFilter);",
    "#if 0\n    app.installNativeEventFilter(&powerEventFilter);\n#endif")


def self_test() -> int:
    """Every direction below is asserted, because a guard that only ever
    greens is decoration -- and because the first version of this self-test
    mutated ONLY by deletion and so tested the guard against the single
    mutation class it already handled.
    """
    cases = [
        ("baseline: declaration + call", _BASE, 0),
        ("CALL deleted, declaration kept "
         "(the case check_engine_wiring.py misses)", _CALL_DELETED, 1),
        ("both declaration and call deleted", _BOTH_DELETED, 1),
        ("a filter declared with no table row (static)", _UNKNOWN_FILTER, 1),
        # The three shapes v1 let through, because it keyed on `static`.
        ("a filter with no table row, NON-STATIC", _UNKNOWN_NONSTATIC, 1),
        ("a filter with no table row, HEAP-allocated", _UNKNOWN_HEAP, 1),
        ("a filter with no table row, inline `new` in the install call",
         _UNKNOWN_INLINE_NEW, 1),
        # Non-deletion mutations. v1 of this guard passed all three.
        ("call COMMENTED OUT with //", _CALL_COMMENTED, 1),
        ("call COMMENTED OUT with /* */", _CALL_BLOCK_COMMENTED, 1),
        ("call deleted but NAMED in a why-not comment",
         _CALL_NAMED_IN_WHY_NOT, 1),
        # Pinned as a KNOWN hole, not a pass: see the header's residual note.
        ("call inside #if 0 (KNOWN GREEN -- preprocessor not evaluated)",
         _CALL_IF_ZERO, 0),
    ]
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "src").mkdir()
        for label, body, want in cases:
            (root / "src" / "main.cpp").write_text(body, encoding="utf-8")
            got = check(root)
            ok = (got == want)
            print("  self-test %-62s want rc=%d got rc=%d  %s"
                  % (label, want, got, "ok" if ok else "MISMATCH"))
            if not ok:
                failures += 1
    if failures:
        print("SELF-TEST FAILED: %d of %d directions wrong" % (failures, len(cases)))
        return 1
    print("self-test OK: all %d directions behave" % len(cases))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    return check(Path(args.repo_root).resolve())


if __name__ == "__main__":
    sys.exit(main())
