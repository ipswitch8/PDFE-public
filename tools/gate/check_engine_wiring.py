#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Flag a class the TEST SUITE exercises that PRODUCTION cannot reach.

This is the standing control for test-gap class **C-BELOW**, "every test enters
below the defect" -- see `docs/audit/test-gap-classes.md` section 3. Seven DCA
records, weight 54, **five of them reached users**, and until this script there
was no checker anywhere in `tools/gate/` addressing it.

THE BLINDNESS IT GUARDS
=======================
The seam the tests use sits beneath the layer holding the bug, so a feature that
is correct and UNREACHABLE looks identical to one that works. `0058`: seven unit
tests proved a refusal that was linked into nothing. `0056`: every oracle called
`followLinkAt` directly while the click route was dead for every user. `0082`:
the two pure layers were tested and the one that talks to Qt was never
instantiated.

THE RULE
========
A class declared under `src/` is FLAGGED when the test suite mentions it and it
is **not reachable from the application entry point**.

Reachability is computed as a TRANSITIVE CLOSURE over CODE references, from the
file that defines `main()`. Both of those words are load-bearing, and each fixes
a way the first version of this script was wrong:

  * **CODE references** -- comments and string literals are blanked first, by
    the same lexer `check_win87_publish_shapes.py` uses. Its header explains why
    in detail: at the moment WIN-87 was actually closed, a plain grep returned
    five hits and every one was a COMMENT describing the defect that had just
    been removed. Here the same shape appeared in this script's own first
    output: `ImageExporter` was reported reachable on the strength of exactly
    two mentions, `src/core/AtomicFileReplace.h:14` and
    `src/core/Document.cpp:532`, both of them prose inside `//` comments. A
    comment naming a component is not a call to it.

  * **TRANSITIVE** -- a reference from a file that is itself unreachable proves
    nothing. `WordExporter` was reported reachable because `ExcelExporter.cpp`
    calls `WordExporter::escapeXml`. That is a real call in real code, and it is
    worthless: `ExcelExporter` is itself reachable from nothing. Dead code
    referencing dead code formed a closed island that a depth-1 rule reads as
    two live components. Only `PlainTextExporter`, called from
    `MainWindow.cpp`, is genuinely wired of the four exporters.

Both errors ran the same direction -- they made the guard UNDER-report, so its
green was worth less than it looked. They were caught by the phase-3 claim-check
gate, not by this script's self-test, which is why the self-test now covers both
shapes explicitly.

Such a class is compiled into the product and reachable by nothing in it. That
is not automatically a defect -- see the false-positive list below -- but it is
always worth a human deciding which it is, which is the whole point.

WHY A BASELINE, AND WHAT THE BASELINE IS NOT
============================================
The classes already in this state (found by the sweep this script comes from)
are listed in `engine_wiring_baseline.json` so the guard can be switched on
TODAY instead of after someone wires or deletes all of them.

The baseline is a RATCHET, not an amnesty: a NEW unreachable class fails the
build, and a baselined entry that becomes reachable must be REMOVED from the
baseline or the guard fails too. A baseline that only ever grows is the
`0023` shape -- a control reporting "class closed" about its own list rather
than about the property.

WHAT IS OUT OF SCOPE, BY CONSTRUCTION
=====================================
Only classes declared in `.h`/`.hpp` are enumerated. A class defined entirely
inside a `.cpp` is invisible to this guard, and deliberately so: such a class is
usable only within its own translation unit, so "is it reachable from
elsewhere?" is not a meaningful question about it. `SignerIssuerPool`
(`src/signatures/LtvValidator.cpp:247`) and `PdfPageAccessible`
(`src/ui/PdfCanvasAccessible.cpp:187`) are both excluded on these grounds.

This is a real limit, not a bug -- but it means a green result says nothing
about translation-unit-local classes, and nobody should read it as if it did.

KNOWN FALSE POSITIVES -- this is lexical within a file, so it over-reports:
  * a class reached only through a base-class pointer or a factory string;
  * a class instantiated via Qt's meta-object system by name;
  * a pimpl whose only user is a header it does not own.
Each baseline entry therefore carries a `reason`, and the reason has to say
which of these it is -- or admit that it is genuinely unreachable.

Usage:
    python tools/gate/check_engine_wiring.py [--repo-root .]
    python tools/gate/check_engine_wiring.py --self-test
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

BASELINE = os.path.join("tools", "gate", "engine_wiring_baseline.json")

CLASS_DECL = re.compile(
    r"^\s*class\s+(?:\w+_EXPORT\s+)?(\w+)\s*(?:final)?\s*[:{]", re.M)

# The file that defines the application's entry point. Reachability is measured
# from here; a class the product cannot reach from main() is unreachable no
# matter how much dead code names it.
ENTRY_POINTS = ("src/main.cpp",)


def _strip_fn():
    """Borrow the C++ comment/literal lexer from the WIN-87 publish sweep.

    Reusing it rather than writing a second one is deliberate: a staleness rule
    implemented twice, with one copy fixed, is DCA `0052`. This is the project's
    one lexer and it FAILS CLOSED -- it raises on anything it cannot lex, so an
    unlexable file can never be silently reported as clean.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "check_win87_publish_shapes.py")
    spec = importlib.util.spec_from_file_location("_w87", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.strip_comments_and_literals


def _tracked(root, prefix, exts):
    out = subprocess.run(["git", "ls-files", prefix], cwd=root,
                         capture_output=True, text=True).stdout.split()
    return [p for p in out if p.endswith(exts)]


def _stem(path):
    return os.path.splitext(path)[0]


def find_unreachable(root="."):
    """Return {class_name: declaring_file} for classes tests use and the product
    cannot reach from its entry point. Returns None if it could not measure."""
    src = _tracked(root, "src", (".cpp", ".h", ".hpp"))
    tst = _tracked(root, "tests", (".cpp", ".py"))
    if not src or not tst:
        return None  # signals "could not measure" -- see main()

    strip = _strip_fn()
    texts = {}
    for p in src:
        raw = io.open(os.path.join(root, p), encoding="utf-8",
                      errors="replace").read()
        try:
            texts[p] = strip(raw)
        except ValueError:
            # Fail closed: a file we cannot lex is treated as referencing
            # everything, so it can never cause a class to be called dead.
            texts[p] = raw
    tst_blob = "\n".join(
        io.open(os.path.join(root, p), encoding="utf-8",
                errors="replace").read() for p in tst)

    declared = {}
    for p in src:
        if p.endswith((".h", ".hpp")):
            for cls in CLASS_DECL.findall(texts[p]):
                declared.setdefault(cls, p)
    if not declared:
        return None

    # stem -> the files forming that class's own translation unit
    by_stem = {}
    for p in src:
        by_stem.setdefault(_stem(p), []).append(p)

    words = {c: re.compile(r"\b%s\b" % re.escape(c)) for c in declared}

    # Which classes does each file reference in CODE (not its own pair)?
    refs = {}
    for p, t in texts.items():
        s = _stem(p)
        refs[p] = {c for c, w in words.items()
                   if _stem(declared[c]) != s and w.search(t)}

    # Transitive closure from the entry point.
    entries = [p for p in ENTRY_POINTS if p in texts]
    if not entries:
        return None  # cannot locate main() -> cannot measure

    live_files = set(entries)
    live_classes = set()
    changed = True
    while changed:
        changed = False
        for p in list(live_files):
            for c in refs.get(p, ()):
                if c in live_classes:
                    continue
                live_classes.add(c)
                changed = True
                for q in by_stem.get(_stem(declared[c]), ()):
                    live_files.add(q)

    out = {}
    for cls, hdr in sorted(declared.items()):
        if not words[cls].search(tst_blob):
            continue
        if cls not in live_classes:
            out[cls] = hdr
    return out


# GitHub #259. Every baseline entry carries a `kind`, and until 2026-09-25
# nothing read it: the vocabulary existed only as prose in the baseline's own
# `_rules`, which `_load_baseline` strips before validating. A taxonomy that
# nothing enforces is a description of what someone once intended, and it drifts
# -- the same shape p92 found in `guard_reachability_baseline.json` and DCA 0093
# records. Defined ONCE here and deliberately not restated in the baseline file;
# `test_the_engine_wiring_baseline_prose_does_not_restate_the_taxonomy` fails if
# a copy reappears there.
BASELINE_KINDS = frozenset((
    "lexical-false-positive",     # base-class pointer, factory string, moc
    "deliberate-test-only-seam",  # exists for tests, product path is elsewhere
    "superseded",                 # replaced, kept only until its callers go
    "admitted-gap",               # a real gap; MUST carry an issue number
))


def _validate_baseline(entries):
    """Return a list of problems with the baseline's own bookkeeping."""
    problems = []
    for name, entry in sorted(entries.items()):
        if not isinstance(entry, dict):
            problems.append("%s: entry is not an object" % name)
            continue
        kind = entry.get("kind")
        if kind not in BASELINE_KINDS:
            problems.append(
                "%s: kind %r is not one of %s"
                % (name, kind, ", ".join(sorted(BASELINE_KINDS))))
        if not (entry.get("reason") or "").strip():
            problems.append("%s: empty reason" % name)
        # An admitted gap that names no issue is an amnesty, not a ratchet.
        # The number lives in its own `issue` field, not in the reason prose --
        # the first version of this check grepped the prose for `#\d+` and
        # reported all 15 gaps as unissued, which was a defect in the CHECK.
        if kind == "admitted-gap":
            issue = entry.get("issue")
            ok = isinstance(issue, int) and issue > 0
            if isinstance(issue, str):
                ok = bool(re.fullmatch(r"#?\d+", issue.strip()))
            if not ok:
                problems.append(
                    "%s: admitted-gap with no usable `issue` field (got %r)"
                    % (name, issue))
    return problems


def _load_baseline(root):
    path = os.path.join(root, BASELINE)
    if not os.path.exists(path):
        return {}
    data = json.load(io.open(path, encoding="utf-8"))
    return {k: v for k, v in data.items() if not k.startswith("_")}


def _run_on(srcmap, tests_blob, entry="src/main.cpp"):
    """The rule, over an in-memory corpus. Shared by the self-test cases."""
    strip = _strip_fn()
    texts = {p: strip(t) for p, t in srcmap.items()}
    declared = {}
    for p, t in texts.items():
        if p.endswith((".h", ".hpp")):
            for cls in CLASS_DECL.findall(t):
                declared.setdefault(cls, p)
    by_stem = {}
    for p in srcmap:
        by_stem.setdefault(_stem(p), []).append(p)
    words = {c: re.compile(r"\b%s\b" % re.escape(c)) for c in declared}
    refs = {p: {c for c, w in words.items()
                if _stem(declared[c]) != _stem(p) and w.search(t)}
            for p, t in texts.items()}

    live_files, live_classes = {entry}, set()
    changed = True
    while changed:
        changed = False
        for p in list(live_files):
            for c in refs.get(p, ()):
                if c not in live_classes:
                    live_classes.add(c)
                    changed = True
                    for q in by_stem.get(_stem(declared[c]), ()):
                        live_files.add(q)

    return {c: h for c, h in declared.items()
            if re.search(r"\b%s\b" % re.escape(c), tests_blob)
            and c not in live_classes}


def self_test():
    """Prove the rule fires, stays quiet, and is not fooled either way.

    Case 3 and case 4 are the two shapes the phase-3 claim-check gate caught in
    this script's first version. Both made it UNDER-report.
    """
    failures = []
    tests_blob = "TEST(X, Y) { Widget w; Helper h; }"

    # 1. Reachable from main() -> must NOT flag.
    reachable = {
        "src/main.cpp": "#include \"a/Widget.h\"\nint main() { Widget w; }",
        "src/a/Widget.h": "class Widget : public QObject { };",
        "src/a/Widget.cpp": "Widget::Widget() {}",
    }
    if _run_on(reachable, tests_blob):
        failures.append("FALSE POSITIVE: a class main() reaches was flagged.")

    # 2. Named by nothing -> MUST flag.
    unreachable = {
        "src/main.cpp": "int main() { return 0; }",
        "src/a/Widget.h": "class Widget : public QObject { };",
        "src/a/Widget.cpp": "Widget::Widget() {}",
    }
    if "Widget" not in _run_on(unreachable, tests_blob):
        failures.append("MISSED: a class nothing names was not flagged -- the "
                        "rule cannot fire.")

    # 3. Named ONLY inside a comment -> MUST still flag. (ImageExporter.)
    comment_only = {
        "src/main.cpp": "// Widget is used by the batch path one day\n"
                        "int main() { return 0; }",
        "src/a/Widget.h": "class Widget : public QObject { };",
        "src/a/Widget.cpp": "Widget::Widget() {}",
    }
    if "Widget" not in _run_on(comment_only, tests_blob):
        failures.append(
            "MISSED: a class named only in a COMMENT was treated as reachable. "
            "This is the ImageExporter shape -- two prose mentions inside `//` "
            "comments read as two call sites.")

    # 4. Referenced only BY dead code -> MUST still flag both. (WordExporter
    #    called from ExcelExporter, neither reachable from main().)
    dead_island = {
        "src/main.cpp": "int main() { return 0; }",
        "src/a/Widget.h": "class Widget : public QObject { };",
        "src/a/Widget.cpp": "Widget::Widget() {}",
        "src/a/Helper.h": "class Helper : public QObject { };",
        "src/a/Helper.cpp": "#include \"Widget.h\"\nvoid Helper::go() "
                            "{ Widget w; }",
    }
    got = _run_on(dead_island, tests_blob)
    if "Widget" not in got or "Helper" not in got:
        failures.append(
            "MISSED: a closed island of dead code referencing dead code was "
            "read as live (flagged %s). This is the WordExporter/ExcelExporter "
            "shape -- a real call from a caller nothing can reach." %
            sorted(got))

    if failures:
        sys.stdout.write("[engine-wiring] SELF-TEST FAILED\n")
        for f in failures:
            sys.stdout.write("  %s\n" % f)
        return 1
    sys.stdout.write(
        "[engine-wiring] self-test OK: flags the unreachable case, spares the "
        "reachable one, and is fooled by neither a comment mention nor a "
        "reference from equally dead code.\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = os.path.abspath(args.repo_root)
    found = find_unreachable(root)

    # "Could not measure" is an ERROR, never a pass. A guard that reports a
    # clean sweep when it enumerated nothing is the C-FAILOPEN shape this
    # project has already been bitten by (`0085`, GitHub #246).
    if found is None:
        sys.stdout.write("[engine-wiring] FAIL: could not enumerate sources, "
                         "tests, declarations or the entry point. Refusing to "
                         "report a clean result from a measurement that did "
                         "not happen.\n")
        return 1

    baseline = _load_baseline(root)

    # GitHub #259: the baseline's own bookkeeping is checked before it is
    # trusted to excuse anything. A `kind` nothing validates is a label.
    problems = _validate_baseline(baseline)
    if problems:
        sys.stdout.write(
            "[engine-wiring] FAIL: %d problem(s) in %s itself:\n"
            % (len(problems), BASELINE))
        for p in problems:
            sys.stdout.write("  %s\n" % p)
        sys.stdout.write(
            "\nEvery entry needs a non-empty reason and a kind from: %s.\n"
            "An 'admitted-gap' must cite an issue number, or it is an amnesty "
            "rather than a ratchet.\n" % ", ".join(sorted(BASELINE_KINDS)))
        return 1

    new = {c: h for c, h in found.items() if c not in baseline}
    fixed = [c for c in baseline if c not in found]

    if new:
        sys.stdout.write(
            "[engine-wiring] FAIL: %d class(es) the tests exercise that the "
            "product cannot reach from %s:\n\n" % (len(new), ENTRY_POINTS[0]))
        for c, h in sorted(new.items()):
            sys.stdout.write("  %-34s declared in %s\n" % (c, h))
        sys.stdout.write(
            "\nEach is compiled into the product and reachable by nothing in "
            "it.\nEither wire it up, delete it, or add it to\n  %s\nwith a "
            "reason saying WHY it is acceptable -- a base-class-pointer or\n"
            "factory-string false positive, or an admitted gap.\n" % BASELINE)
        return 1

    if fixed:
        sys.stdout.write(
            "[engine-wiring] FAIL: %d baselined class(es) are now reachable "
            "and must be REMOVED from the baseline:\n" % len(fixed))
        for c in sorted(fixed):
            sys.stdout.write("  %s\n" % c)
        sys.stdout.write(
            "\nA baseline that only grows stops being a ratchet.\n")
        return 1

    sys.stdout.write(
        "[engine-wiring] OK: %d known unreachable class(es), all baselined "
        "with reasons; no new ones.\n" % len(found))
    return 0


if __name__ == "__main__":
    sys.exit(main())
