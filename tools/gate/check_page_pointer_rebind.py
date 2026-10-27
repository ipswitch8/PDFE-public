#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Every class that caches a raw `Page*` must say what happens when it dangles.

THE CONTRACT
============
`pdfe::core::Command::rebindPage` (`src/core/Command.h`) is virtual with an
**empty default body**, and `src/core/SaveWithRebind.h` calls it on every
command after a full-rewrite save, handing each one the Page that replaced the
one it was holding. A command that caches a `Page*` and does not override
`rebindPage` therefore keeps a pointer to a Page the save has already replaced,
and nothing says so: the base class silently accepts the omission.

Eighteen classes currently honour that contract. All eighteen do so by
discipline; until this guard, **nothing enforced it**, and the failure mode of
forgetting is a stale `Page*` — the same shape as GitHub #234/#235/#236, where
a repaint ran against a page the document had already freed.

WHY THIS GUARD EXISTS RATHER THAN AN EXTENSION OF check_tool_cached_page.py
==========================================================================
This is the standing control for test-gap class **C-ENUM** -- "completeness
asserted from a hand-made enumeration" (`docs/audit/test-gap-classes.md` §1,
14 records, weight 75, the corpus's highest).

`check_tool_cached_page.py` is one of the five controls that class is credited
with, and it is a fair example of the blindness: its population is *classes
that derive from `AnnotationTool`*. That is the right population for the defect
it was written for and it cannot see this one, because the classes at risk here
are `Command` subclasses in five different families (annotations, forms,
redaction, signatures, image). A control whose population is one shape reports
"class closed" about that shape, not about the property.

So the remedy this guard demonstrates is not "add another shape to a list". It
is **derive the population from the property**: every class that declares a raw
`Page*` member, wherever it lives, found by walking the tree. Add a new command
family tomorrow and it is in scope automatically, with no list to update.

Comments and string literals are stripped first, using the lexer from
`check_win87_publish_shapes.py` -- a mention of `page_` in prose is not a
member declaration. That script's header explains why at length; reusing it
rather than writing a second copy is deliberate (DCA `0052`).

WHAT IT CANNOT DO
=================
It matches member DECLARATIONS lexically. A page cached indirectly (inside a
struct member, a `std::vector<Page*>`, a lambda capture stored in a
`std::function`) is invisible to it. That is a real limit and it is why the
baseline carries reasons rather than bare names.

Usage:
    python tools/gate/check_page_pointer_rebind.py [--repo-root .]
    python tools/gate/check_page_pointer_rebind.py --self-test
Exit 0 = clean, 1 = an unbaselined class, 2 = usage error.
"""

from __future__ import annotations

import argparse
import importlib.util
import io
import json
import os
import re
import subprocess
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BASELINE = os.path.join("tools", "gate", "page_pointer_rebind_baseline.json")

CLASS_START = re.compile(r"\bclass\s+(\w+)\s*(?:final\s*)?:?[^;{]*\{")
#: A raw, owning-or-not pointer to a Page held as a member. Deliberately matches
#: any member whose name contains "page", not only the literal `page_`: the
#: spelling is not the property, holding the pointer is.
PAGE_MEMBER = re.compile(r"(?:pdfe::core::)?Page\s*\*\s*\w*[Pp]age\w*_\s*[;=]")
REBIND = re.compile(r"\brebindPage\s*\(")


def _strip_fn():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "check_win87_publish_shapes.py")
    spec = importlib.util.spec_from_file_location("_w87", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.strip_comments_and_literals


def _class_body(text, start):
    i = text.find("{", start)
    if i < 0:
        return ""
    depth, j = 0, i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j]
        j += 1
    return text[i:]


#: An out-of-line override whose body does nothing. This is NOT a pedantic
#: case: the defect being guarded against is that `Command::rebindPage` has an
#: empty default body, so an override that is also empty reproduces the exact
#: failure while looking, to a presence check, like compliance. A guard that
#: accepts it is the vacuous-oracle shape -- it would measure that somebody
#: typed the name, not that anything happens.
EMPTY_OVERRIDE = re.compile(
    r"\b(\w+)\s*::\s*rebindPage\s*\([^)]*\)\s*(?:const\s*)?\{\s*\}")


def scan(texts):
    """{class: header} for classes caching a Page* with no EFFECTIVE override.

    "Effective" means the class names rebindPage AND, where the body is
    visible, that body is not empty.
    """
    cachers, defined, empty = {}, set(), set()
    for p, t in texts.items():
        for m in re.finditer(r"\b(\w+)\s*::\s*rebindPage\s*\(", t):
            defined.add(m.group(1))
        for m in EMPTY_OVERRIDE.finditer(t):
            empty.add(m.group(1))
    for p, t in texts.items():
        if not p.endswith((".h", ".hpp")):
            continue
        for m in CLASS_START.finditer(t):
            body = _class_body(t, m.start())
            if PAGE_MEMBER.search(body):
                # An inline `rebindPage(...) {}` inside the class body is the
                # same no-op, so check the declaration site too.
                inline_empty = bool(re.search(
                    r"\brebindPage\s*\([^)]*\)\s*(?:const\s*)?"
                    r"(?:override\s*)?\{\s*\}", body))
                cachers[m.group(1)] = (p, bool(REBIND.search(body)),
                                       inline_empty)
    out = {}
    for c, (p, decl, inline_empty) in cachers.items():
        if inline_empty or c in empty:
            out[c] = p          # present but does nothing
        elif not decl and c not in defined:
            out[c] = p          # absent entirely
    return out


def find_missing(root="."):
    """Returns {class: header}, or None if it could not measure."""
    files = subprocess.run(["git", "ls-files", "src"], cwd=root,
                           capture_output=True, text=True).stdout.split()
    files = [p for p in files if p.endswith((".h", ".hpp", ".cpp"))]
    if not files:
        return None
    strip = _strip_fn()
    texts = {}
    for p in files:
        raw = io.open(os.path.join(root, p), encoding="utf-8",
                      errors="replace").read()
        try:
            texts[p] = strip(raw)
        except ValueError:
            texts[p] = raw          # fail closed: unlexable -> treat as compliant
    # If nothing in the tree caches a Page* at all, we are not measuring the
    # thing we think we are measuring.
    any_cacher = False
    for p, t in texts.items():
        if p.endswith((".h", ".hpp")) and PAGE_MEMBER.search(t):
            any_cacher = True
            break
    if not any_cacher:
        return None
    return scan(texts)


def _load_baseline(root):
    path = os.path.join(root, BASELINE)
    if not os.path.exists(path):
        return {}
    data = json.load(io.open(path, encoding="utf-8"))
    return {k: v for k, v in data.items() if not k.startswith("_")}


def self_test():
    """Both directions, plus the two ways a lexical rule gets fooled."""
    failures = []
    strip = _strip_fn()

    def run(srcmap):
        return scan({p: strip(t) for p, t in srcmap.items()})

    # 1. Caches a Page*, declares the override -> must NOT flag.
    ok_decl = {"src/a/Cmd.h":
               "class Cmd : public Command {\n"
               "  void rebindPage(Page* p) override;\n"
               "  pdfe::core::Page* page_ = nullptr;\n};"}
    if run(ok_decl):
        failures.append("FALSE POSITIVE: a class declaring the override was flagged.")

    # 2. Caches a Page*, no override anywhere -> MUST flag.
    bad = {"src/a/Cmd.h":
           "class Cmd : public Command {\n"
           "  pdfe::core::Page* page_ = nullptr;\n};"}
    if "Cmd" not in run(bad):
        failures.append("MISSED: a class caching a Page* with no override was "
                        "not flagged -- the rule cannot fire.")

    # 3. Override defined OUT OF LINE in the .cpp -> must NOT flag. This is how
    #    every real command in this tree is written, so getting it wrong would
    #    make the guard useless rather than merely noisy.
    ok_outofline = {
        "src/a/Cmd.h": "class Cmd : public Command {\n"
                       "  pdfe::core::Page* page_ = nullptr;\n};",
        "src/a/Cmd.cpp": "void Cmd::rebindPage(Page* p) { page_ = p; }",
    }
    if run(ok_outofline):
        failures.append("FALSE POSITIVE: an out-of-line rebindPage definition "
                        "was not recognised.")

    # 4. The member appears only in a COMMENT -> must NOT flag. The comment
    #    shape is what defeated the C-BELOW sweep's first version (DCA 0091).
    commented = {"src/a/Cmd.h":
                 "class Cmd : public Command {\n"
                 "  // pdfe::core::Page* page_ = nullptr;  // removed\n"
                 "  int x_ = 0;\n};"}
    if run(commented):
        failures.append("FALSE POSITIVE: a member mentioned only inside a "
                        "comment was read as a real declaration.")

    # 5. An override that EXISTS but does nothing -> MUST flag. This case was
    #    added because the mutation run for this guard found it: an empty body
    #    reproduces the very defect (Command::rebindPage's empty default) while
    #    satisfying a presence check. A guard that accepts it measures that
    #    somebody typed the name, not that anything happens.
    empty_outofline = {
        "src/a/Cmd.h": "class Cmd : public Command {\n"
                       "  void rebindPage(Page* p) override;\n"
                       "  pdfe::core::Page* page_ = nullptr;\n};",
        "src/a/Cmd.cpp": "void Cmd::rebindPage(Page* p) { }",
    }
    if "Cmd" not in run(empty_outofline):
        failures.append(
            "MISSED: an override with an EMPTY body was accepted as "
            "compliance. It reproduces the base class's silent no-op exactly.")

    # 6. Same, declared and defined inline in the class body.
    empty_inline = {
        "src/a/Cmd.h": "class Cmd : public Command {\n"
                       "  void rebindPage(Page* p) override {}\n"
                       "  pdfe::core::Page* page_ = nullptr;\n};"}
    if "Cmd" not in run(empty_inline):
        failures.append(
            "MISSED: an INLINE empty override was accepted as compliance.")

    if failures:
        sys.stdout.write("[page-rebind] SELF-TEST FAILED\n")
        for f in failures:
            sys.stdout.write("  %s\n" % f)
        return 1
    sys.stdout.write(
        "[page-rebind] self-test OK: flags a cacher with no override, spares "
        "one that declares it, one that defines it out of line, and one whose "
        "member is only a comment.\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = os.path.abspath(args.repo_root)
    found = find_missing(root)

    if found is None:
        sys.stdout.write(
            "[page-rebind] FAIL: could not enumerate sources, or found no "
            "class caching a Page* at all. Refusing to report a clean result "
            "from a measurement that did not happen.\n")
        return 1

    baseline = _load_baseline(root)
    new = {c: h for c, h in found.items() if c not in baseline}
    fixed = [c for c in baseline if c not in found]

    if new:
        sys.stdout.write(
            "[page-rebind] FAIL: %d class(es) cache a raw Page* and never "
            "override rebindPage():\n\n" % len(new))
        for c, h in sorted(new.items()):
            sys.stdout.write("  %-34s declared in %s\n" % (c, h))
        sys.stdout.write(
            "\nCommand::rebindPage has an EMPTY default body and "
            "SaveWithRebind.h\ncalls it on every command after a full-rewrite "
            "save, so a command that\nskips it holds a Page the save already "
            "replaced.\n\nEither override rebindPage, or add it to\n  %s\n"
            "with a reason saying why this class is not subject to the "
            "rebind\nprotocol.\n" % BASELINE)
        return 1

    if fixed:
        sys.stdout.write(
            "[page-rebind] FAIL: %d baselined class(es) now override "
            "rebindPage (or no longer cache a Page*) and must be REMOVED from "
            "the baseline:\n" % len(fixed))
        for c in sorted(fixed):
            sys.stdout.write("  %s\n" % c)
        sys.stdout.write("\nA baseline that only grows stops being a ratchet.\n")
        return 1

    sys.stdout.write(
        "[page-rebind] OK: every class caching a raw Page* either overrides "
        "rebindPage or is baselined with a reason (%d baselined).\n" % len(found))
    return 0


if __name__ == "__main__":
    sys.exit(main())
