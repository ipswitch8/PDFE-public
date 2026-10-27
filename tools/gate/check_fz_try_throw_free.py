# -*- coding: utf-8 -*-
"""No C++ `throw` may appear inside an `fz_try` block (GitHub #287).

WHY THIS GUARD EXISTS
=====================
`fz_try` pushes a MuPDF error frame; `fz_catch` pops it. MuPDF's error handling
is setjmp/longjmp, so a C++ exception thrown inside the `fz_try` block
propagates straight past `fz_catch` and the frame is **never popped**. fitz does
not complain at the leak site. It complains when the context is dropped or when
a thread holding a cloned context exits:

    Assertion failed: ctx->error.top == ctx->error.stack_base,
        file source/fitz/context.c, line 218

and that aborts the process. GitHub #287 was exactly this, and it presented as
an "intermittent race in TileRendererTest.CancelPage" -- a test that merely
happened to own a QThreadPool whose worker exit triggered the audit of someone
else's leaked frame. An aborted shard writes no XML, so all 408 tests assigned
to it were recorded as errors.

WHY A GUARD AND NOT JUST THE FIX
================================
#287's fix made the two SHARED primitives leak-proof --
`MuPdfContext::execute` and `MuPdfContext::executeReturn` now capture an
escaping C++ exception, let the `fz_try` block exit normally so `fz_catch` runs,
and rethrow afterwards. That closes the frame those two push, which is the frame
every one of the dozens of `execute(...)` call sites unwinds through.

It does **not** close the class. Measured 2026-10-15 by this guard: **148**
braced `fz_try`/`fz_always` bodies across **37** files under `src/`, of which
only **3** are in the two shared primitives. The other 145 are raw blocks, most
nested *inside* an `execute()` lambda, and each pushes its own frame. A `throw`
inside one of those leaks the INNER frame; the outer primitive then catches the
exception and pops its own frame correctly, so the fix makes the symptom rarer
without making it impossible. A fix that closes one door while the class keeps a
second one is this project's most recurrent shape, and the rule it leaves behind
-- "do not throw in here" -- is a convention every future edit must remember,
not a mechanism.

(The figures are the guard's own, not a grep's. `grep -c fz_try` reports ~200
across 56 files, but that counts prose mentions in comments -- there are many,
because this hazard has been written about repeatedly -- and unbraced
single-statement bodies. A hit is not a block.)

So: enumerate by the PROPERTY (a throw lexically inside an fz_try block), not by
the symptom (whichever test aborted last).

CURRENT STATE: 148 blocks, **0 findings**. The #118 "fz_try/RAII hazard
burn-down" audit holds. This guard's job is to keep it holding without anyone
having to remember why.

WHAT IT DOES NOT CATCH, STATED PLAINLY
======================================
This checks for a LEXICAL `throw`. It does not catch a frame leaked by a
function CALLED from inside the block which throws internally -- that needs
whole-program analysis, and the same objection applies to the hand-audit this
replaces. It is a strict improvement on "we read it once and believed it", and
the one shape it does catch is the one that is unambiguously wrong and that a
future edit is most likely to introduce.

`throw;` (a bare rethrow inside a `catch` nested in the block) is also reported:
it unwinds just as far.

Usage:
    python tools/gate/check_fz_try_throw_free.py [--self-test] [paths...]
Exit 0 clean, 1 on a finding, 2 on a usage/parse error.
"""
from __future__ import annotations

import argparse
import io
import pathlib
import re
import sys

# tools/gate/<this file> -> parents[0]=gate, [1]=tools, [2]=repo root.
REPO = pathlib.Path(__file__).resolve().parents[2]
assert (REPO / ".github" / "workflows").is_dir(), (
    "repo root misresolved to %r" % (REPO,))
DEFAULT_ROOTS = ("src",)
SUFFIXES = (".cpp", ".h", ".hpp", ".cc")

#: `fz_try(` / `fz_always(` / `fz_catch(` all push or ride the same frame, but
#: only the fz_try BODY is the hazard: fz_catch's body runs after the frame has
#: been popped, and fz_always' body runs with it still pushed. Both fz_try and
#: fz_always bodies are therefore scanned; fz_catch is not.
_BLOCK_OPENERS = ("fz_try", "fz_always")

_OPENER_RE = re.compile(r"\bfz_(try|always)\s*\(")

# A `throw` that is a STATEMENT, not part of an identifier or a comment word.
# `throw;` (bare rethrow) and `throw X(...)` both match. `dynamic_cast` style
# false friends like `rethrow_exception` do not, because of the word boundary
# plus the required following `;`, whitespace or `(`.
_THROW_RE = re.compile(r"(^|[^\w:])throw\s*(;|[\w(:\"'])")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string/char literals, preserving offsets.

    Offsets must be preserved because every finding is reported as a line
    number in the ORIGINAL file. Replacing a comment with nothing would shift
    every subsequent line and make the report point at innocent code -- a
    citation that invalidates itself.
    """
    out = list(text)
    i, n = 0, len(text)
    state = None  # None | 'line' | 'block' | 'str' | 'chr'
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if c == '"':
                state = "str"
                out[i] = " "
                i += 1
                continue
            if c == "'":
                state = "chr"
                out[i] = " "
                i += 1
                continue
            i += 1
            continue

        # Inside something: blank it (but keep newlines, so line numbers hold).
        if state == "line":
            if c == "\n":
                state = None
            else:
                out[i] = " "
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = None
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue
        # string / char literal
        if c == "\\":
            out[i] = " "
            if i + 1 < n and text[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
            continue
        if (state == "str" and c == '"') or (state == "chr" and c == "'"):
            out[i] = " "
            state = None
            i += 1
            continue
        if c != "\n":
            out[i] = " "
        i += 1
    return "".join(out)


def _match_paren(s: str, open_idx: int) -> int:
    """Index just past the `)` matching the `(` at ``open_idx``, or -1."""
    depth = 0
    for i in range(open_idx, len(s)):
        if s[i] == "(":
            depth += 1
        elif s[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


def _match_brace(s: str, open_idx: int) -> int:
    """Index of the `}` matching the `{` at ``open_idx``, or -1."""
    depth = 0
    for i in range(open_idx, len(s)):
        if s[i] == "{":
            depth += 1
        elif s[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def fz_try_bodies(text: str) -> list:
    """Every (opener, body_start, body_end) for fz_try/fz_always in ``text``.

    ``text`` must already be comment/string-stripped. Bodies are found by real
    brace matching, not by a regex: a grep for `throw` near `fz_try` would both
    miss throws further down a long block and fire on throws after the block
    ended. Nested fz_try blocks each appear in their own right, which is the
    point -- the inner frame is the one the #287 fix does not cover.
    """
    out = []
    for m in _OPENER_RE.finditer(text):
        opener = "fz_" + m.group(1)
        close_paren = _match_paren(text, m.end() - 1)
        if close_paren < 0:
            continue
        # The body must be a braced block. MuPDF's macros are usable without
        # braces for a single statement; such a body cannot contain a `throw`
        # statement worth reporting separately from the next one, and treating
        # it as a block would swallow the rest of the function.
        j = close_paren
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if j >= len(text) or text[j] != "{":
            continue
        end = _match_brace(text, j)
        if end < 0:
            continue
        out.append((opener, j + 1, end))
    return out


def scan_text(text: str, path: str = "<text>") -> list:
    """Findings as dicts: path, line, opener, depth, snippet.

    ONE FINDING PER `throw`, not one per enclosing block. A throw inside a
    nested fz_try is lexically inside the inner body AND the outer one, and the
    first version of this reported it twice -- two lines of output, same line of
    code, reading as two defects. It is one defect: the exception unwinds
    through both frames, but there is a single thing to fix at a single
    location.

    The finding is attributed to the INNERMOST enclosing block, because that is
    the frame that leaks first and the tightest true citation. ``depth`` records
    how many fz_try/fz_always bodies enclose it, so the nesting is still
    visible without being double-counted.
    """
    stripped = strip_comments_and_strings(text)
    bodies = fz_try_bodies(stripped)
    lines = text.splitlines()

    # throw offset -> list of enclosing bodies
    enclosing: dict = {}
    for opener, start, end in bodies:
        for tm in _THROW_RE.finditer(stripped, start, end):
            at = tm.start(0)
            # Skip the leading non-word char the regex consumed.
            while at < len(stripped) and not stripped.startswith("throw", at):
                at += 1
            enclosing.setdefault(at, []).append((opener, start, end))

    findings = []
    for at in sorted(enclosing):
        # Innermost = the shortest enclosing body.
        opener, start, end = min(enclosing[at], key=lambda b: b[2] - b[1])
        line = text.count("\n", 0, at) + 1
        snippet = lines[line - 1].strip() if line - 1 < len(lines) else ""
        findings.append({"path": path, "line": line, "opener": opener,
                         "depth": len(enclosing[at]), "snippet": snippet[:160]})
    return findings


def iter_sources(paths):
    for p in paths:
        p = pathlib.Path(p)
        if p.is_file():
            yield p
        elif p.is_dir():
            for sub in sorted(p.rglob("*")):
                if sub.is_file() and sub.suffix in SUFFIXES:
                    yield sub


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", help="files or dirs (default: src/)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    roots = args.paths or [str(REPO / r) for r in DEFAULT_ROOTS]
    files = list(iter_sources(roots))
    if not files:
        print("ERROR: no source files found under %r -- a guard that examines "
              "nothing reports clean. Check the paths." % (roots,),
              file=sys.stderr)
        return 2

    findings, blocks = [], 0
    for f in files:
        try:
            text = io.open(f, encoding="utf-8", errors="replace").read()
        except OSError as exc:
            print("ERROR: cannot read %s: %s" % (f, exc), file=sys.stderr)
            return 2
        if "fz_try" not in text and "fz_always" not in text:
            continue
        blocks += len(fz_try_bodies(strip_comments_and_strings(text)))
        try:
            rel = f.relative_to(REPO).as_posix()
        except ValueError:
            rel = f.as_posix()
        findings.extend(scan_text(text, rel))

    if blocks == 0:
        print("ERROR: scanned %d file(s) and matched ZERO fz_try/fz_always "
              "blocks. The codebase has ~200, so the block parser is broken "
              "and this guard is vacuous." % len(files), file=sys.stderr)
        return 2

    print("fz_try/fz_always blocks scanned: %d across %d file(s)"
          % (blocks, len(files)))
    if findings:
        print("\nFINDINGS -- a C++ `throw` inside an fz_try/fz_always body "
              "leaks the MuPDF error frame (GitHub #287):\n")
        for f in findings:
            print("  %s:%d  [%s]  %s" % (f["path"], f["line"], f["opener"],
                                         f["snippet"]))
        print("\nUse fz_throw(ctx, FZ_ERROR_ARGUMENT, ...) instead, or move the "
              "throw after the fz_catch. See the note on "
              "MuPdfContext::execute.")
        return 1
    print("findings: 0")
    return 0


def _self_test() -> int:
    cases = [
        # (name, source, expected finding count)
        ("plain throw inside fz_try", """
            void f() { fz_try(ctx) { throw std::runtime_error("x"); } fz_catch(ctx) {} }
        """, 1),
        ("bare rethrow inside fz_try", """
            void f() { fz_try(ctx) { try { g(); } catch (...) { throw; } } fz_catch(ctx) {} }
        """, 1),
        ("throw AFTER the block is fine", """
            void f() { fz_try(ctx) { g(); } fz_catch(ctx) {} throw std::bad_alloc(); }
        """, 0),
        ("throw in fz_catch body is fine -- frame already popped", """
            void f() { fz_try(ctx) { g(); } fz_catch(ctx) { throw MuPdfException("x"); } }
        """, 0),
        ("fz_always body IS scanned -- frame still pushed", """
            void f() { fz_try(ctx) { g(); } fz_always(ctx) { throw X(); } fz_catch(ctx) {} }
        """, 1),
        ("throw inside a COMMENT is not a throw", """
            void f() { fz_try(ctx) { /* never throw here */ g(); } fz_catch(ctx) {} }
        """, 0),
        ("throw inside a STRING is not a throw", """
            void f() { fz_try(ctx) { log("do not throw x"); } fz_catch(ctx) {} }
        """, 0),
        # ONE finding, not one per enclosing frame. The first version of this
        # guard reported this throw twice -- same line, read as two defects.
        ("nested fz_try -- one finding, attributed innermost", """
            void f() { fz_try(ctx) { fz_try(ctx) { throw X(); } fz_catch(ctx) {} } fz_catch(ctx) {} }
        """, 1),
        ("fz_throw is NOT a C++ throw", """
            void f() { fz_try(ctx) { fz_throw(ctx, FZ_ERROR_ARGUMENT, "x"); } fz_catch(ctx) {} }
        """, 0),
        ("rethrow_exception is not a bare throw", """
            void f() { fz_try(ctx) { g(); } fz_catch(ctx) {} std::rethrow_exception(e); }
        """, 0),
        ("two throws in one body are two findings", """
            void f() { fz_try(ctx) { if (a) throw X(); else throw Y(); } fz_catch(ctx) {} }
        """, 2),
        ("unbraced fz_try body is skipped, not mis-parsed", """
            void f() { fz_try(ctx) g(); fz_catch(ctx) {} throw X(); }
        """, 0),
        ("throw deep in a long body is still found", """
            void f() { fz_try(ctx) { a(); b(); c(); d(); e(); { h(); } throw X(); } fz_catch(ctx) {} }
        """, 1),
        ("line number points at the throw", """void f() {
fz_try(ctx) {
    throw X();
}
fz_catch(ctx) {}
}""", 1),
    ]
    failures = 0
    for name, src, expected in cases:
        got = scan_text(src, name)
        if len(got) != expected:
            failures += 1
            print("FAIL %-55s expected %d, got %d %r"
                  % (name, expected, len(got), got))
        else:
            print("ok   %-55s %d" % (name, expected))

    # The line-number case, checked specifically: a finding that cites the
    # wrong line sends the reader to innocent code.
    linecase = """void f() {
fz_try(ctx) {
    throw X();
}
fz_catch(ctx) {}
}"""
    got = scan_text(linecase, "lines")
    if not got or got[0]["line"] != 3:
        failures += 1
        print("FAIL line attribution: expected line 3, got %r"
              % ([g["line"] for g in got],))
    else:
        print("ok   line attribution -> 3")

    # The nested case specifically: one finding, and the nesting still visible.
    nested = ("void f() { fz_try(ctx) { fz_try(ctx) { throw X(); } "
              "fz_catch(ctx) {} } fz_catch(ctx) {} }")
    got = scan_text(nested, "nested")
    if len(got) != 1 or got[0]["depth"] != 2:
        failures += 1
        print("FAIL nested dedupe: expected 1 finding at depth 2, got %r"
              % ([(g["line"], g["depth"]) for g in got],))
    else:
        print("ok   nested dedupe -> 1 finding, depth 2")

    # Non-vacuity of the stripper: it must preserve offsets exactly.
    s = "ab/*xx*/cd\n//yy\nef"
    st = strip_comments_and_strings(s)
    if len(st) != len(s) or st.count("\n") != s.count("\n"):
        failures += 1
        print("FAIL stripper changed length/newlines: %r -> %r" % (s, st))
    else:
        print("ok   stripper preserves offsets and newlines")

    print("\nself-test: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
