#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Self-test for tools/gate/page_load_guard_scan.py (GitHub #176, F14/phase-6).

WHY FIXTURES, NOT A LIVE src/ RUN, ARE THE PROOF HERE
--------------------------------------------------------
This file's job is to prove the SCANNER's mechanism is sound: that it tells a
call from a declaration, that it follows a macro-expanded call, and that it
follows a call through a function pointer within one translation unit. Those
are properties of the tool, not of this project's current source tree — the
real-`src/` run (see the phase-6 report) additionally answers whether either
shape EXISTS today, which is a different question with a different (and
currently negative) answer.

Every fixture below is deliberately self-contained (forward-declares its own
`fz_context`/`fz_document`/`fz_page` stand-ins) so this test needs nothing
from the real MuPDF headers or the project's own compile_commands.json — only
libclang itself. `--self-test` doubles as a pytest-collectable module (see
`test_*` functions at the bottom) and as a standalone CLI entry point, the
same shape `fz_try_raii_scan.py --self-test` already uses.

REGISTRATION. This needs libclang, which needs a C++ front end available to
libclang at parse time -- the same dependency `fz_try_raii_scan.py` carries.
`fast-checks.yml` runs on `ubuntu-latest` specifically BECAUSE it wants "no
C++ toolchain" (see that workflow's own header comment), so this test belongs
where `fz_try_raii_scan.py`'s own scan step lives: `windows-validation.yml`,
on the self-hosted Windows runner that already has clang-cl and libclang
installed for that step.
"""

from __future__ import annotations

import io
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import clang.cindex as ci
except ImportError:  # pragma: no cover - reported, never silently skipped
    sys.stderr.write(
        "test_page_load_guard_scan: the 'libclang' python package is not "
        "installed.\n                            install it with:  "
        "python -m pip install libclang\n")
    raise

import page_load_guard_scan as pg  # noqa: E402

CursorKind = ci.CursorKind

_TYPES = (
    'extern "C" {\n'
    "typedef struct fz_context fz_context;\n"
    "typedef struct fz_document fz_document;\n"
    "typedef struct fz_page fz_page;\n"
    "}\n"
)

# ---- fixture 1: an ordinary direct call, alongside its own declaration -----
# The declaration line ALSO contains the literal substring "fz_load_page(",
# which is precisely the shape that once padded the lexical scanner's
# guarded-call-site floor by two (see FormXObjectDepthGuard.h / the .cpp
# test's kGuardHeader carve-out). Here there is no filename special case at
# all -- the AST scanner must get this right structurally, by node kind, not
# by knowing which file it is looking at.
FIXTURE_DIRECT = _TYPES + (
    "extern \"C\" fz_page* fz_load_page(fz_context*, fz_document*, int);\n"
    "\n"
    "fz_page* caller(fz_context* ctx, fz_document* doc) {\n"
    "    fz_page* p = fz_load_page(ctx, doc, 0);\n"
    "    return p;\n"
    "}\n"
)

# ---- fixture 2: a macro-wrapped call, hidden from ANY substring/regex scan -
# Token-pasting (##) means the literal text "fz_load_page(" never appears in
# this file at all -- not even inside the macro body -- so a lexical scanner
# cannot find it by construction, not merely by bad luck. The preprocessor
# and the AST both resolve PDFE_PASTE(fz_load, _page) to the real function.
FIXTURE_MACRO = _TYPES + (
    "extern \"C\" fz_page* fz_load_page(fz_context*, fz_document*, int);\n"
    "\n"
    "#define PDFE_PASTE2(a, b) a##b\n"
    "#define PDFE_PASTE(a, b) PDFE_PASTE2(a, b)\n"
    "#define LOAD_PAGE_MACRO(c, d, i) PDFE_PASTE(fz_load, _page)(c, d, i)\n"
    "\n"
    "fz_page* caller(fz_context* ctx, fz_document* doc) {\n"
    "    fz_page* p = LOAD_PAGE_MACRO(ctx, doc, 1);\n"
    "    return p;\n"
    "}\n"
)

# ---- fixture 3: a call through a function pointer ---------------------
# `pfn = fz_load_page;` never puts "fz_load_page(" in the text (no trailing
# paren -- it is a bare reference, decaying to a pointer), and the actual
# call site `pfn(ctx, doc, 2)` mentions no loader name at all. Both are
# structurally invisible to a substring scan; an AST that resolves `pfn`'s
# declaration back to `fz_load_page` is the only way to attribute the call.
FIXTURE_INDIRECT = _TYPES + (
    "extern \"C\" fz_page* fz_load_page(fz_context*, fz_document*, int);\n"
    "typedef fz_page* (*LoaderFn)(fz_context*, fz_document*, int);\n"
    "\n"
    "fz_page* caller(fz_context* ctx, fz_document* doc) {\n"
    "    LoaderFn pfn = fz_load_page;\n"
    "    fz_page* p = pfn(ctx, doc, 2);\n"
    "    return p;\n"
    "}\n"
)

# ---- fixture 4: a reassignment (not an initializer) aliasing the loader ----
FIXTURE_INDIRECT_REASSIGN = _TYPES + (
    "extern \"C\" fz_page* fz_load_page(fz_context*, fz_document*, int);\n"
    "typedef fz_page* (*LoaderFn)(fz_context*, fz_document*, int);\n"
    "\n"
    "fz_page* caller(fz_context* ctx, fz_document* doc, LoaderFn pfn) {\n"
    "    pfn = fz_load_page;\n"
    "    fz_page* p = pfn(ctx, doc, 3);\n"
    "    return p;\n"
    "}\n"
)

# ---- fixture 5: a declaration with NO call at all --------------------------
# The direct control for the decl-vs-call distinction: a file that mentions
# the symbol only as a prototype must score ZERO call sites.
FIXTURE_DECL_ONLY = _TYPES + (
    "extern \"C\" fz_page* fz_load_page(fz_context*, fz_document*, int);\n"
)

# ---- fixture 6: guaranteed FATAL parse error (missing header) -------------
FIXTURE_UNPARSEABLE = (
    '#include "this_header_does_not_exist_pdfe_test.h"\n'
    "void f() {}\n"
)

PLAIN_ARGS = ["--driver-mode=cl", "-TP", "-std:c++20", "-fsyntax-only"]


def _write(tmpdir, name, text):
    path = os.path.join(tmpdir, name)
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return path


def _scan(tmpdir, path, symbols=pg.RAW_LOADERS):
    entry = {"file": path, "arguments": ["clang-cl"] + PLAIN_ARGS + [path]}
    return pg.scan_tu_for_loaders(entry, tmpdir, symbols)


def _lexical_hits_in_text(text, symbols):
    code = pg._strip_non_code(text)
    hits = []
    for sym in symbols:
        needle = sym + "("
        pos = code.find(needle)
        while pos != -1:
            if pg._is_whole_call(code, pos):
                line = 1 + code.count("\n", 0, pos)
                hits.append((sym, line))
            pos = code.find(needle, pos + 1)
    return hits


def self_test(verbose=True):
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        if verbose:
            print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                                   ("  -- " + detail) if detail and not ok else ""))

    tmpdir = tempfile.mkdtemp(prefix="page_load_guard_scan_")
    try:
        # ---- fixture 1: direct call + its own declaration ------------------
        p1 = _write(tmpdir, "direct.cpp", FIXTURE_DIRECT)
        hits1, err1 = _scan(tmpdir, p1)
        check("direct fixture parses cleanly", not err1, err1)
        direct_only = [h for h in hits1 if h["kind"] == pg.DIRECT]
        check("direct fixture: exactly one CALL is found (not the declaration)",
              len(direct_only) == 1, "got %r" % (hits1,))
        if direct_only:
            check("direct fixture: the one hit is NOT flagged as a macro call",
                  direct_only[0]["macro"] is False)
            check("direct fixture: the hit is on the call line (line 9), "
                  "not the declaration line (line 6)",
                  direct_only[0]["line"] == 9, "got line %d" % direct_only[0]["line"])

        lex1 = _lexical_hits_in_text(FIXTURE_DIRECT, pg.RAW_LOADERS)
        check("lexical port over-counts fixture 1: finds BOTH the declaration "
              "and the call (2 hits) -- this is the exact decl-vs-call "
              "confusion the AST scanner does not have",
              len(lex1) == 2, "got %r" % (lex1,))

        # ---- fixture 2: macro-wrapped call ----------------------------------
        p2 = _write(tmpdir, "macro.cpp", FIXTURE_MACRO)
        hits2, err2 = _scan(tmpdir, p2)
        check("macro fixture parses cleanly", not err2, err2)
        macro_hits = [h for h in hits2 if h["kind"] == pg.DIRECT]
        check("macro fixture: the AST scanner finds exactly one real call",
              len(macro_hits) == 1, "got %r" % (hits2,))
        if macro_hits:
            check("macro fixture: that call is flagged macro=True",
                  macro_hits[0]["macro"] is True)
            check("macro fixture: the call is reported on the macro-invocation "
                  "line (line 13), proving location tracking survives expansion",
                  macro_hits[0]["line"] == 13, "got line %d" % macro_hits[0]["line"])

        lex2 = _lexical_hits_in_text(FIXTURE_MACRO, pg.RAW_LOADERS)
        check("macro fixture: the lexical port's ONLY hit is the declaration "
              "line (line 6) -- it never finds the real call on line 13, which "
              "is the whole point of this fixture",
              lex2 == [("fz_load_page", 6)], "got %r" % (lex2,))

        # ---- fixture 3: call through a function pointer (initializer) ------
        p3 = _write(tmpdir, "indirect.cpp", FIXTURE_INDIRECT)
        hits3, err3 = _scan(tmpdir, p3)
        check("indirect fixture parses cleanly", not err3, err3)
        indirect_hits = [h for h in hits3 if h["kind"] == pg.INDIRECT]
        direct_hits3 = [h for h in hits3 if h["kind"] == pg.DIRECT]
        check("indirect fixture: no DIRECT call to fz_load_page exists",
              len(direct_hits3) == 0, "got %r" % (hits3,))
        check("indirect fixture: exactly one INDIRECT (function-pointer) "
              "call site is found",
              len(indirect_hits) == 1, "got %r" % (hits3,))
        if indirect_hits:
            check("indirect fixture: attributed to fz_load_page",
                  indirect_hits[0]["symbol"] == "fz_load_page")
            check("indirect fixture: reported via the pointer variable 'pfn'",
                  indirect_hits[0].get("via") == "pfn",
                  "got %r" % (indirect_hits[0].get("via"),))
            check("indirect fixture: on the call line (line 11)",
                  indirect_hits[0]["line"] == 11,
                  "got line %d" % indirect_hits[0]["line"])

        lex3 = _lexical_hits_in_text(FIXTURE_INDIRECT, pg.RAW_LOADERS)
        check("indirect fixture: the lexical port's ONLY hit is the "
              "declaration line (line 6) -- 'pfn = fz_load_page;' (no paren) "
              "and 'pfn(ctx, doc, 2)' (no symbol name) are both invisible to "
              "it, which is exactly the class of site it cannot see",
              lex3 == [("fz_load_page", 6)], "got %r" % (lex3,))

        # ---- fixture 4: reassignment (not just an initializer) -------------
        p4 = _write(tmpdir, "indirect_reassign.cpp", FIXTURE_INDIRECT_REASSIGN)
        hits4, err4 = _scan(tmpdir, p4)
        check("reassignment fixture parses cleanly", not err4, err4)
        indirect_hits4 = [h for h in hits4 if h["kind"] == pg.INDIRECT]
        check("reassignment fixture: the AST scanner follows a plain `=` "
              "reassignment (not just an initializer) to the same conclusion",
              len(indirect_hits4) == 1, "got %r" % (hits4,))

        # ---- fixture 5: declaration only, no call --------------------------
        p5 = _write(tmpdir, "decl_only.cpp", FIXTURE_DECL_ONLY)
        hits5, err5 = _scan(tmpdir, p5)
        check("decl-only fixture parses cleanly", not err5, err5)
        check("decl-only fixture: the AST scanner reports ZERO call sites "
              "for a file containing nothing but a prototype",
              len(hits5) == 0, "got %r" % (hits5,))

        # ---- fixture 6: unparseable file is a LOUD failure, never silent ---
        p6 = _write(tmpdir, "unparseable.cpp", FIXTURE_UNPARSEABLE)
        hits6, err6 = _scan(tmpdir, p6)
        check("unparseable fixture: a missing #include is reported as a "
              "fatal parse error, not silently swallowed",
              bool(err6), "got hits=%r err=%r" % (hits6, err6))
        check("unparseable fixture: zero hits are returned alongside the "
              "error (a partial AST is not reported as a clean scan)",
              hits6 == [])

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    passed = sum(1 for _, ok, _ in checks if ok)
    print("self-test: %d/%d checks passed" % (passed, len(checks)))
    return passed == len(checks)


# ---------------------------------------------------------------------------
# pytest entry points
# ---------------------------------------------------------------------------

def test_page_load_guard_scan_self_test():
    assert self_test(verbose=False)


if __name__ == "__main__":
    sys.exit(0 if self_test() else 1)
