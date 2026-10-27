#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lint_unc_gate.py -- census: every path-taking pdf_open_document is UNC-gated.

GitHub #92 (WIN-64). PDFE refuses UNC / network-scheme inputs before handing a
path to MuPDF, because opening a document from an attacker-controlled network
location turns a parser bug into a remotely-triggered one, and because
`\\\\host\\share` paths bypass assumptions the rest of the code makes about local
files.

That refusal is implemented site by site: each engine calls
`path_security::is_unc_or_remote(...)` and returns early before its
`pdf_open_document(ctx, path)`. Nothing enforced that pairing. A new engine --
or a new open in an existing one -- silently joins the product ungated, and the
existing tests would stay green because they test the engines that DO have it.

This is the same class as the fz_try/RAII sweep (phase-68): a convention that
holds today, with nothing to stop the next call site breaking it.

WHAT IS CHECKED

Every call to `pdf_open_document(` that takes a PATH must have an
`is_unc_or_remote` guard earlier in the same file, within a bounded window.

`pdf_open_document_with_stream(` is EXCLUDED and that is deliberate, not an
oversight: it takes an already-opened `fz_stream`, so there is no path for this
gate to inspect. Whoever opened the stream owns the check.

WHAT THIS DOES NOT CHECK

The window is a heuristic, not a scope analysis -- it cannot prove the guard
dominates the call on every control-flow path, only that a guard is present
nearby. It is a census, in the sense the issue asks for: it catches an
ungated NEW site, which is the failure mode that actually occurs. It would not
catch a guard placed in a branch that does not cover the open.

Stated plainly because a lint whose limits are unstated gets read as a proof.

GATED-BY-CALLER SITES (GitHub #184)

A site whose guard lives in ANOTHER FILE is invisible to a proximity window at
any width. `src/core/UnencryptedWrapperProbe.cpp` is one: its only non-test
caller, `MainWindow::openFile`, refuses UNC ~580 lines before it reaches the
probe. Such a site may carry

    // lint-unc-gate: gated-by-caller <symbol>
    // <justification>

which does not silence it. The lint re-derives every caller of `<symbol>` under
`src/` on each run and requires each calling file to hold an `is_unc_or_remote`
guard; a new ungated caller, a deleted guard, a rename that leaves the symbol
uncalled, or a missing justification all FAIL. The suppression is re-earned
every run rather than asserted once.

It still cannot prove the caller's guard dominates the call, which is the same
limit the window has.

Exit 0 = every site covered, 1 = at least one ungated site or a directive whose
claim no longer holds, 2 = usage error.
Run --self-test to check the decision functions themselves.
"""
import argparse
import io
import os
import re
import shutil
import sys
import tempfile

# How far back to look for the guard. Generous enough for the real sites (the
# widest is ~24 lines in BatchEngine, where the guard precedes a loop), tight
# enough that an unrelated guard elsewhere in a long file is not credited.
GUARD_WINDOW_LINES = 60

OPEN_CALL = re.compile(r"\bpdf_open_document\s*\(")
OPEN_STREAM_CALL = re.compile(r"\bpdf_open_document_with_stream\s*\(")
GUARD = re.compile(r"\bis_unc_or_remote\s*\(")


def find_sites(text):
    """Return [(lineno, line)] for path-taking pdf_open_document calls.

    Comments are skipped: several sites in this tree are DISCUSSED in comments
    right next to the real call ("Refuse UNC ... before pdf_open_document"), and
    counting those would both inflate the census and mask a real site by
    satisfying it with prose.
    """
    sites = []
    for i, line in enumerate(text.splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
            continue
        if OPEN_STREAM_CALL.search(line):
            continue          # no path to gate -- see module docstring
        if OPEN_CALL.search(line):
            sites.append((i, line.strip()))
    return sites


def is_guarded(text, lineno, window=GUARD_WINDOW_LINES):
    """True when an is_unc_or_remote guard appears within `window` lines above."""
    lines = text.splitlines()
    start = max(0, lineno - 1 - window)
    for line in lines[start:lineno - 1]:
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue          # a comment mentioning the guard is not the guard
        if GUARD.search(line):
            return True
    return False


def audit_file(path):
    with io.open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    out = []
    for lineno, line in find_sites(text):
        out.append((lineno, line, is_guarded(text, lineno)))
    return out


# --------------------------------------------------------------------------
# GATED-BY-CALLER directives (GitHub #184)
#
# THE PROBLEM. src/core/UnencryptedWrapperProbe.cpp (line 202 when GitHub #184
# was filed; 222 once the justification comment below it was written) opens a
# caller-supplied path and carries no guard of its own, so the proximity
# heuristic flagged it.
# It is nevertheless gated: its ONLY non-test caller is
# MainWindow::openFile (src/ui/MainWindow.cpp), which refuses UNC at line ~6000
# and returns before reaching the probe call ~580 lines later. The guard is in a
# DIFFERENT FILE, so no window width could ever have seen it — widening
# GUARD_WINDOW_LINES would not fix this, it would only blind the lint elsewhere.
#
# WHY NOT JUST ADD A GUARD IN THE PROBE. That was rejected as the cheap silence.
# The refusal would be dead code on every reachable path, and a dead branch that
# exists to quiet a linter teaches the next reader that the lint is noise.
#
# WHY NOT A BARE SUPPRESSION. A `// NOLINT` is an assertion nobody re-checks. The
# claim being made here is not "ignore this line", it is "every caller of this
# function gates first" — and THAT is mechanically checkable, so it is checked.
#
# A directive therefore does not silence the site. It REPLACES the proximity
# question with a stronger one:
#
#   // lint-unc-gate: gated-by-caller <symbol>
#   // <one or more lines of justification>
#
# The lint then enumerates EVERY call to <symbol> across src/ and requires each
# calling file to contain an is_unc_or_remote guard. If a second, ungated caller
# is added, or the existing caller's guard is deleted, or the symbol is renamed
# so nothing calls it any more, the directive FAILS — loudly, and naming which.
# Proximity is no longer this site's only signal; the caller set is.
#
# It still cannot prove the guard DOMINATES the call inside the caller, the same
# limit the window heuristic has. Stated here rather than left to be assumed.
# --------------------------------------------------------------------------

#: How far above the call to look for a directive. Comment blocks explaining a
#: call site in this tree run long; this covers them without reaching the
#: previous function.
DIRECTIVE_WINDOW_LINES = 40

DIRECTIVE = re.compile(
    r"lint-unc-gate\s*:\s*gated-by-caller\s+([A-Za-z_][A-Za-z0-9_]*)")


def find_directive(text, lineno, window=DIRECTIVE_WINDOW_LINES):
    """Return the symbol named by a gated-by-caller directive above `lineno`."""
    lines = text.splitlines()
    start = max(0, lineno - 1 - window)
    for line in lines[start:lineno - 1]:
        m = DIRECTIVE.search(line)
        if m:
            return m.group(1)
    return None


def directive_justification(text, lineno, window=DIRECTIVE_WINDOW_LINES):
    """The prose lines following the directive, up to the call.

    A directive with no justification is exactly the unreviewable suppression
    this mechanism exists to avoid, so an empty one is a failure.
    """
    lines = text.splitlines()
    start = max(0, lineno - 1 - window)
    prose, seen = [], False
    for line in lines[start:lineno - 1]:
        if DIRECTIVE.search(line):
            seen = True
            continue
        if not seen:
            continue
        stripped = line.lstrip()
        if stripped.startswith("//"):
            body = stripped[2:].strip()
            if body:
                prose.append(body)
    return prose


def callers_of(root, symbol):
    """Files under src/ that CALL `symbol` (definitions excluded).

    A definition is `Type sym(` at column 0 or after a return type on its own
    line; a call is `sym(` anywhere else. Rather than parse C++, every
    occurrence is returned here and `verify_directive` separates the two: in
    the defining file it exempts the DEFINITION lines only, and holds every
    other occurrence -- including a call added alongside the definition -- to
    the same guard requirement as an external caller.
    """
    call = re.compile(r"\b%s\s*\(" % re.escape(symbol))
    hits = {}
    for src in cpp_sources(root):
        with io.open(src, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        lines = []
        for i, line in enumerate(text.splitlines(), start=1):
            stripped = line.lstrip()
            if stripped.startswith(("//", "*", "/*")):
                continue
            if call.search(line):
                lines.append(i)
        if lines:
            rel = os.path.relpath(src, root).replace("\\", "/")
            hits[rel] = (lines, text)
    return hits


#: A function DEFINITION in this tree starts at column 0 -- `void
#: MainWindow::openFile(const QString&)`, `UnencryptedWrapperInfo
#: probeUnencryptedWrapper(`. Anything indented is a statement inside one.
#: Preprocessor lines, closing braces, comments and namespace/class openers are
#: excluded so they cannot be mistaken for a definition boundary.
FUNC_DEF_LINE = re.compile(r"^[A-Za-z_][^;]*\(")
_NOT_A_DEF_PREFIX = ("#", "}", "//", "/*", "*", "using ", "namespace ",
                     "class ", "struct ", "return ", "typedef ", "extern \"C\"")


def enclosing_function_start(text, lineno):
    """First line of the function definition containing `lineno` (1-based).

    An approximation, and deliberately a conservative one: if no definition
    boundary is found the whole file above the call is searched, which can only
    make the guard check MORE permissive, never less -- and the caller-set
    check is what carries the strength here.
    """
    lines = text.splitlines()
    for i in range(min(lineno, len(lines)) - 1, -1, -1):
        line = lines[i]
        if not line or line[0].isspace():
            continue
        if line.startswith(_NOT_A_DEF_PREFIX):
            continue
        if FUNC_DEF_LINE.match(line):
            return i + 1
    return 1


def caller_guards_call(text, call_line):
    """True when an is_unc_or_remote guard precedes `call_line` in ITS FUNCTION.

    WHY NOT 'anywhere in the file'. That was the first implementation and a
    mutation killed it on the spot: MainWindow.cpp holds THREE unrelated
    is_unc_or_remote guards (drag-drop filtering, launch actions), so deleting
    the one in openFile -- the only one that actually gates the probe -- left
    the directive reporting PASS. A file-wide search credits a guard that
    protects something else entirely.
    """
    start = enclosing_function_start(text, call_line)
    lines = text.splitlines()
    for line in lines[start - 1:call_line - 1]:
        stripped = line.lstrip()
        if stripped.startswith(("//", "*", "/*")):
            continue          # prose about the guard is not the guard
        if GUARD.search(line):
            return True
    return False


def verify_directive(root, symbol, site_rel):
    """Check the claim the directive makes. Returns a list of problem strings."""
    problems = []
    hits = callers_of(root, symbol)
    # The defining file itself is not an EXTERNAL caller -- it is the site
    # under review -- so it does not satisfy the "someone calls this" liveness
    # check below.
    external = {rel: v for rel, v in hits.items() if rel != site_rel}

    # ...but it is NOT exempt from the guard requirement. GitHub #184 /
    # p87 phase-6: a mutation that added a second, UNGATED caller INSIDE
    # src/core/UnencryptedWrapperProbe.cpp left this lint reporting PASS,
    # because the whole file was dropped. `callers_of`'s own docstring already
    # claimed the narrower behaviour ("excludes the file ... only if it
    # contains no other call"), so the code and its documentation had drifted
    # apart -- and the drift was in the permissive direction.
    #
    # The exemption is therefore narrowed to what it was always meant to be:
    # the DEFINITION lines of the symbol, which are not calls. Every other
    # occurrence in the defining file is a real call and must gate like any
    # other.
    if site_rel in hits:
        lines, text = hits[site_rel]
        src_lines = text.splitlines()
        internal_calls = []
        for n in lines:
            raw = src_lines[n - 1] if n - 1 < len(src_lines) else ""
            if (raw
                    and not raw[0].isspace()
                    and not raw.startswith(_NOT_A_DEF_PREFIX)
                    and FUNC_DEF_LINE.match(raw)):
                continue      # the definition (or an overload's) -- not a call
            internal_calls.append(n)
        unguarded_internal = [n for n in internal_calls
                              if not caller_guards_call(text, n)]
        if unguarded_internal:
            problems.append(
                "%s itself calls %s() at line(s) %s with NO is_unc_or_remote "
                "guard earlier in the SAME FUNCTION. A caller added to the "
                "defining file is still a caller; the directive's claim is "
                "false for this call."
                % (site_rel, symbol,
                   ", ".join(str(n) for n in unguarded_internal)))

    if not external:
        problems.append(
            "no file under src/ outside %s calls %s(). The directive claims "
            "callers gate this site, but there are no callers -- either the "
            "symbol was renamed, or the site is now unreachable and the "
            "directive is stale. Re-check it rather than trusting it."
            % (site_rel, symbol))
        return problems

    for rel, (lines, text) in sorted(external.items()):
        unguarded = [n for n in lines if not caller_guards_call(text, n)]
        if unguarded:
            problems.append(
                "%s calls %s() at line(s) %s with NO is_unc_or_remote guard "
                "earlier in the SAME FUNCTION. The directive's claim -- that "
                "every caller gates first -- is false for this call."
                % (rel, symbol, ", ".join(str(n) for n in unguarded)))
    return problems


def cpp_sources(root):
    found = []
    for base, dirs, files in os.walk(os.path.join(root, "src")):
        dirs[:] = [d for d in dirs if d not in (".git", "build")]
        for f in files:
            if f.endswith((".cpp", ".cc", ".cxx")):
                found.append(os.path.join(base, f))
    return sorted(found)


# --------------------------------------------------------------------------
# Self-tests. The lint must be able to go RED and to stay GREEN.
# --------------------------------------------------------------------------
GUARDED = """
void run(const std::filesystem::path& input) {
    if (pdfe::core::path_security::is_unc_or_remote(input.string())) {
        return;
    }
    pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
}
"""

UNGUARDED = """
void run(const std::filesystem::path& input) {
    pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
}
"""

COMMENT_ONLY_GUARD = """
void run(const std::filesystem::path& input) {
    // Refuse UNC / network-scheme inputs -- is_unc_or_remote(input) applies here
    pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
}
"""

STREAM_FORM = """
void run(fz_stream* stm) {
    pdf_document* fdf = pdf_open_document_with_stream(ctx, stm);
}
"""

COMMENT_MENTIONS_OPEN = """
// pdf_open_document() opens `input` by name, and pdf_save_document() ...
void unrelated() {}
"""

DIRECTIVE_SITE = """
void probeThing(Ctx* ctx, const std::string& path) {
    // lint-unc-gate: gated-by-caller probeThing
    // Gated by MainWindow::openFile, which refuses UNC first.
    pdf_document* doc = pdf_open_document(ctx, path.c_str());
}
"""

DIRECTIVE_NO_JUSTIFICATION = """
void probeThing(Ctx* ctx, const std::string& path) {
    // lint-unc-gate: gated-by-caller probeThing
    pdf_document* doc = pdf_open_document(ctx, path.c_str());
}
"""


def self_test():
    failures = 0

    def check(label, got, want):
        nonlocal failures
        ok = (got == want)
        if not ok:
            failures += 1
        print("  [%s] %-46s got=%r" % ("ok  " if ok else "FAIL", label, got))

    def verdicts(src):
        return [g for (_ln, _l, g) in
                [(ln, l, is_guarded(src, ln)) for (ln, l) in find_sites(src)]]

    check("guarded site passes", verdicts(GUARDED), [True])
    check("unguarded site FAILS", verdicts(UNGUARDED), [False])
    # The important one: prose is not a guard.
    check("comment-only guard FAILS", verdicts(COMMENT_ONLY_GUARD), [False])
    # And the exclusions must not be counted at all.
    check("stream form is not a site", find_sites(STREAM_FORM), [])
    check("comment mentioning the call is not a site",
          find_sites(COMMENT_MENTIONS_OPEN), [])

    # ---- gated-by-caller directives (GitHub #184) -----------------------
    # Every one of these is asserted in BOTH directions. A directive check
    # that can only ever redden would block every legitimate site; one that
    # can only ever green is the unreviewable suppression it replaced.
    ln = find_sites(DIRECTIVE_SITE)[0][0]
    check("directive is found above the call",
          find_directive(DIRECTIVE_SITE, ln), "probeThing")
    check("no directive above a bare call",
          find_directive(UNGUARDED, find_sites(UNGUARDED)[0][0]), None)
    check("justification prose is collected",
          directive_justification(DIRECTIVE_SITE, ln),
          ["Gated by MainWindow::openFile, which refuses UNC first."])
    ln_nj = find_sites(DIRECTIVE_NO_JUSTIFICATION)[0][0]
    check("a directive with no prose collects nothing",
          directive_justification(DIRECTIVE_NO_JUSTIFICATION, ln_nj), [])

    tmp = tempfile.mkdtemp(prefix="unc_lint_selftest_")
    try:
        def tree(callers):
            root = tempfile.mkdtemp(dir=tmp)
            srcdir = os.path.join(root, "src", "core")
            os.makedirs(srcdir)
            with io.open(os.path.join(srcdir, "Probe.cpp"), "w",
                         encoding="utf-8", newline="\n") as fh:
                fh.write(DIRECTIVE_SITE)
            for name, body in callers.items():
                with io.open(os.path.join(srcdir, name), "w",
                             encoding="utf-8", newline="\n") as fh:
                    fh.write(body)
            return root

        gating = ("void open(const std::string& p) {\n"
                  "    if (path_security::is_unc_or_remote(p)) return;\n"
                  "    probeThing(ctx, p);\n"
                  "}\n")
        not_gating = ("void open2(const std::string& p) {\n"
                      "    probeThing(ctx, p);\n"
                      "}\n")
        prose_only = ("// is_unc_or_remote(p) is applied by our caller, honest\n"
                      "void open3(const std::string& p) {\n"
                      "    probeThing(ctx, p);\n"
                      "}\n")

        r = tree({"Caller.cpp": gating})
        check("a gating caller satisfies the directive",
              verify_directive(r, "probeThing", "src/core/Probe.cpp"), [])

        r = tree({})
        got = verify_directive(r, "probeThing", "src/core/Probe.cpp")
        check("no caller at all FAILS the directive", len(got), 1)

        r = tree({"Caller.cpp": gating, "Other.cpp": not_gating})
        got = verify_directive(r, "probeThing", "src/core/Probe.cpp")
        check("an UNGATED second caller FAILS the directive", len(got), 1)
        check("and the failure names that caller",
              bool(got and "src/core/Other.cpp" in got[0]), True)

        r = tree({"Caller.cpp": gating, "Prose.cpp": prose_only})
        got = verify_directive(r, "probeThing", "src/core/Probe.cpp")
        check("a caller whose only 'guard' is a COMMENT FAILS", len(got), 1)

        # THE MUTATION THAT KILLED THE FIRST IMPLEMENTATION. This check
        # originally asked "does the calling FILE contain is_unc_or_remote
        # anywhere". MainWindow.cpp holds three unrelated guards, so deleting
        # the one in openFile -- the only one gating the probe -- still
        # reported PASS. Scoping to the enclosing function is what fixed it,
        # and this case is what stops it regressing.
        elsewhere = ("void somethingElse(const std::string& q) {\n"
                     "    if (path_security::is_unc_or_remote(q)) return;\n"
                     "}\n"
                     "void open4(const std::string& p) {\n"
                     "    probeThing(ctx, p);\n"
                     "}\n")
        r = tree({"Elsewhere.cpp": elsewhere})
        got = verify_directive(r, "probeThing", "src/core/Probe.cpp")
        check("a guard in a DIFFERENT function does not count", len(got), 1)
        check("caller_guards_call: guarded call in-function is True",
              caller_guards_call(gating, 3), True)
        check("caller_guards_call: call in an unguarded function is False",
              caller_guards_call(elsewhere, 5), False)

        # ---- the SAME-FILE caller (GitHub #184, p87 phase-6) -------------
        # verify_directive used to drop the defining file wholesale, so an
        # ungated caller added NEXT TO the directive was invisible. A live
        # mutation against the real tree confirmed it: appending an ungated
        # p87ProbeCaller() to src/core/UnencryptedWrapperProbe.cpp left the
        # lint reporting PASS. Both directions are pinned here, because the
        # narrowing must not start counting the DEFINITION as an unguarded
        # call -- that would make the lint permanently red on every directive
        # site, which is the other way this check stops being read.
        def tree_with_probe(probe_src, callers):
            root = tempfile.mkdtemp(dir=tmp)
            srcdir = os.path.join(root, "src", "core")
            os.makedirs(srcdir)
            with io.open(os.path.join(srcdir, "Probe.cpp"), "w",
                         encoding="utf-8", newline="\n") as fh:
                fh.write(probe_src)
            for name, body in callers.items():
                with io.open(os.path.join(srcdir, name), "w",
                             encoding="utf-8", newline="\n") as fh:
                    fh.write(body)
            return root

        r = tree_with_probe(DIRECTIVE_SITE, {"Caller.cpp": gating})
        check("the definition itself is not counted as an ungated caller",
              verify_directive(r, "probeThing", "src/core/Probe.cpp"), [])

        same_file_ungated = DIRECTIVE_SITE + (
            "\nvoid alsoHere(Ctx* ctx, const std::string& p) {\n"
            "    probeThing(ctx, p);\n"
            "}\n")
        r = tree_with_probe(same_file_ungated, {"Caller.cpp": gating})
        got = verify_directive(r, "probeThing", "src/core/Probe.cpp")
        check("an UNGATED caller in the DEFINING FILE FAILS", len(got), 1)
        check("and the failure says it is the defining file",
              bool(got and "itself calls" in got[0]), True)

        same_file_gated = DIRECTIVE_SITE + (
            "\nvoid alsoHere(Ctx* ctx, const std::string& p) {\n"
            "    if (path_security::is_unc_or_remote(p)) return;\n"
            "    probeThing(ctx, p);\n"
            "}\n")
        r = tree_with_probe(same_file_gated, {"Caller.cpp": gating})
        check("a GATED caller in the defining file is accepted",
              verify_directive(r, "probeThing", "src/core/Probe.cpp"), [])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("PASS" if failures == 0 else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--self-test", action="store_true",
                    help="test the decision functions; reads no sources")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    total = 0
    ungated = []
    directed = []          # (rel, lineno, symbol) -- claim verified below
    bad_directives = []    # (rel, lineno, problem)
    for src in cpp_sources(args.root):
        with io.open(src, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        rel = os.path.relpath(src, args.root).replace("\\", "/")
        for lineno, line, guarded in audit_file(src):
            total += 1
            if guarded:
                continue
            symbol = find_directive(text, lineno)
            if symbol is None:
                ungated.append((rel, lineno, line))
                continue
            if not directive_justification(text, lineno):
                bad_directives.append((
                    rel, lineno,
                    "the gated-by-caller directive for %s() carries NO "
                    "justification prose. An unexplained suppression is the "
                    "thing this mechanism exists to replace." % symbol))
                continue
            for problem in verify_directive(args.root, symbol, rel):
                bad_directives.append((rel, lineno, problem))
            directed.append((rel, lineno, symbol))

    if total == 0:
        print("lint_unc_gate: FAIL -- no pdf_open_document call sites found at "
              "all. The census is looking in the wrong place, or the call was "
              "renamed; either way this lint is now inert and must be fixed "
              "rather than trusted.", file=sys.stderr)
        return 1

    if bad_directives:
        print("lint_unc_gate: FAIL -- %d gated-by-caller directive problem(s). "
              "A directive that no longer holds is WORSE than no directive: it "
              "reads as a reviewed exemption while exempting an ungated open."
              % len(bad_directives), file=sys.stderr)
        for rel, lineno, problem in bad_directives:
            print("  %s:%d\n      %s" % (rel, lineno, problem), file=sys.stderr)
        return 1

    if ungated:
        print("lint_unc_gate: FAIL -- %d of %d pdf_open_document site(s) have no "
              "is_unc_or_remote guard within %d lines and no verified "
              "gated-by-caller directive:"
              % (len(ungated), total, GUARD_WINDOW_LINES))
        for rel, lineno, line in ungated:
            print("  %s:%d\n      %s" % (rel, lineno, line))
        return 1

    print("lint_unc_gate: PASS -- all %d path-taking pdf_open_document site(s) "
          "are gated: %d by an is_unc_or_remote guard within %d lines, %d by a "
          "VERIFIED gated-by-caller directive (every caller re-checked this "
          "run, not taken on trust)."
          % (total, total - len(directed), GUARD_WINDOW_LINES, len(directed)))
    for rel, lineno, symbol in directed:
        print("    %s:%d  gated-by-caller %s()" % (rel, lineno, symbol))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
