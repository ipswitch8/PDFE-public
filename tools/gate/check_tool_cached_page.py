#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_tool_cached_page.py — no AnnotationTool subclass may re-declare `page_`.

GitHub #236. Twelve tools each declared their own
``pdfe::core::Page* page_ = nullptr;``. Ten never released it when the document
closed, and two crashes came out of that (#234, #235) — both a repaint running
against a page the document had already freed.

The fix moved the member into ``AnnotationTool`` and made
``onDocumentClosing()`` non-virtual, so releasing is no longer a tool's job and
cannot be skipped. This guard protects the half that a compiler will not:

**a derived class that declares its own ``page_`` SHADOWS the base member.**

Nothing warns. ``setPage()`` assigns the derived copy, ``onDocumentClosing()``
nulls the base copy, the paint path reads the derived copy — and the defect is
back, in a file that looks exactly like the eleven correct ones. That is the
same "looks identical to a correct implementation" property that let the
original bug sit unnoticed through every gate.

WHY THIS IS A SOURCE SCAN AND NOT A TEST. A runtime test can only assert the
contract for tools it names, and the failure mode here is the tool nobody
thought to name — the thirteenth. `test_annotation_tool_page_release.cpp`
covers the base's behaviour for every subclass that will ever exist; this
covers the one thing that behaviour cannot survive.

Deliberately narrow: it looks for a member DECLARATION of a page pointer inside
a class that derives from AnnotationTool. It does not attempt to understand C++.
A false positive is a member genuinely named ``page_`` that is not the cached
page, which is precisely the thing that should not exist.

Usage:
  python tools/gate/check_tool_cached_page.py [--repo-root .]
  python tools/gate/check_tool_cached_page.py --self-test
Exit 0 = clean, 1 = a subclass re-declares the member, 2 = usage error.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

#: A class deriving from AnnotationTool, however it is spelled.
DERIVES = re.compile(
    r"\bclass\s+(\w+)\s*:\s*public\s+(?:pdfe::annotations::)?AnnotationTool\b")

#: A member declaration of the cached page pointer. `Page * page_` with any
#: qualification and spacing; must be a declaration, so it ends in `;` and is
#: not a parameter (no comma/paren before the semicolon).
#:
#: The trailing `(?:\s*(?://|/\*).*)?` is NOT cosmetic. The first version of
#: this pattern anchored the semicolon to end-of-line, and a declaration with a
#: trailing comment -- `pdfe::core::Page* page_ = nullptr;  // cached` -- slipped
#: straight through. That was found by reintroducing a shadow on purpose and
#: watching the guard report OK. Trailing comments on member declarations are
#: ordinary in this codebase (`QColor color_{0,0,0}; // Default black` sits two
#: lines from where the shadow was), so the miss would have been the common
#: case, not an edge case.
PAGE_MEMBER = re.compile(
    r"^[ \t]*(?:pdfe::core::)?Page\s*\*\s*page_\s*(?:=[^;]*)?;"
    r"[ \t]*(?://.*|/\*.*)?$",
    re.MULTILINE)

BASE_HEADER = "src/annotations/AnnotationTool.h"

# ---------------------------------------------------------------------------
# What the base class actually declares, read rather than assumed. (GitHub #254)
#
# FIRST, THE PART THAT WAS ALREADY SAFE, because overstating this would be the
# same sin as the overcount #254 is about. `PAGE_MEMBER` hardcodes the name
# `page_`, but main() already refuses to proceed unless the BASE header matches
# that same pattern. So renaming the base member does not silently blind this
# guard -- it makes it exit 1 with "no longer declares page_". The obvious C-ENUM
# story ("rename the base and the control goes quietly green") is not true here,
# and was checked rather than assumed.
#
# WHAT IS STILL TYPED, AND WHAT THIS ADDS. The existing anchor can only say
# "the base does not match what I expected"; it cannot say what the base says
# INSTEAD, so the operator reading that failure has to go and look. Worse, it
# cannot notice the base growing a SECOND raw page pointer -- `PAGE_MEMBER`
# searches, so one match satisfies it, and a second cached pointer under another
# name would be a shadowable member this guard never looks for in subclasses.
#
# So this reads the base's declarations and reports what is there. The
# single-name model is now asserted rather than presumed.
#
# FAIL-CLOSED. No declaration found returns None and the caller exits non-zero.
# A guard that cannot locate its own anchor must not be able to print OK.
# ---------------------------------------------------------------------------
BASE_MEMBER_DECL = re.compile(
    r"^[ \t]*(?:pdfe::core::)?Page\s*\*\s*(\w+)\s*(?:=[^;]*)?;"
    r"[ \t]*(?://.*|/\*.*)?$",
    re.MULTILINE)

#: The name this file's PAGE_MEMBER pattern is written against. Kept explicit so
#: the comparison below has something to compare TO; deriving both sides from the
#: same read would assert nothing (an output against another output).
ASSUMED_MEMBER_NAME = "page_"


def base_member_name(base_header_text: str) -> str | None:
    """The name of the cached page member declared in AnnotationTool itself.

    Returns None when no such declaration is present, which the caller must
    treat as a failure rather than as "no problem found".
    """
    names = [m.group(1) for m in BASE_MEMBER_DECL.finditer(base_header_text)]
    if not names:
        return None
    # The base declares exactly one raw page pointer. If that ever becomes more
    # than one, the guard's single-name model is wrong and the caller should say
    # so rather than silently picking the first.
    if len(set(names)) > 1:
        return "\0MULTIPLE:" + ",".join(sorted(set(names)))
    return names[0]


def offenders(text: str) -> list[str]:
    """Return the names of AnnotationTool subclasses declaring their own page_.

    Pure: no I/O, so it is directly unit-testable and --self-test exercises the
    real decision logic rather than a re-implementation of it.
    """
    found: list[str] = []
    for m in DERIVES.finditer(text):
        name = m.group(1)
        # Scan from the class head to the end of the file; a header may hold
        # several classes, but a page_ declaration after a later class head is
        # attributed to that later class on its own iteration, so the worst case
        # is attributing one offender to two classes -- never missing one.
        body = text[m.end():]
        nxt = DERIVES.search(body)
        if nxt:
            body = body[:nxt.start()]
        if PAGE_MEMBER.search(body):
            found.append(name)
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        bad = "class InkTool : public AnnotationTool {\nprivate:\n    pdfe::core::Page* page_ = nullptr;\n};\n"
        assert offenders(bad) == ["InkTool"], offenders(bad)

        ok = "class InkTool : public AnnotationTool {\nprivate:\n    AnnotationManager* manager_ = nullptr;\n};\n"
        assert offenders(ok) == [], offenders(ok)

        # Unqualified spelling, no initialiser.
        bad2 = "class T : public pdfe::annotations::AnnotationTool {\n    Page *page_;\n};\n"
        assert offenders(bad2) == ["T"], offenders(bad2)

        # A parameter named page_ is not a member declaration.
        param = ("class T : public AnnotationTool {\n"
                 "    void setPage(pdfe::core::Page* page_);\n};\n")
        assert offenders(param) == [], offenders(param)

        # A class that does NOT derive from AnnotationTool is none of our
        # business, even if it caches a page the same way.
        other = "class Unrelated : public QWidget {\n    pdfe::core::Page* page_ = nullptr;\n};\n"
        assert offenders(other) == [], offenders(other)

        # REGRESSION: a trailing comment must not hide the declaration. The
        # first version of this guard anchored `;` to end-of-line and reported
        # OK for exactly this line.
        commented = ("class T : public AnnotationTool {\n"
                     "    pdfe::core::Page* page_ = nullptr;  // cached\n};\n")
        assert offenders(commented) == ["T"], offenders(commented)

        block = ("class T : public AnnotationTool {\n"
                 "    pdfe::core::Page* page_ = nullptr; /* cached */\n};\n")
        assert offenders(block) == ["T"], offenders(block)

        # --- the base-anchor derivation (GitHub #254) ---------------------
        # Each of these is a branch main() can take. A branch nobody has driven
        # is indistinguishable from a branch that cannot be reached.
        assert base_member_name(
            "class AnnotationTool {\n    pdfe::core::Page* page_ = nullptr;\n};\n"
        ) == "page_"
        # Unqualified, no initialiser, trailing comment.
        assert base_member_name(
            "class AnnotationTool {\n    Page *page_;  // cached\n};\n") == "page_"
        # A renamed base member is REPORTED, not silently accepted as `page_`.
        # This is the check that makes the divergence failure in main() possible.
        assert base_member_name(
            "class AnnotationTool {\n    Page* cachedPage_ = nullptr;\n};\n"
        ) == "cachedPage_"
        # No declaration at all -> None, which main() turns into exit 1 rather
        # than into "nothing found, so nothing wrong".
        assert base_member_name("class AnnotationTool { int n_ = 0; };\n") is None
        # Two distinct page pointers -> flagged, not silently reduced to the
        # first. PAGE_MEMBER.search() would have been satisfied by either.
        multi = base_member_name(
            "class AnnotationTool {\n    Page* page_ = nullptr;\n"
            "    Page* otherPage_ = nullptr;\n};\n")
        assert multi is not None and multi.startswith("\0MULTIPLE:"), multi
        assert "otherPage_" in multi and "page_" in multi, multi
        # The same name twice (e.g. a re-declaration in an #ifdef arm) is ONE
        # name, not a multiple -- set() collapses it, and flagging it would make
        # the guard fail on a legal header.
        assert base_member_name(
            "class AnnotationTool {\n    Page* page_ = nullptr;\n"
            "    Page* page_ = nullptr;\n};\n") == "page_"

        # And the live base header really does satisfy the model, measured here
        # rather than asserted in prose: this is the file every subclass scan is
        # anchored to.
        live = pathlib.Path(__file__).resolve().parents[2] / BASE_HEADER
        if live.is_file():
            name = base_member_name(live.read_text(encoding="utf-8",
                                                  errors="replace"))
            assert name == ASSUMED_MEMBER_NAME, (
                f"{BASE_HEADER} declares {name!r}, this guard assumes "
                f"{ASSUMED_MEMBER_NAME!r}")

        print("[check-tool-cached-page] self-test: 14 checks passed")
        return 0

    root = pathlib.Path(args.repo_root).resolve()
    src = root / "src"
    if not src.is_dir():
        print(f"[check-tool-cached-page] no src/ under {root}", file=sys.stderr)
        return 2

    base = root / BASE_HEADER
    if not base.is_file():
        print(f"[check-tool-cached-page] {BASE_HEADER} is missing -- the base "
              "class this guard exists to protect is gone", file=sys.stderr)
        return 2
    base_text = base.read_text(encoding="utf-8", errors="replace")
    actual = base_member_name(base_text)
    if actual is None:
        print(f"[check-tool-cached-page] FAIL: {BASE_HEADER} declares no raw "
              "pdfe::core::Page* member at all. If the base does not own it, "
              "every tool is back to owning one and this guard is checking a "
              "contract that no longer exists.", file=sys.stderr)
        return 1
    if actual.startswith("\0MULTIPLE:"):
        print(f"[check-tool-cached-page] FAIL: {BASE_HEADER} declares more than "
              f"one raw page pointer ({actual.split(':', 1)[1]}). This guard "
              "looks for ONE shadowable member name in subclasses, so the second "
              "one is a member it would never check. Either fold them into one "
              "or extend PAGE_MEMBER and this check together.", file=sys.stderr)
        return 1
    if actual != ASSUMED_MEMBER_NAME:
        print(f"[check-tool-cached-page] FAIL: {BASE_HEADER} declares the cached "
              f"page as `{actual}`, but this guard scans subclasses for "
              f"`{ASSUMED_MEMBER_NAME}`. Shadowing requires the SAME name, so as "
              "written it would check for a member that cannot shadow anything "
              "and report OK over a fully open hole. Update "
              "ASSUMED_MEMBER_NAME and PAGE_MEMBER to "
              f"`{actual}` together.", file=sys.stderr)
        return 1
    if not PAGE_MEMBER.search(base_text):
        print(f"[check-tool-cached-page] FAIL: {BASE_HEADER} declares "
              f"`{actual}`, but PAGE_MEMBER does not match its declaration — the "
              "two patterns in this file have diverged, so subclass scanning is "
              "using a shape the base does not have.", file=sys.stderr)
        return 1

    bad: list[tuple[str, str]] = []
    scanned = 0
    for h in sorted(src.rglob("*.h")):
        rel = h.relative_to(root).as_posix()
        if rel == BASE_HEADER:
            continue
        text = h.read_text(encoding="utf-8", errors="replace")
        if "AnnotationTool" not in text:
            continue
        scanned += 1
        for cls in offenders(text):
            bad.append((rel, cls))

    print(f"[check-tool-cached-page] scanned {scanned} header(s) mentioning "
          f"AnnotationTool")
    if bad:
        print("[check-tool-cached-page] FAIL: these subclasses declare their "
              "own page_, which SHADOWS the base member. setPage() writes one "
              "copy and onDocumentClosing() clears the other, so the page is "
              "never released -- GitHub #234 / #235 all over again:",
              file=sys.stderr)
        for rel, cls in bad:
            print(f"    {rel}: class {cls}", file=sys.stderr)
        print("  Remove the declaration; AnnotationTool already provides it as "
              "a protected member.", file=sys.stderr)
        return 1

    print("[check-tool-cached-page] OK: the cached page is owned only by "
          "AnnotationTool")
    return 0


if __name__ == "__main__":
    sys.exit(main())
