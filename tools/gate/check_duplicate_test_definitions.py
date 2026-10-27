#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""No substantial helper under tests/ may be defined in two files unadjudicated.

GitHub #275. `DirectoryAttacker` -- a ~155-line helper that spawns a thread and
runs raw Win32 attacks against a watched directory, backing 20 TOCTTOU publish
tests -- was defined twice, and the guard that exists for exactly this defect
could not see it for two independent reasons:

  1. Its population was a hand-written list of three names, so it covered what
     somebody remembered to add. "The population is part of the spec" -- the same
     class as #199, where a leak guard's population was git-tracked files while
     the leak itself was in build output.
  2. Its matcher accepted a definition only when the line began with
     `std::string`, `bool`, `void` or `int`, AND required `<name>(`.
     `struct DirectoryAttacker {` matches neither, so adding the name to the list
     would have produced GREEN -- worse than the silence, because it would have
     looked like coverage.

THE COST, MEASURED. A poll-race fix was applied to the phase58 copy only: all 20
TOCTTOU cases went green, and a stability run under deliberate load then failed 9
of 12, entirely in phase59. One fix, half applied, looking complete. Unloaded,
both copies passed 12/12 -- so an ordinary run could not tell them apart.

WHAT THIS INVERTS
-----------------
The default. A hand-listed population is "covered if someone remembered"; this is
"flagged unless adjudicated". Every substantial column-0 definition appearing in
more than one file under tests/ is reported, and the baseline is where a
deliberate exception is recorded with a reason.

The baseline is a TWO-WAY ratchet: a new unadjudicated duplicate fails, and an
adjudicated entry that has stopped being a duplicate must be removed or this also
fails. A list that only grows reports "closed" about itself rather than about the
property.

WHY SIZE AND NOT SIMILARITY -- the threshold that would have missed its own case
-------------------------------------------------------------------------------
The obvious discriminator is textual similarity: flag a duplicate when the two
bodies look alike. It was measured before being adopted, and it is WRONG here.

    DirectoryAttacker: phase58 = 180 lines, phase59 = 155 lines,
                       difflib SequenceMatcher ratio = 0.519

Any sane similarity threshold EXCLUDES the exact defect #275 exists for -- at 0.80,
sixteen duplicated names qualify and DirectoryAttacker is not among them.
The reason is the defect itself: the copies drifted, and they drifted BECAUSE they
were duplicated and nobody could see both at once. The more dangerous the
duplication has become, the less similar the copies look, so similarity scores the
symptom backwards.

Size survives. Measured over the committable tree:

    min-body-lines >= 10 ->  77 findings
    min-body-lines >= 15 ->  39
    min-body-lines >= 20 ->  27
    min-body-lines >= 25 ->  18   <-- chosen
    min-body-lines >= 30 ->  12
    min-body-lines >= 40 ->   4
    min-body-lines >= 60 ->   1

Those are 2026-10-08 numbers, i.e. after DirectoryAttacker was merged out of the set
and after the multi-line-declaration false positive below was fixed. The threshold was
chosen against the earlier, larger figures -- 83 / 44 / 31 / 23 / 16 / 3 -- and 25 is
the knee either way.

At 25 the findings are all substantial helpers: GapRegisterEmitter (90 lines),
buildOcspResponseDer (48), ChildApp (47), savedPageContent (44), loadTestSigner (30,
verbatim in FIVE files). Below it the set fills with small local fixtures -- a
`struct Cell { int x, y; };` in two files shares a name and nothing else, and
flagging every such pair regardless of size would be the guard that hid four real
failures behind 365 matches for five commits.

The threshold applies to the SMALLEST copy, so a large helper cannot be excused by
having one short variant somewhere.

NOT AN ODR CLAIM, AND THE DISTINCTION MATTERS
---------------------------------------------
Both `DirectoryAttacker` copies live in anonymous namespaces, so they have
internal linkage and violate nothing -- the tree compiles and links correctly. The
defect is MAINTENANCE duplication: a change to one is invisible from the other and
the compiler will never say so. A guard framed as an ODR check would have to
exempt precisely the case that caused the incident.

SCOPE LIMIT, stated so a green result is not over-read
-----------------------------------------------------
Column 0 only. A helper indented inside a namespace block is invisible here. That
is a real hole rather than a safe simplification; it is accepted because this
codebase does not indent inside namespaces (verified: `struct DirectoryAttacker {`
sits at column 0 inside an anonymous namespace in both files) and because the
alternative is a C++ parser. If the convention changes this guard quietly narrows,
so --self-test asserts the convention still holds in tests/support/.

EXIT CODES
----------
  0  no unadjudicated duplicate definitions
  1  a duplicate is unadjudicated, or an adjudicated entry is no longer a duplicate
  2  usage / IO error
"""
from __future__ import annotations

import argparse
import io
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from typing import Dict, List, Optional, Set, Tuple

try:  # pragma: no cover
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

# THREE levels, not two: this file lives at <repo>/tools/gate/, so
# dirname(dirname(...)) lands on <repo>/tools and --tests-dir then defaults to
# <repo>/tools/tests, which does not exist. Caught by running the real mode --
# --self-test does not exercise this, because it builds its own probe trees and
# never uses the default.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BASELINE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "duplicate_test_definitions_baseline.json")

#: Smallest definition that counts as a duplication hazard. The header records the
#: measured distribution behind the number, and why this is a SIZE threshold
#: rather than a similarity one.
MIN_BODY_LINES = 25

#: A TYPE definition at column 0.
TYPE_DEF = re.compile(
    r"^(?:struct|class|union)\s+(\w+)"
    r"|^enum\s+class\s+(\w+)"
    r"|^enum\s+(\w+)")

#: A FUNCTION definition at column 0. Deliberately loose on the return type: the
#: old guard's four-prefix allowlist is the defect being fixed, so enumerating
#: types here would reintroduce it one level down.
FUNC_DEF = re.compile(r"^[A-Za-z_][\w:<>,\s*&]*?(\w+)\s*\(")

#: Lines that look like definitions but are not.
NOT_A_DEF = re.compile(
    r"^\s*(?:#|//|/\*|\*|template\s*<|using\s|typedef\s|friend\s|return\s|"
    r"extern\s+\"C\"|namespace\s|\}|TEST|INSTANTIATE_|MATCHER|MOCK_|"
    r"static_assert)")


def strip_block_comments(src: str) -> str:
    """Blank /* */ bodies, preserving line structure so counts stay true."""
    out: List[str] = []
    i, n, in_c = 0, len(src), False
    while i < n:
        if not in_c and src.startswith("/*", i):
            in_c, i = True, i + 2
            out.append("  ")
            continue
        if in_c and src.startswith("*/", i):
            in_c, i = False, i + 2
            out.append("  ")
            continue
        out.append(src[i] if (not in_c or src[i] == "\n") else " ")
        i += 1
    return "".join(out)


def _opens_a_body(lines: List[str], start: int, limit_hint: int = 40) -> bool:
    """Does the construct beginning at `lines[start]` open a body?

    Balances any parameter list from `start`, then reports on the first
    significant character after it: `{` (or `:` introducing a constructor
    initialiser list) means a DEFINITION; `;` means a DECLARATION.

    A type definition with no parentheses at all is decided by the first `{`
    or `;` encountered, which is why the paren depth starts at zero and the
    loop checks both.
    """
    depth = 0
    seen_open = False
    for k in range(start, min(len(lines), start + limit_hint)):
        for ch in lines[k]:
            if ch == "(":
                depth += 1
                seen_open = True
                continue
            if ch == ")":
                depth -= 1
                continue
            if depth > 0:
                continue          # inside the parameter list
            if ch == "{":
                return True
            if ch == ";":
                return False
            if ch == ":" and seen_open:
                return True       # constructor initialiser list
    # Ran out of lookahead without resolving. Treat as NOT a definition: the
    # cost of a miss is one unflagged duplicate, the cost of a false positive is
    # a guard nobody trusts.
    return False


def definition_sizes(path: str) -> Dict[str, int]:
    """name -> body line count, for column-0 definitions in this file.

    The extent is found by brace balance from the definition line, capped so a
    file whose braces do not balance (an unclosed #if arm, say) cannot run to the
    end of the file and report one enormous definition.
    """
    try:
        raw = io.open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return {}
    lines = strip_block_comments(raw).splitlines()
    out: Dict[str, int] = {}
    for i, line in enumerate(lines):
        if not line or line[0] in " \t" or NOT_A_DEF.match(line):
            continue
        m = TYPE_DEF.match(line) or FUNC_DEF.match(line)
        if not m:
            continue
        # `struct X;` is a forward declaration; `bool f(int);` is a function
        # declaration. Neither is a definition, and counting them would make
        # every forward-declaring header a duplicate site.
        if ";" in line and "{" not in line:
            continue
        name = next((g for g in m.groups() if g), None)
        if not name:
            continue
        # A MULTI-LINE declaration. Checking only the first line for `;` is not
        # enough and the shortfall was real: tests/support/SaveFailureInjection.h
        # declares
        #
        #     KilledSaveResult killChildInsideSaveWindow(
        #         const std::filesystem::path& sourcePdf,
        #         ...
        #         std::chrono::milliseconds waitForWindow = ...);
        #
        # whose `;` is four lines down, so the header was reported as a second
        # DEFINITION and the guard's first run accused a correct
        # declaration-plus-definition pair of being duplication. A guard's first
        # finding being wrong is how it loses its audience.
        #
        # So balance the parameter list and look at what actually follows: `{`
        # opens a body, `;` ends a declaration. `:` is a constructor
        # initialiser list, which also opens a definition.
        if not _opens_a_body(lines, i, limit_hint=40):
            continue
        depth, j, started = 0, i, False
        limit = min(len(lines), i + 800)
        while j < limit:
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                started = True
            if started and depth <= 0:
                break
            j += 1
        # Keep the LARGEST when a name appears twice in one file (overloads):
        # the hazard is the big one.
        out[name] = max(out.get(name, 0), j - i + 1)
    return out


def iter_test_sources(tests_dir: str) -> List[str]:
    """COMMITTABLE C++ sources under tests_dir.

    Tracked + staged + untracked-but-not-ignored -- the same three-way union
    check_tracked_path_hygiene.py uses, and chosen over a plain walk for two
    measured reasons:

      * tests/_tmp/ is gitignored pytest scratch holding SNAPSHOT COPIES of real
        test files. A raw walk found 2515 files and 221 duplicates, most of them
        one source and its own snapshots. A guard that cries wolf on its first run
        is a guard nobody keeps.
      * `git ls-files` alone has a one-commit blind spot: a brand-new helper file
        is untracked, so a duplicate introduced in the same change as its own file
        would be invisible. `--others --exclude-standard` closes that.

    Falls back to a filtered walk outside a git repo, because a guard that cannot
    enumerate must not report OK -- main() treats an empty population as failure.
    """
    def git(*args: str) -> List[str]:
        try:
            r = subprocess.run(["git", "-C", REPO, *args], capture_output=True,
                               text=True, encoding="utf-8", errors="replace")
        except OSError:
            return []
        if r.returncode != 0:
            return []
        return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]

    rels: Set[str] = set()
    rels.update(git("ls-files"))
    rels.update(git("diff", "--cached", "--name-only", "--diff-filter=ACMR"))
    rels.update(git("ls-files", "--others", "--exclude-standard"))

    want = os.path.abspath(tests_dir).replace("\\", "/").rstrip("/") + "/"
    out: List[str] = []
    if rels:
        for rel in rels:
            if not rel.endswith((".cpp", ".h", ".hpp", ".cc")):
                continue
            absolute = os.path.join(REPO, rel.replace("/", os.sep))
            if not os.path.isfile(absolute):
                continue                      # staged deletion
            if os.path.abspath(absolute).replace("\\", "/").startswith(want):
                out.append(absolute)
        if out:
            return sorted(out)

    for dirpath, dirnames, filenames in os.walk(tests_dir):
        dirnames[:] = [d for d in dirnames
                       if d not in {"__pycache__", "_runtime", "_tmp",
                                    "_failure_artifacts"}]
        for name in filenames:
            if name.endswith((".cpp", ".h", ".hpp", ".cc")):
                out.append(os.path.join(dirpath, name))
    return sorted(out)


def find_duplicates(tests_dir: str,
                    min_lines: int = MIN_BODY_LINES
                    ) -> Dict[str, List[Tuple[str, int]]]:
    """name -> [(file, body_lines)] for SUBSTANTIAL definitions in >1 file."""
    where: Dict[str, List[Tuple[str, int]]] = defaultdict(list)
    for path in iter_test_sources(tests_dir):
        try:
            rel = os.path.relpath(path, REPO).replace("\\", "/")
        except ValueError:
            rel = path.replace("\\", "/")
        for name, size in definition_sizes(path).items():
            where[name].append((rel, size))
    out: Dict[str, List[Tuple[str, int]]] = {}
    for name, occ in where.items():
        if len(occ) < 2:
            continue
        if min(sz for _f, sz in occ) < min_lines:
            continue
        out[name] = sorted(occ)
    return out


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tests-dir", default=os.path.join(REPO, "tests"))
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--min-lines", type=int, default=MIN_BODY_LINES)
    ap.add_argument("--list", action="store_true",
                    help="print every duplicate found and exit 0")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if not os.path.isdir(args.tests_dir):
        print("no such tests dir: %s" % args.tests_dir, file=sys.stderr)
        return 2

    sources = iter_test_sources(args.tests_dir)
    dupes = find_duplicates(args.tests_dir, min_lines=args.min_lines)

    if args.list:
        print("%d duplicate definition(s) of >=%d lines across %d file(s)"
              % (len(dupes), args.min_lines, len(sources)))
        for name in sorted(dupes, key=lambda n: -min(s for _f, s in dupes[n])):
            print("  %-34s smallest=%3d lines  in %d file(s)"
                  % (name, min(s for _f, s in dupes[name]), len(dupes[name])))
            for f, sz in dupes[name]:
                print("        %-70s %d" % (f, sz))
        return 0

    if not os.path.isfile(args.baseline):
        print("no baseline at %s" % args.baseline, file=sys.stderr)
        return 2
    base = json.load(io.open(args.baseline, encoding="utf-8"))
    adjudicated = {k: v for k, v in base.items() if not k.startswith("_")}

    print("[dup-test-defs] scanned %d file(s); %d duplicate definition(s) of "
          ">=%d lines; %d adjudicated"
          % (len(sources), len(dupes), args.min_lines, len(adjudicated)))

    # A scan that enumerated nothing is indistinguishable from a broken matcher,
    # so refuse to report OK on an empty population.
    if not sources:
        print("[dup-test-defs] FAIL: enumerated 0 source files, so a clean "
              "result would be vacuous.", file=sys.stderr)
        return 1

    ok = True
    for name in sorted(set(dupes) - set(adjudicated)):
        print("  UNADJUDICATED: %s defined at column 0 in %d file(s), smallest "
              "copy %d lines:"
              % (name, len(dupes[name]), min(s for _f, s in dupes[name])))
        for f, sz in dupes[name]:
            print("      %-70s %d lines" % (f, sz))
        ok = False

    for name in sorted(set(adjudicated) - set(dupes)):
        print("  STALE: %s is adjudicated as a duplicate but is no longer one. "
              "Remove the baseline entry -- a list that only grows reports "
              "'closed' about itself." % name)
        ok = False

    if not ok:
        print("", file=sys.stderr)
        print("[dup-test-defs] FAIL. A helper defined in two files is a helper "
              "whose next fix will be half-applied: #275's poll-race fix went "
              "into one copy, all 20 TOCTTOU cases passed, and a loaded "
              "stability run then failed 9 of 12 in the other. Move it to "
              "tests/support/, or record the exception with a reason in %s."
              % os.path.relpath(args.baseline, REPO).replace("\\", "/"),
              file=sys.stderr)
        return 1

    print("[dup-test-defs] OK -- every duplicate definition of >=%d lines under "
          "tests/ is adjudicated, and every adjudicated entry is still a "
          "duplicate" % args.min_lines)
    return 0


def self_test() -> int:
    """Prove the matcher sees a duplicated TYPE, and that the threshold behaves.

    The specific regression pinned first: `struct X {` has no `(` and does not
    begin with one of std::string/bool/void/int, so the guard this replaces could
    not have detected DirectoryAttacker even with the name added to its list.
    """
    failures: List[str] = []

    def defs_of(text: str) -> Set[str]:
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "probe.cpp")
            with io.open(p, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
            return set(definition_sizes(p))

    cases = [
        ("struct definition",
         "struct DirectoryAttacker {\n    int x;\n};\n", {"DirectoryAttacker"}, set()),
        ("class definition", "class Foo {\npublic:\n};\n", {"Foo"}, set()),
        ("class with base", "class Bar : public Foo {\n};\n", {"Bar"}, set()),
        ("enum class", "enum class Mode {\n    A,\n};\n", {"Mode"}, set()),
        ("forward declarations are NOT definitions",
         "struct Fwd;\nclass Fwd2;\n", set(), {"Fwd", "Fwd2"}),
        ("function definition",
         "bool applyProtectedDacl(const char* p) {\n    return true;\n}\n",
         {"applyProtectedDacl"}, set()),
        # The replaced guard's four-prefix allowlist would have missed this.
        ("function with an unlisted return type",
         "std::set<std::string> seenNames(int n) {\n    return {};\n}\n",
         {"seenNames"}, set()),
        ("function declaration is not a definition",
         "bool applyProtectedDacl(const char* p);\n", set(), {"applyProtectedDacl"}),
        # REGRESSION. The first version checked only the FIRST line for `;`, so
        # this shape -- which is how tests/support/SaveFailureInjection.h declares
        # killChildInsideSaveWindow -- was counted as a second DEFINITION, and the
        # guard's first run accused a correct declaration/definition pair of being
        # duplication. A guard whose first finding is wrong loses its audience.
        ("MULTI-LINE declaration is not a definition",
         "KilledSaveResult killChildInsideSaveWindow(\n"
         "    const std::filesystem::path& sourcePdf,\n"
         "    const std::string& filter,\n"
         "    std::chrono::milliseconds wait = std::chrono::seconds(30));\n",
         set(), {"killChildInsideSaveWindow"}),
        ("multi-line DEFINITION still counts",
         "KilledSaveResult killChildInsideSaveWindow(\n"
         "    const std::filesystem::path& sourcePdf,\n"
         "    const std::string& filter)\n"
         "{\n    return {};\n}\n",
         {"killChildInsideSaveWindow"}, set()),
        ("constructor initialiser list opens a definition",
         "Thing::Thing(int a)\n    : a_(a)\n{\n}\n", {"Thing"}, set()),
        ("indented definition is out of scope (documented limit)",
         "    struct Indented {\n    };\n", set(), {"Indented"}),
        ("TEST macro is not a definition", "TEST(Suite, Case)\n{\n}\n",
         set(), {"Suite", "Case"}),
        ("a struct named in a comment is not a definition",
         "// struct Commented {\nint real() {\n    return 0;\n}\n",
         {"real"}, {"Commented"}),
    ]
    for label, text, must, must_not in cases:
        got = defs_of(text)
        for name in must:
            if name not in got:
                failures.append("%s: expected %r among %s"
                                % (label, name, sorted(got)))
        for name in must_not:
            if name in got:
                failures.append("%s: %r must NOT be reported, got %s"
                                % (label, name, sorted(got)))

    # --- the SIZE threshold, end to end through find_duplicates -------------
    # The matcher cases prove definitions are SEEN. They say nothing about the
    # threshold that decides which ones MATTER, and that threshold is the whole
    # design -- see the header for why similarity would have excluded
    # DirectoryAttacker at 0.519.
    def dupes_of(files: Dict[str, str], min_lines: int):
        with tempfile.TemporaryDirectory() as td:
            sub = os.path.join(td, "tests")
            os.makedirs(sub)
            for name, text in files.items():
                with io.open(os.path.join(sub, name), "w", encoding="utf-8",
                             newline="\n") as fh:
                    fh.write(text)
            # No git repo here, so this also exercises the os.walk fallback --
            # the guard must still enumerate outside a checkout.
            return find_duplicates(sub, min_lines=min_lines)

    big = "struct Helper {\n" + "    int pad;\n" * 40 + "};\n"
    small = "struct Tiny {\n    int x;\n};\n"
    lopsided = "struct Helper {\n    int x;\n};\n"

    size_cases = 0

    if "Helper" not in dupes_of({"a.cpp": big, "b.cpp": big}, 25):
        failures.append("a 42-line struct duplicated across two files was NOT "
                        "flagged; the threshold excludes the hazard it exists "
                        "to catch")
    size_cases += 1

    if "Tiny" in dupes_of({"a.cpp": small, "b.cpp": small}, 25):
        failures.append("a 3-line struct duplicated across two files WAS "
                        "flagged; at that sensitivity the guard reports 201 "
                        "findings and gets ignored")
    size_cases += 1

    # Threshold is on the SMALLEST copy. Recorded as a test so the choice is not
    # rediscovered by someone changing min() to max().
    if "Helper" in dupes_of({"a.cpp": big, "b.cpp": lopsided}, 25):
        failures.append("a large/small pair was flagged; min() over the copies "
                        "is the documented rule")
    size_cases += 1

    if dupes_of({"a.cpp": big}, 25):
        failures.append("a single definition was reported as a duplicate")
    size_cases += 1

    # And the enumeration must actually have found the probe files, or all four
    # assertions above pass vacuously.
    if not dupes_of({"a.cpp": big, "b.cpp": big}, 1):
        failures.append("the walk fallback enumerated nothing, so every "
                        "size-threshold case above proves nothing")
    size_cases += 1

    # --- THE REINTRODUCED DUPLICATE, end to end (GitHub #275 criterion 2) ------
    #
    # The cases above exercise the matcher and the threshold on synthetic shapes.
    # This one is the criterion's actual ask: take the REAL shared helper as it now
    # exists, put a second copy of it in another file, and confirm the guard flags
    # it -- then confirm removing the copy clears it.
    #
    # Done by hand first, once, and the phase-5 karen gate failed the phase for
    # that: a hand-performed mutation proves the guard worked on one afternoon and
    # leaves nothing to repeat the check on a future regression.
    real_header = os.path.join(REPO, "tests", "support", "DirectoryAttacker.h")
    if os.path.isfile(real_header):
        body = io.open(real_header, encoding="utf-8", errors="replace").read()
        # Keep only from the struct onwards, so the probe files are a plausible
        # duplication rather than two copies of a header with include guards.
        at = body.find("struct DirectoryAttacker")
        struct_text = body[at:] if at >= 0 else body
        if "struct DirectoryAttacker" not in struct_text:
            failures.append(
                "could not locate `struct DirectoryAttacker` in the real "
                "tests/support/DirectoryAttacker.h, so the end-to-end "
                "reintroduction case below is not testing what it claims")
        else:
            with_dup = dupes_of({"copy_a.cpp": struct_text,
                                 "copy_b.cpp": struct_text}, MIN_BODY_LINES)
            if "DirectoryAttacker" not in with_dup:
                failures.append(
                    "reintroducing the REAL DirectoryAttacker in two files was NOT "
                    "flagged -- this is the exact defect GitHub #275 exists for, so "
                    "the guard is decoration")
            else:
                sizes = [n for _f, n in with_dup["DirectoryAttacker"]]
                if min(sizes) < MIN_BODY_LINES:
                    failures.append(
                        "the real helper was flagged but measured %r lines, below "
                        "the %d-line threshold -- the brace scan is mis-measuring it"
                        % (sizes, MIN_BODY_LINES))
            # And the converse: ONE copy is not a duplicate, however large.
            single = dupes_of({"copy_a.cpp": struct_text}, MIN_BODY_LINES)
            if "DirectoryAttacker" in single:
                failures.append(
                    "a SINGLE copy of the real helper was reported as a duplicate, "
                    "so the guard would be permanently red -- as useless as "
                    "permanently green")
        size_cases += 2

    # The column-0 convention this guard depends on must still hold in the real
    # tree, or the documented scope limit silently becomes total.
    support = os.path.join(REPO, "tests", "support")
    if os.path.isdir(support):
        found_any = False
        for dirpath, _dn, fns in os.walk(support):
            for fn in fns:
                if fn.endswith((".h", ".cpp", ".hpp")):
                    if definition_sizes(os.path.join(dirpath, fn)):
                        found_any = True
        if not found_any:
            failures.append(
                "no column-0 definition found anywhere in tests/support/, so "
                "the column-0 convention this guard relies on may no longer "
                "hold and a clean result would mean nothing")

    if failures:
        for f in failures:
            print("[dup-test-defs] SELF-TEST FAIL: " + f, file=sys.stderr)
        return 1
    print("[dup-test-defs] self-test: %d matcher case(s) + %d size-threshold "
          "case(s) passed, including the duplicated-TYPE shape the previous "
          "guard could not see, and the column-0 convention still holds in "
          "tests/support/" % (len(cases), size_cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
