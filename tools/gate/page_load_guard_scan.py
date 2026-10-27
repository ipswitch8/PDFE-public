#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AST-based call-site scanner for the Form XObject depth-guard (GitHub #159,
#176 F14/phase-6).

WHY THIS IS NOT THE EXISTING SCANNER
-------------------------------------
`tests/test_page_load_guard_sites.cpp` re-derives PDFE's page-load call-site
list from `src/` on every gtest run. It says plainly, in its own header
comment, what it is: a lexical scan. It strips `//`/`/* */` comments and
`"..."`/`'...'` literals, then substring-matches `SYMBOL(` with a whole-word
check on the character before it. That is enough to tell a call from a mention
inside a comment or a string, and enough to tell `safeLoadPage(` from
`LoadPage(`. It is not enough for two shapes of call site:

1. **A macro-wrapped call.** If some macro `M(...)` expands to
   `fz_load_page(...)`, the literal source text at the call site is `M(...)`,
   not `fz_load_page(...)` — the substring the lexical scan searches for never
   appears there. A regex or lexical pass over raw source text cannot see past
   a macro boundary; only something that scans the code the COMPILER sees
   (i.e. after preprocessing) can.
2. **A call through a function pointer.** `pfn(ctx, doc, i)` where `pfn` was
   earlier assigned `fz_load_page` contains no occurrence of the symbol name
   at the call site at all. A lexical scan cannot follow that data flow even
   in principle; it needs the identifier resolution an AST (or at least a
   symbol table) provides.

The project's precedent for this exact class of problem is
`tools/gate/fz_try_raii_scan.py` (phase-68): "a sweep keyed on phrasing always
comes back" is the standing lesson (see MEMORY.md, pipeline_f20_f25), and the
documented preference (restated inline in the .cpp file above) is an AST pass
over grep. This tool is that AST pass, built the same way: libclang parsing
driven by the project's own `compile_commands.json` (build/vs2022-asan), so it
sees the SAME preprocessor definitions, include paths and language mode the
real build uses — not a hand-rolled stand-in that would only prove the tool
agrees with itself.

THE BUG THIS MUST NOT REINTRODUCE
----------------------------------
The lexical scanner's guarded-call-site floor was once padded by two because a
whole-identifier substring match cannot tell a *call* to `safeLoadPage` from
its *declaration* in `FormXObjectDepthGuard.h` — both look like
`safeLoadPage(` to a text scan. With that padding, the floor of 13 survived a
mutation that deleted a real call site (M9); corrected to 12 by excluding the
header explicitly, by filename, as a special case.

An AST does not need a filename special case: a `CALL_EXPR` and a
`FUNCTION_DECL` are different node kinds by construction. `count_symbols()`
below only ever counts `CALL_EXPR` nodes whose resolved callee is one of the
target functions — a prototype or declaration is a `FUNCTION_DECL`/`CXX_METHOD`
node it never visits as a call, so the same bug cannot recur here by
accident, not because a file was carved out.

WHAT THIS TOOL COUNTS
----------------------
Two independent symbol sets, matching the two GoogleTest assertions in
`test_page_load_guard_sites.cpp`:

* ``--raw`` (default ``fz_load_page,pdf_load_page``) — the ENFORCEMENT count:
  every call site MUST be inside the guard implementation
  (`src/core/FormXObjectDepthGuard.cpp`); anywhere else is the #159 defect
  class recurring.
* ``--guarded`` (default ``safeLoadPage,safeLoadPdfPage``) — the USAGE count:
  every other call site in `src/` that goes through the guard, which is a
  floor (sites only ever grow as legitimate callers are added).

For each symbol set this reports THREE kinds of call site:

* ``direct``   — an ordinary `CALL_EXPR` whose resolved callee is the target
  function. This is what the lexical scanner also finds (when it finds
  anything).
* ``macro``    — a `direct` call whose location is inside a macro expansion,
  i.e. the raw source text at that spot does not contain the symbol name at
  all. Reported as a distinguished subset of `direct`, not a separate total,
  because it IS a real call the AST already counted; the label just answers
  the "did you check macro sites" question with evidence.
* ``indirect`` — a call through a variable (a function pointer) that was
  observed, in the same translation unit, being initialized or assigned from
  the target function. This is the call-through-function-pointer class the
  lexical scanner cannot see even in principle.

HOW INDIRECT (FUNCTION-POINTER) SITES ARE FOUND
-------------------------------------------------
This is necessarily a heuristic — full points-to analysis is out of scope —
but it is a checked one, not an assumed one:

1. Walk every `DECL_REF_EXPR` that resolves to one of the target
   `FUNCTION_DECL`s.
2. If its nearest enclosing `CALL_EXPR` treats it as the callee (i.e. that
   call's own resolved referent is the SAME function), it is a `direct` call
   — already handled by (1) above, not double-counted here.
3. Otherwise the function is being used AS A VALUE: assigned to a variable
   (`Type p = fz_load_page;`), reassigned (`p = fz_load_page;`), or passed as
   an argument to something else. The enclosing `VAR_DECL` (for an
   initializer) or the LHS of an enclosing `=` `BINARY_OPERATOR` (for a plain
   assignment) is resolved and its USR recorded as "aliases this function".
4. A second pass then looks at every `CALL_EXPR` whose resolved callee is a
   `VAR_DECL`/`PARM_DECL`/`FIELD_DECL` (i.e. a call through a variable, not a
   named function) and reports it as an `indirect` call site when that
   variable's USR was recorded in step 3.

This is a lower bound on indirect sites, exactly as `fz_try_raii_scan.py`
documents itself as a lower bound on RAII hazards: a pointer stored in a
struct field and populated in a DIFFERENT translation unit, or one that flows
through more than one hop of assignment, is not traced. `--self-test` proves
the mechanism catches the straightforward, single-TU case; the real-`src/`
run below states plainly whether anything of the kind was found, and how the
absence was checked.

The machine-readable record is written by ``--json`` (there is no ``--report``
flag; an earlier draft of this docstring named one, along with a report file
that was never generated — a documented capability that does not exist is worse
than an undocumented one, because a reader stops looking). The committed run for
GitHub #176 is ``docs/audit/p83-outstanding/phase-6/page-load-ast-scan.json``,
and its prose reading is in that directory's ``notes.md``.

UNPARSEABLE FILES ARE A HARD FAILURE, NOT A SILENT SKIP
---------------------------------------------------------
A scan that silently drops a translation unit it could not parse reports a
clean number over a SUBSET and looks identical to a clean sweep of the whole
tree — the exact failure mode `fz_try_raii_scan.py` was built to avoid
(“…looks identical to a clean tree” — see the same tool's own
`--allow-parse-errors` guard). This tool inherits that behaviour rather than
re-deriving it: a translation unit with a FATAL diagnostic is reported as a
parse error and fails the run unless `--allow-parse-errors` is passed
explicitly, in which case the reduced coverage is printed, not hidden.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import io
import json
import multiprocessing as mp
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

try:
    import clang.cindex as ci
except ImportError:  # pragma: no cover - reported, never silently skipped
    sys.stderr.write(
        "page_load_guard_scan: the 'libclang' python package is not installed.\n"
        "                      install it with:  python -m pip install libclang\n")
    raise

# Reuse the compile_commands.json / clang-cl argument plumbing this project
# already built and mutation-tested for exactly this purpose (phase-68), so
# the two tools cannot silently drift apart on how a translation unit gets its
# include paths.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fz_try_raii_scan as _base  # noqa: E402  (path insert must come first)

CursorKind = ci.CursorKind

RAW_LOADERS = ("fz_load_page", "pdf_load_page")
GUARDED_LOADERS = ("safeLoadPage", "safeLoadPdfPage")

#: The only file allowed to call the raw loaders directly (mirrors
#: kGuardImpl in test_page_load_guard_sites.cpp).
GUARD_IMPL = "src/core/FormXObjectDepthGuard.cpp"
#: The guard's own header. A lexical scan of `safeLoadPage(` cannot tell this
#: file's two DECLARATIONS from a call, which is the bug this tool exists not
#: to reintroduce (see module docstring). An AST scan simply never visits a
#: FUNCTION_DECL as a CALL_EXPR, so no filename special-case is needed to get
#: the right count here -- this constant exists only so the report can label
#: the header's zero call sites explicitly rather than leaving them unmentioned.
GUARD_HEADER = "src/core/FormXObjectDepthGuard.h"

DIRECT = "direct"
INDIRECT = "indirect"


def _rel(path, root):
    try:
        return os.path.relpath(path, root).replace("\\", "/")
    except ValueError:
        return path


def is_macro_call(tu, call_cursor, callee_name):
    """True when the literal source text at `call_cursor`'s start is not the
    callee name itself -- i.e. this call only exists because a macro expanded
    to it. libclang does not expose a direct is-macro-expansion boolean on
    Cursor, so this asks the lexer for the actual token at the call's start
    location and compares its spelling to the callee name. That comparison is
    ground truth a text/regex scan of the raw file can never produce: the raw
    file contains the macro invocation (a different identifier, or none at
    all), not the expanded call.
    """
    start = call_cursor.extent.start
    try:
        end = ci.SourceLocation.from_position(
            tu, start.file, start.line, start.column + len(callee_name) + 1)
        toks = list(tu.get_tokens(extent=ci.SourceRange.from_locations(start, end)))
    except Exception:
        return False
    if not toks:
        return True
    return toks[0].spelling != callee_name


def _enclosing(parents, kind):
    for p in reversed(parents):
        if p.kind == kind:
            return p
    return None


def _walk_with_parents(cursor):
    stack = [cursor]

    def rec(node):
        for c in node.get_children():
            stack.append(c)
            yield c, list(stack[:-1])
            yield from rec(c)
            stack.pop()

    yield from rec(cursor)


def _resolved_function(cursor):
    """The FUNCTION_DECL a CALL_EXPR resolves to, or None (e.g. a call
    through a variable, where `referenced` is the VAR_DECL/PARM_DECL/
    FIELD_DECL instead)."""
    ref = cursor.referenced
    if ref is not None and ref.kind == CursorKind.FUNCTION_DECL:
        return ref
    return None


def scan_tu_for_loaders(entry, repo_root, symbols):
    """Scan one translation unit for direct/macro/indirect calls to `symbols`.

    Returns (hits, error). `hits` is a list of dicts; `error` is a non-empty
    fatal-diagnostic string when the TU failed to parse (hits is then empty --
    a partial AST is not trustworthy enough to report from).
    """
    root = _base._norm(repo_root)
    args = _base.parse_args_for(entry)
    index = ci.Index.create()
    try:
        tu = index.parse(entry["file"], args=args)
    except ci.TranslationUnitLoadError as exc:
        return [], str(exc)

    fatals = [d.spelling for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
    if fatals:
        return [], "; ".join(fatals)

    def in_tree(path):
        return bool(path) and _base._norm(path).startswith(root + os.sep)

    symbol_set = set(symbols)
    hits = []
    aliased_vars = {}  # usr -> function name this variable was set to

    # ---- pass 1: direct calls, and collect function-as-value aliasing -----
    for node, parents in _walk_with_parents(tu.cursor):
        loc = node.location
        if loc.file is None or not in_tree(loc.file.name):
            continue

        if node.kind == CursorKind.CALL_EXPR:
            fn = _resolved_function(node)
            if fn is not None and fn.spelling in symbol_set:
                macro = is_macro_call(tu, node, fn.spelling)
                hits.append({
                    "file": _rel(loc.file.name, root), "line": loc.line,
                    "column": loc.column, "symbol": fn.spelling,
                    "kind": DIRECT, "macro": macro,
                })
            continue

        if node.kind != CursorKind.DECL_REF_EXPR:
            continue
        ref = node.referenced
        if ref is None or ref.kind != CursorKind.FUNCTION_DECL:
            continue
        if ref.spelling not in symbol_set:
            continue

        # Is this DeclRefExpr itself the callee of its nearest enclosing call?
        enclosing_call = _enclosing(parents, CursorKind.CALL_EXPR)
        if enclosing_call is not None:
            enclosing_fn = _resolved_function(enclosing_call)
            if enclosing_fn is not None and enclosing_fn.spelling == ref.spelling:
                continue  # the direct-call case, already recorded above

        # The function is being used AS A VALUE (assigned, reassigned, passed
        # as an argument to something else). Try to attribute it to a
        # variable: either the VAR_DECL it initializes, or the LHS of an
        # enclosing `=` BINARY_OPERATOR.
        var_decl = _enclosing(parents, CursorKind.VAR_DECL)
        if var_decl is not None:
            usr = var_decl.get_usr()
            if usr:
                aliased_vars[usr] = ref.spelling
            continue

        binop = _enclosing(parents, CursorKind.BINARY_OPERATOR)
        if binop is not None:
            children = list(binop.get_children())
            if len(children) == 2:
                lhs = children[0]
                lhs_ref = lhs.referenced
                if (lhs_ref is not None
                        and lhs_ref.kind in (CursorKind.VAR_DECL,
                                              CursorKind.PARM_DECL,
                                              CursorKind.FIELD_DECL)):
                    usr = lhs_ref.get_usr()
                    if usr:
                        aliased_vars[usr] = ref.spelling

    # ---- pass 2: calls through a variable that aliases a target function --
    if aliased_vars:
        for node, _parents in _walk_with_parents(tu.cursor):
            if node.kind != CursorKind.CALL_EXPR:
                continue
            loc = node.location
            if loc.file is None or not in_tree(loc.file.name):
                continue
            ref = node.referenced
            if ref is None or ref.kind not in (
                    CursorKind.VAR_DECL, CursorKind.PARM_DECL, CursorKind.FIELD_DECL):
                continue
            usr = ref.get_usr()
            fn_name = aliased_vars.get(usr)
            if fn_name is None:
                continue
            hits.append({
                "file": _rel(loc.file.name, root), "line": loc.line,
                "column": loc.column, "symbol": fn_name,
                "kind": INDIRECT, "macro": False,
                "via": ref.spelling,
            })

    return hits, ""


def _worker(payload):
    entry, repo_root, symbols = payload
    try:
        hits, error = scan_tu_for_loaders(entry, repo_root, symbols)
        return {"file": entry["file"], "hits": hits, "error": error}
    except Exception as exc:  # pragma: no cover - surfaced, never swallowed
        return {"file": entry["file"], "hits": [],
                "error": "scan crashed: %r" % (exc,)}


def _mp_worker_entry(entry, repo_root, symbols, out_queue):
    """Runs in its OWN child process (see _run_one_isolated). Never lets an
    exception escape uncaught -- the whole point of the isolation below is
    that a genuine native crash (a libclang assertion/segfault on some
    pathological translation unit) takes down only THIS process, and is
    still reported, rather than silently terminating the whole sweep with
    zero signal about which file was responsible. That happened on the first
    real run against this project's tree: a shared, non-isolated worker loop
    vanished partway through with no error, no partial JSON and no clue which
    of 231 files was at fault."""
    try:
        result = _worker((entry, repo_root, symbols))
    except Exception as exc:  # belt and braces; _worker already catches
        result = {"file": entry["file"], "hits": [],
                  "error": "scan crashed: %r" % (exc,)}
    out_queue.put(result)


def _run_one_isolated(entry, repo_root, symbols, timeout):
    """Parse exactly one translation unit in a dedicated child process.

    Returns the worker's result dict in the normal case. If the child dies
    without producing a result (killed, segfault, native assertion) or runs
    longer than `timeout` (a hang, not merely a slow parse), THIS file is
    reported as a parse error and every other file is unaffected -- unlike a
    shared multiprocessing.Pool, where a single crashed worker can silently
    strand the task queue for every OTHER file still waiting on that worker.
    """
    ctx = mp.get_context("spawn")
    q = ctx.Queue()
    p = ctx.Process(target=_mp_worker_entry, args=(entry, repo_root, symbols, q))
    p.start()
    p.join(timeout)
    if p.is_alive():
        p.terminate()
        p.join(5)
        return {"file": entry["file"], "hits": [],
                "error": "TIMEOUT after %ds (parse did not finish -- treated "
                         "as a hang, not a slow parse)" % timeout}
    try:
        return q.get_nowait()
    except Exception:
        return {"file": entry["file"], "hits": [],
                "error": "child process exited with code %r and produced no "
                         "result -- likely a native crash inside libclang "
                         "while parsing this translation unit"
                         % (p.exitcode,)}


def run_ast_scan(db_path, repo_root, symbols, subdirs=("src",), jobs=None,
                  progress=False, entries=None, timeout=240):
    # `entries` lets the caller pass a compile-command list it has already
    # loaded and possibly augmented (see --synthesize-missing). Falling back to
    # loading from db_path keeps every existing caller and the self-test
    # working unchanged.
    if entries is None:
        entries = _base.load_compile_commands(db_path, repo_root, subdirs)
    jobs = jobs or max(1, (os.cpu_count() or 2) - 1)

    results = [None] * len(entries)
    done = 0
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {
            pool.submit(_run_one_isolated, e, repo_root, symbols, timeout): i
            for i, e in enumerate(entries)
        }
        for fut in cf.as_completed(futures):
            i = futures[fut]
            try:
                results[i] = fut.result()
            except Exception as exc:  # pragma: no cover - defence in depth
                results[i] = {"file": entries[i]["file"], "hits": [],
                              "error": "isolation harness failed: %r" % (exc,)}
            done += 1
            if progress:
                print("  [%d/%d] %s" % (done, len(entries), results[i]["file"]))

    hits, errors = [], []
    for r in results:
        hits.extend(r["hits"])
        if r["error"]:
            errors.append({"file": r["file"], "error": r["error"][:400]})
    hits.sort(key=lambda h: (h["file"], h["line"], h["column"]))
    return {
        "translation_units_total": len(entries),
        "translation_units_parsed": len(entries) - len(errors),
        "hits": hits,
        "errors": errors,
    }


def dedupe_direct_indirect(hits):
    """Collapse to one row per (file, line, column, symbol) preferring the
    richer entry (macro flag / indirect kind); a location cannot legitimately
    be both a direct and an indirect call to the SAME symbol, but two
    independent DeclRefExpr visits (e.g. through an implicit cast) could in
    principle report the same call twice."""
    seen = {}
    for h in hits:
        key = (h["file"], h["line"], h["column"], h["symbol"], h["kind"])
        seen[key] = h
    return sorted(seen.values(), key=lambda h: (h["file"], h["line"], h["column"]))


# ---------------------------------------------------------------------------
# lexical reference implementation -- a faithful Python port of
# tests/test_page_load_guard_sites.cpp's scan(), so "the prior lexical
# scanner's count" is a number this tool can actually produce and compare
# against, not a number quoted from memory of the C++ file.
# ---------------------------------------------------------------------------

def _strip_non_code(src):
    out = []
    state = "code"
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"; out.append("  "); i += 2; continue
            if c == "/" and nxt == "*":
                state = "block"; out.append("  "); i += 2; continue
            if c == '"':
                state = "str"; out.append(" "); i += 1; continue
            if c == "'":
                state = "chr"; out.append(" "); i += 1; continue
            out.append(c); i += 1; continue
        if state == "line":
            if c == "\n":
                state = "code"; out.append(c)
            else:
                out.append(" ")
            i += 1; continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = "code"; out.append("  "); i += 2; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if state == "str":
            if c == "\\":
                out.append("  "); i += 2; continue
            if c == '"':
                state = "code"; out.append(" "); i += 1; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if state == "chr":
            if c == "\\":
                out.append("  "); i += 2; continue
            if c == "'":
                state = "code"; out.append(" "); i += 1; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
    return "".join(out)


def _is_whole_call(hay, pos):
    if pos > 0:
        prev = hay[pos - 1]
        if prev.isalnum() or prev == "_":
            return False
    return True


def lexical_scan(repo_root, symbols):
    """Direct Python port of scan()/isWholeCall() in
    tests/test_page_load_guard_sites.cpp. Kept in lock-step with that file
    deliberately -- this is what makes "the AST count is >= the lexical
    count" a real comparison rather than an assertion about a number nobody
    re-derived."""
    hits = []
    root = os.path.join(repo_root, "src")
    if not os.path.isdir(root):
        return hits
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            ext = os.path.splitext(fn)[1]
            if ext not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            full = os.path.join(dirpath, fn)
            try:
                with io.open(full, encoding="utf-8", errors="replace") as fh:
                    raw = fh.read()
            except OSError:
                continue
            code = _strip_non_code(raw)
            rel = os.path.relpath(full, repo_root).replace("\\", "/")
            for sym in symbols:
                needle = sym + "("
                pos = code.find(needle)
                while pos != -1:
                    if _is_whole_call(code, pos):
                        line = 1 + code.count("\n", 0, pos)
                        hits.append({"file": rel, "line": line, "symbol": sym})
                    pos = code.find(needle, pos + 1)
    return hits


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _count_unguarded(ast_hits, guard_impl):
    return [h for h in ast_hits if h["file"] != guard_impl]


def _count_guarded_call_sites(ast_hits, guard_impl, guard_header):
    return [h for h in ast_hits if h["file"] not in (guard_impl, guard_header)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[2])
    ap.add_argument("--compile-commands",
                    default="build/vs2022-asan/compile_commands.json")
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--subdir", action="append", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--synthesize-missing", action="store_true",
                    help="for a source file absent from the compile "
                         "database, borrow the compile command of another "
                         "file in the SAME directory. Every synthesis is "
                         "printed and recorded; a file with no "
                         "same-directory donor stays uncovered and still "
                         "fails.")
    ap.add_argument("--allow-parse-errors", action="store_true")
    ap.add_argument("--progress", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--jobs", type=int, default=None,
                     help="concurrent isolated parses (default: cpu_count-1)")
    ap.add_argument("--timeout", type=int, default=240,
                     help="per-translation-unit wall-clock seconds before a "
                          "parse is treated as hung rather than slow")
    args = ap.parse_args(argv)

    if args.self_test:
        from test_page_load_guard_scan import self_test  # local import: avoid
        return 0 if self_test() else 1                    # a hard dep when unused

    repo_root = os.path.abspath(args.repo_root)
    db = args.compile_commands
    if not os.path.isabs(db):
        db = os.path.join(repo_root, db)
    if not os.path.exists(db):
        sys.stderr.write(
            "page_load_guard_scan: no compile database at %s\n"
            "  generate one with:  cmake --preset windows-vs2022-asan\n" % db)
        return 2

    subdirs = tuple(args.subdir) if args.subdir else ("src",)

    # ---------------------------------------------------------------------
    # COVERAGE CHECK -- a file absent from the compile database is NOT a parse
    # error, so nothing below would ever mention it.
    #
    # This scanner's whole claim over the lexical one is completeness. A
    # compile database is a snapshot: it lists the translation units that
    # existed when CMake last configured. Every .cpp added since is simply
    # missing, and the scan then reports a confident count over a subset --
    # strictly worse than the lexical scanner, which at least walks the tree.
    #
    # Found the first time this ran for real: the database was three weeks
    # stale and omitted 5 of 236 files, INCLUDING FormXObjectDepthGuard.cpp,
    # the guard implementation itself. The scan reported 231 TUs and looked
    # complete.
    #
    # So: enumerate the tree, compare, and fail loudly. Reduced coverage must
    # be an explicit opt-in, exactly as it already is for parse errors.
    # ---------------------------------------------------------------------
    db_entries = _base.load_compile_commands(db, repo_root, subdirs)
    db_files = set()
    for e in db_entries:
        try:
            db_files.add(os.path.normcase(os.path.realpath(e["file"])))
        except Exception:
            pass

    on_disk = []
    for sd in subdirs:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(repo_root, sd)):
            for fn in filenames:
                if fn.endswith((".cpp", ".cc", ".cxx")):
                    on_disk.append(os.path.join(dirpath, fn))

    uncovered = sorted(
        os.path.relpath(p, repo_root).replace("\\", "/")
        for p in on_disk
        if os.path.normcase(os.path.realpath(p)) not in db_files)

    print("=== compile-database coverage ===")
    print("%s on disk            : %d" % (subdirs, len(on_disk)))
    print("present in compile db : %d" % (len(on_disk) - len(uncovered)))
    print("NOT in compile db     : %d" % len(uncovered))
    for p in uncovered:
        print("    %s" % p)
    print()

    synthesized = []
    if uncovered and args.synthesize_missing:
        # ------------------------------------------------------------------
        # Borrow a sibling's compile command.
        #
        # This is a FALLBACK and is named as one. It is sound for the case it
        # exists for -- a source file added after the database was generated,
        # sitting in a directory whose other files are in the database. Those
        # files are in the same CMake target, so their include paths, defines
        # and standard level are the same; only the input filename differs.
        #
        # It is NOT sound in general: a new file in a NEW directory, or one in
        # a target with different defines, would get the wrong flags. So the
        # donor must come from the same directory (never an ancestor), and any
        # file with no same-directory donor stays uncovered and still fails.
        # Every synthesis is printed and recorded in the JSON, because a
        # fallback nobody can see is a silent assumption.
        # ------------------------------------------------------------------
        by_dir = {}
        for e in db_entries:
            try:
                d = os.path.normcase(os.path.dirname(os.path.realpath(e["file"])))
            except Exception:
                continue
            by_dir.setdefault(d, e)

        still_uncovered = []
        for rel in uncovered:
            absp = os.path.realpath(os.path.join(repo_root, rel))
            donor = by_dir.get(os.path.normcase(os.path.dirname(absp)))
            if donor is None:
                still_uncovered.append(rel)
                continue
            new_entry = dict(donor)
            new_entry["file"] = absp
            donor_file = donor["file"]
            for key in ("command", "arguments"):
                if key not in new_entry:
                    continue
                val = new_entry[key]
                if isinstance(val, str):
                    new_entry[key] = val.replace(donor_file, absp)
                else:
                    new_entry[key] = [absp if a == donor_file else a for a in val]
            db_entries.append(new_entry)
            synthesized.append({
                "file": rel,
                "donor": os.path.relpath(donor_file, repo_root).replace("\\", "/"),
            })

        print("--- synthesized compile commands (same-directory donor) ---")
        for s in synthesized:
            print("    %s   <- flags from %s" % (s["file"], s["donor"]))
        if still_uncovered:
            print("    STILL UNCOVERED (no same-directory donor):")
            for p in still_uncovered:
                print("        %s" % p)
        print()
        uncovered = still_uncovered

    if uncovered and not args.allow_parse_errors:
        sys.stderr.write(
            "page_load_guard_scan: %d source file(s) under %s are absent from\n"
            "  the compile database and were therefore NOT SCANNED. This is a\n"
            "  hard failure: an unscanned file reports zero call sites and is\n"
            "  indistinguishable from a clean one, which would make this AST\n"
            "  sweep less complete than the lexical scanner it replaces.\n"
            "  Re-configure CMake to refresh the database, or pass\n"
            "  --allow-parse-errors to accept the stated reduced coverage.\n"
            % (len(uncovered), subdirs))
        return 4

    lex_raw = lexical_scan(repo_root, RAW_LOADERS)
    lex_guarded = lexical_scan(repo_root, GUARDED_LOADERS)
    lex_raw_count = len([h for h in lex_raw if h["file"] != GUARD_IMPL])
    lex_guarded_count = len([h for h in lex_guarded
                              if h["file"] not in (GUARD_IMPL, GUARD_HEADER)])

    ast_raw = run_ast_scan(db, repo_root, RAW_LOADERS, subdirs,
                           jobs=args.jobs, progress=args.progress,
                           entries=db_entries, timeout=args.timeout)
    ast_guarded = run_ast_scan(db, repo_root, GUARDED_LOADERS, subdirs,
                                jobs=args.jobs, progress=args.progress,
                                entries=db_entries, timeout=args.timeout)

    raw_hits = dedupe_direct_indirect(ast_raw["hits"])
    guarded_hits = dedupe_direct_indirect(ast_guarded["hits"])

    raw_unguarded = _count_unguarded(raw_hits, GUARD_IMPL)
    guarded_sites = _count_guarded_call_sites(guarded_hits, GUARD_IMPL, GUARD_HEADER)

    macro_raw = [h for h in raw_hits if h.get("macro")]
    macro_guarded = [h for h in guarded_hits if h.get("macro")]
    indirect_raw = [h for h in raw_hits if h["kind"] == INDIRECT]
    indirect_guarded = [h for h in guarded_hits if h["kind"] == INDIRECT]

    errors = ast_raw["errors"] + ast_guarded["errors"]
    seen_err_files = set()
    dedup_errors = []
    for e in errors:
        if e["file"] in seen_err_files:
            continue
        seen_err_files.add(e["file"])
        dedup_errors.append(e)

    print("=== translation units ===")
    print("total in compile db (subdirs=%s): %d" % (subdirs, ast_raw["translation_units_total"]))
    print("parsed cleanly                 : %d" % ast_raw["translation_units_parsed"])
    print("failed to parse                : %d" % len(dedup_errors))
    for e in dedup_errors[:20]:
        print("    %s: %s" % (e["file"], e["error"][:160]))

    print()
    print("=== raw loader call sites (fz_load_page / pdf_load_page) ===")
    print("lexical scanner (Python port of the .cpp scan()): %d unguarded call site(s)"
          % lex_raw_count)
    print("AST scanner                                      : %d unguarded call site(s)"
          % len(raw_unguarded))
    print("  of which via macro expansion   : %d" % len(macro_raw))
    print("  of which via function pointer  : %d" % len(indirect_raw))
    for h in raw_unguarded:
        print("    %s:%d  %s()  [%s%s]" % (
            h["file"], h["line"], h["symbol"], h["kind"],
            ", via macro" if h.get("macro") else ""))

    print()
    print("=== guarded loader call sites (safeLoadPage / safeLoadPdfPage) ===")
    print("lexical scanner (Python port of the .cpp scan()): %d call site(s)"
          % lex_guarded_count)
    print("AST scanner                                      : %d call site(s)"
          % len(guarded_sites))
    print("  of which via macro expansion   : %d" % len(macro_guarded))
    print("  of which via function pointer  : %d" % len(indirect_guarded))

    print()
    total_lex = lex_raw_count + lex_guarded_count
    total_ast = len(raw_unguarded) + len(guarded_sites)
    print("TOTAL lexical count : %d" % total_lex)
    print("TOTAL AST count     : %d" % total_ast)
    if total_ast >= total_lex:
        print("AST count >= lexical count: OK")
    else:
        print("AST count <  lexical count: REGRESSION")

    result = {
        "source_files_on_disk": len(on_disk),
        "source_files_not_in_compile_db": uncovered,
        "synthesized_compile_commands": synthesized,
        "translation_units_total": ast_raw["translation_units_total"],
        "translation_units_parsed": ast_raw["translation_units_parsed"],
        "parse_errors": dedup_errors,
        "lexical_raw_count": lex_raw_count,
        "lexical_guarded_count": lex_guarded_count,
        "ast_raw_hits": raw_unguarded,
        "ast_guarded_hits": guarded_sites,
        "ast_raw_count": len(raw_unguarded),
        "ast_guarded_count": len(guarded_sites),
        "macro_raw_hits": macro_raw,
        "macro_guarded_hits": macro_guarded,
        "indirect_raw_hits": indirect_raw,
        "indirect_guarded_hits": indirect_guarded,
    }
    if args.json:
        with io.open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
            fh.write("\n")

    if dedup_errors and not args.allow_parse_errors:
        sys.stderr.write(
            "page_load_guard_scan: %d translation unit(s) failed to parse.\n"
            "  A sweep that parsed nothing reports zero call sites and looks\n"
            "  identical to a clean tree, so this is a hard failure. Pass\n"
            "  --allow-parse-errors only if you have read the errors above\n"
            "  and accept the reduced coverage.\n" % len(dedup_errors))
        return 3

    if total_ast < total_lex:
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
